/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/param.h>
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_st7123.h"

#define FW_VERSION_REG          (0x0000)
#define FW_REVISION_REG         (0x000C)
#define MAX_X_COORD_H_REG       (0x0005)
#define MAX_X_COORD_L_REG       (0x0006)
#define MAX_Y_COORD_H_REG       (0x0007)
#define MAX_Y_COORD_L_REG       (0x0008)
#define MAX_TOUCHES_REG         (0x0009)
#define REPORT_COORD_0_REG      (0x0014)

static const char *TAG = "ST7123";

static uint8_t s_fail_count = 0;   /* 连续读取失败计数 */
static TickType_t s_retry_after_tick = 0;
static esp_lcd_touch_st7123_recovery_cb_t s_recovery_cb = NULL;
static void *s_recovery_user_data = NULL;
static void st7123_clear_fail_count(void) { s_fail_count = 0; }

void esp_lcd_touch_st7123_set_recovery_cb(esp_lcd_touch_st7123_recovery_cb_t callback,
                                          void *user_data)
{
    s_recovery_cb = callback;
    s_recovery_user_data = user_data;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);
static esp_err_t reset(esp_lcd_touch_handle_t tp);

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);
static esp_err_t read_fw_info(esp_lcd_touch_handle_t tp);

esp_err_t esp_lcd_touch_new_i2c_st7123(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "Invalid io");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    /* Prepare main structure */
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t st7123 = (esp_lcd_touch_handle_t)calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(st7123, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    /* Communication interface */
    st7123->io = io;
    /* Only supported callbacks are set */
    st7123->read_data = read_data;
    st7123->get_xy = get_xy;
    st7123->del = del;
    /* Mutex */
    st7123->data.lock.owner = portMUX_FREE_VAL;
    /* Save config */
    memcpy(&st7123->config, config, sizeof(esp_lcd_touch_config_t));

    /* Prepare pin for touch interrupt */
    if (config->int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (config->levels.interrupt) ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
            .pin_bit_mask = BIT64(config->int_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");

        /* Register interrupt callback */
        if (config->interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(st7123, config->interrupt_callback);
        }
    }
    /* Prepare pin for touch controller reset */
    if (config->rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(config->rst_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }
    /* Reset controller */
    ESP_GOTO_ON_ERROR(reset(st7123), err, TAG, "Reset failed");
    ESP_GOTO_ON_ERROR(read_fw_info(st7123), err, TAG, "Read version failed");

    ESP_LOGI(TAG, "Touch panel create success, version: %d.%d.%d", ESP_LCD_TOUCH_ST7123_VER_MAJOR,
             ESP_LCD_TOUCH_ST7123_VER_MINOR, ESP_LCD_TOUCH_ST7123_VER_PATCH);

    *tp = st7123;

    return ESP_OK;
err:
    if (st7123) {
        del(st7123);
    }
    ESP_LOGE(TAG, "Initialization failed!");
    return ret;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - s_retry_after_tick) < 0) {
        return ESP_OK;
    }

    uint8_t adv = 0;
    esp_err_t ret = i2c_read_bytes(tp, 0x0010, &adv, 1);
    if (ret != ESP_OK) goto read_failed;
    st7123_clear_fail_count();
    if (!(adv & 0x08)) {
        return ESP_OK;   /* With Coord = 0，无新坐标 */
    }

    uint8_t max_touches = 0;
    ret = i2c_read_bytes(tp, MAX_TOUCHES_REG, &max_touches, 1);
    if (ret != ESP_OK) goto read_failed;
    if (max_touches > 10) {
        max_touches = 10;
    }

    /*
     * ST7123 协议（TDDI Interface Protocol V01.11，6.x 节，与 Arduino 参考驱动一致）：
     *   - 坐标报告区从 0x0014 起，每点 6 字节（X_H/X_L/Y_H/Y_L/Area/Intensity）
     *   - 主机必须读到"最后一个支持坐标的最后一个寄存器地址"（含 checksum），
     *     芯片才会清除 With Coord 状态并释放 INT。
     *   - 固定读 70 字节（0x0014..0x0059）覆盖 10 点坐标 + checksum，保证触发清除。
     *   （官方组件按 max_touches 动态读取，读不完 checksum 会导致后续触摸失效）
    */
    uint8_t report[70];
    ret = i2c_read_bytes(tp, REPORT_COORD_0_REG, report, sizeof(report));
    if (ret != ESP_OK) goto read_failed;

    portENTER_CRITICAL(&tp->data.lock);
    /* Fill all coordinates */
    int j = 0;
    for (int i = 0; (i < max_touches) && (j < CONFIG_ESP_LCD_TOUCH_MAX_POINTS); i++) {
        const uint8_t *p = &report[i * 6];
        if (!(p[0] & 0x80)) {
            continue;   /* Valid 位（bit7） */
        }
        tp->data.coords[j].x = ((uint16_t)(p[0] & 0x3F) << 8) | p[1];   /* X_H 6bit + X_L */
        tp->data.coords[j].y = ((uint16_t)(p[2] & 0x3F) << 8) | p[3];   /* Y_H 6bit + Y_L */
        tp->data.coords[j++].strength = p[4];
    }
    /* Expect Number of touched points */
    tp->data.points = j;
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;

read_failed:
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    /* 连续失败自动复位芯片：ST7123 偶发异常会把 I2C 总线拉死，
     * 拉 RST 复位后芯片释放 SDA，总线恢复。 */
    if (++s_fail_count >= 3) {
        s_fail_count = 0;
        ESP_LOGW(TAG, "touch I2C failed consecutively, recovering controller and bus");
        reset(tp);
        if (s_recovery_cb != NULL) {
            esp_err_t recovery_ret = s_recovery_cb(s_recovery_user_data);
            if (recovery_ret != ESP_OK) {
                ESP_LOGW(TAG, "touch I2C bus reset failed: %s",
                         esp_err_to_name(recovery_ret));
            }
        }
        /* 总线异常后暂时停止访问，避免 LVGL 轮询形成连续复位风暴。 */
        s_retry_after_tick = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    }

    static TickType_t last_warning_tick = 0;
    now = xTaskGetTickCount();
    if ((now - last_warning_tick) >= pdMS_TO_TICKS(1000)) {
        last_warning_tick = now;
        ESP_LOGW(TAG, "touch read failed (%s), ignoring this sample",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    /* Count of points */
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;

        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    /* Clear available touch points count */
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    /* Reset GPIO pin settings */
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    /* Release memory */
    free(tp);

    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(30));    /* 低电平 30ms（ST7123 需要足够复位时间） */
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level failed");
        vTaskDelay(pdMS_TO_TICKS(200));   /* 高电平 200ms：等芯片完成启动，否则读版本会失败 */
    }

    return ESP_OK;
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE((len == 0) || (data != NULL), ESP_ERR_INVALID_ARG, TAG, "Invalid data");
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t read_fw_info(esp_lcd_touch_handle_t tp)
{
    uint8_t version = 0;
    uint8_t revision[4] = { 0 };
    struct {
        uint8_t max_x_h;
        uint8_t max_x_l;
        uint8_t max_y_h;
        uint8_t max_y_l;
        uint8_t max_touches;
    } info;

    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, FW_VERSION_REG, &version, 1), TAG, "Read version failed");
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, FW_REVISION_REG, (uint8_t *)&revision[0], 4), TAG, "Read revision failed");
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, MAX_X_COORD_H_REG, (uint8_t *)&info, sizeof(info)), TAG, "Read info failed");

    ESP_LOGI(TAG, "Firmware version: %d(%d.%d.%d.%d), Max.X: %d, Max.Y: %d, Max.Touchs: %d",
             version, revision[0], revision[1], revision[2], revision[3], ((uint16_t)info.max_x_h << 8) | info.max_x_l,
             ((uint16_t)info.max_y_h << 8) | info.max_y_l, info.max_touches);

    return ESP_OK;
}

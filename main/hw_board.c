/*
 * hw_board.c - 屏幕/触摸/背光/LVGL 初始化
 *
 * 屏幕初始化参数与资料工程 esp32p4-idf5_st7102_lvgl9 完全一致：
 *   - MIPI DSI 2-lane, 820Mbps, LDO_VO3 供 DSI PHY 2.5V
 *   - ST7102 480x800 RGB565, DPI 37.8MHz
 *   - ST7123 触摸 I2C 总线（内置上拉）
 *   - LVGL 9 + esp_lvgl_port, 双缓冲在 PSRAM, 旋转 270°
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_touch_st7123.h"

#include "esp_lcd_st7102.h"
#include "st7102_init_cmds.h"

#include "hw_board.h"
#include "app_config.h"

static const char *TAG = "hw_board";

extern const uint8_t boot_text_rgb565_start[]
    asm("_binary_boot_text_rgb565_start");

#define BOOT_TEXT_W 400
#define BOOT_TEXT_H 128

#if LV_COLOR_DEPTH == 16
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB565)
#define BSP_LCD_COLOR_DEPTH (16)
#define LV_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#else
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB888)
#define BSP_LCD_COLOR_DEPTH (24)
#define LV_COLOR_FORMAT LV_COLOR_FORMAT_RGB888
#endif

static void board_enable_dsi_phy_power(void)
{
    /* VDD_MIPI_DPHY 需 2.5V，用内部 LDO_VO3 供电 */
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY powered (LDO_VO3=2.5V)");
}

static esp_err_t board_backlight_init(void)
{
    /* io12 背光：LEDC PWM */
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CONFIG_GENSHIN_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer config failed");

    ledc_channel_config_t ch_cfg = {
        .gpio_num = CONFIG_GENSHIN_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "backlight channel config failed");
    return ESP_OK;
}

void hw_board_set_backlight(uint8_t percent)
{
    if (percent < 20) percent = 20;
    if (percent > 100) percent = 100;
    uint32_t duty = ((uint32_t)percent * 1024U) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "backlight set to %u%%", (unsigned)percent);
}

static esp_err_t board_lcd_init(hw_board_t *board)
{
    esp_err_t ret = ESP_OK;

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 820,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), err, TAG, "DSI bus failed");

    /* DBI 通道发初始化命令 */
    esp_lcd_dbi_io_config_t dbi_config = ST7102_MIPI_PANEL_IO_DBI_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &board->io),
                      err, TAG, "DBI IO failed");

    esp_lcd_dpi_panel_config_t dpi_config = ST7102_MIPI_480_800_PANEL_60HZ_DPI_CONFIG(MIPI_DPI_PX_FORMAT);

    st7102_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,   /* GPIO24 复位会卡死 LCD init，暂不控制 RST */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_COLOR_DEPTH,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7102(board->io, &panel_config, &board->lcd),
                      err, TAG, "ST7102 panel failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(board->lcd), err, TAG, "panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(board->lcd), err, TAG, "panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(board->lcd, true), err, TAG, "panel disp_on failed");

    /* 点亮后立即填黑屏，避免开机白屏窗口（LVGL 首帧还没画出来） */
    uint16_t *black = heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (black != NULL) {
        memset(black, 0, (size_t)LCD_H_RES * LCD_V_RES * 2);
        esp_lcd_panel_draw_bitmap(board->lcd, 0, 0, LCD_H_RES, LCD_V_RES, black);
        heap_caps_free(black);
    }

    ESP_LOGI(TAG, "ST7102 480x800 RGB565 ready");
    return ESP_OK;

err:
    return ret;
}


/* 触摸坐标处理（诊断已移除） */
static void touch_noop_process_coords(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                      uint16_t *strength, uint8_t *point_num,
                                      uint8_t max_point_num)
{
    (void)tp; (void)x; (void)y; (void)strength; (void)point_num; (void)max_point_num;
}

static esp_err_t touch_i2c_recovery_cb(void *user_data)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)user_data;
    return bus != NULL ? i2c_master_bus_reset(bus) : ESP_ERR_INVALID_ARG;
}

static esp_err_t board_touch_init(hw_board_t *board)
{
    /* 触摸 I2C 总线（SCL=25 SDA=26，外置上拉） */
    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_PORT,
        .scl_io_num = TOUCH_SCL,
        .sda_io_num = TOUCH_SDA,
        /* 外置上拉之外保留内部弱上拉，提高线缆和供电扰动下的空闲电平稳定性。 */
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_config, &i2c_bus), TAG, "touch I2C bus failed");
    esp_lcd_touch_st7123_set_recovery_cb(touch_i2c_recovery_cb, i2c_bus);

    /* 手动复位触摸芯片（参考 Arduino 版时序：低 30ms → 高 200ms）。
     * 官方组件的 reset() 没有延时，复位后立即读版本会失败。 */
    gpio_config_t rst_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << TOUCH_RST,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    tp_io_config.scl_speed_hz = TOUCH_I2C_CLK_HZ;
    /* 触摸 I2C 显式超时：trans_timeout_ms=0 会被转为无限等待(-1)，
     * 触摸芯片偶发 NACK/拉死总线时 LVGL 任务将无限忙等卡死。 */
    tp_io_config.transaction_timeout_ms = 30;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io),
                        TAG, "touch panel IO failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST,     /* 供组件在总线卡死时自动复位芯片 */
        .int_gpio_num = GPIO_NUM_NC,   /* 不用 INT：纯轮询读取，排除中断对芯片的干扰 */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .process_coordinates = touch_noop_process_coords,
    };

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, &board->touch);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "ST7123 init attempt %d failed (%s)", attempt + 1,
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ST7123 touch init failed, UI will run without touch");
        return ESP_OK;   /* 触摸失败不阻塞整机 */
    }
    ESP_LOGI(TAG, "ST7123 touch ready");
    return ESP_OK;
}

static esp_err_t board_lvgl_init(hw_board_t *board)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 4096 * 2,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = board->io,
        .panel_handle = board->lcd,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,      /* 软件旋转（配合 PPA） */
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };
    board->disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (board->disp == NULL) {
        return ESP_FAIL;
    }
    /* 竖屏 480x800 直出（与 v1.3.2 一致，LVGL_UI 为竖屏布局）。
     * 若屏体安装方向不同，可在此改 LV_DISP_ROTATION_90/180/270，
     * 触摸坐标由 esp_lvgl_port 自动适配旋转。 */
    lv_disp_set_rotation(board->disp, LV_DISP_ROTATION_0);

    if (board->touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = board->disp,
            .handle = board->touch,
        };
        board->touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (board->touch_indev != NULL) {
            /* esp_lvgl_port 在有 INT 脚时会用 EVENT(中断) 模式，触摸易不灵敏。
             * 强制轮询模式：每帧(15ms)直接读一次，不依赖 INT 中断，更稳。 */
            lv_indev_set_mode(board->touch_indev, LV_INDEV_MODE_TIMER);
            lv_timer_t *touch_timer = lv_indev_get_read_timer(board->touch_indev);
            if (touch_timer != NULL) {
                lv_timer_set_period(touch_timer, 30);
            }
            /* LVGL 默认滚动触发距离仅 10px，手指按下/释放轻微抖动就会
             * 被判定为"移动"导致点击失效（点很多次才成功）。调大到 35px：
             * 点击更稳，滑动仍灵敏。 */
            lv_indev_set_scroll_limit(board->touch_indev, 35);
            lv_indev_set_long_press_time(board->touch_indev, 1000);
        }
    }
    return ESP_OK;
}

static esp_err_t board_show_boot_screen(hw_board_t *board)
{
    if (!lvgl_port_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }

    board->boot_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(board->boot_screen, lv_color_hex(0x101827), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(board->boot_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(board->boot_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(board->boot_screen, LV_OBJ_FLAG_SCROLLABLE);

    static const lv_image_dsc_t boot_text = {
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.w = BOOT_TEXT_W,
        .header.h = BOOT_TEXT_H,
        .data_size = BOOT_TEXT_W * BOOT_TEXT_H * 2,
        .data = boot_text_rgb565_start,
    };
    lv_obj_t *text_image = lv_image_create(board->boot_screen);
    lv_image_set_src(text_image, &boot_text);
    lv_obj_center(text_image);

    lv_screen_load(board->boot_screen);
    lv_refr_now(board->disp);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t hw_board_init(hw_board_t *board)
{
    memset(board, 0, sizeof(*board));

    board_enable_dsi_phy_power();
    ESP_RETURN_ON_ERROR(board_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(board_lcd_init(board), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(board_touch_init(board), TAG, "touch init failed");
    ESP_RETURN_ON_ERROR(board_lvgl_init(board), TAG, "LVGL init failed");
    ESP_RETURN_ON_ERROR(board_show_boot_screen(board), TAG, "boot screen failed");

    hw_board_set_backlight(CONFIG_GENSHIN_BACKLIGHT_DEFAULT_PERCENT);
    return ESP_OK;
}

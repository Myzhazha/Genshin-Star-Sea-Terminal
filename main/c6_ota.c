/*
 * c6_ota.c - P4 通过 SDIO 直接烧录 C6 ROM download 模式
 *
 * C6 为空白芯片时不能使用 esp_hosted_slave_ota_*()：那些 API 要求
 * C6 已经运行 ESP-Hosted 固件。这里使用 esp-serial-flasher 的 SDIO
 * 端口，直接连接 C6 ROM 下载器并写入三段镜像：
 *   bootloader.bin       -> 0x0000
 *   partition-table.bin  -> 0x8000
 *   network_adapter.bin  -> 0x10000
 *
 * 下载模式时序由 esp32_sdio_port 完成：P4 GPIO0（C6 IO9）拉低，
 * P4 GPIO54（C6 EN）复位 C6；连接成功后烧录，完成后 GPIO0 拉高并复位。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"

#include "esp32_sdio_port.h"
#include "esp_loader.h"
#include "esp_loader_io.h"

#include "app_config.h"
#include "c6_ota.h"
#include "hosted_wifi.h"

#define C6_OTA_TAG  "c6_ota"
#define C6_FW_DIR   "/sdcard/c6fw"
#define C6_FW_BOOT  C6_FW_DIR "/bootloader.bin"
#define C6_FW_PART  C6_FW_DIR "/partition-table.bin"
#define C6_FW_APP   C6_FW_DIR "/network_adapter.bin"

#define C6_BOOT_ADDR 0x0000U
#define C6_PART_ADDR 0x8000U
#define C6_APP_ADDR  0x10000U
#define C6_FLASH_BLOCK_SIZE 4096U
#define C6_APP_IMAGE_MAGIC 0xE9U
#define C6_FW_MAX_SIZE (4U * 1024U * 1024U)

static volatile bool s_c6_ota_running = false;
static portMUX_TYPE s_c6_ota_mux = portMUX_INITIALIZER_UNLOCKED;
static c6_ota_event_cb_t s_c6_ota_event_cb = NULL;
static void *s_c6_ota_event_user_data = NULL;

void c6_ota_set_event_cb(c6_ota_event_cb_t callback, void *user_data)
{
    s_c6_ota_event_cb = callback;
    s_c6_ota_event_user_data = user_data;
}

bool c6_ota_is_running(void)
{
    bool running;
    portENTER_CRITICAL(&s_c6_ota_mux);
    running = s_c6_ota_running;
    portEXIT_CRITICAL(&s_c6_ota_mux);
    return running;
}

static bool get_file_size(const char *path, long *size_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    fclose(f);
    if (size <= 0 || (unsigned long)size > C6_FW_MAX_SIZE) return false;
    *size_out = size;
    return true;
}

static bool validate_app_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    uint8_t magic = 0;
    bool ok = fread(&magic, 1, 1, f) == 1 && magic == C6_APP_IMAGE_MAGIC;
    fclose(f);
    return ok;
}

static esp_loader_error_t flash_file(esp_loader_t *loader,
                                     const char *path,
                                     uint32_t address)
{
    long size = 0;
    if (!get_file_size(path, &size)) {
        ESP_LOGE(C6_OTA_TAG, "invalid or missing firmware file: %s", path);
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(C6_OTA_TAG, "open %s failed", path);
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }

    ESP_LOGI(C6_OTA_TAG, "flashing %s (%ld bytes) @0x%08lx",
             path, size, (unsigned long)address);

    const uint32_t image_size = ((uint32_t)size + 3U) & ~3U;
    esp_loader_flash_cfg_t cfg = {
        .offset = address,
        .image_size = image_size,
        .block_size = C6_FLASH_BLOCK_SIZE,
        .skip_verify = false,
    };
    esp_loader_error_t err = esp_loader_flash_start(loader, &cfg);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(C6_OTA_TAG, "flash start failed for %s: %d", path, err);
        fclose(f);
        return err;
    }

    static uint8_t buf[C6_FLASH_BLOCK_SIZE];
    size_t total = 0;
    while (total < (size_t)image_size) {
        memset(buf, 0xFF, sizeof(buf));
        size_t remaining_file = total < (size_t)size ? (size_t)size - total : 0;
        size_t to_read = remaining_file > sizeof(buf) ? sizeof(buf) : remaining_file;
        size_t n = to_read > 0 ? fread(buf, 1, to_read, f) : 0;
        if (n != to_read) {
            ESP_LOGE(C6_OTA_TAG, "read failed at %u / %ld bytes", (unsigned)total, size);
            fclose(f);
            return ESP_LOADER_ERROR_FAIL;
        }

        /* ROM flash writes and MD5 verification require the configured image
         * size to be 4-byte aligned. Official images normally are aligned,
         * but pad a short final block with erased-flash bytes defensively. */
        size_t chunk = to_read;
        if (chunk == 0) {
            chunk = (size_t)image_size - total;
        }

        err = esp_loader_flash_write(loader, &cfg, buf, (uint32_t)chunk);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(C6_OTA_TAG, "flash write failed at %u: %d", (unsigned)total, err);
            fclose(f);
            return err;
        }
        total += chunk;
        if (total == (size_t)size || (total % (256U * 1024U)) < sizeof(buf)) {
            ESP_LOGI(C6_OTA_TAG, "%s: %u / %ld bytes", path, (unsigned)total, size);
        }
    }
    fclose(f);

    err = esp_loader_flash_finish(loader, &cfg);
    ESP_LOGI(C6_OTA_TAG, "%s %s", path,
             err == ESP_LOADER_SUCCESS ? "flashed" : "flash verify failed");
    return err;
}

static void stop_hosted_transport_for_rom_flash(void)
{
    /* 已运行的 C6 才存在 ESP-Hosted 传输；空白 C6 不初始化它。 */
    hosted_wifi_deinit_for_rom_flash();
}

/* The stock SDIO port releases boot_pin after the reset pulse. For this board
 * IO0 is wired directly to C6 IO9 and must remain LOW for the entire ROM
 * flashing session. Release it only after all images have been verified. */
static void c6_sdio_enter_bootloader_hold(esp_loader_port_t *base)
{
    esp32_sdio_port_t *port = container_of(base, esp32_sdio_port_t, port);

    ESP_LOGI(C6_OTA_TAG, "enter download mode: GPIO%d LOW",
             (int)port->boot_pin);
    gpio_set_level(port->boot_pin, 0);
    ESP_LOGI(C6_OTA_TAG, "enter download mode: GPIO%d LOW",
             (int)port->reset_pin);
    gpio_set_level(port->reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(C6_OTA_TAG, "enter download mode: GPIO%d HIGH, GPIO%d stays LOW",
             (int)port->reset_pin, (int)port->boot_pin);
    gpio_set_level(port->reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Keep boot_pin low until flash_c6_from_sdcard() finishes. */
}

static esp_loader_error_t flash_c6_from_sdcard(void)
{
    long boot_size = 0;
    long part_size = 0;
    long app_size = 0;
    if (!get_file_size(C6_FW_BOOT, &boot_size) ||
        !get_file_size(C6_FW_PART, &part_size) ||
        !get_file_size(C6_FW_APP, &app_size) ||
        !validate_app_image(C6_FW_APP)) {
        ESP_LOGE(C6_OTA_TAG,
                 "C6 firmware set is missing or invalid; need %s, %s and %s",
                 C6_FW_BOOT, C6_FW_PART, C6_FW_APP);
        return ESP_LOADER_ERROR_INVALID_PARAM;
    }

    ESP_LOGI(C6_OTA_TAG, "C6 firmware set ready: boot=%ld part=%ld app=%ld bytes",
             boot_size, part_size, app_size);

    stop_hosted_transport_for_rom_flash();

    esp_loader_port_ops_t c6_sdio_ops = esp32_sdio_ops;
    c6_sdio_ops.enter_bootloader = c6_sdio_enter_bootloader_hold;

    esp32_sdio_port_t port = {
        .port.ops = &c6_sdio_ops,
        .slot = SDMMC_HOST_SLOT_1,
        .max_freq_khz = SDMMC_FREQ_DEFAULT,
        .sdio_clk_pin = GPIO_NUM_18,
        .sdio_cmd_pin = GPIO_NUM_19,
        .sdio_d0_pin = GPIO_NUM_14,
        .sdio_d1_pin = GPIO_NUM_15,
        .sdio_d2_pin = GPIO_NUM_16,
        .sdio_d3_pin = GPIO_NUM_17,
        .reset_pin = C6_RESET_PIN,
        .boot_pin = C6_BOOT_MODE_PIN,
        .dont_initialize_host_driver = false,
        .bus_width = SDIO_4BIT,
    };

    esp_loader_t loader;
    esp_loader_error_t err = esp_loader_init_sdio(&loader, &port.port);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(C6_OTA_TAG, "SDIO ROM loader init failed: %d", err);
        return err;
    }

    esp_loader_connect_args_t connect_args = {
        .sync_timeout = 200,
        .trials = 30,
    };
    err = esp_loader_connect(&loader, &connect_args);
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGE(C6_OTA_TAG, "C6 ROM download connect failed: %d", err);
        return err;
    }

    ESP_LOGI(C6_OTA_TAG, "C6 ROM connected, target=%d",
             (int)esp_loader_get_target(&loader));

    err = flash_file(&loader, C6_FW_BOOT, C6_BOOT_ADDR);
    if (err == ESP_LOADER_SUCCESS) err = flash_file(&loader, C6_FW_PART, C6_PART_ADDR);
    if (err == ESP_LOADER_SUCCESS) err = flash_file(&loader, C6_FW_APP, C6_APP_ADDR);

    if (err == ESP_LOADER_SUCCESS) {
        /* 三个镜像全部完成并通过校验后，才允许 IO9 变高。 */
        ESP_LOGI(C6_OTA_TAG, "flash verified: GPIO%d HIGH",
                 (int)C6_BOOT_MODE_PIN);
        gpio_set_level(C6_BOOT_MODE_PIN, 1);
        /* esp_loader_reset_target() is a void API; the flash result above is
         * the operation's success value. */
        ESP_LOGI(C6_OTA_TAG, "restart C6: GPIO%d LOW -> HIGH",
                 (int)C6_RESET_PIN);
        esp_loader_reset_target(&loader);
        ESP_LOGI(C6_OTA_TAG, "C6 ROM flash complete");
    } else {
        /* Keep IO0 low until the task restarts P4; startup sequencing will
         * restore the normal high level after C6 is held in reset. */
        ESP_LOGE(C6_OTA_TAG, "C6 ROM flash failed: %d", err);
    }
    return err;
}

esp_err_t c6_ota_from_sdcard(void)
{
    esp_loader_error_t err = flash_c6_from_sdcard();
    return err == ESP_LOADER_SUCCESS ? ESP_OK : ESP_FAIL;
}

static void c6_ota_task(void *arg)
{
    (void)arg;
    /* 等媒体任务关闭当前 MJPEG 文件，避免 SD 卡并发访问。 */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t ret = c6_ota_from_sdcard();
    if (s_c6_ota_event_cb != NULL) {
        s_c6_ota_event_cb(ret, s_c6_ota_event_user_data);
    }

    /* ROM flasher 已经拆掉了 ESP-Hosted 传输，成功/失败都重启 P4，
     * 让启动流程重新初始化 SDIO、Wi-Fi、BLE，避免半初始化状态。 */
    ESP_LOGW(C6_OTA_TAG, "C6 ROM flash %s, restarting P4",
             ret == ESP_OK ? "succeeded" : "failed");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

esp_err_t c6_ota_start_from_sdcard(void)
{
    portENTER_CRITICAL(&s_c6_ota_mux);
    if (s_c6_ota_running) {
        portEXIT_CRITICAL(&s_c6_ota_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_c6_ota_running = true;
    portEXIT_CRITICAL(&s_c6_ota_mux);

    if (xTaskCreate(c6_ota_task, "c6_rom_flash", 12288, NULL, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_c6_ota_mux);
        s_c6_ota_running = false;
        portEXIT_CRITICAL(&s_c6_ota_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

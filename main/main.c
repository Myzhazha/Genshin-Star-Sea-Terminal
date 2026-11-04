/*
 * main.c - ESP32-P4 4.3" ST7102 原神挂件 主入口
 *
 * 功能：
 *   - 开机循环播放 /sdcard/start/start.mjpeg
 *   - 短按 IO11 依次切换：启动视频 → 图片 → 视频 → 设置
 *   - 图片/视频页左右滑动切换文件
 *   - 设置页（LVGL_UI）对接全部外设：蓝牙 UID、喇叭、振动、Wi-Fi、
 *     时钟(DS1302)、亮度、图片/视频模式；右上角显示 INA226 电池电压电流
 *   - 附近设备（另一台挂件）BLE 靠近：弹窗 UID + 震动 + alert.mp3，
 *     距离越近提示越频繁（3s→0.5s），关闭后该 UID 静默 1 小时
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_config.h"
#include "hw_board.h"
#include "sdcard.h"
#include "ina226.h"
#include "ds1302.h"
#include "audio_mp3.h"
#include "hosted_wifi.h"
#include "proximity_ble.h"
#include "app_settings.h"
#include "app_media.h"
#include "lvgl_ui_settings.h"
#include "src/font/lv_binfont_loader.h"

/* 嵌入固件的中文字库（EMBED_FILES） */
extern const uint8_t font_hywenhei_cjk_16_start[] asm("_binary_hywenhei_cjk_16_bin_start");
extern const uint8_t font_hywenhei_cjk_16_end[]   asm("_binary_hywenhei_cjk_16_bin_end");
extern const uint8_t font_hywenhei_title_32_start[] asm("_binary_hywenhei_title_32_bin_start");
extern const uint8_t font_hywenhei_title_32_end[]   asm("_binary_hywenhei_title_32_bin_end");

#define MAIN_TAG "main"

/* C6 IO9 接到 P4 IO0，且板上没有外部上拉。先让 GPIO54 保持 C6
 * EN/CHIP_PU 为低，再把 IO0 稳定拉高，最后释放 EN 启动 C6。 */
static void init_c6_boot_mode_pin(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << C6_BOOT_MODE_PIN) | (1ULL << C6_RESET_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(C6_RESET_PIN, 0));
    ESP_ERROR_CHECK(gpio_set_level(C6_BOOT_MODE_PIN, 1));
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_ERROR_CHECK(gpio_set_level(C6_RESET_PIN, 1));
    ESP_LOGI(MAIN_TAG, "C6 boot mode GPIO%d set HIGH", C6_BOOT_MODE_PIN);
}

/* ---------------- 堆与启动检查 ---------------- */
static void check_psram(void)
{
    size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (total == 0) {
        ESP_LOGE(MAIN_TAG, "PSRAM not detected! MJPEG frame buffer needs PSRAM.");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(MAIN_TAG, "PSRAM total: %u KB", (unsigned)(total / 1024));
}

/* ---------------- DS1302 时钟同步 ---------------- */
static bool rtc_time_valid(const struct tm *t)
{
    int year = t->tm_year + 1900;
    if (year < 2024 || year > 2099) return false;
    if (t->tm_mon < 0 || t->tm_mon > 11) return false;
    if (t->tm_mday < 1 || t->tm_mday > 31) return false;
    if (t->tm_hour < 0 || t->tm_hour > 23) return false;
    if (t->tm_min < 0 || t->tm_min > 59) return false;
    if (t->tm_sec < 0 || t->tm_sec > 59) return false;
    return true;
}

static bool parse_build_time(struct tm *t)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char date_str[16], time_str[16];
    strncpy(date_str, __DATE__, sizeof(date_str) - 1);
    date_str[sizeof(date_str) - 1] = '\0';
    strncpy(time_str, __TIME__, sizeof(time_str) - 1);
    time_str[sizeof(time_str) - 1] = '\0';
    char mon[4];
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(date_str, "%3s %d %d", mon, &day, &year) != 3) return false;
    if (sscanf(time_str, "%d:%d:%d", &hh, &mm, &ss) != 3) return false;
    int mo = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(months[i], mon) == 0) { mo = i; break; }
    }
    if (mo < 0) return false;
    memset(t, 0, sizeof(*t));
    t->tm_year = year - 1900;
    t->tm_mon = mo;
    t->tm_mday = day;
    t->tm_hour = hh;
    t->tm_min = mm;
    t->tm_sec = ss;
    return true;
}

static void init_ds1302_and_sync(void)
{
    if (ds1302_init() != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "DS1302 init failed");
        return;
    }
    struct tm t;
    if (ds1302_read_time(&t) && rtc_time_valid(&t)) {
        struct timeval tv = { mktime(&t), 0 };
        settimeofday(&tv, NULL);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        ESP_LOGI(MAIN_TAG, "DS1302 valid: %s, system time synced", buf);
    } else {
        struct tm bt;
        if (parse_build_time(&bt)) {
            ds1302_write_time(&bt);
            struct timeval tv = { mktime(&bt), 0 };
            settimeofday(&tv, NULL);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &bt);
            ESP_LOGI(MAIN_TAG, "DS1302 invalid, wrote build time: %s", buf);
        } else {
            ESP_LOGW(MAIN_TAG, "DS1302 invalid and build time parse failed");
        }
    }
}

/* ---------------- 设备 ID（NVS） ---------------- */
static uint32_t get_device_id(nvs_handle_t nvs)
{
    uint32_t id = 0;
    if (nvs_get_u32(nvs, "deviceid", &id) == ESP_OK && id != 0) {
        return id;
    }
    id = esp_random();
    if (id == 0) id = 1;
    nvs_set_u32(nvs, "deviceid", id);
    nvs_commit(nvs);
    return id;
}

/* ---------------- 入口 ---------------- */
void app_main(void)
{
    /* 设备界面和 DS1302 使用中国标准时间；系统时钟仍保存 UTC epoch。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 尽可能早地建立 C6 启动模式电平，并保证 C6 只在 IO0 拉高后启动。 */
    init_c6_boot_mode_pin();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    check_psram();

    /* 屏幕 / 触摸 / 背光 / LVGL */
    static app_media_ctx_t ctx;
    ESP_ERROR_CHECK(hw_board_init(&ctx.board));

    /* 外设 */
    ret = ina226_init(&ctx.ina, INA226_I2C_PORT, INA226_SDA, INA226_SCL,
                      INA226_ADDR, INA226_SHUNT_MOHM);
    if (ret != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "INA226 unavailable (%s)", esp_err_to_name(ret));
    }
    init_ds1302_and_sync();
    ESP_ERROR_CHECK(audio_init());

    /* 先读取设置和 C6 状态。空白 C6 没有 ESP-Hosted 从机可连接，
     * P4 必须能够独立启动并进入设置页，通过 SDIO ROM 下载器烧录。 */
    app_settings_init();

    /* esp-hosted：WiFi + BLE（C6 经 SDIO）。只有 C6 已经刷过固件时才
     * 尝试建立传输；否则启动阶段不再无意义地等待 SDIO 超时。 */
    nvs_handle_t nvs;
    uint32_t device_id = esp_random();
    if (device_id == 0) device_id = 1;
    if (nvs_open("genshin", NVS_READWRITE, &nvs) == ESP_OK) {
        device_id = get_device_id(nvs);
        nvs_close(nvs);
    }

    bool c6_connected = false;
    if (app_settings_c6_firmware_ready()) {
        ret = hosted_wifi_init();
        if (ret != ESP_OK) {
            app_settings_set_c6_firmware_ready(false);
            ESP_LOGW(MAIN_TAG, "hosted WiFi init failed: %s; C6 marked not ready",
                     esp_err_to_name(ret));
        } else {
            app_settings_set_c6_firmware_ready(true);
            c6_connected = true;
        }
    } else {
        ESP_LOGI(MAIN_TAG, "C6 firmware not marked ready; skipping ESP-Hosted until SDIO update");
    }
    lvgl_ui_settings_set_c6_connected(c6_connected);

    /* SD 卡（esp-hosted 传输就绪后挂载） */
    bool sd_ok = (sdcard_init() == ESP_OK);
    if (sd_ok) {
        sdcard_scan_all(&ctx.start_videos, &ctx.pictures, &ctx.videos);
    }

    /* 中文字库已嵌入固件（flash XIP，直接内存加载，毫秒级）。
     * 旧方案从 SD 卡读 5MB 字体需 26 秒，已废弃。 */
    const lv_font_t *ui_font = lv_binfont_create_from_buffer(
        (void *)font_hywenhei_cjk_16_start,
        (uint32_t)(font_hywenhei_cjk_16_end - font_hywenhei_cjk_16_start));
    if (ui_font != NULL) {
        lvgl_ui_settings_set_font(ui_font);
        ESP_LOGI(MAIN_TAG, "UI font loaded from flash (embedded)");
    } else {
        ESP_LOGW(MAIN_TAG, "UI font load failed, using built-in CJK");
    }
    /* 设置页大标题专用字体（32px 金色大字） */
    const lv_font_t *title_font = lv_binfont_create_from_buffer(
        (void *)font_hywenhei_title_32_start,
        (uint32_t)(font_hywenhei_title_32_end - font_hywenhei_title_32_start));
    if (title_font != NULL) {
        lvgl_ui_settings_set_title_font(title_font);
        ESP_LOGI(MAIN_TAG, "title font loaded from %s", SD_TITLE_FONT_BIN);
    } else {
        ESP_LOGW(MAIN_TAG, "title font load failed (%s), using UI font", SD_TITLE_FONT_BIN);
    }

    /* MJPEG 播放器（开机启动视频） */
    const char *startup = ctx.start_videos.count
                              ? ctx.start_videos.path[0]
                              : "/sdcard/start/start.mjpeg";
    if (mjpeg_player_begin(&ctx.mjpeg, startup, LCD_H_RES, LCD_V_RES) != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "MJPEG player init failed (SD files missing?)");
    } else {
        mjpeg_player_set_loop(&ctx.mjpeg, true);
    }

    /* 媒体页 + 设置页 UI + 任务；失败不 abort，降级继续（下次启动重试） */
    esp_err_t media_ret = app_media_init(&ctx);
    if (media_ret != ESP_OK) {
        ESP_LOGW(MAIN_TAG, "app_media_init failed (%s), UI degraded", esp_err_to_name(media_ret));
    }

    const char *uid = app_settings_get_uid();
    bool nearby = app_settings_nearby_enabled();
    if (app_settings_c6_firmware_ready()) {
        if (!proximity_ble_begin(device_id, uid, nearby, app_media_proximity_report, NULL)) {
            ESP_LOGW(MAIN_TAG, "proximity BLE failed to start");
        }
    } else {
        ESP_LOGI(MAIN_TAG, "C6 firmware not ready; skipping proximity BLE");
    }

    ESP_LOGI(MAIN_TAG, "system ready: sd=%d start=%u pic=%u video=%u uid='%s' nearby=%d",
             sd_ok, ctx.start_videos.count, ctx.pictures.count, ctx.videos.count,
             uid, nearby);

    /* 周期处理：接近检测（10ms） */
    while (1) {
        app_media_process_proximity();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

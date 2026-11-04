/*
 * app_settings.c - 设置页事件对接外设 + NVS 持久化
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "lvgl_ui_settings.h"
#include "esp_lvgl_port.h"
#include "app_settings.h"
#include "app_config.h"
#include "audio_mp3.h"
#include "ds1302.h"
#include "proximity_ble.h"
#include "app_media.h"
#include "hosted_wifi.h"
#include "hw_board.h"
#include "c6_ota.h"

#define APP_SETTINGS_TAG "settings"
#define NVS_NAMESPACE    "genshin"
#define ENCOUNTER_UID_MAX 256
#define ENCOUNTER_UID_LEN 11
#define ENCOUNTER_RESULTS_MAX (ENCOUNTER_UID_MAX * (ENCOUNTER_UID_LEN + 1))

/* ---------- 运行态设置（与 NVS 同步） ---------- */
static char s_uid[24] = "";
static bool s_nearby_enabled = false;
static uint16_t s_scan_seconds = 0;
static bool s_vibration_enabled = false;
static uint8_t s_vibration_level = 50;
static bool s_speaker_enabled = true;
static uint8_t s_speaker_volume = 70;
static bool s_wifi_enabled = false;
static bool s_c6_firmware_ready = false;
static volatile bool s_c6_update_active = false;
static volatile bool s_c6_update_restore_pending = false;
static bool s_image_auto = false;
static uint16_t s_image_interval = 10;
static uint16_t s_video_mode = 0;
static bool s_clock_24h = true;
static bool s_clock_auto_sync = true;
static char s_encounter_uids[ENCOUNTER_UID_MAX][ENCOUNTER_UID_LEN];
static uint16_t s_encounter_count = 0;
static char s_encounter_results[ENCOUNTER_RESULTS_MAX];

static char s_wifi_ssid[33] = "";

/* ---------- NVS 辅助（避免与 IDF nvs_set_* API 同名） ---------- */
static nvs_handle_t s_nvs;
static bool s_nvs_ready = false;

static esp_err_t load_u8(const char *key, uint8_t *value)
{
    return s_nvs_ready ? nvs_get_u8(s_nvs, key, value) : ESP_ERR_INVALID_STATE;
}

static esp_err_t load_u16(const char *key, uint16_t *value)
{
    return s_nvs_ready ? nvs_get_u16(s_nvs, key, value) : ESP_ERR_INVALID_STATE;
}

static esp_err_t load_blob(const char *key, void *value, size_t *length)
{
    return s_nvs_ready ? nvs_get_blob(s_nvs, key, value, length) : ESP_ERR_INVALID_STATE;
}

static void commit_setting(esp_err_t set_result, const char *key)
{
    if (!s_nvs_ready) return;
    if (set_result != ESP_OK) {
        ESP_LOGW(APP_SETTINGS_TAG, "failed to store %s: %s", key,
                 esp_err_to_name(set_result));
        return;
    }
    esp_err_t ret = nvs_commit(s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(APP_SETTINGS_TAG, "failed to commit %s: %s", key,
                 esp_err_to_name(ret));
    }
}

static void store_str(const char *key, const char *val)
{
    if (!s_nvs_ready) return;
    commit_setting(nvs_set_str(s_nvs, key, val), key);
}

static void store_u8(const char *key, uint8_t val)
{
    if (!s_nvs_ready) return;
    commit_setting(nvs_set_u8(s_nvs, key, val), key);
}

static void store_u16(const char *key, uint16_t val)
{
    if (!s_nvs_ready) return;
    commit_setting(nvs_set_u16(s_nvs, key, val), key);
}

static void store_bool(const char *key, bool val)
{
    if (!s_nvs_ready) return;
    commit_setting(nvs_set_u8(s_nvs, key, val ? 1 : 0), key);
}

static void rebuild_encounter_results(void)
{
    size_t used = 0;
    s_encounter_results[0] = '\0';
    for (uint16_t i = 0; i < s_encounter_count; i++) {
        size_t uid_len = strlen(s_encounter_uids[i]);
        if (used + uid_len + (used ? 1 : 0) >= sizeof(s_encounter_results)) {
            break;
        }
        if (used) {
            s_encounter_results[used++] = '\n';
        }
        memcpy(s_encounter_results + used, s_encounter_uids[i], uid_len);
        used += uid_len;
        s_encounter_results[used] = '\0';
    }
}

static void store_encounters(void)
{
    if (!s_nvs_ready) return;
    esp_err_t ret = s_encounter_count > 0
                        ? nvs_set_blob(s_nvs, "encuids", s_encounter_uids,
                                       (size_t)s_encounter_count * ENCOUNTER_UID_LEN)
                        : nvs_erase_key(s_nvs, "encuids");
    commit_setting(ret, "encuids");
}

/* ---------- 振动（TC118S H 桥：INA 固定低，INB 走 PWM 调速） ---------- */
static void vib_gpio_init(void)
{
    static bool initialized = false;
    if (!initialized) {
        gpio_config_t cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << CONFIG_GENSHIN_VIBRATION_INA_PIN,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(CONFIG_GENSHIN_VIBRATION_INA_PIN, 0);   /* INA 固定低 */

        /* INB 用 LEDC PWM 调速（5kHz；TIMER_1/CH_1 与背光 TIMER_0 不冲突） */
        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer_cfg);
        ledc_channel_config_t ch_cfg = {
            .gpio_num = CONFIG_GENSHIN_VIBRATION_INB_PIN,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&ch_cfg);
        initialized = true;
    }
}

static void vib_set_pwm(bool on)
{
    uint32_t duty = 0;
    if (on) {
        /* 强度 0~100 → 占空比 30%~100%（低于 30% 马达可能不转） */
        uint8_t lvl = s_vibration_level > 100 ? 100 : s_vibration_level;
        duty = (uint32_t)lvl * 1023U / 100U;
        if (duty < 307) duty = 307;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/** 启动震动（INA=0 + INB=PWM(强度) → 力度随设置页"振动强度"） */
void app_settings_vibration_start(void)
{
    vib_gpio_init();
    vib_set_pwm(true);
}

void app_settings_vibration_start_max(void)
{
    vib_gpio_init();
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1023);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/** 停止震动（INB PWM=0 → 滑行停止） */
void app_settings_vibration_stop(void)
{
    vib_gpio_init();
    vib_set_pwm(false);
}

void app_settings_vibration_pulse(uint32_t duration_ms)
{
    app_settings_vibration_start();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    app_settings_vibration_stop();
}

/* ---------- 设置事件回调 ---------- */

/* WiFi 扫描完成：把 SSID 列表刷新到 UI（事件循环线程，需加 LVGL 锁） */
static void wifi_scan_done_cb(const char *ssid_list, void *user_data)
{
    (void)user_data;
    if (lvgl_port_lock(0)) {
        lvgl_ui_settings_set_wifi_results(ssid_list);
        lvgl_port_unlock();
    }
}

static void wifi_status_cb(hosted_wifi_status_t status, void *user_data)
{
    (void)user_data;
    const char *text = "未连接";
    if (status == HOSTED_WIFI_STATUS_CONNECTING) {
        text = "正在连接...";
    } else if (status == HOSTED_WIFI_STATUS_CONNECTED) {
        text = "已连接";
    } else if (status == HOSTED_WIFI_STATUS_DISCONNECTED) {
        text = "连接失败";
    }
    if (lvgl_port_lock(0)) {
        lvgl_ui_settings_set_wifi_status(text);
        lvgl_port_unlock();
    }
}

void app_settings_set_c6_firmware_ready(bool ready)
{
    s_c6_firmware_ready = ready;
    store_bool("c6ready", ready);
}

static void c6_ota_event_cb(esp_err_t result, void *user_data)
{
    (void)user_data;
    if (result == ESP_OK) {
        app_settings_set_c6_firmware_ready(true);
        if (lvgl_port_lock(50)) {
            lvgl_ui_settings_set_c6_update_status("更新完成，设备即将重启", false);
            lvgl_port_unlock();
        }
        return;
    }

    /* c6_ota_task restarts the P4 after both success and failure. Keep the
     * media task paused until that reboot so it cannot reopen the SD card
     * while the ROM flasher still owns the SDIO host driver. */
    if (lvgl_port_lock(50)) {
        lvgl_ui_settings_set_c6_update_status("更新失败，请检查 C6 固件文件", true);
        lvgl_port_unlock();
    }
}

static void settings_event_cb(lvgl_ui_setting_t setting, int32_t value,
                              const char *text, bool enabled, void *user_data)
{
    (void)user_data;
    switch (setting) {
    case LVGL_UI_SETTING_UID:
        lv_strlcpy(s_uid, text != NULL ? text : "", sizeof(s_uid));
        store_str("uid", s_uid);
        proximity_ble_update(s_uid, s_nearby_enabled);
        break;

    case LVGL_UI_SETTING_UID_VISIBLE:
        s_nearby_enabled = enabled;
        store_bool("nearby", enabled);
        proximity_ble_update(s_uid, enabled);
        if (!enabled) {
            lvgl_ui_settings_close_nearby_alert();
        }
        break;

    case LVGL_UI_SETTING_UID_SCAN_SECONDS:
        s_scan_seconds = (uint16_t)value;
        store_u16("scanfreq", s_scan_seconds);
        break;

    case LVGL_UI_SETTING_SPEAKER_ENABLED:
        s_speaker_enabled = enabled;
        store_bool("speaker", enabled);
        audio_set_enabled(enabled);
        break;

    case LVGL_UI_SETTING_SPEAKER_VOLUME:
        s_speaker_volume = (uint8_t)value;
        store_u8("volume", s_speaker_volume);
        audio_set_volume(s_speaker_volume);
        break;

    case LVGL_UI_SETTING_VIBRATION_ENABLED:
        s_vibration_enabled = enabled;
        store_bool("vibration", enabled);
        break;

    case LVGL_UI_SETTING_VIBRATION_INTENSITY:
        s_vibration_level = (uint8_t)value;
        store_u8("viblevel", s_vibration_level);
        break;

    case LVGL_UI_SETTING_WIFI_ENABLED:
        s_wifi_enabled = enabled;
        store_bool("wifi", enabled);
        hosted_wifi_set_enabled(enabled);
        if (!enabled) {
            lvgl_ui_settings_set_wifi_status("未连接");
        }
        break;

    case LVGL_UI_SETTING_WIFI_SCAN_REQUESTED:
        hosted_wifi_scan_start();
        break;

    case LVGL_UI_SETTING_WIFI_SSID:
        if (text != NULL) {
            lv_strlcpy(s_wifi_ssid, text, sizeof(s_wifi_ssid));
        }
        break;

    case LVGL_UI_SETTING_WIFI_PASSWORD:
        if (s_wifi_ssid[0] != '\0') {
            ESP_LOGI(APP_SETTINGS_TAG, "connecting to %s", s_wifi_ssid);
            esp_err_t wifi_ret = hosted_wifi_connect(s_wifi_ssid, text != NULL ? text : "");
            if (wifi_ret != ESP_OK) {
                lvgl_ui_settings_set_wifi_status("连接失败");
            }
        }
        break;

    case LVGL_UI_SETTING_CLOCK_TEXT: {
        /* "HH:MM" → 写 DS1302 并同步系统时间 */
        int hour = 0, minute = 0;
        if (text != NULL && sscanf(text, "%d:%d", &hour, &minute) == 2 &&
            hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
            if (s_clock_auto_sync) {
                ESP_LOGI(APP_SETTINGS_TAG,
                         "manual clock change ignored while automatic sync is enabled");
                break;
            }
            time_t now = time(NULL);
            struct tm t;
            localtime_r(&now, &t);
            t.tm_hour = hour;
            t.tm_min = minute;
            t.tm_sec = 0;
            ds1302_write_time(&t);
            struct timeval tv = { mktime(&t), 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(APP_SETTINGS_TAG, "clock set to %02d:%02d", hour, minute);
        }
        break;
    }

    case LVGL_UI_SETTING_CLOCK_24H:
        s_clock_24h = enabled;
        store_bool("fmt24", enabled);
        break;

    case LVGL_UI_SETTING_CLOCK_AUTO_SYNC:
        s_clock_auto_sync = enabled;
        store_bool("autosync", enabled);
        hosted_wifi_set_time_sync_enabled(enabled);
        break;

    case LVGL_UI_SETTING_BRIGHTNESS:
        if (value < 20) value = 20;
        store_u8("brightness", (uint8_t)value);
        hw_board_set_backlight((uint8_t)value);
        break;

    case LVGL_UI_SETTING_IMAGE_AUTO:
        s_image_auto = enabled;
        store_bool("imgauto", enabled);
        break;

    case LVGL_UI_SETTING_IMAGE_INTERVAL_SECONDS:
        s_image_interval = (uint16_t)value;
        store_u16("imgtime", s_image_interval);
        break;

    case LVGL_UI_SETTING_VIDEO_MODE:
        s_video_mode = (uint16_t)value;
        store_u16("vidmode", s_video_mode);
        break;

    case LVGL_UI_SETTING_C6_FIRMWARE_UPDATE_REQUESTED:
        ESP_LOGI(APP_SETTINGS_TAG, "C6 firmware update requested from settings");
        if (s_c6_update_active || c6_ota_is_running()) {
            lvgl_ui_settings_set_c6_update_status("正在更新，请勿重复操作", false);
            break;
        }
        s_c6_update_active = true;
        /* OTA 与 Wi-Fi/BLE 共用 ESP-Hosted SDIO，更新期间暂停上层业务流量。 */
        proximity_ble_update(s_uid, false);
        hosted_wifi_set_enabled(false);
        audio_stop();
        app_settings_vibration_stop();
        lvgl_ui_settings_set_c6_update_status("正在通过 SDIO 更新 C6，请勿断电", false);
        if (c6_ota_start_from_sdcard() != ESP_OK) {
            s_c6_update_active = false;
            hosted_wifi_set_enabled(s_wifi_enabled);
            proximity_ble_update(s_uid, s_nearby_enabled);
            lvgl_ui_settings_set_c6_update_status("更新无法开始，请检查 C6 固件文件", true);
            ESP_LOGW(APP_SETTINGS_TAG,
                     "C6 firmware update is already running or could not start");
        }
        break;

    case LVGL_UI_SETTING_ENCOUNTER_DELETE:
        if (app_settings_delete_encounter(text)) {
            app_media_forget_proximity_uid(text);
            lvgl_ui_settings_set_encounter_count(s_encounter_count);
            lvgl_ui_settings_set_encounter_records(s_encounter_results);
        }
        break;

    default:
        break;
    }
}

/* ---------- 初始化 ---------- */
void app_settings_init(void)
{
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(APP_SETTINGS_TAG, "nvs open failed: %s", esp_err_to_name(ret));
    } else {
        s_nvs_ready = true;
    }

    /* 读 NVS（默认值与 UI 一致） */
    size_t len = sizeof(s_uid);
    if (!s_nvs_ready || nvs_get_str(s_nvs, "uid", s_uid, &len) != ESP_OK) {
        s_uid[0] = '\0';
    }
    uint8_t v = 0;
    s_nearby_enabled = (load_u8("nearby", &v) == ESP_OK) && (v != 0);
    s_scan_seconds = 0;
    uint16_t u16 = 0;
    if (load_u16("scanfreq", &u16) == ESP_OK) {
        s_scan_seconds = (u16 == 5 || u16 == 10) ? u16 : 0;
    }
    s_speaker_enabled = true;
    if (load_u8("speaker", &v) == ESP_OK) s_speaker_enabled = (v != 0);
    s_speaker_volume = 70;
    if (load_u8("volume", &v) == ESP_OK) s_speaker_volume = v;
    s_vibration_enabled = false;
    if (load_u8("vibration", &v) == ESP_OK) s_vibration_enabled = (v != 0);
    s_vibration_level = 50;
    if (load_u8("viblevel", &v) == ESP_OK) s_vibration_level = v;
    s_wifi_enabled = false;
    if (load_u8("wifi", &v) == ESP_OK) s_wifi_enabled = (v != 0);
    s_c6_firmware_ready = (load_u8("c6ready", &v) == ESP_OK) && (v != 0);
    s_image_auto = false;
    if (load_u8("imgauto", &v) == ESP_OK) s_image_auto = (v != 0);
    s_image_interval = 10;
    if (load_u16("imgtime", &u16) == ESP_OK) s_image_interval = u16;
    s_video_mode = 0;
    if (load_u16("vidmode", &u16) == ESP_OK) s_video_mode = u16;
    s_clock_24h = true;
    if (load_u8("fmt24", &v) == ESP_OK) s_clock_24h = (v != 0);
    s_clock_auto_sync = true;
    if (load_u8("autosync", &v) == ESP_OK) s_clock_auto_sync = (v != 0);
    uint8_t brightness = 80;
    if (load_u8("brightness", &v) == ESP_OK) brightness = v;
    if (brightness < 20) brightness = 20;

    size_t encounter_blob_size = sizeof(s_encounter_uids);
    if (load_blob("encuids", s_encounter_uids, &encounter_blob_size) == ESP_OK) {
        s_encounter_count = (uint16_t)(encounter_blob_size / ENCOUNTER_UID_LEN);
        if (s_encounter_count > ENCOUNTER_UID_MAX) {
            s_encounter_count = ENCOUNTER_UID_MAX;
        }
    } else {
        s_encounter_count = 0;
        memset(s_encounter_uids, 0, sizeof(s_encounter_uids));
    }
    rebuild_encounter_results();

    /* 恢复 UI 初始状态 */
    lvgl_ui_settings_initial_t init = {
        .uid = s_uid[0] ? s_uid : NULL,
        .uid_visible = s_nearby_enabled,
        .uid_duration = s_scan_seconds,
        .speaker_enabled = s_speaker_enabled,
        .speaker_volume = s_speaker_volume,
        .vibration_enabled = s_vibration_enabled,
        .vibration_intensity = s_vibration_level,
        .brightness = brightness,
        .image_auto = s_image_auto,
        .image_interval = s_image_interval,
        .video_mode = s_video_mode,
        .wifi_enabled = s_wifi_enabled,
        .clock_24h = s_clock_24h,
        .clock_auto_sync = s_clock_auto_sync,
        .encounter_count = s_encounter_count,
    };
    lvgl_ui_settings_set_initial(&init);
    lvgl_ui_settings_set_encounter_records(s_encounter_results);

    /* 同步外设 */
    audio_set_volume(s_speaker_volume);
    if (!s_speaker_enabled) {
        audio_set_enabled(false);
    }
    hw_board_set_backlight(brightness);
    hosted_wifi_set_enabled(s_wifi_enabled);
    hosted_wifi_set_time_sync_enabled(s_clock_auto_sync);

    lvgl_ui_settings_set_event_cb(settings_event_cb, NULL);
    c6_ota_set_event_cb(c6_ota_event_cb, NULL);
    hosted_wifi_set_scan_done_cb(wifi_scan_done_cb, NULL);
    hosted_wifi_set_status_cb(wifi_status_cb, NULL);
    ESP_LOGI(APP_SETTINGS_TAG, "settings loaded (uid='%s' nearby=%d vol=%u wifi=%d)",
             s_uid, s_nearby_enabled, s_speaker_volume, s_wifi_enabled);
}

/* ---------- getters ---------- */
const char *app_settings_get_uid(void) { return s_uid; }
bool app_settings_nearby_enabled(void) { return s_nearby_enabled; }
uint16_t app_settings_scan_seconds(void) { return s_scan_seconds; }
bool app_settings_image_auto(void) { return s_image_auto; }
uint16_t app_settings_image_interval(void) { return s_image_interval; }
uint16_t app_settings_video_mode(void) { return s_video_mode; }
bool app_settings_vibration_enabled(void) { return s_vibration_enabled; }
uint8_t app_settings_vibration_level(void) { return s_vibration_level; }
bool app_settings_speaker_enabled(void) { return s_speaker_enabled; }
uint8_t app_settings_speaker_volume(void) { return s_speaker_volume; }
bool app_settings_clock_24h(void) { return s_clock_24h; }
bool app_settings_clock_auto_sync(void) { return s_clock_auto_sync; }
bool app_settings_c6_update_active(void) { return s_c6_update_active; }
bool app_settings_c6_firmware_ready(void) { return s_c6_firmware_ready; }

void app_settings_process_c6_update(void)
{
    if (!s_c6_update_restore_pending) return;
    s_c6_update_restore_pending = false;
    hosted_wifi_set_enabled(s_wifi_enabled);
    proximity_ble_update(s_uid, s_nearby_enabled);
}

uint16_t app_settings_encounter_count(void)
{
    return s_encounter_count;
}

uint16_t app_settings_record_encounter(const char *uid)
{
    if (uid == NULL || uid[0] == '\0') {
        return s_encounter_count;
    }

    for (uint16_t i = 0; i < s_encounter_count; i++) {
        if (strcmp(s_encounter_uids[i], uid) == 0) {
            return s_encounter_count;
        }
    }

    if (s_encounter_count >= ENCOUNTER_UID_MAX) {
        return s_encounter_count;
    }

    lv_strlcpy(s_encounter_uids[s_encounter_count], uid, ENCOUNTER_UID_LEN);
    s_encounter_count++;
    rebuild_encounter_results();
    store_encounters();
    return s_encounter_count;
}

const char *app_settings_encounter_records(void)
{
    return s_encounter_results;
}

bool app_settings_delete_encounter(const char *uid)
{
    if (uid == NULL || uid[0] == '\0') {
        return false;
    }

    for (uint16_t i = 0; i < s_encounter_count; i++) {
        if (strcmp(s_encounter_uids[i], uid) != 0) {
            continue;
        }
        if (i + 1 < s_encounter_count) {
            memmove(&s_encounter_uids[i], &s_encounter_uids[i + 1],
                    (size_t)(s_encounter_count - i - 1) * ENCOUNTER_UID_LEN);
        }
        s_encounter_count--;
        memset(s_encounter_uids[s_encounter_count], 0, ENCOUNTER_UID_LEN);
        rebuild_encounter_results();
        store_encounters();
        return true;
    }
    return false;
}

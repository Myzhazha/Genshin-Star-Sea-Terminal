#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the 480x800 Genshin-inspired settings UI.
 *
 * The function only creates LVGL objects and keeps all settings in local
 * memory. It does not depend on ESP-IDF, Wi-Fi, audio, vibration, or RTC
 * drivers.
 */
void lvgl_ui_settings_create(void);

typedef enum {
    LVGL_UI_SETTING_UID = 0,
    LVGL_UI_SETTING_UID_VISIBLE,
    LVGL_UI_SETTING_UID_SCAN_SECONDS,
    LVGL_UI_SETTING_SPEAKER_ENABLED,
    LVGL_UI_SETTING_SPEAKER_VOLUME,
    LVGL_UI_SETTING_VIBRATION_ENABLED,
    LVGL_UI_SETTING_VIBRATION_INTENSITY,
    LVGL_UI_SETTING_WIFI_ENABLED,
    LVGL_UI_SETTING_WIFI_SCAN_REQUESTED,
    LVGL_UI_SETTING_WIFI_SSID,
    LVGL_UI_SETTING_WIFI_PASSWORD,
    LVGL_UI_SETTING_CLOCK_TEXT,
    LVGL_UI_SETTING_CLOCK_AUTO_SYNC,
    LVGL_UI_SETTING_CLOCK_24H,
    LVGL_UI_SETTING_BRIGHTNESS,
    LVGL_UI_SETTING_IMAGE_AUTO,
    LVGL_UI_SETTING_IMAGE_INTERVAL_SECONDS,
    LVGL_UI_SETTING_VIDEO_MODE,
    LVGL_UI_SETTING_ENCOUNTER_DELETE,
    LVGL_UI_SETTING_C6_FIRMWARE_UPDATE_REQUESTED,
} lvgl_ui_setting_t;

typedef void (*lvgl_ui_setting_cb_t)(lvgl_ui_setting_t setting,
                                     int32_t value,
                                     const char *text,
                                     bool enabled,
                                     void *user_data);

void lvgl_ui_settings_set_event_cb(lvgl_ui_setting_cb_t callback, void *user_data);

/** @brief 用 SD 卡加载的完整字库替换默认字体（开机加载后调用） */
void lvgl_ui_settings_set_font(const lv_font_t *font);

/** @brief 设置页大标题专用字体（48px 金色，/sdcard/font/ 下的 .bin） */
void lvgl_ui_settings_set_title_font(const lv_font_t *font);

void lvgl_ui_settings_set_battery(float voltage_v, float current_ma, bool valid);

/**
 * @brief 开机初始状态（从 NVS 恢复）。必须在 lvgl_ui_settings_create() 之前调用。
 */
typedef struct {
    const char *uid;
    bool uid_visible;
    uint16_t uid_duration;         /* 0=始终扫描 5=5秒 10=10秒 */
    bool speaker_enabled;
    int32_t speaker_volume;
    bool vibration_enabled;
    int32_t vibration_intensity;   /* 0~100 */        /* 0~100 */
    int32_t brightness;            /* 0~100 */
    bool image_auto;
    uint16_t image_interval;       /* 秒 */
    uint16_t video_mode;           /* 0=单个循环 1=自动轮播 */
    bool wifi_enabled;
    bool clock_24h;
    bool clock_auto_sync;
    uint16_t encounter_count;
} lvgl_ui_settings_initial_t;

void lvgl_ui_settings_set_initial(const lvgl_ui_settings_initial_t *init);

/**
 * @brief 附近设备 UID 弹窗（模态，显示在顶层；两个挂件互相弹出）。
 *        点击"关闭"后调用 close_cb。
 */
typedef void (*lvgl_ui_nearby_alert_close_cb_t)(void *user_data);

void lvgl_ui_settings_show_nearby_alert(const char *uid, int16_t rssi,
                                        lvgl_ui_nearby_alert_close_cb_t close_cb,
                                        void *user_data);
void lvgl_ui_settings_update_nearby_rssi(int16_t rssi);
void lvgl_ui_settings_close_nearby_alert(void);

/** @brief 低电量提醒弹窗（显示在顶层；点击"关闭"后调用 close_cb）。 */
void lvgl_ui_settings_show_battery_low_alert(lvgl_ui_nearby_alert_close_cb_t close_cb,
                                              void *user_data);
void lvgl_ui_settings_close_battery_low_alert(void);

/** @brief 显示 C6 固件更新确认框。 */
void lvgl_ui_settings_show_c6_update_confirm(void);

/** @brief 刷新 C6 更新状态；can_close 为 true 时显示关闭按钮。 */
void lvgl_ui_settings_set_c6_update_status(const char *text, bool can_close);

/** @brief 当前选中的 Wi-Fi SSID（连接密码用） */
const char *lvgl_ui_settings_get_selected_wifi_ssid(void);

/** @brief 异步扫描完成后回填结果（SSID 列表，'\n' 分隔） */
void lvgl_ui_settings_set_wifi_results(const char *newline_separated_ssids);

/** @brief 更新 Wi-Fi 密码连接状态文本。 */
void lvgl_ui_settings_set_wifi_status(const char *status);

/** @brief 回填最近扫描到的附近设备 UID（每行一个）。 */
void lvgl_ui_settings_set_nearby_results(const char *newline_separated_uids);

/** @brief 更新累计相遇人数。 */
void lvgl_ui_settings_set_encounter_count(uint16_t count);
void lvgl_ui_settings_set_encounter_records(const char *newline_separated_uids);

/** @brief 更新关于终端页面中的存储使用量。 */
void lvgl_ui_settings_set_system_info(uint64_t flash_used, uint64_t flash_total,
                                      uint64_t psram_used, uint64_t psram_total,
                                      uint64_t tf_used, uint64_t tf_total,
                                      bool tf_ready);

/** @brief 更新关于终端页面中的 C6 SDIO 连接状态。 */
void lvgl_ui_settings_set_c6_connected(bool connected);

/**
 * @brief Return the screen object used by the UI.
 */
lv_obj_t *lvgl_ui_settings_get_root(void);

#ifdef __cplusplus
}
#endif

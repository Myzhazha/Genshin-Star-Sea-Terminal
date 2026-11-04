/*
 * app_settings.h - 设置页事件对接外设 + NVS 持久化
 *
 * 把 LVGL_UI 的回调事件翻译成外设动作：
 *   UID/可见性 → 蓝牙广播；喇叭 → NS4168；振动 → PWM 马达；
 *   Wi-Fi → esp-hosted；时钟 → DS1302；亮度 → 背光 PWM；
 *   图片/视频 → 媒体播放模式；全部设置存 NVS。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化：读取 NVS → 恢复 UI 初始状态 → 同步外设 → 注册事件回调 */
void app_settings_init(void);

/* ---- 供媒体页 / 接近检测读取的当前设置 ---- */
const char *app_settings_get_uid(void);
bool app_settings_nearby_enabled(void);
uint16_t app_settings_scan_seconds(void);          /* 0=始终 5/10=间隔秒 */
bool app_settings_image_auto(void);
uint16_t app_settings_image_interval(void);        /* 秒 */
uint16_t app_settings_video_mode(void);            /* 0=单个循环 1=自动轮播 */
bool app_settings_vibration_enabled(void);
uint8_t app_settings_vibration_level(void);
bool app_settings_speaker_enabled(void);
uint8_t app_settings_speaker_volume(void);
bool app_settings_clock_24h(void);
bool app_settings_clock_auto_sync(void);
bool app_settings_c6_update_active(void);
void app_settings_process_c6_update(void);
bool app_settings_c6_firmware_ready(void);
void app_settings_set_c6_firmware_ready(bool ready);

/** @brief 振动（TC118S H 桥：INA/INB 两脚驱动）。 */
void app_settings_vibration_start(void);
/** @brief 以最大 PWM 转速启动震动，用于附近设备提醒。 */
void app_settings_vibration_start_max(void);
void app_settings_vibration_stop(void);
void app_settings_vibration_pulse(uint32_t duration_ms);

/** @brief 记录一次相遇；相同 UID 只计数一次，并持久化到 NVS。 */
uint16_t app_settings_record_encounter(const char *uid);
uint16_t app_settings_encounter_count(void);
const char *app_settings_encounter_records(void);
bool app_settings_delete_encounter(const char *uid);

#ifdef __cplusplus
}
#endif

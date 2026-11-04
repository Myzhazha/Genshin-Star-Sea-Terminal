/*
 * app_media.c - 媒体页（启动/图片/视频）+ 模式切换 + 接近检测
 *
 * 移植自 v1.3.2 的页面逻辑：
 *   - 媒体页全屏 RGB565 帧直写 LCD（绕开 LVGL，时钟显示时切回 LVGL 合成）
 *   - 按键短按循环切换：启动视频/图片/视频/设置
 *   - 图片页左右滑动切图（固定模式）或按间隔自动轮播
 *   - 视频页左右滑动切视频（单个循环模式）或自动轮播
 *   - 接近检测：BLE 扫到对方 UID → 弹窗 + 震动 + alert.mp3；
 *     距离越近（RSSI 越大）提示频率 3s → 0.5s；关闭后该 UID 静默 1 小时
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "lvgl_ui_settings.h"
#include "app_media.h"
#include "app_settings.h"
#include "app_config.h"
#include "audio_mp3.h"
#include "proximity_ble.h"

#define APP_MEDIA_TAG "app_media"

#define FRAME_MS (1000 / 30)              /* 目标 30fps */
#define PROXIMITY_PEER_MAX 12
#define PROXIMITY_PEER_TIMEOUT_MS 5000
#define PROXIMITY_SUPPRESSION_MAX 12
#define PROXIMITY_SUPPRESSION_US (3600ULL * 1000000ULL)
#define PROXIMITY_RESULTS_MAX (PROXIMITY_PEER_MAX * 12)

/* ---------- 全局上下文 ---------- */
static app_media_ctx_t *s_ctx = NULL;

static lv_obj_t *s_page_video = NULL;     /* 启动/图片/视频共用媒体页 screen */
static lv_obj_t *s_video_img = NULL;
static lv_image_dsc_t s_video_dsc = {0};
static lv_obj_t *s_media_clock = NULL;

static volatile app_page_t s_app_page = APP_PAGE_START;
static volatile bool s_media_open_request = false;
static volatile int8_t s_media_step_request = 0;
static uint8_t s_picture_index = 0;
static uint8_t s_video_index = 0;
static uint32_t s_next_image_change_ms = 0;

/* ---------- 接近检测状态 ---------- */
typedef struct {
    uint32_t device_id;
    int16_t rssi;
    uint32_t last_seen_ms;
    char uid[11];
    bool alerted;
    bool counted;
} proximity_peer_t;

typedef struct {
    char uid[11];
    uint64_t until_us;
} proximity_suppression_t;

static portMUX_TYPE s_proximity_mux = portMUX_INITIALIZER_UNLOCKED;
static proximity_peer_t s_peers[PROXIMITY_PEER_MAX];
static proximity_suppression_t s_suppressions[PROXIMITY_SUPPRESSION_MAX];
static uint32_t s_next_alert_ms = 0;
/* 附近设备提醒震动状态机：固定震 1 秒、停 1 秒。 */
static bool s_vib_on = false;
static uint32_t s_vib_until_ms = 0;   /* 当前"震"状态持续到（on 时） */
static uint32_t s_vib_next_ms = 0;    /* 下次开始震的时刻（off 时） */
static bool s_peer_alert_active = false;
static uint32_t s_alert_peer_id = 0;
static int16_t s_alert_rssi = -90;
static char s_alert_uid[11] = "";
static bool s_alert_close_requested = false;
static char s_closed_alert_uid[11] = "";
/* 弹窗挂起（LVGL 锁忙时下轮重试） */
static bool s_show_alert_pending = false;
static char s_published_nearby_results[PROXIMITY_RESULTS_MAX] = "";

/* 低电量提醒：关闭后需先回升到 3.5V 以上，下一次跌落才再次提醒 */
static volatile bool s_low_battery_alert_active = false;
static volatile bool s_low_battery_alert_acknowledged = false;

static void low_battery_alert_closed(void *user_data)
{
    (void)user_data;
    s_low_battery_alert_active = false;
    s_low_battery_alert_acknowledged = true;
}

static int find_peer_locked(uint32_t device_id)
{
    for (int i = 0; i < PROXIMITY_PEER_MAX; i++) {
        if (s_peers[i].device_id == device_id) return i;
    }
    return -1;
}

static int select_peer_slot_locked(uint32_t now_ms)
{
    int oldest = 0;
    for (int i = 0; i < PROXIMITY_PEER_MAX; i++) {
        if (s_peers[i].device_id == 0 ||
            now_ms - s_peers[i].last_seen_ms > PROXIMITY_PEER_TIMEOUT_MS) {
            return i;
        }
        if (s_peers[i].last_seen_ms < s_peers[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    return oldest;
}

static bool uid_is_suppressed_locked(const char *uid, uint64_t now_us)
{
    for (int i = 0; i < PROXIMITY_SUPPRESSION_MAX; i++) {
        if (s_suppressions[i].until_us > now_us &&
            strcmp(s_suppressions[i].uid, uid) == 0) {
            return true;
        }
    }
    return false;
}

static void suppress_uid_locked(const char *uid, uint64_t now_us)
{
    int slot = 0;
    for (int i = 0; i < PROXIMITY_SUPPRESSION_MAX; i++) {
        if (strcmp(s_suppressions[i].uid, uid) == 0) {
            slot = i;
            break;
        }
        if (s_suppressions[i].until_us <= now_us) {
            slot = i;
            break;
        }
        if (s_suppressions[i].until_us < s_suppressions[slot].until_us) {
            slot = i;
        }
    }
    lv_strlcpy(s_suppressions[slot].uid, uid, sizeof(s_suppressions[slot].uid));
    s_suppressions[slot].until_us = now_us + PROXIMITY_SUPPRESSION_US;
}

/* ---------- 接近检测回调（BLE 扫描线程 → 主循环处理） ---------- */
void app_media_proximity_report(uint32_t peer_id, const char *uid, int8_t rssi,
                                void *user_data)
{
    (void)user_data;
    if (peer_id == 0 || uid == NULL || uid[0] == '\0') return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_proximity_mux);
    int slot = find_peer_locked(peer_id);
    if (slot < 0) {
        slot = select_peer_slot_locked(now_ms);
        memset(&s_peers[slot], 0, sizeof(s_peers[slot]));
        s_peers[slot].device_id = peer_id;
    } else if (strcmp(s_peers[slot].uid, uid) != 0) {
        s_peers[slot].alerted = false;
        s_peers[slot].counted = false;
    }
    lv_strlcpy(s_peers[slot].uid, uid, sizeof(s_peers[slot].uid));
    s_peers[slot].rssi = rssi;
    s_peers[slot].last_seen_ms = now_ms;
    if (peer_id == s_alert_peer_id) {
        s_alert_rssi = rssi;
    }
    portEXIT_CRITICAL(&s_proximity_mux);
}

void app_media_forget_proximity_uid(const char *uid)
{
    if (uid == NULL || uid[0] == '\0') return;

    portENTER_CRITICAL(&s_proximity_mux);

    /*
     * 删除记录不应继续受“已提醒”或“关闭后静默 1 小时”影响。
     * 保留 peer 本身，让下一次 BLE 广播到达时能直接重新触发提醒；
     * counted 清零后也会重新计入相遇记录。
     */
    for (int i = 0; i < PROXIMITY_PEER_MAX; i++) {
        if (s_peers[i].device_id != 0 &&
            strcmp(s_peers[i].uid, uid) == 0) {
            s_peers[i].alerted = false;
            s_peers[i].counted = false;
        }
    }

    for (int i = 0; i < PROXIMITY_SUPPRESSION_MAX; i++) {
        if (strcmp(s_suppressions[i].uid, uid) == 0) {
            memset(&s_suppressions[i], 0, sizeof(s_suppressions[i]));
        }
    }

    if (strcmp(s_closed_alert_uid, uid) == 0) {
        s_closed_alert_uid[0] = '\0';
        s_alert_close_requested = false;
    }

    portEXIT_CRITICAL(&s_proximity_mux);
    ESP_LOGI(APP_MEDIA_TAG, "proximity UID re-armed after record deletion: %s", uid);
}

static void nearby_alert_closed(void *user_data)
{
    (void)user_data;
    portENTER_CRITICAL(&s_proximity_mux);
    lv_strlcpy(s_closed_alert_uid, s_alert_uid, sizeof(s_closed_alert_uid));
    s_alert_close_requested = true;
    portEXIT_CRITICAL(&s_proximity_mux);
}

/* RSSI -90~-40 → 提示间隔 2000ms~500ms */
static uint32_t proximity_interval_ms(int16_t rssi)
{
    if (rssi < -90) rssi = -90;
    if (rssi > -40) rssi = -40;
    uint32_t ms = (uint32_t)(2000 - (int32_t)(rssi + 90) * 1500 / 50);
    return ms < 500 ? 500 : ms;   /* 下限 500ms */
}

void app_media_process_proximity(void)
{
    app_settings_process_c6_update();
    if (app_settings_c6_update_active()) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* 兜底：HELIX 解码器偶发死循环（帧头解析 bug），卡死会占死 CPU0。
     * 检测到"播放中但 5s 无新帧"→ 自动重启恢复，避免系统永久锁死。 */
    static uint32_t last_stuck_check_ms = 0;
    if (now - last_stuck_check_ms >= 1000) {
        last_stuck_check_ms = now;
        bool playing = false;
        uint32_t last_frame = 0;
        audio_get_status(&playing, &last_frame);
        if (playing && last_frame > 0 && (int32_t)(now - last_frame) > 5000) {
            ESP_LOGE(APP_MEDIA_TAG, "audio decoder stuck (no frame for %lums), restarting",
                     (unsigned long)(now - last_frame));
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
    uint64_t now_us = esp_timer_get_time();
    uint16_t scan_seconds = app_settings_scan_seconds();
    bool nearby_enabled = app_settings_nearby_enabled();
    bool has_nearby_peer = false;
    bool close_alert = false;
    bool new_alert = false;
    char nearby_results[PROXIMITY_RESULTS_MAX] = "";
    char new_encounter_uids[PROXIMITY_PEER_MAX][11];
    uint8_t new_encounter_count = 0;

    portENTER_CRITICAL(&s_proximity_mux);
    if (s_alert_close_requested) {
        if (s_closed_alert_uid[0] != '\0') {
            suppress_uid_locked(s_closed_alert_uid, now_us);
        }
        s_alert_close_requested = false;
        s_closed_alert_uid[0] = '\0';
        s_peer_alert_active = false;
        s_show_alert_pending = false;
        s_alert_peer_id = 0;
        close_alert = true;
    }

    for (int i = 0; i < PROXIMITY_PEER_MAX; i++) {
        proximity_peer_t *peer = &s_peers[i];
        if (peer->device_id == 0) continue;
        if (now - peer->last_seen_ms > PROXIMITY_PEER_TIMEOUT_MS) {
            if (peer->device_id == s_alert_peer_id) {
                s_peer_alert_active = false;
                s_show_alert_pending = false;
                s_alert_peer_id = 0;
                close_alert = true;
            }
            memset(peer, 0, sizeof(*peer));
            continue;
        }

        has_nearby_peer = true;
        if (!peer->counted && new_encounter_count < PROXIMITY_PEER_MAX) {
            peer->counted = true;
            lv_strlcpy(new_encounter_uids[new_encounter_count], peer->uid,
                       sizeof(new_encounter_uids[new_encounter_count]));
            new_encounter_count++;
        }
        bool uid_already_listed = false;
        for (int j = 0; j < i; j++) {
            if (s_peers[j].device_id != 0 &&
                strcmp(s_peers[j].uid, peer->uid) == 0) {
                uid_already_listed = true;
                break;
            }
        }
        if (uid_already_listed) {
            continue;
        }
        size_t used = strlen(nearby_results);
        size_t uid_len = strlen(peer->uid);
        if (used + uid_len + (used ? 1 : 0) < sizeof(nearby_results)) {
            if (used) nearby_results[used++] = '\n';
            memcpy(nearby_results + used, peer->uid, uid_len + 1);
        }
    }

    if (nearby_enabled && !s_peer_alert_active && !s_show_alert_pending) {
        for (int i = 0; i < PROXIMITY_PEER_MAX; i++) {
            proximity_peer_t *peer = &s_peers[i];
            if (peer->device_id == 0 || peer->alerted ||
                uid_is_suppressed_locked(peer->uid, now_us)) {
                continue;
            }
            peer->alerted = true;
            s_peer_alert_active = true;
            s_alert_peer_id = peer->device_id;
            s_alert_rssi = peer->rssi;
            lv_strlcpy(s_alert_uid, peer->uid, sizeof(s_alert_uid));
            s_next_alert_ms = now;
            s_show_alert_pending = true;
            new_alert = true;
            break;
        }
    }

    if (!nearby_enabled && s_peer_alert_active) {
        s_peer_alert_active = false;
        s_show_alert_pending = false;
        s_alert_peer_id = 0;
        close_alert = true;
    }
    portEXIT_CRITICAL(&s_proximity_mux);

    if (new_encounter_count > 0) {
        uint16_t old_count = app_settings_encounter_count();
        uint16_t encounter_count = old_count;
        for (uint8_t i = 0; i < new_encounter_count; i++) {
            encounter_count = app_settings_record_encounter(new_encounter_uids[i]);
        }
        if (encounter_count != old_count && lvgl_port_lock(50)) {
            lvgl_ui_settings_set_encounter_count(encounter_count);
            lvgl_ui_settings_set_encounter_records(app_settings_encounter_records());
            lvgl_port_unlock();
        }
    }

    /* 扫描开关：始终扫描，或按周期（每 5/10 秒扫 1.5 秒）。 */
    bool should_scan = nearby_enabled &&
                       (has_nearby_peer || scan_seconds == 0 ||
                        (now % (scan_seconds * 1000U)) < 1500U);
    static bool ble_scan_active = true;
    if (should_scan != ble_scan_active) {
        ble_scan_active = should_scan;
        proximity_ble_set_scanning(should_scan);
    }

    if (strcmp(nearby_results, s_published_nearby_results) != 0 && lvgl_port_lock(0)) {
        lvgl_ui_settings_set_nearby_results(nearby_results);
        lv_strlcpy(s_published_nearby_results, nearby_results,
                   sizeof(s_published_nearby_results));
        lvgl_port_unlock();
    }

    if (close_alert) {
        audio_stop();
        if (s_vib_on) {
            s_vib_on = false;
            app_settings_vibration_stop();
        }
        if (!s_show_alert_pending && lvgl_port_lock(0)) {
            lvgl_ui_settings_close_nearby_alert();
            lvgl_port_unlock();
        }
    }

    /* 挂起的弹窗：拿不到 LVGL 锁则下轮重试 */
    if (s_show_alert_pending && lvgl_port_lock(50)) {
        lvgl_ui_settings_show_nearby_alert(s_alert_uid, s_alert_rssi,
                                      nearby_alert_closed, NULL);
        lvgl_port_unlock();
        s_show_alert_pending = false;
    }

    if (new_alert) {
        /* 新设备进入提醒状态时立即开始固定节奏的第一段震动。 */
        s_vib_next_ms = now;
        ESP_LOGI(APP_MEDIA_TAG, "nearby peer uid=%s rssi=%d", s_alert_uid, s_alert_rssi);
    }

    /* 弹窗期间实时刷新信号强度 */
    if (s_peer_alert_active && lvgl_port_lock(0)) {
        lvgl_ui_settings_update_nearby_rssi(s_alert_rssi);
        lvgl_port_unlock();
    }

    /* 震动节奏独立于提示音 RSSI 节奏，确保严格震 1 秒、停 1 秒。 */
    if (!s_peer_alert_active) return;
    if (!app_settings_vibration_enabled()) {
        if (s_vib_on) {
            s_vib_on = false;
            app_settings_vibration_stop();
        }
    } else if (!s_vib_on) {
        /* 停 → 到点开始震 1 秒；附近设备提醒使用最大转速。 */
        if ((int32_t)(now - s_vib_next_ms) >= 0) {
            app_settings_vibration_start_max();
            s_vib_on = true;
            s_vib_until_ms = now + 1000;
        }
    } else if ((int32_t)(now - s_vib_until_ms) >= 0) {
        /* 震 1 秒结束 → 停 1 秒，再开始下一轮。 */
        app_settings_vibration_stop();
        s_vib_on = false;
        s_vib_next_ms = now + 1000;
    }

    /* 提示音节奏仍随 RSSI 变化，但不影响上面的固定震动节奏。 */
    if ((int32_t)(now - s_next_alert_ms) < 0) return;
    uint32_t interval = proximity_interval_ms(s_alert_rssi);
    s_next_alert_ms = now + interval;

    if (app_settings_speaker_enabled()) {
        audio_play(SD_ALERT_MP3, false);
        ESP_LOGI(APP_MEDIA_TAG, "alert sound (interval %lums, rssi %d)",
                 (unsigned long)interval, s_alert_rssi);
    }
}

/* ==================== 媒体页 UI ==================== */

static void media_clock_timer_cb(lv_timer_t *timer);

static void media_gesture_event(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    if (s_app_page != APP_PAGE_PICTURE && s_app_page != APP_PAGE_VIDEO) return;

    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_BOTTOM) {
        /* 下滑显示时钟 */
        lv_obj_remove_flag(s_media_clock, LV_OBJ_FLAG_HIDDEN);
        media_clock_timer_cb(NULL);
    } else if (direction == LV_DIR_TOP) {
        /* 上滑隐藏时钟 */
        lv_obj_add_flag(s_media_clock, LV_OBJ_FLAG_HIDDEN);
    } else if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
        bool manual_picture = (s_app_page == APP_PAGE_PICTURE &&
                               !app_settings_image_auto());
        bool manual_video = (s_app_page == APP_PAGE_VIDEO &&
                             app_settings_video_mode() == 0);
        if (manual_picture || manual_video) {
            s_media_step_request = (direction == LV_DIR_LEFT) ? 1 : -1;
        }
    }
}

static void media_clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_media_clock == NULL ||
        lv_obj_has_flag(s_media_clock, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    time_t tnow = time(NULL);
    struct tm value;
    localtime_r(&tnow, &value);
    char text[16];
    strftime(text, sizeof(text), app_settings_clock_24h() ? "%H:%M" : "%I:%M %p", &value);
    lv_label_set_text(s_media_clock, text);
}

static void create_media_page(app_media_ctx_t *ctx)
{
    s_page_video = lv_obj_create(NULL);          /* 独立 screen */
    lv_obj_set_size(s_page_video, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_page_video, lv_color_black(), 0);
    lv_obj_clear_flag(s_page_video, LV_OBJ_FLAG_SCROLLABLE);

    s_video_img = lv_image_create(s_page_video);
    s_video_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_video_dsc.header.w = LCD_H_RES;
    s_video_dsc.header.h = LCD_V_RES;
    s_video_dsc.data_size = (uint32_t)LCD_H_RES * LCD_V_RES * 2;
    uint8_t *video_data = (uint8_t *)mjpeg_player_frame_buffer(&ctx->mjpeg);
    if (video_data == NULL) {
        /* 播放器未就绪：用 PSRAM 零缓冲占位（黑屏） */
        video_data = heap_caps_malloc(s_video_dsc.data_size, MALLOC_CAP_SPIRAM);
        if (video_data) {
            memset(video_data, 0, s_video_dsc.data_size);
        }
    }
    if (video_data != NULL) {
        s_video_dsc.data = video_data;
        lv_image_set_src(s_video_img, &s_video_dsc);
        lv_obj_set_pos(s_video_img, 0, 0);
        lv_obj_clear_flag(s_video_img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        /* 无任何帧缓冲：媒体页保持纯黑（不创建 image，避免空指针渲染） */
        lv_obj_del(s_video_img);
        s_video_img = NULL;
    }

    if (!sdcard_ready()) {
        lv_obj_t *sd_hint = lv_label_create(s_page_video);
        lv_label_set_text(sd_hint, "Please insert SD card");
        lv_obj_set_style_text_color(sd_hint, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_font(sd_hint, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(sd_hint);
    }

    lv_obj_add_flag(s_page_video, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_page_video, media_gesture_event, LV_EVENT_GESTURE, NULL);

    /* 顶部时钟（默认隐藏；下滑显示 / 上滑隐藏） */
    s_media_clock = lv_label_create(s_page_video);
    lv_label_set_text(s_media_clock, "00:00");
    lv_obj_set_style_text_color(s_media_clock, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_media_clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_bg_color(s_media_clock, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_media_clock, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_media_clock, 20, 0);
    lv_obj_set_style_pad_ver(s_media_clock, 10, 0);
    lv_obj_set_style_radius(s_media_clock, 6, 0);
    lv_obj_align(s_media_clock, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(s_media_clock, LV_OBJ_FLAG_HIDDEN);

    /* 设置页（LVGL_UI 用自己的 screen root） */
    lvgl_ui_settings_create();

    bool delete_boot_screen = (ctx->board.boot_screen != NULL);
    lv_screen_load_anim(s_page_video, LV_SCREEN_LOAD_ANIM_NONE, 0, 0,
                        delete_boot_screen);
    ctx->board.boot_screen = NULL;
}

/* ==================== 按键切换模式 ==================== */

static void btn_timer_cb(lv_timer_t *t)
{
    (void)t;
    static bool was_pressed = false;
    static uint32_t pressed_at = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    bool pressed = gpio_get_level((gpio_num_t)BTN_PIN) == 0;
    if (pressed && !was_pressed) {
        pressed_at = now;
    } else if (!pressed && was_pressed) {
        uint32_t duration = now - pressed_at;
        if (duration >= 30 && duration <= 800) {
            /* 短按：循环切换模式 */
            s_app_page = (app_page_t)(((uint8_t)s_app_page + 1) % APP_PAGE_COUNT);
            if (s_app_page == APP_PAGE_SETTINGS) {
                lv_scr_load(lvgl_ui_settings_get_root());
            } else {
                lv_obj_add_flag(s_media_clock, LV_OBJ_FLAG_HIDDEN);
                lv_scr_load(s_page_video);
                s_media_open_request = true;
            }
            static const char *names[] = {"启动视频", "图片", "视频", "设置"};
            ESP_LOGI(APP_MEDIA_TAG, "switch to %s page", names[s_app_page]);
        }
    }
    was_pressed = pressed;
}

/* ==================== 电池显示（右上角） ==================== */

static void ina_timer_cb(lv_timer_t *t)
{
    (void)t;
    app_media_ctx_t *ctx = s_ctx;
    if (ctx == NULL) return;
    if (!ctx->ina.initialized) {
        lvgl_ui_settings_set_battery(0, 0, false);
        return;
    }
    float v = 0, i = 0, p = 0, sv = 0;
    if (ina226_read_all(&ctx->ina, &v, &i, &p, &sv)) {
        lvgl_ui_settings_set_battery(v, i * 1000.0f, true);
        if (v > 3.5f) {
            s_low_battery_alert_acknowledged = false;
        } else if (v <= 3.5f && !s_low_battery_alert_acknowledged &&
                   !s_low_battery_alert_active) {
            s_low_battery_alert_active = true;
            lvgl_ui_settings_show_battery_low_alert(low_battery_alert_closed, NULL);
        }
    } else {
        lvgl_ui_settings_set_battery(0, 0, false);
    }
}

static void update_system_info(void)
{
    uint32_t flash_total = 0;
    uint64_t flash_used = 0;
    if (esp_flash_get_size(NULL, &flash_total) == ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running != NULL) {
            esp_partition_pos_t position = {
                .offset = running->address,
                .size = running->size,
            };
            esp_image_metadata_t metadata;
            if (esp_image_get_metadata(&position, &metadata) == ESP_OK) {
                flash_used = metadata.image_len;
            }
        }
    }

    uint64_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint64_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint64_t psram_used = psram_total >= psram_free ? psram_total - psram_free : 0;

    uint64_t tf_total = 0;
    uint64_t tf_free = 0;
    uint64_t tf_used = 0;
    bool tf_ready = esp_vfs_fat_info(SD_MOUNT_POINT, &tf_total, &tf_free) == ESP_OK;
    if (tf_ready) {
        tf_used = tf_total >= tf_free ? tf_total - tf_free : 0;
    }

    lvgl_ui_settings_set_system_info(flash_used, flash_total,
                                     psram_used, psram_total,
                                     tf_used, tf_total, tf_ready);
}

/* ==================== 媒体解码任务 ==================== */

static void media_task(void *arg)
{
    app_media_ctx_t *ctx = (app_media_ctx_t *)arg;
    bool media_paused_for_c6_update = false;
    while (1) {
        if (app_settings_c6_update_active()) {
            if (!media_paused_for_c6_update) {
                mjpeg_player_close_file(&ctx->mjpeg);
                media_paused_for_c6_update = true;
                ESP_LOGI(APP_MEDIA_TAG, "media SD access paused for C6 OTA");
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (media_paused_for_c6_update) {
            media_paused_for_c6_update = false;
            s_media_open_request = true;
            ESP_LOGI(APP_MEDIA_TAG, "media SD access resumed after C6 OTA");
        }
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        bool frame_changed = false;
        app_page_t page = s_app_page;

        if (s_media_open_request) {
            s_media_open_request = false;
            s_media_step_request = 0;
            if (page == APP_PAGE_START && ctx->start_videos.count) {
                mjpeg_player_set_loop(&ctx->mjpeg, true);
                mjpeg_player_open(&ctx->mjpeg, ctx->start_videos.path[0]);
            } else if (page == APP_PAGE_PICTURE && ctx->pictures.count) {
                if (mjpeg_player_decode_jpeg(&ctx->mjpeg, ctx->pictures.path[s_picture_index]) == ESP_OK) {
                    frame_changed = true;
                }
                s_next_image_change_ms = t0 + (uint32_t)app_settings_image_interval() * 1000;
            } else if (page == APP_PAGE_VIDEO && ctx->videos.count) {
                mjpeg_player_set_loop(&ctx->mjpeg, app_settings_video_mode() == 0);
                mjpeg_player_open(&ctx->mjpeg, ctx->videos.path[s_video_index]);
            }
        }

        int8_t step = s_media_step_request;
        if (step != 0) s_media_step_request = 0;

        if (page == APP_PAGE_PICTURE && ctx->pictures.count) {
            if (app_settings_image_auto() &&
                (int32_t)(t0 - s_next_image_change_ms) >= 0) {
                step = 1;
            }
            if (step != 0) {
                int next = (int)s_picture_index + step;
                if (next < 0) next = ctx->pictures.count - 1;
                if (next >= ctx->pictures.count) next = 0;
                s_picture_index = (uint8_t)next;
                if (mjpeg_player_decode_jpeg(&ctx->mjpeg, ctx->pictures.path[s_picture_index]) == ESP_OK) {
                    frame_changed = true;
                }
                s_next_image_change_ms = t0 + (uint32_t)app_settings_image_interval() * 1000;
                ESP_LOGI(APP_MEDIA_TAG, "pic %u/%u %s", s_picture_index + 1,
                         ctx->pictures.count, ctx->pictures.path[s_picture_index]);
            }
        } else if (page == APP_PAGE_START) {
            frame_changed = ctx->start_videos.count &&
                            mjpeg_player_decode_next_frame(&ctx->mjpeg);
        } else if (page == APP_PAGE_VIDEO && ctx->videos.count) {
            if (step != 0 && app_settings_video_mode() == 0) {
                int next = (int)s_video_index + step;
                if (next < 0) next = ctx->videos.count - 1;
                if (next >= ctx->videos.count) next = 0;
                s_video_index = (uint8_t)next;
                mjpeg_player_set_loop(&ctx->mjpeg, true);
                mjpeg_player_open(&ctx->mjpeg, ctx->videos.path[s_video_index]);
                ESP_LOGI(APP_MEDIA_TAG, "video %u/%u %s", s_video_index + 1,
                         ctx->videos.count, ctx->videos.path[s_video_index]);
            }
            frame_changed = mjpeg_player_decode_next_frame(&ctx->mjpeg);
            if (!frame_changed && mjpeg_player_ended(&ctx->mjpeg) &&
                app_settings_video_mode() != 0) {
                /* 自动轮播：播完换下一个 */
                s_video_index = (uint8_t)((s_video_index + 1) % ctx->videos.count);
                mjpeg_player_set_loop(&ctx->mjpeg, false);
                mjpeg_player_open(&ctx->mjpeg, ctx->videos.path[s_video_index]);
                ESP_LOGI(APP_MEDIA_TAG, "video next %u/%u", s_video_index + 1,
                         ctx->videos.count);
            }
        }

        if (frame_changed && s_app_page == page && lvgl_port_lock(0)) {
            bool clock_visible = s_media_clock &&
                                 !lv_obj_has_flag(s_media_clock, LV_OBJ_FLAG_HIDDEN);
            bool overlay_visible = clock_visible ||
                                   s_peer_alert_active ||
                                   s_show_alert_pending ||
                                   s_low_battery_alert_active;
            if (overlay_visible) {
                /* 时钟或全局弹窗可见：让 LVGL 合成当前帧和顶层 UI */
                if (s_video_img != NULL) {
                    lv_obj_invalidate(s_video_img);
                }
            } else {
                /* 常规播放：帧直写 LCD，绕开 LVGL 全屏拷贝 */
                uint16_t *fb = mjpeg_player_frame_buffer(&ctx->mjpeg);
                if (fb != NULL) {
                    esp_lcd_panel_draw_bitmap(s_ctx->board.lcd, 0, 0,
                                              LCD_H_RES, LCD_V_RES, fb);
                }
            }
            lvgl_port_unlock();
        }

        uint32_t elapsed = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        int32_t wait = (int32_t)FRAME_MS - (int32_t)elapsed;
        if (wait < 1) wait = 1;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

/* ==================== 初始化 ==================== */

esp_err_t app_media_init(app_media_ctx_t *ctx)
{
    s_ctx = ctx;

    /* 模式按键 IO11：输入上拉（按下=低电平）。必须在读取前初始化，
     * 否则 gpio_get_level 返回不确定值，按键状态机无法工作。 */
    const gpio_config_t btn_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BTN_PIN,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn_cfg), "app_media", "button gpio config failed");

    /* 启动期首次全屏渲染 + SD 卡字体加载可能较长，超时给足 5s */
    if (!lvgl_port_lock(5000)) {
        ESP_LOGE(APP_MEDIA_TAG, "failed to lock LVGL for UI creation");
        return ESP_ERR_TIMEOUT;
    }
    create_media_page(ctx);
    update_system_info();
    lv_timer_create(btn_timer_cb, 30, NULL);
    lv_timer_create(media_clock_timer_cb, 1000, NULL);
    lv_timer_create(ina_timer_cb, 1000, NULL);
    lvgl_port_unlock();

    if (xTaskCreate(media_task, "media", 8192, ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(APP_MEDIA_TAG, "failed to create media task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

app_page_t app_media_current_page(void)
{
    return s_app_page;
}

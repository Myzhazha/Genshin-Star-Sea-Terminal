/*
 * app_media.h - 媒体页（启动/图片/视频）+ 模式切换 + 接近检测
 *
 * 页面流转（短按 IO11）：
 *   启动视频 → 图片 → 视频 → 设置 → 回到启动视频
 */
#pragma once

#include "esp_err.h"
#include "app_config.h"
#include "hw_board.h"
#include "mjpeg_player.h"
#include "sdcard.h"
#include "ina226.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hw_board_t board;                 /* LCD/LVGL 句柄 */
    mjpeg_player_t mjpeg;
    ina226_t ina;                     /* 电池电压/电流（右上角显示） */
    media_list_t start_videos;
    media_list_t pictures;
    media_list_t videos;
} app_media_ctx_t;

/**
 * @brief 创建媒体页与设置页 UI，注册按键/时钟/电池定时器，启动媒体解码任务。
 *        需在 app_settings_init() 之后调用（初始状态已注入 UI）。
 */
esp_err_t app_media_init(app_media_ctx_t *ctx);

/** @brief 当前页面（供 main 的周期性任务使用） */
app_page_t app_media_current_page(void);

/** @brief BLE 扫描到附近设备时的回调（传给 proximity_ble_begin） */
void app_media_proximity_report(uint32_t peer_id, const char *uid, int8_t rssi,
                                void *user_data);

/** @brief 删除相遇记录后，重新允许该 UID 触发发现提醒和计数。 */
void app_media_forget_proximity_uid(const char *uid);

/** @brief 周期性处理：接近检测扫描控制 / UID 弹窗 / 震动 / 提示音（每 10ms 调用） */
void app_media_process_proximity(void);

#ifdef __cplusplus
}
#endif

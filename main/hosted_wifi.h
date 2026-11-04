/*
 * hosted_wifi.h - 经 esp-hosted(C6/SDIO) 的 WiFi（标准 esp_wifi API）
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 扫描完成回调：ssid_list 为 '\n' 分隔的 SSID（可为 NULL 表示失败） */
typedef void (*hosted_wifi_scan_done_cb_t)(const char *ssid_list, void *user_data);

typedef enum {
    HOSTED_WIFI_STATUS_CONNECTING = 0,
    HOSTED_WIFI_STATUS_CONNECTED,
    HOSTED_WIFI_STATUS_DISCONNECTED,
} hosted_wifi_status_t;

typedef void (*hosted_wifi_status_cb_t)(hosted_wifi_status_t status, void *user_data);

/**
 * @brief 连接 C6 从机并初始化 esp_netif / esp_wifi（esp_wifi_remote 实现）。
 *        必须在任何 WiFi API 之前调用。
 */
esp_err_t hosted_wifi_init(void);

void hosted_wifi_set_scan_done_cb(hosted_wifi_scan_done_cb_t cb, void *user_data);
void hosted_wifi_set_status_cb(hosted_wifi_status_cb_t cb, void *user_data);

/** @brief 异步扫描附近 AP；结果经回调返回 */
esp_err_t hosted_wifi_scan_start(void);

/** @brief 连接指定 AP（STA 模式，自动启动/切换） */
esp_err_t hosted_wifi_connect(const char *ssid, const char *password);

/** @brief 开关 WiFi（关闭=断开+停止） */
void hosted_wifi_set_enabled(bool enabled);

/** @brief 开关 Wi-Fi 获取 IP 后的阿里云 SNTP 自动校时。 */
void hosted_wifi_set_time_sync_enabled(bool enabled);

bool hosted_wifi_is_enabled(void);

/** @brief Wi-Fi/ESP-Hosted transport has completed initialization. */
bool hosted_wifi_is_initialized(void);

/** @brief 停止远端 Wi-Fi/BLE 并释放 ESP-Hosted SDIO 传输。 */
void hosted_wifi_deinit_for_rom_flash(void);

#ifdef __cplusplus
}
#endif

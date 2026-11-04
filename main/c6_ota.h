/*
 * c6_ota.h - 通过 P4 SDIO 直接烧录空白 C6（ROM download 模式）
 *
 * 用法：把 C6 的应用固件 network_adapter.bin 放到 SD 卡 /c6fw/ 目录，
 * 将 /sdcard/c6fw/ 下的 bootloader.bin、partition-table.bin、
 * network_adapter.bin 写入 C6 的 0x0、0x8000、0x10000。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通过 SDIO ROM 下载器烧录 C6 三段固件。
 * @return ESP_OK 刷写完成；ESP_FAIL 表示固件文件、ROM 下载器或 SDIO 失败。
 */
esp_err_t c6_ota_from_sdcard(void);

/** @brief 异步启动 C6 SDIO OTA，避免阻塞 LVGL 线程。 */
esp_err_t c6_ota_start_from_sdcard(void);

typedef void (*c6_ota_event_cb_t)(esp_err_t result, void *user_data);

void c6_ota_set_event_cb(c6_ota_event_cb_t callback, void *user_data);
bool c6_ota_is_running(void);

#ifdef __cplusplus
}
#endif

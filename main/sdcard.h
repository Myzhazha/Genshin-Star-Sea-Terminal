/*
 * sdcard.h - SD 卡挂载与媒体文件扫描
 *
 * 注意：P4 上 SD 卡使用 SDMMC Slot 0，esp-hosted(SDIO) 使用 Slot 1。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_MEDIA_FILES 32
#define MAX_MEDIA_PATH  128

typedef struct {
    char path[MAX_MEDIA_FILES][MAX_MEDIA_PATH];
    uint8_t count;
} media_list_t;

/**
 * @brief 初始化 DET 检测引脚并挂载 SD 卡（FAT，4-bit）。
 * @return ESP_OK 挂载成功；否则失败（不阻塞系统，后续可用 sdcard_ready 查询）。
 */
esp_err_t sdcard_init(void);

/** @brief SD 卡是否已挂载 */
bool sdcard_ready(void);

/**
 * @brief 扫描三个媒体目录：
 *        start 目录的 .mjpeg、pic 目录的 .jpg/.jpeg、
 *        video 目录的 .mjpeg（按文件名排序）
 */
void sdcard_scan_all(media_list_t *start_videos, media_list_t *pictures,
                     media_list_t *videos);

#ifdef __cplusplus
}
#endif

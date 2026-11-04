/*
 * mjpeg_player.h - MJPEG 视频播放器（ESP32-P4 硬件 JPEG 解码）
 *
 * - 从 SD 卡读取标准 MJPEG 流（FFD8...FFD9 序列）或单张 JPEG
 * - 使用 P4 硬件 JPEG 引擎解码（jpeg_decoder_process，输出 RGB565）
 * - 帧缓冲 480x800x2 = 750KB，放 PSRAM（DMA 对齐）
 *
 * 使用：begin() 后循环 decode_next_frame()，每帧把 frame_buffer() 送屏；
 *       帧率由调用方控制（30fps ≈ 33ms/帧）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"
#include "driver/jpeg_decode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t w, h;
    uint8_t *in_buf;          /* 单帧压缩数据（jpeg_alloc_decoder_mem） */
    size_t in_cap;
    size_t frame_size;
    uint16_t *out_buf;        /* RGB565 输出帧缓冲（PSRAM） */
    size_t out_cap;
    uint8_t *read_buf;        /* SD 流缓冲（PSRAM，16KB） */
    size_t read_pos, read_len;
    FILE *file;
    long file_size;           /* 打开时缓存，供空文件/循环判断 */
    jpeg_decoder_handle_t dec;
    bool loop;
    bool ended;
} mjpeg_player_t;

/** @brief 分配解码器缓冲并打开第一个文件 */
esp_err_t mjpeg_player_begin(mjpeg_player_t *p, const char *path, uint16_t w, uint16_t h);

/** @brief 切换到另一个 MJPEG 文件（不重建解码器） */
esp_err_t mjpeg_player_open(mjpeg_player_t *p, const char *path);

/** @brief 只关闭当前媒体文件，保留解码器和缓冲，便于稍后恢复播放。 */
void mjpeg_player_close_file(mjpeg_player_t *p);

/** @brief 解码单张 JPEG 图片到共享帧缓冲 */
esp_err_t mjpeg_player_decode_jpeg(mjpeg_player_t *p, const char *path);

/** @brief 解码下一帧；成功返回 true，输出在 frame_buffer() */
bool mjpeg_player_decode_next_frame(mjpeg_player_t *p);

void mjpeg_player_set_loop(mjpeg_player_t *p, bool enabled);
bool mjpeg_player_ended(const mjpeg_player_t *p);

/** @brief RGB565 帧数据（w*h*2 字节） */
uint16_t *mjpeg_player_frame_buffer(mjpeg_player_t *p);

void mjpeg_player_end(mjpeg_player_t *p);

#ifdef __cplusplus
}
#endif

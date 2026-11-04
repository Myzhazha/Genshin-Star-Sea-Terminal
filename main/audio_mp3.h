/*
 * audio_mp3.h - NS4168 I2S 功放 + Helix MP3 软件解码
 *
 * 引脚：CTRL=4(使能) BCLK=2 LRCLK=3 DIN=1，I2S_NUM_1 标准模式
 * 播放命令经队列发给 audio 任务，解码/写 I2S 全部在 audio 任务内完成。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S 输出与功放使能脚，启动 audio 任务。
 *        音量默认取 CONFIG 默认值（AMP_DEFAULT_VOLUME）。
 */
esp_err_t audio_init(void);

/** @brief 功放开关：en=false 时拉低 CTRL 并停止当前播放 */
void audio_set_enabled(bool enabled);

/** @brief 音量百分比 0~100（软件缩放 PCM 样本） */
void audio_set_volume(uint8_t percent);
void audio_get_status(bool *playing, uint32_t *last_frame_ms);

/** @brief 异步播放 MP3（非阻塞），loop=true 循环播放 */
esp_err_t audio_play(const char *path, bool loop);

/** @brief 停止当前播放 */
void audio_stop(void);

/** @brief 是否正在播放 */
bool audio_is_playing(void);

#ifdef __cplusplus
}
#endif

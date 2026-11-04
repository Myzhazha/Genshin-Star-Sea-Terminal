/*
 * audio_mp3.c - NS4168 I2S 功放 + Helix MP3 解码播放
 *
 * 工作方式：
 *   - audio 任务独占 helix 解码器与 I2S 写操作（避免多线程竞争）
 *   - 播放命令（play/stop）通过 FreeRTOS 队列传递
 *   - 音量 = PCM 样本定点缩放（gain Q15）
 *   - MP3 采样率在解码第一帧后动态重配 I2S 时钟
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "mp3dec.h"

#include "audio_mp3.h"
#include "app_config.h"

#define AUDIO_TASK_STACK  32768   /* HELIX 解码单帧栈需求大，8K 会栈溢出死循环 */
#define AUDIO_TASK_PRIO   4
#define IN_CHUNK_SIZE     (16 * 1024)     /* 输入缓冲（内部 RAM） */
#define OUT_SAMPLE_MAX    2304            /* 单帧交错样本上限（stereo） */

typedef struct {
    uint8_t cmd;             /* 0=play 1=stop */
    bool loop;
    char path[64];
} audio_cmd_t;

typedef enum {
    AUDIO_CMD_PLAY = 0,
    AUDIO_CMD_STOP,
} audio_cmd_id_t;

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx = NULL;
static QueueHandle_t s_queue = NULL;
static volatile bool s_enabled = true;
static volatile bool s_playing = false;
static volatile uint32_t s_last_frame_ms = 0;   /* 最近一次成功解码帧的时间（毫秒） */
static volatile uint8_t s_volume = AMP_DEFAULT_VOLUME;
static volatile int32_t s_gain_q15 = 0;    /* 音量 0~32768 */

static void update_gain(void)
{
    uint8_t vol = s_volume;
    if (vol > 100) vol = 100;
    s_gain_q15 = ((int32_t)vol * 32768) / 100;
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AMP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   /* 播放停止后输出静音，避免爆音 */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AMP_BCLK,
            .ws = AMP_LRCLK,
            .dout = AMP_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable failed");
    return ESP_OK;
}

/* 播放单个文件直到结束或收到下一条命令。 */
static bool play_file(const char *path, bool loop, int32_t gain_q15,
                      audio_cmd_t *next_cmd)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open %s failed", path);
        return false;
    }
    ESP_LOGI(TAG, "play_file: open OK, decoding start");

    HMP3Decoder dec = MP3InitDecoder();
    if (dec == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        fclose(f);
        return false;
    }
    ESP_LOGI(TAG, "play_file: MP3InitDecoder OK");

    static uint8_t inbuf[IN_CHUNK_SIZE];        /* 解码输入（帧数据流） */
    static int16_t outbuf[OUT_SAMPLE_MAX];      /* 解码输出（交错样本） */
    static int16_t mono2stereo[OUT_SAMPLE_MAX * 2];

    /* 首次调用 MP3Decode 前必须先填数据：此 HELIX 移植版不解 bytesLeft=0，
     * 空缓冲会被当作帧头解析导致死循环。 */
    int bytesLeft = (int)fread(inbuf, 1, IN_CHUNK_SIZE, f);
    if (bytesLeft <= 0) {
        ESP_LOGW(TAG, "read %s failed", path);
        fclose(f);
        MP3FreeDecoder(dec);
        return false;
    }
    uint8_t *inptr = inbuf;
    int current_rate = 0;
    bool first_frame = true;
    bool stopped = false;
    bool command_received = false;
    int frame_count = 0;   /* 诊断 */
    int underflow_count = 0;
    uint32_t loop_iterations = 0;
    uint32_t bad_frame_count = 0;

    while (!stopped) {
        /* Helix 在损坏/不兼容的帧上可能长时间重同步；定期让出 CPU，
         * 防止音频任务独占 CPU0 导致 IDLE0 看门狗触发。 */
        if ((++loop_iterations & 0x7FU) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        /* 停止命令检查 */
        audio_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
            *next_cmd = cmd;
            command_received = true;
            stopped = true;
        }
        if (stopped) break;

        int ret = MP3Decode(dec, &inptr, &bytesLeft, outbuf, 0);
        if (ret == ERR_MP3_NONE) {
            bad_frame_count = 0;
            frame_count++;
            s_last_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (frame_count <= 5 || frame_count % 20 == 0) {
                ESP_LOGI(TAG, "mp3 frame %d decoded (bytesLeft=%d)", frame_count, bytesLeft);
            }
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (fi.outputSamps <= 0 || fi.outputSamps > OUT_SAMPLE_MAX) {
                continue;
            }
            if (first_frame || fi.samprate != current_rate) {
                current_rate = fi.samprate;
                first_frame = false;
                /* 动态重配 I2S 时钟（播放中只发生一次或切文件时） */
                i2s_channel_disable(s_tx);
                i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(current_rate);
                i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
                i2s_channel_enable(s_tx);
                ESP_LOGI(TAG, "I2S %dHz %dch", fi.samprate, fi.nChans);
            }

            size_t bytes;
            if (fi.nChans == 1) {
                /* mono → 复制到左右声道 */
                int32_t n = fi.outputSamps;
                for (int32_t i = 0; i < n; i++) {
                    int32_t s = (int32_t)outbuf[i] * gain_q15 >> 15;
                    mono2stereo[i * 2] = (int16_t)s;
                    mono2stereo[i * 2 + 1] = (int16_t)s;
                }
                i2s_channel_write(s_tx, mono2stereo, (size_t)n * 2 * sizeof(int16_t), &bytes,
                                  pdMS_TO_TICKS(100));
            } else {
                int32_t n = fi.outputSamps;
                for (int32_t i = 0; i < n; i++) {
                    outbuf[i] = (int16_t)((int32_t)outbuf[i] * gain_q15 >> 15);
                }
                i2s_channel_write(s_tx, outbuf, (size_t)n * sizeof(int16_t), &bytes,
                                  pdMS_TO_TICKS(100));
            }
        } else if (ret == ERR_MP3_INDATA_UNDERFLOW || ret == ERR_MP3_MAINDATA_UNDERFLOW) {
            /* 需要更多数据：把未消费的搬到开头再读 */
            underflow_count++;
            if (underflow_count <= 3) {
                ESP_LOGI(TAG, "mp3 underflow #%d (frame=%d, bytesLeft=%d)",
                         underflow_count, frame_count, bytesLeft);
            }
            int remaining = bytesLeft;
            if (remaining > 0) {
                memmove(inbuf, inptr, (size_t)remaining);
            }
            inptr = inbuf;
            size_t n = fread(inbuf + remaining, 1, IN_CHUNK_SIZE - (size_t)remaining, f);
            bytesLeft = remaining + (int)n;
            if (n == 0) {
                /* EOF */
                if (loop && bytesLeft == 0) {
                    fseek(f, 0, SEEK_SET);
                    bytesLeft = 0;
                    inptr = inbuf;
                    continue;
                }
                if (bytesLeft == 0) {
                    stopped = true;
                    break;
                }
            }
        } else {
            /* 坏帧：丢弃 1 字节重同步（helix 常见处理） */
            if (++bad_frame_count > 4096) {
                ESP_LOGW(TAG, "too many invalid MP3 frames in %s, aborting", path);
                stopped = true;
                break;
            }
            if (bytesLeft > 0) {
                inptr++;
                bytesLeft--;
            }
        }
    }

    MP3FreeDecoder(dec);
    fclose(f);
    return command_received;
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (cmd.cmd == AUDIO_CMD_PLAY) {
                if (!s_enabled || s_gain_q15 == 0) {
                    continue;
                }
                s_playing = true;
                ESP_LOGI(TAG, "playing %s loop=%d", cmd.path, cmd.loop);
                for (;;) {
                    audio_cmd_t next = { .cmd = AUDIO_CMD_STOP };
                    bool interrupted = play_file(cmd.path, cmd.loop, s_gain_q15, &next);
                    if (interrupted) {
                        if (next.cmd == AUDIO_CMD_PLAY) {
                            cmd = next;   /* 换文件继续 */
                            continue;
                        }
                        break;            /* 收到 STOP */
                    }
                    break;                /* 自然结束 */
                }
                s_playing = false;
            } else if (cmd.cmd == AUDIO_CMD_STOP) {
                s_playing = false;
            }
        }
    }
}

esp_err_t audio_init(void)
{
    /* 功放使能脚 */
    gpio_config_t ctrl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << AMP_CTRL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ctrl_cfg), TAG, "amp ctrl gpio failed");
    gpio_set_level(AMP_CTRL, 1);   /* 默认使能 */

    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "i2s init failed");

    /* 队列只保存最新命令，STOP 可以覆盖尚未处理的 PLAY。 */
    s_queue = xQueueCreate(1, sizeof(audio_cmd_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    update_gain();
    if (xTaskCreate(audio_task, "audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
        gpio_set_level(AMP_CTRL, 0);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio ready (I2S%d, CTRL=%d)", AMP_I2S_NUM, AMP_CTRL);
    return ESP_OK;
}

void audio_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        audio_stop();
    }
    gpio_set_level(AMP_CTRL, enabled ? 1 : 0);
    ESP_LOGI(TAG, "amp %s", enabled ? "enabled" : "disabled");
}

/** 解码状态查询：供外部检测解码器卡死（返回最近成功帧时间） */
void audio_get_status(bool *playing, uint32_t *last_frame_ms)
{
    if (playing) *playing = s_playing;
    if (last_frame_ms) *last_frame_ms = s_last_frame_ms;
}

void audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;
    update_gain();
}

esp_err_t audio_play(const char *path, bool loop)
{
    if (s_queue == NULL || path == NULL) {
        return ESP_FAIL;
    }
    audio_cmd_t cmd = {
        .cmd = AUDIO_CMD_PLAY,
        .loop = loop,
    };
    strlcpy(cmd.path, path, sizeof(cmd.path));
    xQueueOverwrite(s_queue, &cmd);
    return ESP_OK;
}

void audio_stop(void)
{
    if (s_queue == NULL) return;
    audio_cmd_t cmd = { .cmd = AUDIO_CMD_STOP };
    xQueueOverwrite(s_queue, &cmd);
}

bool audio_is_playing(void)
{
    return s_playing;
}

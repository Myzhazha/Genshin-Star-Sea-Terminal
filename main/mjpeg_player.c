/*
 * mjpeg_player.c - MJPEG 播放器（ESP32-P4 硬件 JPEG 解码）
 *
 * 帧解析（单遍流式，16KB 持久块缓冲）：
 *   - 扫描 FFD8(SOI) 开始拷贝；遇 FFD9(EOI) 结束
 *   - 支持 0xFF 0x00 字节填充与 0xFF xx marker 原样保留
 *   - EOF 回到文件头循环播放（loop 模式）
 * 持久保存块内未消费数据，避免 EOI 后丢掉同一块里的下一帧开头。
 *
 * 移植自 v1.3.2 mjpeg_player.cpp，文件接口从 Arduino File 换成 FILE*。
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mjpeg_player.h"
#include "app_config.h"

#define MJPEG_MAX_FRAME_BYTES (384 * 1024)   /* 480x800 单帧压缩数据上限 */
#define READ_BUFFER_SIZE      (16 * 1024)

static const char *TAG = "mjpeg";

esp_err_t mjpeg_player_begin(mjpeg_player_t *p, const char *path, uint16_t w, uint16_t h)
{
    memset(p, 0, sizeof(*p));
    p->w = w;
    p->h = h;
    p->loop = true;

    /* 解码器要求 DMA 对齐缓冲：必须用 jpeg_alloc_decoder_mem 分配 */
    size_t allocated = 0;
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    p->in_cap = MJPEG_MAX_FRAME_BYTES;
    p->in_buf = (uint8_t *)jpeg_alloc_decoder_mem(p->in_cap, &mem_cfg, &allocated);
    if (allocated > 0) {
        p->in_cap = allocated;
    }

    mem_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    p->out_cap = (size_t)w * h * 2;
    p->out_buf = (uint16_t *)jpeg_alloc_decoder_mem(p->out_cap, &mem_cfg, &allocated);
    if (allocated > 0) {
        p->out_cap = allocated;
    }

    if (p->in_buf == NULL || p->out_buf == NULL) {
        ESP_LOGE(TAG, "frame buffer alloc failed (need PSRAM)");
        mjpeg_player_end(p);
        return ESP_ERR_NO_MEM;
    }

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 100,
    };
    if (jpeg_new_decoder_engine(&eng_cfg, &p->dec) != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decoder engine create failed");
        mjpeg_player_end(p);
        return ESP_FAIL;
    }

    p->read_buf = (uint8_t *)heap_caps_malloc(READ_BUFFER_SIZE,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p->read_buf == NULL) {
        ESP_LOGE(TAG, "stream buffer alloc failed");
        mjpeg_player_end(p);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "player ready: %ux%u, in=%uKB out=%uKB",
             w, h, (unsigned)(p->in_cap / 1024), (unsigned)(p->out_cap / 1024));
    return mjpeg_player_open(p, path);
}

esp_err_t mjpeg_player_open(mjpeg_player_t *p, const char *path)
{
    if (p->file) {
        fclose(p->file);
        p->file = NULL;
    }
    p->ended = false;
    p->read_pos = 0;
    p->read_len = 0;

    p->file = fopen(path, "rb");
    if (p->file == NULL) {
        ESP_LOGW(TAG, "open %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(p->file, 0, SEEK_END);
    p->file_size = ftell(p->file);
    fseek(p->file, 0, SEEK_SET);
    ESP_LOGI(TAG, "opened %s (%ld bytes)", path, p->file_size);
    return ESP_OK;
}

void mjpeg_player_close_file(mjpeg_player_t *p)
{
    if (p == NULL) return;
    if (p->file != NULL) {
        fclose(p->file);
        p->file = NULL;
    }
    p->ended = true;
    p->read_pos = 0;
    p->read_len = 0;
}

/* 扫描并装载一帧到 in_buf；返回 true 表示已找到一帧 */
static bool find_and_load_frame(mjpeg_player_t *p)
{
    if (p->file == NULL || p->file_size == 0) return false;

    size_t size = 0;
    bool started = false;
    bool previous_ff = false;
    bool wrapped = false;

    while (size < p->in_cap) {
        if (p->read_pos >= p->read_len) {
            size_t n = fread(p->read_buf, 1, READ_BUFFER_SIZE, p->file);
            if (n <= 0) {
                if (!p->loop) {
                    p->ended = true;
                    return false;
                }
                if (wrapped) return false;
                wrapped = true;
                fseek(p->file, 0, SEEK_SET);
                p->read_pos = 0;
                p->read_len = 0;
                size = 0;
                started = false;
                previous_ff = false;
                continue;
            }
            p->read_pos = 0;
            p->read_len = n;
        }

        if (!started) {
            size_t marker = SIZE_MAX;
            size_t consume_to = 0;
            if (previous_ff && p->read_buf[p->read_pos] == 0xD8) {
                marker = p->read_pos;
                consume_to = marker + 1;
            } else {
                for (size_t i = p->read_pos; i + 1 < p->read_len; i++) {
                    if (p->read_buf[i] == 0xFF && p->read_buf[i + 1] == 0xD8) {
                        marker = i;
                        consume_to = i + 2;
                        break;
                    }
                }
            }
            if (marker != SIZE_MAX) {
                p->in_buf[0] = 0xFF;
                p->in_buf[1] = 0xD8;
                size = 2;
                started = true;
                previous_ff = false;
                p->read_pos = consume_to;
            } else {
                previous_ff = (p->read_buf[p->read_len - 1] == 0xFF);
                p->read_pos = p->read_len;
            }
            continue;
        }

        /* 找 EOI */
        size_t frame_end = SIZE_MAX;
        if (previous_ff && p->read_buf[p->read_pos] == 0xD9) {
            frame_end = p->read_pos + 1;
        } else {
            for (size_t i = p->read_pos; i + 1 < p->read_len; i++) {
                if (p->read_buf[i] == 0xFF && p->read_buf[i + 1] == 0xD9) {
                    frame_end = i + 2;
                    break;
                }
            }
        }

        size_t copy_end = (frame_end == SIZE_MAX) ? p->read_len : frame_end;
        size_t copy_len = copy_end - p->read_pos;
        if (copy_len > p->in_cap - size) {
            ESP_LOGW(TAG, "single frame exceeds input buffer");
            return false;
        }
        memcpy(p->in_buf + size, p->read_buf + p->read_pos, copy_len);
        size += copy_len;
        p->read_pos = copy_end;
        previous_ff = (copy_len > 0 && p->read_buf[copy_end - 1] == 0xFF);

        if (frame_end != SIZE_MAX) {
            p->frame_size = size;
            return true;
        }
    }
    ESP_LOGW(TAG, "single frame exceeds input buffer");
    return false;
}

static bool decode_loaded_frame(mjpeg_player_t *p)
{
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* RGB565 字节序（红蓝反时改 _RGB） */
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(p->dec, &dec_cfg, p->in_buf,
                                         (uint32_t)p->frame_size,
                                         (uint8_t *)p->out_buf,
                                         (uint32_t)p->out_cap, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool mjpeg_player_decode_next_frame(mjpeg_player_t *p)
{
    if (p->dec == NULL || p->in_buf == NULL || p->out_buf == NULL) {
        return false;
    }
    if (!find_and_load_frame(p)) {
        return false;
    }
    return decode_loaded_frame(p);
}

esp_err_t mjpeg_player_decode_jpeg(mjpeg_player_t *p, const char *path)
{
    if (p->dec == NULL || p->in_buf == NULL || p->out_buf == NULL) {
        return ESP_FAIL;
    }
    FILE *image = fopen(path, "rb");
    if (image == NULL) {
        ESP_LOGW(TAG, "open jpeg %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(image, 0, SEEK_END);
    long size = ftell(image);
    fseek(image, 0, SEEK_SET);
    if (size <= 0 || (size_t)size > p->in_cap) {
        ESP_LOGW(TAG, "jpeg invalid or too large: %s (%ld bytes)", path, size);
        fclose(image);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fread(p->in_buf, 1, (size_t)size, image) != (size_t)size) {
        fclose(image);
        return ESP_FAIL;
    }
    fclose(image);
    p->frame_size = (size_t)size;
    return decode_loaded_frame(p) ? ESP_OK : ESP_FAIL;
}

void mjpeg_player_set_loop(mjpeg_player_t *p, bool enabled)
{
    p->loop = enabled;
}

bool mjpeg_player_ended(const mjpeg_player_t *p)
{
    return p->ended;
}

uint16_t *mjpeg_player_frame_buffer(mjpeg_player_t *p)
{
    return p->out_buf;
}

void mjpeg_player_end(mjpeg_player_t *p)
{
    if (p->dec) {
        jpeg_del_decoder_engine(p->dec);
        p->dec = NULL;
    }
    if (p->in_buf) {
        heap_caps_free(p->in_buf);
        p->in_buf = NULL;
    }
    if (p->out_buf) {
        heap_caps_free(p->out_buf);
        p->out_buf = NULL;
    }
    if (p->read_buf) {
        heap_caps_free(p->read_buf);
        p->read_buf = NULL;
    }
    if (p->file) {
        fclose(p->file);
        p->file = NULL;
    }
}

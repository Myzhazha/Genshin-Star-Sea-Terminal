/*
 * sdcard.c - SD 卡挂载（SDMMC Slot 0, 4-bit）与媒体文件扫描
 *
 * 引脚：DET=45 CMD=44 SCLK=43 D3=42 D2=41 D1=40 D0=39
 * 目录：/sdcard/start 下的 .mjpeg、/sdcard/pic 下的 .jpg/.jpeg、
 *       /sdcard/video 下的 .mjpeg
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
/* SD 卡供电控制引脚：低电平导通（供 3.3V），高电平关闭 */
#ifndef SD_PWR_GPIO
#define SD_PWR_GPIO 13
#endif

#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sd_protocol_defs.h"
#include "sd_protocol_types.h"

#include "sdcard.h"
#include "app_config.h"

static const char *TAG = "sdcard";

static bool s_ready = false;

bool sdcard_ready(void)
{
    return s_ready;
}

esp_err_t sdcard_init(void)
{
    /* 卡检测引脚：输入上拉，仅用于日志 */
    gpio_config_t det_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << SD_PIN_DET,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&det_cfg);
    ESP_LOGI(TAG, "SD card DET(IO%d) = %d", SD_PIN_DET,
             gpio_get_level((gpio_num_t)SD_PIN_DET));

    /* SD 卡用 Slot 0（esp-hosted 的 SDIO 用 Slot 1） */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;

    /* ---- SD 卡供电：IO13 低电平导通（供 3.3V），高电平关闭 ---- */
    gpio_config_t sd_pwr_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << SD_PWR_GPIO,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sd_pwr_cfg);
    gpio_set_level(SD_PWR_GPIO, 0);   /* 低电平 = 给卡供电 */
    ESP_LOGI(TAG, "SD card power: GPIO%d ON (low)", SD_PWR_GPIO);
    vTaskDelay(pdMS_TO_TICKS(200));                  /* 等卡上电稳定 */

    /* P4 上 SD 卡 VDD 由 on-chip LDO 供电（官方默认 LDO_VO4）。
     * 不初始化电源则卡无电，CMD1(send_op_cond) 超时返回 0x107。 */
#if CONFIG_GENSHIN_SD_PWR_CTRL_LDO_CHAN >= 0
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_GENSHIN_SD_PWR_CTRL_LDO_CHAN,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = SD_PIN_D1;
    slot_config.d2 = SD_PIN_D2;
    slot_config.d3 = SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    /* 卡由外置 3.3V 供电，LDO 断电无法复位卡（仅管 IO 电平）。
     * 偶发失败时用 SD 协议 CMD0(GO_IDLE_STATE) 软复位卡，再重新挂载。 */
    const int kMaxAttempts = 8;
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
#if CONFIG_GENSHIN_SD_PWR_CTRL_LDO_CHAN >= 0
        if (attempt == 1) {
            esp_err_t ldo_ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
            if (ldo_ret == ESP_OK) {
                host.pwr_ctrl_handle = pwr_ctrl_handle;
                ESP_LOGI(TAG, "SD card power LDO channel %d enabled",
                         CONFIG_GENSHIN_SD_PWR_CTRL_LDO_CHAN);
            } else {
                host.pwr_ctrl_handle = NULL;
                ESP_LOGW(TAG, "SD power LDO init failed: %s", esp_err_to_name(ldo_ret));
            }
        }
#endif
        sdmmc_card_t *card = NULL;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                      &mount_config, &card);
        if (ret == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            s_ready = true;
            ESP_LOGI(TAG, "SD card mounted on attempt %d/%d", attempt, kMaxAttempts);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD card mount attempt %d/%d failed: %s",
                 attempt, kMaxAttempts, esp_err_to_name(ret));

        if (attempt < kMaxAttempts) {
            /* 发 CMD0(GO_IDLE_STATE) 让卡回到 idle，强制重新初始化 */
            sdmmc_command_t cmd0 = {
                .opcode = MMC_GO_IDLE_STATE,
                .arg = 0,
                .flags = SCF_CMD_BC,   /* broadcast, 无响应 */
                .data = NULL,
                .datalen = 0,
                .buflen = 0,
                .blklen = 0,
                .timeout_ms = 1000,
            };
            esp_err_t cmd_ret = sdmmc_host_do_transaction(SDMMC_HOST_SLOT_0, &cmd0);
            ESP_LOGW(TAG, "SD retry %d: CMD0 reset card -> %s", attempt,
                     (cmd_ret == ESP_OK) ? "OK" : esp_err_to_name(cmd_ret));
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    ESP_LOGE(TAG, "SD card mount failed after %d attempts (check card/FAT32 wiring)", kMaxAttempts);
    return ret;
}

static void sort_media_list(media_list_t *list)
{
    char temp[MAX_MEDIA_PATH];
    for (uint8_t i = 0; i < list->count; i++) {
        for (uint8_t j = i + 1; j < list->count; j++) {
            if (strcmp(list->path[i], list->path[j]) > 0) {
                memcpy(temp, list->path[i], sizeof(temp));
                memcpy(list->path[i], list->path[j], MAX_MEDIA_PATH);
                memcpy(list->path[j], temp, MAX_MEDIA_PATH);
            }
        }
    }
}

static const char *str_iext(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static void scan_media_folder(const char *folder, bool images, media_list_t *list)
{
    list->count = 0;
    DIR *dir = opendir(folder);
    if (dir == NULL) {
        ESP_LOGW(TAG, "media folder missing: %s", folder);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && list->count < MAX_MEDIA_FILES) {
        /* d_type 在部分 FATFS 配置下不填充（DT_UNKNOWN），此时按文件处理 */
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) {
            ESP_LOGI(TAG, "  [%s] d_type=%d (skip)", entry->d_name, entry->d_type);
            continue;
        }
        const char *name = entry->d_name;
        const char *ext = str_iext(name);
        bool supported = false;
        if (images) {
            supported = (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0);
        } else {
            /* 兼容 .mjpeg 与 FATFS 8.3 短名截断的 .mjp */
            supported = (strcasecmp(ext, "mjpeg") == 0 || strcasecmp(ext, "mjp") == 0);
        }
        ESP_LOGI(TAG, "  [%s] ext='%s' -> %s", name, ext, supported ? "MATCH" : "no-match");
        if (!supported) {
            continue;
        }
        /* 文件名截断到 63 字符，避免拼接到 128 字节路径时截断告警 */
        char short_name[64];
        strlcpy(short_name, name, sizeof(short_name));
        snprintf(list->path[list->count], MAX_MEDIA_PATH, "%s/%s", folder, short_name);
        list->count++;
    }
    closedir(dir);

    sort_media_list(list);
    ESP_LOGI(TAG, "%s: %u files", folder, list->count);
}

void sdcard_scan_all(media_list_t *start_videos, media_list_t *pictures,
                     media_list_t *videos)
{
    scan_media_folder(SD_START_DIR, false, start_videos);
    scan_media_folder(SD_PIC_DIR, true, pictures);
    scan_media_folder(SD_VIDEO_DIR, false, videos);
}

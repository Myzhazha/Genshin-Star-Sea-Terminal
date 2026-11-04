/*
 * hosted_wifi.c - esp-hosted WiFi（标准 esp_wifi API，底层走 C6 的 SDIO）
 *
 * C6 固件就绪后由这里显式初始化 ESP-Hosted：
 *   1. esp_hosted_init() 创建 SDIO 传输
 *   2. esp_hosted_connect_to_slave() 建立与 C6 的连接
 *   3. esp_netif + esp_wifi 初始化（esp_wifi_remote 组件映射到 RPC）
 */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "nvs_flash.h"

#include "hosted_wifi.h"
#include "ds1302.h"

#define HOSTED_WIFI_TAG "hosted_wifi"

static hosted_wifi_scan_done_cb_t s_scan_cb = NULL;
static void *s_scan_user_data = NULL;
static hosted_wifi_status_cb_t s_status_cb = NULL;
static void *s_status_user_data = NULL;
static bool s_wifi_enabled = true;
static bool s_wifi_initialized = false;
static bool s_hosted_initialized = false;
static bool s_sntp_initialized = false;
static bool s_time_sync_enabled = true;
static bool s_sta_has_ip = false;

static void notify_wifi_status(hosted_wifi_status_t status)
{
    if (s_status_cb != NULL) {
        s_status_cb(status, s_status_user_data);
    }
}

static void ntp_sync_cb(struct timeval *tv)
{
    if (tv == NULL) return;

    time_t now = tv->tv_sec;
    struct tm local_time;
    localtime_r(&now, &local_time);
    ds1302_write_time(&local_time);

    char text[32];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local_time);
    ESP_LOGI(HOSTED_WIFI_TAG, "SNTP synced from ntp.aliyun.com: %s", text);
}

static void init_sntp(void)
{
    if (!s_time_sync_enabled || s_sntp_initialized) return;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = ntp_sync_cb;
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret == ESP_OK) {
        s_sntp_initialized = true;
        ESP_LOGI(HOSTED_WIFI_TAG, "SNTP configured: ntp.aliyun.com");
    } else {
        ESP_LOGW(HOSTED_WIFI_TAG, "SNTP init failed: %s", esp_err_to_name(ret));
    }
}

static void start_sntp_after_ip(void)
{
    s_sta_has_ip = true;
    if (!s_time_sync_enabled) return;
    init_sntp();
    if (!s_sntp_initialized) return;
    esp_err_t ret = esp_netif_sntp_start();
    if (ret != ESP_OK) {
        ESP_LOGW(HOSTED_WIFI_TAG, "SNTP start failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(HOSTED_WIFI_TAG, "SNTP started after WiFi got IP");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num == 0) {
            if (s_scan_cb) s_scan_cb(NULL, s_scan_user_data);
            return;
        }
        wifi_ap_record_t *records = calloc(ap_num, sizeof(wifi_ap_record_t));
        if (records == NULL) {
            if (s_scan_cb) s_scan_cb(NULL, s_scan_user_data);
            return;
        }
        esp_wifi_scan_get_ap_records(&ap_num, records);

        /* 拼接 SSID 列表（去重、跳过空 SSID），'\n' 分隔 */
        size_t total = 0;
        for (uint16_t i = 0; i < ap_num; i++) {
            size_t len = strlen((const char *)records[i].ssid);
            if (len > 0) total += len + 1;
        }
        char *list = malloc(total + 1);
        if (list == NULL) {
            free(records);
            if (s_scan_cb) s_scan_cb(NULL, s_scan_user_data);
            return;
        }
        list[0] = '\0';
        size_t pos = 0;
        for (uint16_t i = 0; i < ap_num; i++) {
            const char *ssid = (const char *)records[i].ssid;
            if (strlen(ssid) == 0) continue;
            /* 简单去重 */
            bool dup = false;
            char *p = list;
            while (p != list + pos) {
                char *nl = strchr(p, '\n');
                size_t plen = nl ? (size_t)(nl - p) : strlen(p);
                if (plen == strlen(ssid) && strncmp(p, ssid, plen) == 0) {
                    dup = true;
                    break;
                }
                p = nl ? nl + 1 : p + plen;
            }
            if (dup) continue;
            size_t n = strlen(ssid);
            memcpy(list + pos, ssid, n);
            pos += n;
            list[pos++] = '\n';
        }
        if (pos > 0) {
            list[pos - 1] = '\0';   /* 去掉末尾换行 */
        } else {
            list[0] = '\0';
        }

        if (s_scan_cb) s_scan_cb(list, s_scan_user_data);
        free(list);
        free(records);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(HOSTED_WIFI_TAG, "STA connected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        s_sta_has_ip = false;
        ESP_LOGW(HOSTED_WIFI_TAG, "STA disconnected, reason=%d", d->reason);
        notify_wifi_status(HOSTED_WIFI_STATUS_DISCONNECTED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(HOSTED_WIFI_TAG, "STA got IP: " IPSTR, IP2STR(&got_ip->ip_info.ip));
        start_sntp_after_ip();
        notify_wifi_status(HOSTED_WIFI_STATUS_CONNECTED);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_sta_has_ip = false;
        ESP_LOGW(HOSTED_WIFI_TAG, "STA lost IP");
    }
}

esp_err_t hosted_wifi_init(void)
{
    /* 空白 C6 烧录时不启动此模块。C6 固件就绪后才显式创建传输，
     * 避免组件 constructor 提前占用 SDIO Slot 1。 */
    esp_err_t ret = esp_hosted_init();
    if (ret != ESP_OK) {
        ESP_LOGE(HOSTED_WIFI_TAG, "esp_hosted_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_hosted_initialized = true;

    ret = esp_hosted_connect_to_slave();
    ESP_LOGI(HOSTED_WIFI_TAG, "esp_hosted_connect_to_slave = %d (%s)",
             ret, esp_err_to_name(ret));
    if (ret != ESP_OK) {
        (void)esp_hosted_deinit();
        s_hosted_initialized = false;
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), HOSTED_WIFI_TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), HOSTED_WIFI_TAG,
                        "event loop create failed");
    esp_netif_create_default_wifi_sta();

    /* 只在 Wi-Fi 真正获取 IP 后启动阿里云 SNTP，避免无网络时反复等待。 */
    init_sntp();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), HOSTED_WIFI_TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        HOSTED_WIFI_TAG, "wifi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        HOSTED_WIFI_TAG, "IP event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                   wifi_event_handler, NULL),
                        HOSTED_WIFI_TAG, "IP lost event register failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), HOSTED_WIFI_TAG,
                        "set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), HOSTED_WIFI_TAG,
                        "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), HOSTED_WIFI_TAG, "wifi start failed");

    s_wifi_initialized = true;
    if (!s_wifi_enabled) {
        (void)esp_wifi_stop();
    }
    ESP_LOGI(HOSTED_WIFI_TAG, "WiFi (esp-hosted) ready");
    return ESP_OK;
}

void hosted_wifi_set_scan_done_cb(hosted_wifi_scan_done_cb_t cb, void *user_data)
{
    s_scan_cb = cb;
    s_scan_user_data = user_data;
}

void hosted_wifi_set_status_cb(hosted_wifi_status_cb_t cb, void *user_data)
{
    s_status_cb = cb;
    s_status_user_data = user_data;
}

esp_err_t hosted_wifi_scan_start(void)
{
    if (!s_wifi_initialized || !s_wifi_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan, false);   /* 异步 */
    ESP_LOGI(HOSTED_WIFI_TAG, "scan start: %d (%s)", ret, esp_err_to_name(ret));
    return ret;
}

esp_err_t hosted_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_wifi_initialized || !s_wifi_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    notify_wifi_status(HOSTED_WIFI_STATUS_CONNECTING);
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password != NULL && strlen(password) > 0) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), HOSTED_WIFI_TAG,
                        "wifi set config failed");
    esp_err_t ret = esp_wifi_connect();
    ESP_LOGI(HOSTED_WIFI_TAG, "connecting to %s: %d", ssid, ret);
    return ret;
}

void hosted_wifi_set_enabled(bool enabled)
{
    if (s_wifi_initialized && s_wifi_enabled == enabled) {
        return;
    }
    s_wifi_enabled = enabled;
    if (!s_wifi_initialized) {
        return;
    }
    if (enabled) {
        esp_wifi_start();
    } else {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    ESP_LOGI(HOSTED_WIFI_TAG, "wifi %s", enabled ? "enabled" : "disabled");
}

bool hosted_wifi_is_enabled(void)
{
    return s_wifi_enabled;
}

void hosted_wifi_set_time_sync_enabled(bool enabled)
{
    s_time_sync_enabled = enabled;
    if (!s_wifi_initialized) {
        return;
    }

    if (!enabled) {
        if (s_sntp_initialized) {
            esp_netif_sntp_deinit();
            s_sntp_initialized = false;
        }
        ESP_LOGI(HOSTED_WIFI_TAG, "SNTP automatic sync disabled");
        return;
    }

    if (s_sta_has_ip) {
        init_sntp();
        start_sntp_after_ip();
    }
    ESP_LOGI(HOSTED_WIFI_TAG, "SNTP automatic sync enabled");
}

bool hosted_wifi_is_initialized(void)
{
    return s_wifi_initialized;
}

void hosted_wifi_deinit_for_rom_flash(void)
{
    if (s_wifi_initialized) {
        (void)esp_hosted_bt_controller_disable();
        (void)esp_hosted_bt_controller_deinit(true);
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_hosted_initialized) {
        (void)esp_hosted_deinit();
        s_hosted_initialized = false;
    }
    if (s_sntp_initialized) {
        esp_netif_sntp_deinit();
        s_sntp_initialized = false;
    }
}

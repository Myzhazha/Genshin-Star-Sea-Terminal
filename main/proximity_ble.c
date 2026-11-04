/*
 * proximity_ble.c - 附近设备发现（NimBLE host + esp-hosted VHCI）
 *
 * 移植自 v1.3.2 proximity_ble.cpp。esp-hosted 的 BT 控制器（在 C6 侧）
 * 先 init/enable，NimBLE host 经 VHCI 复用 SDIO 与控制器通信。
 */
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"

#include "esp_hosted.h"
#include "esp_hosted_misc.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

#include "proximity_ble.h"

#define PROXIMITY_TAG "proximity_ble"

#define PAYLOAD_MAGIC0 'G'
#define PAYLOAD_MAGIC1 'F'
#define PAYLOAD_VERSION 1
#define UID_MAX_LENGTH 10

#pragma pack(push, 1)
typedef struct {
    uint16_t company_id;
    uint8_t magic[2];
    uint8_t version;
    uint32_t device_id;
    uint8_t uid_length;
    char uid[UID_MAX_LENGTH];
} proximity_payload_t;
#pragma pack(pop)

static proximity_payload_t s_payload;
static uint8_t s_addr_type = 0;
static bool s_enabled = false;
static bool s_scanning = true;
static bool s_synced = false;
static proximity_peer_cb_t s_callback = NULL;
static void *s_user_data = NULL;

static int proximity_gap_event(struct ble_gap_event *event, void *arg);

static void stop_gap(void)
{
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
}

static int start_scan(void)
{
    if (!s_synced || !s_enabled || !s_scanning || s_payload.uid_length == 0) return 0;
    ble_gap_disc_cancel();
    struct ble_gap_disc_params scan_params = {
        .passive = 1,
        .filter_duplicates = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
    };
    return ble_gap_disc(s_addr_type, BLE_HS_FOREVER, &scan_params,
                        proximity_gap_event, NULL);
}

static int proximity_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type != BLE_GAP_EVENT_DISC || !s_enabled) return 0;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) != 0) {
        return 0;
    }
    if (fields.mfg_data == NULL || fields.mfg_data_len != sizeof(proximity_payload_t)) {
        return 0;
    }

    proximity_payload_t peer;
    memcpy(&peer, fields.mfg_data, sizeof(peer));
    if (peer.magic[0] != PAYLOAD_MAGIC0 || peer.magic[1] != PAYLOAD_MAGIC1 ||
        peer.version != PAYLOAD_VERSION || peer.device_id == s_payload.device_id ||
        peer.uid_length == 0 || peer.uid_length > UID_MAX_LENGTH) {
        return 0;
    }

    char uid[UID_MAX_LENGTH + 1];
    memset(uid, 0, sizeof(uid));
    memcpy(uid, peer.uid, peer.uid_length);
    if (s_callback != NULL) {
        s_callback(peer.device_id, uid, event->disc.rssi, s_user_data);
    }
    return 0;
}

static void start_gap(void)
{
    if (!s_synced || !s_enabled || s_payload.uid_length == 0) return;
    stop_gap();

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = (const uint8_t *)&s_payload;
    fields.mfg_data_len = sizeof(s_payload);
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(PROXIMITY_TAG, "failed to set advertising payload");
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 显式设置广播间隔（100ms）：interval=0 经 esp-hosted VHCI 传给
     * C6 controller 会因非法间隔导致不广播（独立固件已验证需显式设置） */
    adv_params.itvl_min = 160;   /* 0x28 = 100ms */
    adv_params.itvl_max = 160;

    int adv_rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER,
                                   &adv_params, proximity_gap_event, NULL);
    int scan_rc = start_scan();
    ESP_LOGI(PROXIMITY_TAG, "advertise=%d scan=%d uid=%.*s",
             adv_rc, scan_rc, s_payload.uid_length, s_payload.uid);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGW(PROXIMITY_TAG, "address setup failed: %d", rc);
        return;
    }
    /* 打印本机 BLE MAC（C6 控制器的地址，手机扫描识别用） */
    uint8_t mac[6] = {0};
    if (ble_hs_id_copy_addr(s_addr_type, mac, NULL) == 0) {
        ESP_LOGI(PROXIMITY_TAG, "BLE MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    s_synced = true;
    start_gap();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void update_payload(const char *uid)
{
    memset(s_payload.uid, 0, sizeof(s_payload.uid));
    size_t length = uid ? strnlen(uid, UID_MAX_LENGTH) : 0;
    if (length > 0) {
        memcpy(s_payload.uid, uid, length);
    }
    s_payload.uid_length = (uint8_t)length;
}

bool proximity_ble_begin(uint32_t device_id, const char *uid, bool enabled,
                         proximity_peer_cb_t callback, void *user_data)
{
    s_payload.company_id = 0xFFFF;
    s_payload.magic[0] = PAYLOAD_MAGIC0;
    s_payload.magic[1] = PAYLOAD_MAGIC1;
    s_payload.version = PAYLOAD_VERSION;
    s_payload.device_id = device_id;
    s_callback = callback;
    s_user_data = user_data;
    s_enabled = enabled;
    update_payload(uid);

    /* esp-hosted：初始化/使能 C6 侧的 BT 控制器（VHCI 传输） */
    if (esp_hosted_bt_controller_init() != ESP_OK) {
        ESP_LOGE(PROXIMITY_TAG, "hosted bt controller init failed (C6 not ready?)");
        return false;
    }
    if (esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGE(PROXIMITY_TAG, "hosted bt controller enable failed");
        return false;
    }

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(PROXIMITY_TAG, "NimBLE init failed: %d", rc);
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(PROXIMITY_TAG, "BLE started (device_id=%lu, uid=%.*s)",
             (unsigned long)device_id, (int)s_payload.uid_length, s_payload.uid);
    return true;
}

void proximity_ble_update(const char *uid, bool enabled)
{
    s_enabled = enabled;
    update_payload(uid);
    if (!s_synced) return;
    if (enabled) {
        start_gap();
    } else {
        stop_gap();
    }
}

void proximity_ble_set_scanning(bool enabled)
{
    if (s_scanning == enabled) return;
    s_scanning = enabled;
    if (!s_synced || !s_enabled) return;
    if (enabled) {
        int rc = start_scan();
        ESP_LOGI(PROXIMITY_TAG, "scan resumed: %d", rc);
    } else {
        ble_gap_disc_cancel();
        ESP_LOGI(PROXIMITY_TAG, "scan paused");
    }
}

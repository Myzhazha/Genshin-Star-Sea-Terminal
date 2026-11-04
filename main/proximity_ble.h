/*
 * proximity_ble.h - 附近设备发现（BLE 广播 + 被动扫描，经 esp-hosted VHCI）
 *
 * 广播 payload（manufacturer specific data）：
 *   company_id(2B)=0xFFFF + "GF"(2B) + version(1B)=1 + device_id(4B)
 *   + uid_length(1B) + uid(<=10B)
 *
 * 两个挂件互相广播/扫描：扫到对方 → 回调(peer_id, uid, rssi)。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*proximity_peer_cb_t)(uint32_t peer_id, const char *uid,
                                    int8_t rssi, void *user_data);

/**
 * @brief 初始化 esp-hosted BT 控制器（VHCI）并启动 NimBLE host。
 * @param device_id 本机唯一 ID（来自 NVS，随机生成）
 * @param uid       本机 UID（广播内容，最长 10 字符）
 * @param enabled   初始是否启用附近可见
 */
bool proximity_ble_begin(uint32_t device_id, const char *uid, bool enabled,
                         proximity_peer_cb_t callback, void *user_data);

/** @brief 更新 UID 与可见开关（运行时设置变更） */
void proximity_ble_update(const char *uid, bool enabled);

/** @brief 暂停/恢复扫描（配合"扫描频率"设置，始终模式不调用） */
void proximity_ble_set_scanning(bool enabled);

#ifdef __cplusplus
}
#endif

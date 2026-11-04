/*
 * ina226.h - INA226 双向电流/电压/功率监测（I2C）
 *
 * 寄存器：0x00 Config / 0x01 Shunt / 0x02 Bus / 0x03 Power / 0x04 Current / 0x05 Calibration
 * 量程：Bus 0~36V（1.25mV/LSB）；Shunt 有符号 ±81.92mV（2.5uV/LSB）
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    float shunt_ohms;    /* 分流电阻（Ω），本板 0.010 */
    float current_lsb;   /* 每 LSB 电流（A） */
    bool initialized;    /* 校准和配置读回均成功 */
} ina226_t;

/**
 * @brief 创建 I2C 总线（内置上拉）并初始化 INA226（地址 0x40）。
 * @param shunt_mohm 分流电阻毫欧（本板 10）
 */
esp_err_t ina226_init(ina226_t *ina, int i2c_port, int sda_gpio, int scl_gpio,
                      uint8_t addr, uint16_t shunt_mohm);

/** @brief 一次读齐电压/电流/功率/分流电压；返回 false 表示 I2C 通信失败 */
bool ina226_read_all(const ina226_t *ina, float *bus_voltage_v,
                     float *current_a, float *power_w, float *shunt_v);

#ifdef __cplusplus
}
#endif

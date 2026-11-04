/*
 * ina226.c - INA226 驱动（ESP-IDF I2C master 新驱动）
 *
 * 校准公式：CAL = 0.00512 / (current_LSB * Rshunt)
 * 取 CAL = 4096 (0x1000) → current_LSB = 0.00512 / (4096 * Rshunt)
 * 例：Rshunt = 10mΩ → LSB = 125µA/LSB，量程 ±5.12A
 */
#include <string.h>
#include "esp_log.h"
#include "ina226.h"

#define INA226_REG_CONFIG      0x00
#define INA226_REG_SHUNT       0x01
#define INA226_REG_BUS         0x02
#define INA226_REG_POWER       0x03
#define INA226_REG_CURRENT     0x04
#define INA226_REG_CALIBRATION 0x05

static const char *TAG = "ina226";

static uint16_t ina_read_reg(const ina226_t *ina, uint8_t reg)
{
    uint8_t buf[2] = {0, 0};
    esp_err_t err = i2c_master_transmit_receive(ina->dev, &reg, 1, buf, 2, 100);
    if (err != ESP_OK) {
        return 0xFFFF;
    }
    return (uint16_t)((buf[0] << 8) | buf[1]);   /* INA226 大端 */
}

static bool ina_write_reg(const ina226_t *ina, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(ina->dev, buf, 3, 100) == ESP_OK;
}

esp_err_t ina226_init(ina226_t *ina, int i2c_port, int sda_gpio, int scl_gpio,
                      uint8_t addr, uint16_t shunt_mohm)
{
    memset(ina, 0, sizeof(*ina));
    ina->shunt_ohms = (float)shunt_mohm / 1000.0f;
    ina->current_lsb = 0.00512f / (4096.0f * ina->shunt_ohms);

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = i2c_port,
        .scl_io_num = scl_gpio,
        .sda_io_num = sda_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* I2C 无外置上拉，用内部上拉 */
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &ina->bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(ina->bus, &dev_cfg, &ina->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 校准寄存器 */
    if (!ina_write_reg(ina, INA226_REG_CALIBRATION, 0x1000)) {
        ESP_LOGW(TAG, "INA226 not responding (0x%02X)", addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* 读回 Config（上电默认 0x4000）；无应答时 readReg 返回 0xFFFF */
    uint16_t cfg = ina_read_reg(ina, INA226_REG_CONFIG);
    if (cfg == 0xFFFF || cfg == 0x0000) {
        ESP_LOGW(TAG, "INA226 config readback failed (0x%04X)", cfg);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "INA226 ready (0x%02X, %umΩ, LSB=%.1fµA)",
             addr, shunt_mohm, ina->current_lsb * 1e6f);
    ina->initialized = true;
    return ESP_OK;
}

bool ina226_read_all(const ina226_t *ina, float *bus_voltage_v,
                     float *current_a, float *power_w, float *shunt_v)
{
    if (ina == NULL || ina->dev == NULL || !ina->initialized) {
        return false;
    }
    uint16_t b = ina_read_reg(ina, INA226_REG_BUS);
    if (b == 0xFFFF) {
        return false;
    }
    uint16_t current = ina_read_reg(ina, INA226_REG_CURRENT);
    uint16_t power = ina_read_reg(ina, INA226_REG_POWER);
    uint16_t shunt = ina_read_reg(ina, INA226_REG_SHUNT);
    if (current == 0xFFFF || power == 0xFFFF || shunt == 0xFFFF) {
        return false;
    }
    if (bus_voltage_v) *bus_voltage_v = b * 0.00125f;                    /* 1.25mV/LSB */
    if (current_a) *current_a = (int16_t)current * ina->current_lsb;
    if (power_w) *power_w = power * 25.0f * ina->current_lsb;
    if (shunt_v) *shunt_v = (int16_t)shunt * 0.0000025f;
    return true;
}

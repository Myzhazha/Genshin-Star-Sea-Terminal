/*
 * ds1302.c - DS1302 三线串行 RTC（ESP-IDF GPIO 位操作）
 *
 * DS1302 要求每个独立事务都以 CE 拉低结束。时钟/日历使用突发命令：
 *   0xBE: burst write，后跟 8 个寄存器字节
 *   0xBF: burst read，后读出 8 个寄存器字节
 */
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "ds1302.h"
#include "app_config.h"

static const char *TAG = "ds1302";

static void io_write(uint8_t level)
{
    gpio_set_level(DS1302_IO, level);
}

static void io_output(void)
{
    gpio_set_direction(DS1302_IO, GPIO_MODE_OUTPUT);
}

static void io_input(void)
{
    gpio_set_direction(DS1302_IO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DS1302_IO, GPIO_PULLUP_ONLY);
}

static uint8_t io_read(void)
{
    return (uint8_t)gpio_get_level(DS1302_IO);
}

static void delay_bus_us(void)
{
    /*
     * DS1302ZN 在低电压下要求 CE 拉高到第一个 SCLK 边沿之间最多
     * 约 4 us。本地总线速度很低，统一留 5 us 裕量。
     */
    esp_rom_delay_us(5);
}

static void write_byte(uint8_t v)
{
    io_output();
    for (int i = 0; i < 8; i++) {
        io_write((v >> i) & 1);          /* LSB first */
        delay_bus_us();
        gpio_set_level(DS1302_SCLK, 1);  /* 上升沿锁存 */
        delay_bus_us();
        gpio_set_level(DS1302_SCLK, 0);
        delay_bus_us();
    }
}

static uint8_t read_byte(void)
{
    uint8_t v = 0;
    io_input();

    for (int i = 0; i < 8; i++) {
        /*
         * DS1302 在下降沿更新 I/O，下一次上升沿前数据保持有效。
         * 因此先采样当前位，再产生一对时钟让芯片推出下一位。
         * 数据按 bit0 -> bit7 传输。
         */
        delay_bus_us();
        if (io_read()) {
            v |= (uint8_t)(1U << i);
        }
        gpio_set_level(DS1302_SCLK, 1);
        delay_bus_us();
        gpio_set_level(DS1302_SCLK, 0);
        delay_bus_us();
    }
    return v;
}

static uint8_t bcd2dec(uint8_t b)
{
    return (uint8_t)((b >> 4) * 10 + (b & 0x0F));
}

static uint8_t dec2bcd(uint8_t d)
{
    return (uint8_t)(((d / 10) << 4) | (d % 10));
}

static bool bcd_valid(uint8_t value, uint8_t min, uint8_t max)
{
    uint8_t decoded = bcd2dec(value);
    return ((value & 0x0F) <= 9) &&
           (decoded >= min) &&
           (decoded <= max);
}

static void begin_transaction(void)
{
    gpio_set_level(DS1302_CE, 0);
    gpio_set_level(DS1302_SCLK, 0);
    io_output();
    io_write(0);
    delay_bus_us();
    gpio_set_level(DS1302_CE, 1);
    delay_bus_us();
}

static void end_transaction(void)
{
    gpio_set_level(DS1302_SCLK, 0);
    gpio_set_level(DS1302_CE, 0);
    io_output();
    io_write(0);
    delay_bus_us();
}

static void write_register(uint8_t command, uint8_t value)
{
    begin_transaction();
    write_byte(command);
    write_byte(value);
    end_transaction();
}

esp_err_t ds1302_init(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << DS1302_CE) | (1ULL << DS1302_SCLK) | (1ULL << DS1302_IO),
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    gpio_set_pull_mode(DS1302_IO, GPIO_PULLUP_ONLY);
    gpio_set_level(DS1302_CE, 0);
    gpio_set_level(DS1302_SCLK, 0);
    io_write(0);
    ESP_LOGI(TAG, "DS1302 init (CE=%d SCLK=%d IO=%d)", DS1302_CE, DS1302_SCLK, DS1302_IO);
    return ESP_OK;
}

bool ds1302_read_time(struct tm *t)
{
    if (t == NULL) return false;
    uint8_t raw[8];

    begin_transaction();
    write_byte(0xBF); /* clock/calendar burst read */
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = read_byte();
    }
    end_transaction();

    /* 屏蔽小时的 12/24 小时控制位和秒寄存器的 CH 位。 */
    raw[0] &= 0x7F;
    raw[2] &= 0x3F;

    if (!bcd_valid(raw[0], 0, 59) ||
        !bcd_valid(raw[1], 0, 59) ||
        !bcd_valid(raw[2], 0, 23) ||
        !bcd_valid(raw[3], 1, 31) ||
        !bcd_valid(raw[4], 1, 12) ||
        !bcd_valid(raw[5], 1, 7) ||
        !bcd_valid(raw[6], 0, 99)) {
        ESP_LOGW(TAG, "invalid DS1302 data: %02x %02x %02x %02x %02x %02x %02x %02x",
                 raw[0], raw[1], raw[2], raw[3], raw[4],
                 raw[5], raw[6], raw[7]);
        return false;
    }

    memset(t, 0, sizeof(*t));
    t->tm_sec  = bcd2dec(raw[0]);
    t->tm_min  = bcd2dec(raw[1]);
    t->tm_hour = bcd2dec(raw[2]);
    t->tm_mday = bcd2dec(raw[3]);
    t->tm_mon  = bcd2dec(raw[4]) - 1; /* tm_mon: 0~11 */
    t->tm_wday = bcd2dec(raw[5]) - 1; /* tm_wday: 0=周日 */
    t->tm_year = bcd2dec(raw[6]) + 100; /* tm_year = 年份 - 1900 */
    return true;
}

void ds1302_write_time(const struct tm *t)
{
    if (t == NULL ||
        t->tm_sec < 0 || t->tm_sec > 59 ||
        t->tm_min < 0 || t->tm_min > 59 ||
        t->tm_hour < 0 || t->tm_hour > 23 ||
        t->tm_mday < 1 || t->tm_mday > 31 ||
        t->tm_mon < 0 || t->tm_mon > 11 ||
        t->tm_year < 100 || t->tm_year > 199) {
        ESP_LOGW(TAG, "refusing invalid time");
        return;
    }

    uint8_t burst[8] = {
        (uint8_t)(dec2bcd((uint8_t)t->tm_sec) & 0x7F), /* CH=0 starts clock */
        dec2bcd((uint8_t)t->tm_min),
        (uint8_t)(dec2bcd((uint8_t)t->tm_hour) & 0x3F), /* 24-hour mode */
        dec2bcd((uint8_t)t->tm_mday),
        dec2bcd((uint8_t)(t->tm_mon + 1)),
        dec2bcd((uint8_t)((t->tm_wday >= 0 && t->tm_wday <= 6) ? t->tm_wday + 1 : 1)),
        dec2bcd((uint8_t)(t->tm_year - 100)),
        0x00, /* control register: keep write protect cleared for this burst */
    };

    /* Write-protect must be cleared in its own CE-delimited transaction. */
    write_register(0x8E, 0x00);

    begin_transaction();
    write_byte(0xBE); /* clock/calendar burst write */
    for (size_t i = 0; i < sizeof(burst); i++) {
        write_byte(burst[i]);
    }
    end_transaction();

    /* Protect the registers again after the full burst has committed. */
    write_register(0x8E, 0x80);

    struct tm verify;
    if (!ds1302_read_time(&verify) ||
        verify.tm_sec != t->tm_sec ||
        verify.tm_min != t->tm_min ||
        verify.tm_hour != t->tm_hour ||
        verify.tm_mday != t->tm_mday ||
        verify.tm_mon != t->tm_mon ||
        verify.tm_year != t->tm_year) {
        ESP_LOGW(TAG, "DS1302 write verify failed for %04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    } else {
        ESP_LOGI(TAG, "DS1302 time written: %04d-%02d-%02d %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    }
}

/*
 * ds1302.h - DS1302 三线串行 RTC（CE / SCLK / IO，LSB first）
 *
 * 时间寄存器（BCD）：秒 0x80 / 分 0x82 / 时 0x84 / 日 0x86 / 月 0x88 /
 *                    星期 0x8A / 年 0x8C；写保护寄存器 0x8E
 */
#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ds1302_init(void);

/** @brief 读取时间到 tm；失败返回 false */
bool ds1302_read_time(struct tm *t);

/** @brief 写入时间（自动关闭/恢复写保护；年份存 2000~2099 末两位） */
void ds1302_write_time(const struct tm *t);

#ifdef __cplusplus
}
#endif

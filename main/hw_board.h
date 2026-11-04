/*
 * hw_board.h - 屏幕/触摸/背光/LVGL 初始化
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_handle_t lcd;            /* ST7102 面板（媒体帧直写用） */
    esp_lcd_panel_io_handle_t io;          /* MIPI DBI IO（esp_lvgl_port 用） */
    esp_lcd_touch_handle_t touch;
    lv_display_t *disp;
    lv_indev_t *touch_indev;
    lv_obj_t *boot_screen;
} hw_board_t;

/**
 * @brief 初始化 MIPI DSI PHY 供电、ST7102 面板、ST7123 触摸、LVGL 显示。
 *        启动画面完成首次刷新后，再按 Kconfig 百分比打开背光。
 */
esp_err_t hw_board_init(hw_board_t *board);

/** @brief 背光 PWM 亮度 0~100 */
void hw_board_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif

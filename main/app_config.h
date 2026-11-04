/*
 * app_config.h - 全局引脚定义与项目常量
 *
 * ESP32-P4 4.3" ST7102 MIPI 原神挂件
 * 引脚表（与硬件接线一一对应）：
 *   SD 卡(SDMMC 4bit): DET=45 CMD=44 SCLK=43 D3=42 D2=41 D1=40 D0=39
 *   INA226(I2C):       SDA=6 SCL=5  (外置 4.7K 上拉, 地址 0x40, 分流电阻 10mΩ)
 *   DS1302ZN:          CE=10 IO=11 SCLK=12
 *   NS4168 I2S 功放:   CTRL=4 LRCLK=3 BCLK=2 DIN=1
 *   触摸 ST7123(I2C):  INT=28 RST=27 SDA=26 SCL=25
 *   屏幕 RST:          24
 *   背光 PWM:          23
 *   模式按键(按下低):  9
 *   振动马达(TC118S H桥): INA=47 INB=46（震: INA低+INB高；停: 都低）
 *   外挂 C6 (esp-hosted SDIO): P4 侧 D0=14 D1=15 D2=16 D3=17 CLK=18 CMD=19
 *                              Reset=54（连 C6 EN/CHIP_PU）；
 *                              Boot mode=0（连 C6 IO9，启动时必须为高电平）
 */
#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 屏幕 ---------------- */
#define LCD_H_RES 480
#define LCD_V_RES 800
#define LCD_BITS_PER_PIXEL 16
#define LCD_RST_PIN GPIO_NUM_24        /* ST7102 复位 */

/* MIPI DSI PHY 供电（LDO_VO3 = 2.5V） */
#define MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

/* ---------------- SD 卡（SDMMC Slot1，4-bit） ---------------- */
#define SD_PIN_DET  45
#define SD_PIN_CLK  43
#define SD_PIN_CMD  44
#define SD_PIN_D0   39
#define SD_PIN_D1   40
#define SD_PIN_D2   41
#define SD_PIN_D3   42

/* ---------------- INA226（I2C0，外置 4.7K 上拉） ---------------- */
#define INA226_SDA         GPIO_NUM_6
#define INA226_SCL         GPIO_NUM_5
#define INA226_I2C_PORT    I2C_NUM_0
#define INA226_ADDR        0x40
#define INA226_SHUNT_MOHM  10   /* 分流电阻 10mΩ */

/* ---------------- DS1302ZN ---------------- */
#define DS1302_CE   GPIO_NUM_10
#define DS1302_SCLK GPIO_NUM_12
#define DS1302_IO   GPIO_NUM_11

/* ---------------- NS4168 I2S 功放（I2S1） ---------------- */
#define AMP_CTRL      GPIO_NUM_4   /* 高电平使能 */
#define AMP_BCLK      GPIO_NUM_2
#define AMP_LRCLK     GPIO_NUM_3
#define AMP_DIN       GPIO_NUM_1
#define AMP_I2S_NUM   I2S_NUM_1
#define AMP_DEFAULT_VOLUME 70     /* 0~100，UI 里百分比 */

/* ---------------- ST7123 触摸（I2C1） ---------------- */
#define TOUCH_SDA      GPIO_NUM_26
#define TOUCH_SCL      GPIO_NUM_25
#define TOUCH_RST      GPIO_NUM_27
#define TOUCH_INT      GPIO_NUM_28
#define TOUCH_I2C_PORT I2C_NUM_1
#define TOUCH_I2C_CLK_HZ 100000

/* ---------------- 背光 / 按键（在 Kconfig） ---------------- */
#define BTN_PIN       GPIO_NUM_9    /* 按下=低电平 */

/* C6 启动模式：C6 IO9 连接到 P4 IO0，无外部上拉。
 * P4 在启动早期将该脚配置为推挽输出并保持高电平，随后才释放 C6 复位。 */
#define C6_BOOT_MODE_PIN GPIO_NUM_0
#define C6_RESET_PIN     GPIO_NUM_54

#ifndef CONFIG_GENSHIN_VIBRATION_INA_PIN
#define CONFIG_GENSHIN_VIBRATION_INA_PIN (47)
#endif
#ifndef CONFIG_GENSHIN_VIBRATION_INB_PIN
#define CONFIG_GENSHIN_VIBRATION_INB_PIN (46)
#endif
#ifndef CONFIG_GENSHIN_BACKLIGHT_PIN
#define CONFIG_GENSHIN_BACKLIGHT_PIN 23
#endif

/* ---------------- SD 卡目录 ---------------- */
#define SD_MOUNT_POINT   "/sdcard"
#define SD_ALERT_MP3     "/sdcard/alert/alert.mp3"
#define SD_START_DIR     "/sdcard/start"
#define SD_PIC_DIR       "/sdcard/pic"
#define SD_VIDEO_DIR     "/sdcard/video"
/* 字体 .bin 路径（LVGL 文件系统盘符 S: 前缀，对应 VFS /sdcard） */
#define SD_FONT_BIN      "S:/sdcard/font/hywenhei_cjk_16.bin"
#define SD_TITLE_FONT_BIN "S:/sdcard/font/hywenhei_title_32.bin"

/* ---------------- 媒体页模式 ---------------- */
typedef enum {
    APP_PAGE_START = 0,   /* 开机启动视频（循环播放 start/start.mjpeg） */
    APP_PAGE_PICTURE,     /* 图片模式（左右滑动切换） */
    APP_PAGE_VIDEO,       /* 视频模式（左右滑动切换） */
    APP_PAGE_SETTINGS,    /* 设置页（LVGL_UI） */
    APP_PAGE_COUNT
} app_page_t;

/* 图片/视频播放方式（对应设置页"图片设置/视频设置"） */
typedef enum {
    MEDIA_MODE_SINGLE = 0,    /* 固定显示 / 单个循环 */
    MEDIA_MODE_PLAYLIST,      /* 定时自动切换 / 自动轮播 */
} media_play_mode_t;

#ifdef __cplusplus
}
#endif

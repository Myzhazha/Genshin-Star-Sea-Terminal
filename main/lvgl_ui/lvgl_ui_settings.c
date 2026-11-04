#include "lvgl_ui_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * 中文界面字体：
 *  - 默认内置思源黑体 16px（GB2312 常用字，覆盖不全，仅作开机兜底）
 *  - 开机后从 SD 卡 /sdcard/font/ 下的 .bin 加载完整字库（lvgl_ui_settings_set_font）
 *  - 符号字形（键盘）由蒙塞拉特字体提供，见 show_keyboard()
 */
static const lv_font_t *s_ui_font = &lv_font_source_han_sans_sc_16_cjk;
static const lv_font_t *s_title_font = NULL;   /* 48px 大标题字体（SD 卡加载） */
#define LVGL_UI_FONT (s_ui_font)

#define UI_SCREEN_W 480
#define UI_SCREEN_H 800

#define UI_MARGIN 20
#define UI_CONTENT_W (UI_SCREEN_W - UI_MARGIN * 2)
#define UI_HEADER_H 88
#define UI_DETAIL_BODY_H (UI_SCREEN_H - UI_HEADER_H - 24)
#define UI_KEYBOARD_H 300
#define UI_BODY_WITH_KEYBOARD_H 390

#define UI_COLOR_BG lv_color_hex(0x101827)
#define UI_COLOR_PANEL lv_color_hex(0x1A2940)
#define UI_COLOR_PANEL_ALT lv_color_hex(0x31405A)
#define UI_COLOR_INK lv_color_hex(0xF6EEDC)
#define UI_COLOR_MUTED lv_color_hex(0xA7B2C0)
#define UI_COLOR_GOLD lv_color_hex(0xE3BD72)
#define UI_COLOR_GOLD_LIGHT lv_color_hex(0x453A29)
#define UI_COLOR_TEAL lv_color_hex(0x66C5C7)
#define UI_COLOR_TEAL_DARK lv_color_hex(0x8EDFE0)
#define UI_COLOR_GREEN lv_color_hex(0x89DF9E)
#define UI_COLOR_RED lv_color_hex(0xEF7F70)
#define UI_COLOR_WHITE lv_color_hex(0xFFFFFF)

typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_UID,
    UI_PAGE_ENCOUNTERS,
    UI_PAGE_NEARBY,
    UI_PAGE_SPEAKER,
    UI_PAGE_VIBRATION,
    UI_PAGE_WIFI,
    UI_PAGE_CLOCK,
    UI_PAGE_BRIGHTNESS,
    UI_PAGE_IMAGE,
    UI_PAGE_VIDEO,
    UI_PAGE_ABOUT,
} ui_page_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *home_page;
    lv_obj_t *detail_page;
    lv_obj_t *detail_body;
    lv_obj_t *keyboard;
    lv_obj_t *active_textarea;
    lv_obj_t *wifi_list;
    lv_obj_t *nearby_list;
    lv_obj_t *encounter_list;
    lv_obj_t *encounter_label;
    lv_obj_t *encounter_delete_modal;
    lv_obj_t *wifi_password;
    lv_obj_t *wifi_password_modal;
    lv_obj_t *wifi_modal_title;
    lv_obj_t *wifi_selected_label;
    lv_obj_t *wifi_status_label;
    lv_obj_t *battery_label;
    lv_obj_t *c6_update_modal;
    lv_obj_t *c6_update_status_label;
    ui_page_t active_page;
    bool keyboard_visible;

    char uid[24];
    bool uid_visible;
    uint16_t uid_duration;

    bool speaker_enabled;
    int32_t speaker_volume;
    bool vibration_enabled;
    int32_t vibration_intensity;
    int32_t brightness;
    bool image_auto;
    uint16_t image_interval;
    uint16_t video_mode;

    bool wifi_enabled;
    char wifi_password_text[64];
    char wifi_selected_ssid[32];

    char clock_text[8];
    bool clock_24h;
    bool clock_auto_sync;
    uint16_t encounter_count;
    lv_obj_t *clock_hour_dropdown;
    lv_obj_t *clock_minute_dropdown;
} ui_state_t;

static ui_state_t s_ui = {
    .uid = "1000000000",
    .uid_visible = true,
    .uid_duration = 0,
    .speaker_enabled = true,
    .speaker_volume = 70,
    .vibration_enabled = false,
    .vibration_intensity = 50,
    .brightness = 80,
    .image_auto = false,
    .image_interval = 10,
    .video_mode = 0,
    .wifi_enabled = false,
    .clock_text = "12:30",
    .clock_24h = true,
    .clock_auto_sync = true,
    .active_page = UI_PAGE_HOME,
};

static lvgl_ui_setting_cb_t s_setting_cb;
static void *s_setting_cb_user_data;

/* ---- 附近设备 UID 弹窗 ---- */
static lv_obj_t *s_nearby_alert = NULL;
static lv_obj_t *s_nearby_rssi_label = NULL;   /* 弹窗内"信号强度"标签 */
static void nearby_rssi_update(int16_t rssi)
{
    if (s_nearby_rssi_label == NULL) return;
    char text[32];
    lv_snprintf(text, sizeof(text), "信号强度%d", (int)rssi);
    lv_label_set_text(s_nearby_rssi_label, text);
}
void lvgl_ui_settings_update_nearby_rssi(int16_t rssi) { nearby_rssi_update(rssi); }
static lvgl_ui_nearby_alert_close_cb_t s_nearby_close_cb = NULL;
static void *s_nearby_user_data = NULL;

/* ---- 低电量弹窗 ---- */
static lv_obj_t *s_battery_low_alert = NULL;
static lvgl_ui_nearby_alert_close_cb_t s_battery_low_close_cb = NULL;
static void *s_battery_low_user_data = NULL;

static void notify_setting(lvgl_ui_setting_t setting, int32_t value,
                           const char *text, bool enabled)
{
    if (s_setting_cb != NULL) {
        s_setting_cb(setting, value, text, enabled, s_setting_cb_user_data);
    }
}

/* 扫描结果缓存（业务层回填），SSID 每行一个 */
#define WIFI_RESULTS_MAX    768
#define WIFI_SSID_CACHE_MAX 16
#define NEARBY_RESULTS_MAX  192
#define ENCOUNTER_RESULTS_MAX 3072
#define ENCOUNTER_CACHE_MAX 256
static char s_wifi_results[WIFI_RESULTS_MAX] = "";
static char s_wifi_ssid_cache[WIFI_SSID_CACHE_MAX][33];
static uint8_t s_wifi_ssid_count = 0;
static char s_nearby_results[NEARBY_RESULTS_MAX] = "";
static char s_encounter_results[ENCOUNTER_RESULTS_MAX] = "";
static char s_encounter_uid_cache[ENCOUNTER_CACHE_MAX][11];
static uint16_t s_encounter_uid_count = 0;
static char s_pending_delete_uid[11] = "";
static uint64_t s_flash_used = 0;
static uint64_t s_flash_total = 0;
static uint64_t s_psram_used = 0;
static uint64_t s_psram_total = 0;
static uint64_t s_tf_used = 0;
static uint64_t s_tf_total = 0;
static bool s_tf_ready = false;
static bool s_c6_connected = false;
static char s_wifi_status[24] = "未连接";

static const char s_clock_hour_options[] =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
    "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";

static const char s_clock_minute_options[] =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n"
    "15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n"
    "31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n"
    "46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

static void set_text_style(lv_obj_t *obj, lv_color_t color, int32_t size);
static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_color_t color, int32_t size);
static lv_obj_t *create_text_button(lv_obj_t *parent, const char *text);
static lv_obj_t *create_setting_card(lv_obj_t *parent, const char *title,
                                     const char *subtitle, const char *status, ui_page_t page,
                                     lv_color_t accent);
static lv_obj_t *create_section_title(lv_obj_t *parent, const char *title, const char *hint);
static lv_obj_t *create_switch_row(lv_obj_t *parent, const char *title, const char *hint,
                                   bool checked, lv_event_cb_t callback, lv_obj_t **switch_out);
static lv_obj_t *create_slider_row(lv_obj_t *parent, const char *title, int32_t value,
                                   int32_t min_value, lv_event_cb_t callback,
                                   lv_obj_t **value_label_out);
static lv_obj_t *create_textarea_row(lv_obj_t *parent, const char *title, const char *placeholder,
                                      const char *text, bool numeric, lv_obj_t **textarea_out);
static lv_obj_t *create_dropdown_row(lv_obj_t *parent, const char *title, const char *options,
                                      uint16_t selected);
static void show_keyboard(lv_obj_t *textarea, bool numeric);
static void hide_keyboard(void);
static void show_home_page(void);
static void show_detail_page(ui_page_t page);

static void build_brightness_page(void);
static void build_image_page(void);
static void build_video_page(void);
static void create_wifi_list(void);
static void create_nearby_list(void);
static void build_nearby_page(void);
static void build_encounter_page(void);
static void build_about_page(void);
static void page_open_event_cb(lv_event_t *e);
static void return_home_event_cb(lv_event_t *e);
static void textarea_event_cb(lv_event_t *e);
static void switch_event_cb(lv_event_t *e);
static void clock_auto_sync_event_cb(lv_event_t *e);
static void clock_save_event_cb(lv_event_t *e);
static void slider_event_cb(lv_event_t *e);
static void dropdown_event_cb(lv_event_t *e);
static void wifi_scan_event_cb(lv_event_t *e);
static void wifi_network_event_cb(lv_event_t *e);
static void wifi_password_modal_close_cb(lv_event_t *e);
static void create_wifi_password_modal(void);
static void commit_active_textarea(void);
static void encounter_summary_event_cb(lv_event_t *e);
static void encounter_row_event_cb(lv_event_t *e);
static void encounter_delete_cancel_cb(lv_event_t *e);
static void encounter_delete_confirm_cb(lv_event_t *e);
static void close_encounter_delete_modal(void);
static void create_encounter_list(void);
static void close_c6_update_modal(void);

static const char *const s_numeric_keyboard_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, NULL
};

static const lv_buttonmatrix_ctrl_t s_numeric_keyboard_ctrl[] = {
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
};

static const char *const s_text_keyboard_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "_", "\n",
    "z", "x", "c", "v", "b", "n", "m", "@", ".", "0", LV_SYMBOL_OK, NULL
};

static const lv_buttonmatrix_ctrl_t s_text_keyboard_ctrl[] = {
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
    LV_BUTTONMATRIX_CTRL_WIDTH_4, LV_BUTTONMATRIX_CTRL_WIDTH_4,
};

static void set_text_style(lv_obj_t *obj, lv_color_t color, int32_t size)
{
    lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, LVGL_UI_FONT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(obj, LVGL_UI_FONT, LV_PART_ITEMS);

    /*
     * The size argument is intentionally kept for future custom-font
     * variants. Built-in fonts are selected through LVGL_UI_FONT.
     */
    (void)size;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_color_t color, int32_t size)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    set_text_style(label, color, size);
    return label;
}

static lv_obj_t *create_text_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, 44);
    lv_obj_set_width(button, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(button, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(button, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_right(button, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(button, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(button, 2, LV_PART_MAIN);

    lv_obj_t *label = create_label(button, text, UI_COLOR_INK, 16);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *create_header(lv_obj_t *parent, const char *title, bool with_return)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, UI_SCREEN_W, UI_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(header, UI_MARGIN, LV_PART_MAIN);
    lv_obj_set_style_pad_right(header, UI_MARGIN, LV_PART_MAIN);
    lv_obj_set_style_pad_top(header, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(header, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (with_return) {
        lv_obj_t *return_button = create_text_button(header, "设置");
        lv_obj_add_event_cb(return_button, return_home_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_width(return_button, 84);
    } else {
        lv_obj_t *brand = lv_obj_create(header);
        lv_obj_set_size(brand, 50, 50);
        lv_obj_clear_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(brand, UI_COLOR_PANEL, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(brand, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(brand, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(brand, UI_COLOR_GOLD, LV_PART_MAIN);
        lv_obj_set_style_radius(brand, 4, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(brand, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(brand, UI_COLOR_GOLD, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(brand, LV_OPA_30, LV_PART_MAIN);
        lv_obj_t *brand_text = create_label(brand, LV_SYMBOL_SETTINGS, UI_COLOR_GOLD, 20);
        lv_obj_set_style_text_font(brand_text, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(brand_text);
    }

    lv_obj_t *title_box = lv_obj_create(header);
    /* Match the 50px icon tile so the home title aligns to its vertical center. */
    lv_obj_set_height(title_box, 50);
    lv_obj_set_flex_grow(title_box, 1);
    lv_obj_clear_flag(title_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(title_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(title_box, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title_label;
    if (with_return) {
        /* 详情页标题：常规小字 */
        title_label = create_label(title_box, title, UI_COLOR_INK, 16);
        lv_obj_set_style_text_font(title_label, LVGL_UI_FONT, LV_PART_MAIN);
    } else {
        /* 设置页主标题：32px 金色大字，用标题专用字体 */
        title_label = create_label(title_box, title, UI_COLOR_GOLD, 32);
        lv_obj_set_style_text_font(title_label,
                                   s_title_font ? s_title_font : LVGL_UI_FONT,
                                   LV_PART_MAIN);
    }

    /* 已按要求去掉 TEYVAT ASTRAL TERMINAL 副标题 */

    s_ui.battery_label = create_label(header, "电池电压 --\n电池电流 --", UI_COLOR_MUTED, 10);
    lv_obj_set_width(s_ui.battery_label, 148);
    lv_label_set_long_mode(s_ui.battery_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_ui.battery_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

    return header;
}

static lv_obj_t *create_setting_card(lv_obj_t *parent, const char *title,
                                     const char *subtitle, const char *status, ui_page_t page,
                                     lv_color_t accent)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_width(card, UI_CONTENT_W);
    lv_obj_set_height(card, 82);
    lv_obj_set_style_bg_color(card, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, UI_COLOR_PANEL_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, accent, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(card, page_open_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

    lv_obj_t *text_box = lv_obj_create(card);
    lv_obj_set_height(text_box, 60);
    lv_obj_set_flex_grow(text_box, 1);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(text_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(text_box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_right(text_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(text_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(text_box, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* Decorative children must not become the pointer target instead of the card. */
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(text_box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *title_label = create_label(text_box, title, UI_COLOR_INK, 20);
    lv_obj_clear_flag(title_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(title_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_text_font(title_label, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_t *subtitle_label = create_label(text_box, subtitle, UI_COLOR_MUTED, 12);
    lv_obj_clear_flag(subtitle_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subtitle_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(subtitle_label, 210);

    lv_obj_t *status_box = lv_obj_create(card);
    lv_obj_set_size(status_box, 150, 36);
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(status_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(status_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(status_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status_box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *status_label = create_label(status_box, status, UI_COLOR_MUTED, 12);
    lv_obj_clear_flag(status_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(status_label, 142);
    lv_obj_center(status_label);

    return card;
}

static lv_obj_t *create_section_title(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_set_width(section, UI_CONTENT_W);
    lv_obj_set_height(section, 52);
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(section, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(section, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(section, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(section, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(section, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(section, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(section, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(section, title, UI_COLOR_GOLD, 16);
    create_label(section, hint, UI_COLOR_MUTED, 11);
    return section;
}

static lv_obj_t *create_switch_row(lv_obj_t *parent, const char *title, const char *hint,
                                   bool checked, lv_event_cb_t callback, lv_obj_t **switch_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 78);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *text_box = lv_obj_create(row);
    lv_obj_set_height(text_box, 54);
    lv_obj_set_flex_grow(text_box, 1);
    lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(text_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(text_box, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(text_box, title, UI_COLOR_INK, 17);
    create_label(text_box, hint, UI_COLOR_MUTED, 11);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 62, 34);
    lv_obj_set_style_bg_color(sw, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_COLOR_GOLD, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, UI_COLOR_WHITE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 4, LV_PART_MAIN);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, callback, LV_EVENT_VALUE_CHANGED, NULL);

    if (switch_out != NULL) {
        *switch_out = sw;
    }
    return row;
}

static lv_obj_t *create_slider_row(lv_obj_t *parent, const char *title, int32_t value,
                                   int32_t min_value, lv_event_cb_t callback,
                                   lv_obj_t **value_label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, UI_CONTENT_W);
    lv_obj_set_height(row, 106);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *top = lv_obj_create(row);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, 30);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(top, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    create_label(top, title, UI_COLOR_INK, 17);

    char value_text[12];
    lv_snprintf(value_text, sizeof(value_text), "%d%%", (int)value);
    lv_obj_t *value_label = create_label(top, value_text, UI_COLOR_GOLD, 16);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 22);
    if (value < min_value) value = min_value;
    lv_slider_set_range(slider, min_value, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COLOR_GOLD, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_COLOR_WHITE, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, UI_COLOR_GOLD, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 3, LV_PART_MAIN);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, value_label);

    if (value_label_out != NULL) {
        *value_label_out = value_label;
    }
    return row;
}

static lv_obj_t *create_textarea_row(lv_obj_t *parent, const char *title, const char *placeholder,
                                      const char *text, bool numeric, lv_obj_t **textarea_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 102);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(row, title, UI_COLOR_INK, 16);

    lv_obj_t *textarea = lv_textarea_create(row);
    lv_obj_set_width(textarea, LV_PCT(100));
    lv_obj_set_height(textarea, 48);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_textarea_set_text(textarea, text != NULL ? text : "");
    lv_textarea_set_password_mode(textarea, false);
    lv_obj_set_style_bg_color(textarea, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(textarea, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(textarea, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(textarea, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_border_color(textarea, UI_COLOR_GOLD, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_radius(textarea, 3, LV_PART_MAIN);
    lv_obj_set_style_text_color(textarea, UI_COLOR_INK, LV_PART_MAIN);
    lv_obj_set_style_text_color(textarea, UI_COLOR_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(textarea, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(textarea, LVGL_UI_FONT, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(textarea, textarea_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)numeric);
    lv_obj_add_event_cb(textarea, textarea_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)numeric);
    lv_obj_add_event_cb(textarea, textarea_event_cb, LV_EVENT_FOCUSED, (void *)(uintptr_t)numeric);
    lv_obj_add_event_cb(textarea, textarea_event_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)numeric);
    lv_obj_add_event_cb(textarea, textarea_event_cb, LV_EVENT_READY, NULL);

    if (textarea_out != NULL) {
        *textarea_out = textarea;
    }
    return row;
}

static lv_obj_t *create_dropdown_row(lv_obj_t *parent, const char *title, const char *options,
                                      uint16_t selected)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, UI_CONTENT_W);
    lv_obj_set_height(row, 92);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(row, title, UI_COLOR_INK, 16);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_obj_set_width(dropdown, LV_PCT(100));
    lv_obj_set_height(dropdown, 44);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_style_bg_color(dropdown, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, UI_COLOR_GOLD, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_radius(dropdown, 3, LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, UI_COLOR_INK, LV_PART_MAIN);
    lv_obj_set_style_text_font(dropdown, LVGL_UI_FONT, LV_PART_MAIN);
    /* 弹出选项列表也要用界面字体（默认是 LV_FONT_DEFAULT，内置字体缺字） */
    lv_obj_t *dd_list = lv_dropdown_get_list(dropdown);
    lv_obj_set_style_text_font(dd_list, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd_list, LVGL_UI_FONT, LV_PART_SELECTED);
    lv_obj_add_event_cb(dropdown, dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* 去掉右侧下拉箭头（字体不含符号，避免显示方框） */
    lv_dropdown_set_symbol(dropdown, NULL);
    return row;
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *text, lv_color_t color,
                                      lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, 54);
    lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_darken(color, 15), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = create_label(button, text, UI_COLOR_WHITE, 16);
    lv_obj_center(label);
    return button;
}

static void c6_update_close_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        close_c6_update_modal();
    }
}

static void c6_update_cancel_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        close_c6_update_modal();
    }
}

static void c6_update_confirm_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    close_c6_update_modal();
    notify_setting(LVGL_UI_SETTING_C6_FIRMWARE_UPDATE_REQUESTED, 0, NULL, false);
}

static void close_c6_update_modal(void)
{
    if (s_ui.c6_update_modal != NULL) {
        lv_obj_delete(s_ui.c6_update_modal);
        s_ui.c6_update_modal = NULL;
    }
    s_ui.c6_update_status_label = NULL;
}

static lv_obj_t *create_c6_update_overlay(const char *title_text, const char *message,
                                          bool can_close)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    s_ui.c6_update_modal = overlay;
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);

    lv_obj_t *dialog = lv_obj_create(overlay);
    lv_obj_set_size(dialog, 400, can_close ? 260 : 230);
    lv_obj_center(dialog);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dialog, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, 20, LV_PART_MAIN);

    lv_obj_t *title = create_label(dialog, title_text, UI_COLOR_GOLD, 18);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *status = create_label(dialog, message, UI_COLOR_INK, 16);
    lv_obj_set_width(status, 340);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -10);
    s_ui.c6_update_status_label = status;

    if (can_close) {
        lv_obj_t *close = create_action_button(dialog, "关闭", UI_COLOR_PANEL_ALT,
                                                c6_update_close_event_cb, NULL);
        lv_obj_set_width(close, 150);
        lv_obj_set_height(close, 50);
        lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
    lv_obj_move_foreground(overlay);
    return overlay;
}

void lvgl_ui_settings_show_c6_update_confirm(void)
{
    close_c6_update_modal();
    lv_obj_t *overlay = create_c6_update_overlay(
        "C6固件更新", "将使用 SD 卡 /c6fw/ 中的三个固件文件烧录 C6，确认继续？", false);
    lv_obj_t *dialog = lv_obj_get_child(overlay, 0);
    if (dialog == NULL) return;

    lv_obj_t *buttons = lv_obj_create(dialog);
    lv_obj_set_size(buttons, 340, 52);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(buttons, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(buttons, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(buttons, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);

    lv_obj_t *cancel = create_action_button(buttons, "取消", UI_COLOR_PANEL_ALT,
                                             c6_update_cancel_event_cb, NULL);
    lv_obj_set_width(cancel, 164);
    lv_obj_set_height(cancel, 50);
    lv_obj_t *confirm = create_action_button(buttons, "开始更新", UI_COLOR_GOLD,
                                              c6_update_confirm_event_cb, NULL);
    lv_obj_set_width(confirm, 164);
    lv_obj_set_height(confirm, 50);
}

void lvgl_ui_settings_set_c6_update_status(const char *text, bool can_close)
{
    close_c6_update_modal();
    create_c6_update_overlay("C6固件更新", text != NULL ? text : "", can_close);
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (code == LV_EVENT_READY) {
            commit_active_textarea();
        }
        hide_keyboard();
    }
}

static void commit_active_textarea(void)
{
    lv_obj_t *textarea = s_ui.active_textarea;
    if (textarea == NULL) {
        return;
    }

    if (textarea == s_ui.wifi_password) {
        lv_strlcpy(s_ui.wifi_password_text, lv_textarea_get_text(textarea),
                   sizeof(s_ui.wifi_password_text));
    } else if (s_ui.active_page == UI_PAGE_UID) {
        lv_strlcpy(s_ui.uid, lv_textarea_get_text(textarea), sizeof(s_ui.uid));
        notify_setting(LVGL_UI_SETTING_UID, 0, s_ui.uid, false);
    } else if (s_ui.active_page == UI_PAGE_CLOCK) {
        lv_strlcpy(s_ui.clock_text, lv_textarea_get_text(textarea),
                   sizeof(s_ui.clock_text));
        notify_setting(LVGL_UI_SETTING_CLOCK_TEXT, 0, s_ui.clock_text, false);
    }
}

static void show_keyboard(lv_obj_t *textarea, bool numeric)
{
    if (s_ui.keyboard == NULL) {
        s_ui.keyboard = lv_keyboard_create(s_ui.root);
        lv_obj_set_size(s_ui.keyboard, UI_SCREEN_W, UI_KEYBOARD_H);
        lv_obj_align(s_ui.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(s_ui.keyboard, UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_ui.keyboard, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_ui.keyboard, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_ui.keyboard, UI_COLOR_GOLD, LV_PART_MAIN);
        lv_obj_set_style_radius(s_ui.keyboard, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_ui.keyboard, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(s_ui.keyboard, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(s_ui.keyboard, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_ui.keyboard, UI_COLOR_PANEL_ALT, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(s_ui.keyboard, UI_COLOR_GOLD,
                                  LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(s_ui.keyboard, UI_COLOR_INK, LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_ui.keyboard, UI_COLOR_BG,
                                    LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(s_ui.keyboard, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(s_ui.keyboard, UI_COLOR_GOLD, LV_PART_ITEMS);
        lv_obj_set_style_radius(s_ui.keyboard, 3, LV_PART_ITEMS);
        /* 键盘用蒙塞拉特（ASCII+LVGL 符号），中文界面字体不包含符号字形 */
        lv_obj_set_style_text_font(s_ui.keyboard, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ui.keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
        lv_obj_add_event_cb(s_ui.keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_ui.keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
        lv_keyboard_set_map(s_ui.keyboard, LV_KEYBOARD_MODE_USER_1,
                            s_numeric_keyboard_map, s_numeric_keyboard_ctrl);
        lv_keyboard_set_map(s_ui.keyboard, LV_KEYBOARD_MODE_USER_2,
                            s_text_keyboard_map, s_text_keyboard_ctrl);
    }

    bool already_open = s_ui.keyboard_visible && s_ui.active_textarea == textarea;
    s_ui.active_textarea = textarea;
    lv_keyboard_set_textarea(s_ui.keyboard, textarea);
    lv_keyboard_set_mode(s_ui.keyboard, numeric ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_USER_2);
    lv_obj_clear_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.keyboard);
    s_ui.keyboard_visible = true;

    if (s_ui.detail_body != NULL) {
        lv_obj_set_height(s_ui.detail_body, UI_BODY_WITH_KEYBOARD_H);
    }
    if (!already_open && textarea != s_ui.wifi_password) {
        lv_obj_scroll_to_view(textarea, LV_ANIM_ON);
    }
}

static void hide_keyboard(void)
{
    if (s_ui.keyboard != NULL) {
        lv_keyboard_set_textarea(s_ui.keyboard, NULL);
        lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    s_ui.active_textarea = NULL;
    s_ui.keyboard_visible = false;
    if (s_ui.detail_body != NULL) {
        lv_obj_set_height(s_ui.detail_body, UI_DETAIL_BODY_H);
    }
}

static void page_open_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ui_page_t page = (ui_page_t)(uintptr_t)lv_event_get_user_data(e);
    show_detail_page(page);
}

static void return_home_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_home_page();
    }
}

static void c6_firmware_update_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lvgl_ui_settings_show_c6_update_confirm();
}

static void textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *textarea = lv_event_get_target(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        if (textarea == s_ui.wifi_password) {
            lv_strlcpy(s_ui.wifi_password_text, lv_textarea_get_text(textarea),
                       sizeof(s_ui.wifi_password_text));
        } else if (s_ui.active_page == UI_PAGE_UID) {
            lv_strlcpy(s_ui.uid, lv_textarea_get_text(textarea), sizeof(s_ui.uid));
        } else if (s_ui.active_page == UI_PAGE_CLOCK) {
            lv_strlcpy(s_ui.clock_text, lv_textarea_get_text(textarea),
                       sizeof(s_ui.clock_text));
        }
        return;
    }

    if (code == LV_EVENT_READY) {
        s_ui.active_textarea = textarea;
        commit_active_textarea();
        hide_keyboard();
        return;
    }

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        bool numeric = (bool)(uintptr_t)lv_event_get_user_data(e);
        show_keyboard(textarea, numeric);
    }
}

static void switch_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (s_ui.active_page == UI_PAGE_UID) {
        s_ui.uid_visible = checked;
        notify_setting(LVGL_UI_SETTING_UID_VISIBLE, 0, NULL, checked);
    } else if (s_ui.active_page == UI_PAGE_SPEAKER) {
        s_ui.speaker_enabled = checked;
        notify_setting(LVGL_UI_SETTING_SPEAKER_ENABLED, 0, NULL, checked);
    } else if (s_ui.active_page == UI_PAGE_VIBRATION) {
        s_ui.vibration_enabled = checked;
        notify_setting(LVGL_UI_SETTING_VIBRATION_ENABLED, 0, NULL, checked);
    } else if (s_ui.active_page == UI_PAGE_WIFI) {
        s_ui.wifi_enabled = checked;
        notify_setting(LVGL_UI_SETTING_WIFI_ENABLED, 0, NULL, checked);
    } else if (s_ui.active_page == UI_PAGE_CLOCK) {
        s_ui.clock_24h = checked;
        notify_setting(LVGL_UI_SETTING_CLOCK_24H, 0, NULL, checked);
    } else if (s_ui.active_page == UI_PAGE_IMAGE) {
        s_ui.image_auto = checked;
        notify_setting(LVGL_UI_SETTING_IMAGE_AUTO, 0, NULL, checked);
    }
}

static void clock_auto_sync_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    s_ui.clock_auto_sync = checked;
    notify_setting(LVGL_UI_SETTING_CLOCK_AUTO_SYNC, 0, NULL, checked);
}

static void clock_save_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* 自动同步打开时，手动时间只作为预览，不提交到 RTC。 */
    if (s_ui.clock_auto_sync) {
        return;
    }
    notify_setting(LVGL_UI_SETTING_CLOCK_TEXT, 0, s_ui.clock_text, false);
}

static void slider_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    lv_obj_t *value_label = lv_event_get_user_data(e);
    char value_text[12];
    lv_snprintf(value_text, sizeof(value_text), "%d%%", (int)value);
    lv_label_set_text(value_label, value_text);

    if (s_ui.active_page == UI_PAGE_SPEAKER) {
        s_ui.speaker_volume = value;
        notify_setting(LVGL_UI_SETTING_SPEAKER_VOLUME, value, NULL, false);
    } else if (s_ui.active_page == UI_PAGE_VIBRATION) {
        s_ui.vibration_intensity = value;
        notify_setting(LVGL_UI_SETTING_VIBRATION_INTENSITY, value, NULL, false);
    } else if (s_ui.active_page == UI_PAGE_BRIGHTNESS) {
        if (value < 20) {
            value = 20;
            lv_slider_set_value(slider, value, LV_ANIM_OFF);
            lv_label_set_text(value_label, "20%");
        }
        s_ui.brightness = value;
        notify_setting(LVGL_UI_SETTING_BRIGHTNESS, value, NULL, false);
    }
}

static void dropdown_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target(e);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    if (s_ui.active_page == UI_PAGE_UID) {
        if (selected == 0) {
            s_ui.uid_duration = 0;
        } else if (selected == 1) {
            s_ui.uid_duration = 5;
        } else {
            s_ui.uid_duration = 10;
        }
        notify_setting(LVGL_UI_SETTING_UID_SCAN_SECONDS, s_ui.uid_duration, NULL, false);
    } else if (s_ui.active_page == UI_PAGE_CLOCK) {
        int hour = 0;
        int minute = 0;
        if (s_ui.clock_hour_dropdown != NULL) {
            hour = (int)lv_dropdown_get_selected(s_ui.clock_hour_dropdown);
        }
        if (s_ui.clock_minute_dropdown != NULL) {
            minute = (int)lv_dropdown_get_selected(s_ui.clock_minute_dropdown);
        }
        lv_snprintf(s_ui.clock_text, sizeof(s_ui.clock_text), "%02d:%02d", hour, minute);
    } else if (s_ui.active_page == UI_PAGE_IMAGE) {
        static const uint16_t intervals[] = {5, 10, 20, 30, 60};
        if (selected < sizeof(intervals) / sizeof(intervals[0])) {
            s_ui.image_interval = intervals[selected];
            notify_setting(LVGL_UI_SETTING_IMAGE_INTERVAL_SECONDS,
                           s_ui.image_interval, NULL, false);
        }
    } else if (s_ui.active_page == UI_PAGE_VIDEO) {
        s_ui.video_mode = selected;
        notify_setting(LVGL_UI_SETTING_VIDEO_MODE, s_ui.video_mode, NULL, false);
    }
}

static void wifi_scan_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* 通知业务层发起真实扫描；结果经 lvgl_ui_settings_set_wifi_results 回填 */
    notify_setting(LVGL_UI_SETTING_WIFI_SCAN_REQUESTED, 0, NULL, false);
}

static void wifi_network_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    const char *ssid = lv_event_get_user_data(e);
    lv_strlcpy(s_ui.wifi_selected_ssid, ssid, sizeof(s_ui.wifi_selected_ssid));
    notify_setting(LVGL_UI_SETTING_WIFI_SSID, 0, s_ui.wifi_selected_ssid, false);

    if (s_ui.wifi_selected_label != NULL) {
        char selected_text[64];
        lv_snprintf(selected_text, sizeof(selected_text), "已选择：%s", s_ui.wifi_selected_ssid);
        lv_label_set_text(s_ui.wifi_selected_label, selected_text);
    }
    if (s_ui.wifi_status_label != NULL) {
        lv_label_set_text(s_ui.wifi_status_label, "请输入密码后连接");
        lv_obj_set_style_text_color(s_ui.wifi_status_label, UI_COLOR_MUTED, LV_PART_MAIN);
    }

    if (s_ui.wifi_password_modal == NULL) {
        create_wifi_password_modal();
    }
    if (s_ui.wifi_modal_title != NULL) {
        char modal_title[64];
        lv_snprintf(modal_title, sizeof(modal_title), "连接 %s", s_ui.wifi_selected_ssid);
        lv_label_set_text(s_ui.wifi_modal_title, modal_title);
    }
    if (s_ui.wifi_password != NULL) {
        lv_textarea_set_text(s_ui.wifi_password, s_ui.wifi_password_text);
        lv_obj_clear_flag(s_ui.wifi_password_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.wifi_password_modal);
        show_keyboard(s_ui.wifi_password, false);
    }
}

static void wifi_password_modal_close_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_ui.wifi_password != NULL) {
        lv_strlcpy(s_ui.wifi_password_text, lv_textarea_get_text(s_ui.wifi_password),
                   sizeof(s_ui.wifi_password_text));
        lvgl_ui_settings_set_wifi_status("正在连接...");
        /* 密码输入完成：通知业务层用所选 SSID + 密码连接 */
        notify_setting(LVGL_UI_SETTING_WIFI_PASSWORD, 0, s_ui.wifi_password_text, false);
    }
    hide_keyboard();
    if (s_ui.wifi_password_modal != NULL) {
        lv_obj_add_flag(s_ui.wifi_password_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_wifi_password_modal(void)
{
    s_ui.wifi_password_modal = lv_obj_create(s_ui.root);
    lv_obj_set_size(s_ui.wifi_password_modal, UI_CONTENT_W, 300);
    lv_obj_align(s_ui.wifi_password_modal, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_clear_flag(s_ui.wifi_password_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.wifi_password_modal, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.wifi_password_modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.wifi_password_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.wifi_password_modal, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.wifi_password_modal, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.wifi_password_modal, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.wifi_password_modal, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.wifi_password_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.wifi_password_modal, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_ui.wifi_modal_title = create_label(s_ui.wifi_password_modal, "连接 Wi-Fi",
                                         UI_COLOR_GOLD, 18);
    create_label(s_ui.wifi_password_modal, "密码将以明文显示", UI_COLOR_MUTED, 11);
    create_textarea_row(s_ui.wifi_password_modal, "Wi-Fi 密码", "请输入密码",
                        s_ui.wifi_password_text, false, &s_ui.wifi_password);
    lv_obj_t *done = create_action_button(s_ui.wifi_password_modal, "完成",
                                          UI_COLOR_PANEL_ALT,
                                          wifi_password_modal_close_cb, NULL);
    lv_obj_set_height(done, 46);
    lv_obj_set_style_border_width(done, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(done, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_t *done_label = lv_obj_get_child(done, 0);
    if (done_label != NULL) {
        lv_obj_set_style_text_color(done_label, UI_COLOR_GOLD, LV_PART_MAIN);
    }
    lv_obj_add_flag(s_ui.wifi_password_modal, LV_OBJ_FLAG_HIDDEN);
}

static void create_wifi_list(void)
{
    if (s_ui.wifi_list == NULL) {
        return;
    }

    lv_obj_clean(s_ui.wifi_list);

    /* 把结果 SSID 缓存到静态数组，供按钮 user_data 引用 */
    s_wifi_ssid_count = 0;
    const char *p = s_wifi_results;
    while (*p != '\0' && s_wifi_ssid_count < WIFI_SSID_CACHE_MAX) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            if (len >= 33) len = 32;
            memcpy(s_wifi_ssid_cache[s_wifi_ssid_count], p, len);
            s_wifi_ssid_cache[s_wifi_ssid_count][len] = '\0';
            s_wifi_ssid_count++;
        }
        p = nl ? nl + 1 : p + strlen(p);
    }

    if (s_wifi_ssid_count == 0) {
        lv_obj_t *empty = lv_button_create(s_ui.wifi_list);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_height(empty, 54);
        lv_obj_set_style_bg_color(empty, UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(empty, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(empty, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(empty, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_left(empty, 14, LV_PART_MAIN);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        create_label(empty, "暂无结果，请先扫描", UI_COLOR_MUTED, 14);
        return;
    }

    for (uint8_t i = 0; i < s_wifi_ssid_count; i++) {
        lv_obj_t *network = lv_button_create(s_ui.wifi_list);
        lv_obj_set_width(network, LV_PCT(100));
        lv_obj_set_height(network, 54);
        lv_obj_set_style_bg_color(network, UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(network, UI_COLOR_PANEL_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(network, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(network, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(network, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_left(network, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_right(network, 14, LV_PART_MAIN);
        lv_obj_set_flex_flow(network, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(network, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(network, wifi_network_event_cb, LV_EVENT_CLICKED,
                            (void *)s_wifi_ssid_cache[i]);

        lv_obj_t *name = create_label(network, s_wifi_ssid_cache[i], UI_COLOR_INK, 15);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_t *strength = create_label(network, "点击连接", UI_COLOR_TEAL_DARK, 12);
        lv_obj_set_style_text_align(strength, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }
}

void lvgl_ui_settings_set_wifi_results(const char *newline_separated_ssids)
{
    if (newline_separated_ssids != NULL) {
        lv_strlcpy(s_wifi_results, newline_separated_ssids, sizeof(s_wifi_results));
    } else {
        s_wifi_results[0] = '\0';
    }
    if (s_ui.wifi_list != NULL) {
        create_wifi_list();
    }
}

void lvgl_ui_settings_set_wifi_status(const char *status)
{
    lv_strlcpy(s_wifi_status, status != NULL ? status : "未连接", sizeof(s_wifi_status));
    if (s_ui.wifi_status_label == NULL) return;
    lv_label_set_text(s_ui.wifi_status_label, s_wifi_status);
    if (strcmp(s_wifi_status, "已连接") == 0) {
        lv_obj_set_style_text_color(s_ui.wifi_status_label, UI_COLOR_GREEN, LV_PART_MAIN);
    } else if (strcmp(s_wifi_status, "正在连接...") == 0 ||
               strcmp(s_wifi_status, "连接中...") == 0) {
        lv_obj_set_style_text_color(s_ui.wifi_status_label, UI_COLOR_GOLD, LV_PART_MAIN);
    } else if (strcmp(s_wifi_status, "连接失败") == 0 ||
               strcmp(s_wifi_status, "已断开") == 0) {
        lv_obj_set_style_text_color(s_ui.wifi_status_label, UI_COLOR_RED, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(s_ui.wifi_status_label, UI_COLOR_MUTED, LV_PART_MAIN);
    }
}

static void create_nearby_list(void)
{
    if (s_ui.nearby_list == NULL) {
        return;
    }

    lv_obj_clean(s_ui.nearby_list);

    const char *p = s_nearby_results;
    bool has_result = false;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            char uid[11];
            if (len >= sizeof(uid)) {
                len = sizeof(uid) - 1;
            }
            memcpy(uid, p, len);
            uid[len] = '\0';

            lv_obj_t *row = lv_obj_create(s_ui.nearby_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 54);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, UI_COLOR_BG, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
            lv_obj_set_style_pad_left(row, 14, LV_PART_MAIN);
            lv_obj_set_style_pad_right(row, 14, LV_PART_MAIN);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            lv_obj_t *uid_label = create_label(row, uid, UI_COLOR_INK, 16);
            lv_obj_set_flex_grow(uid_label, 1);
            has_result = true;
        }
        p = nl ? nl + 1 : p + strlen(p);
    }

    if (!has_result) {
        lv_obj_t *empty = lv_obj_create(s_ui.nearby_list);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_height(empty, 54);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(empty, UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(empty, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(empty, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(empty, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_left(empty, 14, LV_PART_MAIN);
        create_label(empty, "暂未发现附近设备", UI_COLOR_MUTED, 14);
    }
}

void lvgl_ui_settings_set_nearby_results(const char *newline_separated_uids)
{
    if (newline_separated_uids != NULL) {
        lv_strlcpy(s_nearby_results, newline_separated_uids, sizeof(s_nearby_results));
    } else {
        s_nearby_results[0] = '\0';
    }
    if (s_ui.nearby_list != NULL) {
        create_nearby_list();
    }
}

void lvgl_ui_settings_set_encounter_count(uint16_t count)
{
    s_ui.encounter_count = count;
    if (s_ui.encounter_label != NULL) {
        lv_label_set_text_fmt(s_ui.encounter_label, "已与%u位旅行者相遇",
                              (unsigned)count);
    }
}

void lvgl_ui_settings_set_encounter_records(const char *newline_separated_uids)
{
    lv_strlcpy(s_encounter_results,
               newline_separated_uids != NULL ? newline_separated_uids : "",
               sizeof(s_encounter_results));
    if (s_ui.encounter_list != NULL) {
        create_encounter_list();
    }
}

void lvgl_ui_settings_set_system_info(uint64_t flash_used, uint64_t flash_total,
                                      uint64_t psram_used, uint64_t psram_total,
                                      uint64_t tf_used, uint64_t tf_total,
                                      bool tf_ready)
{
    s_flash_used = flash_used;
    s_flash_total = flash_total;
    s_psram_used = psram_used;
    s_psram_total = psram_total;
    s_tf_used = tf_used;
    s_tf_total = tf_total;
    s_tf_ready = tf_ready;
}

void lvgl_ui_settings_set_c6_connected(bool connected)
{
    s_c6_connected = connected;
}

static void format_storage_usage(char *buffer, size_t buffer_size,
                                 uint64_t used, uint64_t total)
{
    const uint64_t mb = 1024ULL * 1024ULL;
    uint64_t used_tenths = used * 10ULL / mb;
    uint64_t total_tenths = total * 10ULL / mb;
    lv_snprintf(buffer, buffer_size, "%llu.%llu MB / %llu.%llu MB",
                (unsigned long long)(used_tenths / 10ULL),
                (unsigned long long)(used_tenths % 10ULL),
                (unsigned long long)(total_tenths / 10ULL),
                (unsigned long long)(total_tenths % 10ULL));
}

static void build_uid_page(void)
{
    create_section_title(s_ui.detail_body, "UID 设置", "管理显示身份与附近设备可见性");

    lv_obj_t *encounter = lv_button_create(s_ui.detail_body);
    lv_obj_set_width(encounter, UI_CONTENT_W);
    lv_obj_set_height(encounter, 64);
    lv_obj_clear_flag(encounter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(encounter, UI_COLOR_GOLD_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(encounter, UI_COLOR_PANEL_ALT,
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(encounter, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(encounter, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(encounter, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(encounter, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(encounter, 16, LV_PART_MAIN);
    lv_obj_add_flag(encounter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(encounter, encounter_summary_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_align(encounter, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    char encounter_text[48];
    lv_snprintf(encounter_text, sizeof(encounter_text), "已与%u位旅行者相遇",
                (unsigned)s_ui.encounter_count);
    s_ui.encounter_label = create_label(encounter, encounter_text, UI_COLOR_GOLD, 16);
    lv_obj_center(s_ui.encounter_label);

    lv_obj_t *uid_textarea = NULL;
    create_textarea_row(s_ui.detail_body, "UID", "请输入数字 UID", s_ui.uid, true, &uid_textarea);
    lv_textarea_set_max_length(uid_textarea, 10);   /* UID 最多 10 位 */

    lv_obj_t *switch_obj = NULL;
    create_switch_row(s_ui.detail_body, "对附近设备可见", "打开后，其他设备可发现你的 UID",
                      s_ui.uid_visible, switch_event_cb, &switch_obj);
    create_dropdown_row(s_ui.detail_body, "扫描频率", "始终\n5 秒\n10 秒",
                        s_ui.uid_duration == 0 ? 0 : s_ui.uid_duration == 5 ? 1 : 2);

    lv_obj_t *tip = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(tip, UI_CONTENT_W);
    lv_obj_set_height(tip, 64);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tip, UI_COLOR_GOLD_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tip, 14, LV_PART_MAIN);
    lv_obj_set_flex_align(tip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    create_label(tip, "提示：点击输入框后，使用屏幕键盘编辑 UID。", UI_COLOR_TEAL_DARK, 12);
}

static void encounter_summary_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_detail_page(UI_PAGE_ENCOUNTERS);
    }
}

static void close_encounter_delete_modal(void)
{
    if (s_ui.encounter_delete_modal != NULL) {
        lv_obj_delete(s_ui.encounter_delete_modal);
        s_ui.encounter_delete_modal = NULL;
    }
    s_pending_delete_uid[0] = '\0';
}

static void encounter_delete_cancel_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        close_encounter_delete_modal();
    }
}

static void encounter_delete_confirm_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_pending_delete_uid[0] == '\0') {
        return;
    }
    char uid[sizeof(s_pending_delete_uid)];
    lv_strlcpy(uid, s_pending_delete_uid, sizeof(uid));
    close_encounter_delete_modal();
    notify_setting(LVGL_UI_SETTING_ENCOUNTER_DELETE, 0, uid, false);
}

static void show_encounter_delete_modal(const char *uid)
{
    close_encounter_delete_modal();
    lv_strlcpy(s_pending_delete_uid, uid, sizeof(s_pending_delete_uid));

    lv_obj_t *overlay = lv_obj_create(s_ui.root);
    s_ui.encounter_delete_modal = overlay;
    lv_obj_set_size(overlay, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);

    lv_obj_t *dialog = lv_obj_create(overlay);
    lv_obj_set_size(dialog, 400, 240);
    lv_obj_center(dialog);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dialog, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, 20, LV_PART_MAIN);

    lv_obj_t *title = create_label(dialog, "删除相遇记录", UI_COLOR_GOLD, 18);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    char message[48];
    lv_snprintf(message, sizeof(message), "确认删除 UID %s？", uid);
    lv_obj_t *message_label = create_label(dialog, message, UI_COLOR_INK, 16);
    lv_obj_align(message_label, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *buttons = lv_obj_create(dialog);
    lv_obj_set_size(buttons, 340, 52);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(buttons, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(buttons, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(buttons, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);

    lv_obj_t *cancel = create_action_button(buttons, "取消", UI_COLOR_PANEL_ALT,
                                            encounter_delete_cancel_cb, NULL);
    lv_obj_set_width(cancel, 164);
    lv_obj_set_height(cancel, 50);
    lv_obj_t *confirm = create_action_button(buttons, "删除", UI_COLOR_RED,
                                             encounter_delete_confirm_cb, NULL);
    lv_obj_set_width(confirm, 164);
    lv_obj_set_height(confirm, 50);
    lv_obj_move_foreground(overlay);
}

static void encounter_row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        const char *uid = lv_event_get_user_data(e);
        if (uid != NULL && uid[0] != '\0') {
            show_encounter_delete_modal(uid);
        }
    }
}

static void create_encounter_list(void)
{
    if (s_ui.encounter_list == NULL) return;
    lv_obj_clean(s_ui.encounter_list);
    s_encounter_uid_count = 0;

    const char *p = s_encounter_results;
    while (*p != '\0' && s_encounter_uid_count < ENCOUNTER_CACHE_MAX) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            if (len >= sizeof(s_encounter_uid_cache[0])) {
                len = sizeof(s_encounter_uid_cache[0]) - 1;
            }
            memcpy(s_encounter_uid_cache[s_encounter_uid_count], p, len);
            s_encounter_uid_cache[s_encounter_uid_count][len] = '\0';
            s_encounter_uid_count++;
        }
        p = nl ? nl + 1 : p + strlen(p);
    }

    if (s_encounter_uid_count == 0) {
        lv_obj_t *empty = lv_obj_create(s_ui.encounter_list);
        lv_obj_set_size(empty, LV_PCT(100), 60);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(empty, UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(empty, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(empty, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(empty, 4, LV_PART_MAIN);
        lv_obj_t *label = create_label(empty, "暂无相遇记录", UI_COLOR_MUTED, 14);
        lv_obj_center(label);
        return;
    }

    for (uint16_t i = 0; i < s_encounter_uid_count; i++) {
        lv_obj_t *row = lv_button_create(s_ui.encounter_list);
        lv_obj_set_size(row, LV_PCT(100), 58);
        lv_obj_set_style_bg_color(row, UI_COLOR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, UI_COLOR_PANEL_ALT,
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
        lv_obj_add_event_cb(row, encounter_row_event_cb, LV_EVENT_LONG_PRESSED,
                            s_encounter_uid_cache[i]);
        lv_obj_t *label = create_label(row, s_encounter_uid_cache[i], UI_COLOR_INK, 16);
        lv_obj_center(label);
    }
}

static void build_encounter_page(void)
{
    create_section_title(s_ui.detail_body, "相遇记录", "已记录的旅行者 UID");
    s_ui.encounter_list = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(s_ui.encounter_list, UI_CONTENT_W);
    lv_obj_set_height(s_ui.encounter_list, UI_DETAIL_BODY_H - 84);
    lv_obj_add_flag(s_ui.encounter_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_ui.encounter_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_ui.encounter_list, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.encounter_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.encounter_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.encounter_list, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.encounter_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.encounter_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.encounter_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.encounter_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.encounter_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    create_encounter_list();
}

static void build_nearby_page(void)
{
    create_section_title(s_ui.detail_body, "附近的设备", "显示最近扫描到的设备 UID");

    s_ui.nearby_list = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(s_ui.nearby_list, UI_CONTENT_W);
    lv_obj_set_height(s_ui.nearby_list, UI_DETAIL_BODY_H - 84);
    lv_obj_add_flag(s_ui.nearby_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_ui.nearby_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_ui.nearby_list, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.nearby_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.nearby_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.nearby_list, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.nearby_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.nearby_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.nearby_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.nearby_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.nearby_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    create_nearby_list();
}

static void create_about_row(lv_obj_t *parent, const char *title, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, UI_CONTENT_W);
    lv_obj_set_height(row, 62);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(row, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = create_label(row, title, UI_COLOR_MUTED, 14);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_t *value_label = create_label(row, value, UI_COLOR_INK, 14);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}

static void build_about_page(void)
{
    create_section_title(s_ui.detail_body, "关于终端", "终端存储与版本信息");

    char flash_text[48];
    char psram_text[48];
    char tf_text[48];
    format_storage_usage(flash_text, sizeof(flash_text), s_flash_used, s_flash_total);
    format_storage_usage(psram_text, sizeof(psram_text), s_psram_used, s_psram_total);
    if (s_tf_ready) {
        format_storage_usage(tf_text, sizeof(tf_text), s_tf_used, s_tf_total);
    } else {
        lv_strlcpy(tf_text, "未挂载", sizeof(tf_text));
    }

    create_about_row(s_ui.detail_body, "Flash使用量：", flash_text);
    create_about_row(s_ui.detail_body, "PSRAM使用量：", psram_text);
    create_about_row(s_ui.detail_body, "TF卡使用量：", tf_text);
    create_about_row(s_ui.detail_body, "C6状态：",
                     s_c6_connected ? "已连接" : "未连接");
    create_about_row(s_ui.detail_body, "硬件版本：", "V1.0");
    create_about_row(s_ui.detail_body, "软件版本：", "V1.1");
    create_about_row(s_ui.detail_body, "作者：", "渣渣一块钱4个");

    create_action_button(s_ui.detail_body, "C6固件更新", UI_COLOR_GOLD,
                         c6_firmware_update_event_cb, NULL);
}

static void build_speaker_page(void)
{
    create_section_title(s_ui.detail_body, "喇叭设置", "调整提示音与播放音量");
    create_switch_row(s_ui.detail_body, "喇叭开关", "控制所有提示音输出",
                      s_ui.speaker_enabled, switch_event_cb, NULL);
    create_slider_row(s_ui.detail_body, "音量", s_ui.speaker_volume, 0, slider_event_cb, NULL);

    lv_obj_t *tip = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(tip, UI_CONTENT_W);
    lv_obj_set_height(tip, 88);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tip, UI_COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tip, UI_COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tip, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(tip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(tip, "当前音量", UI_COLOR_TEAL_DARK, 13);
    char volume_text[32];
    lv_snprintf(volume_text, sizeof(volume_text), "%d%% · 渐进式音量", (int)s_ui.speaker_volume);
    create_label(tip, volume_text, UI_COLOR_INK, 16);
}

static void build_vibration_page(void)
{
    create_section_title(s_ui.detail_body, "振动设置", "调整触摸反馈与提醒振动");
    create_switch_row(s_ui.detail_body, "振动开关", "控制所有振动反馈",
                      s_ui.vibration_enabled, switch_event_cb, NULL);
    create_slider_row(s_ui.detail_body, "振动强度", s_ui.vibration_intensity, 0, slider_event_cb, NULL);

    lv_obj_t *tip = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(tip, UI_CONTENT_W);
    lv_obj_set_height(tip, 88);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tip, UI_COLOR_GOLD_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tip, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(tip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    create_label(tip, "建议", UI_COLOR_GOLD, 13);
    create_label(tip, "夜间模式可将强度调至 30% 以下。", UI_COLOR_INK, 14);
}

static void build_brightness_page(void)
{
    create_section_title(s_ui.detail_body, "屏幕亮度", "调节屏幕显示亮度");
    create_slider_row(s_ui.detail_body, "屏幕亮度", s_ui.brightness, 20, slider_event_cb, NULL);
}

static void build_image_page(void)
{
    create_section_title(s_ui.detail_body, "图片设置", "设置图片固定显示或定时自动切换");
    create_switch_row(s_ui.detail_body, "自动切换", "关闭时固定显示当前图片",
                      s_ui.image_auto, switch_event_cb, NULL);

    uint16_t selected = 1;
    static const uint16_t intervals[] = {5, 10, 20, 30, 60};
    for (uint16_t i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i) {
        if (s_ui.image_interval == intervals[i]) {
            selected = i;
            break;
        }
    }
    create_dropdown_row(s_ui.detail_body, "切换时间（秒）", "5 秒\n10 秒\n20 秒\n30 秒\n60 秒",
                        selected);
}

static void build_video_page(void)
{
    create_section_title(s_ui.detail_body, "视频设置", "选择视频播放循环方式");
    create_dropdown_row(s_ui.detail_body, "播放模式", "单个循环\n自动轮播", s_ui.video_mode);
}

static void build_wifi_page(void)
{
    create_section_title(s_ui.detail_body, "Wi-Fi 设置", "选择网络后输入密码，密码以明文显示");
    create_switch_row(s_ui.detail_body, "Wi-Fi 开关", "启用或停用无线网络功能",
                      s_ui.wifi_enabled, switch_event_cb, NULL);
    lv_obj_t *scan_btn = create_action_button(s_ui.detail_body, "扫描附近 Wi-Fi", UI_COLOR_PANEL, wifi_scan_event_cb, NULL);
    /* 与左上角"设置"按钮同风格：深蓝底 + 金色边框 */
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(scan_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(scan_btn, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(scan_btn, 4, LV_PART_MAIN);

    s_ui.wifi_list = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(s_ui.wifi_list, UI_CONTENT_W);
    lv_obj_set_height(s_ui.wifi_list, 244);
    lv_obj_add_flag(s_ui.wifi_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_ui.wifi_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_ui.wifi_list, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.wifi_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.wifi_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.wifi_list, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.wifi_list, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.wifi_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.wifi_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_ui.wifi_selected_label = create_label(s_ui.detail_body,
                                            "点击 Wi-Fi 名称后输入密码",
                                            UI_COLOR_MUTED, 12);
    lv_obj_set_width(s_ui.wifi_selected_label, UI_CONTENT_W);
    s_ui.wifi_status_label = create_label(s_ui.detail_body, s_wifi_status, UI_COLOR_MUTED, 12);
    lv_obj_set_width(s_ui.wifi_status_label, UI_CONTENT_W);
    lvgl_ui_settings_set_wifi_status(s_wifi_status);

    create_wifi_list();
}

static void build_clock_page(void)
{
    create_section_title(s_ui.detail_body, "时钟设置", "设置显示时间与时间格式");
    uint16_t hour_selected = 0;
    uint16_t minute_selected = 0;
    int hour = 0;
    int minute = 0;
    if (sscanf(s_ui.clock_text, "%d:%d", &hour, &minute) == 2) {
        if (hour >= 0 && hour <= 23) {
            hour_selected = (uint16_t)hour;
        }
        if (minute >= 0 && minute <= 59) {
            minute_selected = (uint16_t)minute;
        }
    }
    lv_obj_t *time_row = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(time_row, UI_CONTENT_W);
    lv_obj_set_height(time_row, 92);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(time_row, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(time_row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(time_row, UI_COLOR_PANEL_ALT, LV_PART_MAIN);
    lv_obj_set_style_radius(time_row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(time_row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ui.clock_hour_dropdown = lv_dropdown_create(time_row);
    lv_obj_set_width(s_ui.clock_hour_dropdown, 150);
    lv_dropdown_set_options(s_ui.clock_hour_dropdown, s_clock_hour_options);
    lv_dropdown_set_selected(s_ui.clock_hour_dropdown, hour_selected);
    create_label(time_row, ":", UI_COLOR_GOLD, 22);
    s_ui.clock_minute_dropdown = lv_dropdown_create(time_row);
    lv_obj_set_width(s_ui.clock_minute_dropdown, 150);
    lv_dropdown_set_options(s_ui.clock_minute_dropdown, s_clock_minute_options);
    lv_dropdown_set_selected(s_ui.clock_minute_dropdown, minute_selected);
    lv_obj_set_style_bg_color(s_ui.clock_hour_dropdown, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.clock_minute_dropdown, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.clock_hour_dropdown, UI_COLOR_INK, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.clock_minute_dropdown, UI_COLOR_INK, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.clock_hour_dropdown, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.clock_minute_dropdown, UI_COLOR_GOLD, LV_PART_MAIN);
    /* 弹出列表字体与按钮一致（内置默认字体缺字） */
    lv_obj_t *hh_list = lv_dropdown_get_list(s_ui.clock_hour_dropdown);
    lv_obj_t *mm_list = lv_dropdown_get_list(s_ui.clock_minute_dropdown);
    lv_obj_set_style_text_font(hh_list, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(hh_list, LVGL_UI_FONT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(mm_list, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(mm_list, LVGL_UI_FONT, LV_PART_SELECTED);
    lv_dropdown_set_symbol(s_ui.clock_hour_dropdown, NULL);
    lv_dropdown_set_symbol(s_ui.clock_minute_dropdown, NULL);
    lv_obj_add_event_cb(s_ui.clock_hour_dropdown, dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ui.clock_minute_dropdown, dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    create_action_button(s_ui.detail_body, "保存时间", UI_COLOR_GOLD,
                         clock_save_event_cb, NULL);
    create_switch_row(s_ui.detail_body, "自动同步时间", "连接 Wi-Fi 后从阿里云自动校准时间",
                      s_ui.clock_auto_sync, clock_auto_sync_event_cb, NULL);
    create_switch_row(s_ui.detail_body, "24 小时制", "关闭后使用 12 小时制显示",
                      s_ui.clock_24h, switch_event_cb, NULL);

    lv_obj_t *tip = lv_obj_create(s_ui.detail_body);
    lv_obj_set_width(tip, UI_CONTENT_W);
    lv_obj_set_height(tip, 96);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tip, UI_COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(tip, UI_COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_radius(tip, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tip, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(tip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(tip, "当前预览", UI_COLOR_TEAL_DARK, 13);
    char preview_text[32];
    lv_snprintf(preview_text, sizeof(preview_text), "%s · %s",
                s_ui.clock_text, s_ui.clock_24h ? "24 小时制" : "12 小时制");
    lv_obj_set_style_text_align(create_label(tip, preview_text, UI_COLOR_INK, 20),
                                LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void create_detail_page(ui_page_t page)
{
    s_ui.detail_page = lv_obj_create(s_ui.root);
    lv_obj_set_size(s_ui.detail_page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_clear_flag(s_ui.detail_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.detail_page, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.detail_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.detail_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.detail_page, 0, LV_PART_MAIN);

    const char *title = "设置";
    switch (page) {
    case UI_PAGE_UID:
        title = "原神 UID";
        break;
    case UI_PAGE_ENCOUNTERS:
        title = "相遇记录";
        break;
    case UI_PAGE_NEARBY:
        title = "附近的设备";
        break;
    case UI_PAGE_SPEAKER:
        title = "喇叭";
        break;
    case UI_PAGE_VIBRATION:
        title = "振动";
        break;
    case UI_PAGE_WIFI:
        title = "Wi-Fi";
        break;
    case UI_PAGE_CLOCK:
        title = "时钟";
        break;
    case UI_PAGE_BRIGHTNESS:
        title = "屏幕亮度";
        break;
    case UI_PAGE_IMAGE:
        title = "图片设置";
        break;
    case UI_PAGE_VIDEO:
        title = "视频设置";
        break;
    case UI_PAGE_ABOUT:
        title = "关于终端";
        break;
    default:
        break;
    }
    create_header(s_ui.detail_page, title, true);

    s_ui.detail_body = lv_obj_create(s_ui.detail_page);
    lv_obj_set_size(s_ui.detail_body, UI_CONTENT_W, UI_DETAIL_BODY_H);
    lv_obj_align(s_ui.detail_body, LV_ALIGN_TOP_MID, 0, UI_HEADER_H);
    lv_obj_add_flag(s_ui.detail_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_ui.detail_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_ui.detail_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.detail_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_ui.detail_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_ui.detail_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_ui.detail_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_ui.detail_body, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.detail_body, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.detail_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.detail_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    switch (page) {
    case UI_PAGE_UID:
        build_uid_page();
        break;
    case UI_PAGE_ENCOUNTERS:
        build_encounter_page();
        break;
    case UI_PAGE_NEARBY:
        build_nearby_page();
        break;
    case UI_PAGE_SPEAKER:
        build_speaker_page();
        break;
    case UI_PAGE_VIBRATION:
        build_vibration_page();
        break;
    case UI_PAGE_WIFI:
        build_wifi_page();
        break;
    case UI_PAGE_CLOCK:
        build_clock_page();
        break;
    case UI_PAGE_BRIGHTNESS:
        build_brightness_page();
        break;
    case UI_PAGE_IMAGE:
        build_image_page();
        break;
    case UI_PAGE_VIDEO:
        build_video_page();
        break;
    case UI_PAGE_ABOUT:
        build_about_page();
        break;
    default:
        break;
    }
}

static void show_detail_page(ui_page_t page)
{
    close_encounter_delete_modal();
    commit_active_textarea();
    hide_keyboard();
    if (s_ui.wifi_password_modal != NULL) {
        lv_obj_del(s_ui.wifi_password_modal);
        s_ui.wifi_password_modal = NULL;
        s_ui.wifi_modal_title = NULL;
        s_ui.wifi_password = NULL;
    }
    if (s_ui.home_page != NULL) {
        lv_obj_add_flag(s_ui.home_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.detail_page != NULL) {
        lv_obj_del(s_ui.detail_page);
        s_ui.detail_page = NULL;
    }

    s_ui.detail_body = NULL;
    s_ui.wifi_list = NULL;
    s_ui.nearby_list = NULL;
    s_ui.encounter_list = NULL;
    s_ui.encounter_label = NULL;
    s_ui.wifi_password = NULL;
    s_ui.wifi_selected_label = NULL;
    s_ui.wifi_status_label = NULL;
    s_ui.active_page = page;
    create_detail_page(page);
}

static void show_home_page(void)
{
    close_encounter_delete_modal();
    commit_active_textarea();
    hide_keyboard();
    if (s_ui.wifi_password_modal != NULL) {
        lv_obj_del(s_ui.wifi_password_modal);
        s_ui.wifi_password_modal = NULL;
        s_ui.wifi_modal_title = NULL;
        s_ui.wifi_password = NULL;
    }
    if (s_ui.detail_page != NULL) {
        lv_obj_del(s_ui.detail_page);
        s_ui.detail_page = NULL;
    }
    s_ui.detail_body = NULL;
    s_ui.wifi_list = NULL;
    s_ui.nearby_list = NULL;
    s_ui.encounter_list = NULL;
    s_ui.encounter_label = NULL;
    s_ui.wifi_password = NULL;
    s_ui.wifi_password_modal = NULL;
    s_ui.wifi_modal_title = NULL;
    s_ui.wifi_selected_label = NULL;
    s_ui.wifi_status_label = NULL;
    s_ui.active_page = UI_PAGE_HOME;
    if (s_ui.home_page != NULL) {
        lv_obj_del(s_ui.home_page);
    }

    s_ui.home_page = lv_obj_create(s_ui.root);
    lv_obj_set_size(s_ui.home_page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_clear_flag(s_ui.home_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_ui.home_page, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.home_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.home_page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.home_page, 0, LV_PART_MAIN);

    create_header(s_ui.home_page, "星海终端设置", false);

    lv_obj_t *intro = lv_obj_create(s_ui.home_page);
    lv_obj_set_size(intro, UI_CONTENT_W, 72);
    lv_obj_align(intro, LV_ALIGN_TOP_MID, 0, UI_HEADER_H + 4);
    lv_obj_clear_flag(intro, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(intro, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(intro, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(intro, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(intro, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(intro, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_label(intro, "向着星辰与深渊", UI_COLOR_GOLD, 18);
    create_label(intro, "选择要调整的元素回路", UI_COLOR_MUTED, 12);

    lv_obj_t *cards = lv_obj_create(s_ui.home_page);
    lv_obj_set_size(cards, UI_CONTENT_W, 570);
    lv_obj_align(cards, LV_ALIGN_TOP_MID, 0, UI_HEADER_H + 78);
    lv_obj_add_flag(cards, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cards, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cards, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cards, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(cards, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    char uid_status[32];
    lv_snprintf(uid_status, sizeof(uid_status), "%s", s_ui.uid);
    create_setting_card(cards, "UID 设置", "身份、可见性与扫描频率", uid_status,
                        UI_PAGE_UID, UI_COLOR_GOLD);

    create_setting_card(cards, "附近的设备", "查看最近扫描到的 UID", "查看",
                        UI_PAGE_NEARBY, UI_COLOR_TEAL);

    char speaker_status[16];
    lv_snprintf(speaker_status, sizeof(speaker_status), "%d%%", (int)s_ui.speaker_volume);
    create_setting_card(cards, "喇叭设置", "开关与音量百分比", speaker_status,
                        UI_PAGE_SPEAKER, UI_COLOR_TEAL);

    create_setting_card(cards, "振动设置", "开关与触感强度",
                        s_ui.vibration_enabled ? "开启" : "关闭",
                        UI_PAGE_VIBRATION, UI_COLOR_GREEN);

    create_setting_card(cards, "Wi-Fi 设置", "扫描网络并输入密码",
                        s_ui.wifi_enabled ? "开启" : "关闭",
                        UI_PAGE_WIFI, UI_COLOR_TEAL_DARK);

    create_setting_card(cards, "时钟设置", "时间与 12/24 小时制",
                        s_ui.clock_24h ? "24 小时" : "12 小时",
                        UI_PAGE_CLOCK, UI_COLOR_RED);

    char brightness_status[16];
    lv_snprintf(brightness_status, sizeof(brightness_status), "%d%%", (int)s_ui.brightness);
    create_setting_card(cards, "屏幕亮度", "调节屏幕显示亮度", brightness_status,
                        UI_PAGE_BRIGHTNESS, UI_COLOR_GOLD);

    create_setting_card(cards, "图片设置", "固定显示或定时自动切换",
                        s_ui.image_auto ? "自动切换" : "固定显示",
                        UI_PAGE_IMAGE, UI_COLOR_TEAL);

    create_setting_card(cards, "视频设置", "单个循环或自动轮播",
                        s_ui.video_mode == 0 ? "单个循环" : "自动轮播",
                        UI_PAGE_VIDEO, UI_COLOR_RED);

    create_setting_card(cards, "关于终端", "存储、版本与作者信息", "查看",
                        UI_PAGE_ABOUT, UI_COLOR_GOLD);

    lv_obj_t *footer = create_label(s_ui.home_page, "星海终端V1.1\nby：渣渣一块钱4个",
                                    UI_COLOR_MUTED, 10);
    lv_obj_set_width(footer, UI_CONTENT_W);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void lvgl_ui_settings_create(void)
{
    s_ui.root = lv_obj_create(NULL);
    lv_obj_set_size(s_ui.root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(s_ui.root, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.root, 0, LV_PART_MAIN);
    s_ui.home_page = NULL;
    s_ui.detail_page = NULL;
    s_ui.detail_body = NULL;
    s_ui.keyboard = NULL;
    s_ui.active_textarea = NULL;
    s_ui.wifi_list = NULL;
    s_ui.nearby_list = NULL;
    s_ui.encounter_list = NULL;
    s_ui.encounter_label = NULL;
    s_ui.encounter_delete_modal = NULL;
    s_ui.c6_update_modal = NULL;
    s_ui.c6_update_status_label = NULL;
    s_ui.wifi_password = NULL;
    s_ui.wifi_selected_label = NULL;
    s_ui.wifi_status_label = NULL;
    s_ui.keyboard_visible = false;
    show_home_page();
}

void lvgl_ui_settings_set_event_cb(lvgl_ui_setting_cb_t callback, void *user_data)
{
    s_setting_cb = callback;
    s_setting_cb_user_data = user_data;
}

void lvgl_ui_settings_set_font(const lv_font_t *font)
{
    if (font != NULL) {
        s_ui_font = font;
    }
}

void lvgl_ui_settings_set_title_font(const lv_font_t *font)
{
    if (font != NULL) {
        s_title_font = font;
    }
}

lv_obj_t *lvgl_ui_settings_get_root(void)
{
    return s_ui.root;
}

void lvgl_ui_settings_set_battery(float voltage_v, float current_ma, bool valid)
{
    if (s_ui.battery_label == NULL) return;
    if (valid) {
        lv_label_set_text_fmt(s_ui.battery_label, "电池电压 %.2fV\n电池电流 %.0fmA",
                              voltage_v, current_ma);
    } else {
        lv_label_set_text(s_ui.battery_label, "电池电压 --\n电池电流 --");
    }
}

/* ==================== 附近设备 UID 弹窗 ==================== */

static void nearby_alert_close_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lvgl_ui_nearby_alert_close_cb_t callback = s_nearby_close_cb;
    void *user_data = s_nearby_user_data;
    lvgl_ui_settings_close_nearby_alert();
    if (callback != NULL) {
        callback(user_data);
    }
}

void lvgl_ui_settings_close_nearby_alert(void)
{
    if (s_nearby_alert != NULL) {
        lv_obj_delete(s_nearby_alert);
        s_nearby_alert = NULL;
    }
    s_nearby_rssi_label = NULL;
    s_nearby_close_cb = NULL;
    s_nearby_user_data = NULL;
}

void lvgl_ui_settings_show_nearby_alert(const char *uid, int16_t rssi,
                                        lvgl_ui_nearby_alert_close_cb_t close_cb,
                                        void *user_data)
{
    lvgl_ui_settings_close_nearby_alert();
    (void)rssi;

    lv_obj_t *screen = lv_layer_top();
    s_nearby_alert = lv_obj_create(screen);
    lv_obj_set_size(s_nearby_alert, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_nearby_alert, 0, 0);
    lv_obj_set_style_bg_color(s_nearby_alert, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_nearby_alert, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_nearby_alert, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_nearby_alert, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_nearby_alert, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog = lv_obj_create(s_nearby_alert);
    lv_obj_set_size(dialog, 390, 280);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, 24, LV_PART_MAIN);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = create_label(dialog, "发现附近的原友", UI_COLOR_GOLD, 20);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    char uid_text[40];
    lv_snprintf(uid_text, sizeof(uid_text), "UID为%s", uid ? uid : "");
    lv_obj_t *uid_label = create_label(dialog, uid_text, UI_COLOR_INK, 18);
    lv_obj_align(uid_label, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *hint = create_label(dialog, "信号强度0", UI_COLOR_MUTED, 12);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
    s_nearby_rssi_label = hint;
    nearby_rssi_update(rssi);

    lv_obj_t *close_button = lv_button_create(dialog);
    lv_obj_set_size(close_button, 150, 50);
    lv_obj_align(close_button, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(close_button, UI_COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_button, UI_COLOR_GOLD, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_button, 5, LV_PART_MAIN);
    lv_obj_set_style_text_font(close_button, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_add_event_cb(close_button, nearby_alert_close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = create_label(close_button, "关闭", UI_COLOR_WHITE, 16);
    lv_obj_center(close_label);

    s_nearby_close_cb = close_cb;
    s_nearby_user_data = user_data;
}

/* ==================== 低电量提醒弹窗 ==================== */

static void battery_low_alert_close_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lvgl_ui_nearby_alert_close_cb_t callback = s_battery_low_close_cb;
    void *user_data = s_battery_low_user_data;
    lvgl_ui_settings_close_battery_low_alert();
    if (callback != NULL) {
        callback(user_data);
    }
}

void lvgl_ui_settings_close_battery_low_alert(void)
{
    if (s_battery_low_alert != NULL) {
        lv_obj_delete(s_battery_low_alert);
        s_battery_low_alert = NULL;
    }
    s_battery_low_close_cb = NULL;
    s_battery_low_user_data = NULL;
}

void lvgl_ui_settings_show_battery_low_alert(lvgl_ui_nearby_alert_close_cb_t close_cb,
                                              void *user_data)
{
    lvgl_ui_settings_close_battery_low_alert();

    lv_obj_t *screen = lv_layer_top();
    s_battery_low_alert = lv_obj_create(screen);
    lv_obj_set_size(s_battery_low_alert, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_battery_low_alert, 0, 0);
    lv_obj_set_style_bg_color(s_battery_low_alert, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_battery_low_alert, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_battery_low_alert, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_battery_low_alert, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_battery_low_alert, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog = lv_obj_create(s_battery_low_alert);
    lv_obj_set_size(dialog, 390, 280);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, UI_COLOR_GOLD, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog, 24, LV_PART_MAIN);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = create_label(dialog, "电池电量低，请充电", UI_COLOR_GOLD, 20);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -28);

    lv_obj_t *close_button = lv_button_create(dialog);
    lv_obj_set_size(close_button, 150, 50);
    lv_obj_align(close_button, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(close_button, UI_COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_button, UI_COLOR_GOLD, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_button, 5, LV_PART_MAIN);
    lv_obj_set_style_text_font(close_button, LVGL_UI_FONT, LV_PART_MAIN);
    lv_obj_add_event_cb(close_button, battery_low_alert_close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = create_label(close_button, "关闭", UI_COLOR_WHITE, 16);
    lv_obj_center(close_label);

    s_battery_low_close_cb = close_cb;
    s_battery_low_user_data = user_data;
}

/* ==================== 初始状态（NVS 恢复） ==================== */

void lvgl_ui_settings_set_initial(const lvgl_ui_settings_initial_t *init)
{
    if (init == NULL) return;
    if (init->uid != NULL) {
        lv_strlcpy(s_ui.uid, init->uid, sizeof(s_ui.uid));
    }
    s_ui.uid_visible = init->uid_visible;
    s_ui.uid_duration = init->uid_duration;
    s_ui.speaker_enabled = init->speaker_enabled;
    s_ui.speaker_volume = init->speaker_volume;
    s_ui.vibration_enabled = init->vibration_enabled;
    s_ui.vibration_intensity = init->vibration_intensity;
    s_ui.brightness = init->brightness;
    s_ui.image_auto = init->image_auto;
    s_ui.image_interval = init->image_interval;
    s_ui.video_mode = init->video_mode;
    s_ui.wifi_enabled = init->wifi_enabled;
    s_ui.clock_24h = init->clock_24h;
    s_ui.clock_auto_sync = init->clock_auto_sync;
    s_ui.encounter_count = init->encounter_count;
}

const char *lvgl_ui_settings_get_selected_wifi_ssid(void)
{
    return s_ui.wifi_selected_ssid;
}

#include "bridge_ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "lvgl_port.h"

typedef enum {
    KB_ALPHA_LOWER,
    KB_ALPHA_UPPER,
    KB_NUMERIC,
} keyboard_mode_t;

static bridge_ui_actions_t s_actions;
static lv_obj_t *s_main_page;
static lv_obj_t *s_wifi_page;
static lv_obj_t *s_wifi_pill;
static lv_obj_t *s_ble_pill;
static lv_obj_t *s_cat_pill;
static lv_obj_t *s_wifi_switch;
static lv_obj_t *s_ble_switch;
static lv_obj_t *s_ssid_field;
static lv_obj_t *s_password_field;
static lv_obj_t *s_scan_list;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_wifi_info;
static lv_obj_t *s_ble_box;
static lv_obj_t *s_wifi_box;
static lv_obj_t *s_cat_box;
static lv_obj_t *s_keyboard_target;
static keyboard_mode_t s_keyboard_mode = KB_ALPHA_LOWER;
static bool s_wifi_enabled = true;

static lv_style_t s_screen_style;
static lv_style_t s_card_style;
static lv_style_t s_button_style;
static lv_style_t s_button_active_style;

static const char *s_keys_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i",
    "o", "p", "a", "s", "d", "f", "g", "h",
    "j", "k", "l", "z", "x", "c", "v", "b",
    "n", "m", ".", "-", "_", "BS", "CLR", "ABC",
};

static const char *s_keys_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I",
    "O", "P", "A", "S", "D", "F", "G", "H",
    "J", "K", "L", "Z", "X", "C", "V", "B",
    "N", "M", ".", "-", "_", "BS", "CLR", "abc",
};

static const char *s_keys_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "0", ".", "-", "_", "@", "#", "!",
    "?", "/", ":", ";", "+", "=", "*", "$",
    "%", "&", "(", ")", "BS", "CLR", "abc", "ABC",
};

static void keyboard_fill(void);

static void set_label_text(lv_obj_t *obj, const char *text)
{
    if (obj != NULL) {
        lv_label_set_text(obj, text != NULL ? text : "");
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

static void set_pill(lv_obj_t *pill, const char *text, bool active)
{
    if (pill == NULL) {
        return;
    }
    lv_label_set_text(pill, text);
    lv_obj_set_style_bg_color(pill, lv_color_hex(active ? 0x073f46 : 0x17242a), LV_PART_MAIN);
    lv_obj_set_style_border_color(pill, lv_color_hex(active ? 0x28f0ff : 0x45616a), LV_PART_MAIN);
    lv_obj_set_style_text_color(pill, lv_color_hex(active ? 0x28f0ff : 0xb6c8ce), LV_PART_MAIN);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, const lv_font_t *font, int h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, &s_button_style, LV_PART_MAIN);
    lv_obj_add_style(btn, &s_button_active_style, LV_STATE_PRESSED);
    lv_obj_set_height(btn, h);
    lv_obj_t *label = make_label(btn, text, font, 0xf0fbff, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &s_card_style, LV_PART_MAIN);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, 5, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    make_label(card, title, &lv_font_montserrat_14, 0x8fb5c4, LV_TEXT_ALIGN_LEFT);
    return card;
}

static void show_page(lv_obj_t *page)
{
    lv_obj_add_flag(s_main_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static void open_wifi_page_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_page(s_wifi_page);
    }
}

static void back_to_main_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        show_page(s_main_page);
    }
}

static void scan_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_actions.scan != NULL) {
        s_actions.scan();
    }
}

static void connect_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_actions.connect != NULL && s_ssid_field != NULL && s_password_field != NULL) {
        s_actions.connect(lv_textarea_get_text(s_ssid_field), lv_textarea_get_text(s_password_field));
    }
}

static void disconnect_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_actions.disconnect != NULL) {
        s_actions.disconnect();
    }
}

static void switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (sw == s_wifi_switch && s_actions.wifi_enable != NULL) {
        s_actions.wifi_enable(enabled);
    } else if (sw == s_ble_switch && s_actions.ble_enable != NULL) {
        s_actions.ble_enable(enabled);
    }
}

static void field_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        s_keyboard_target = lv_event_get_target(event);
        keyboard_fill();
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view(s_keyboard_target, LV_ANIM_OFF);
    }
}

static void key_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_keyboard_target == NULL) {
        return;
    }
    const char *key = (const char *)lv_event_get_user_data(event);
    if (strcmp(key, "BS") == 0) {
        lv_textarea_delete_char(s_keyboard_target);
    } else if (strcmp(key, "CLR") == 0) {
        lv_textarea_set_text(s_keyboard_target, "");
    } else if (strcmp(key, "abc") == 0) {
        s_keyboard_mode = KB_ALPHA_LOWER;
        keyboard_fill();
    } else if (strcmp(key, "ABC") == 0) {
        s_keyboard_mode = KB_ALPHA_UPPER;
        keyboard_fill();
    } else if (strcmp(key, "123") == 0) {
        s_keyboard_mode = KB_NUMERIC;
        keyboard_fill();
    } else {
        lv_textarea_add_text(s_keyboard_target, key);
    }
}

static void keyboard_fill(void)
{
    lv_obj_clean(s_keyboard);
    const char **keys = s_keys_lower;
    if (s_keyboard_mode == KB_ALPHA_UPPER) {
        keys = s_keys_upper;
    } else if (s_keyboard_mode == KB_NUMERIC) {
        keys = s_keys_num;
    }

    for (int i = 0; i < 32; ++i) {
        lv_obj_t *key = make_button(s_keyboard, keys[i], &lv_font_montserrat_14, 34);
        lv_obj_set_size(key, 35, 34);
        lv_obj_add_event_cb(key, key_event_cb, LV_EVENT_CLICKED, (void *)keys[i]);
    }
}

static void ap_event_cb(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target(event);
    const char *line = lv_label_get_text(lv_obj_get_child(btn, 0));
    char ssid[33] = {};
    if (line != NULL) {
        const char *sep = strstr(line, " ");
        size_t len = sep != NULL ? (size_t)(sep - line) : strlen(line);
        if (len >= sizeof(ssid)) {
            len = sizeof(ssid) - 1;
        }
        memcpy(ssid, line, len);
    }
    if (ssid[0] != '\0' && s_ssid_field != NULL) {
        lv_textarea_set_text(s_ssid_field, ssid);
    }
    uint32_t count = lv_obj_get_child_count(s_scan_list);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_clear_state(lv_obj_get_child(s_scan_list, i), LV_STATE_CHECKED);
    }
    lv_obj_add_state(btn, LV_STATE_CHECKED);
}

static lv_obj_t *make_source_box(lv_obj_t *parent, const char *title)
{
    lv_obj_t *box = make_card(parent, title);
    lv_obj_set_height(box, 112);
    lv_obj_t *text = make_label(box, "-", &lv_font_montserrat_14, 0xf0fbff, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(text, LV_PCT(100));
    return box;
}

static lv_obj_t *source_text_label(lv_obj_t *box)
{
    return box != NULL && lv_obj_get_child_count(box) > 1 ? lv_obj_get_child(box, 1) : NULL;
}

static lv_obj_t *make_page(lv_obj_t *screen)
{
    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(page, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(page, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x061014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    return page;
}

static void create_status_row(lv_obj_t *parent)
{
    lv_obj_t *status = lv_obj_create(parent);
    lv_obj_remove_style_all(status);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_height(status, 36);
    lv_obj_set_style_pad_gap(status, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);

    s_wifi_pill = make_label(status, "WIFI OFF", &lv_font_montserrat_12, 0xb6c8ce, LV_TEXT_ALIGN_CENTER);
    s_ble_pill = make_label(status, "BLE", &lv_font_montserrat_12, 0xb6c8ce, LV_TEXT_ALIGN_CENTER);
    s_cat_pill = make_label(status, "CAT", &lv_font_montserrat_12, 0xb6c8ce, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *pills[] = {s_wifi_pill, s_ble_pill, s_cat_pill};
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_flex_grow(pills[i], 1);
        lv_obj_set_style_radius(pills[i], 5, LV_PART_MAIN);
        lv_obj_set_style_border_width(pills[i], 1, LV_PART_MAIN);
        lv_obj_set_style_pad_top(pills[i], 7, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pills[i], LV_OPA_COVER, LV_PART_MAIN);
    }
    lv_obj_add_flag(s_wifi_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wifi_pill, open_wifi_page_cb, LV_EVENT_CLICKED, NULL);
    set_pill(s_wifi_pill, "WIFI OFF", false);
    set_pill(s_ble_pill, "BLE ON", true);
    set_pill(s_cat_pill, "CAT ?", false);
}

static void create_main_page(lv_obj_t *screen)
{
    s_main_page = make_page(screen);
    lv_obj_t *title = make_label(s_main_page, "CAT3 BRIDGE", &lv_font_montserrat_18, 0xffffff, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, LV_PCT(100));
    create_status_row(s_main_page);

    s_ble_box = make_source_box(s_main_page, "BLE COMMAND");
    s_wifi_box = make_source_box(s_main_page, "WIFI COMMAND");
    s_cat_box = make_source_box(s_main_page, "CAT RESPONSE");

    lv_obj_t *bottom = make_card(s_main_page, "WIRELESS");
    lv_obj_set_flex_grow(bottom, 1);
    lv_obj_t *setup_btn = make_button(bottom, "WIFI SETUP", &lv_font_montserrat_16, 42);
    lv_obj_set_width(setup_btn, LV_PCT(100));
    lv_obj_add_event_cb(setup_btn, open_wifi_page_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rows[2];
    lv_obj_t **switches[] = {&s_wifi_switch, &s_ble_switch};
    const char *labels[] = {"WIFI", "BLE"};
    for (int i = 0; i < 2; ++i) {
        rows[i] = lv_obj_create(bottom);
        lv_obj_remove_style_all(rows[i]);
        lv_obj_set_width(rows[i], LV_PCT(100));
        lv_obj_set_height(rows[i], 48);
        lv_obj_set_style_pad_gap(rows[i], 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(rows[i], LV_FLEX_FLOW_ROW);
        lv_obj_t *label = make_label(rows[i], labels[i], &lv_font_montserrat_16, 0xf0fbff, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(label, 1);
        *switches[i] = lv_switch_create(rows[i]);
        lv_obj_set_size(*switches[i], 64, 34);
        lv_obj_add_state(*switches[i], LV_STATE_CHECKED);
        lv_obj_add_event_cb(*switches[i], switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void create_wifi_page(lv_obj_t *screen)
{
    s_wifi_page = make_page(screen);
    lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *top = lv_obj_create(s_wifi_page);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, 42);
    lv_obj_set_style_pad_gap(top, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_t *title = make_label(top, "WIFI SETUP", &lv_font_montserrat_18, 0xffffff, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_t *back = make_button(top, "BACK", &lv_font_montserrat_12, 34);
    lv_obj_set_width(back, 64);
    lv_obj_add_event_cb(back, back_to_main_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_status = make_card(s_wifi_page, "STATUS");
    s_wifi_info = make_label(wifi_status, "SSID: -\nIP: -", &lv_font_montserrat_14, 0xf0fbff, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_wifi_info, LV_PCT(100));

    lv_obj_t *scan_card = make_card(s_wifi_page, "SCAN");
    lv_obj_set_height(scan_card, 210);
    lv_obj_t *scan_btn = make_button(scan_card, "SCAN WIFI", &lv_font_montserrat_16, 42);
    lv_obj_set_width(scan_btn, LV_PCT(100));
    lv_obj_add_event_cb(scan_btn, scan_event_cb, LV_EVENT_CLICKED, NULL);
    s_scan_list = lv_obj_create(scan_card);
    lv_obj_set_width(s_scan_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_scan_list, 1);
    lv_obj_set_style_bg_color(s_scan_list, lv_color_hex(0x08171b), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_scan_list, lv_color_hex(0x294852), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scan_list, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_scan_list, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scan_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_scan_list, 3, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_scan_list, LV_SCROLLBAR_MODE_AUTO);
    make_label(s_scan_list, "Tap SCAN", &lv_font_montserrat_12, 0x8fb5c4, LV_TEXT_ALIGN_CENTER);

    lv_obj_t *connect_card = make_card(s_wifi_page, "CONNECT");
    s_ssid_field = lv_textarea_create(connect_card);
    lv_obj_set_width(s_ssid_field, LV_PCT(100));
    lv_obj_set_height(s_ssid_field, 46);
    lv_textarea_set_one_line(s_ssid_field, true);
    lv_textarea_set_placeholder_text(s_ssid_field, "SSID");
    lv_obj_add_event_cb(s_ssid_field, field_event_cb, LV_EVENT_ALL, NULL);

    s_password_field = lv_textarea_create(connect_card);
    lv_obj_set_width(s_password_field, LV_PCT(100));
    lv_obj_set_height(s_password_field, 46);
    lv_textarea_set_one_line(s_password_field, true);
    lv_textarea_set_password_mode(s_password_field, true);
    lv_textarea_set_placeholder_text(s_password_field, "PASSWORD");
    lv_obj_add_event_cb(s_password_field, field_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *connect_btn = make_button(connect_card, "CONNECT", &lv_font_montserrat_16, 42);
    lv_obj_set_width(connect_btn, LV_PCT(100));
    lv_obj_add_event_cb(connect_btn, connect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disconnect_btn = make_button(connect_card, "DISCONNECT", &lv_font_montserrat_16, 42);
    lv_obj_set_width(disconnect_btn, LV_PCT(100));
    lv_obj_add_event_cb(disconnect_btn, disconnect_event_cb, LV_EVENT_CLICKED, NULL);

    s_keyboard = lv_obj_create(s_wifi_page);
    lv_obj_remove_style_all(s_keyboard);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_height(s_keyboard, 314);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x0b1d22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(0x345763), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_keyboard, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_keyboard, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_keyboard, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_keyboard, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void bridge_ui_set_actions(const bridge_ui_actions_t *actions)
{
    memset(&s_actions, 0, sizeof(s_actions));
    if (actions != NULL) {
        s_actions = *actions;
    }
}

void bridge_ui_create(void)
{
    lv_style_init(&s_screen_style);
    lv_style_set_bg_color(&s_screen_style, lv_color_hex(0x061014));
    lv_style_set_bg_opa(&s_screen_style, LV_OPA_COVER);
    lv_style_set_text_color(&s_screen_style, lv_color_hex(0xf0fbff));

    lv_style_init(&s_card_style);
    lv_style_set_bg_color(&s_card_style, lv_color_hex(0x0d2228));
    lv_style_set_bg_opa(&s_card_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_card_style, lv_color_hex(0x345763));
    lv_style_set_border_width(&s_card_style, 1);
    lv_style_set_radius(&s_card_style, 6);

    lv_style_init(&s_button_style);
    lv_style_set_bg_color(&s_button_style, lv_color_hex(0x142d34));
    lv_style_set_bg_opa(&s_button_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_button_style, lv_color_hex(0x416a76));
    lv_style_set_border_width(&s_button_style, 1);
    lv_style_set_radius(&s_button_style, 6);

    lv_style_init(&s_button_active_style);
    lv_style_set_bg_color(&s_button_active_style, lv_color_hex(0x063f46));
    lv_style_set_border_color(&s_button_active_style, lv_color_hex(0x28f0ff));

    lv_obj_t *screen = lv_screen_active();
    lv_obj_remove_style_all(screen);
    lv_obj_add_style(screen, &s_screen_style, LV_PART_MAIN);

    create_main_page(screen);
    create_wifi_page(screen);
}

void bridge_ui_set_wifi_status(bool connected, const char *ssid, const char *ip)
{
    if (s_wifi_pill == NULL) {
        return;
    }
    if (!lvgl_port_lock(50)) {
        return;
    }
    char line[128] = {};
    snprintf(line, sizeof(line), "SSID: %s\nIP: %s", ssid != NULL && ssid[0] ? ssid : "-",
             ip != NULL && ip[0] ? ip : "-");
    set_label_text(s_wifi_info, line);
    set_pill(s_wifi_pill, connected ? "WIFI ON" : (s_wifi_enabled ? "WIFI OFF" : "WIFI DIS"), connected);
    lvgl_port_unlock();
}

void bridge_ui_set_cat_status(bool online)
{
    if (s_cat_pill == NULL) {
        return;
    }
    if (lvgl_port_lock(50)) {
        set_pill(s_cat_pill, online ? "CAT OK" : "CAT ?", online);
        lvgl_port_unlock();
    }
}

void bridge_ui_set_ble_status(bool online)
{
    if (s_ble_pill == NULL) {
        return;
    }
    if (lvgl_port_lock(50)) {
        set_pill(s_ble_pill, online ? "BLE ON" : "BLE OFF", online);
        lvgl_port_unlock();
    }
}

void bridge_ui_set_wifi_enabled(bool enabled)
{
    s_wifi_enabled = enabled;
    if (s_wifi_switch == NULL || !lvgl_port_lock(50)) {
        return;
    }
    if (enabled) {
        lv_obj_add_state(s_wifi_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_wifi_switch, LV_STATE_CHECKED);
        set_pill(s_wifi_pill, "WIFI DIS", false);
    }
    lvgl_port_unlock();
}

void bridge_ui_set_ble_enabled(bool enabled)
{
    if (s_ble_switch == NULL || !lvgl_port_lock(50)) {
        return;
    }
    if (enabled) {
        lv_obj_add_state(s_ble_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_ble_switch, LV_STATE_CHECKED);
        set_pill(s_ble_pill, "BLE OFF", false);
    }
    lvgl_port_unlock();
}

void bridge_ui_set_scan_running(bool running)
{
    if (s_scan_list == NULL) {
        return;
    }
    if (!lvgl_port_lock(50)) {
        return;
    }
    lv_obj_clean(s_scan_list);
    make_label(s_scan_list, running ? "Scanning..." : "No network", &lv_font_montserrat_12, 0x8fb5c4,
               LV_TEXT_ALIGN_CENTER);
    lvgl_port_unlock();
}

void bridge_ui_set_scan_results(const bridge_ui_ap_t *aps, uint8_t count)
{
    if (s_scan_list == NULL) {
        return;
    }
    if (!lvgl_port_lock(100)) {
        return;
    }
    lv_obj_clean(s_scan_list);
    if (aps == NULL || count == 0) {
        make_label(s_scan_list, "No network", &lv_font_montserrat_12, 0x8fb5c4, LV_TEXT_ALIGN_CENTER);
        lvgl_port_unlock();
        return;
    }
    for (uint8_t i = 0; i < count; ++i) {
        char line[72] = {};
        snprintf(line, sizeof(line), "%s %ddBm CH%u%s", aps[i].ssid, aps[i].rssi, aps[i].channel,
                 aps[i].secure ? "" : " OPEN");
        lv_obj_t *btn = make_button(s_scan_list, line, &lv_font_montserrat_12, 34);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_event_cb(btn, ap_event_cb, LV_EVENT_CLICKED, NULL);
    }
    lvgl_port_unlock();
}

void bridge_ui_set_source_text(const char *source, const char *text)
{
    if (s_ble_box == NULL || s_wifi_box == NULL || s_cat_box == NULL) {
        return;
    }
    if (!lvgl_port_lock(20)) {
        return;
    }
    lv_obj_t *label = NULL;
    if (source != NULL && strcmp(source, "BLE") == 0) {
        label = source_text_label(s_ble_box);
    } else if (source != NULL && strcmp(source, "WIFI") == 0) {
        label = source_text_label(s_wifi_box);
    } else if (source != NULL && strcmp(source, "CAT") == 0) {
        label = source_text_label(s_cat_box);
    }
    set_label_text(label, text != NULL && text[0] ? text : "-");
    lvgl_port_unlock();
}

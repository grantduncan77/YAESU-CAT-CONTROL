#include "app_controller.h"

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio_state.h"
#include "usb/cdc_acm_host.h"
#include "usb/cdc_acm_host_ops.h"
#include "usb/usb_host.h"
#include "usb/usb_types_cdc.h"

static const char *TAG = "ft710_controller";

typedef enum {
    CMD_SET_FREQ,
    CMD_SET_POWER,
    CMD_ADJUST_POWER,
    CMD_SET_POWER_STEP,
    CMD_SET_DNR_LEVEL,
    CMD_ADJUST_DNR,
    CMD_SET_WIDTH,
    CMD_SET_MODE,
    CMD_SELECT_MAIN_VFO,
    CMD_SET_BAND_FREQ,
} app_cmd_type_t;

typedef struct {
    app_cmd_type_t type;
    char vfo;
    uint32_t hz;
    uint8_t value;
    int8_t delta;
    ft710_mode_t mode;
} app_cmd_t;

typedef struct {
    uint8_t value;
} rx_byte_t;

typedef enum {
    SCREEN_MAIN,
    SCREEN_WIFI,
    SCREEN_WIFI_KEYBOARD,
} app_screen_t;

typedef enum {
    TIME_LOCAL,
    TIME_UTC,
} time_view_t;

typedef enum {
    WIFI_STATE_OFF,
    WIFI_STATE_READY,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_ONLINE,
    WIFI_STATE_FAILED,
} wifi_state_t;

typedef enum {
    WIFI_EDIT_SSID,
    WIFI_EDIT_PASSWORD,
} wifi_edit_field_t;

typedef enum {
    WIFI_CMD_SCAN,
    WIFI_CMD_CONNECT,
    WIFI_CMD_DISCONNECT,
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
} wifi_cmd_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t authmode;
} wifi_ap_item_t;

#define WIFI_AP_MAX 16
#define WIFI_DEFAULT_SSID "KE"
#define WIFI_DEFAULT_PASSWORD "qazwsxedc"
#define WIFI_BRIDGE_TCP_PORT 7100
#define WIFI_BRIDGE_DISCOVERY_PORT 7101
#define ENCODER_STEP_HZ 1000U
#define MCP23017_ADDR_MIN 0x20
#define MCP23017_ADDR_MAX 0x27
#define MCP23017_REG_IODIRA 0x00
#define MCP23017_REG_IODIRB 0x01
#define MCP23017_REG_GPPUA 0x0C
#define MCP23017_REG_GPPUB 0x0D
#define MCP23017_REG_GPIOA 0x12
#define MCP23017_REG_GPIOB 0x13
#define MCP23017_FREQ_ENCODER_MASK 0x07
#define MCP23017_DNR_ENCODER_MASK (BIT(3) | BIT(4) | BIT(5))
#define MCP23017_POWER_ENCODER_A_MASK (BIT(6) | BIT(7))
#define MCP23017_POWER_ENCODER_B_MASK BIT(0)
#define MCP23017_WIDTH_ENCODER_B_MASK (BIT(5) | BIT(6) | BIT(7))
#define AUX_I2C_HZ 100000
#define AUX_OLED_WIDTH 128
#define AUX_OLED_HEIGHT 64
#define AUX_OLED_PAGES (AUX_OLED_HEIGHT / 8)
#define AUX_OLED_ADDR_PRIMARY 0x3C
#define AUX_OLED_ADDR_SECONDARY 0x3D
#define TCA9548A_ADDR 0x70
#define TCA9548A_WIDTH_OLED_CHANNEL 2
#define TCA9548A_POWER_OLED_CHANNEL 3
#define TCA9548A_DNR_OLED_CHANNEL 4
#define WIDTH_INDEX_MIN 0
#define WIDTH_INDEX_MAX 23

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t frame[AUX_OLED_WIDTH * AUX_OLED_PAGES];
    uint8_t addr;
    uint8_t channel;
    bool ready;
    char last_title[12];
    char last_value[12];
} aux_oled_t;

static QueueHandle_t s_rx_queue;
static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_wifi_cmd_queue;
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_i2c_mutex;
static radio_state_t s_state;
static char s_input[18];
static char s_input_target_vfo = 'A';
static uint8_t s_power_step = 2;
static bool s_power_encoder_selecting_step;
static app_screen_t s_screen = SCREEN_MAIN;
static time_view_t s_time_view = TIME_LOCAL;
static wifi_state_t s_wifi_state = WIFI_STATE_OFF;
static esp_netif_t *s_wifi_netif;
static bool s_sntp_started;
static bool s_wifi_manual_disconnect;
static bool s_wifi_reconfig_disconnect;
static bool s_kb_upper = true;
static wifi_edit_field_t s_wifi_edit_field = WIFI_EDIT_PASSWORD;
static int s_wifi_retry;
static char s_wifi_ssid[33] = WIFI_DEFAULT_SSID;
static char s_wifi_edit_value[65] = WIFI_DEFAULT_PASSWORD;
static char s_wifi_ip[16] = "--.--.--.--";
static char s_wifi_gateway[16] = "--.--.--.--";
static char s_wifi_dns[16] = "--.--.--.--";
static wifi_ap_item_t s_wifi_aps[WIFI_AP_MAX];
static char s_wifi_ap_ids[WIFI_AP_MAX][6];
static uint16_t s_wifi_ap_count;
static bool s_wifi_bridge_task_started;
static i2c_master_dev_handle_t s_mcp23017_dev;
static i2c_master_dev_handle_t s_tca9548a_dev;
static aux_oled_t s_power_oled = {.channel = TCA9548A_POWER_OLED_CHANNEL};
static aux_oled_t s_dnr_oled = {.channel = TCA9548A_DNR_OLED_CHANNEL};
static aux_oled_t s_width_oled = {.channel = TCA9548A_WIDTH_OLED_CHANNEL};

static lv_obj_t *s_cat_status;
static lv_obj_t *s_bt_status;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_rx_status;
static lv_obj_t *s_time_label;
static lv_obj_t *s_wifi_conn_ssid_label;
static lv_obj_t *s_wifi_ip_label;
static lv_obj_t *s_wifi_gateway_label;
static lv_obj_t *s_wifi_dns_label;
static lv_obj_t *s_wifi_page_status_label;
static lv_obj_t *s_freq_a;
static lv_obj_t *s_freq_b;
static lv_obj_t *s_vfo_a_tag;
static lv_obj_t *s_vfo_b_tag;
static lv_obj_t *s_vfo_a_meta;
static lv_obj_t *s_vfo_b_meta;
static lv_obj_t *s_input_hint;
static lv_obj_t *s_power_label;
static lv_obj_t *s_power_step_btns[3];
static lv_obj_t *s_dnr_label;
static lv_obj_t *s_dnr_cmd_label;
static lv_obj_t *s_mode_btns[5];
static lv_obj_t *s_band_btns[6];

static const lv_color_t C_BG = LV_COLOR_MAKE(0x04, 0x09, 0x0B);
static const lv_color_t C_PANEL = LV_COLOR_MAKE(0x0B, 0x17, 0x1B);
static const lv_color_t C_BTN = LV_COLOR_MAKE(0x1A, 0x2B, 0x31);
static const lv_color_t C_BTN_ACTIVE = LV_COLOR_MAKE(0x0E, 0x3D, 0x45);
static const lv_color_t C_BORDER = LV_COLOR_MAKE(0x31, 0x51, 0x5B);
static const lv_color_t C_TEXT = LV_COLOR_MAKE(0xD9, 0xF2, 0xF3);
static const lv_color_t C_MUTED = LV_COLOR_MAKE(0x82, 0xA5, 0xAE);
static const lv_color_t C_CYAN = LV_COLOR_MAKE(0x35, 0xE8, 0xF2);

static void update_ui(void);
static void update_ui_locked(void);
static void create_ui(void);
static void create_wifi_ui(void);
static void create_wifi_keyboard_ui(void);

static bool rx_cb(const uint8_t *data, size_t data_len, void *arg)
{
    (void)arg;
    for (size_t i = 0; i < data_len; ++i) {
        const rx_byte_t b = {.value = data[i]};
        xQueueSend(s_rx_queue, &b, 0);
    }
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        s_state.online = false;
        ESP_LOGW(TAG, "CH9102 disconnected");
    } else if (event->type == CDC_ACM_HOST_ERROR) {
        ESP_LOGE(TAG, "CDC error: %d", event->data.error);
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void flush_rx(void)
{
    rx_byte_t b = {};
    while (xQueueReceive(s_rx_queue, &b, 0) == pdTRUE) {
    }
}

static esp_err_t read_frame(char *out, size_t out_size, TickType_t timeout)
{
    ESP_RETURN_ON_FALSE(out && out_size > 0, ESP_ERR_INVALID_ARG, TAG, "bad read buffer");
    size_t pos = 0;
    const TickType_t deadline = xTaskGetTickCount() + timeout;
    while (xTaskGetTickCount() < deadline) {
        rx_byte_t b = {};
        const TickType_t now = xTaskGetTickCount();
        const TickType_t wait = (deadline > now) ? (deadline - now) : 0;
        if (xQueueReceive(s_rx_queue, &b, wait) == pdTRUE) {
            if (pos + 1 < out_size) {
                out[pos++] = (char)b.value;
            }
            if (b.value == ';') {
                out[pos] = '\0';
                return ESP_OK;
            }
        }
    }
    out[pos] = '\0';
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cat_query(cdc_acm_dev_hdl_t dev, const char *cmd, char *resp, size_t resp_size, int64_t *elapsed_ms)
{
    const int64_t start_us = esp_timer_get_time();
    flush_rx();
    ESP_RETURN_ON_ERROR(cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)cmd, strlen(cmd), 1000), TAG, "tx failed");
    const esp_err_t err = read_frame(resp, resp_size, pdMS_TO_TICKS(500));
    if (elapsed_ms) {
        *elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    }
    return err;
}

static esp_err_t cat_send(cdc_acm_dev_hdl_t dev, const char *cmd, int64_t *elapsed_ms)
{
    const int64_t start_us = esp_timer_get_time();
    flush_rx();
    const esp_err_t err = cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)cmd, strlen(cmd), 1000);
    if (elapsed_ms) {
        *elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    }
    return err;
}

static bool parse_fixed_uint(const char *s, int start, int end, uint32_t *out)
{
    uint32_t value = 0;
    for (int i = start; i < end; ++i) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
        value = value * 10 + (uint32_t)(s[i] - '0');
    }
    *out = value;
    return true;
}

static bool parse_vfo_hz(const char *resp, const char *prefix, uint32_t *hz)
{
    return resp && prefix && strlen(resp) >= 12 && strncmp(resp, prefix, 2) == 0 && resp[11] == ';' &&
           parse_fixed_uint(resp, 2, 11, hz);
}

static bool parse_u8_3(const char *resp, const char *prefix, uint8_t *value)
{
    uint32_t parsed = 0;
    if (!resp || !prefix || strlen(resp) < 6 || strncmp(resp, prefix, 2) != 0 || !parse_fixed_uint(resp, 2, 5, &parsed)) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static bool parse_mode(const char *resp, const char *prefix, ft710_mode_t *mode)
{
    if (!resp || !prefix || strlen(resp) < 5 || strncmp(resp, prefix, 3) != 0 || resp[4] != ';') {
        return false;
    }
    const char c = (char)toupper((unsigned char)resp[3]);
    uint8_t code = 0;
    if (c >= '0' && c <= '9') {
        code = (uint8_t)(c - '0');
    } else if (c >= 'A' && c <= 'F') {
        code = (uint8_t)(10 + c - 'A');
    } else {
        return false;
    }
    *mode = radio_state_mode_from_code(code);
    return true;
}

static bool parse_bool_4(const char *resp, const char *prefix, bool *value)
{
    uint32_t parsed = 0;
    if (!resp || !prefix || strlen(resp) < 5 || strncmp(resp, prefix, 3) != 0 || resp[4] != ';' ||
        !parse_fixed_uint(resp, 3, 4, &parsed)) {
        return false;
    }
    *value = parsed != 0;
    return true;
}

static bool parse_width_index(const char *resp, uint8_t *value)
{
    uint32_t parsed = 0;
    if (!resp || strlen(resp) < 7 || strncmp(resp, "SH00", 4) != 0 || resp[6] != ';' ||
        !parse_fixed_uint(resp, 4, 6, &parsed)) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static bool parse_input_hz(uint32_t *hz)
{
    if (!s_input[0]) {
        return false;
    }
    uint32_t mhz = 0;
    uint32_t frac = 0;
    int frac_digits = 0;
    bool saw_dot = false;
    for (size_t i = 0; s_input[i]; ++i) {
        const char c = s_input[i];
        if (c == '.') {
            if (saw_dot) {
                return false;
            }
            saw_dot = true;
            continue;
        }
        if (!isdigit((unsigned char)c)) {
            return false;
        }
        if (saw_dot) {
            if (frac_digits < 6) {
                frac = frac * 10 + (uint32_t)(c - '0');
                frac_digits++;
            }
        } else {
            mhz = mhz * 10 + (uint32_t)(c - '0');
        }
    }
    while (frac_digits < 6) {
        frac *= 10;
        frac_digits++;
    }
    const uint32_t value = mhz * 1000000U + frac;
    if (value == 0 || value > 999999999U) {
        return false;
    }
    *hz = value;
    return true;
}

static void fmt_freq(uint32_t hz, char *out, size_t out_size)
{
    snprintf(out, out_size, "%02lu.%03lu.%03lu", (unsigned long)(hz / 1000000U),
             (unsigned long)((hz / 1000U) % 1000U), (unsigned long)(hz % 1000U));
}

static const char *wifi_state_text(void)
{
    switch (s_wifi_state) {
    case WIFI_STATE_READY:
        return "WIFI READY";
    case WIFI_STATE_SCANNING:
        return "WIFI SCAN";
    case WIFI_STATE_CONNECTING:
        return "WIFI JOIN";
    case WIFI_STATE_ONLINE:
        return "WIFI ON";
    case WIFI_STATE_FAILED:
        return "WIFI FAIL";
    case WIFI_STATE_OFF:
    default:
        return "WIFI OFF";
    }
}

static const char *wifi_page_status_text(void)
{
    switch (s_wifi_state) {
    case WIFI_STATE_SCANNING:
        return "SCANNING";
    case WIFI_STATE_CONNECTING:
        return "JOINING";
    case WIFI_STATE_ONLINE:
        return "ONLINE";
    case WIFI_STATE_FAILED:
        return "FAILED";
    case WIFI_STATE_READY:
        return "READY";
    case WIFI_STATE_OFF:
    default:
        return "OFFLINE";
    }
}

static const char *wifi_auth_text(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "--";
    }
}

static void send_wifi_cmd(wifi_cmd_type_t type)
{
    if (!s_wifi_cmd_queue) {
        return;
    }
    const wifi_cmd_t cmd = {.type = type};
    xQueueSend(s_wifi_cmd_queue, &cmd, 0);
}

static char *wifi_edit_target(size_t *size)
{
    if (s_wifi_edit_field == WIFI_EDIT_SSID) {
        if (size) {
            *size = sizeof(s_wifi_ssid);
        }
        return s_wifi_ssid;
    }
    if (size) {
        *size = sizeof(s_wifi_edit_value);
    }
    return s_wifi_edit_value;
}

static void password_mask(char *out, size_t out_size)
{
    size_t len = strlen(s_wifi_edit_value);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memset(out, '*', len);
    out[len] = '\0';
    if (!len) {
        strlcpy(out, "--", out_size);
    }
}

static void clear_wifi_addrs(void)
{
    strlcpy(s_wifi_ip, "--.--.--.--", sizeof(s_wifi_ip));
    strlcpy(s_wifi_gateway, "--.--.--.--", sizeof(s_wifi_gateway));
    strlcpy(s_wifi_dns, "--.--.--.--", sizeof(s_wifi_dns));
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(obj, 1, LV_PART_MAIN);
    return obj;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void set_button_active(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_color(btn, active ? C_BTN_ACTIVE : C_BTN, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, active ? C_CYAN : C_BORDER, LV_PART_MAIN);
    lv_obj_t *child = lv_obj_get_child(btn, 0);
    if (child) {
        lv_obj_set_style_text_color(child, active ? C_CYAN : C_MUTED, LV_PART_MAIN);
    }
}

static void send_cmd(const app_cmd_t *cmd)
{
    xQueueSend(s_cmd_queue, cmd, 0);
}

static void update_input_hint_locked(void)
{
    if (!s_input_hint) {
        return;
    }
    char buf[40] = {};
    snprintf(buf, sizeof(buf), "%c  %s", s_input_target_vfo, s_input[0] ? s_input : "--.---");
    lv_label_set_text(s_input_hint, buf);
}

static uint32_t selected_input_vfo_hz(void)
{
    return s_input_target_vfo == 'B' ? s_state.vfo_b_hz : s_state.vfo_a_hz;
}

static void set_input_from_hz(uint32_t hz)
{
    snprintf(s_input, sizeof(s_input), "%lu.%06lu", (unsigned long)(hz / 1000000U),
             (unsigned long)(hz % 1000000U));
}

static bool i2c_take(TickType_t timeout)
{
    return !s_i2c_mutex || xSemaphoreTake(s_i2c_mutex, timeout) == pdTRUE;
}

static void i2c_give(void)
{
    if (s_i2c_mutex) {
        xSemaphoreGive(s_i2c_mutex);
    }
}

static void i2c_scan_range(i2c_master_bus_handle_t bus, const char *label, uint8_t first, uint8_t last)
{
    char found[192] = {};
    size_t used = 0;
    uint8_t count = 0;
    for (uint8_t addr = first; addr <= last; ++addr) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            int written = snprintf(found + used, sizeof(found) - used, "%s0x%02X", count ? " " : "", addr);
            if (written > 0) {
                used += (size_t)written;
                if (used >= sizeof(found)) {
                    used = sizeof(found) - 1;
                }
            }
            count++;
        }
    }
    ESP_LOGI(TAG, "I2C scan %s: %s", label, count ? found : "no devices");
}

static void i2c_diagnostic_scan(void)
{
    ESP_LOGI(TAG, "I2C diagnostic: BSP bus SCL=GPIO%d SDA=GPIO%d", BSP_I2C_SCL, BSP_I2C_SDA);
    ESP_LOGI(TAG, "I2C diagnostic: line levels before scan SDA=%d SCL=%d", gpio_get_level(BSP_I2C_SDA),
             gpio_get_level(BSP_I2C_SCL));

    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C diagnostic: bus init failed: %s", esp_err_to_name(ret));
        return;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "I2C diagnostic: BSP I2C handle is null");
        return;
    }
    if (!i2c_take(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "I2C diagnostic: bus mutex timeout");
        return;
    }

    i2c_scan_range(bus, "main bus", 0x03, 0x77);

    if (i2c_master_probe(bus, TCA9548A_ADDR, 20) == ESP_OK) {
        i2c_master_dev_handle_t mux = NULL;
        const i2c_device_config_t mux_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TCA9548A_ADDR,
            .scl_speed_hz = AUX_I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &mux_cfg, &mux) == ESP_OK) {
            for (uint8_t ch = 0; ch < 8; ++ch) {
                const uint8_t mask = BIT(ch);
                char label[24] = {};
                snprintf(label, sizeof(label), "TCA ch%u", ch);
                if (i2c_master_transmit(mux, &mask, 1, 100) == ESP_OK) {
                    i2c_scan_range(bus, label, 0x03, 0x77);
                } else {
                    ESP_LOGW(TAG, "I2C scan %s: mux select failed", label);
                }
            }
            const uint8_t off = 0;
            i2c_master_transmit(mux, &off, 1, 100);
            i2c_master_bus_rm_device(mux);
        } else {
            ESP_LOGW(TAG, "I2C diagnostic: failed to add TCA9548A temporary device");
        }
    }

    ESP_LOGI(TAG, "I2C diagnostic: line levels after scan SDA=%d SCL=%d", gpio_get_level(BSP_I2C_SDA),
             gpio_get_level(BSP_I2C_SCL));
    i2c_give();
}

static esp_err_t aux_oled_select(aux_oled_t *oled)
{
    if (!s_tca9548a_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t mask = BIT(oled->channel);
    return i2c_master_transmit(s_tca9548a_dev, &mask, 1, 1000);
}

static esp_err_t aux_oled_cmd(aux_oled_t *oled, uint8_t cmd)
{
    const uint8_t data[2] = {0x00, cmd};
    ESP_RETURN_ON_ERROR(aux_oled_select(oled), TAG, "select aux OLED channel");
    return i2c_master_transmit(oled->dev, data, sizeof(data), 1000);
}

static esp_err_t aux_oled_data(aux_oled_t *oled, const uint8_t *data, size_t len)
{
    uint8_t line[1 + AUX_OLED_WIDTH] = {0x40};
    while (len) {
        const size_t chunk = len > AUX_OLED_WIDTH ? AUX_OLED_WIDTH : len;
        memcpy(&line[1], data, chunk);
        ESP_RETURN_ON_ERROR(aux_oled_select(oled), TAG, "select aux OLED channel");
        ESP_RETURN_ON_ERROR(i2c_master_transmit(oled->dev, line, chunk + 1, 1000), TAG, "aux OLED data");
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t aux_oled_flush(aux_oled_t *oled)
{
    for (uint8_t page = 0; page < AUX_OLED_PAGES; ++page) {
        ESP_RETURN_ON_ERROR(aux_oled_cmd(oled, 0xB0 | page), TAG, "aux OLED page");
        ESP_RETURN_ON_ERROR(aux_oled_cmd(oled, 0x00), TAG, "aux OLED low col");
        ESP_RETURN_ON_ERROR(aux_oled_cmd(oled, 0x10), TAG, "aux OLED high col");
        ESP_RETURN_ON_ERROR(aux_oled_data(oled, &oled->frame[page * AUX_OLED_WIDTH], AUX_OLED_WIDTH), TAG,
                            "aux OLED flush");
    }
    return ESP_OK;
}

static esp_err_t aux_oled_ssd1306_init(aux_oled_t *oled)
{
    const uint8_t init[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40, 0x81, 0x7F, 0xA1, 0xA6,
        0xA8, 0x3F, 0xA4, 0xD3, 0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF,
    };
    for (size_t i = 0; i < sizeof(init); ++i) {
        ESP_RETURN_ON_ERROR(aux_oled_cmd(oled, init[i]), TAG, "aux OLED init");
    }
    memset(oled->frame, 0, sizeof(oled->frame));
    return aux_oled_flush(oled);
}

static void aux_oled_pixel(aux_oled_t *oled, int x, int y, bool on)
{
    if (x < 0 || x >= AUX_OLED_WIDTH || y < 0 || y >= AUX_OLED_HEIGHT) {
        return;
    }
    uint8_t *byte = &oled->frame[(y / 8) * AUX_OLED_WIDTH + x];
    const uint8_t bit = BIT(y % 8);
    if (on) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
}

static void aux_oled_rect(aux_oled_t *oled, int x, int y, int w, int h, bool on)
{
    for (int i = 0; i < w; ++i) {
        aux_oled_pixel(oled, x + i, y, on);
        aux_oled_pixel(oled, x + i, y + h - 1, on);
    }
    for (int i = 0; i < h; ++i) {
        aux_oled_pixel(oled, x, y + i, on);
        aux_oled_pixel(oled, x + w - 1, y + i, on);
    }
}

static void aux_oled_fill_rect(aux_oled_t *oled, int x, int y, int w, int h, bool on)
{
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            aux_oled_pixel(oled, x + xx, y + yy, on);
        }
    }
}

static void aux_oled_line(aux_oled_t *oled, int x0, int y0, int x1, int y1, bool on)
{
    const int dx = abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        aux_oled_pixel(oled, x0, y0, on);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static const uint8_t *aux_oled_glyph(char c)
{
    static const uint8_t space[5] = {0, 0, 0, 0, 0};
    static const uint8_t minus[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0, 0, 0x60, 0x60, 0};
    static const uint8_t colon[5] = {0, 0x36, 0x36, 0, 0};
    static const uint8_t digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    };

    if (c == ' ') return space;
    if (c == '-') return minus;
    if (c == '.') return dot;
    if (c == ':') return colon;
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    return space;
}

static void aux_oled_char(aux_oled_t *oled, int x, int y, char c, int scale)
{
    const uint8_t *g = aux_oled_glyph(c);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (g[col] & BIT(row)) {
                for (int sx = 0; sx < scale; ++sx) {
                    for (int sy = 0; sy < scale; ++sy) {
                        aux_oled_pixel(oled, x + col * scale + sx, y + row * scale + sy, true);
                    }
                }
            }
        }
    }
}

static void aux_oled_text(aux_oled_t *oled, int x, int y, const char *text, int scale)
{
    while (*text) {
        aux_oled_char(oled, x, y, *text++, scale);
        x += 6 * scale;
    }
}

static void aux_oled_draw_status(aux_oled_t *oled, const char *title, const char *value)
{
    memset(oled->frame, 0, sizeof(oled->frame));
    aux_oled_rect(oled, 0, 0, AUX_OLED_WIDTH, AUX_OLED_HEIGHT, true);
    aux_oled_text(oled, 6, 4, title, 1);

    const int scale = strlen(value) > 3 ? 4 : 5;
    const int width = (int)strlen(value) * 6 * scale - scale;
    aux_oled_text(oled, (AUX_OLED_WIDTH - width) / 2, 20, value, scale);
}

static void aux_oled_show_status(aux_oled_t *oled, const char *title, const char *value, bool force)
{
    if (!oled->ready ||
        (!force && strcmp(oled->last_title, title) == 0 && strcmp(oled->last_value, value) == 0) ||
        !i2c_take(pdMS_TO_TICKS(50))) {
        return;
    }
    aux_oled_draw_status(oled, title, value);
    const esp_err_t err = aux_oled_flush(oled);
    if (err == ESP_OK) {
        strlcpy(oled->last_title, title, sizeof(oled->last_title));
        strlcpy(oled->last_value, value, sizeof(oled->last_value));
    } else {
        ESP_LOGW(TAG, "Aux OLED update failed: %s", esp_err_to_name(err));
    }
    i2c_give();
}

static void aux_oled_show_power(uint8_t power_w, bool force)
{
    char value[8] = {};
    snprintf(value, sizeof(value), "%uW", power_w);
    if (!s_power_oled.ready ||
        (!force && strcmp(s_power_oled.last_title, "RF POWER") == 0 && strcmp(s_power_oled.last_value, value) == 0) ||
        !i2c_take(pdMS_TO_TICKS(50))) {
        return;
    }

    memset(s_power_oled.frame, 0, sizeof(s_power_oled.frame));
    aux_oled_rect(&s_power_oled, 0, 0, AUX_OLED_WIDTH, AUX_OLED_HEIGHT, true);
    aux_oled_text(&s_power_oled, 6, 4, "RF POWER", 1);

    const int scale = 4;
    const int text_width = (int)strlen(value) * 6 * scale - scale;
    aux_oled_text(&s_power_oled, (AUX_OLED_WIDTH - text_width) / 2, 16, value, scale);

    const int bar_x = 8;
    const int bar_y = 55;
    const int bar_w = AUX_OLED_WIDTH - bar_x * 2;
    const int bar_h = 6;
    uint8_t clamped = power_w;
    if (clamped < 5) {
        clamped = 5;
    } else if (clamped > 100) {
        clamped = 100;
    }
    const int fill_w = (clamped - 5) * (bar_w - 2) / 95;
    aux_oled_rect(&s_power_oled, bar_x, bar_y, bar_w, bar_h, true);
    if (fill_w > 0) {
        aux_oled_fill_rect(&s_power_oled, bar_x + 1, bar_y + 1, fill_w, bar_h - 2, true);
    }

    const esp_err_t err = aux_oled_flush(&s_power_oled);
    if (err == ESP_OK) {
        strlcpy(s_power_oled.last_title, "RF POWER", sizeof(s_power_oled.last_title));
        strlcpy(s_power_oled.last_value, value, sizeof(s_power_oled.last_value));
    } else {
        ESP_LOGW(TAG, "Power OLED update failed: %s", esp_err_to_name(err));
    }
    i2c_give();
}

static void aux_oled_show_power_step(uint8_t step_w, bool force)
{
    char value[8] = {};
    snprintf(value, sizeof(value), "%uW", step_w);
    aux_oled_show_status(&s_power_oled, "RF STEP", value, force);
}

static void aux_oled_show_dnr(bool force)
{
    char value[8] = {};
    if (s_state.dnr_on) {
        snprintf(value, sizeof(value), "%02u", s_state.dnr_level);
    } else {
        snprintf(value, sizeof(value), "OFF");
    }
    aux_oled_show_status(&s_dnr_oled, "DNR", value, force);
}

static ft710_mode_t current_width_mode(void)
{
    return s_state.active_vfo == 'B' ? s_state.mode_b : s_state.mode_a;
}

static int width_index_to_hz(ft710_mode_t mode, uint8_t width_index)
{
    static const uint16_t ssb_width_hz[WIDTH_INDEX_MAX + 1] = {
        0, 300, 400, 600, 850, 1100, 1200, 1500,
        1650, 1800, 1950, 2100, 2250, 2400, 2450, 2500,
        2600, 2700, 2800, 2900, 3000, 3200, 3500, 4000,
    };
    static const uint16_t data_width_hz[WIDTH_INDEX_MAX + 1] = {
        0, 50, 100, 150, 200, 250, 300, 350,
        400, 450, 500, 600, 800, 1200, 1400, 1700,
        2000, 2400, 3000, 3200, 3500, 4000, 0, 0,
    };

    if (width_index == 0 || width_index > WIDTH_INDEX_MAX) {
        return 0;
    }

    switch (mode) {
    case FT710_MODE_LSB:
    case FT710_MODE_USB:
        return ssb_width_hz[width_index];
    case FT710_MODE_DATA_U:
        return data_width_hz[width_index];
    case FT710_MODE_AM:
        if (width_index == 1) return 6000;
        if (width_index == 2) return 9000;
        return -1;
    case FT710_MODE_FM:
        if (width_index == 3) return 16000;
        return -1;
    default:
        return -1;
    }
}

static void aux_oled_draw_width(aux_oled_t *oled, uint8_t width_index)
{
    memset(oled->frame, 0, sizeof(oled->frame));
    aux_oled_rect(oled, 0, 0, AUX_OLED_WIDTH, AUX_OLED_HEIGHT, true);
    aux_oled_text(oled, 6, 1, "WIDTH", 1);

    const int span = WIDTH_INDEX_MAX - WIDTH_INDEX_MIN;
    const int value = width_index > WIDTH_INDEX_MAX ? WIDTH_INDEX_MAX : width_index;
    const int top_len = 14 + (value - WIDTH_INDEX_MIN) * 78 / span;
    const int bottom_len = 28 + (value - WIDTH_INDEX_MIN) * 92 / span;
    const int cx = AUX_OLED_WIDTH / 2;
    const int y_top = 12;
    const int y_bottom = 39;
    const int x_top_l = cx - top_len / 2;
    const int x_top_r = cx + top_len / 2;
    const int x_bot_l = cx - bottom_len / 2;
    const int x_bot_r = cx + bottom_len / 2;

    const int fill_lines = 7;
    for (int i = 0; i < fill_lines; ++i) {
        const int b = x_bot_l + i * bottom_len / (fill_lines - 1);
        const int t_forward = x_top_l + ((i + 2) < fill_lines ? (i + 2) : (fill_lines - 1)) * top_len / (fill_lines - 1);
        const int t_back = x_top_l + ((i - 2) > 0 ? (i - 2) : 0) * top_len / (fill_lines - 1);
        aux_oled_line(oled, b, y_bottom - 1, t_forward, y_top + 1, true);
        aux_oled_line(oled, b, y_bottom - 1, t_back, y_top + 1, true);
    }

    aux_oled_line(oled, x_top_l, y_top, x_top_r, y_top, true);
    aux_oled_line(oled, 4, y_bottom, AUX_OLED_WIDTH - 5, y_bottom, true);
    aux_oled_line(oled, x_top_l, y_top, x_bot_l, y_bottom, true);
    aux_oled_line(oled, x_top_r, y_top, x_bot_r, y_bottom, true);

    char value_text[8] = {};
    const int hz = width_index_to_hz(current_width_mode(), width_index);
    if (width_index == 0) {
        snprintf(value_text, sizeof(value_text), "DEF");
    } else if (hz > 0) {
        snprintf(value_text, sizeof(value_text), "%d", hz);
    } else {
        snprintf(value_text, sizeof(value_text), "---");
    }
    const int scale = 2;
    const int text_width = (int)strlen(value_text) * 6 * scale - scale;
    aux_oled_text(oled, (AUX_OLED_WIDTH - text_width) / 2, 47, value_text, scale);
}

static void aux_oled_show_width(bool force)
{
    char value[12] = {};
    snprintf(value, sizeof(value), "%c:%u:%02u", s_state.active_vfo, (unsigned)current_width_mode(), s_state.width_index);
    if (!s_width_oled.ready ||
        (!force && strcmp(s_width_oled.last_title, "WIDTH") == 0 && strcmp(s_width_oled.last_value, value) == 0) ||
        !i2c_take(pdMS_TO_TICKS(50))) {
        return;
    }
    aux_oled_draw_width(&s_width_oled, s_state.width_index);
    const esp_err_t err = aux_oled_flush(&s_width_oled);
    if (err == ESP_OK) {
        strlcpy(s_width_oled.last_title, "WIDTH", sizeof(s_width_oled.last_title));
        strlcpy(s_width_oled.last_value, value, sizeof(s_width_oled.last_value));
    } else {
        ESP_LOGW(TAG, "Width OLED update failed: %s", esp_err_to_name(err));
    }
    i2c_give();
}

static void aux_oled_show_current(bool force)
{
    aux_oled_show_width(force);
    if (s_power_encoder_selecting_step) {
        aux_oled_show_power_step(s_power_step, force);
    } else {
        aux_oled_show_power(s_state.power_w, force);
    }
    aux_oled_show_dnr(force);
}

static esp_err_t aux_oled_init_one(i2c_master_bus_handle_t bus, aux_oled_t *oled)
{
    ESP_RETURN_ON_ERROR(aux_oled_select(oled), TAG, "select aux OLED channel");

    uint8_t addr = 0;
    if (i2c_master_probe(bus, AUX_OLED_ADDR_PRIMARY, 100) == ESP_OK) {
        addr = AUX_OLED_ADDR_PRIMARY;
    } else if (i2c_master_probe(bus, AUX_OLED_ADDR_SECONDARY, 100) == ESP_OK) {
        addr = AUX_OLED_ADDR_SECONDARY;
    }
    if (addr == 0) {
        ESP_LOGW(TAG, "SSD1306 not found on TCA9548A channel %d", oled->channel);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t oled_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = AUX_I2C_HZ,
    };
    if (s_width_oled.ready && s_width_oled.addr == addr && s_width_oled.dev) {
        oled->dev = s_width_oled.dev;
    } else if (s_power_oled.ready && s_power_oled.addr == addr && s_power_oled.dev) {
        oled->dev = s_power_oled.dev;
    } else if (s_dnr_oled.ready && s_dnr_oled.addr == addr && s_dnr_oled.dev) {
        oled->dev = s_dnr_oled.dev;
    } else {
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &oled_cfg, &oled->dev), TAG, "add aux OLED");
    }
    ESP_RETURN_ON_ERROR(aux_oled_ssd1306_init(oled), TAG, "init aux OLED");

    oled->addr = addr;
    oled->ready = true;
    ESP_LOGI(TAG, "Aux OLED ready: TCA9548A=0x%02X channel=%d SSD1306=0x%02X", TCA9548A_ADDR, oled->channel,
             oled->addr);
    return ESP_OK;
}

static esp_err_t aux_oled_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "init BSP I2C for aux OLED");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus, ESP_FAIL, TAG, "BSP I2C handle is null");

    if (!i2c_take(pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = i2c_master_probe(bus, TCA9548A_ADDR, 100);
    if (ret != ESP_OK) {
        i2c_give();
        return ret;
    }
    const i2c_device_config_t tca_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9548A_ADDR,
        .scl_speed_hz = AUX_I2C_HZ,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(bus, &tca_cfg, &s_tca9548a_dev), cleanup, TAG, "add TCA9548A");
    ret = aux_oled_init_one(bus, &s_width_oled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Width OLED disabled: %s", esp_err_to_name(ret));
    }
    ret = aux_oled_init_one(bus, &s_power_oled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Power OLED disabled: %s", esp_err_to_name(ret));
    }
    ret = aux_oled_init_one(bus, &s_dnr_oled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DNR OLED disabled: %s", esp_err_to_name(ret));
    }
    ret = (s_width_oled.ready || s_power_oled.ready || s_dnr_oled.ready) ? ESP_OK : ESP_ERR_NOT_FOUND;

cleanup:
    i2c_give();
    return ret;
}

static bool submit_input_frequency(void)
{
    uint32_t hz = 0;
    if (!parse_input_hz(&hz)) {
        return false;
    }

    const app_cmd_t cmd = {
        .type = CMD_SET_FREQ,
        .vfo = s_input_target_vfo,
        .hz = hz,
    };
    send_cmd(&cmd);
    s_input[0] = '\0';
    return true;
}

static void encoder_adjust_input(int delta)
{
    if (s_screen != SCREEN_MAIN || delta == 0) {
        return;
    }

    uint32_t hz = 0;
    if (!parse_input_hz(&hz)) {
        hz = selected_input_vfo_hz();
    }

    int64_t next = (int64_t)hz + (int64_t)delta * ENCODER_STEP_HZ;
    if (next < 1000) {
        next = 1000;
    } else if (next > 999999999) {
        next = 999999999;
    }
    set_input_from_hz((uint32_t)next);

    if (bsp_display_lock(pdMS_TO_TICKS(50))) {
        update_input_hint_locked();
        bsp_display_unlock();
    }
}

static int power_step_index(uint8_t step)
{
    const uint8_t steps[] = {2, 5, 10};
    for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
        if (step == steps[i]) {
            return i;
        }
    }
    return 0;
}

static void encoder_adjust_power_step(int delta)
{
    if (delta == 0) {
        return;
    }

    const uint8_t steps[] = {2, 5, 10};
    int index = power_step_index(s_power_step) + delta;
    if (index < 0) {
        index = 0;
    } else if (index >= (int)(sizeof(steps) / sizeof(steps[0]))) {
        index = (int)(sizeof(steps) / sizeof(steps[0])) - 1;
    }
    if (steps[index] == s_power_step) {
        return;
    }

    s_power_step = steps[index];
    ESP_LOGI(TAG, "Power encoder step select %uW", s_power_step);
    aux_oled_show_current(true);
    if (s_screen == SCREEN_MAIN && bsp_display_lock(pdMS_TO_TICKS(50))) {
        update_ui_locked();
        bsp_display_unlock();
    }
}

static void encoder_adjust_power(int delta)
{
    if (delta == 0) {
        return;
    }

    int p = (int)s_state.power_w + delta * (int)s_power_step;
    if (p < 5) {
        p = 5;
    } else if (p > 100) {
        p = 100;
    }
    if (p == (int)s_state.power_w) {
        return;
    }

    s_state.power_w = (uint8_t)p;
    ESP_LOGI(TAG, "Power encoder set %uW step=%uW", s_state.power_w, s_power_step);
    aux_oled_show_current(true);
    const app_cmd_t cmd = {
        .type = CMD_SET_POWER,
        .value = (uint8_t)p,
    };
    send_cmd(&cmd);

    if (s_screen == SCREEN_MAIN && bsp_display_lock(pdMS_TO_TICKS(50))) {
        update_ui_locked();
        bsp_display_unlock();
    }
}

static void encoder_adjust_dnr(int delta)
{
    if (delta == 0) {
        return;
    }

    const int old_level = s_state.dnr_on ? s_state.dnr_level : 0;
    int level = old_level + delta;
    if (level < 0) {
        level = 0;
    } else if (level > 15) {
        level = 15;
    }
    if (level == old_level) {
        return;
    }

    s_state.dnr_level = (uint8_t)level;
    s_state.dnr_on = level > 0;
    ESP_LOGI(TAG, "DNR encoder set %s level=%u", s_state.dnr_on ? "ON" : "OFF", s_state.dnr_level);
    aux_oled_show_dnr(true);
    const app_cmd_t cmd = {
        .type = CMD_SET_DNR_LEVEL,
        .value = (uint8_t)level,
    };
    send_cmd(&cmd);

    if (s_screen == SCREEN_MAIN && bsp_display_lock(pdMS_TO_TICKS(50))) {
        update_ui_locked();
        bsp_display_unlock();
    }
}

static void encoder_dnr_off(void)
{
    s_state.dnr_on = false;
    s_state.dnr_level = 0;
    ESP_LOGI(TAG, "DNR encoder button OFF");
    aux_oled_show_dnr(true);
    const app_cmd_t cmd = {
        .type = CMD_SET_DNR_LEVEL,
        .value = 0,
    };
    send_cmd(&cmd);

    if (s_screen == SCREEN_MAIN && bsp_display_lock(pdMS_TO_TICKS(50))) {
        update_ui_locked();
        bsp_display_unlock();
    }
}

static ft710_mode_t active_vfo_mode(void)
{
    return current_width_mode();
}

static uint8_t width_default_for_mode(ft710_mode_t mode)
{
    (void)mode;
    return 0;
}

static void encoder_set_width(uint8_t width_index, bool is_default)
{
    if (width_index > WIDTH_INDEX_MAX) {
        width_index = WIDTH_INDEX_MAX;
    }
    s_state.width_index = width_index;
    ESP_LOGI(TAG, "Width encoder set %02u%s", s_state.width_index, is_default ? " default" : "");
    aux_oled_show_width(true);

    const app_cmd_t cmd = {
        .type = CMD_SET_WIDTH,
        .value = width_index,
    };
    send_cmd(&cmd);
}

static void encoder_adjust_width(int delta)
{
    if (delta == 0) {
        return;
    }

    int width = (int)s_state.width_index + delta;
    if (width < WIDTH_INDEX_MIN) {
        width = WIDTH_INDEX_MIN;
    } else if (width > WIDTH_INDEX_MAX) {
        width = WIDTH_INDEX_MAX;
    }
    if (width == (int)s_state.width_index) {
        return;
    }
    encoder_set_width((uint8_t)width, false);
}

static void encoder_width_default(void)
{
    encoder_set_width(width_default_for_mode(active_vfo_mode()), true);
}

static esp_err_t mcp23017_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    if (!i2c_take(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = i2c_master_transmit(s_mcp23017_dev, data, sizeof(data), 50);
    i2c_give();
    return err;
}

static esp_err_t mcp23017_read_reg(uint8_t reg, uint8_t *value)
{
    if (!i2c_take(pdMS_TO_TICKS(50))) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = i2c_master_transmit_receive(s_mcp23017_dev, &reg, 1, value, 1, 50);
    i2c_give();
    return err;
}

static esp_err_t mcp23017_write_reg_retry(uint8_t reg, uint8_t value, const char *name)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = mcp23017_write_reg(reg, value);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "MCP23017 write %s reg=0x%02X value=0x%02X failed attempt %d: %s", name, reg, value, attempt,
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err;
}

static esp_err_t encoder_mcp23017_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "init BSP I2C");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus, ESP_FAIL, TAG, "BSP I2C handle is null");

    const uint8_t candidates[] = {0x27, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
    esp_err_t last_err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(candidates); ++i) {
        const uint8_t candidate = candidates[i];
        if (i2c_master_probe(bus, candidate, 100) == ESP_OK) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = candidate,
                .scl_speed_hz = AUX_I2C_HZ,
            };
            last_err = i2c_master_bus_add_device(bus, &dev_cfg, &s_mcp23017_dev);
            if (last_err != ESP_OK) {
                ESP_LOGW(TAG, "MCP23017 add candidate 0x%02X failed: %s", candidate, esp_err_to_name(last_err));
                s_mcp23017_dev = NULL;
                continue;
            }

            ESP_LOGI(TAG, "MCP23017 candidate detected at 0x%02X, configuring at %d Hz", candidate, AUX_I2C_HZ);

            uint8_t iodira = 0xFF;
            if (mcp23017_read_reg(MCP23017_REG_IODIRA, &iodira) != ESP_OK) {
                iodira = 0xFF;
            }
            last_err = mcp23017_write_reg_retry(MCP23017_REG_IODIRA,
                                                iodira | MCP23017_FREQ_ENCODER_MASK |
                                                    MCP23017_DNR_ENCODER_MASK | MCP23017_POWER_ENCODER_A_MASK,
                                                "IODIRA");
            if (last_err != ESP_OK) {
                ESP_LOGW(TAG, "MCP23017 candidate 0x%02X rejected at IODIRA: %s", candidate,
                         esp_err_to_name(last_err));
                i2c_master_bus_rm_device(s_mcp23017_dev);
                s_mcp23017_dev = NULL;
                continue;
            }

            uint8_t gppua = 0;
            if (mcp23017_read_reg(MCP23017_REG_GPPUA, &gppua) != ESP_OK) {
                gppua = 0;
            }
            last_err = mcp23017_write_reg_retry(MCP23017_REG_GPPUA,
                                                gppua | MCP23017_FREQ_ENCODER_MASK |
                                                    MCP23017_DNR_ENCODER_MASK | MCP23017_POWER_ENCODER_A_MASK,
                                                "GPPUA");
            if (last_err != ESP_OK) {
                ESP_LOGW(TAG, "MCP23017 candidate 0x%02X rejected at GPPUA: %s", candidate,
                         esp_err_to_name(last_err));
                i2c_master_bus_rm_device(s_mcp23017_dev);
                s_mcp23017_dev = NULL;
                continue;
            }

            uint8_t iodirb = 0xFF;
            if (mcp23017_read_reg(MCP23017_REG_IODIRB, &iodirb) != ESP_OK) {
                iodirb = 0xFF;
            }
            last_err = mcp23017_write_reg_retry(MCP23017_REG_IODIRB,
                                                iodirb | MCP23017_POWER_ENCODER_B_MASK |
                                                    MCP23017_WIDTH_ENCODER_B_MASK,
                                                "IODIRB");
            if (last_err != ESP_OK) {
                ESP_LOGW(TAG, "MCP23017 candidate 0x%02X rejected at IODIRB: %s", candidate,
                         esp_err_to_name(last_err));
                i2c_master_bus_rm_device(s_mcp23017_dev);
                s_mcp23017_dev = NULL;
                continue;
            }

            uint8_t gppub = 0;
            if (mcp23017_read_reg(MCP23017_REG_GPPUB, &gppub) != ESP_OK) {
                gppub = 0;
            }
            last_err = mcp23017_write_reg_retry(MCP23017_REG_GPPUB,
                                                gppub | MCP23017_POWER_ENCODER_B_MASK |
                                                    MCP23017_WIDTH_ENCODER_B_MASK,
                                                "GPPUB");
            if (last_err != ESP_OK) {
                ESP_LOGW(TAG, "MCP23017 candidate 0x%02X rejected at GPPUB: %s", candidate,
                         esp_err_to_name(last_err));
                i2c_master_bus_rm_device(s_mcp23017_dev);
                s_mcp23017_dev = NULL;
                continue;
            }

            ESP_LOGI(TAG,
                     "MCP23017 encoder detected at 0x%02X, freq A=PA0 B=PA1 S=PA2 step=%lu Hz, dnr A=PA3 B=PA4 S=PA5, power A=PA6 B=PA7 S=PB0 step=%d W, width A=PB7 B=PB6 S=PB5",
                     candidate, (unsigned long)ENCODER_STEP_HZ, s_power_step);
            return ESP_OK;
        }
    }

    ESP_RETURN_ON_ERROR(last_err, TAG, "configure MCP23017");
    return ESP_ERR_NOT_FOUND;
}

static void encoder_task(void *arg)
{
    (void)arg;
    if (encoder_mcp23017_init() != ESP_OK) {
        ESP_LOGW(TAG, "External encoder disabled; check I2C wiring and MCP23017 address jumpers");
        vTaskDelete(NULL);
        return;
    }

    uint8_t gpioa = 0xFF;
    uint8_t gpiob = 0xFF;
    uint8_t prev_freq_ab = 0x03;
    uint8_t prev_dnr_ab = 0x03;
    uint8_t prev_power_ab = 0x03;
    uint8_t prev_width_ab = 0x03;
    if (mcp23017_read_reg(MCP23017_REG_GPIOA, &gpioa) == ESP_OK) {
        prev_freq_ab = gpioa & 0x03;
        prev_dnr_ab = (gpioa >> 3) & 0x03;
        prev_power_ab = (gpioa >> 6) & 0x03;
    }
    if (mcp23017_read_reg(MCP23017_REG_GPIOB, &gpiob) == ESP_OK) {
        prev_width_ab = (uint8_t)(((gpiob & BIT(7)) ? BIT(1) : 0) | ((gpiob & BIT(6)) ? BIT(0) : 0));
        ESP_LOGI(TAG,
                 "MCP23017 initial GPIOA=0x%02X GPIOB=0x%02X freq_ab=%u dnr_ab=%u power_ab=%u width_ab=%u",
                 gpioa, gpiob, prev_freq_ab, prev_dnr_ab, prev_power_ab, prev_width_ab);
    }

    bool stable_freq_button = true;
    bool last_freq_button_sample = true;
    bool stable_dnr_button = true;
    bool last_dnr_button_sample = true;
    bool stable_power_button = true;
    bool last_power_button_sample = true;
    bool stable_width_button = true;
    bool last_width_button_sample = true;
    TickType_t last_freq_button_change = xTaskGetTickCount();
    TickType_t last_dnr_button_change = xTaskGetTickCount();
    TickType_t last_power_button_change = xTaskGetTickCount();
    TickType_t last_width_button_change = xTaskGetTickCount();
    int8_t freq_quad_accum = 0;
    int8_t dnr_quad_accum = 0;
    int8_t power_quad_accum = 0;
    int8_t width_quad_accum = 0;
    const int8_t quad_table[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0,
    };

    while (true) {
        if (mcp23017_read_reg(MCP23017_REG_GPIOA, &gpioa) == ESP_OK) {
            const uint8_t freq_ab = gpioa & 0x03;
            if (freq_ab != prev_freq_ab) {
                freq_quad_accum += quad_table[(prev_freq_ab << 2) | freq_ab];
                prev_freq_ab = freq_ab;
                if (freq_quad_accum >= 4) {
                    encoder_adjust_input(1);
                    freq_quad_accum = 0;
                } else if (freq_quad_accum <= -4) {
                    encoder_adjust_input(-1);
                    freq_quad_accum = 0;
                }
            }

            const uint8_t dnr_ab = (gpioa >> 3) & 0x03;
            if (dnr_ab != prev_dnr_ab) {
                dnr_quad_accum += quad_table[(prev_dnr_ab << 2) | dnr_ab];
                prev_dnr_ab = dnr_ab;
                if (dnr_quad_accum >= 4) {
                    encoder_adjust_dnr(1);
                    dnr_quad_accum = 0;
                } else if (dnr_quad_accum <= -4) {
                    encoder_adjust_dnr(-1);
                    dnr_quad_accum = 0;
                }
            }

            const uint8_t power_ab = (gpioa >> 6) & 0x03;
            if (power_ab != prev_power_ab) {
                power_quad_accum += quad_table[(prev_power_ab << 2) | power_ab];
                prev_power_ab = power_ab;
                if (power_quad_accum >= 4) {
                    if (s_power_encoder_selecting_step) {
                        encoder_adjust_power_step(1);
                    } else {
                        encoder_adjust_power(1);
                    }
                    power_quad_accum = 0;
                } else if (power_quad_accum <= -4) {
                    if (s_power_encoder_selecting_step) {
                        encoder_adjust_power_step(-1);
                    } else {
                        encoder_adjust_power(-1);
                    }
                    power_quad_accum = 0;
                }
            }

            const bool dnr_button_released = (gpioa & BIT(5)) != 0;
            const TickType_t dnr_now = xTaskGetTickCount();
            if (dnr_button_released != last_dnr_button_sample) {
                last_dnr_button_sample = dnr_button_released;
                last_dnr_button_change = dnr_now;
            }
            if ((dnr_now - last_dnr_button_change) >= pdMS_TO_TICKS(35) &&
                dnr_button_released != stable_dnr_button) {
                stable_dnr_button = dnr_button_released;
                if (!stable_dnr_button) {
                    dnr_quad_accum = 0;
                    encoder_dnr_off();
                }
            }

            const bool freq_button_released = (gpioa & BIT(2)) != 0;
            const TickType_t now = xTaskGetTickCount();
            if (freq_button_released != last_freq_button_sample) {
                last_freq_button_sample = freq_button_released;
                last_freq_button_change = now;
            }
            if ((now - last_freq_button_change) >= pdMS_TO_TICKS(35) &&
                freq_button_released != stable_freq_button) {
                stable_freq_button = freq_button_released;
                if (!stable_freq_button && s_screen == SCREEN_MAIN) {
                    submit_input_frequency();
                    if (bsp_display_lock(pdMS_TO_TICKS(50))) {
                        update_input_hint_locked();
                        bsp_display_unlock();
                    }
                }
            }
        }
        if (mcp23017_read_reg(MCP23017_REG_GPIOB, &gpiob) == ESP_OK) {
            const uint8_t width_ab = (uint8_t)(((gpiob & BIT(7)) ? BIT(1) : 0) | ((gpiob & BIT(6)) ? BIT(0) : 0));
            if (width_ab != prev_width_ab) {
                width_quad_accum += quad_table[(prev_width_ab << 2) | width_ab];
                prev_width_ab = width_ab;
                if (width_quad_accum >= 4) {
                    encoder_adjust_width(1);
                    width_quad_accum = 0;
                } else if (width_quad_accum <= -4) {
                    encoder_adjust_width(-1);
                    width_quad_accum = 0;
                }
            }

            const bool width_button_released = (gpiob & BIT(5)) != 0;
            const TickType_t width_now = xTaskGetTickCount();
            if (width_button_released != last_width_button_sample) {
                last_width_button_sample = width_button_released;
                last_width_button_change = width_now;
            }
            if ((width_now - last_width_button_change) >= pdMS_TO_TICKS(35) &&
                width_button_released != stable_width_button) {
                stable_width_button = width_button_released;
                if (!stable_width_button) {
                    width_quad_accum = 0;
                    encoder_width_default();
                }
            }

            const bool power_button_released = (gpiob & BIT(0)) != 0;
            const TickType_t now = xTaskGetTickCount();
            if (power_button_released != last_power_button_sample) {
                last_power_button_sample = power_button_released;
                last_power_button_change = now;
            }
            if ((now - last_power_button_change) >= pdMS_TO_TICKS(35) &&
                power_button_released != stable_power_button) {
                stable_power_button = power_button_released;
                if (!stable_power_button) {
                    s_power_encoder_selecting_step = !s_power_encoder_selecting_step;
                    power_quad_accum = 0;
                    ESP_LOGI(TAG, "Power encoder %s step select, step=%uW",
                             s_power_encoder_selecting_step ? "enter" : "exit", s_power_step);
                    aux_oled_show_current(true);
                    if (s_screen == SCREEN_MAIN && bsp_display_lock(pdMS_TO_TICKS(50))) {
                        update_ui_locked();
                        bsp_display_unlock();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}

static void aux_oled_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    if (aux_oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "Aux OLED disabled; check TCA9548A/OLED wiring");
        vTaskDelete(NULL);
        return;
    }

    aux_oled_show_current(true);
    while (true) {
        aux_oled_show_current(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void button_event_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !id) {
        return;
    }

    if (strcmp(id, "TIME_TOGGLE") == 0) {
        s_time_view = s_time_view == TIME_LOCAL ? TIME_UTC : TIME_LOCAL;
        update_ui();
        return;
    }
    if (strcmp(id, "NAV_WIFI") == 0) {
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            create_wifi_ui();
            bsp_display_unlock();
        }
        return;
    }
    if (strcmp(id, "BACK_MAIN") == 0) {
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            create_ui();
            bsp_display_unlock();
        }
        return;
    }
    if (strcmp(id, "EDIT_SSID") == 0 || strcmp(id, "EDIT_PASS") == 0 || strcmp(id, "KB_OPEN") == 0) {
        s_wifi_edit_field = strcmp(id, "EDIT_SSID") == 0 ? WIFI_EDIT_SSID : WIFI_EDIT_PASSWORD;
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            create_wifi_keyboard_ui();
            bsp_display_unlock();
        }
        return;
    }
    if (strcmp(id, "KB_DONE") == 0 || strcmp(id, "KB_CANCEL") == 0) {
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            create_wifi_ui();
            bsp_display_unlock();
        }
        return;
    }
    if (strcmp(id, "WIFI_CONNECT") == 0 || strcmp(id, "WIFI_DISCONNECT") == 0) {
        send_wifi_cmd(strcmp(id, "WIFI_CONNECT") == 0 ? WIFI_CMD_CONNECT : WIFI_CMD_DISCONNECT);
        return;
    }
    if (strcmp(id, "WIFI_SCAN") == 0) {
        send_wifi_cmd(WIFI_CMD_SCAN);
        return;
    }
    if (strncmp(id, "AP_", 3) == 0) {
        const int index = atoi(id + 3);
        if (index >= 0 && index < s_wifi_ap_count && index < (int)(sizeof(s_wifi_aps) / sizeof(s_wifi_aps[0]))) {
            strlcpy(s_wifi_ssid, s_wifi_aps[index].ssid, sizeof(s_wifi_ssid));
            if (bsp_display_lock(pdMS_TO_TICKS(100))) {
                create_wifi_ui();
                bsp_display_unlock();
            }
        }
        return;
    }
    if (s_screen == SCREEN_WIFI_KEYBOARD) {
        size_t target_size = 0;
        char *target = wifi_edit_target(&target_size);
        const size_t len = strlen(target);
        if (strcmp(id, "K_SHIFT") == 0) {
            s_kb_upper = !s_kb_upper;
        } else if (strcmp(id, "K_SPACE") == 0 && len + 1 < target_size) {
            target[len] = ' ';
            target[len + 1] = '\0';
        } else if (strcmp(id, "K_BS") == 0 && len > 0) {
            target[len - 1] = '\0';
        } else if (strcmp(id, "K_CLR") == 0) {
            target[0] = '\0';
        } else if (strncmp(id, "K_", 2) == 0 && len + 1 < target_size) {
            target[len] = id[2];
            target[len + 1] = '\0';
        }
        if (bsp_display_lock(pdMS_TO_TICKS(50))) {
            create_wifi_keyboard_ui();
            bsp_display_unlock();
        }
        return;
    }
    if (s_screen != SCREEN_MAIN) {
        return;
    }

    app_cmd_t cmd = {};
    if (strlen(id) == 1 && (isdigit((unsigned char)id[0]) || id[0] == '.')) {
        const size_t len = strlen(s_input);
        if (len + 1 < sizeof(s_input) && !(id[0] == '.' && strchr(s_input, '.'))) {
            s_input[len] = id[0];
            s_input[len + 1] = '\0';
        }
    } else if (strcmp(id, "BS") == 0) {
        const size_t len = strlen(s_input);
        if (len) {
            s_input[len - 1] = '\0';
        }
    } else if (strcmp(id, "CLR") == 0) {
        s_input[0] = '\0';
    } else if (strcmp(id, "TARGET_A") == 0) {
        s_input_target_vfo = 'A';
    } else if (strcmp(id, "TARGET_B") == 0) {
        s_input_target_vfo = 'B';
    } else if (strcmp(id, "SET") == 0) {
        submit_input_frequency();
    } else if (strcmp(id, "AB") == 0) {
        cmd.type = CMD_SELECT_MAIN_VFO;
        cmd.vfo = s_state.active_vfo == 'A' ? 'B' : 'A';
        s_state.active_vfo = cmd.vfo;
        send_cmd(&cmd);
    } else if (strcmp(id, "P2") == 0 || strcmp(id, "P5") == 0 || strcmp(id, "P10") == 0) {
        s_power_step = (uint8_t)atoi(id + 1);
    } else if (strcmp(id, "P-") == 0 || strcmp(id, "P+") == 0) {
        int p = (int)s_state.power_w + (strcmp(id, "P+") == 0 ? (int)s_power_step : -(int)s_power_step);
        if (p < 5) p = 5;
        if (p > 100) p = 100;
        s_state.power_w = (uint8_t)p;
        cmd.type = CMD_SET_POWER;
        cmd.value = (uint8_t)p;
        send_cmd(&cmd);
    } else if (strcmp(id, "D-") == 0 || strcmp(id, "D+") == 0) {
        int level = s_state.dnr_on ? s_state.dnr_level : 0;
        level += strcmp(id, "D+") == 0 ? 1 : -1;
        if (level <= 0) {
            level = 0;
            s_state.dnr_on = false;
        } else {
            if (level > 15) level = 15;
            s_state.dnr_on = true;
        }
        s_state.dnr_level = (uint8_t)level;
        cmd.type = CMD_SET_DNR_LEVEL;
        cmd.value = (uint8_t)level;
        send_cmd(&cmd);
    } else if (strncmp(id, "M_", 2) == 0) {
        cmd.type = CMD_SET_MODE;
        if (strcmp(id, "M_LSB") == 0) cmd.mode = FT710_MODE_LSB;
        else if (strcmp(id, "M_USB") == 0) cmd.mode = FT710_MODE_USB;
        else if (strcmp(id, "M_FM") == 0) cmd.mode = FT710_MODE_FM;
        else if (strcmp(id, "M_AM") == 0) cmd.mode = FT710_MODE_AM;
        else cmd.mode = FT710_MODE_DATA_U;
        cmd.vfo = s_input_target_vfo;
        if (cmd.vfo == 'B') {
            s_state.mode_b = cmd.mode;
        } else {
            s_state.mode_a = cmd.mode;
        }
        send_cmd(&cmd);
    } else if (strncmp(id, "B_", 2) == 0) {
        cmd.type = CMD_SET_BAND_FREQ;
        cmd.vfo = s_state.active_vfo;
        cmd.hz = radio_state_band_default_hz(id + 2);
        if (cmd.vfo == 'B') {
            s_state.vfo_b_hz = cmd.hz;
        } else {
            s_state.vfo_a_hz = cmd.hz;
        }
        send_cmd(&cmd);
    }

    if (bsp_display_lock(pdMS_TO_TICKS(50))) {
        update_input_hint_locked();
        update_ui_locked();
        bsp_display_unlock();
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, const char *id, int x, int y, int w, int h, const lv_font_t *font)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, C_BTN, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_CLICKED, (void *)id);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, text);
    lv_obj_set_style_text_font(txt, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, C_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(txt, 1, LV_PART_MAIN);
    lv_obj_center(txt);
    return btn;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, int x, int y, bool active)
{
    lv_obj_t *obj = button(parent, text, "noop", x, y, 120, 25, &lv_font_montserrat_14);
    lv_obj_remove_event_cb(obj, button_event_cb);
    set_button_active(obj, active);
    return obj;
}

static void touch_zone(lv_obj_t *parent, const char *id, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(obj, button_event_cb, LV_EVENT_CLICKED, (void *)id);
}

static void format_time_text(char *buf, size_t size)
{
    const time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(buf, size, "%s --:--:--", s_time_view == TIME_LOCAL ? "LOCAL" : "UTC");
        return;
    }

    struct tm tm_info = {};
    if (s_time_view == TIME_LOCAL) {
        localtime_r(&now, &tm_info);
        snprintf(buf, size, "LOCAL %02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        gmtime_r(&now, &tm_info);
        snprintf(buf, size, "UTC %02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    }
}

static void update_top_bar_locked(void)
{
    if (s_time_label) {
        char buf[32] = {};
        format_time_text(buf, sizeof(buf));
        lv_label_set_text(s_time_label, buf);
    }
}

static void create_top_bar(lv_obj_t *scr, bool show_back)
{
    box(scr, 8, 6, 1008, 36, C_PANEL);
    label(scr, "FT-710 CONTROL", 24, 15, &lv_font_montserrat_14, C_TEXT);
    s_time_label = label(scr, "LOCAL --:--:--", 212, 15, &lv_font_montserrat_14, C_TEXT);
    touch_zone(scr, "TIME_TOGGLE", 198, 6, 180, 36);
    s_cat_status = lv_obj_get_child(pill(scr, "CAT WAIT", 388, 12, false), 0);
    s_bt_status = lv_obj_get_child(pill(scr, "BT OFF", 518, 12, false), 0);
    lv_obj_t *wifi_btn = button(scr, wifi_state_text(), "NAV_WIFI", 648, 12, 120, 25, &lv_font_montserrat_14);
    s_wifi_status = lv_obj_get_child(wifi_btn, 0);
    set_button_active(wifi_btn, s_wifi_state == WIFI_STATE_ONLINE || s_wifi_state == WIFI_STATE_CONNECTING ||
                                    s_wifi_state == WIFI_STATE_SCANNING);
    s_rx_status = lv_obj_get_child(pill(scr, "RX", 778, 12, true), 0);
    if (show_back) {
        button(scr, "BACK", "BACK_MAIN", 908, 12, 92, 25, &lv_font_montserrat_14);
    } else {
        label(scr, "MENU", 944, 15, &lv_font_montserrat_14, C_MUTED);
    }
}

static void update_ui_locked(void)
{
    update_top_bar_locked();
    if (s_screen != SCREEN_MAIN) {
        if (s_screen == SCREEN_WIFI && s_wifi_page_status_label) {
            lv_label_set_text(s_wifi_conn_ssid_label, s_wifi_state == WIFI_STATE_ONLINE ? s_wifi_ssid : "--");
            lv_label_set_text(s_wifi_ip_label, s_wifi_ip);
            lv_label_set_text(s_wifi_gateway_label, s_wifi_gateway);
            lv_label_set_text(s_wifi_dns_label, s_wifi_dns);
            lv_label_set_text(s_wifi_page_status_label, wifi_page_status_text());
            lv_obj_set_style_text_color(s_wifi_page_status_label,
                                        s_wifi_state == WIFI_STATE_ONLINE ? C_CYAN : C_MUTED, LV_PART_MAIN);
        }
        return;
    }

    char buf[48] = {};
    fmt_freq(s_state.vfo_a_hz, buf, sizeof(buf));
    lv_label_set_text(s_freq_a, buf);
    fmt_freq(s_state.vfo_b_hz, buf, sizeof(buf));
    lv_label_set_text(s_freq_b, buf);

    lv_label_set_text(s_vfo_a_tag, s_state.active_vfo == 'A' ? "VFO-A" : "VFO-A");
    lv_label_set_text(s_vfo_b_tag, s_state.active_vfo == 'B' ? "VFO-B" : "VFO-B");
    lv_obj_set_style_text_color(s_vfo_a_tag, s_state.active_vfo == 'A' ? C_CYAN : C_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_vfo_b_tag, s_state.active_vfo == 'B' ? C_CYAN : C_MUTED, LV_PART_MAIN);

    snprintf(buf, sizeof(buf), "%s\n%uW", radio_state_mode_name(s_state.mode_a), s_state.power_w);
    lv_label_set_text(s_vfo_a_meta, buf);
    snprintf(buf, sizeof(buf), "%s\n%uW", radio_state_mode_name(s_state.mode_b), s_state.power_w);
    lv_label_set_text(s_vfo_b_meta, buf);

    if (s_power_encoder_selecting_step) {
        snprintf(buf, sizeof(buf), "STEP %uW", s_power_step);
    } else {
        snprintf(buf, sizeof(buf), "%uW", s_state.power_w);
    }
    lv_label_set_text(s_power_label, buf);
    snprintf(buf, sizeof(buf), "%02u", s_state.dnr_on ? s_state.dnr_level : 0);
    lv_label_set_text(s_dnr_label, s_state.dnr_on ? buf : "OFF");
    snprintf(buf, sizeof(buf), "RL%03u", s_state.dnr_level);
    lv_label_set_text(s_dnr_cmd_label, buf);

    lv_label_set_text(s_cat_status, s_state.online ? "CAT ONLINE" : "CAT WAIT");
    lv_label_set_text(s_wifi_status, wifi_state_text());
    set_button_active(lv_obj_get_parent(s_cat_status), s_state.online);
    set_button_active(lv_obj_get_parent(s_bt_status), false);
    set_button_active(lv_obj_get_parent(s_wifi_status), s_wifi_state == WIFI_STATE_ONLINE ||
                                                s_wifi_state == WIFI_STATE_CONNECTING ||
                                                s_wifi_state == WIFI_STATE_SCANNING);
    set_button_active(lv_obj_get_parent(s_rx_status), true);

    set_button_active(s_power_step_btns[0], s_power_step == 2);
    set_button_active(s_power_step_btns[1], s_power_step == 5);
    set_button_active(s_power_step_btns[2], s_power_step == 10);
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_border_width(s_power_step_btns[i], s_power_encoder_selecting_step ? 3 : 1, LV_PART_MAIN);
    }

    ft710_mode_t active_mode = s_input_target_vfo == 'B' ? s_state.mode_b : s_state.mode_a;
    const ft710_mode_t modes[5] = {FT710_MODE_LSB, FT710_MODE_USB, FT710_MODE_FM, FT710_MODE_AM, FT710_MODE_DATA_U};
    for (int i = 0; i < 5; ++i) {
        set_button_active(s_mode_btns[i], active_mode == modes[i]);
    }

    const uint32_t active_hz = s_state.active_vfo == 'B' ? s_state.vfo_b_hz : s_state.vfo_a_hz;
    const uint32_t bands[6] = {3500000, 7074000, 14270000, 21400000, 28400000, 50125000};
    for (int i = 0; i < 6; ++i) {
        uint32_t low = bands[i] > 500000 ? bands[i] - 500000 : 0;
        uint32_t high = bands[i] + 500000;
        set_button_active(s_band_btns[i], active_hz >= low && active_hz <= high);
    }
}

static void update_ui(void)
{
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        update_ui_locked();
        bsp_display_unlock();
    }
}

static void create_wifi_ui(void)
{
    s_screen = SCREEN_WIFI;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_top_bar(scr, true);

    box(scr, 8, 52, 392, 540, C_PANEL);
    label(scr, "WIFI", 24, 72, &lv_font_montserrat_24, C_TEXT);
    label(scr, "SCAN RESULT", 286, 76, &lv_font_montserrat_14, C_MUTED);
    lv_obj_t *ap_list = box(scr, 24, 112, 360, 360, C_BG);
    lv_obj_set_style_bg_opa(ap_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ap_list, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(ap_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ap_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(ap_list, LV_OBJ_FLAG_SCROLLABLE);
    const int shown_count = s_wifi_ap_count > 0 ? s_wifi_ap_count : 1;
    for (int i = 0; i < shown_count; ++i) {
        char ap_text[96] = {};
        if (i < s_wifi_ap_count) {
            snprintf(ap_text, sizeof(ap_text), "%-18s %d\n%s - CH %u", s_wifi_aps[i].ssid,
                     s_wifi_aps[i].rssi, wifi_auth_text(s_wifi_aps[i].authmode), s_wifi_aps[i].channel);
            snprintf(s_wifi_ap_ids[i], sizeof(s_wifi_ap_ids[i]), "AP_%02d", i);
        } else {
            snprintf(ap_text, sizeof(ap_text), "NO SCAN RESULT\nTOUCH SCAN");
        }
        lv_obj_t *net = button(ap_list, ap_text, i < s_wifi_ap_count ? s_wifi_ap_ids[i] : "noop",
                               0, i * 82, 350, 70, &lv_font_montserrat_20);
        if (i < s_wifi_ap_count && strcmp(s_wifi_aps[i].ssid, s_wifi_ssid) == 0) {
            set_button_active(net, true);
        }
    }
    button(scr, "SCAN", "WIFI_SCAN", 24, 490, 168, 60, &lv_font_montserrat_22);
    button(scr, "FORGET", "noop", 216, 490, 168, 60, &lv_font_montserrat_22);

    box(scr, 414, 52, 602, 210, C_PANEL);
    label(scr, "CONNECTION", 430, 72, &lv_font_montserrat_24, C_TEXT);
    s_wifi_page_status_label = label(scr, wifi_page_status_text(), 840, 76, &lv_font_montserrat_24,
                                     s_wifi_state == WIFI_STATE_ONLINE ? C_CYAN : C_MUTED);
    box(scr, 430, 112, 270, 54, C_BTN);
    label(scr, "SSID", 444, 124, &lv_font_montserrat_14, C_MUTED);
    s_wifi_conn_ssid_label = label(scr, s_wifi_state == WIFI_STATE_ONLINE ? s_wifi_ssid : "--", 520, 122, &lv_font_montserrat_18, C_TEXT);
    box(scr, 718, 112, 282, 54, C_BTN);
    label(scr, "IP ADDRESS", 732, 124, &lv_font_montserrat_14, C_MUTED);
    s_wifi_ip_label = label(scr, s_wifi_ip, 846, 122, &lv_font_montserrat_18, C_CYAN);
    box(scr, 430, 184, 270, 54, C_BTN);
    label(scr, "GATEWAY", 444, 196, &lv_font_montserrat_14, C_MUTED);
    s_wifi_gateway_label = label(scr, s_wifi_gateway, 548, 194, &lv_font_montserrat_18, C_TEXT);
    box(scr, 718, 184, 282, 54, C_BTN);
    label(scr, "DNS", 732, 196, &lv_font_montserrat_14, C_MUTED);
    s_wifi_dns_label = label(scr, s_wifi_dns, 846, 194, &lv_font_montserrat_18, C_TEXT);

    box(scr, 414, 282, 602, 310, C_PANEL);
    label(scr, "CONFIG", 430, 302, &lv_font_montserrat_24, C_TEXT);
    label(scr, "TOUCH INPUT", 874, 308, &lv_font_montserrat_14, C_MUTED);
    box(scr, 430, 360, 570, 58, C_BTN);
    label(scr, "SSID", 446, 379, &lv_font_montserrat_16, C_MUTED);
    label(scr, s_wifi_ssid[0] ? s_wifi_ssid : "--", 556, 374, &lv_font_montserrat_22, C_TEXT);
    touch_zone(scr, "EDIT_SSID", 430, 360, 570, 58);
    char pass_buf[24] = {};
    password_mask(pass_buf, sizeof(pass_buf));
    box(scr, 430, 438, 570, 58, C_BTN);
    label(scr, "PASSWORD", 446, 457, &lv_font_montserrat_16, C_MUTED);
    label(scr, pass_buf, 556, 452, &lv_font_montserrat_22, C_TEXT);
    touch_zone(scr, "EDIT_PASS", 430, 438, 570, 58);
    button(scr, "CONNECT", "WIFI_CONNECT", 430, 522, 270, 54, &lv_font_montserrat_20);
    button(scr, "DISCONNECT", "WIFI_DISCONNECT", 730, 522, 270, 54, &lv_font_montserrat_20);

    update_ui_locked();
}

static void create_wifi_keyboard_ui(void)
{
    s_screen = SCREEN_WIFI_KEYBOARD;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_top_bar(scr, true);
    box(scr, 38, 58, 948, 512, C_PANEL);
    label(scr, "WIFI INPUT", 62, 80, &lv_font_montserrat_24, C_TEXT);
    label(scr, s_wifi_edit_field == WIFI_EDIT_SSID ? "SSID" : "PASSWORD", 746, 86, &lv_font_montserrat_14, C_MUTED);
    box(scr, 62, 126, 900, 62, C_BTN);
    size_t target_size = 0;
    char *target = wifi_edit_target(&target_size);
    (void)target_size;
    label(scr, target[0] ? target : "--", 84, 144, &lv_font_montserrat_24, C_CYAN);

    const char *row1[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    const char *id1[] = {"K_1", "K_2", "K_3", "K_4", "K_5", "K_6", "K_7", "K_8", "K_9", "K_0"};
    for (int i = 0; i < 10; ++i) {
        button(scr, row1[i], id1[i], 62 + i * 90, 214, 76, 48, &lv_font_montserrat_22);
    }

    const char *row2_upper[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
    const char *row2_lower[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
    const char *id2_upper[] = {"K_Q", "K_W", "K_E", "K_R", "K_T", "K_Y", "K_U", "K_I", "K_O", "K_P"};
    const char *id2_lower[] = {"K_q", "K_w", "K_e", "K_r", "K_t", "K_y", "K_u", "K_i", "K_o", "K_p"};
    for (int i = 0; i < 10; ++i) {
        button(scr, s_kb_upper ? row2_upper[i] : row2_lower[i], s_kb_upper ? id2_upper[i] : id2_lower[i],
               62 + i * 90, 276, 76, 48, &lv_font_montserrat_22);
    }

    const char *row3_upper[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L"};
    const char *row3_lower[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l"};
    const char *id3_upper[] = {"K_A", "K_S", "K_D", "K_F", "K_G", "K_H", "K_J", "K_K", "K_L"};
    const char *id3_lower[] = {"K_a", "K_s", "K_d", "K_f", "K_g", "K_h", "K_j", "K_k", "K_l"};
    for (int i = 0; i < 9; ++i) {
        button(scr, s_kb_upper ? row3_upper[i] : row3_lower[i], s_kb_upper ? id3_upper[i] : id3_lower[i],
               104 + i * 90, 338, 76, 48, &lv_font_montserrat_22);
    }

    const char *row4_upper[] = {"Z", "X", "C", "V", "B", "N", "M", ".", "-"};
    const char *row4_lower[] = {"z", "x", "c", "v", "b", "n", "m", "_", "@"};
    const char *id4_upper[] = {"K_Z", "K_X", "K_C", "K_V", "K_B", "K_N", "K_M", "K_.", "K_-"};
    const char *id4_lower[] = {"K_z", "K_x", "K_c", "K_v", "K_b", "K_n", "K_m", "K__", "K_@"};
    for (int i = 0; i < 9; ++i) {
        button(scr, s_kb_upper ? row4_upper[i] : row4_lower[i], s_kb_upper ? id4_upper[i] : id4_lower[i],
               104 + i * 90, 400, 76, 48, &lv_font_montserrat_22);
    }

    button(scr, "SHIFT", "K_SHIFT", 62, 480, 120, 54, &lv_font_montserrat_18);
    button(scr, "CLEAR", "K_CLR", 198, 480, 104, 54, &lv_font_montserrat_18);
    button(scr, "SPACE", "K_SPACE", 318, 480, 226, 54, &lv_font_montserrat_18);
    button(scr, "BACK", "K_BS", 560, 480, 120, 54, &lv_font_montserrat_18);
    button(scr, "CANCEL", "KB_CANCEL", 696, 480, 120, 54, &lv_font_montserrat_18);
    button(scr, "DONE", "KB_DONE", 832, 480, 130, 54, &lv_font_montserrat_18);

    update_ui_locked();
}

static void create_ui(void)
{
    s_screen = SCREEN_MAIN;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_top_bar(scr, false);

    const int left_w = 398;
    box(scr, 8, 52, left_w, 150, C_PANEL);
    label(scr, "FREQUENCY  DUAL VFO", 24, 68, &lv_font_montserrat_14, C_MUTED);
    s_vfo_a_tag = label(scr, "VFO-A", 26, 104, &lv_font_montserrat_16, C_CYAN);
    s_freq_a = label(scr, "00.000.000", 90, 88, &lv_font_montserrat_40, C_TEXT);
    s_vfo_a_meta = label(scr, "--\n--W", 330, 92, &lv_font_montserrat_18, C_CYAN);
    s_vfo_b_tag = label(scr, "VFO-B", 26, 158, &lv_font_montserrat_16, C_MUTED);
    s_freq_b = label(scr, "00.000.000", 90, 142, &lv_font_montserrat_40, C_TEXT);
    s_vfo_b_meta = label(scr, "--\n--W", 330, 146, &lv_font_montserrat_18, C_CYAN);

    box(scr, 8, 212, left_w, 380, C_PANEL);
    label(scr, "DIRECT FREQUENCY", 24, 230, &lv_font_montserrat_16, C_TEXT);
    s_input_hint = label(scr, "A  --.---", 280, 230, &lv_font_montserrat_16, C_MUTED);
    const int kx = 22, ky = 258, kw = 116, kh = 46, gap = 7;
    const char *keys[5][3] = {
        {"1", "2", "3"},
        {"4", "5", "6"},
        {"7", "8", "9"},
        {".", "0", "BS"},
        {"A", "B", "A/B"},
    };
    const char *ids[5][3] = {
        {"1", "2", "3"},
        {"4", "5", "6"},
        {"7", "8", "9"},
        {".", "0", "BS"},
        {"TARGET_A", "TARGET_B", "AB"},
    };
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 3; ++c) {
            button(scr, keys[r][c], ids[r][c], kx + c * (kw + gap), ky + r * (kh + gap), kw, kh,
                   r == 4 ? &lv_font_montserrat_22 : &lv_font_montserrat_26);
        }
    }
    button(scr, "CLR", "CLR", 22, 540, 178, 44, &lv_font_montserrat_18);
    button(scr, "ENTER / SET", "SET", 208, 540, 176, 44, &lv_font_montserrat_18);

    const int rx = 420;
    box(scr, rx, 52, 286, 266, C_PANEL);
    label(scr, "RF POWER", rx + 16, 70, &lv_font_montserrat_16, C_TEXT);
    label(scr, "PC COMMAND", rx + 188, 72, &lv_font_montserrat_14, C_MUTED);
    s_power_step_btns[0] = button(scr, "2W", "P2", rx + 18, 104, 80, 48, &lv_font_montserrat_18);
    s_power_step_btns[1] = button(scr, "5W", "P5", rx + 104, 104, 80, 48, &lv_font_montserrat_18);
    s_power_step_btns[2] = button(scr, "10W", "P10", rx + 190, 104, 80, 48, &lv_font_montserrat_18);
    s_power_label = label(scr, "46W", rx + 78, 170, &lv_font_montserrat_48, C_TEXT);
    button(scr, "-", "P-", rx + 18, 252, 122, 50, &lv_font_montserrat_36);
    button(scr, "+", "P+", rx + 150, 252, 122, 50, &lv_font_montserrat_36);

    box(scr, 718, 52, 298, 266, C_PANEL);
    label(scr, "DNR", 734, 70, &lv_font_montserrat_16, C_TEXT);
    s_dnr_cmd_label = label(scr, "RL000", 946, 72, &lv_font_montserrat_14, C_MUTED);
    button(scr, "-", "D-", 734, 112, 76, 188, &lv_font_montserrat_40);
    s_dnr_label = label(scr, "OFF", 822, 168, &lv_font_montserrat_40, C_CYAN);
    label(scr, "DNR LEVEL", 826, 232, &lv_font_montserrat_14, C_MUTED);
    button(scr, "+", "D+", 924, 112, 76, 188, &lv_font_montserrat_40);

    box(scr, rx, 330, 286, 262, C_PANEL);
    label(scr, "MODE", rx + 16, 348, &lv_font_montserrat_16, C_TEXT);
    s_mode_btns[0] = button(scr, "LSB", "M_LSB", rx + 18, 386, 122, 58, &lv_font_montserrat_24);
    s_mode_btns[1] = button(scr, "USB", "M_USB", rx + 150, 386, 122, 58, &lv_font_montserrat_24);
    s_mode_btns[2] = button(scr, "FM", "M_FM", rx + 18, 454, 122, 58, &lv_font_montserrat_24);
    s_mode_btns[3] = button(scr, "AM", "M_AM", rx + 150, 454, 122, 58, &lv_font_montserrat_24);
    s_mode_btns[4] = button(scr, "DATA-U", "M_DATA", rx + 18, 522, 254, 50, &lv_font_montserrat_24);

    box(scr, 718, 330, 298, 262, C_PANEL);
    label(scr, "BAND", 734, 348, &lv_font_montserrat_16, C_TEXT);
    label(scr, "MHz", 968, 350, &lv_font_montserrat_14, C_MUTED);
    const char *band_txt[6] = {"3.5", "7", "14", "21", "28", "50"};
    const char *band_id[6] = {"B_3.5", "B_7", "B_14", "B_21", "B_28", "B_50"};
    for (int i = 0; i < 6; ++i) {
        int col = i % 3;
        int row = i / 3;
        s_band_btns[i] = button(scr, band_txt[i], band_id[i], 734 + col * 90, 386 + row * 94, 80, 74, &lv_font_montserrat_32);
    }

    update_ui_locked();
}

static void apply_command(cdc_acm_dev_hdl_t dev, app_cmd_t *cmd)
{
    char out[24] = {};
    int64_t elapsed_ms = 0;
    esp_err_t err = ESP_OK;
    switch (cmd->type) {
    case CMD_SET_FREQ:
        snprintf(out, sizeof(out), "F%c%09lu;", cmd->vfo, (unsigned long)cmd->hz);
        err = cat_send(dev, out, &elapsed_ms);
        break;
    case CMD_SET_POWER:
        if (cmd->value < 5) cmd->value = 5;
        if (cmd->value > 100) cmd->value = 100;
        snprintf(out, sizeof(out), "PC%03u;", cmd->value);
        err = cat_send(dev, out, &elapsed_ms);
        break;
    case CMD_ADJUST_POWER: {
        int p = (int)s_state.power_w + cmd->delta * (int)s_power_step;
        if (p < 5) p = 5;
        if (p > 100) p = 100;
        snprintf(out, sizeof(out), "PC%03d;", p);
        err = cat_send(dev, out, &elapsed_ms);
        break;
    }
    case CMD_SET_POWER_STEP:
        s_power_step = cmd->value;
        s_power_encoder_selecting_step = false;
        update_ui();
        return;
    case CMD_SET_DNR_LEVEL:
        if (cmd->value == 0) {
            snprintf(out, sizeof(out), "NR00;");
            err = cat_send(dev, out, &elapsed_ms);
        } else {
            uint8_t level = cmd->value > 15 ? 15 : cmd->value;
            snprintf(out, sizeof(out), "RL0%02u;", level);
            err = cat_send(dev, out, &elapsed_ms);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                err = cat_send(dev, "NR01;", &elapsed_ms);
            }
        }
        break;
    case CMD_ADJUST_DNR: {
        int level = s_state.dnr_on ? s_state.dnr_level : 0;
        level += cmd->delta;
        if (level <= 0) {
            snprintf(out, sizeof(out), "NR00;");
            err = cat_send(dev, out, &elapsed_ms);
        } else {
            if (level > 15) level = 15;
            snprintf(out, sizeof(out), "RL0%02d;", level);
            err = cat_send(dev, out, &elapsed_ms);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(40));
                err = cat_send(dev, "NR01;", &elapsed_ms);
            }
        }
        break;
    }
    case CMD_SET_WIDTH:
        if (cmd->value > WIDTH_INDEX_MAX) cmd->value = WIDTH_INDEX_MAX;
        snprintf(out, sizeof(out), "SH00%02u;", cmd->value);
        err = cat_send(dev, out, &elapsed_ms);
        break;
    case CMD_SET_MODE:
        snprintf(out, sizeof(out), "MD%c%X;", cmd->vfo == 'B' ? '1' : '0', radio_state_mode_code(cmd->mode));
        err = cat_send(dev, out, &elapsed_ms);
        break;
    case CMD_SELECT_MAIN_VFO:
        snprintf(out, sizeof(out), "VS%c;", cmd->vfo == 'B' ? '1' : '0');
        err = cat_send(dev, out, &elapsed_ms);
        break;
    case CMD_SET_BAND_FREQ:
        snprintf(out, sizeof(out), "F%c%09lu;", cmd->vfo, (unsigned long)cmd->hz);
        err = cat_send(dev, out, &elapsed_ms);
        break;
    default:
        return;
    }
    ESP_LOGI(TAG, "CAT set %s -> %s (%lld ms)", out, esp_err_to_name(err), elapsed_ms);
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static void wifi_save_config(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "ssid", s_wifi_ssid);
    nvs_set_str(nvs, "password", s_wifi_edit_value);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void wifi_load_config(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_wifi_ssid);
    nvs_get_str(nvs, "ssid", s_wifi_ssid, &len);
    len = sizeof(s_wifi_edit_value);
    nvs_get_str(nvs, "password", s_wifi_edit_value, &len);
    nvs_close(nvs);
}

static void wifi_apply_test_defaults(void)
{
    strlcpy(s_wifi_ssid, WIFI_DEFAULT_SSID, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_edit_value, WIFI_DEFAULT_PASSWORD, sizeof(s_wifi_edit_value));
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
}

static bool parse_bridge_discovery(const char *msg, char *ip, size_t ip_size, int *port)
{
    char marker[16] = {};
    char parsed_ip[16] = {};
    int parsed_port = 0;
    if (sscanf(msg, "%15s %15s %d", marker, parsed_ip, &parsed_port) != 3) {
        return false;
    }
    if (strcmp(marker, "CAT3BRIDGE") != 0 || parsed_port <= 0 || parsed_port > 65535) {
        return false;
    }
    strlcpy(ip, parsed_ip, ip_size);
    *port = parsed_port;
    return true;
}

static esp_err_t wifi_bridge_tcp_probe(const char *ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "WiFi bridge socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = inet_addr(ip),
    };
    struct timeval timeout = {
        .tv_sec = 2,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGW(TAG, "WiFi bridge connect %s:%d failed: errno=%d", ip, port, errno);
        close(sock);
        return ESP_FAIL;
    }

    const char test_msg[] = "P4_WIFI_UART_TEST;";
    send(sock, test_msg, strlen(test_msg), 0);
    char rx[128] = {};
    int len = recv(sock, rx, sizeof(rx) - 1, 0);
    close(sock);
    if (len <= 0) {
        ESP_LOGW(TAG, "WiFi bridge probe timeout/no data");
        return ESP_ERR_TIMEOUT;
    }
    rx[len] = '\0';
    ESP_LOGI(TAG, "WiFi bridge probe OK: sent='%s' recv='%s'", test_msg, rx);
    return ESP_OK;
}

static void wifi_bridge_client_task(void *arg)
{
    (void)arg;
    int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp < 0) {
        ESP_LOGW(TAG, "WiFi bridge UDP socket failed: errno=%d", errno);
        vTaskDelete(NULL);
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_BRIDGE_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(udp, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGW(TAG, "WiFi bridge UDP bind failed: errno=%d", errno);
        close(udp);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "WiFi bridge discovery listening on UDP %d", WIFI_BRIDGE_DISCOVERY_PORT);
    while (true) {
        char msg[96] = {};
        int len = recv(udp, msg, sizeof(msg) - 1, 0);
        if (len <= 0) {
            continue;
        }
        msg[len] = '\0';
        char bridge_ip[16] = {};
        int bridge_port = 0;
        if (!parse_bridge_discovery(msg, bridge_ip, sizeof(bridge_ip), &bridge_port)) {
            continue;
        }
        ESP_LOGI(TAG, "WiFi bridge found: %s:%d", bridge_ip, bridge_port);
        if (wifi_bridge_tcp_probe(bridge_ip, bridge_port) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_reconfig_disconnect) {
            s_wifi_reconfig_disconnect = false;
            s_wifi_state = WIFI_STATE_CONNECTING;
            update_ui();
            return;
        }
        if (s_wifi_manual_disconnect) {
            s_wifi_manual_disconnect = false;
            s_wifi_reconfig_disconnect = false;
            s_wifi_state = WIFI_STATE_READY;
            clear_wifi_addrs();
            update_ui();
            return;
        }
        s_wifi_state = WIFI_STATE_FAILED;
        clear_wifi_addrs();
        if (s_wifi_retry < 5) {
            s_wifi_retry++;
            s_wifi_state = WIFI_STATE_CONNECTING;
            esp_wifi_connect();
        } else if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        update_ui();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_wifi_ip, sizeof(s_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(s_wifi_gateway, sizeof(s_wifi_gateway), IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_dns_info_t dns_info = {};
        if (s_wifi_netif && esp_netif_get_dns_info(s_wifi_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            snprintf(s_wifi_dns, sizeof(s_wifi_dns), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        }
        s_wifi_reconfig_disconnect = false;
        s_wifi_retry = 0;
        s_wifi_state = WIFI_STATE_ONLINE;
        wifi_save_config();
        start_sntp_once();
        if (!s_wifi_bridge_task_started) {
            s_wifi_bridge_task_started = true;
            BaseType_t created = xTaskCreate(wifi_bridge_client_task, "wifi_bridge_client", 6144, NULL, 4, NULL);
            if (created != pdTRUE) {
                s_wifi_bridge_task_started = false;
                ESP_LOGW(TAG, "Failed to start WiFi bridge client task");
            }
        }
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        update_ui();
    }
}

static void wifi_scan(void)
{
    s_wifi_state = WIFI_STATE_SCANNING;
    update_ui();
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_FAILED;
        update_ui();
        return;
    }

    uint16_t count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&count));
    wifi_ap_record_t records[WIFI_AP_MAX] = {};
    uint16_t request = count > WIFI_AP_MAX ? WIFI_AP_MAX : count;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&request, records));
    s_wifi_ap_count = request;
    for (int i = 0; i < s_wifi_ap_count; ++i) {
        strlcpy(s_wifi_aps[i].ssid, (const char *)records[i].ssid, sizeof(s_wifi_aps[i].ssid));
        s_wifi_aps[i].rssi = records[i].rssi;
        s_wifi_aps[i].channel = records[i].primary;
        s_wifi_aps[i].authmode = records[i].authmode;
        ESP_LOGI(TAG, "WiFi AP[%d]: ssid='%s' rssi=%d channel=%u auth=%s", i, s_wifi_aps[i].ssid,
                 s_wifi_aps[i].rssi, s_wifi_aps[i].channel, wifi_auth_text(s_wifi_aps[i].authmode));
    }
    ESP_LOGI(TAG, "WiFi scan complete: %u AP shown, %u AP total", s_wifi_ap_count, count);
    s_wifi_state = WIFI_STATE_READY;
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        if (s_screen == SCREEN_WIFI) {
            create_wifi_ui();
        } else {
            update_ui_locked();
        }
        bsp_display_unlock();
    }
}

static void wifi_connect_current(void)
{
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_wifi_edit_value, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(s_wifi_edit_value) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    s_wifi_retry = 0;
    s_wifi_manual_disconnect = false;
    bool was_active = s_wifi_state == WIFI_STATE_ONLINE || s_wifi_state == WIFI_STATE_CONNECTING;
    s_wifi_state = WIFI_STATE_CONNECTING;
    clear_wifi_addrs();
    if (was_active) {
        s_wifi_reconfig_disconnect = true;
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err == ESP_ERR_WIFI_NOT_CONNECT) {
            s_wifi_reconfig_disconnect = false;
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(disconnect_err);
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(err));
        s_wifi_reconfig_disconnect = false;
        s_wifi_state = WIFI_STATE_FAILED;
    }
    update_ui();
}

static void wifi_manager_task(void *arg)
{
    (void)arg;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_load_config();
    wifi_apply_test_defaults();

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_state = WIFI_STATE_READY;
    ESP_LOGI(TAG, "ESP-Hosted WiFi ready, SSID='%s'", s_wifi_ssid);
    update_ui();

    send_wifi_cmd(WIFI_CMD_SCAN);
    send_wifi_cmd(WIFI_CMD_CONNECT);
    wifi_cmd_t cmd = {};
    while (true) {
        if (xQueueReceive(s_wifi_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.type) {
        case WIFI_CMD_SCAN:
            wifi_scan();
            break;
        case WIFI_CMD_CONNECT:
            wifi_connect_current();
            break;
        case WIFI_CMD_DISCONNECT:
            s_wifi_manual_disconnect = true;
            s_wifi_retry = 5;
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
            s_wifi_state = WIFI_STATE_READY;
            clear_wifi_addrs();
            update_ui();
            break;
        default:
            break;
        }
    }
}

static void poll_one(cdc_acm_dev_hdl_t dev, const char *cmd, char *resp, size_t resp_size)
{
    int64_t elapsed = 0;
    esp_err_t err = cat_query(dev, cmd, resp, resp_size, &elapsed);
    s_state.last_cat_ms = (uint32_t)elapsed;
    if (err != ESP_OK) {
        s_state.fail_count++;
        ESP_LOGW(TAG, "CAT query %s failed: %s resp='%s'", cmd, esp_err_to_name(err), resp);
        return;
    }
    s_state.ok_count++;
    uint32_t hz = 0;
    uint8_t v = 0;
    bool b = false;
    ft710_mode_t mode = FT710_MODE_UNKNOWN;
    if (parse_vfo_hz(resp, "FA", &hz)) s_state.vfo_a_hz = hz;
    else if (parse_vfo_hz(resp, "FB", &hz)) s_state.vfo_b_hz = hz;
    else if (parse_mode(resp, "MD0", &mode)) s_state.mode_a = mode;
    else if (parse_mode(resp, "MD1", &mode)) s_state.mode_b = mode;
    else if (parse_u8_3(resp, "PC", &v)) s_state.power_w = v;
    else if (parse_bool_4(resp, "NR0", &b)) s_state.dnr_on = b;
    else if (parse_u8_3(resp, "RL0", &v)) s_state.dnr_level = v;
    else if (parse_width_index(resp, &v)) s_state.width_index = v > WIDTH_INDEX_MAX ? WIDTH_INDEX_MAX : v;
    else if (resp[0] == 'V' && resp[1] == 'S' && resp[3] == ';') s_state.active_vfo = resp[2] == '1' ? 'B' : 'A';
}

static void cat_task(void *arg)
{
    (void)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    assert(task_created == pdTRUE);
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 20000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = event_cb,
        .data_cb = rx_cb,
        .user_arg = NULL,
    };

    cdc_acm_dev_hdl_t dev = NULL;
    ESP_LOGI(TAG, "Waiting for CH9102 1A86:55D4");
    esp_err_t err = cdc_acm_host_open(0x1A86, 0x55D4, 0, &dev_config, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open CH9102: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }

    const cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 38400,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };
    ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(dev, &line_coding));
    ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(dev, false, false));
    s_state.online = true;
    ESP_LOGI(TAG, "CH9102 opened, FT-710 control UI running");

    char resp[64] = {};
    const char *slow_polls[] = {"ID;", "MD0;", "MD1;", "PC;", "NR0;", "RL0;", "SH0;", "VS;"};
    size_t slow_idx = 0;
    uint32_t loop_count = 0;
    while (true) {
        app_cmd_t cmd = {};
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            apply_command(dev, &cmd);
        }
        memset(resp, 0, sizeof(resp));
        poll_one(dev, "FA;", resp, sizeof(resp));
        memset(resp, 0, sizeof(resp));
        poll_one(dev, "FB;", resp, sizeof(resp));
        if ((loop_count % 3) == 0) {
            memset(resp, 0, sizeof(resp));
            poll_one(dev, slow_polls[slow_idx], resp, sizeof(resp));
            slow_idx = (slow_idx + 1) % (sizeof(slow_polls) / sizeof(slow_polls[0]));
        }
        loop_count++;
        update_ui();
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

void app_controller_start(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    radio_state_init(&s_state);
    s_rx_queue = xQueueCreate(512, sizeof(rx_byte_t));
    s_cmd_queue = xQueueCreate(16, sizeof(app_cmd_t));
    s_wifi_cmd_queue = xQueueCreate(8, sizeof(wifi_cmd_t));
    s_i2c_mutex = xSemaphoreCreateMutex();
    assert(s_rx_queue && s_cmd_queue && s_wifi_cmd_queue && s_i2c_mutex);

    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    ESP_ERROR_CHECK(display != NULL ? ESP_OK : ESP_FAIL);
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(bsp_display_lock(-1) ? ESP_OK : ESP_ERR_TIMEOUT);
    create_ui();
    bsp_display_unlock();

    i2c_diagnostic_scan();

    BaseType_t wifi_task_created = xTaskCreate(wifi_manager_task, "wifi_manager", 8192, NULL, 6, NULL);
    assert(wifi_task_created == pdTRUE);

    BaseType_t encoder_task_created = xTaskCreate(encoder_task, "encoder_task", 4096, NULL, 7, NULL);
    assert(encoder_task_created == pdTRUE);

    BaseType_t aux_oled_task_created = xTaskCreate(aux_oled_task, "aux_oled", 4096, NULL, 4, NULL);
    assert(aux_oled_task_created == pdTRUE);

    BaseType_t task_created = xTaskCreate(cat_task, "cat_task", 8192, NULL, 8, NULL);
    assert(task_created == pdTRUE);
}

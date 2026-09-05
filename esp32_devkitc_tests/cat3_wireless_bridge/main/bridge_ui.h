#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIDGE_UI_MAX_APS 12

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;
} bridge_ui_ap_t;

typedef struct {
    void (*scan)(void);
    void (*connect)(const char *ssid, const char *password);
    void (*disconnect)(void);
    void (*wifi_enable)(bool enable);
    void (*ble_enable)(bool enable);
} bridge_ui_actions_t;

void bridge_ui_set_actions(const bridge_ui_actions_t *actions);
void bridge_ui_create(void);
void bridge_ui_set_wifi_status(bool connected, const char *ssid, const char *ip);
void bridge_ui_set_cat_status(bool online);
void bridge_ui_set_ble_status(bool online);
void bridge_ui_set_wifi_enabled(bool enabled);
void bridge_ui_set_ble_enabled(bool enabled);
void bridge_ui_set_scan_running(bool running);
void bridge_ui_set_scan_results(const bridge_ui_ap_t *aps, uint8_t count);
void bridge_ui_set_source_text(const char *source, const char *text);

#ifdef __cplusplus
}
#endif

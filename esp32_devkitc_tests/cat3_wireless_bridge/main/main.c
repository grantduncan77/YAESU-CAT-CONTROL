#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bridge_ui.h"
#include "ble_uart.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "i2c_bsp.h"
#include "lvgl_port.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"

#define WIFI_SSID "KE"
#define WIFI_PASSWORD "qazwsxedc"
#define TCP_BRIDGE_PORT 7100
#define UDP_DISCOVERY_PORT 7101

#define CAT_UART_NUM UART_NUM_2
#define CAT_UART_TX_GPIO 43
#define CAT_UART_RX_GPIO 44
#define LEVEL_SHIFTER_OE_GPIO 2
#define CAT_UART_BAUD 38400

static const char *TAG = "cat3_bridge";
static EventGroupHandle_t s_wifi_events;
static int s_tcp_client = -1;
static char s_ip_addr[16] = "0.0.0.0";
static char s_wifi_ssid[33] = WIFI_SSID;
static char s_wifi_password[65] = WIFI_PASSWORD;
static bool s_wifi_started = false;
static bool s_wifi_enabled = true;
static bool s_wifi_auto_reconnect = true;
static bool s_ble_started = false;
static bool s_ble_enabled = true;
static bool s_wifi_scan_running = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1

static void format_printable(const uint8_t *data, size_t len, char *out, size_t out_size);

static void update_source_box(const char *source, const uint8_t *data, size_t len)
{
    char text[96] = {};
    format_printable(data, len, text, sizeof(text));
    bridge_ui_set_source_text(source, text);
}

static void create_task_checked(TaskFunction_t task_func, const char *name, uint32_t stack_depth, UBaseType_t priority)
{
    BaseType_t ok = xTaskCreate(task_func, name, stack_depth, NULL, priority, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task %s, stack=%" PRIu32, name, stack_depth);
    }
}

static void ble_uart_on_rx(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "BLE RX %u bytes: %.*s", (unsigned)len, (int)len, (const char *)data);
    if (s_ble_enabled && data != NULL && len > 0) {
        update_source_box("BLE", data, len);
        uart_write_bytes(CAT_UART_NUM, data, len);
    }
}

static void start_ble_uart(void)
{
    s_ble_enabled = true;
    bridge_ui_set_ble_enabled(true);

    if (s_ble_started) {
        esp_err_t ret = ble_uart_open();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "BLE reopen failed: %s", esp_err_to_name(ret));
            bridge_ui_set_ble_status(false);
            return;
        }
        ESP_LOGI(TAG, "BLE UART advertising resumed");
        bridge_ui_set_ble_status(true);
        return;
    }

    uint8_t mac[6] = {};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    char name[24] = {};
    snprintf(name, sizeof(name), "CAT3-BRIDGE-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(ble_uart_install(&(ble_uart_config_t){
        .device_name = name,
        .encrypted = false,
        .ble_uart_on_rx = ble_uart_on_rx,
    }));
    ESP_ERROR_CHECK(ble_uart_open());
    s_ble_started = true;
    ESP_LOGI(TAG, "BLE UART advertising as %s", name);
    bridge_ui_set_ble_status(true);
}

static void load_wifi_config(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    size_t ssid_len = sizeof(s_wifi_ssid);
    size_t pass_len = sizeof(s_wifi_password);
    if (nvs_get_str(nvs, "ssid", s_wifi_ssid, &ssid_len) != ESP_OK || s_wifi_ssid[0] == '\0') {
        strlcpy(s_wifi_ssid, WIFI_SSID, sizeof(s_wifi_ssid));
    }
    if (nvs_get_str(nvs, "password", s_wifi_password, &pass_len) != ESP_OK) {
        strlcpy(s_wifi_password, WIFI_PASSWORD, sizeof(s_wifi_password));
    }
    nvs_close(nvs);
}

static void save_wifi_config(const char *ssid, const char *password)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "ssid", ssid != NULL ? ssid : "");
    nvs_set_str(nvs, "password", password != NULL ? password : "");
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void connect_wifi_with_current_config(void)
{
    if (!s_wifi_enabled || !s_wifi_started) {
        ESP_LOGW(TAG, "WiFi connect skipped: enabled=%d started=%d", s_wifi_enabled, s_wifi_started);
        return;
    }

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    s_wifi_auto_reconnect = true;
    if (s_wifi_events != NULL) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "WiFi connect start failed: %s", esp_err_to_name(ret));
    }
    bridge_ui_set_wifi_status(false, s_wifi_ssid, "connecting");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        snprintf(s_ip_addr, sizeof(s_ip_addr), "0.0.0.0");
        if (s_wifi_events != NULL) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        bridge_ui_set_wifi_status(false, s_wifi_ssid, s_ip_addr);
        if (s_wifi_enabled && s_wifi_auto_reconnect) {
            ESP_LOGW(TAG, "WiFi disconnected, retrying");
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "WiFi disconnected by touch UI or disabled");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi connected: SSID=%s IP=%s", s_wifi_ssid, s_ip_addr);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        bridge_ui_set_wifi_status(true, s_wifi_ssid, s_ip_addr);
    }
}

static void start_wifi_sta(void)
{
    s_wifi_enabled = true;
    bridge_ui_set_wifi_enabled(true);

    if (s_wifi_started) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
        connect_wifi_with_current_config();
        return;
    }

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(event_ret);
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    connect_wifi_with_current_config();
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    bridge_ui_set_scan_running(true);
    ESP_LOGI(TAG, "WiFi scan requested from touch UI");
    esp_wifi_scan_stop();
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        s_wifi_scan_running = false;
        bridge_ui_set_scan_running(false);
        vTaskDelete(NULL);
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count > BRIDGE_UI_MAX_APS) {
        ap_count = BRIDGE_UI_MAX_APS;
    }
    wifi_ap_record_t records[BRIDGE_UI_MAX_APS] = {};
    if (ap_count > 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&ap_count, records));
    }

    bridge_ui_ap_t aps[BRIDGE_UI_MAX_APS] = {};
    for (uint16_t i = 0; i < ap_count; ++i) {
        strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
        aps[i].rssi = records[i].rssi;
        aps[i].channel = records[i].primary;
        aps[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    bridge_ui_set_scan_results(aps, (uint8_t)ap_count);
    s_wifi_scan_running = false;
    vTaskDelete(NULL);
}

static void ui_scan_requested(void)
{
    if (s_wifi_scan_running) {
        ESP_LOGI(TAG, "WiFi scan already running; repeated touch ignored");
        return;
    }
    if (!s_wifi_enabled || !s_wifi_started) {
        start_wifi_sta();
    }
    s_wifi_scan_running = true;
    create_task_checked(wifi_scan_task, "wifi_scan", 4096, 4);
}

static void ui_connect_requested(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGW(TAG, "Ignoring empty SSID from touch UI");
        return;
    }
    strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_password, password != NULL ? password : "", sizeof(s_wifi_password));
    save_wifi_config(s_wifi_ssid, s_wifi_password);
    if (!s_wifi_enabled || !s_wifi_started) {
        start_wifi_sta();
        return;
    }
    connect_wifi_with_current_config();
}

static void ui_disconnect_requested(void)
{
    ESP_LOGI(TAG, "WiFi disconnect requested from touch UI");
    s_wifi_auto_reconnect = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    snprintf(s_ip_addr, sizeof(s_ip_addr), "0.0.0.0");
    bridge_ui_set_wifi_status(false, s_wifi_ssid, s_ip_addr);
}

static void ui_wifi_enable_requested(bool enable)
{
    ESP_LOGI(TAG, "WiFi %s requested from touch UI", enable ? "enable" : "disable");
    if (enable) {
        start_wifi_sta();
        return;
    }

    s_wifi_enabled = false;
    s_wifi_auto_reconnect = false;
    if (s_tcp_client >= 0) {
        close(s_tcp_client);
        s_tcp_client = -1;
    }
    if (s_wifi_started) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    }
    if (s_wifi_events != NULL) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    }
    snprintf(s_ip_addr, sizeof(s_ip_addr), "0.0.0.0");
    bridge_ui_set_wifi_enabled(false);
    bridge_ui_set_wifi_status(false, s_wifi_ssid, "disabled");
}

static void ui_ble_enable_requested(bool enable)
{
    ESP_LOGI(TAG, "BLE %s requested from touch UI", enable ? "enable" : "disable");
    if (enable) {
        start_ble_uart();
        return;
    }

    s_ble_enabled = false;
    if (s_ble_started) {
        esp_err_t ret = ble_uart_close();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "BLE close failed: %s", esp_err_to_name(ret));
        }
    }
    bridge_ui_set_ble_enabled(false);
    bridge_ui_set_ble_status(false);
}

static void start_cat_uart(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(LEVEL_SHIFTER_OE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(LEVEL_SHIFTER_OE_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LEVEL_SHIFTER_OE_GPIO, 1));

    const uart_config_t config = {
        .baud_rate = CAT_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CAT_UART_NUM, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CAT_UART_NUM, &config));
    ESP_ERROR_CHECK(uart_set_pin(CAT_UART_NUM, CAT_UART_TX_GPIO, CAT_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(gpio_pullup_en(CAT_UART_RX_GPIO));
    ESP_LOGI(TAG, "CAT UART2 ready: TX=%d RX=%d baud=%d, TXS0108E OE=%d high",
             CAT_UART_TX_GPIO, CAT_UART_RX_GPIO, CAT_UART_BAUD, LEVEL_SHIFTER_OE_GPIO);
}

static void cat_uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[256] = {};
    while (true) {
        int uart_len = uart_read_bytes(CAT_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (uart_len > 0) {
            ESP_LOGI(TAG, "CAT UART RX %d bytes", uart_len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, uart_len, ESP_LOG_DEBUG);
            update_source_box("CAT", buf, uart_len);
            bridge_ui_set_cat_status(true);
            if (s_tcp_client >= 0) {
                send(s_tcp_client, buf, uart_len, 0);
            }
            if (s_ble_enabled) {
                ble_uart_tx(buf, uart_len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static size_t cat_query(const char *cmd, uint8_t *resp, size_t resp_size, TickType_t timeout)
{
    if (resp == NULL || resp_size == 0) {
        return 0;
    }
    uart_flush_input(CAT_UART_NUM);
    uart_write_bytes(CAT_UART_NUM, cmd, strlen(cmd));
    uart_wait_tx_done(CAT_UART_NUM, pdMS_TO_TICKS(100));

    size_t used = 0;
    const TickType_t deadline = xTaskGetTickCount() + timeout;
    while (xTaskGetTickCount() < deadline && used < resp_size) {
        uint8_t ch = 0;
        int got = uart_read_bytes(CAT_UART_NUM, &ch, 1, pdMS_TO_TICKS(40));
        if (got == 1) {
            resp[used++] = ch;
            if (ch == ';') {
                return used;
            }
        }
    }
    return used;
}

static void format_printable(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < out_size; ++i) {
        out[n++] = (data[i] >= 0x20 && data[i] <= 0x7e) ? (char)data[i] : '.';
    }
    out[n] = '\0';
}

static bool cat_response_matches(const char *cmd, const uint8_t *resp, size_t len)
{
    return len >= 3 && resp[0] == (uint8_t)cmd[0] && resp[1] == (uint8_t)cmd[1] && resp[len - 1] == ';';
}

static void cat_probe_task(void *arg)
{
    (void)arg;
    const char *queries[] = {"ID;", "FA;", "FB;", "IF;"};
    const uint32_t intervals_ms[] = {20, 50, 80, 100, 150, 200, 250, 300, 400, 500};
    const uint32_t rounds_per_interval = 3;
    uint8_t resp[96] = {};
    char printable[96] = {};
    while (true) {
        ESP_LOGI(TAG, "CAT interval sweep: command gap 20..500 ms, %u rounds each",
                 (unsigned)rounds_per_interval);
        for (size_t gap_i = 0; gap_i < sizeof(intervals_ms) / sizeof(intervals_ms[0]); ++gap_i) {
            uint32_t gap_ms = intervals_ms[gap_i];
            unsigned ok = 0;
            unsigned total = 0;
            ESP_LOGI(TAG, "CAT gap %lu ms test begin", (unsigned long)gap_ms);
            for (uint32_t round = 0; round < rounds_per_interval; ++round) {
                ESP_LOGI(TAG, "CAT gap %lu ms round %lu begin",
                         (unsigned long)gap_ms, (unsigned long)(round + 1));
                for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
                    size_t got = cat_query(queries[i], resp, sizeof(resp), pdMS_TO_TICKS(800));
                    bool valid = got > 0 && cat_response_matches(queries[i], resp, got);
                    total++;
                    if (valid) {
                        ok++;
                    }
                    if (got > 0) {
                        format_printable(resp, got, printable, sizeof(printable));
                        ESP_LOGI(TAG, "CAT gap %lu ms %s -> %u bytes %s ascii='%s'",
                                 (unsigned long)gap_ms, queries[i], (unsigned)got,
                                 valid ? "OK" : "BAD", printable);
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, resp, got, ESP_LOG_INFO);
                        if (valid) {
                            if (s_ble_enabled) {
                                ble_uart_tx(resp, got);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "CAT gap %lu ms %s -> timeout/no frame",
                                 (unsigned long)gap_ms, queries[i]);
                    }
                    vTaskDelay(pdMS_TO_TICKS(gap_ms));
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            ESP_LOGI(TAG, "CAT gap %lu ms summary: %u/%u valid responses",
                     (unsigned long)gap_ms, ok, total);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        ESP_LOGI(TAG, "CAT interval sweep complete; restarting in 5 seconds");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void discovery_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket failed: errno=%d", errno);
        vTaskDelete(NULL);
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_DISCOVERY_PORT),
        .sin_addr.s_addr = inet_addr("255.255.255.255"),
    };
    char msg[64] = {};
    while (true) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_events);
        if (!s_wifi_enabled || (bits & WIFI_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        snprintf(msg, sizeof(msg), "CAT3BRIDGE %s %d", s_ip_addr, TCP_BRIDGE_PORT);
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&dest, sizeof(dest));
        ESP_LOGI(TAG, "Discovery broadcast: %s", msg);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void tcp_bridge_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "TCP socket failed: errno=%d", errno);
        vTaskDelete(NULL);
    }
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_BRIDGE_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    ESP_ERROR_CHECK(bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(listen(listen_sock, 1) == 0 ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "TCP bridge listening on %s:%d", s_ip_addr, TCP_BRIDGE_PORT);

    uint8_t buf[256] = {};
    while (true) {
        struct sockaddr_in source_addr = {};
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            continue;
        }
        int yes = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        s_tcp_client = sock;
        ESP_LOGI(TAG, "TCP client connected");
        while (true) {
            int rx = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
            if (rx > 0) {
                ESP_LOGI(TAG, "TCP RX %d bytes: %.*s", rx, rx, (char *)buf);
                update_source_box("WIFI", buf, rx);
                if (rx == 18 && memcmp(buf, "P4_WIFI_UART_TEST;", 18) == 0) {
                    ESP_LOGI(TAG, "Ignoring P4 WiFi link self-test; not forwarding it to CAT");
                    const char prefix[] = "WIFI-ECHO:";
                    send(sock, prefix, strlen(prefix), 0);
                    send(sock, buf, rx, 0);
                } else {
                    uart_write_bytes(CAT_UART_NUM, buf, rx);
                }
            } else if (rx == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "TCP recv error: errno=%d", errno);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
        }
        close(sock);
        s_tcp_client = -1;
        ESP_LOGI(TAG, "TCP client disconnected");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_wifi_config();

    i2c_master_Init();
    bridge_ui_set_actions(&(bridge_ui_actions_t){
        .scan = ui_scan_requested,
        .connect = ui_connect_requested,
        .disconnect = ui_disconnect_requested,
        .wifi_enable = ui_wifi_enable_requested,
        .ble_enable = ui_ble_enable_requested,
    });
    lvgl_port_set_ui_create_cb(bridge_ui_create);
    lvgl_port_init();
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
    bridge_ui_set_wifi_status(false, s_wifi_ssid, s_ip_addr);
    bridge_ui_set_cat_status(false);
    bridge_ui_set_ble_status(true);
    bridge_ui_set_wifi_enabled(true);
    bridge_ui_set_ble_enabled(true);

    start_cat_uart();
    start_wifi_sta();

    // Keep the display allocation order aligned with the verified MP3 project:
    // LVGL owns its DMA buffer first, then WiFi/BLE/CAT bridge tasks start.
    // xTaskCreate(cat_probe_task, "cat_probe", 4096, NULL, 5, NULL);
    create_task_checked(cat_uart_rx_task, "cat_uart_rx", 3072, 6);
    create_task_checked(discovery_task, "discovery", 3072, 4);
    create_task_checked(tcp_bridge_task, "tcp_bridge", 4096, 5);
    start_ble_uart();

    bridge_ui_set_wifi_status((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0, s_wifi_ssid, s_ip_addr);
}

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#include "nvs_flash.h"

#define WIFI_SSID "KE"
#define WIFI_PASSWORD "qazwsxedc"
#define TCP_BRIDGE_PORT 7100
#define UDP_DISCOVERY_PORT 7101

#define CAT_UART_NUM UART_NUM_2
#define CAT_UART_TX_GPIO 16
#define CAT_UART_RX_GPIO 17
#define LEVEL_SHIFTER_OE_GPIO 32
#define CAT_UART_BAUD 38400

static const char *TAG = "cat3_bridge";
static EventGroupHandle_t s_wifi_events;
static int s_tcp_client = -1;
static char s_ip_addr[16] = "0.0.0.0";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1

static void ble_uart_on_rx(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "BLE RX %u bytes: %.*s", (unsigned)len, (int)len, (const char *)data);
    const char prefix[] = "BLE-ECHO:";
    ble_uart_tx((const uint8_t *)prefix, strlen(prefix));
    ble_uart_tx(data, len);
}

static void start_ble_uart(void)
{
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
    ESP_LOGI(TAG, "BLE UART advertising as %s", name);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi connected: SSID=%s IP=%s", WIFI_SSID, s_ip_addr);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void start_wifi_sta(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
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
    ESP_LOGI(TAG, "CAT UART2 ready: TX=%d RX=%d baud=%d, TXS0108E OE=%d high",
             CAT_UART_TX_GPIO, CAT_UART_RX_GPIO, CAT_UART_BAUD, LEVEL_SHIFTER_OE_GPIO);
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
        s_tcp_client = sock;
        ESP_LOGI(TAG, "TCP client connected");
        while (true) {
            int rx = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
            if (rx > 0) {
                ESP_LOGI(TAG, "TCP RX %d bytes: %.*s", rx, rx, (char *)buf);
                uart_write_bytes(CAT_UART_NUM, buf, rx);
                const char prefix[] = "WIFI-ECHO:";
                send(sock, prefix, strlen(prefix), 0);
                send(sock, buf, rx, 0);
            } else if (rx == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "TCP recv error: errno=%d", errno);
                break;
            }

            int uart_len = uart_read_bytes(CAT_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10));
            if (uart_len > 0) {
                ESP_LOGI(TAG, "CAT UART RX %d bytes", uart_len);
                send(sock, buf, uart_len, 0);
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

    start_cat_uart();
    start_ble_uart();
    start_wifi_sta();

    xTaskCreate(discovery_task, "discovery", 4096, NULL, 4, NULL);
    xTaskCreate(tcp_bridge_task, "tcp_bridge", 6144, NULL, 5, NULL);
}

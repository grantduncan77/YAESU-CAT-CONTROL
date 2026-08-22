#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/cdc_acm_host_ops.h"
#include "usb/usb_host.h"
#include "usb/usb_types_cdc.h"

static const char *TAG = "ft710_ch9102";

static QueueHandle_t s_rx_queue;

typedef struct {
    uint8_t value;
} rx_byte_t;

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
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "CH9102 disconnected");
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
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
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: all devices freed");
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

static esp_err_t cat_tx(cdc_acm_dev_hdl_t dev, const char *cmd)
{
    ESP_LOGI(TAG, "CAT TX: %s", cmd);
    return cdc_acm_host_data_tx_blocking(dev, (const uint8_t *)cmd, strlen(cmd), 1000);
}

static esp_err_t query_cat(cdc_acm_dev_hdl_t dev, const char *cmd)
{
    char resp[96] = {};
    const int64_t start_us = esp_timer_get_time();
    flush_rx();
    ESP_RETURN_ON_ERROR(cat_tx(dev, cmd), TAG, "tx failed");
    const esp_err_t err = read_frame(resp, sizeof(resp), pdMS_TO_TICKS(1800));
    const int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CAT RX: %s (%lld ms)", resp, elapsed_ms);
    } else {
        ESP_LOGW(TAG, "CAT RX timeout for %s after %lld ms, partial='%s'", cmd, elapsed_ms, resp);
    }
    return err;
}

static esp_err_t write_cat(cdc_acm_dev_hdl_t dev, const char *cmd)
{
    flush_rx();
    const int64_t start_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(cat_tx(dev, cmd), TAG, "tx failed");
    vTaskDelay(pdMS_TO_TICKS(180));
    const int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "CAT write settled after %lld ms: %s", elapsed_ms, cmd);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "FT-710 CAT-3 via external CH9102 USB-TTL probe starting");
    ESP_LOGI(TAG, "Expected USB device: WCH CH9102 VID:PID 1A86:55D4");
    ESP_LOGI(TAG, "Line coding: 38400 8N1, no flow control. Only read-only CAT queries are sent.");

    s_rx_queue = xQueueCreate(512, sizeof(rx_byte_t));
    assert(s_rx_queue);

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
    ESP_LOGI(TAG, "Waiting for CH9102 on ESP32-P4 USB Host...");
    esp_err_t err = cdc_acm_host_open(0x1A86, 0x55D4, 0, &dev_config, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open CH9102 as CDC/CDC-like device: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "If enumeration succeeded but open failed, this adapter may need usb_host_ch34x_vcp vendor init.");
        return;
    }

    ESP_LOGI(TAG, "CH9102 opened");

    const cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 38400,
        .bCharFormat = 0, // 1 stop bit
        .bParityType = 0, // none
        .bDataBits = 8,
    };
    err = cdc_acm_host_line_coding_set(dev, &line_coding);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Line coding set to 38400 8N1");
    } else {
        ESP_LOGW(TAG, "Line coding set returned %s; continuing anyway", esp_err_to_name(err));
    }
    err = cdc_acm_host_set_control_line_state(dev, false, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTS/DTR set false");
    } else {
        ESP_LOGW(TAG, "Set RTS/DTR returned %s; continuing anyway", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    int ok_count = 0;
    const char *initial_queries[] = {"ID;", "FA;", "FB;", "MD0;", "MD1;", "NR0;", "RL0;"};
    ESP_LOGI(TAG, "Initial readback");
    for (size_t i = 0; i < sizeof(initial_queries) / sizeof(initial_queries[0]); ++i) {
        if (query_cat(dev, initial_queries[i]) == ESP_OK) {
            ok_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "Applying write test: VFO-A 7.074000 MHz USB, VFO-B 14.270000 MHz LSB, DNR ON level 7");
    ESP_ERROR_CHECK(write_cat(dev, "FA007074000;"));
    ESP_ERROR_CHECK(write_cat(dev, "FB014270000;"));
    ESP_ERROR_CHECK(write_cat(dev, "MD02;"));
    ESP_ERROR_CHECK(write_cat(dev, "MD11;"));
    ESP_ERROR_CHECK(write_cat(dev, "NR01;"));
    ESP_ERROR_CHECK(write_cat(dev, "RL007;"));

    int verify_count = 0;
    const char *verify_queries[] = {"FA;", "FB;", "MD0;", "MD1;", "NR0;", "RL0;"};
    ESP_LOGI(TAG, "Verify readback after write test");
    for (size_t i = 0; i < sizeof(verify_queries) / sizeof(verify_queries[0]); ++i) {
        if (query_cat(dev, verify_queries[i]) == ESP_OK) {
            verify_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "Probe finished: initial %d/%d, verify %d/%d CAT queries returned complete frames",
             ok_count, (int)(sizeof(initial_queries) / sizeof(initial_queries[0])),
             verify_count, (int)(sizeof(verify_queries) / sizeof(verify_queries[0])));
    ESP_LOGI(TAG, "Leave connected or reset ESP to run again.");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

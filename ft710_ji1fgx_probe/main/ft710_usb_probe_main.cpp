#include <stdio.h>
#include <string.h>

#include <memory>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_cp210x.hpp"

using namespace esp_usb;

namespace {

static const char *TAG = "ft710_probe";

static QueueHandle_t s_rx_queue;
static usb_phy_handle_t s_usb_phy;

static constexpr bool kUseFullSpeedRootPortWorkaround = false;

struct RxByte {
    uint8_t value;
};

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    (void)arg;
    for (size_t i = 0; i < data_len; ++i) {
        const RxByte b = {.value = data[i]};
        xQueueSend(s_rx_queue, &b, 0);
    }
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error, err_no=%d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "FT-710 USB device disconnected");
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state notification: 0x%04X", event->data.serial_state.val);
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

static esp_err_t read_frame(char *out, size_t out_size, TickType_t timeout_ticks)
{
    if (out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t pos = 0;
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (xTaskGetTickCount() < deadline) {
        RxByte b = {};
        const TickType_t now = xTaskGetTickCount();
        const TickType_t wait = (deadline > now) ? (deadline - now) : 0;
        if (xQueueReceive(s_rx_queue, &b, wait) == pdTRUE) {
            if (pos + 1 < out_size) {
                out[pos++] = static_cast<char>(b.value);
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

static void flush_rx_queue(void)
{
    RxByte b = {};
    while (xQueueReceive(s_rx_queue, &b, 0) == pdTRUE) {
    }
}

static void send_cat(CdcAcmDevice *vcp, const char *cmd)
{
    ESP_LOGI(TAG, "CAT TX: %s", cmd);
    uint8_t tx_buf[32] = {};
    const size_t len = strlen(cmd);
    assert(len < sizeof(tx_buf));
    memcpy(tx_buf, cmd, len);
    ESP_ERROR_CHECK(vcp->tx_blocking(tx_buf, len));
}

static esp_err_t query_cat(CdcAcmDevice *vcp, const char *cmd, char *resp, size_t resp_size)
{
    flush_rx_queue();
    send_cat(vcp, cmd);
    const esp_err_t err = read_frame(resp, resp_size, pdMS_TO_TICKS(1500));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CAT RX: %s", resp);
    } else {
        ESP_LOGW(TAG, "CAT RX timeout for %s", cmd);
    }
    return err;
}

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "FT-710 USB Host CAT probe starting");
    ESP_LOGI(TAG, "Safety: only Enhanced VCP auto-open path, no PTT/TX commands, RTS/DTR false");
    if (kUseFullSpeedRootPortWorkaround) {
        ESP_LOGI(TAG, "USB workaround trial: force ESP32-P4 OTG1.1 Full-Speed root port to avoid HS hub TT");
    } else {
        ESP_LOGI(TAG, "USB mode: default ESP32-P4 High-Speed root port");
    }

    s_rx_queue = xQueueCreate(512, sizeof(RxByte));
    assert(s_rx_queue);

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    if (kUseFullSpeedRootPortWorkaround) {
        const usb_phy_config_t phy_config = {
            .controller = USB_PHY_CTRL_OTG,
            .target = USB_PHY_TARGET_INT,
            .otg_mode = USB_OTG_MODE_HOST,
            .otg_speed = USB_PHY_SPEED_FULL,
            .ext_io_conf = nullptr,
            .otg_io_conf = nullptr,
        };
        ESP_ERROR_CHECK(usb_new_phy(&phy_config, &s_usb_phy));
        host_config.skip_phy_setup = true;
        host_config.root_port_unpowered = true;
        host_config.peripheral_map = BIT1;
    }

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    if (kUseFullSpeedRootPortWorkaround) {
        ESP_LOGI(TAG, "Power-cycle Full-Speed root port before enumeration");
        ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(false));
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(true));
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, nullptr, 10, nullptr);
    assert(task_created == pdTRUE);

    ESP_ERROR_CHECK(cdc_acm_host_install(nullptr));
    VCP::register_driver<CP210x>();

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 20000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = nullptr,
    };

    ESP_LOGI(TAG, "Waiting for FT-710 CP2105 VCP device...");
    auto vcp = std::unique_ptr<CdcAcmDevice>(VCP::open(&dev_config));
    if (vcp == nullptr) {
        ESP_LOGE(TAG, "Failed to open CP210x VCP device");
        return;
    }

    ESP_LOGI(TAG, "CP210x VCP opened");
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 38400,
        .bCharFormat = 2, // 2 stop bits
        .bParityType = 0, // none
        .bDataBits = 8,
    };
    ESP_ERROR_CHECK(vcp->line_coding_set(&line_coding));
    ESP_ERROR_CHECK(vcp->set_control_line_state(false, false));
    ESP_LOGI(TAG, "Line coding set to 38400 8N2, RTS/DTR false");

    vTaskDelay(pdMS_TO_TICKS(300));

    char resp[96] = {};
    int ok_count = 0;
    const char *commands[] = {"ID;", "FA;", "FB;", "MD0;", "MD1;", "PC;", "NR0;", "RL0;"};
    for (const char *cmd : commands) {
        if (query_cat(vcp.get(), cmd, resp, sizeof(resp)) == ESP_OK) {
            ok_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "Probe finished: %d/%d CAT queries returned complete ';' terminated frames",
             ok_count, static_cast<int>(sizeof(commands) / sizeof(commands[0])));
    ESP_LOGI(TAG, "Leave FT-710 connected or reset ESP to run probe again.");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

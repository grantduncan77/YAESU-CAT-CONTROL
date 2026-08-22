#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ft710_uart";

enum {
    FT710_UART_PORT = UART_NUM_1,
    FT710_UART_TX_GPIO = 27,
    FT710_UART_RX_GPIO = 26,
    FT710_UART_BAUD = 38400,
};

static void uart_send_cat(const char *cmd)
{
    ESP_LOGI(TAG, "CAT TX: %s", cmd);
    uart_write_bytes(FT710_UART_PORT, cmd, strlen(cmd));
    uart_wait_tx_done(FT710_UART_PORT, pdMS_TO_TICKS(200));
}

static esp_err_t uart_read_frame(char *out, size_t out_size, TickType_t timeout_ticks)
{
    if (out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t pos = 0;
    const TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (xTaskGetTickCount() < deadline) {
        uint8_t byte = 0;
        const TickType_t now = xTaskGetTickCount();
        const TickType_t wait = (deadline > now) ? (deadline - now) : 0;
        const int read = uart_read_bytes(FT710_UART_PORT, &byte, 1, wait);
        if (read == 1) {
            if (pos + 1 < out_size) {
                out[pos++] = (char)byte;
            }
            if (byte == ';') {
                out[pos] = '\0';
                return ESP_OK;
            }
        }
    }

    out[pos] = '\0';
    return ESP_ERR_TIMEOUT;
}

static esp_err_t query_cat(const char *cmd, char *resp, size_t resp_size)
{
    uart_flush_input(FT710_UART_PORT);
    uart_send_cat(cmd);
    const esp_err_t err = uart_read_frame(resp, resp_size, pdMS_TO_TICKS(1500));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CAT RX: %s", resp);
    } else {
        ESP_LOGW(TAG, "CAT RX timeout for %s, partial='%s'", cmd, resp);
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "FT-710 CAT-3 UART probe starting");
    ESP_LOGI(TAG, "Safety: read-only CAT commands, no PTT/TX, UART1 TX GPIO%d RX GPIO%d",
             FT710_UART_TX_GPIO, FT710_UART_RX_GPIO);
    ESP_LOGW(TAG, "FT-710 CAT-3 is 5V TTL. Use a level shifter before connecting to ESP32-P4 GPIO.");

    const uart_config_t uart_config = {
        .baud_rate = FT710_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(FT710_UART_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FT710_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FT710_UART_PORT, FT710_UART_TX_GPIO, FT710_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    vTaskDelay(pdMS_TO_TICKS(500));

    char resp[96] = {0};
    int ok_count = 0;
    const char *commands[] = {"ID;", "FA;", "FB;", "MD0;", "MD1;", "PC;", "NR0;", "RL0;"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (query_cat(commands[i], resp, sizeof(resp)) == ESP_OK) {
            ok_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "Probe finished: %d/%d CAT queries returned complete ';' terminated frames",
             ok_count, (int)(sizeof(commands) / sizeof(commands[0])));

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

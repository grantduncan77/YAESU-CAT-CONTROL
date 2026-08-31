idf_component_register(
    SRCS app_controller.c
    INCLUDE_DIRS include
    REQUIRES esp_driver_i2c esp_event esp_netif esp_timer esp_wifi esp32_p4_wifi6_touch_lcd_7b
             lvgl__lvgl lwip nvs_flash radio_state usb_host_cdc_acm)

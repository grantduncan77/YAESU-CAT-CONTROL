idf_component_register(SRCS "main.c" "bridge_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES ble_uart esp_event esp_netif esp_wifi nvs_flash driver lwip waveshare_s3_349)

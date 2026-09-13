idf_component_register(SRCS "board_self_test.c" "rc522.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_spi esp_event esp_http_client esp_lcd
                                esp_netif esp_psram esp_system esp_wifi spiffs
                                espressif__cjson espressif__led_strip espressif__network_provisioning
                                nvs_flash spi_flash)

spiffs_create_partition_image(audio audio FLASH_IN_PROJECT)

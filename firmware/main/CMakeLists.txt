idf_component_register(SRCS "board_self_test.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_spi esp_event esp_http_client esp_lcd
                                esp_netif esp_psram esp_system esp_wifi
                                espressif__cjson espressif__led_strip nvs_flash spi_flash)

idf_component_register(SRCS "main.c" "browser.c" "remote.c" "settings.c"
                        EMBED_FILES "c6_fw_2_12_12.bin"
                        INCLUDE_DIRS "."
                        REQUIRES driver esp_driver_i2c esp_wifi esp_wifi_remote esp_hosted esp_netif esp_event
                                   esp_http_client esp_http_server nvs_flash app_update esp_app_format
                                   esp_lcd lvgl esp_timer esp_psram esp_system heap log mbedtls esp_mm
                                   esp_lcd_touch esp_lcd_touch_gsl3680 esp_driver_jpeg lwip)

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES nvs_flash esp_driver_gpio esp_wifi esp_http_server mqtt
                                  esp_http_client esp-tls esp_netif esp_event esp_timer
                                  lwip mbedtls libsodium)

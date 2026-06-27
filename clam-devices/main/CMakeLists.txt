idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ""
                    PRIV_REQUIRES nvs_flash esp_driver_gpio esp_wifi esp_http_server mqtt)

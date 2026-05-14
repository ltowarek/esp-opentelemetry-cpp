idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS ""
                    REQUIRES
                    esp-opentelemetry-cpp
                    esp_http_client
                    esp_wifi
                    esp_event
                    esp_netif
                    pthread
                    nvs_flash)

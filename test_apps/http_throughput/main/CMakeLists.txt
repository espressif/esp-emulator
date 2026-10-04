idf_component_register(
    SRCS "http_throughput_main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_netif esp_event esp_timer esp_http_client esp_wifi esp_eth
)

idf_component_register(SRCS "main.c" "Src/wifi_sta.c" "Src/depth_spi.c" "Src/power_udp.c" "Src/depth_udp.c" "Src/stream_tasks.c" "Src/init.c" "Src/i2c.c" "Src/ina226.c"
                       PRIV_REQUIRES spi_flash esp_timer driver esp_wifi esp_netif esp_event nvs_flash lwip
                       INCLUDE_DIRS ".")

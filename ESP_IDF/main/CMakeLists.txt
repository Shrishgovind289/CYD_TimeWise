idf_component_register(
    SRCS 
        "main.c"
        "NTPClock.c" 
        "WeatherStation.c"
        "TFT_Display.c"
        "SDCard.c"
        "Audio.c"
        "sun.c"
        "moon.c"
        "WebSocketControl.c"
       
       
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        json
        mbedtls
        nvs_flash
        esp_netif
        esp_event
        lwip
        esp_timer
        driver
        esp_lcd
        esp_http_client
        fatfs
        sdmmc
    PRIV_REQUIRES
        esp_http_server
)                       
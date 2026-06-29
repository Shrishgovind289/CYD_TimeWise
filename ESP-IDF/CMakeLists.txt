idf_component_register(
    SRCS 
        "main.c"
        "NTPClock.c" 
        "WeatherStation.c"
        "TFT_Display.c"
        "SDCard.c"
        "Audio.c"
        "sun.c"
       
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
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
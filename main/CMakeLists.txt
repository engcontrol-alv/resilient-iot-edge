# =============================================================================
# Resilient IoT Edge — main component
# =============================================================================
idf_component_register(
    SRCS
        "main.c"
        "storage_driver.c"       # [DEV1] LittleFS Write-First driver
        "wifi_manager.c"         # [DEV2] Wi-Fi AP/STA + NVS credentials
        "captive_portal.c"       # [DEV2] HTTP server + DNS responder
        "oled_driver.c"
        "mqtt_manager.c"         # [DEV4] Cliente MQTT
    INCLUDE_DIRS
        "."
    REQUIRES
        freertos
        esp_timer
        driver                   # Legacy I2C (driver/i2c.h)
        esp_driver_gpio          # driver/gpio.h — explicit in ESP-IDF 6.x
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        lwip
        mqtt                 
)
# No REQUIRES: the main component automatically depends on every IDF
# component, which keeps this file working on ESP-IDF 5.2 through 6.x.
idf_component_register(
    SRCS 
          "main.c" 
          "app_state.c" 
          "access.c" 
          "rooms.c" 
          "schedule.c"
          "storage.c" 
          "rtc_ds1307.c" 
          "lcd_i2c.c" 
          "hw.c" 
          "console.c"
    INCLUDE_DIRS "." 
    REQUIRES 
          mbedtls  
          driver 
          nvs_flash 
          esp_driver_i2c
          esp_driver_gpio
          fatfs
          esp_wifi
          esp_netif
          esp_timer
          esp_driver_ledc
          esp_adc
          esp_driver_uart

)

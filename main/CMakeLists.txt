idf_component_register(SRCS "barrier_usb_main.c" "led.c"
                       PRIV_REQUIRES spi_flash esp_wifi esp_event mbedtls
                       INCLUDE_DIRS "")

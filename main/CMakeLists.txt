idf_component_register(SRCS "hello_world_main.c" "led.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "")

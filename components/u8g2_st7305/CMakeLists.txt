idf_component_register(
    SRCS "u8g2_st7305.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_spi
        u8g2
)

idf_component_register(
    SRCS "main.c" "display.c" "led.c" "session.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_driver_spi esp_driver_gpio json esp_timer
)

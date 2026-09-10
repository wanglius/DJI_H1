idf_component_register(
    SRCS "dtu_uart_bridge_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_driver_usb_serial_jtag
)

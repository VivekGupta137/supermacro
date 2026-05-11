idf_component_register(SRCS "perf_log.c" "macpass_spi.c" "macpass_usb.c" "main.c"
                       PRIV_REQUIRES usb espressif__usb_host_hid esp_driver_gpio esp_driver_spi esp_timer
                       INCLUDE_DIRS "")

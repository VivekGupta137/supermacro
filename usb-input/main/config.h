// External project dependencies
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "errno.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_crc.h"
#include "esp_timer.h"

// Local dependencies
#include "macpass_hid.h"
#include "macpass_spi.h"
#include "macpass_usb.h"

// Define title for logging: for UART debug purposes
#define LOG_TITLE "MacroPassthrough"

// Enable debug log, (it impacts performance)
#define DEBUG_LOG 0

// Throttled HID input stats only (one line per window); independent of DEBUG_LOG
#define USB_INPUT_PERF_LOG_ENABLE 1
#define USB_INPUT_PERF_LOG_WINDOW_MS 1000

// USB host: wait for hub/peripheral power before usb_host_install() (usb_lib_task).
#ifndef USB_HOST_BOOT_DELAY_MS
#define USB_HOST_BOOT_DELAY_MS 800
#endif
// Defer SPI3 slave (LED path) until root enumeration can finish without DMA contention.
#ifndef USB_SPI_SLAVE_DEFER_MS
#define USB_SPI_SLAVE_DEFER_MS 500
#endif

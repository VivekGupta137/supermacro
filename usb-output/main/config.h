// External project dependencies
#include <stdlib.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_crc.h"
#include "esp_timer.h"

// Local dependencies
#include "macpass_spi.h"
#include "macpass_hid.h"
#include "macpass_usb.h"
#include "macpass_macro.h"
#include "macpass_tool.h"
#include "usb_identity.h"

// Define title for logging: for UART debug purposes
#define LOG_TITLE "MacroPassthrough"
#define calc(x) x

// Enable debug log, (it impacts performance)
#define DEBUG_LOG 0

/** Spread macro mouse deltas across USB reports (~125 Hz) between bullet ticks. */
#define HID_MOUSE_DRIP_INTERVAL_US 8000

// Throttled SPI/macro/USB stats (one line per second when enabled). Off by default — UART is slow.
#define USB_OUTPUT_PERF_LOG_ENABLE 1
#define USB_OUTPUT_PERF_LOG_WINDOW_MS 1000

// ---
// --- TinyUSB descriptors
// ---

#define CUSTOM_CONFIG 0
#if CUSTOM_CONFIG
#include "config_custom.h"
#else
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))};
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 4, true, sizeof(hid_report_descriptor), 0x81, 16, 1),
};

// ---
// USER sequence (compile-time default in macro_defaults.c; optional JSON on flash if web UI enabled)
// ---
#if !CUSTOM_CONFIG
#include "macro_defaults.h"
#endif
#endif

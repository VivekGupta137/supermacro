#include "config.h"

#if USB_INPUT_PERF_LOG_ENABLE

#include "perf_log.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "usb/hid_host.h"

static int64_t s_window_start_us;
static int64_t s_last_us;
static uint32_t s_count;
static uint32_t s_kbd;
static uint32_t s_mouse;
static uint32_t s_other;
static uint64_t s_dt_sum_us;
static uint32_t s_dt_samples;

void perf_hid_input_report(uint8_t hid_proto)
{
    int64_t now = esp_timer_get_time();

    if (s_window_start_us == 0) {
        s_window_start_us = now;
    }
    if (s_last_us != 0 && now > s_last_us) {
        s_dt_sum_us += (uint64_t)(now - s_last_us);
        s_dt_samples++;
    }
    s_last_us = now;
    s_count++;

    if (hid_proto == HID_PROTOCOL_KEYBOARD) {
        s_kbd++;
    } else if (hid_proto == HID_PROTOCOL_MOUSE) {
        s_mouse++;
    } else {
        s_other++;
    }

    if ((now - s_window_start_us) < (int64_t)USB_INPUT_PERF_LOG_WINDOW_MS * 1000) {
        return;
    }

    uint32_t avg_dt = s_dt_samples ? (uint32_t)(s_dt_sum_us / s_dt_samples) : 0;
    uint32_t approx_hz = avg_dt ? (1000000U / avg_dt) : 0;

    ESP_LOGI(LOG_TITLE,
             "perf HID %lums: n=%lu kbd=%lu mouse=%lu other=%lu avg_dt=%luus ~%luHz",
             (unsigned long)USB_INPUT_PERF_LOG_WINDOW_MS,
             (unsigned long)s_count,
             (unsigned long)s_kbd,
             (unsigned long)s_mouse,
             (unsigned long)s_other,
             (unsigned long)avg_dt,
             (unsigned long)approx_hz);

    s_window_start_us = now;
    s_count = 0;
    s_kbd = 0;
    s_mouse = 0;
    s_other = 0;
    s_dt_sum_us = 0;
    s_dt_samples = 0;
}

#endif /* USB_INPUT_PERF_LOG_ENABLE */

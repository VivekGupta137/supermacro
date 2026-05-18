#include "config.h"

#if USB_OUTPUT_PERF_LOG_ENABLE

#include "perf_log.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static int64_t s_window_start_us;
static int64_t s_last_us;
static uint32_t s_counts[PERF_STAT_COUNT];
static uint64_t s_dt_sum_us;
static uint32_t s_dt_samples;

void perf_stat_bump(perf_stat_kind_t kind)
{
    if (kind >= PERF_STAT_COUNT) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_window_start_us == 0) {
        s_window_start_us = now;
    }
    if (s_last_us != 0 && now > s_last_us) {
        s_dt_sum_us += (uint64_t)(now - s_last_us);
        s_dt_samples++;
    }
    s_last_us = now;
    s_counts[kind]++;

    if ((now - s_window_start_us) < (int64_t)USB_OUTPUT_PERF_LOG_WINDOW_MS * 1000) {
        return;
    }

    uint32_t avg_dt = s_dt_samples ? (uint32_t)(s_dt_sum_us / s_dt_samples) : 0;
    uint32_t approx_hz = avg_dt ? (1000000U / avg_dt) : 0;

    ESP_LOGI(LOG_TITLE,
             "perf OUT %lums: spi kbd=%lu mouse=%lu | macro start=%lu tick kbd=%lu mouse=%lu | "
             "usb kbd=%lu mouse=%lu | avg_dt=%luus ~%luHz",
             (unsigned long)USB_OUTPUT_PERF_LOG_WINDOW_MS,
             (unsigned long)s_counts[PERF_SPI_KBD],
             (unsigned long)s_counts[PERF_SPI_MOUSE],
             (unsigned long)s_counts[PERF_MACRO_START],
             (unsigned long)s_counts[PERF_MACRO_TICK_KBD],
             (unsigned long)s_counts[PERF_MACRO_TICK_MOUSE],
             (unsigned long)s_counts[PERF_USB_OUT_KBD],
             (unsigned long)s_counts[PERF_USB_OUT_MOUSE],
             (unsigned long)avg_dt,
             (unsigned long)approx_hz);

    s_window_start_us = now;
    memset(s_counts, 0, sizeof(s_counts));
    s_dt_sum_us = 0;
    s_dt_samples = 0;
}

#endif /* USB_OUTPUT_PERF_LOG_ENABLE */

#pragma once

#include <stdint.h>

#ifndef USB_OUTPUT_PERF_LOG_ENABLE
#define USB_OUTPUT_PERF_LOG_ENABLE 0
#endif

#ifndef USB_OUTPUT_PERF_LOG_WINDOW_MS
#define USB_OUTPUT_PERF_LOG_WINDOW_MS 1000
#endif

typedef enum {
    PERF_SPI_KBD = 0,
    PERF_SPI_MOUSE,
    PERF_SPI_OTHER,
    PERF_MACRO_START,
    PERF_MACRO_TICK_KBD,
    PERF_MACRO_TICK_MOUSE,
    PERF_USB_OUT_KBD,
    PERF_USB_OUT_MOUSE,
    PERF_STAT_COUNT,
} perf_stat_kind_t;

#if USB_OUTPUT_PERF_LOG_ENABLE
void perf_stat_bump(perf_stat_kind_t kind);
#else
static inline void perf_stat_bump(perf_stat_kind_t kind)
{
    (void)kind;
}
#endif

#pragma once

#include <stdint.h>

#include "sdkconfig.h"

/** Set via menuconfig (USB output performance); default off for low CPU contention (IMP-9). */
#ifndef USB_OUTPUT_PERF_LOG_ENABLE
#ifdef CONFIG_USB_OUTPUT_PERF_LOG
#define USB_OUTPUT_PERF_LOG_ENABLE 1
#else
#define USB_OUTPUT_PERF_LOG_ENABLE 0
#endif
#endif

#ifndef USB_OUTPUT_PERF_LOG_WINDOW_MS
#if CONFIG_USB_OUTPUT_PERF_LOG
#define USB_OUTPUT_PERF_LOG_WINDOW_MS CONFIG_USB_OUTPUT_PERF_LOG_WINDOW_MS
#else
#define USB_OUTPUT_PERF_LOG_WINDOW_MS 1000
#endif
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

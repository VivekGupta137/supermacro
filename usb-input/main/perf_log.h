#pragma once

#include <stdint.h>

#ifndef USB_INPUT_PERF_LOG_ENABLE
#define USB_INPUT_PERF_LOG_ENABLE 0
#endif

#if USB_INPUT_PERF_LOG_ENABLE
void perf_hid_input_report(uint8_t hid_proto);
#else
static inline void perf_hid_input_report(uint8_t hid_proto)
{
    (void)hid_proto;
}
#endif

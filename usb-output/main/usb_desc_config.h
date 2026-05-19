#pragma once

/**
 * USB device identity (VID/PID and string descriptors).
 * Copy config_custom.h.example → config_custom.h (gitignored) to override.
 *
 * Default mimics a common OEM keyboard+mouse HID (Holtek 04D9:1400), listed in
 * public USB ID databases as "PS/2 keyboard + mouse controller" — not Espressif,
 * not TinyUSB, not Logitech/Razer retail mice.
 */
#if defined(__has_include)
#if __has_include("config_custom.h")
#include "config_custom.h"
#endif
#endif

#ifndef USB_DESC_VID
#define USB_DESC_VID 0x04D9
#endif
#ifndef USB_DESC_PID
#define USB_DESC_PID 0x1400
#endif
#ifndef USB_DESC_BCD_DEVICE
#define USB_DESC_BCD_DEVICE 0x0100
#endif
#ifndef USB_DESC_MANUFACTURER
#define USB_DESC_MANUFACTURER "Holtek Semiconductor, Inc."
#endif
#ifndef USB_DESC_PRODUCT
#define USB_DESC_PRODUCT "PS/2 keyboard + mouse controller"
#endif
#ifndef USB_DESC_INTERFACE
#define USB_DESC_INTERFACE "HID"
#endif

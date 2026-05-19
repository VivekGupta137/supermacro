#pragma once

#include <stdint.h>

#include "tusb.h"

/** Build serial string from chip MAC; call once before tinyusb_driver_install(). */
void usb_identity_init(void);

const tusb_desc_device_t *usb_identity_device(void);
const char **usb_identity_string_table(void);
uint8_t usb_identity_string_count(void);

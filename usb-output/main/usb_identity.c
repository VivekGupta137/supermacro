#include "usb_identity.h"

#include "usb_desc_config.h"

#include <stdio.h>

#include "esp_mac.h"

static char s_lang[] = {0x09, 0x04};
static char s_serial[13];
static const char *s_string_table[5];

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_DESC_VID,
    .idProduct = USB_DESC_PID,
    .bcdDevice = USB_DESC_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

void usb_identity_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);

    s_string_table[0] = s_lang;
    s_string_table[1] = USB_DESC_MANUFACTURER;
    s_string_table[2] = USB_DESC_PRODUCT;
    s_string_table[3] = s_serial;
    s_string_table[4] = USB_DESC_INTERFACE;
}

const tusb_desc_device_t *usb_identity_device(void)
{
    return &s_device;
}

const char **usb_identity_string_table(void)
{
    return s_string_table;
}

uint8_t usb_identity_string_count(void)
{
    return (uint8_t)(sizeof(s_string_table) / sizeof(s_string_table[0]));
}

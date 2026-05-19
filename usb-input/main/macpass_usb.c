// Import global project config
#include "config.h"
#include "perf_log.h"

#define USB_HID_VENDOR_LOGITECH 0x046du
/** G304/G305 Lightspeed USB receiver — also exposes a keyboard HID iface (often unused). */
#define USB_HID_PID_LOGITECH_LS_RECEIVER 0xc53fu
/** G515 wired keyboard — 16-byte report-protocol payload. */
#define USB_HID_PID_LOGITECH_G515 0xc358u

#define MAX_TRACKED_HID_IFACES 8

static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"};

typedef struct
{
    hid_host_device_handle_t handle;
    uint16_t vid;
    uint16_t pid;
    bool in_use;
    bool keyboard_boot_protocol;
} tracked_hid_iface_t;

static tracked_hid_iface_t s_tracked_ifaces[MAX_TRACKED_HID_IFACES];
/** Wired keyboard (e.g. G515) connected — ignore keyboard HID on the mouse dongle. */
static int s_dedicated_keyboard_count = 0;

hid_host_device_handle_t usb_keyboard_handle = NULL;
QueueHandle_t hid_event_queue = NULL;
#if DEBUG_LOG
int64_t report_time;
#endif
#if USB_INPUT_PERF_LOG_ENABLE
static int64_t mouse_pkt_log_last_us = 0;
#endif

static inline int8_t clamp_i16_to_i8(int32_t v)
{
    if (v > 127)
    {
        return 127;
    }
    if (v < -127)
    {
        return -127;
    }
    return (int8_t)v;
}

static void tracked_iface_clear(hid_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
    {
        if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == handle)
        {
            s_tracked_ifaces[i].in_use = false;
            s_tracked_ifaces[i].handle = NULL;
            s_tracked_ifaces[i].vid = 0;
            s_tracked_ifaces[i].pid = 0;
            s_tracked_ifaces[i].keyboard_boot_protocol = false;
        }
    }
}

static void tracked_iface_store(hid_host_device_handle_t handle)
{
    tracked_iface_clear(handle);
    hid_host_dev_info_t info = {0};
    if (hid_host_get_device_info(handle, &info) != ESP_OK)
    {
        return;
    }
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
    {
        if (!s_tracked_ifaces[i].in_use)
        {
            s_tracked_ifaces[i].in_use = true;
            s_tracked_ifaces[i].handle = handle;
            s_tracked_ifaces[i].vid = info.VID;
            s_tracked_ifaces[i].pid = info.PID;
            ESP_LOGI(LOG_TITLE, "HID iface VID:PID %04x:%04x", (unsigned)info.VID, (unsigned)info.PID);
            return;
        }
    }
    ESP_LOGW(LOG_TITLE, "HID iface tracking table full");
}

static uint16_t tracked_iface_vid(hid_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
    {
        if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == handle)
        {
            return s_tracked_ifaces[i].vid;
        }
    }
    return 0;
}

static uint16_t tracked_iface_pid(hid_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
    {
        if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == handle)
        {
            return s_tracked_ifaces[i].pid;
        }
    }
    return 0;
}

static bool is_logitech_ls_receiver(uint16_t vid, uint16_t pid)
{
    return vid == USB_HID_VENDOR_LOGITECH && pid == USB_HID_PID_LOGITECH_LS_RECEIVER;
}

static bool keyboard_iface_is_ls_receiver(hid_host_device_handle_t handle)
{
    return is_logitech_ls_receiver(tracked_iface_vid(handle), tracked_iface_pid(handle));
}

static bool keyboard_boot_active(hid_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
    {
        if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == handle)
        {
            return s_tracked_ifaces[i].keyboard_boot_protocol;
        }
    }
    return false;
}

/** Boot with no reserved byte: [modifier][key0..key5] (common on Logitech after SET_PROTOCOL BOOT). */
static void keyboard_compact_boot_to_standard(const uint8_t *payload, hid_keyboard_report_t *out)
{
    memset(out, 0, sizeof(*out));
    out->modifier = payload[0];
    memcpy(out->keycode, &payload[1], 6);
}

static void keyboard_led_handle_refresh(void)
{
    usb_keyboard_handle = NULL;
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
    {
        if (!s_tracked_ifaces[i].in_use)
        {
            continue;
        }
        hid_host_dev_params_t p = {0};
        if (hid_host_device_get_params(s_tracked_ifaces[i].handle, &p) != ESP_OK ||
            p.proto != HID_PROTOCOL_KEYBOARD)
        {
            continue;
        }
        bool ls = is_logitech_ls_receiver(s_tracked_ifaces[i].vid, s_tracked_ifaces[i].pid);
        if (s_dedicated_keyboard_count > 0 && ls)
        {
            continue;
        }
        usb_keyboard_handle = s_tracked_ifaces[i].handle;
        return;
    }
}

/** HID++ feature traffic — not pointer movement. */
static bool mouse_raw_is_hidpp(const uint8_t *data, size_t data_length)
{
    if (data_length < 1)
    {
        return true;
    }
    uint8_t id = data[0];
    return (id == 0x10 || id == 0x11);
}

static void spi_emit_mouse_report(const hid_mouse_report_t *mouse)
{
    hid_report_t report = {0};
    report.mouse = *mouse;
    spi_send_master_hid_sender(HEADER_HID_MOUSE, &report);
}

/**
 * Logitech G304 / G305 via Lightspeed receiver (046d:c53f) — HID report ID 2, 9 bytes wire.
 * Matches Linux hid-logitech-dj.c mse_high_res_descriptor[] (recvr_type_gaming_hidpp):
 *   byte0: report ID 0x02
 *   bytes1-2: 16-bit button bits (boot: low 5)
 *   bytes3-4: X int16 LE
 *   bytes5-6: Y int16 LE
 *   byte7: wheel int8
 *   byte8: pan int8 (not forwarded)
 */
static bool try_parse_logitech_g304_report2(const uint8_t *data, size_t data_length, hid_mouse_report_t *out)
{
    if (data_length != 9 || data[0] != 0x02)
    {
        return false;
    }

    const uint8_t *p = data + 1;
    uint16_t btn16 = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    out->buttons = btn16 & 0x1F;
    out->pan = 0;

    int16_t x = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
    int16_t y = (int16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
    out->wheel = (int8_t)p[6];
    out->x = clamp_i16_to_i8(x);
    out->y = clamp_i16_to_i8(y);
    return true;
}

#if USB_INPUT_PERF_LOG_ENABLE
/** One sample per USB_INPUT_PERF_LOG_WINDOW_MS (same throttle as perf HID line). */
static void mouse_pkt_log_sample(const uint8_t *data, size_t data_length)
{
    if (data_length == 9 && data[0] == 0x02)
    {
        const uint8_t *p = data + 1;
        int16_t x16 = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        int16_t y16 = (int16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
        hid_mouse_report_t parsed = {0};
        try_parse_logitech_g304_report2(data, data_length, &parsed);
        ESP_LOGI(LOG_TITLE,
                 "mouse pkt: len=9 %02X %02X %02X %02X %02X %02X %02X %02X %02X | x16=%d y16=%d btn=%02X spi x=%d y=%d wh=%d",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], (int)x16,
                 (int)y16, parsed.buttons, (int)parsed.x, (int)parsed.y, (int)parsed.wheel);
    }
    else
    {
        ESP_LOGI(LOG_TITLE, "mouse pkt: len=%u %02X %02X %02X %02X %02X", (unsigned)data_length,
                 data_length > 0 ? data[0] : 0, data_length > 1 ? data[1] : 0, data_length > 2 ? data[2] : 0,
                 data_length > 3 ? data[3] : 0, data_length > 4 ? data[4] : 0);
    }
}
#endif

/** Boot modifier bitmask in byte0 (not report IDs 0x03/0x04 consumer). */
static bool keyboard_g515_valid_mod_mask(uint8_t m)
{
    if (m == 0)
    {
        return false;
    }
    return (m & (uint8_t) ~(0x01u | 0x02u | 0x04u | 0x08u | 0x10u | 0x20u | 0x40u | 0x80u)) == 0;
}

/** Key bitmap bytes only (skip byte1 when it holds modifier on report-ID 0x08/0x10/0x11). */
static bool keyboard_g515_key_bitmap_active(const uint8_t *data)
{
    int start = 1;
    const uint8_t b0 = data[0];
    if ((b0 == 0x08 || b0 == 0x10 || b0 == 0x11) && keyboard_g515_valid_mod_mask(data[1]))
    {
        start = 2;
    }
    for (int i = start; i < 15; i++)
    {
        if (data[i] != 0)
        {
            return true;
        }
    }
    return false;
}

static bool keyboard_is_g515_iface(hid_host_device_handle_t handle);

/**
 * HID++ on G515: payload bytes at index 2+.
 * [10|11][modifier][00..] is a modifier report, not HID++.
 */
static bool keyboard_g515_wire_is_hidpp(const uint8_t *data, size_t data_length)
{
    if (data_length != 16 || (data[0] != 0x10 && data[0] != 0x11))
    {
        return false;
    }
    if (keyboard_g515_valid_mod_mask(data[1]) && !keyboard_g515_key_bitmap_active(data))
    {
        return false;
    }
    for (int i = 2; i < 15; i++)
    {
        if (data[i] != 0)
        {
            return true;
        }
    }
    return false;
}

/** Consumer / HID++ report IDs — not key matrix (G515: 0x04/0x08/0x10 may be modifiers). */
static bool keyboard_wire_report_skipped(hid_host_device_handle_t handle, const uint8_t *data, size_t data_length)
{
    if (data_length < 1)
    {
        return true;
    }
    switch (data[0])
    {
    case 0x04:
        /* G515: byte0=0x04 is Left Alt modifier; Alt+Tab is 04 .. 80 .. (bitmap set) — not consumer. */
        if (keyboard_is_g515_iface(handle) && data_length == 16)
        {
            return false;
        }
        return true;
    case 0x03:
        if (keyboard_is_g515_iface(handle) && data_length == 16 && !keyboard_g515_key_bitmap_active(data))
        {
            return false;
        }
        return true;
    case 0x10:
    case 0x11:
        if (keyboard_is_g515_iface(handle) && data_length == 16 && !keyboard_g515_wire_is_hidpp(data, data_length))
        {
            return false;
        }
        return true;
    default:
        return false;
    }
}

/** Latched modifiers; side-channel RID when byte0 is 0x08 / 0x10 / 0x11. */
static uint8_t s_g515_channel_rid;
static uint8_t s_g515_mod_latch;
/** Previous report had key bitmap set (distinguish key-up from modifier-up on 00 00). */
static bool s_g515_had_keys;

/**
 * Resolve boot modifier byte for G515 16-byte reports.
 * Modifiers often arrive in a separate packet before the key bitmap (byte0=0).
 * Do not clear the latch on key-only release (00 00 after a letter); only on
 * modifier-up / full idle when no keys were active in the prior report.
 */
static uint8_t keyboard_g515_resolve_modifier(const uint8_t *data)
{
    const uint8_t b0 = data[0];
    const bool bitmap = keyboard_g515_key_bitmap_active(data);

    if (b0 == 0x08 || b0 == 0x10 || b0 == 0x11)
    {
        if (keyboard_g515_valid_mod_mask(data[1]) && !bitmap)
        {
            s_g515_channel_rid = b0;
            s_g515_mod_latch = data[1];
            s_g515_had_keys = false;
            return s_g515_mod_latch;
        }
        if (data[1] == 0 && !bitmap && s_g515_channel_rid == b0)
        {
            s_g515_channel_rid = 0;
            s_g515_mod_latch = 0;
            s_g515_had_keys = false;
            return 0;
        }
    }

    if (keyboard_g515_valid_mod_mask(b0) && !bitmap)
    {
        s_g515_channel_rid = 0;
        s_g515_mod_latch = b0;
        s_g515_had_keys = false;
        return b0;
    }

    if (bitmap)
    {
        s_g515_had_keys = true;
        const uint8_t row_mod = keyboard_g515_valid_mod_mask(b0) ? b0 : 0;
        s_g515_mod_latch |= row_mod;
        return s_g515_mod_latch;
    }

    if (b0 == 0 && !bitmap)
    {
        if (s_g515_had_keys)
        {
            s_g515_had_keys = false;
            return s_g515_mod_latch;
        }
        s_g515_channel_rid = 0;
        s_g515_mod_latch = 0;
        return 0;
    }

    return s_g515_mod_latch;
}

/** HID keyboard usage 0xE0..0xE7 → boot modifier bitmask. */
static uint8_t keyboard_hid_usage_to_modifier(uint8_t usage)
{
    switch (usage)
    {
    case 0xE0:
        return 0x01;
    case 0xE1:
        return 0x02;
    case 0xE2:
        return 0x04;
    case 0xE3:
        return 0x08;
    case 0xE4:
        return 0x10;
    case 0xE5:
        return 0x20;
    case 0xE6:
        return 0x40;
    case 0xE7:
        return 0x80;
    default:
        return 0;
    }
}

/** Boot keyboard: byte1 reserved==0 and key usages in HID keyboard range. */
static bool keyboard_payload_is_boot(const uint8_t *payload)
{
    if (payload[1] != 0)
    {
        return false;
    }
    for (int i = 2; i < 8; i++)
    {
        if (payload[i] != 0 && (payload[i] < 0x04 || payload[i] > 0xE7))
        {
            return false;
        }
    }
    return true;
}

/**
 * Logitech report-protocol layout (G515, K400, etc.): modifiers + 6-byte key bitmap.
 * Each bit is a keyboard usage (0x00..); TinyUSB expects boot keycode array instead.
 */
static void keyboard_bitmap_to_boot(const uint8_t *payload, hid_keyboard_report_t *out)
{
    memset(out, 0, sizeof(*out));
    out->modifier = payload[0];
    int n = 0;
    for (int bi = 1; bi <= 6 && n < 6; bi++)
    {
        for (int b = 0; b < 8 && n < 6; b++)
        {
            if (payload[bi] & (1u << b))
            {
                uint8_t usage = (uint8_t)((bi - 1) * 8 + b);
                if (usage >= 0x04)
                {
                    out->keycode[n++] = usage;
                }
            }
        }
    }
}

static bool keyboard_is_g515_iface(hid_host_device_handle_t handle)
{
    return tracked_iface_pid(handle) == USB_HID_PID_LOGITECH_G515;
}

/**
 * G515 16-byte report-protocol (empirical; byte 15 = sequence counter):
 *   Modifier-only: byte0 = boot modifier mask (0x01..0x80); 0x04 LAlt, 0x08 Win, 0x40 RAlt.
 *   [10|11][mod][00..] — right-side mods may use report ID in byte0, mask in byte1.
 *   Matrix: byte0 = held modifiers, bytes [1+2p]/[2+2p] key bitmaps.
 */
static void keyboard_g515_report16_to_standard(const uint8_t *data, hid_keyboard_report_t *out)
{
    memset(out, 0, sizeof(*out));
    out->modifier = keyboard_g515_resolve_modifier(data);

    int n = 0;
    for (int p = 0; p < 7 && n < 6; p++)
    {
        const int base = 4 + p * 16;
        const uint8_t lo = data[1 + 2 * p];
        const uint8_t hi = data[2 + 2 * p];
        for (int bit = 0; bit < 8 && n < 6; bit++)
        {
            if (lo & (1u << bit))
            {
                uint8_t usage = (uint8_t)(base + bit);
                if (usage >= 0x04 && usage <= 0xE7)
                {
                    out->keycode[n++] = usage;
                }
            }
        }
        for (int bit = 0; bit < 8 && n < 6; bit++)
        {
            if (hi & (1u << bit))
            {
                uint8_t usage = (uint8_t)(base + 8 + bit);
                if (usage >= 0x04 && usage <= 0xE7)
                {
                    out->keycode[n++] = usage;
                }
            }
        }
    }
}

/** Send when wire changes or decoded boot report changes (release must not be dropped). */
static bool keyboard_g515_report_changed(const uint8_t *data, size_t data_length,
                                         const hid_keyboard_report_t *decoded)
{
    static uint8_t last_wire[16];
    static size_t last_len;
    static uint8_t last_mod;
    static uint8_t last_keys[6];

    size_t n = data_length;
    if (n > sizeof(last_wire))
    {
        n = sizeof(last_wire);
    }
    uint8_t norm[16];
    memcpy(norm, data, n);
    if (n == 16)
    {
        norm[15] = 0;
    }

    const bool wire_changed = (n != last_len) || (memcmp(norm, last_wire, n) != 0);
    const bool state_changed =
        (decoded->modifier != last_mod) || (memcmp(decoded->keycode, last_keys, sizeof(last_keys)) != 0);

    if (!wire_changed && !state_changed)
    {
        return false;
    }

    memcpy(last_wire, norm, n);
    last_len = n;
    last_mod = decoded->modifier;
    memcpy(last_keys, decoded->keycode, sizeof(last_keys));
    return true;
}

static const uint8_t *keyboard_strip_report_id(const uint8_t *data, size_t *plen_io)
{
    if (*plen_io == sizeof(hid_keyboard_report_t) + 1 && data[0] != 0)
    {
        *plen_io = sizeof(hid_keyboard_report_t);
        return data + 1;
    }
    if (*plen_io > sizeof(hid_keyboard_report_t) && data[0] != 0 && data[0] < 0x10)
    {
        *plen_io -= 1;
        return data + 1;
    }
    return data;
}

/**
 * Decode keyboard wire buffer to boot HID report for SPI.
 * Mode: G=G515 16-byte, S=standard boot, C=compact boot, B=bitmap, -=skipped.
 */
static char keyboard_decode_report(hid_host_device_handle_t handle, const uint8_t *data, size_t data_length,
                                   hid_keyboard_report_t *out)
{
    if (keyboard_wire_report_skipped(handle, data, data_length))
    {
        return '-';
    }

    if (keyboard_is_g515_iface(handle))
    {
        if (data_length == sizeof(hid_keyboard_report_t) && keyboard_boot_active(handle))
        {
            memcpy(out, data, sizeof(hid_keyboard_report_t));
            return 'g';
        }
        if (data_length == 16)
        {
            keyboard_g515_report16_to_standard(data, out);
            return 'G';
        }
        return '-';
    }

    if (data_length == 16)
    {
        return '-';
    }

    size_t plen = data_length;
    const uint8_t *payload = keyboard_strip_report_id(data, &plen);
    if (plen < sizeof(hid_keyboard_report_t))
    {
        return '-';
    }

    if (keyboard_payload_is_boot(payload))
    {
        memcpy(out, payload, sizeof(hid_keyboard_report_t));
        return 'S';
    }
    if (keyboard_boot_active(handle))
    {
        keyboard_compact_boot_to_standard(payload, out);
        return 'C';
    }
    keyboard_bitmap_to_boot(payload, out);
    return 'B';
}

#if USB_INPUT_PERF_LOG_ENABLE
/** True if any key-region byte is non-zero (G515: bytes 0..14; boot: mod + key slots). */
static bool keyboard_wire_keys_down(hid_host_device_handle_t handle, const uint8_t *data, size_t data_length)
{
    if (keyboard_wire_report_skipped(handle, data, data_length))
    {
        return false;
    }
    if (data_length == 16)
    {
        if (keyboard_g515_resolve_modifier(data) != 0)
        {
            return true;
        }
        return keyboard_g515_key_bitmap_active(data);
    }
    size_t plen = data_length;
    const uint8_t *payload = keyboard_strip_report_id(data, &plen);
    if (plen < sizeof(hid_keyboard_report_t))
    {
        return false;
    }
    if (payload[0] != 0)
    {
        return true;
    }
    for (int i = 2; i < 8; i++)
    {
        if (payload[i] != 0)
        {
            return true;
        }
    }
    return false;
}

/** Copy wire for compare; byte 15 on 16-byte G515 reports is a sequence counter — ignore it. */
static void keyboard_wire_normalize(const uint8_t *data, size_t data_length, uint8_t *out, size_t *out_len)
{
    size_t n = data_length;
    if (n > 32)
    {
        n = 32;
    }
    memcpy(out, data, n);
    if (data_length == 16)
    {
        out[15] = 0;
    }
    *out_len = n;
}

/**
 * Log raw keyboard wire on key down / up only (no repeat while held).
 * DOWN: new press or changed key matrix while keys still down.
 * UP: all key-region bytes cleared.
 */
static void keyboard_pkt_log_edge(hid_host_device_handle_t handle, const uint8_t *data, size_t data_length)
{
    static uint8_t last_norm[32];
    static size_t last_norm_len;
    static bool last_keys_down;

    if (keyboard_wire_report_skipped(handle, data, data_length))
    {
        return;
    }

    uint8_t norm[32];
    size_t norm_len = 0;
    keyboard_wire_normalize(data, data_length, norm, &norm_len);
    if (norm_len == last_norm_len && memcmp(norm, last_norm, norm_len) == 0)
    {
        return;
    }

    bool keys_down = keyboard_wire_keys_down(handle, data, data_length);
    const char *edge = NULL;
    if (!last_keys_down && keys_down)
    {
        edge = "DOWN";
    }
    else if (last_keys_down && !keys_down)
    {
        edge = "UP";
    }
    else if (last_keys_down && keys_down)
    {
        edge = "DOWN";
    }
    else
    {
        memcpy(last_norm, norm, norm_len);
        last_norm_len = norm_len;
        return;
    }

    hid_keyboard_report_t kb = {0};
    char mode = keyboard_decode_report(handle, data, data_length, &kb);

    if (data_length == 16)
    {
        ESP_LOGI(LOG_TITLE,
                 "kbd %s: len=%u %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | "
                 "mode=%c boot=%d | dec mod=%02X k=%02X %02X %02X %02X %02X %02X",
                 edge, (unsigned)data_length, data[0], data[1], data[2], data[3], data[4], data[5], data[6],
                 data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15], mode,
                 keyboard_boot_active(handle) ? 1 : 0, (unsigned)kb.modifier, (unsigned)kb.keycode[0],
                 (unsigned)kb.keycode[1], (unsigned)kb.keycode[2], (unsigned)kb.keycode[3], (unsigned)kb.keycode[4],
                 (unsigned)kb.keycode[5]);
    }
    else if (data_length > 0)
    {
        ESP_LOGI(LOG_TITLE,
                 "kbd %s: len=%u %02X %02X %02X %02X %02X %02X %02X %02X | mode=%c boot=%d | dec mod=%02X "
                 "k=%02X %02X %02X %02X %02X %02X",
                 edge, (unsigned)data_length, data_length > 0 ? data[0] : 0, data_length > 1 ? data[1] : 0,
                 data_length > 2 ? data[2] : 0, data_length > 3 ? data[3] : 0, data_length > 4 ? data[4] : 0,
                 data_length > 5 ? data[5] : 0, data_length > 6 ? data[6] : 0, data_length > 7 ? data[7] : 0,
                 data_length > 8 ? data[8] : 0, mode, keyboard_boot_active(handle) ? 1 : 0, (unsigned)kb.modifier,
                 (unsigned)kb.keycode[0], (unsigned)kb.keycode[1], (unsigned)kb.keycode[2], (unsigned)kb.keycode[3],
                 (unsigned)kb.keycode[4], (unsigned)kb.keycode[5]);
    }

    memcpy(last_norm, norm, norm_len);
    last_norm_len = norm_len;
    last_keys_down = keys_down;
}
#endif

static void spi_send_keyboard_report(hid_host_device_handle_t handle, const uint8_t *data, size_t data_length)
{
    hid_report_t report = {0};
    if (keyboard_decode_report(handle, data, data_length, &report.keyboard) == '-')
    {
        return;
    }

    if (keyboard_is_g515_iface(handle) && data_length == 16 &&
        !keyboard_g515_report_changed(data, data_length, &report.keyboard))
    {
        return;
    }

    spi_send_master_hid_sender(HEADER_HID_KEYBOARD, &report);
}

static bool spi_send_logitech_mouse(const uint8_t *data, size_t data_length)
{
    if (data_length < 3 || mouse_raw_is_hidpp(data, data_length))
    {
        return false;
    }

    /* Report 3/4/8 are consumer/system/media on this receiver — not pointer data. */
    if (data[0] == 0x03 || data[0] == 0x04 || data[0] == 0x08)
    {
        return false;
    }

    hid_mouse_report_t mouse = {0};
    if (try_parse_logitech_g304_report2(data, data_length, &mouse))
    {
        spi_emit_mouse_report(&mouse);
        return true;
    }

#if DEBUG_LOG
    ESP_LOGW(LOG_TITLE, "Logitech mouse: unhandled pkt len=%u b0=%02X", (unsigned)data_length, data[0]);
#endif
    return false;
}

/**
 * @brief USB HID Host interface callback
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event,
                                 void *arg)
{
    uint8_t data[32] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    switch (event)
    {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(
            hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length));
        perf_hid_input_report((uint8_t)dev_params.proto);
#if USB_INPUT_PERF_LOG_ENABLE
        if (dev_params.proto == HID_PROTOCOL_MOUSE)
        {
            int64_t now_us = esp_timer_get_time();
            int64_t window_us = (int64_t)USB_INPUT_PERF_LOG_WINDOW_MS * 1000;
            if (mouse_pkt_log_last_us == 0 || (now_us - mouse_pkt_log_last_us) >= window_us)
            {
                mouse_pkt_log_sample(data, data_length);
                mouse_pkt_log_last_us = now_us;
            }
        }
        else if (dev_params.proto == HID_PROTOCOL_KEYBOARD &&
                 !(keyboard_iface_is_ls_receiver(hid_device_handle) && s_dedicated_keyboard_count > 0))
        {
            keyboard_pkt_log_edge(hid_device_handle, data, data_length);
        }
#endif
#if DEBUG_LOG
        ESP_LOGI(LOG_TITLE, "HID Report subclass: %d, proto %d, size: %d", dev_params.sub_class, dev_params.proto,
                 data_length);
#endif

#if DEBUG_LOG
        if (data_length == 0)
        {
            break;
        }
#endif
        if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
        {
            uint16_t vid = tracked_iface_vid(hid_device_handle);
            bool logitech = (vid == USB_HID_VENDOR_LOGITECH);
            if (keyboard_iface_is_ls_receiver(hid_device_handle) && s_dedicated_keyboard_count > 0)
            {
                break;
            }
            if (!logitech && HID_SUBCLASS_BOOT_INTERFACE != dev_params.sub_class)
            {
                break;
            }
#if DEBUG_LOG
            {
                hid_keyboard_report_t kb_dbg = {0};
                const uint8_t *kb_p = data;
                size_t kb_len = data_length;
                if (logitech && data_length == 9 && data[0] == 0x01)
                {
                    kb_p = data + 1;
                    kb_len = 8;
                }
                size_t cpy = kb_len;
                if (cpy > sizeof(kb_dbg))
                {
                    cpy = sizeof(kb_dbg);
                }
                memcpy(&kb_dbg, kb_p, cpy);
                if (keycode_contains_key(kb_dbg, HID_KEY_CAPS_LOCK))
                {
                    report_time = esp_timer_get_time();
                }
            }
#endif
            spi_send_keyboard_report(hid_device_handle, data, data_length);
        }
        else if (HID_PROTOCOL_MOUSE == dev_params.proto)
        {
            if (!is_logitech_ls_receiver(tracked_iface_vid(hid_device_handle),
                                         tracked_iface_pid(hid_device_handle)))
            {
                break;
            }
            spi_send_logitech_mouse(data, data_length);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
        if (dev_params.proto == HID_PROTOCOL_KEYBOARD && !keyboard_iface_is_ls_receiver(hid_device_handle))
        {
            if (s_dedicated_keyboard_count > 0)
            {
                s_dedicated_keyboard_count--;
            }
        }
        tracked_iface_clear(hid_device_handle);
        keyboard_led_handle_refresh();
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' TRANSFER_ERROR", hid_proto_name_str[dev_params.proto]);
        break;
    default:
        ESP_LOGE(LOG_TITLE, "HID Device, protocol '%s' Unhandled event", hid_proto_name_str[dev_params.proto]);
        break;
    }
}

/**
 * @brief USB HID Host Device event
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event)
    {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);

        if (dev_params.proto == 0 || dev_params.proto > 2)
        {
            ESP_LOGI(LOG_TITLE, "HID Device, skipped");
            break;
        }

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL};

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        tracked_iface_store(hid_device_handle);
#if DEBUG_LOG
        ESP_LOGI(LOG_TITLE, "DEBUG: handle=%p, sub_class=%d, proto=%d", hid_device_handle, dev_params.sub_class,
                 dev_params.proto);
#endif
        if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
        {
            uint16_t vid = tracked_iface_vid(hid_device_handle);
            uint16_t pid = tracked_iface_pid(hid_device_handle);
            bool ls_receiver = is_logitech_ls_receiver(vid, pid);

            if (!ls_receiver)
            {
                s_dedicated_keyboard_count++;
                ESP_LOGI(LOG_TITLE, "Primary keyboard %04x:%04x (hub + G515 path)", (unsigned)vid,
                         (unsigned)pid);
            }
            else if (s_dedicated_keyboard_count > 0)
            {
                ESP_LOGI(LOG_TITLE, "Lightspeed dongle keyboard iface ignored (use wired keyboard)");
            }
            else
            {
                ESP_LOGI(LOG_TITLE, "Keyboard via Lightspeed receiver %04x:%04x", (unsigned)vid, (unsigned)pid);
            }
            keyboard_led_handle_refresh();
        }
        else if (HID_PROTOCOL_MOUSE == dev_params.proto)
        {
#if DEBUG_LOG
            ESP_LOGI(LOG_TITLE, "DEBUG: Set MOUSE REPORT protocol");
#endif
            esp_err_t proto_ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT);
            if (proto_ret != ESP_OK)
            {
                ESP_LOGW(LOG_TITLE, "Set REPORT protocol failed: %s", esp_err_to_name(proto_ret));
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));

        if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
        {
            uint16_t vid = tracked_iface_vid(hid_device_handle);
            uint16_t pid = tracked_iface_pid(hid_device_handle);
            bool ls_receiver = is_logitech_ls_receiver(vid, pid);

            esp_err_t proto_ret =
                hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
            bool boot_ok = (proto_ret == ESP_OK);
            for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++)
            {
                if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == hid_device_handle)
                {
                    s_tracked_ifaces[i].keyboard_boot_protocol = boot_ok;
                    break;
                }
            }
            if (!boot_ok)
            {
                ESP_LOGW(LOG_TITLE, "Set BOOT protocol failed %04x:%04x: %s (use array16 decode)",
                         (unsigned)vid, (unsigned)pid, esp_err_to_name(proto_ret));
            }
            else if (!ls_receiver)
            {
                ESP_LOGI(LOG_TITLE, "Keyboard BOOT protocol %04x:%04x", (unsigned)vid, (unsigned)pid);
            }
            if (usb_keyboard_handle == hid_device_handle)
            {
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
            }
        }
        break;
    default:
        break;
    }
}

/**
 * @brief HID Host Device callback — queues connect events for hid_lib_task.
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg)
{
    static hid_event_queue_t evt_queue;
    evt_queue.handle = hid_device_handle;
    evt_queue.event = event;
    evt_queue.arg = arg;

    xQueueSend(hid_event_queue, &evt_queue, 0);
}

void hid_host_keyboard_report_output(char report)
{
    if (usb_keyboard_handle != NULL)
    {
#if DEBUG_LOG
        ESP_LOGI(pcTaskGetName(NULL), "CapsLock ping => %lld us", esp_timer_get_time() - report_time);
#endif
        esp_err_t ret =
            hid_class_request_set_report(usb_keyboard_handle, HID_REPORT_TYPE_OUTPUT, 0, (void *)&report, sizeof(char));
        assert(ret == ESP_OK);
    }
}

void usb_lib_task(void *arg)
{
    // Let an external hub finish power-up before the host starts enumerating downstream ports.
    vTaskDelay(pdMS_TO_TICKS(USB_HOST_BOOT_DELAY_MS));

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(LOG_TITLE, "USB host installed (boot delay %d ms)", USB_HOST_BOOT_DELAY_MS);

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = USB_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = USB_TASK_COREID,
        .callback = hid_host_device_callback,
        .callback_arg = NULL};

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    while (true)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(LOG_TITLE, "USB shutdown");
    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

void hid_lib_task(void *arg)
{
    hid_event_queue_t evt_queue;

    while (true)
    {
        if (xQueueReceive(hid_event_queue, &evt_queue, portMAX_DELAY))
        {
            hid_host_device_event(evt_queue.handle, evt_queue.event, evt_queue.arg);
        }
    }
}

void usb_init(void)
{
    memset(s_tracked_ifaces, 0, sizeof(s_tracked_ifaces));
    hid_event_queue = xQueueCreate(10, sizeof(hid_event_queue_t));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, NULL, USB_TASK_PRIORITY, NULL, USB_TASK_COREID);
    xTaskCreatePinnedToCore(hid_lib_task, "hid_events", 4096, NULL, USB_TASK_PRIORITY, NULL, USB_TASK_COREID);
}

#pragma once

#include <math.h>

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"

#define BYTE_TO_BINARY(byte)  \
((byte) & 0x80 ? '1' : '0'), \
((byte) & 0x40 ? '1' : '0'), \
((byte) & 0x20 ? '1' : '0'), \
((byte) & 0x10 ? '1' : '0'), \
((byte) & 0x08 ? '1' : '0'), \
((byte) & 0x04 ? '1' : '0'), \
((byte) & 0x02 ? '1' : '0'), \
((byte) & 0x01 ? '1' : '0')

#define ONE_KEYBOARD_KEY(key) \
{ \
    .header = HEADER_HID_KEYBOARD, \
    .event = {.keyboard = {.keycode = {key}}} \
} \

#define ONE_MOUSE_KEY(key) \
{ \
    .header = HEADER_HID_MOUSE, \
    .event = {.mouse = {.buttons = key}} \
} \

#define TWO_KEYBOARD_KEY(key, key2) \
{ \
    .header = HEADER_HID_KEYBOARD, \
    .event = {.keyboard = {.keycode = {key, key2}}} \
} \

#define EMPTY_KEYBOARD \
{ \
    .header = HEADER_HID_KEYBOARD, \
} \

#define EMPTY_MOUSE \
{ \
    .header = HEADER_HID_MOUSE, \
} \

#define MOUSE_MOUVEMENT(mov_x, mov_y) \
{ \
    .header = HEADER_HID_MOUSE, \
    .event = { \
        .mouse = { \
            .x = mov_x, \
            .y = mov_y, \
        } \
    } \
 }\

static inline void print_keyboard_report(const char* title, const hid_keyboard_report_t report){
    char line[128];
    int offset = snprintf(line, sizeof(line), "Keyboard report [ ");
    for (int i = 0; i < sizeof(hid_keyboard_report_t); i++) {
        if (i == 0) {
            offset += snprintf(line + offset, sizeof(line) - offset, ""BYTE_TO_BINARY_PATTERN"; ", BYTE_TO_BINARY(((char*)&report)[i]));
        } else if (i == 1){
            offset += snprintf(line + offset, sizeof(line) - offset, "%02X; ", ((char*)&report)[i]);
        } else {
            offset += snprintf(line + offset, sizeof(line) - offset, "%02X ", ((char*)&report)[i]);
        }
    }
    offset += snprintf(line + offset, sizeof(line) - offset, "]");
    ESP_LOGI(title, "%s", line);
};

/**
 * Displays the contents of a HID mouse report.
 */
static inline void print_mouse_report(const char* title, const hid_mouse_report_t report){
    char line[128];
    int offset = snprintf(line, sizeof(line), "Mouse report [ buttons: "BYTE_TO_BINARY_PATTERN, BYTE_TO_BINARY(report.buttons));
    offset += snprintf(line + offset, sizeof(line) - offset, "; x: %d; y: %d; wheel: %d; pan: %d ]",
        report.x, report.y, report.wheel, report.pan);
    ESP_LOGI(title, "%s", line);
}

static inline bool keycode_contains_key(const hid_keyboard_report_t report, const uint8_t keycode){
    for (int i = 0; i < 6; i++){
        uint8_t key = report.keycode[i];
        if (key == 0) break;
        if (key == keycode){
            return true;
        }
    }
    return false;
}

static inline void add_keycode(hid_keyboard_report_t* report, const uint8_t keycode){
    for (int i = 0; i < 6; i++){
        if (report->keycode[i] == 0){
            report->keycode[i] = keycode;
            break;
        }
    }
};

static inline void remove_keycode(hid_keyboard_report_t* report, const uint8_t keycode){
    for (int i = 0; i < 6; i++){
        if (report->keycode[i] == keycode || report->keycode[i] == 0){
            report->keycode[i] = 0;
            break;
        }
    }
};

/** Boot keyboard usage 0xE0..0xE7 → modifier bit (when host sends Shift as a key slot). */
static inline bool keyboard_modifier_bit_from_usage(uint8_t usage, uint8_t *bit_out)
{
    if (usage < 0xE0 || usage > 0xE7) {
        return false;
    }
    *bit_out = (uint8_t)(1u << (usage - 0xE0));
    return true;
}

static inline uint8_t keyboard_modifier_mask_from_report(const hid_keyboard_report_t report)
{
    uint8_t mask = report.modifier;
    for (int i = 0; i < 6; i++) {
        uint8_t bit = 0;
        if (report.keycode[i] != 0 && keyboard_modifier_bit_from_usage(report.keycode[i], &bit)) {
            mask |= bit;
        }
    }
    return mask;
}

static inline bool keyboard_report_contains_event(const hid_keyboard_report_t report, const hid_keyboard_report_t expected){
    // Check modifier keys (modifier byte and/or 0xE0..0xE7 in key slots)
    if (expected.modifier != 0) {
        const uint8_t live = keyboard_modifier_mask_from_report(report);
        if ((live & expected.modifier) != expected.modifier) {
            return false;
        }
    }
    // Check keycodes
    for (int i = 0; i < 6; i++) {
        uint8_t key = expected.keycode[i];
        if (key == 0) break;
        if (!keycode_contains_key(report, key)) {
            return false;
        }
    }
    return true;
}
static inline bool mouse_report_contains_event(const hid_mouse_report_t report, const hid_mouse_report_t expected){
    // Assumption: standard structure
    if (expected.buttons != 0){
        if ((report.buttons & expected.buttons) != expected.buttons){
            return false;
        }
    }  
    return true;
}

/** Chord held: `exact` => buttons equal mask; else all mask bits down (extras allowed). */
static inline bool mouse_chord_held(const hid_mouse_report_t report, const hid_mouse_report_t expected, bool exact)
{
    if (expected.buttons == 0) {
        return false;
    }
    if (exact) {
        return report.buttons == expected.buttons;
    }
    return mouse_report_contains_event(report, expected);
}

/** Rising edge for press trigger. */
static inline bool mouse_chord_rising(const hid_mouse_report_t cur, const hid_mouse_report_t prev,
                                      const hid_mouse_report_t expected, bool exact)
{
    return mouse_chord_held(cur, expected, exact) && !mouse_chord_held(prev, expected, exact);
}

/**
 * Adds the contents of src into dst for HID reports.
 * - For keyboard: adds modifiers and merges keycodes (avoids duplicates).
 * - For mouse: only adds buttons (bitwise OR), ignores x/y/wheel/pan.
 * - If headers are different, does nothing.
 */
static inline void add_event_to_report(hid_transmit_t* dst, const hid_transmit_t src) {
    if (dst->header != src.header) return;
    if (dst->header == HEADER_HID_KEYBOARD) {
        dst->event.keyboard.modifier |= src.event.keyboard.modifier;
        for (int i = 0; i < 6; i++) {
            uint8_t key = src.event.keyboard.keycode[i];
            if (key == 0) break;
            add_keycode(&dst->event.keyboard, key);
        }
    } else if (dst->header == HEADER_HID_MOUSE) {
        dst->event.mouse.buttons |= src.event.mouse.buttons;
        dst->event.mouse.pan |= src.event.mouse.pan;
    }
}

// Set mouse movement from src to dst for HID mouse reports.
static inline void set_mouse_movement_to_report(hid_mouse_report_t* dst, const hid_mouse_report_t src) {
    dst->x = src.x;
    dst->y = src.y;
    dst->wheel = src.wheel;
    dst->pan = src.pan;
}

static inline int8_t clamp_i32_to_i8_mouse(int v)
{
    if (v > 127) {
        return 127;
    }
    if (v < -128) {
        return (int8_t)-128;
    }
    return (int8_t)v;
}

/** Scale script x/y/w/p by `scale` (1.0 = unchanged). Buttons unchanged. */
static inline hid_mouse_report_t scale_mouse_movement(const hid_mouse_report_t src, float scale)
{
    if (scale <= 0.f || scale == 1.f) {
        return src;
    }
    hid_mouse_report_t out = src;
    out.x = clamp_i32_to_i8_mouse((int)lroundf((float)src.x * scale));
    out.y = clamp_i32_to_i8_mouse((int)lroundf((float)src.y * scale));
    out.wheel = clamp_i32_to_i8_mouse((int)lroundf((float)src.wheel * scale));
    out.pan = clamp_i32_to_i8_mouse((int)lroundf((float)src.pan * scale));
    return out;
}

/** Add macro deltas to an existing mouse report (user + script), clamping each axis to int8. */
static inline void add_mouse_movement_delta(hid_mouse_report_t *dst, const hid_mouse_report_t src)
{
    int x = (int)dst->x + (int)src.x;
    int y = (int)dst->y + (int)src.y;
    int w = (int)dst->wheel + (int)src.wheel;
    int p = (int)dst->pan + (int)src.pan;
    if (x > 127) {
        x = 127;
    } else if (x < -128) {
        x = -128;
    }
    if (y > 127) {
        y = 127;
    } else if (y < -128) {
        y = -128;
    }
    if (w > 127) {
        w = 127;
    } else if (w < -128) {
        w = -128;
    }
    if (p > 127) {
        p = 127;
    } else if (p < -128) {
        p = -128;
    }
    dst->x = (int8_t)x;
    dst->y = (int8_t)y;
    dst->wheel = (int8_t)w;
    dst->pan = (int8_t)p;
    dst->buttons |= src.buttons;
}

/** Reset step index and timer. Cancel in-flight mouse spread only when stopping (not on loop wrap). */
static inline void reset_sequence(key_modification_sequence_t *sequence, bool cancel_mouse_spread)
{
    if (sequence->timer) {
        esp_timer_stop(sequence->timer);
    }
    sequence->pos = 0;
    sequence->previous_key.header = 0;
    sequence->started_time = esp_timer_get_time();
    sequence->waited_sum = 0;
    sequence->last_step_fire_us = 0;
    sequence->is_recording = false;
    if (cancel_mouse_spread) {
        macro_after_sequence_reset();
    }
}

static inline void add_keyboard_record(key_modification_sequence_t* sequence, const hid_transmit_t report){
    if (sequence->pos >= MAX_KEY_MODIFICATION_EVENT) return;
    // Compute next time target for next key sequence.
    int64_t now = esp_timer_get_time();
    int64_t duration = now - sequence->started_time - sequence->waited_sum;
    // Update wait time
    sequence->waited_sum += duration;
    // Set the new record
    sequence->list[sequence->pos].duration = duration;
    sequence->list[sequence->pos].event = report;
    sequence->pos++;
    sequence->size = sequence->pos;
}
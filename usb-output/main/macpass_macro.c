// Import global project config
#include "config.h"

#include "macro_profile.h"
#include "perf_log.h"
#include "macpass_hid.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_timer.h"

hid_keyboard_report_t last_keyboard_report[HISTORY_SIZE];
hid_mouse_report_t last_mouse_report;
group_sequence_t group_sequence;

static bool sequence_timer_active(const key_modification_sequence_t *seq)
{
    if (seq->timer == NULL) {
        return false;
    }
    return esp_timer_is_active(seq->timer);
}

static bool sequence_is_running(const key_modification_sequence_t *seq)
{
    return seq->pos > 0 || sequence_timer_active(seq);
}

static bool macro_sequence_has_mouse_steps(const key_modification_sequence_t *seq)
{
    for (uint8_t i = 0; i < seq->size; i++) {
        if (seq->list[i].event.header == HEADER_HID_MOUSE) {
            return true;
        }
    }
    return false;
}

static bool macro_any_mouse_sequence_running(void)
{
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        const key_modification_sequence_t *seq = &group_sequence.list[i];
        if (seq->size == 0 || !macro_sequence_has_mouse_steps(seq)) {
            continue;
        }
        if (sequence_is_running(seq)) {
            return true;
        }
    }
    return false;
}

void macro_after_sequence_reset(void)
{
    if (!macro_any_mouse_sequence_running()) {
        hid_macro_cancel_mouse_spread();
    }
}

static void macro_reset_mode_set_peers(int started_idx, uint8_t mode_set)
{
    if (mode_set == 0) {
        return;
    }
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        if (i == started_idx) {
            continue;
        }
        key_modification_sequence_t *peer = &group_sequence.list[i];
        if (peer->size == 0 || peer->mode_set != mode_set) {
            continue;
        }
        if (sequence_is_running(peer)) {
            reset_sequence(peer, true);
        }
    }
}

static bool mode_press_held(const hid_mouse_report_t *mouse, const hid_keyboard_report_t *kbd,
                            const key_modification_sequence_t *seq)
{
    if (!seq->press_mouse_required && !seq->press_kbd_required) {
        if (seq->event_press.header == HEADER_HID_MOUSE) {
            return mouse_chord_held(*mouse, seq->event_press.event.mouse, seq->press_exact);
        }
        if (seq->event_press.header == HEADER_HID_KEYBOARD) {
            return keyboard_report_contains_event(*kbd, seq->event_press.event.keyboard);
        }
        return false;
    }
    if (seq->press_mouse_required &&
        !mouse_chord_held(*mouse, seq->event_press.event.mouse, seq->press_exact)) {
        return false;
    }
    if (seq->press_kbd_required && !keyboard_report_contains_event(*kbd, seq->press_kbd)) {
        return false;
    }
    return true;
}

static bool mode_press_rising(const hid_mouse_report_t *cur_m, const hid_mouse_report_t *prev_m,
                              const hid_keyboard_report_t *cur_k, const hid_keyboard_report_t *prev_k,
                              const key_modification_sequence_t *seq)
{
    if (!seq->press_mouse_required && !seq->press_kbd_required) {
        if (seq->event_press.header == HEADER_HID_MOUSE) {
            return mouse_chord_rising(*cur_m, *prev_m, seq->event_press.event.mouse, seq->press_exact);
        }
        if (seq->event_press.header == HEADER_HID_KEYBOARD) {
            return keyboard_report_contains_event(*cur_k, seq->event_press.event.keyboard) &&
                   !keyboard_report_contains_event(*prev_k, seq->event_press.event.keyboard);
        }
        return false;
    }
    return mode_press_held(cur_m, cur_k, seq) && !mode_press_held(prev_m, prev_k, seq);
}

/** Stop macros when mouse and/or keyboard press conditions are no longer held. */
static void macro_press_sync_running_sequences(void)
{
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        key_modification_sequence_t *seq = &group_sequence.list[i];
        if (seq->size == 0) {
            continue;
        }
        if (!sequence_is_running(seq)) {
            continue;
        }
        if (seq->event_press.header == 0 && !seq->press_mouse_required && !seq->press_kbd_required) {
            continue;
        }
        if (seq->press_tap) {
            continue;
        }
        if (!mode_press_held(&last_mouse_report, &last_keyboard_report[0], seq)) {
            reset_sequence(seq, true);
        }
    }
}

static void macro_try_start_press_mode(int started_idx, key_modification_sequence_t *sequence);

static key_modification_sequence_t *s_pending_starts[MAX_KEY_MODIFICATION_SEQUENCE];
static uint8_t s_pending_start_count;

static void macro_pending_starts_clear(void)
{
    s_pending_start_count = 0;
}

static void macro_queue_start(key_modification_sequence_t *sequence)
{
    if (sequence == NULL || s_pending_start_count >= MAX_KEY_MODIFICATION_SEQUENCE) {
        return;
    }
    s_pending_starts[s_pending_start_count++] = sequence;
}

static void macro_flush_pending_starts(void)
{
    for (uint8_t i = 0; i < s_pending_start_count; i++) {
        start_sequence(s_pending_starts[i]);
    }
    macro_pending_starts_clear();
}

/** Arm timer for step at `sequence->pos`; always stop before re-arm. */
static void macro_arm_step_timer(key_modification_sequence_t *sequence, uint32_t delay_us)
{
    if (!sequence->timer) {
        return;
    }
    esp_timer_stop(sequence->timer);
    if (delay_us == 0) {
        esp_timer_start_once(sequence->timer, 1);
        return;
    }
    int64_t now = esp_timer_get_time();
    sequence->waited_sum += (int64_t)delay_us;
    int64_t target = sequence->started_time + sequence->waited_sum;
    if (target > now) {
        esp_timer_start_once(sequence->timer, (uint64_t)(target - now));
    } else {
        esp_timer_start_once(sequence->timer, macro_profile_catchup_delay_us(delay_us));
    }
}

/** int16 step deltas; fallback to int8 in `event.mouse` for compile-time defaults. */
static void macro_step_mouse_deltas(const key_modification_event_t *step, int16_t *x, int16_t *y,
                                    int16_t *w, int16_t *p)
{
    *x = step->mouse_x;
    *y = step->mouse_y;
    *w = step->mouse_wheel;
    *p = step->mouse_pan;
    if (*x == 0 && *y == 0 && *w == 0 && *p == 0 && step->event.header == HEADER_HID_MOUSE) {
        const hid_mouse_report_t *m = &step->event.event.mouse;
        *x = (int16_t)m->x;
        *y = (int16_t)m->y;
        *w = (int16_t)m->wheel;
        *p = (int16_t)m->pan;
    }
}

static void macro_evaluate_press_modes(const hid_mouse_report_t *cur_m, const hid_mouse_report_t *prev_m,
                                       const hid_keyboard_report_t *cur_k, const hid_keyboard_report_t *prev_k)
{
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        key_modification_sequence_t *sequence = &group_sequence.list[i];
        if (sequence->size == 0) {
            continue;
        }
        if (!macro_profile_macros_enabled()) {
            continue;
        }
        if (mode_press_rising(cur_m, prev_m, cur_k, prev_k, sequence)) {
            macro_try_start_press_mode(i, sequence);
        }
    }
}

static void macro_try_start_press_mode(int started_idx, key_modification_sequence_t *sequence)
{
#if USB_OUTPUT_PERF_LOG_ENABLE
    if (sequence->press_mouse_required && sequence->press_kbd_required) {
        ESP_LOGI(LOG_TITLE, "macro start: %s (mouse+kbd press want=0x%02X m=0x%02X steps=%u)",
                 sequence->timer_args.name, (unsigned)sequence->event_press.event.mouse.buttons,
                 (unsigned)sequence->press_kbd.modifier, (unsigned)sequence->size);
    } else if (sequence->press_mouse_required) {
        ESP_LOGI(LOG_TITLE, "macro start: %s (mouse press want=0x%02X steps=%u)", sequence->timer_args.name,
                 (unsigned)sequence->event_press.event.mouse.buttons, (unsigned)sequence->size);
    } else {
        ESP_LOGI(LOG_TITLE, "macro start: %s (keyboard press m=0x%02X steps=%u)", sequence->timer_args.name,
                 (unsigned)sequence->press_kbd.modifier, (unsigned)sequence->size);
    }
    perf_stat_bump(PERF_MACRO_START);
#endif
    macro_reset_mode_set_peers(started_idx, sequence->mode_set);
    reset_sequence(sequence, true);
    macro_queue_start(sequence);
}

static SemaphoreHandle_t s_seq_mux;

static void macro_seq_mux_init(void)
{
    if (s_seq_mux == NULL) {
        s_seq_mux = xSemaphoreCreateMutex();
    }
}

static void macro_sequence_teardown_timers(void)
{
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        key_modification_sequence_t *sequence = &group_sequence.list[i];
        if (sequence->timer != NULL) {
            esp_timer_stop(sequence->timer);
            esp_timer_delete(sequence->timer);
            sequence->timer = NULL;
        }
        if (sequence->timer_args.name != NULL) {
            free((void *)sequence->timer_args.name);
            sequence->timer_args.name = NULL;
        }
    }
}

static void macro_sequence_install_timers(void)
{
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        key_modification_sequence_t *sequence = &group_sequence.list[i];
        if (sequence->size == 0) {
            continue;
        }
        if (sequence->timer != NULL) {
            continue;
        }
        sequence->timer_args.callback = &macro_sequence_callback;
        sequence->timer_args.arg = &group_sequence.list[i];
        sequence->timer_args.name = malloc(15 * sizeof(char));
        snprintf((char *)sequence->timer_args.name, 15, "sequence%d", i);
        esp_timer_create(&sequence->timer_args, &sequence->timer);
    }
}

void macro_init(void)
{
    macro_seq_mux_init();
    macro_sequence_install_timers();
#if USB_OUTPUT_PERF_LOG_ENABLE
    int armed = 0;
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        if (group_sequence.list[i].size > 0 && group_sequence.list[i].timer != NULL) {
            armed++;
        }
    }
    ESP_LOGI(LOG_TITLE, "macro_init: macrosOn=%d additiveMouse=%d sequences armed=%d profile=%s",
             (int)macro_profile_macros_enabled(), (int)macro_profile_additive_mouse_enabled(), armed,
             macro_profile_get_name());
#endif
}

void macro_sequences_apply(const group_sequence_t *src)
{
    macro_seq_mux_init();
    /* Tear down timers without holding s_seq_mux so esp_timer_stop() can wait for any
     * in-flight callback; the callback only takes the mutex around its short group_sequence loop. */
    macro_sequence_teardown_timers();
    xSemaphoreTake(s_seq_mux, portMAX_DELAY);
    memcpy(&group_sequence, src, sizeof(group_sequence_t));
    macro_profile_sync_active_group_scales(&group_sequence);
    macro_sequence_install_timers();
    xSemaphoreGive(s_seq_mux);
}

// Prehook for macro HID transmission. Return true to end transmission chain. Default to false.
bool macro_prehook_transmission(hid_transmit_t* report){
    
    // --- START USER CUSTOM MACRO
    if (report->header == HEADER_HID_KEYBOARD){
        if (keycode_contains_key(report->event.keyboard, HID_KEY_A) && keycode_contains_key(report->event.keyboard, HID_KEY_D)){
            if (keycode_contains_key(last_keyboard_report[0], HID_KEY_A)){
                remove_keycode(&report->event.keyboard, HID_KEY_A);
            } else if (keycode_contains_key(last_keyboard_report[0], HID_KEY_D)){
                remove_keycode(&report->event.keyboard, HID_KEY_D);
            }
        }
    }
    // --- END
     
    // Example: Remove a KEY
    // remove_keycode(report, HID_KEY_C);

    // Exemple: Skip all A key
    // if (keycode_contains_key(report->event.keyboard, HID_KEY_A) return true;

    return false;
}

void macro_posthook_transmission(hid_transmit_t* report){

    #if DEBUG_LOG
    ESP_LOGI(pcTaskGetName(NULL), "posthook(): Start function");
    if (report->header == HEADER_HID_KEYBOARD) {
        print_keyboard_report(pcTaskGetName(NULL), report->event.keyboard);
    } else if (report->header == HEADER_HID_MOUSE) {
        print_mouse_report(pcTaskGetName(NULL), report->event.mouse);
    }
    #endif

    // Case n°1: Keyboard HID
    if (report->header == HEADER_HID_KEYBOARD){
        // Update last report transmission, like this all macro are added to real keys press by user
        last_keyboard_report[1] = last_keyboard_report[0];
        last_keyboard_report[0] = report->event.keyboard;

        // --- START USER CUSTOM MACRO
        // --- END

#if CONFIG_MACRO_WEB_UI
        macro_profile_try_action_hotkeys(&last_keyboard_report[1], &last_keyboard_report[0],
                                         &last_mouse_report, &last_mouse_report);
#endif
        macro_seq_mux_init();
        macro_pending_starts_clear();
        xSemaphoreTake(s_seq_mux, portMAX_DELAY);
        macro_evaluate_press_modes(&last_mouse_report, &last_mouse_report, &last_keyboard_report[0],
                                   &last_keyboard_report[1]);
        for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++){
            key_modification_sequence_t* sequence = &group_sequence.list[i];
            // Ignore empty sequence
            if (sequence->size == 0) {
                continue;
            }
            if (!macro_profile_macros_enabled()) {
                continue;
            }
            // If a unpress key is defined for sequence
            if (sequence->event_release.header == HEADER_HID_KEYBOARD && keyboard_report_contains_event(last_keyboard_report[1], sequence->event_release.event.keyboard) && !keyboard_report_contains_event(last_keyboard_report[0], sequence->event_release.event.keyboard)){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Starting keyboard release macro: %s", sequence->timer_args.name);
                #endif
#if USB_OUTPUT_PERF_LOG_ENABLE
                ESP_LOGI(LOG_TITLE, "macro start: %s (keyboard release)", sequence->timer_args.name);
                perf_stat_bump(PERF_MACRO_START);
#endif
                reset_sequence(sequence, true);
                macro_queue_start(sequence);
            }
            // If a recording sequence
            if (sequence->save_press.header == HEADER_HID_KEYBOARD && sequence->is_recording){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Recording keyboard event");
                #endif
                add_keyboard_record(sequence, *report);
            }
            // If a save press key is defined for sequence
            if (sequence->save_press.header == HEADER_HID_KEYBOARD && keyboard_report_contains_event(last_keyboard_report[0], sequence->save_press.event.keyboard)){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Starting keyboard save macro: %s", sequence->timer_args.name);
                #endif
                reset_sequence(sequence, true);
                sequence->is_recording = true;
            }
        }
        macro_press_sync_running_sequences();
        xSemaphoreGive(s_seq_mux);
        macro_flush_pending_starts();
    // Case n°2: Mouse HID
    } else if (report->header == HEADER_HID_MOUSE){
        hid_mouse_report_t prev_mouse = last_mouse_report;
        // Update last report transmission, like this all macro are added to real keys press by user
        last_mouse_report = report->event.mouse;

        // --- START USER CUSTOM MACRO
        // --- END

#if CONFIG_MACRO_WEB_UI
        macro_profile_try_action_hotkeys(&last_keyboard_report[1], &last_keyboard_report[0], &prev_mouse,
                                         &last_mouse_report);
#endif
        macro_seq_mux_init();
        macro_pending_starts_clear();
        xSemaphoreTake(s_seq_mux, portMAX_DELAY);
        macro_evaluate_press_modes(&last_mouse_report, &prev_mouse, &last_keyboard_report[0],
                                   &last_keyboard_report[1]);
        macro_press_sync_running_sequences();
        xSemaphoreGive(s_seq_mux);
        macro_flush_pending_starts();
    }
}

void macro_sequence_callback(void* arg) {
    key_modification_sequence_t *key_seq = (key_modification_sequence_t *)arg;

    macro_seq_mux_init();
    xSemaphoreTake(s_seq_mux, portMAX_DELAY);

    if (!macro_profile_macros_enabled()) {
        reset_sequence(key_seq, true);
        xSemaphoreGive(s_seq_mux);
        return;
    }
    if (key_seq->pos >= key_seq->size) {
        xSemaphoreGive(s_seq_mux);
        return;
    }

    const uint8_t step_idx = key_seq->pos;
    const key_modification_event_t *step_ev = &key_seq->list[step_idx];
    hid_transmit_t macro_event = step_ev->event;

    hid_transmit_t copy_report;
    if (macro_event.header == HEADER_HID_KEYBOARD) {
        copy_report.header = HEADER_HID_KEYBOARD;
        copy_report.event.keyboard = last_keyboard_report[0];
    } else if (macro_event.header == HEADER_HID_MOUSE) {
        copy_report.header = HEADER_HID_MOUSE;
        copy_report.event.mouse = last_mouse_report;
    } else {
#if DEBUG_LOG
        ESP_LOGI(pcTaskGetName(NULL), "macro_sequence(): Invalid macro sequence n°%u (header: %d): %s",
                 (unsigned)step_idx, macro_event.header, key_seq->timer_args.name);
#endif
        xSemaphoreGive(s_seq_mux);
        return;
    }

    key_seq->previous_key = macro_event;
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++) {
        key_modification_sequence_t *sequence = &group_sequence.list[i];
        if (sequence->size == 0) {
            continue;
        }
        if (sequence->previous_key.header != macro_event.header) {
            continue;
        }
        add_event_to_report(&copy_report, sequence->previous_key);
    }

    uint32_t step_delay_us = 0;
    bool step_delay_valid = false;
    if (macro_event.header == HEADER_HID_MOUSE) {
        int16_t mx, my, mw, mp;
        macro_step_mouse_deltas(step_ev, &mx, &my, &mw, &mp);
        macro_profile_apply_mouse_step_jitter(&mx, &my);

        /* IMP-2: complete prior bullet spread before starting this step. */
        hid_macro_flush_mouse_spread();

        const uint32_t nominal_us = step_ev->duration;
        step_delay_us = macro_profile_step_delay_us(nominal_us);
        step_delay_valid = true;
        hid_macro_feed_mouse_step(mx, my, mw, mp, nominal_us);
#if USB_OUTPUT_PERF_LOG_ENABLE
        perf_stat_bump(PERF_MACRO_TICK_MOUSE);
#endif
    } else if (macro_event.header == HEADER_HID_KEYBOARD) {
#if USB_OUTPUT_PERF_LOG_ENABLE
        perf_stat_bump(PERF_MACRO_TICK_KBD);
#endif
#if DEBUG_LOG
        ESP_LOGI(pcTaskGetName(NULL), "macro_sequence(): Send report from: %s", key_seq->timer_args.name);
#endif
        hid_add_report(copy_report);
    }

    key_seq->pos = (uint8_t)(step_idx + 1);

    if (key_seq->pos >= key_seq->size) {
        /* Keep dripping the step we just fed; release/press-sync cancels spread later. */
        reset_sequence(key_seq, false);
        if (!key_seq->loop || key_seq->press_tap) {
            xSemaphoreGive(s_seq_mux);
            return;
        }
        step_delay_us = macro_profile_step_delay_us(step_ev->duration);
        step_delay_valid = true;
    }

    if (key_seq->loop && !key_seq->press_tap && key_seq->event_press.header != 0 &&
        !mode_press_held(&last_mouse_report, &last_keyboard_report[0], key_seq)) {
        reset_sequence(key_seq, true);
        xSemaphoreGive(s_seq_mux);
        return;
    }

    if (step_delay_valid) {
        macro_arm_step_timer(key_seq, step_delay_us);
    } else {
        macro_arm_step_timer(key_seq, 0);
    }
    xSemaphoreGive(s_seq_mux);
}

void start_sequence_with_delay(key_modification_sequence_t *sequence, uint32_t step_us)
{
    macro_arm_step_timer(sequence, step_us);
}

void start_sequence(key_modification_sequence_t *sequence)
{
    macro_arm_step_timer(sequence, 0);
}

// Import global project config
#include "config.h"
#include "perf_log.h"
#include "macpass_macro.h"
#include "macro_profile.h"


#include "esp_timer.h"


#define HID_MOUSE_QUEUE_LEN 16

// Queues: mouse FIFO (macro drips); keyboard preserves order.
QueueHandle_t global_hid_mouse_queue = NULL;
QueueHandle_t global_hid_keyboard_queue = NULL;


static TaskHandle_t s_hid_task;


/** Current bullet segment (time-linear spread between step boundaries). */
static int16_t s_target_mx;
static int16_t s_target_my;
static int16_t s_target_mw;
static int16_t s_target_mp;
static int16_t s_sent_mx;
static int16_t s_sent_my;
static int16_t s_sent_mw;
static int16_t s_sent_mp;
static int64_t s_spread_start_us;
static int64_t s_spread_end_us;
static uint32_t s_last_drip_tick;
static uint32_t s_spread_drip_goal;
static esp_timer_handle_t s_drip_timer;

/** Latest SPI button-only report (zero movement); coalesced, not queued in FIFO. */
static hid_transmit_t s_pending_button_only_report;
static bool s_pending_button_only;


static void hid_queue_report(hid_transmit_t report);
static bool hid_mouse_merge_spread_drip(hid_mouse_report_t *m);
static uint32_t hid_mouse_drip_interval_us(void);
static void hid_spread_flush_remainder_immediate(void);


static bool hid_spread_pending(void)
{
    return s_sent_mx != s_target_mx || s_sent_my != s_target_my || s_sent_mw != s_target_mw ||
           s_sent_mp != s_target_mp;
}


static bool hid_mouse_spread_enabled(void)
{
    return hid_mouse_drip_interval_us() > 0;
}


static uint32_t hid_mouse_drip_interval_us(void)
{
    return macro_profile_mouse_drip_interval_us();
}


static void hid_spread_reset_segment(void)
{
    s_target_mx = s_target_my = s_target_mw = s_target_mp = 0;
    s_sent_mx = s_sent_my = s_sent_mw = s_sent_mp = 0;
    s_spread_start_us = 0;
    s_spread_end_us = 0;
    s_spread_drip_goal = 0;
    s_last_drip_tick = 0;
}

static void hid_spread_drip_timer_stop(void)
{
    if (s_drip_timer != NULL && esp_timer_is_active(s_drip_timer)) {
        esp_timer_stop(s_drip_timer);
    }
}

/** Elapsed progress 0..goal across the bullet spread window (smooth in wall time). */
static uint32_t hid_spread_elapsed_tick(uint32_t goal)
{
    if (goal == 0) {
        return 0;
    }
    int64_t dur = s_spread_end_us - s_spread_start_us;
    if (dur <= 0) {
        return goal;
    }
    int64_t elapsed = esp_timer_get_time() - s_spread_start_us;
    if (elapsed <= 0) {
        return 0;
    }
    if (elapsed >= dur) {
        return goal;
    }
    return (uint32_t)((elapsed * (int64_t)goal) / dur);
}

/**
 * ideal = target * axis_tick / eff_goal − sent.
 * Small |target| uses a shorter eff_goal so X ramps in time; large Y uses full window.
 */
static int8_t hid_take_drip_axis_spread(int16_t target, int16_t *sent, uint32_t tick, uint32_t goal)
{
    int32_t remain = (int32_t)target - (int32_t)*sent;
    if (remain == 0) {
        return 0;
    }

    if (goal == 0) {
        goal = 1;
    }
    if (tick > goal) {
        tick = goal;
    }

    uint32_t eff_goal = goal;
    int32_t abs_t = target >= 0 ? (int32_t)target : -(int32_t)target;
    if (abs_t > 0 && (uint32_t)abs_t < eff_goal) {
        eff_goal = (uint32_t)abs_t;
    }

    uint32_t axis_tick = tick;
    if (eff_goal < goal) {
        axis_tick = (uint32_t)(((uint64_t)tick * eff_goal) / goal);
        if (axis_tick == 0 && tick > 0 && target != 0) {
            axis_tick = 1;
        }
    }
    if (axis_tick > eff_goal) {
        axis_tick = eff_goal;
    }

    int32_t ideal = (int32_t)(((int64_t)target * (int64_t)axis_tick) / (int64_t)eff_goal);
    int32_t move = ideal - (int32_t)*sent;

    if ((remain > 0 && move < 0) || (remain < 0 && move > 0)) {
        move = 0;
    }
    if (remain > 0 && move > remain) {
        move = remain;
    } else if (remain < 0 && move < remain) {
        move = remain;
    }
    /* Avoid stall-then-jump when integer ideal matches sent but spread is still active. */
    if (move == 0 && remain != 0 && tick > 0) {
        move = (remain > 0) ? 1 : -1;
    }
    if (move == 0) {
        return 0;
    }

    move = (int32_t)clamp_i32_to_i8_mouse((int)move);
    *sent += (int16_t)move;
    return (int8_t)move;
}


static bool hid_mouse_merge_spread_drip(hid_mouse_report_t *m)
{
    if (!hid_mouse_spread_enabled() || !hid_spread_pending()) {
        return false;
    }

    const uint32_t goal = s_spread_drip_goal;
    const uint32_t tick = hid_spread_elapsed_tick(goal);
    if (tick <= s_last_drip_tick && tick < goal) {
        return false;
    }
    s_last_drip_tick = tick;

    hid_mouse_report_t drip = {0};
    drip.x = hid_take_drip_axis_spread(s_target_mx, &s_sent_mx, tick, goal);
    drip.y = hid_take_drip_axis_spread(s_target_my, &s_sent_my, tick, goal);
    drip.wheel = hid_take_drip_axis_spread(s_target_mw, &s_sent_mw, tick, goal);
    drip.pan = hid_take_drip_axis_spread(s_target_mp, &s_sent_mp, tick, goal);
    add_mouse_movement_delta(m, drip);
    return drip.x != 0 || drip.y != 0 || drip.wheel != 0 || drip.pan != 0;
}


static void hid_drip_timer_stop_if_idle(void)
{
    if (hid_spread_pending() || s_drip_timer == NULL) {
        return;
    }
    hid_spread_drip_timer_stop();
}


static void hid_drip_timer_ensure_running(void)
{
    const uint32_t drip_us = hid_mouse_drip_interval_us();
    if (drip_us == 0 || s_drip_timer == NULL) {
        return;
    }
    if (esp_timer_is_active(s_drip_timer)) {
        return;
    }
    esp_timer_start_periodic(s_drip_timer, drip_us);
}


void hid_mouse_drip_apply_interval(void)
{
    if (s_drip_timer == NULL) {
        return;
    }
    const uint32_t drip_us = hid_mouse_drip_interval_us();
    if (esp_timer_is_active(s_drip_timer)) {
        esp_timer_stop(s_drip_timer);
    }
    if (drip_us > 0 && hid_spread_pending()) {
        esp_timer_start_periodic(s_drip_timer, drip_us);
    }
}


static bool hid_macro_emit_drip_report(void)
{
    if (!hid_spread_pending()) {
        return false;
    }

    hid_transmit_t report;
    report.header = HEADER_HID_MOUSE;
    report.event.mouse = last_mouse_report;
    report.event.mouse.x = 0;
    report.event.mouse.y = 0;
    report.event.mouse.wheel = 0;
    report.event.mouse.pan = 0;
    if (hid_mouse_merge_spread_drip(&report.event.mouse)) {
        hid_queue_report(report);
        return true;
    }
    return false;
}


static void hid_drip_timer_cb(void *arg)
{
    (void)arg;
    if (!hid_spread_pending()) {
        hid_drip_timer_stop_if_idle();
        return;
    }

    (void)hid_macro_emit_drip_report();
    hid_drip_timer_stop_if_idle();
}


static void hid_macro_send_immediate_step(int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
    hid_transmit_t report;
    report.header = HEADER_HID_MOUSE;
    report.event.mouse = last_mouse_report;
    report.event.mouse.x = x;
    report.event.mouse.y = y;
    report.event.mouse.wheel = wheel;
    report.event.mouse.pan = pan;
    hid_queue_report(report);
}

static int8_t hid_take_axis_slice(int16_t *v)
{
    if (*v == 0) {
        return 0;
    }
    int16_t slice;
    if (*v > 127) {
        slice = 127;
    } else if (*v < -128) {
        slice = -128;
    } else {
        slice = *v;
    }
    *v = (int16_t)(*v - slice);
    return (int8_t)slice;
}

/** Send full step when drip is off (may use multiple HID reports). */
static void hid_macro_send_immediate_step16(int16_t x, int16_t y, int16_t wheel, int16_t pan)
{
    for (unsigned n = 0; n < 32u && (x != 0 || y != 0 || wheel != 0 || pan != 0); n++) {
        hid_macro_send_immediate_step(hid_take_axis_slice(&x), hid_take_axis_slice(&y),
                                      hid_take_axis_slice(&wheel), hid_take_axis_slice(&pan));
    }
}

/** Python-style final MoveMouse: send target − sent and mark segment complete. */
static void hid_spread_flush_remainder_immediate(void)
{
    int16_t rx = (int16_t)((int32_t)s_target_mx - (int32_t)s_sent_mx);
    int16_t ry = (int16_t)((int32_t)s_target_my - (int32_t)s_sent_my);
    int16_t rw = (int16_t)((int32_t)s_target_mw - (int32_t)s_sent_mw);
    int16_t rp = (int16_t)((int32_t)s_target_mp - (int32_t)s_sent_mp);
    if (rx == 0 && ry == 0 && rw == 0 && rp == 0) {
        return;
    }
    s_sent_mx = s_target_mx;
    s_sent_my = s_target_my;
    s_sent_mw = s_target_mw;
    s_sent_mp = s_target_mp;
    hid_macro_send_immediate_step16(rx, ry, rw, rp);
}


void hid_macro_flush_mouse_spread(void)
{
    if (!hid_spread_pending()) {
        return;
    }

    hid_spread_drip_timer_stop();

    if (!hid_mouse_spread_enabled()) {
        hid_spread_flush_remainder_immediate();
        hid_spread_reset_segment();
        return;
    }

    /* Finish segment at phase end; drip loop, then any int8 residue (IMP-2). */
    s_last_drip_tick = 0;
    for (unsigned n = 0; n < 64u && hid_spread_pending(); n++) {
        (void)hid_macro_emit_drip_report();
    }
    if (hid_spread_pending()) {
        hid_spread_flush_remainder_immediate();
    }
    hid_spread_reset_segment();
}


void hid_macro_feed_mouse_step(int16_t x, int16_t y, int16_t wheel, int16_t pan, uint32_t spread_us,
                               int64_t phase_start_us)
{
    if (x == 0 && y == 0 && wheel == 0 && pan == 0) {
        return;
    }


    if (!hid_mouse_spread_enabled()) {
        hid_macro_send_immediate_step16(x, y, wheel, pan);
        return;
    }


    /* IMP-3: discard unfinished prior segment (IMP-2 should have flushed). */
    if (hid_spread_pending()) {
        hid_macro_flush_mouse_spread();
    }

    hid_spread_drip_timer_stop();

    int64_t now = esp_timer_get_time();
    const uint32_t drip_us = hid_mouse_drip_interval_us();

    s_target_mx = x;
    s_target_my = y;
    s_target_mw = wheel;
    s_target_mp = pan;
    s_sent_mx = s_sent_my = s_sent_mw = s_sent_mp = 0;
    if (spread_us < 1000u) {
        spread_us = 1000u;
    }
    if (phase_start_us > 0) {
        s_spread_start_us = phase_start_us;
        s_spread_end_us = phase_start_us + (int64_t)spread_us;
    } else {
        s_spread_start_us = now;
        s_spread_end_us = now + (int64_t)spread_us;
    }
    s_spread_drip_goal = drip_us > 0 ? (spread_us + drip_us - 1u) / drip_us : 1u;
    if (s_spread_drip_goal == 0) {
        s_spread_drip_goal = 1;
    }
    s_last_drip_tick = 0;

    hid_drip_timer_ensure_running();
    (void)hid_macro_emit_drip_report();
    hid_wake_pump();
}


void hid_macro_cancel_mouse_spread(void)
{
    hid_spread_drip_timer_stop();
    hid_spread_reset_segment();
}


bool hid_macro_mouse_spread_active(void)
{
    return hid_spread_pending();
}


static bool hid_send_report(hid_transmit_t *report)
{
    if (!tud_ready()) {
        return false;
    }
    if (report->header == HEADER_HID_KEYBOARD) {
        bool ok = tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, report->event.keyboard.modifier,
                                          report->event.keyboard.keycode);
#if USB_OUTPUT_PERF_LOG_ENABLE
        if (ok) {
            perf_stat_bump(PERF_USB_OUT_KBD);
        }
#endif
        return ok;
    }
    if (report->header == HEADER_HID_MOUSE) {
        bool ok = tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, report->event.mouse.buttons, report->event.mouse.x,
                                       report->event.mouse.y, report->event.mouse.wheel, report->event.mouse.pan);
#if USB_OUTPUT_PERF_LOG_ENABLE
        if (ok) {
            perf_stat_bump(PERF_USB_OUT_MOUSE);
        }
#endif
        return ok;
    }
    return false;
}


static void hid_queue_report(hid_transmit_t report)
{
    bool queued = false;


    if (report.header == HEADER_HID_MOUSE) {
        if (global_hid_mouse_queue != NULL) {
            /* Passthrough from SPI: coalesce button-only (xQueueOverwrite is depth-1 only). */
            if (report.event.mouse.x == 0 && report.event.mouse.y == 0 &&
                report.event.mouse.wheel == 0 && report.event.mouse.pan == 0) {
                s_pending_button_only_report = report;
                s_pending_button_only = true;
            } else {
                if (xQueueSend(global_hid_mouse_queue, &report, 0) != pdTRUE) {
                    /* Queue full: drop oldest, retry once. */
                    hid_transmit_t drop;
                    (void)xQueueReceive(global_hid_mouse_queue, &drop, 0);
                    (void)xQueueSend(global_hid_mouse_queue, &report, 0);
                }
            }
            queued = true;
        }
    } else if (global_hid_keyboard_queue != NULL) {
        if (xQueueSend(global_hid_keyboard_queue, &report, 0) == pdTRUE) {
            queued = true;
        }
    }


    if (queued) {
        hid_wake_pump();
    }
}


static void hid_drain_all_pending(void)
{
    hid_transmit_t report;


    for (int budget = 0; budget < 32; budget++) {
        bool work = false;


        if (xQueueReceive(global_hid_keyboard_queue, &report, 0) == pdTRUE) {
            work = true;
            if (!hid_send_report(&report)) {
                (void)xQueueSendToFront(global_hid_keyboard_queue, &report, 0);
            }
        }


        if (xQueueReceive(global_hid_mouse_queue, &report, 0) == pdTRUE) {
            work = true;
            if (!hid_send_report(&report)) {
                (void)xQueueSendToFront(global_hid_mouse_queue, &report, 0);
            }
        }


        if (s_pending_button_only) {
            work = true;
            if (hid_send_report(&s_pending_button_only_report)) {
                s_pending_button_only = false;
            }
        }


        if (!work) {
            break;
        }
    }
}


void hid_task_multiplexer(void *pvParameters)
{
    (void)pvParameters;


    for (;;) {
        hid_drain_all_pending();
        TickType_t wait = hid_macro_mouse_spread_active() ? pdMS_TO_TICKS(1) : pdMS_TO_TICKS(10);
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}


void hid_init_multiplexer()
{
    global_hid_mouse_queue = xQueueCreate(HID_MOUSE_QUEUE_LEN, sizeof(hid_transmit_t));
    global_hid_keyboard_queue = xQueueCreate(16, sizeof(hid_transmit_t));


    esp_timer_create_args_t drip_args = {
        .callback = &hid_drip_timer_cb,
        .name = "hid_mouse_drip",
    };
    ESP_ERROR_CHECK(esp_timer_create(&drip_args, &s_drip_timer));


    xTaskCreatePinnedToCore(hid_task_multiplexer, "HID Report Mult", 4096, NULL, 18, &s_hid_task, 1);
}


void hid_wake_pump(void)
{
    if (s_hid_task != NULL) {
        xTaskNotifyGive(s_hid_task);
    }
}


void hid_add_report(hid_transmit_t report)
{
    if (report.header == HEADER_HID_MOUSE) {
        if (macro_profile_debug_mode() && hid_spread_pending() &&
            (report.event.mouse.x != 0 || report.event.mouse.y != 0 || report.event.mouse.wheel != 0 ||
             report.event.mouse.pan != 0)) {
            report.event.mouse.x = 0;
            report.event.mouse.y = 0;
            report.event.mouse.wheel = 0;
            report.event.mouse.pan = 0;
        }
        (void)hid_mouse_merge_spread_drip(&report.event.mouse);
    }
    hid_queue_report(report);
}


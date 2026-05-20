// Import global project config
#include "config.h"
#include "perf_log.h"
#include "macpass_macro.h"
#include "macro_profile.h"


#include "esp_timer.h"


// Queues: mouse keeps only the latest report; keyboard preserves order.
QueueHandle_t global_hid_mouse_queue = NULL;
QueueHandle_t global_hid_keyboard_queue = NULL;


static TaskHandle_t s_hid_task;


/** Current bullet segment (lerp 0 → target over [start, end]). */
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
static int64_t s_last_drip_us;
static esp_timer_handle_t s_drip_timer;


static void hid_queue_report(hid_transmit_t report);
static bool hid_mouse_merge_spread_drip(hid_mouse_report_t *m);
static uint32_t hid_mouse_drip_interval_us(void);


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
}

/** Time-linear lerp: ideal = target * elapsed / duration; send floor(ideal) - sent (Python SmoothMove). */
static int8_t hid_take_drip_axis_lerp(int16_t target, int16_t *sent)
{
    int32_t remain = (int32_t)target - (int32_t)*sent;
    if (remain == 0) {
        return 0;
    }

    int64_t now = esp_timer_get_time();
    int64_t dur = s_spread_end_us - s_spread_start_us;
    if (dur <= 0) {
        int32_t out32 = (int32_t)clamp_i32_to_i8_mouse(remain);
        *sent += (int16_t)out32;
        return (int8_t)out32;
    }

    int64_t elapsed = now - s_spread_start_us;
    if (elapsed < 0) {
        elapsed = 0;
    }
    if (elapsed > dur) {
        elapsed = dur;
    }

    int32_t ideal = (int32_t)(((int64_t)target * elapsed) / dur);
    int32_t move = ideal - (int32_t)*sent;

    if (elapsed >= dur) {
        move = remain;
    }
    if (move == 0) {
        return 0;
    }

    if (move > 0) {
        if (move > remain) {
            move = remain;
        }
    } else if (move < remain) {
        move = remain;
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


    int64_t now = esp_timer_get_time();
    const uint32_t drip_us = hid_mouse_drip_interval_us();
    if (s_last_drip_us != 0 && (now - s_last_drip_us) < (int64_t)drip_us) {
        return false;
    }
    s_last_drip_us = now;

    hid_mouse_report_t drip = {0};
    drip.x = hid_take_drip_axis_lerp(s_target_mx, &s_sent_mx);
    drip.y = hid_take_drip_axis_lerp(s_target_my, &s_sent_my);
    drip.wheel = hid_take_drip_axis_lerp(s_target_mw, &s_sent_mw);
    drip.pan = hid_take_drip_axis_lerp(s_target_mp, &s_sent_mp);
    add_mouse_movement_delta(m, drip);
    return drip.x != 0 || drip.y != 0 || drip.wheel != 0 || drip.pan != 0;
}


static void hid_drip_timer_stop_if_idle(void)
{
    if (hid_spread_pending() || s_drip_timer == NULL) {
        return;
    }
    if (esp_timer_is_active(s_drip_timer)) {
        esp_timer_stop(s_drip_timer);
    }
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


static void hid_drip_timer_cb(void *arg)
{
    (void)arg;
    if (!hid_spread_pending()) {
        hid_drip_timer_stop_if_idle();
        return;
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
    }
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


void hid_macro_feed_mouse_step(int8_t x, int8_t y, int8_t wheel, int8_t pan, uint32_t spread_us)
{
    if (x == 0 && y == 0 && wheel == 0 && pan == 0) {
        return;
    }


    if (!hid_mouse_spread_enabled()) {
        hid_macro_send_immediate_step(x, y, wheel, pan);
        return;
    }


    int64_t now = esp_timer_get_time();

    /* Fold unfinished prior segment into this bullet (per-bullet SmoothMove). */
    int32_t tx = (int32_t)x + ((int32_t)s_target_mx - (int32_t)s_sent_mx);
    int32_t ty = (int32_t)y + ((int32_t)s_target_my - (int32_t)s_sent_my);
    int32_t tw = (int32_t)wheel + ((int32_t)s_target_mw - (int32_t)s_sent_mw);
    int32_t tp = (int32_t)pan + ((int32_t)s_target_mp - (int32_t)s_sent_mp);
    if (tx > 32767) {
        tx = 32767;
    } else if (tx < -32768) {
        tx = -32768;
    }
    if (ty > 32767) {
        ty = 32767;
    } else if (ty < -32768) {
        ty = -32768;
    }
    if (tw > 32767) {
        tw = 32767;
    } else if (tw < -32768) {
        tw = -32768;
    }
    if (tp > 32767) {
        tp = 32767;
    } else if (tp < -32768) {
        tp = -32768;
    }

    s_target_mx = (int16_t)tx;
    s_target_my = (int16_t)ty;
    s_target_mw = (int16_t)tw;
    s_target_mp = (int16_t)tp;
    s_sent_mx = s_sent_my = s_sent_mw = s_sent_mp = 0;
    s_spread_start_us = now;
    if (spread_us < 1000u) {
        spread_us = 1000u;
    }
    s_spread_end_us = now + (int64_t)spread_us;
    s_last_drip_us = 0;

    hid_drip_timer_ensure_running();
    hid_wake_pump();
}


void hid_macro_cancel_mouse_spread(void)
{
    hid_spread_reset_segment();
    s_last_drip_us = 0;
    hid_drip_timer_stop_if_idle();
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
            xQueueOverwrite(global_hid_mouse_queue, &report);
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
                (void)xQueueOverwrite(global_hid_mouse_queue, &report);
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
    global_hid_mouse_queue = xQueueCreate(1, sizeof(hid_transmit_t));
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
        (void)hid_mouse_merge_spread_drip(&report.event.mouse);
    }
    hid_queue_report(report);
}


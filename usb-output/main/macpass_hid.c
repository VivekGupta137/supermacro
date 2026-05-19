// Import global project config
#include "config.h"
#include "perf_log.h"

// Queues: mouse keeps only the latest report; keyboard preserves order.
QueueHandle_t global_hid_mouse_queue = NULL;
QueueHandle_t global_hid_keyboard_queue = NULL;

static TaskHandle_t s_hid_task;

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

static void hid_drain_all_pending(void)
{
    hid_transmit_t report;

    for (int budget = 0; budget < 32; budget++) {
        bool work = false;

        /* Keyboard before mouse so keys are not starved by continuous mouse SPI traffic. */
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

// Consumer task: same core as TinyUSB (core 1).
void hid_task_multiplexer(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        hid_drain_all_pending();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}

void hid_init_multiplexer()
{
    global_hid_mouse_queue = xQueueCreate(1, sizeof(hid_transmit_t));
    global_hid_keyboard_queue = xQueueCreate(16, sizeof(hid_transmit_t));

    /* Core 1 with TinyUSB (macpass_usb.c task.xCoreID = 1). Priority below tud task (23). */
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

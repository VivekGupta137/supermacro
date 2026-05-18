// Import global project config
#include "config.h"
#include "perf_log.h"

#define USB_HID_VENDOR_LOGITECH 0x046du

#define MAX_TRACKED_HID_IFACES 8

static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"
};

typedef struct {
    hid_host_device_handle_t handle;
    uint16_t vid;
    uint16_t pid;
    bool in_use;
} tracked_hid_iface_t;

static tracked_hid_iface_t s_tracked_ifaces[MAX_TRACKED_HID_IFACES];

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
    if (v > 127) {
        return 127;
    }
    if (v < -127) {
        return -127;
    }
    return (int8_t)v;
}

static void tracked_iface_clear(hid_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++) {
        if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == handle) {
            s_tracked_ifaces[i].in_use = false;
            s_tracked_ifaces[i].handle = NULL;
            s_tracked_ifaces[i].vid = 0;
            s_tracked_ifaces[i].pid = 0;
        }
    }
}

static void tracked_iface_store(hid_host_device_handle_t handle)
{
    tracked_iface_clear(handle);
    hid_host_dev_info_t info = {0};
    if (hid_host_get_device_info(handle, &info) != ESP_OK) {
        return;
    }
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++) {
        if (!s_tracked_ifaces[i].in_use) {
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
    for (int i = 0; i < MAX_TRACKED_HID_IFACES; i++) {
        if (s_tracked_ifaces[i].in_use && s_tracked_ifaces[i].handle == handle) {
            return s_tracked_ifaces[i].vid;
        }
    }
    return 0;
}

/** HID++ feature traffic — not pointer movement. */
static bool mouse_raw_is_hidpp(const uint8_t *data, size_t data_length)
{
    if (data_length < 1) {
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
    if (data_length != 9 || data[0] != 0x02) {
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
    if (data_length == 9 && data[0] == 0x02) {
        const uint8_t *p = data + 1;
        int16_t x16 = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        int16_t y16 = (int16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
        hid_mouse_report_t parsed = {0};
        try_parse_logitech_g304_report2(data, data_length, &parsed);
        ESP_LOGI(LOG_TITLE,
                 "mouse pkt: len=9 %02X %02X %02X %02X %02X %02X %02X %02X %02X | x16=%d y16=%d btn=%02X spi x=%d y=%d wh=%d",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], (int)x16,
                 (int)y16, parsed.buttons, (int)parsed.x, (int)parsed.y, (int)parsed.wheel);
    } else {
        ESP_LOGI(LOG_TITLE, "mouse pkt: len=%u %02X %02X %02X %02X %02X", (unsigned)data_length,
                 data_length > 0 ? data[0] : 0, data_length > 1 ? data[1] : 0, data_length > 2 ? data[2] : 0,
                 data_length > 3 ? data[3] : 0, data_length > 4 ? data[4] : 0);
    }
}
#endif

static bool spi_send_logitech_mouse(const uint8_t *data, size_t data_length)
{
    if (data_length < 3 || mouse_raw_is_hidpp(data, data_length)) {
        return false;
    }

    /* Report 3/4/8 are consumer/system/media on this receiver — not pointer data. */
    if (data[0] == 0x03 || data[0] == 0x04 || data[0] == 0x08) {
        return false;
    }

    hid_mouse_report_t mouse = {0};
    if (try_parse_logitech_g304_report2(data, data_length, &mouse)) {
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
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(
            hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length));
        perf_hid_input_report((uint8_t)dev_params.proto);
#if USB_INPUT_PERF_LOG_ENABLE
        if (dev_params.proto == HID_PROTOCOL_MOUSE) {
            int64_t now_us = esp_timer_get_time();
            if (mouse_pkt_log_last_us == 0 ||
                (now_us - mouse_pkt_log_last_us) >= ((int64_t)USB_INPUT_PERF_LOG_WINDOW_MS * 1000)) {
                mouse_pkt_log_sample(data, data_length);
                mouse_pkt_log_last_us = now_us;
            }
        }
#endif
#if DEBUG_LOG
        ESP_LOGI(LOG_TITLE, "HID Report subclass: %d, proto %d, size: %d", dev_params.sub_class, dev_params.proto,
                 data_length);
#endif

#if DEBUG_LOG
        if (data_length == 0) {
            break;
        }
#endif
        if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            if (HID_SUBCLASS_BOOT_INTERFACE != dev_params.sub_class) {
                break;
            }
#if DEBUG_LOG
            if (keycode_contains_key(*((hid_keyboard_report_t *)data), HID_KEY_CAPS_LOCK)) {
                report_time = esp_timer_get_time();
            }
#endif
            hid_report_t report = {0};
            size_t copy_len = data_length;
            if (copy_len > sizeof(hid_keyboard_report_t)) {
                copy_len = sizeof(hid_keyboard_report_t);
            }
            memcpy(&report.keyboard, data, copy_len);
            spi_send_master_hid_sender(HEADER_HID_KEYBOARD, &report);
        } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
            uint16_t vid = tracked_iface_vid(hid_device_handle);
            if (vid != 0 && vid != USB_HID_VENDOR_LOGITECH) {
                break;
            }
            spi_send_logitech_mouse(data, data_length);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
        tracked_iface_clear(hid_device_handle);
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

    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);

        if (dev_params.proto == 0 || dev_params.proto > 2) {
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
        if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
#if DEBUG_LOG
                ESP_LOGI(LOG_TITLE, "DEBUG: Set KEYBOARD BOOT protocol");
#endif
                esp_err_t proto_ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
                if (proto_ret != ESP_OK) {
                    ESP_LOGE(LOG_TITLE, "Set BOOT protocol failed: %s", esp_err_to_name(proto_ret));
                }
                usb_keyboard_handle = hid_device_handle;
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
            }
        } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
#if DEBUG_LOG
            ESP_LOGI(LOG_TITLE, "DEBUG: Set MOUSE REPORT protocol");
#endif
            esp_err_t proto_ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT);
            if (proto_ret != ESP_OK) {
                ESP_LOGW(LOG_TITLE, "Set REPORT protocol failed: %s", esp_err_to_name(proto_ret));
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
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
    if (usb_keyboard_handle != NULL) {
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
    vTaskDelay(pdMS_TO_TICKS(300));

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = USB_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = USB_TASK_COREID,
        .callback = hid_host_device_callback,
        .callback_arg = NULL};

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
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

    while (true) {
        if (xQueueReceive(hid_event_queue, &evt_queue, portMAX_DELAY)) {
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

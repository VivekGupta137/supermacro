// Import global project config
#include "config.h"
#include "perf_log.h"

static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"
};

hid_host_device_handle_t usb_keyboard_handle = NULL;
QueueHandle_t hid_event_queue = NULL;
#if DEBUG_LOG
int64_t report_time;
#endif
#if USB_INPUT_PERF_LOG_ENABLE
static int64_t mouse_pkt_log_last_us = 0;
#endif

static inline int8_t clamp_i16_to_i8(int32_t v){
    if (v > 127) return 127;
    if (v < -127) return -127;
    return (int8_t)v;
}

static void spi_send_mouse_boot_compatible(const uint8_t *data, size_t data_length){
    hid_report_t report = {0};
    if (data_length < 3) {
        return;
    }

    // DeathAdder V3 MI_00 report descriptor (8-byte packet):
    // byte0 buttons(5 bits), byte1..2 vendor, byte3 wheel, byte4..5 x(int16), byte6..7 y(int16)
    if (data_length >= 8) {
        report.mouse.buttons = data[0] & 0x1F;
        report.mouse.wheel = (int8_t)data[3];
        int32_t x16 = (int16_t)((uint16_t)data[4] | ((uint16_t)data[5] << 8));
        int32_t y16 = (int16_t)((uint16_t)data[6] | ((uint16_t)data[7] << 8));
        report.mouse.x = clamp_i16_to_i8(x16);
        report.mouse.y = clamp_i16_to_i8(y16);
        report.mouse.pan = 0;
    } else {
        // Generic boot fallback: buttons, x, y, optional wheel.
        report.mouse.buttons = data[0] & 0x1F;
        report.mouse.x = (int8_t)data[1];
        report.mouse.y = (int8_t)data[2];
        report.mouse.wheel = (data_length >= 4) ? (int8_t)data[3] : 0;
        report.mouse.pan = 0;
    }

    spi_send_master_hid_sender(HEADER_HID_MOUSE, &report);
}

/**
 * @brief USB HID Host interface callback
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host interface event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg){
    uint8_t data[32] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, sizeof(data), &data_length));
            perf_hid_input_report((uint8_t)dev_params.proto);
            #if USB_INPUT_PERF_LOG_ENABLE
            if (dev_params.proto == HID_PROTOCOL_MOUSE) {
                int64_t now_us = esp_timer_get_time();
                if (mouse_pkt_log_last_us == 0 || (now_us - mouse_pkt_log_last_us) >= ((int64_t)USB_INPUT_PERF_LOG_WINDOW_MS * 1000)) {
                    uint8_t b0 = (data_length > 0) ? data[0] : 0;
                    uint8_t b1 = (data_length > 1) ? data[1] : 0;
                    uint8_t b2 = (data_length > 2) ? data[2] : 0;
                    uint8_t b3 = (data_length > 3) ? data[3] : 0;
                    ESP_LOGI(LOG_TITLE, "mouse pkt: len=%u b0=%02X b1=%02X b2=%02X b3=%02X",
                             (unsigned)data_length, b0, b1, b2, b3);
                    mouse_pkt_log_last_us = now_us;
                }
            }
            #endif
            #if DEBUG_LOG
            ESP_LOGI(LOG_TITLE, "HID Report subclass: %d, proto %d, size: %d", dev_params.sub_class, dev_params.proto, data_length);
            #endif

            // Manage different type of report
            #if DEBUG_LOG
            if (data_length==0) break;
            #endif
            /*
             * usb-output expects the same hid_report_t layout (boot keyboard + boot mouse:
             * buttons, int8 x, int8 y, wheel, pan). Report-protocol vendor packets must not
             * be memcpy'd into that struct — wrong layout reads as horizontal-only / jitter.
             */
            if (HID_SUBCLASS_BOOT_INTERFACE != dev_params.sub_class) {
                break;
            }
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                #if DEBUG_LOG
                if (keycode_contains_key(*((hid_keyboard_report_t*)data), HID_KEY_CAPS_LOCK)){
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
                spi_send_mouse_boot_compatible(data, data_length);
            }
            break;
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' DISCONNECTED", hid_proto_name_str[dev_params.proto]);
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
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host Device event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg){
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(LOG_TITLE, "HID Device, protocol '%s' CONNECTED", hid_proto_name_str[dev_params.proto]);
        
        // ESP32-S3 only have few USB handle available.
        //  we need to skip NONE peripheral to connect in order
        //  to works with USB hub. Because gaming Keyboard/Mouse 
        //  can have 4-5 HID device per peripheral.
        if(dev_params.proto == 0 || dev_params.proto > 2){
            ESP_LOGI(LOG_TITLE, "HID Device, skipped");
            break;
        }

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        #if DEBUG_LOG
        ESP_LOGI(LOG_TITLE, "DEBUG: handle=%p, sub_class=%d, proto=%d", hid_device_handle, dev_params.sub_class, dev_params.proto);
        #endif
        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                #if DEBUG_LOG
                ESP_LOGI(LOG_TITLE, "DEBUG: Set KEYBOARD BOOT protocol");
                #endif
                esp_err_t proto_ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
                if (proto_ret != ESP_OK) {
                    ESP_LOGE(LOG_TITLE, "Set BOOT protocol failed: %s", esp_err_to_name(proto_ret));
                }
                usb_keyboard_handle = hid_device_handle;
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
            } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                #if DEBUG_LOG
                ESP_LOGI(LOG_TITLE, "DEBUG: Set MOUSE REPORT protocol");
                #endif
                esp_err_t proto_ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT);
                if (proto_ret != ESP_OK) {
                    ESP_LOGW(LOG_TITLE, "Set REPORT protocol failed: %s", esp_err_to_name(proto_ret));
                }
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        break;
    default:
        break;
    }
}

/**
 * @brief HID Host Device callback
 *
 * Puts new HID Device event to the queue
 *
 * @param[in] hid_device_handle HID Device handle
 * @param[in] event             HID Device event
 * @param[in] arg               Not used
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg){
    static hid_event_queue_t evt_queue;
    evt_queue.handle = hid_device_handle;
    evt_queue.event = event;
    evt_queue.arg = arg;

    xQueueSend(hid_event_queue, &evt_queue, 0);
}

void hid_host_keyboard_report_output(char report){
    if (usb_keyboard_handle != NULL) {
        #if DEBUG_LOG
        ESP_LOGI(pcTaskGetName(NULL), "CapsLock ping => %lld us", esp_timer_get_time()-report_time);
        #endif
        esp_err_t ret = hid_class_request_set_report(usb_keyboard_handle, HID_REPORT_TYPE_OUTPUT, 0, (void*)&report, sizeof(char));
        assert(ret == ESP_OK);
    }
}

/**
 * @brief Start USB Host install and handle common USB host library events
 *
 * @param[in] arg  Not used
 */
void usb_lib_task(void *arg){
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
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // In this example, there is only one client registered
        // So, once we deregister the client, this call must succeed with ESP_OK
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(LOG_TITLE, "USB shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

void hid_lib_task(void *arg){
    hid_event_queue_t evt_queue;

    while (true) {
        if (xQueueReceive(hid_event_queue, &evt_queue, portMAX_DELAY)) {
            hid_host_device_event(evt_queue.handle, evt_queue.event, evt_queue.arg);
        }
    }
}

void usb_init(void){
    /*
    * Create usb_lib_task to:
    * - initialize USB Host library
    * - Handle USB Host events while APP pin in in HIGH state
    */
    hid_event_queue = xQueueCreate(10, sizeof(hid_event_queue_t));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, NULL, USB_TASK_PRIORITY, NULL, USB_TASK_COREID);
    xTaskCreatePinnedToCore(hid_lib_task, "hid_events", 4096, NULL, USB_TASK_PRIORITY, NULL, USB_TASK_COREID);
}

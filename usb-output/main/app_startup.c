#include "app_startup.h"
#include "config.h"
#include "macro_profile.h"
#include "macro_web.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STARTUP_HID_STACK  8192
#define STARTUP_WIFI_STACK 12288
#define STARTUP_HID_PRIO   20
#define STARTUP_WIFI_PRIO  12

static void startup_hid_task(void *arg)
{
    (void)arg;
    ESP_LOGI(LOG_TITLE, "HID/USB startup");

    /* Queues must exist before the SPI receiver task can enqueue reports. */
    hid_init_multiplexer();
    spi_init_slave_hid_receiver();
    spi_init_master_pc_sender();

#if CUSTOM_CONFIG
    macro_profile_init(&macro_sequence);
#else
    macro_profile_init(&macro_sequence_default);
#endif
    macro_init();
    tud_user_initialization();

    ESP_LOGI(LOG_TITLE, "HID/USB ready");
    vTaskDelete(NULL);
}

#if CONFIG_MACRO_WEB_UI
static void startup_wifi_task(void *arg)
{
    (void)arg;
    ESP_LOGI(LOG_TITLE, "Wi-Fi/web startup");
    macro_web_start();
    ESP_LOGI(LOG_TITLE, "Wi-Fi/web startup done");
    vTaskDelete(NULL);
}
#endif

void app_startup_run(void)
{
#if CONFIG_MACRO_WEB_UI
    if (xTaskCreatePinnedToCore(startup_wifi_task, "startup_wifi", STARTUP_WIFI_STACK, NULL,
                                STARTUP_WIFI_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(LOG_TITLE, "startup_wifi task create failed");
    }
#endif
    if (xTaskCreatePinnedToCore(startup_hid_task, "startup_hid", STARTUP_HID_STACK, NULL,
                                STARTUP_HID_PRIO, NULL, 1) != pdPASS) {
        ESP_LOGE(LOG_TITLE, "startup_hid task create failed");
    }
}

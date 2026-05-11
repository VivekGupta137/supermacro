// Import global project config
#include "config.h"
#include "macro_profile.h"
#include "macro_web.h"

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // At sleep in case of computer boot
    ESP_LOGI(LOG_TITLE, "Starting -MacroPassthrough- application");

    // Initialize SPI
    spi_init_slave_hid_receiver();
    spi_init_master_pc_sender();

    // Initialize hid multiplexer worker (aggregate keyboard report & macro report)
    hid_init_multiplexer();

#if CUSTOM_CONFIG
    macro_profile_init(&macro_sequence);
#else
    macro_profile_init(&macro_sequence_default);
#endif

    // Initialize macro configuration (timers from group_sequence)
    macro_init();

#if CONFIG_MACRO_WEB_UI
    /* Wi-Fi before USB: more reliable SoftAP bring-up on ESP32-S3 with USB device stack. */
    macro_web_start();
#endif

    // Initialize TinyUSB
    tud_user_initialization();

    // Leave main() in background
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

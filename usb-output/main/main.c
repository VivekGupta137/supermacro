// Import global project config
#include "config.h"
#include "app_startup.h"

void app_main(void)
{
    /* Brief delay for host/USB-input boot; HID and Wi-Fi then start in parallel. */
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(LOG_TITLE, "Starting -MacroPassthrough- (parallel HID + Wi-Fi)");

    app_startup_run();

    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}

// Import global project config
#include "config.h"

void app_main(void)
{
    ESP_LOGI(LOG_TITLE, "Starting USB Input");

    // SPI first; USB host starts after a short hub power-up delay inside usb_lib_task.
    spi_init_master_hid_sender();
    spi_init_slave_pc_receiver();
    usb_init();

    // Leave main() in background
    while (true)
    {
        vTaskDelay(portMAX_DELAY);
    }
}

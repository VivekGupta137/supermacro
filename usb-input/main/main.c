// Import global project config
#include "config.h"

static void spi_slave_deferred_init_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(USB_SPI_SLAVE_DEFER_MS));
    spi_init_slave_pc_receiver();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(LOG_TITLE, "Starting USB Input (built %s %s)", __DATE__, __TIME__);

    spi_init_master_hid_sender();
    usb_init();
    xTaskCreate(spi_slave_deferred_init_task, "spi_pc_defer", 2048, NULL, 10, NULL);

    // Leave main() in background
    while (true)
    {
        vTaskDelay(portMAX_DELAY);
    }
}

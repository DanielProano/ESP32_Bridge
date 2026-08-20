#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("MAIN", "ESP32 Bridge starting");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

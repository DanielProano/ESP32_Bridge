#include "bridge.h"
#include "oled.h"
#include "freertos/task.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("MAIN", "ESP32 Bridge starting");

    wifi_init();
    uart_init_stm32();
    uart_init_crsf();
    oled_init();

    g_cmd_queue = xQueueCreate(LAPTOP_QUEUE_DEPTH, sizeof(STM32_CMD));
    g_client_sock_mutex = xSemaphoreCreateMutex();

    xTaskCreate(crsf_to_stm32_task, "crsf_to_stm32", BRIDGE_TASK_STACK_SIZE, NULL, CRSF_TO_STM32_TASK_PRIORITY, NULL);
    xTaskCreate(stm32_to_laptop_task, "stm32_to_laptop", BRIDGE_TASK_STACK_SIZE, NULL, STM32_TO_LAPTOP_TASK_PRIORITY, NULL);
    xTaskCreate(queue_to_stm32_task, "queue_to_stm32", BRIDGE_TASK_STACK_SIZE, NULL, QUEUE_TO_STM32_TASK_PRIORITY, NULL);
    xTaskCreate(tcp_server_task, "tcp_server", BRIDGE_TASK_STACK_SIZE, NULL, TCP_SERVER_TASK_PRIORITY, NULL);
}

#include "bridge.h"
#include "freertos/task.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("MAIN", "ESP32 Bridge starting");

    wifi_init();
    uart_init_stm32();
    uart_init_crsf();

    g_cmd_queue = xQueueCreate(LAPTOP_QUEUE_DEPTH, sizeof(STM32_CMD));
    g_client_sock_mutex = xSemaphoreCreateMutex();

    xTaskCreate(crsf_rx_task, "crsf_rx", BRIDGE_TASK_STACK_SIZE, NULL, CRSF_RX_TASK_PRIORITY, NULL);
    xTaskCreate(stm32_rx_task, "stm32_rx", BRIDGE_TASK_STACK_SIZE, NULL, STM32_RX_TASK_PRIORITY, NULL);
    xTaskCreate(stm32_tx_task, "stm32_tx", BRIDGE_TASK_STACK_SIZE, NULL, STM32_TX_TASK_PRIORITY, NULL);
    xTaskCreate(tcp_server_task, "tcp_server", BRIDGE_TASK_STACK_SIZE, NULL, TCP_SERVER_TASK_PRIORITY, NULL);
}

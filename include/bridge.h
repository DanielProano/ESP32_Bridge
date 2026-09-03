#ifndef BRIDGE_H
#define BRIDGE_H

#include "protocol.h"
#include "stdbool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include <stdint.h>

#define STM32_UART      UART_NUM_1
#define STM32_TX_PIN    17
#define STM32_RX_PIN    16
#define STM32_BAUD      921600
#define STM32_UART_BUF_SIZE 512

#define TCP_PORT        8080
#define WIFI_SSID       "Dragonfly"
#define WIFI_PASS       "dragonfly123"

#define LAPTOP_QUEUE_DEPTH       4
#define STM32_LINK_TIMEOUT_MS    1000
#define STM32_UART_MUTEX_TIMEOUT_MS 10
#define CLIENT_SOCK_MUTEX_TIMEOUT_MS 10
#define STM32_UART_READ_TIMEOUT_MS   50

#define BRIDGE_TASK_STACK_SIZE      4096

#define STM32_TO_LAPTOP_TASK_PRIORITY 5
#define QUEUE_TO_STM32_TASK_PRIORITY  4
#define TCP_SERVER_TASK_PRIORITY      3

typedef struct {
    uint8_t msg_id;
    uint8_t payload_len;
    uint8_t payload[PAYLOAD_MAX_SIZE];
} STM32_CMD;

extern int g_client_sock;
extern QueueHandle_t g_cmd_queue;
extern SemaphoreHandle_t g_client_sock_mutex;
extern SemaphoreHandle_t g_stm32_uart_mutex;

void wifi_init(void);
void uart_init_stm32(void);
void tcp_server_task(void *pvParameters);
void queue_to_stm32_task(void *pvParameters);
void stm32_to_laptop_task(void *pvParameters);
void bridge_to_stm32(uint8_t msg_id, const uint8_t *payload, uint8_t payload_len);
void bridge_to_laptop(const FRAME *frame);

#endif

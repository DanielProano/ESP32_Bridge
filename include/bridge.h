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

#define CRSF_UART       UART_NUM_2
#define CRSF_RX_PIN     18
#define CRSF_BAUD       420000
#define CRSF_UART_BUF_SIZE  256

#define TCP_PORT        8080
#define WIFI_SSID       "Dragonfly"
#define WIFI_PASS       "dragonfly123"

#define LAPTOP_QUEUE_DEPTH       4
#define CRSF_FAILSAFE_TIMEOUT_MS 500
#define CRSF_MUTEX_TIMEOUT_MS    10
#define STM32_UART_MUTEX_TIMEOUT_MS 10

typedef struct {
    RC_CHANNELS_PAYLOAD data;
    uint32_t last_update_ticks;
} CRSF_SLOT;

typedef struct {
    uint8_t msg_id;
    uint8_t payload_len;
    uint8_t payload[PAYLOAD_MAX_SIZE];
} STM32_CMD;

extern int g_client_sock;
extern QueueHandle_t g_cmd_queue;
extern CRSF_SLOT g_crsf_slot;
extern SemaphoreHandle_t g_client_sock_mutex;
extern SemaphoreHandle_t g_stm32_uart_mutex;
extern SemaphoreHandle_t g_crsf_mutex;

void wifi_init(void);
void uart_init_stm32(void);
void uart_init_crsf(void);
void tcp_server_task(void *pvParameters);
void stm32_tx_task(void *pvParameters);
void stm32_rx_task(void *pvParameters);
void crsf_rx_task(void *pvParameters);
void bridge_to_stm32(uint8_t msg_id, const uint8_t *payload, uint8_t payload_len);
void bridge_to_laptop(const FRAME *frame);
void crsf_to_stm32(const RC_CHANNELS_PAYLOAD *rc);
bool crsf_is_stale(void);

#endif

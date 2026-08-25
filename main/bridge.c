#include "bridge.h"
#include "protocol_codec.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <string.h>

CRSF_SLOT g_crsf_slot = {0};
SemaphoreHandle_t g_crsf_mutex = NULL;
SemaphoreHandle_t g_stm32_uart_mutex = NULL;

void wifi_init(void)
{
    // Example code: https://github.com/espressif/esp-idf/blob/master/examples/wifi/getting_started/station/main/station_example_main.c
    // Init flash & if full, erase and write
    esp_err_t non_volatile_storage = nvs_flash_init();
    if (non_volatile_storage == ESP_ERR_NVS_NO_FREE_PAGES || non_volatile_storage == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        non_volatile_storage = nvs_flash_init();
    }

    ESP_ERROR_CHECK(non_volatile_storage);

    // bringup TCP
    ESP_ERROR_CHECK(esp_netif_init());

    // system queue structure for events
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Enter AP Mode for ESP32 to create its own Wi-Fi network
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    // example code: https://github.com/espressif/esp-idf/blob/master/examples/wifi/getting_started/softAP/main/softap_example_main.c
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = (uint8_t) strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void uart_init_stm32(void)
{
    // example code: https://github.com/espressif/esp-idf/blob/master/examples/peripherals/uart/uart_echo/main/uart_echo_example_main.c
    uart_config_t uart_config = {
        .baud_rate = STM32_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        // Flow control allows receiver to pause sender,
        //something we don't need
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(STM32_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART, STM32_TX_PIN, STM32_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(STM32_UART, STM32_UART_BUF_SIZE, STM32_UART_BUF_SIZE, 0, NULL, 0));

    g_stm32_uart_mutex = xSemaphoreCreateMutex();
}

void uart_init_crsf(void)
{
    uart_config_t uart_config = {
        .baud_rate = CRSF_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(CRSF_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CRSF_UART, UART_PIN_NO_CHANGE, CRSF_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CRSF_UART, CRSF_UART_BUF_SIZE, 0, 0, NULL, 0));

    g_crsf_mutex = xSemaphoreCreateMutex();
}

// if semaphore occupcied and waits CRSF_MUTEX_TIMEOUT_MS
// time, then return true. 
bool crsf_is_stale(void)
{
    // need the mutex to guarantee we get most 
    // up to date ticks info from struct
    if (xSemaphoreTake(g_crsf_mutex, pdMS_TO_TICKS(CRSF_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return true;
    }

    uint32_t last_update_ticks = g_crsf_slot.last_update_ticks;
    xSemaphoreGive(g_crsf_mutex);

    return (xTaskGetTickCount() - last_update_ticks) > pdMS_TO_TICKS(CRSF_FAILSAFE_TIMEOUT_MS);
}

void bridge_to_stm32(uint8_t msg_id, const uint8_t *payload, uint8_t payload_len)
{
    static uint8_t sequence = 0;

    if (payload_len > PAYLOAD_MAX_SIZE) {
        return;
    }

    FRAME frame = {
        .start_byte  = PROTOCOL_START_BYTE,
        .version     = PROTOCOL_VERSION,
        .message_id  = msg_id,
        .sequence    = sequence++,
        .payload_len = payload_len,
    };

    if (payload_len > 0) {
        memcpy(frame.payload, payload, payload_len);
    }

    uint8_t buffer[sizeof(FRAME)];
    int encoded_len = protocol_frame_encode(buffer, sizeof(buffer), &frame);
    if (encoded_len < 0) {
        return;
    }

    if (xSemaphoreTake(g_stm32_uart_mutex, pdMS_TO_TICKS(STM32_UART_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return;
    }

    uart_write_bytes(STM32_UART, buffer, (size_t) encoded_len);
    xSemaphoreGive(g_stm32_uart_mutex);
}

void crsf_to_stm32(const RC_CHANNELS_PAYLOAD *rc)
{
    bridge_to_stm32(MSG_RC_CHANNELS, (const uint8_t *) rc, sizeof(RC_CHANNELS_PAYLOAD));
}

void stm32_tx_task(void *pvParameters)
{
    g_cmd_queue = xQueueCreate(LAPTOP_QUEUE_DEPTH, sizeof(STM32_CMD));

    STM32_CMD cmd;
    while (1) {
        if (xQueueReceive(g_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            bridge_to_stm32(cmd.msg_id, cmd.payload, cmd.payload_len);
        }
    }
}

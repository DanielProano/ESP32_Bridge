#include "bridge.h"
#include "protocol_codec.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

#define CRSF_RC_CHANNELS_PAYLOAD_SIZE 22

int g_client_sock = -1;
QueueHandle_t g_cmd_queue = NULL;
CRSF_SLOT g_crsf_slot = {0};
SemaphoreHandle_t g_crsf_mutex = NULL;
SemaphoreHandle_t g_stm32_uart_mutex = NULL;
SemaphoreHandle_t g_client_sock_mutex = NULL;

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

// if semaphore occupied and waits CRSF_MUTEX_TIMEOUT_MS
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

void bridge_to_laptop(const FRAME *frame)
{
    uint8_t buffer[sizeof(FRAME)];
    int encoded_len = protocol_frame_encode(buffer, sizeof(buffer), frame);
    if (encoded_len < 0) {
        return;
    }

    if (xSemaphoreTake(g_client_sock_mutex, pdMS_TO_TICKS(CLIENT_SOCK_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return;
    }

    if (g_client_sock >= 0) {
        send(g_client_sock, buffer, (size_t) encoded_len, 0);
    }

    xSemaphoreGive(g_client_sock_mutex);
}

void stm32_tx_task(void *pvParameters)
{
    STM32_CMD cmd;
    while (1) {
        if (xQueueReceive(g_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            bridge_to_stm32(cmd.msg_id, cmd.payload, cmd.payload_len);
        }
    }
}

void stm32_rx_task(void *pvParameters)
{
    uint8_t buffer[sizeof(FRAME)];

    while (1) {
        uint8_t start_byte;

        if (uart_read_bytes(STM32_UART, &start_byte, 1, portMAX_DELAY) != 1) {
            continue;
        }

        if (start_byte != PROTOCOL_START_BYTE) {
            continue;
        }

        buffer[0] = start_byte;
        uint8_t header_info = 4;

        if (uart_read_bytes(STM32_UART, &buffer[1], header_info, pdMS_TO_TICKS(STM32_UART_READ_TIMEOUT_MS)) != 4) {
            continue;
        }

        uint8_t payload_len = buffer[4];

        if (payload_len > PAYLOAD_MAX_SIZE) {
            continue;
        }

        size_t remaining = (size_t) payload_len + sizeof(uint16_t);
        if (uart_read_bytes(STM32_UART, &buffer[5], remaining, pdMS_TO_TICKS(STM32_UART_READ_TIMEOUT_MS)) != (int) remaining) {
            continue;
        }

        FRAME frame;

        if (protocol_frame_decode(&frame, buffer, sizeof(uint8_t) + header_info + remaining) < 0) {
            continue;
        }

        bridge_to_laptop(&frame);
    }
}

static int tcp_server_setup(void) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        vTaskDelete(NULL);
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(TCP_PORT),
    };

    if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        vTaskDelete(NULL);
    }

    if (listen(listen_sock, 1) < 0) {
        vTaskDelete(NULL);
    }

    return listen_sock;
}

static int recv_exact(int sock, uint8_t *buffer, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int n = recv(sock, &buffer[received], len - received, 0);
        if (n <= 0) {
            return -1;
        }
        received += (size_t) n;
    }
    return (int) received;
}

typedef enum {
    OK,
    CONTINUE,
    BREAK,
} TCP_CLIENT_STATUS;

static TCP_CLIENT_STATUS tcp_server_read_client(int client_sock, uint8_t *buffer) {
    uint8_t start_byte;

    if (recv(client_sock, &start_byte, 1, 0) <= 0) {
        return BREAK;
    }

    if (start_byte != PROTOCOL_START_BYTE) {
        return CONTINUE;
    }
    
    buffer[0] = start_byte;

    if (recv_exact(client_sock, &buffer[1], 4) < 0) {
        return BREAK;
    }

    uint8_t payload_len = buffer[4];
    if (payload_len > PAYLOAD_MAX_SIZE) {
        return CONTINUE;
    }

    size_t remaining = (size_t) payload_len + sizeof(uint16_t);
    if (recv_exact(client_sock, &buffer[5], remaining) < 0) {
        return BREAK;
    }

    FRAME frame;
    if (protocol_frame_decode(&frame, buffer, 5 + remaining) < 0) {
        return CONTINUE;
    }

    STM32_CMD cmd = {
        .msg_id      = frame.message_id,
        .payload_len = frame.payload_len,
    };
    memcpy(cmd.payload, frame.payload, frame.payload_len);
    xQueueSend(g_cmd_queue, &cmd, 0);

    return OK;
}

void tcp_server_task(void *pvParameters)
{
    int listen_sock = tcp_server_setup();
    while (1) {
        int client_sock = accept(listen_sock, NULL, NULL);
        if (client_sock < 0) {
            continue;
        }

        xSemaphoreTake(g_client_sock_mutex, portMAX_DELAY);
        g_client_sock = client_sock;
        xSemaphoreGive(g_client_sock_mutex);

        uint8_t buffer[sizeof(FRAME)];
        bool client_connected = true;
        while (client_connected) {
            TCP_CLIENT_STATUS stat = tcp_server_read_client(client_sock, buffer);
            switch (stat) {
                case OK:
                    break;
                case CONTINUE:
                    break;
                case BREAK:
                    client_connected = false;
                    break;
            }
        }

        xSemaphoreTake(g_client_sock_mutex, portMAX_DELAY);
        g_client_sock = -1;
        xSemaphoreGive(g_client_sock_mutex);

        close(client_sock);
    }
}

static uint8_t crsf_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t) ((crc << 1) ^ 0xD5) : (uint8_t) (crc << 1);
        }
    }
    return crc;
}

static void crsf_unpack_channels(const uint8_t *payload, int16_t *channels)
{
    int bit_pos = 0;
    for (int ch = 0; ch < 16; ch++) {
        uint16_t value = 0;
        for (int b = 0; b < 11; b++) {
            int byte_index = bit_pos / 8;
            int bit_index = bit_pos % 8;
            if (payload[byte_index] & (1 << bit_index)) {
                value |= (1 << b);
            }
            bit_pos++;
        }
        channels[ch] = (int16_t) value;
    }
}

// reference doc: https://github.com/tbs-fpv/tbs-crsf-spec/blob/main/crsf.md
void crsf_rx_task(void *pvParameters)
{
    uint8_t buffer[CRSF_MAX_FRAME_SIZE];

    while (1) {
        uint8_t sync_byte;
        if (uart_read_bytes(CRSF_UART, &sync_byte, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (sync_byte != CRSF_SYNC_BYTE) {
            continue;
        }

        uint8_t frame_len;
        if (uart_read_bytes(CRSF_UART, &frame_len, 1, pdMS_TO_TICKS(CRSF_UART_READ_TIMEOUT_MS)) != 1) {
            continue;
        }
        if (frame_len < 2 || frame_len > CRSF_MAX_FRAME_SIZE - 2) {
            continue;
        }

        if (uart_read_bytes(CRSF_UART, buffer, frame_len, pdMS_TO_TICKS(CRSF_UART_READ_TIMEOUT_MS)) != frame_len) {
            continue;
        }

        uint8_t type = buffer[0];
        uint8_t received_crc = buffer[frame_len - 1];
        uint8_t calc_crc = crsf_crc8(buffer, (size_t) frame_len - 1);
        if (received_crc != calc_crc) {
            continue;
        }

        if (type != CRSF_FRAMETYPE_RC_CHANNELS_PACKED) {
            continue;
        }

        if (frame_len != 1 + CRSF_RC_CHANNELS_PAYLOAD_SIZE + 1) {
            continue;
        }

        int16_t channels[16];
        crsf_unpack_channels(&buffer[1], channels);

        RC_CHANNELS_PAYLOAD rc = {0};
        rc.timestamp = xTaskGetTickCount();
        rc.valid_mask = 0xFFFF;
        memcpy(rc.channels, channels, sizeof(channels));

        if (xSemaphoreTake(g_crsf_mutex, pdMS_TO_TICKS(CRSF_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            g_crsf_slot.data = rc;
            g_crsf_slot.last_update_ticks = xTaskGetTickCount();
            xSemaphoreGive(g_crsf_mutex);
        }

        crsf_to_stm32(&rc);
    }
}

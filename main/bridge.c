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
#define CRSF_NUM_OF_CHANNELS 16
#define CRSF_NUM_OF_BITS 11

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

void queue_to_stm32_task(void *pvParameters)
{
    STM32_CMD cmd;
    while (1) {
        if (xQueueReceive(g_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            bridge_to_stm32(cmd.msg_id, cmd.payload, cmd.payload_len);
        }
    }
}

void stm32_to_laptop_task(void *pvParameters)
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

/* 
 * Model: CRC-8 / DVB-S2 (8-bit)
 * Poly: 0xD5, Init: 0x00, XorOut: 0x00
 * CRC Table C Source Code & Lookup Table Generator
 * 
 * Found at: https://crc-calc.com/crc-8-dvb-s2/
 */

static const uint8_t crc_table[256] = {
  0x00, 0xD5, 0x7F, 0xAA, 0xFE, 0x2B, 0x81, 0x54, 
  0x29, 0xFC, 0x56, 0x83, 0xD7, 0x02, 0xA8, 0x7D, 
  0x52, 0x87, 0x2D, 0xF8, 0xAC, 0x79, 0xD3, 0x06, 
  0x7B, 0xAE, 0x04, 0xD1, 0x85, 0x50, 0xFA, 0x2F, 
  0xA4, 0x71, 0xDB, 0x0E, 0x5A, 0x8F, 0x25, 0xF0, 
  0x8D, 0x58, 0xF2, 0x27, 0x73, 0xA6, 0x0C, 0xD9, 
  0xF6, 0x23, 0x89, 0x5C, 0x08, 0xDD, 0x77, 0xA2, 
  0xDF, 0x0A, 0xA0, 0x75, 0x21, 0xF4, 0x5E, 0x8B, 
  0x9D, 0x48, 0xE2, 0x37, 0x63, 0xB6, 0x1C, 0xC9, 
  0xB4, 0x61, 0xCB, 0x1E, 0x4A, 0x9F, 0x35, 0xE0, 
  0xCF, 0x1A, 0xB0, 0x65, 0x31, 0xE4, 0x4E, 0x9B, 
  0xE6, 0x33, 0x99, 0x4C, 0x18, 0xCD, 0x67, 0xB2, 
  0x39, 0xEC, 0x46, 0x93, 0xC7, 0x12, 0xB8, 0x6D, 
  0x10, 0xC5, 0x6F, 0xBA, 0xEE, 0x3B, 0x91, 0x44, 
  0x6B, 0xBE, 0x14, 0xC1, 0x95, 0x40, 0xEA, 0x3F, 
  0x42, 0x97, 0x3D, 0xE8, 0xBC, 0x69, 0xC3, 0x16, 
  0xEF, 0x3A, 0x90, 0x45, 0x11, 0xC4, 0x6E, 0xBB, 
  0xC6, 0x13, 0xB9, 0x6C, 0x38, 0xED, 0x47, 0x92, 
  0xBD, 0x68, 0xC2, 0x17, 0x43, 0x96, 0x3C, 0xE9, 
  0x94, 0x41, 0xEB, 0x3E, 0x6A, 0xBF, 0x15, 0xC0, 
  0x4B, 0x9E, 0x34, 0xE1, 0xB5, 0x60, 0xCA, 0x1F, 
  0x62, 0xB7, 0x1D, 0xC8, 0x9C, 0x49, 0xE3, 0x36, 
  0x19, 0xCC, 0x66, 0xB3, 0xE7, 0x32, 0x98, 0x4D, 
  0x30, 0xE5, 0x4F, 0x9A, 0xCE, 0x1B, 0xB1, 0x64, 
  0x72, 0xA7, 0x0D, 0xD8, 0x8C, 0x59, 0xF3, 0x26, 
  0x5B, 0x8E, 0x24, 0xF1, 0xA5, 0x70, 0xDA, 0x0F, 
  0x20, 0xF5, 0x5F, 0x8A, 0xDE, 0x0B, 0xA1, 0x74, 
  0x09, 0xDC, 0x76, 0xA3, 0xF7, 0x22, 0x88, 0x5D, 
  0xD6, 0x03, 0xA9, 0x7C, 0x28, 0xFD, 0x57, 0x82, 
  0xFF, 0x2A, 0x80, 0x55, 0x01, 0xD4, 0x7E, 0xAB, 
  0x84, 0x51, 0xFB, 0x2E, 0x7A, 0xAF, 0x05, 0xD0, 
  0xAD, 0x78, 0xD2, 0x07, 0x53, 0x86, 0x2C, 0xF9
};

static uint8_t crsf_crc8(const uint8_t *data, size_t length) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < length; i++) {
        crc = crc_table[crc ^ data[i]];
    }
    return crc;
}

// CRSF packs 11 bits per channel for 16 channels,
// total of 22 bytes
// We unpack that formats into memory with 2 bytes
// per channel
static void crsf_unpack_channels(const uint8_t *payload, int16_t *channels)
{
    int bit_pos = 0;
    for (int channel = 0; channel < CRSF_NUM_OF_CHANNELS; channel++) {
        uint16_t value = 0;
        for (int b = 0; b < CRSF_NUM_OF_BITS; b++) {
            int byte_index = bit_pos / 8;
            int bit_index = bit_pos % 8;
            if (payload[byte_index] & (1 << bit_index)) {
                value |= (1 << b);
            }
            bit_pos++;
        }
        channels[channel] = (int16_t) value;
    }
}

// reference doc: https://github.com/tbs-fpv/tbs-crsf-spec/blob/main/crsf.md
void crsf_to_stm32_task(void *pvParameters)
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

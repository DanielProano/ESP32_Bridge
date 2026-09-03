#include "bridge.h"
#include "oled.h"
#include "protocol.h"
#include "protocol_codec.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

int g_client_sock = -1;
QueueHandle_t g_cmd_queue = NULL;
SemaphoreHandle_t g_stm32_uart_mutex = NULL;
SemaphoreHandle_t g_client_sock_mutex = NULL;

static volatile uint32_t g_stm32_frames_ok = 0;
static volatile uint32_t g_stm32_frames_err = 0;
static volatile TickType_t g_stm32_last_frame_ticks = 0;

void wifi_init(void)
{
    // Example code: https://github.com/espressif/esp-idf/blob/master/examples/wifi/getting_started/station/main/station_example_main.c
    // Init flash & if full, erase and write
    esp_err_t non_volatile_storage = nvs_flash_init();
    if (non_volatile_storage == ESP_ERR_NVS_NO_FREE_PAGES || non_volatile_storage == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        oled_error("Wifi NVS Full");
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

    oled_print("Wifi Init Complete");
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
        // something we don't need
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(STM32_UART, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART, STM32_TX_PIN, STM32_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(STM32_UART, STM32_UART_BUF_SIZE, STM32_UART_BUF_SIZE, 0, NULL, 0));

    g_stm32_uart_mutex = xSemaphoreCreateMutex();

    oled_print("UART-STM Init Complete");
}

void bridge_to_stm32(uint8_t msg_id, const uint8_t *payload, uint8_t payload_len)
{
    static uint8_t sequence = 0;

    if (payload_len > PAYLOAD_MAX_SIZE) {
        oled_error("B2STM len > max");
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
        oled_error("B2STM 0 buf len");
        return;
    }

    if (xSemaphoreTake(g_stm32_uart_mutex, pdMS_TO_TICKS(STM32_UART_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return;
    }

    uart_write_bytes(STM32_UART, buffer, (size_t) encoded_len);
    xSemaphoreGive(g_stm32_uart_mutex);
}

void bridge_to_laptop(const FRAME *frame)
{
    uint8_t buffer[sizeof(FRAME)];
    int encoded_len = protocol_frame_encode(buffer, sizeof(buffer), frame);
    if (encoded_len < 0) {
        oled_error("B2Lap 0 buf len");
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
    FRAME frame;
    while (1) {
        if (xQueueReceive(g_cmd_queue, &frame, portMAX_DELAY) == pdTRUE) {
            bridge_to_stm32(frame.message_id, frame.payload, frame.payload_len);
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
            g_stm32_frames_err++;
            continue;
        }

        uint8_t payload_len = buffer[4];

        if (payload_len > PAYLOAD_MAX_SIZE) {
            g_stm32_frames_err++;
            continue;
        }

        size_t remaining = (size_t) payload_len + sizeof(uint16_t);
        if (uart_read_bytes(STM32_UART, &buffer[5], remaining, pdMS_TO_TICKS(STM32_UART_READ_TIMEOUT_MS)) != (int) remaining) {
            g_stm32_frames_err++;
            continue;
        }

        FRAME frame;

        if (protocol_frame_decode(&frame, buffer, sizeof(uint8_t) + header_info + remaining) < 0) {
            g_stm32_frames_err++;
            continue;
        }

        g_stm32_frames_ok++;
        g_stm32_last_frame_ticks = xTaskGetTickCount();

        bridge_to_laptop(&frame);
    }
}

static int tcp_server_setup(void) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        oled_error("TCP Deleted Tsk");
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
        oled_error("TCP Deleted Tsk");
        vTaskDelete(NULL);
    }

    if (listen(listen_sock, 1) < 0) {
        oled_error("TCP Deleted Tsk");
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
            oled_error("RECV 0 bytes");
            return -1;
        }
        received += (size_t) n;
    }
    return (int) received;
}

static void esp32_send_status(int client_sock, uint8_t sequence)
{
    ESP_LOGI("MAIN", "ESP32 Bridge starting");
    wifi_sta_list_t sta_list = {0};
    esp_wifi_ap_get_sta_list(&sta_list);

    TickType_t now = xTaskGetTickCount();
    uint32_t frame_age_ms = (uint32_t) ((now - g_stm32_last_frame_ticks) * portTICK_PERIOD_MS);

    ESP32_STATUS_PAYLOAD status = {
        .uptime_ms               = (uint32_t) (esp_timer_get_time() / 1000),
        .free_heap_bytes         = esp_get_free_heap_size(),
        .wifi_client_count       = (uint8_t) sta_list.num,
        .stm32_link_up           = (g_stm32_frames_ok > 0) && (frame_age_ms < STM32_LINK_TIMEOUT_MS),
        .stm32_last_frame_age_ms = frame_age_ms,
        .stm32_frames_ok         = g_stm32_frames_ok,
        .stm32_frames_err        = g_stm32_frames_err,
    };

    FRAME frame = {
        .start_byte  = PROTOCOL_START_BYTE,
        .version     = PROTOCOL_VERSION,
        .message_id  = MSG_ESP32_STATUS,
        .sequence    = sequence,
        .payload_len = sizeof(status),
    };
    memcpy(frame.payload, &status, sizeof(status));

    uint8_t buffer[sizeof(FRAME)];
    int encoded_len = protocol_frame_encode(buffer, sizeof(buffer), &frame);
    if (encoded_len < 0) {
        oled_error("ESP32 0 buf len");
        return;
    }

    send(client_sock, buffer, (size_t) encoded_len, 0);
}

static bool tcp_server_read_client(int client_sock, uint8_t *buffer) {
    uint8_t start_byte;

    if (recv(client_sock, &start_byte, 1, 0) <= 0) {
        return false;
    }

    if (start_byte != PROTOCOL_START_BYTE) {
        return true;
    }

    buffer[0] = start_byte;

    if (recv_exact(client_sock, &buffer[1], 4) < 0) {
        oled_error("TCP_cl fail recv");
        return false;
    }

    uint8_t payload_len = buffer[4];
    if (payload_len > PAYLOAD_MAX_SIZE) {
        oled_error("TCP_cl len > max");
        return true;
    }

    size_t remaining = (size_t) payload_len + sizeof(uint16_t);
    if (recv_exact(client_sock, &buffer[5], remaining) < 0) {
        oled_error("TCP_cl recv fail");
        return false;
    }

    FRAME frame;
    if (protocol_frame_decode(&frame, buffer, 5 + remaining) < 0) {
        oled_error("TCP_cl fail decode");
        return true;
    }

    if (frame.message_id == MSG_ESP32_STATUS) {
        esp32_send_status(client_sock, frame.sequence);
        return true;
    }

    if (frame.message_id == MSG_OLED) {
        OLED_PAYLOAD payload;
        memcpy(&payload, frame.payload, frame.payload_len);
        switch (payload.cmd) {
            case OLED_PRINT: {
                static const char prefix[] = "GS: ";
                size_t prefix_len = strlen(prefix);
                size_t text_len = strnlen(payload.text, PAYLOAD_TEXT_SIZE);
                size_t buf_cap = OLED_LOG_LINE_LEN - 1;

                char buf[OLED_LOG_LINE_LEN];
                size_t first_len = buf_cap - prefix_len;
                if (first_len > text_len) {
                    first_len = text_len;
                }
                memcpy(buf, prefix, prefix_len);
                memcpy(buf + prefix_len, payload.text, first_len);
                buf[prefix_len + first_len] = '\0';
                oled_print(buf);

                size_t offset = first_len;
                while (offset < text_len) {
                    size_t chunk_len = text_len - offset;
                    if (chunk_len > buf_cap) {
                        chunk_len = buf_cap;
                    }
                    memcpy(buf, payload.text + offset, chunk_len);
                    buf[chunk_len] = '\0';
                    oled_print(buf);
                    offset += chunk_len;
                }
                break;
            }
            case OLED_CLEAR:
                oled_clear();
                break;
            default:
                break;
        }
        return true;
    }

    xQueueSend(g_cmd_queue, &frame, 0);

    char buf[24];
    snprintf(buf, sizeof(buf), "TCP got CMD: %d", frame.message_id);
    oled_print(buf);

    return true;
}

void tcp_server_task(void *pvParameters)
{
    int listen_sock = tcp_server_setup();
    while (1) {
        int client_sock = accept(listen_sock, NULL, NULL);
        if (client_sock < 0) {
            oled_error("Client accept fail");
            continue;
        }

        xSemaphoreTake(g_client_sock_mutex, portMAX_DELAY);
        g_client_sock = client_sock;
        xSemaphoreGive(g_client_sock_mutex);

        uint8_t buffer[sizeof(FRAME)];
        while (tcp_server_read_client(client_sock, buffer)) {
        }

        xSemaphoreTake(g_client_sock_mutex, portMAX_DELAY);
        g_client_sock = -1;
        xSemaphoreGive(g_client_sock_mutex);

        close(client_sock);
    }
}

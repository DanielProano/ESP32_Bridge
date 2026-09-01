#include "oled.h"
#include "u8g2.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define OLED_LOG_LINES    4
#define OLED_LOG_LINE_LEN 24

static char s_log_lines[OLED_LOG_LINES][OLED_LOG_LINE_LEN];

static u8g2_t s_u8g2;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_tx_buf[32];
static uint16_t s_tx_len;

static uint8_t u8x8_byte_esp32_i2c(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
        case U8X8_MSG_BYTE_INIT:
        case U8X8_MSG_BYTE_SET_DC:
            break;

        case U8X8_MSG_BYTE_START_TRANSFER:
            s_tx_len = 0;
            break;

        case U8X8_MSG_BYTE_SEND:
            memcpy(&s_tx_buf[s_tx_len], arg_ptr, arg_int);
            s_tx_len += arg_int;
            break;

        case U8X8_MSG_BYTE_END_TRANSFER:
            i2c_master_transmit(s_dev, s_tx_buf, s_tx_len, 1000);
            break;

        default:
            return 0;
    }
    return 1;
}

static uint8_t u8x8_gpio_and_delay_esp32(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT: {
            i2c_master_bus_config_t bus_cfg = {
                .i2c_port = OLED_I2C_PORT,
                .sda_io_num = OLED_SDA_PIN,
                .scl_io_num = OLED_SCL_PIN,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .flags.enable_internal_pullup = true,
            };
            i2c_new_master_bus(&bus_cfg, &s_bus);

            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = OLED_I2C_ADDR,
                .scl_speed_hz = OLED_I2C_CLK_HZ,
            };
            i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
            break;
        }

        case U8X8_MSG_DELAY_MILLI:
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;

        case U8X8_MSG_DELAY_10MICRO:
            esp_rom_delay_us(10);
            break;

        case U8X8_MSG_DELAY_100NANO:
            esp_rom_delay_us(1);
            break;

        case U8X8_MSG_GPIO_RESET:
        case U8X8_MSG_GPIO_I2C_CLOCK:
        case U8X8_MSG_GPIO_I2C_DATA:
            break;

        default:
            return 0;
    }
    return 1;
}

void oled_init(void)
{
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0, u8x8_byte_esp32_i2c, u8x8_gpio_and_delay_esp32);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
}

void oled_print(char *msg) {
    for (int i = 0; i < OLED_LOG_LINES - 1; i++) {
        memcpy(s_log_lines[i], s_log_lines[i + 1], OLED_LOG_LINE_LEN);
    }
    snprintf(s_log_lines[OLED_LOG_LINES - 1], OLED_LOG_LINE_LEN, "%s", msg);

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tf);
    for (int i = 0; i < OLED_LOG_LINES; i++) {
        u8g2_DrawStr(&s_u8g2, 0, 10 + i * 12, s_log_lines[i]);
    }
    u8g2_SendBuffer(&s_u8g2);
}

void oled_clear(void) {
    for (int i = 0; i < OLED_LOG_LINES; i++) {
        memset(s_log_lines[i], 0, OLED_LOG_LINE_LEN);
    }

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
}

void oled_show_frame(const FRAME *frame)
{
    char buf[24];
    
    // Note, in worst case of 3 chars per int, 
    // we use full 24 byte buffer
    snprintf(buf, sizeof(buf), "FRAME: MSG %d, SEQ %d", frame->message_id, frame->sequence);
    oled_print(buf);
}

void oled_error(const char *msg) {
    char buf[24];
    snprintf(buf, sizeof(buf), "ERR: %s", msg);
    oled_print(buf);
}
#ifndef OLED_H
#define OLED_H

#include "protocol.h"
#include "driver/i2c_types.h"

#define OLED_I2C_PORT    I2C_NUM_0
#define OLED_SDA_PIN     21
#define OLED_SCL_PIN     22
#define OLED_I2C_CLK_HZ  400000
#define OLED_I2C_ADDR    0x3C

void oled_init(void);
void oled_print(char *msg);
void oled_show_frame(const FRAME *frame);
void oled_error(const char *msg);

#endif

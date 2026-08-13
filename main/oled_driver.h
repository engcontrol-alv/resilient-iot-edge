#ifndef OLED_DRIVER_H
#define OLED_DRIVER_H

#include <stdint.h>

// Funções públicas expostas para o main.c
void oled_power_setup(void);
void i2c_master_init(void);
void oled_software_setup(void);
void oled_clear(void);
void oled_print(uint8_t x, uint8_t page, const char *str);

#endif // OLED_DRIVER_H
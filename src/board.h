#ifndef BOARD_H
#define BOARD_H

#include "clock.h"
#include "stm32f4xx.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool board_init(void);
void SysTick_Handler(void);
uint32_t board_millis(void);
void board_delay(uint32_t duration_ms);
void board_led(bool on);
bool board_led_is_on(void);
void board_w5500_reset(void);
bool board_spi_transfer(uint16_t address, uint8_t control, const uint8_t *tx, uint8_t *rx,
                        size_t size);
bool board_adc_read(uint16_t values[2]);
uint32_t board_spi_console_duration_us(void);
bool board_flash_transfer(const uint8_t *prefix, size_t prefix_size, const uint8_t *tx, uint8_t *rx,
                          size_t size);

#endif

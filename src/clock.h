#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_HSE_HZ 25000000U
#define BOARD_CORE_HZ 96000000U
#define BOARD_PCLK1_HZ 48000000U
#define BOARD_PCLK2_HZ 12000000U
#define BOARD_APB1_TIMER_HZ 96000000U
#define BOARD_APB2_TIMER_HZ 24000000U
#define BOARD_SPI1_HZ 3000000U
#define BOARD_ADC_HZ 6000000U
#define BOARD_CYCLES_PER_US 96U
#define BOARD_SPI_DEFAULT_HZ 750000U

bool board_clock_init(void);
uint32_t board_clock_error(void);
void NMI_Handler(void);

#endif

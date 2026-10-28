#ifndef SPI_CONSOLE_H
#define SPI_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPI_CONSOLE_MAX_BYTES 256U

typedef struct
{
    uint32_t hz;
    uint32_t mode;
    uint32_t lsb_first;
    uint32_t cs_high;
    size_t size;
    uint8_t tx[SPI_CONSOLE_MAX_BYTES];
} spi_command_t;

bool spi_command_parse(const char *text, size_t size, spi_command_t *command);
bool board_spi_console(const spi_command_t *command, uint8_t rx[SPI_CONSOLE_MAX_BYTES]);

#endif

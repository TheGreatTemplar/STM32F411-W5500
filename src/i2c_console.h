#ifndef I2C_CONSOLE_H
#define I2C_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define I2C_CONSOLE_MAX_BYTES 256U

typedef struct
{
    uint32_t hz;
    uint32_t address;
    uint32_t operation; /* 0 write, 1 read, 2 write/repeated START/read */
    uint32_t read_size;
    size_t size;
    uint8_t tx[I2C_CONSOLE_MAX_BYTES];
    uint32_t ascii;
} i2c_command_t;

typedef enum
{
    I2C_OK,
    I2C_NACK,
    I2C_BUSY,
    I2C_TIMEOUT,
    I2C_BUS_ERROR,
    I2C_ARBITRATION_LOST,
    I2C_INVALID
} i2c_error_t;

typedef struct
{
    uint32_t tx_done;
    uint32_t rx_done;
    uint32_t duration_us;
    uint32_t actual_hz;
} i2c_result_t;

bool i2c_command_parse(const char *text, size_t size, i2c_command_t *command);
bool i2c_command_valid(const i2c_command_t *command);
void board_i2c_init(void);
i2c_error_t board_i2c_console(const i2c_command_t *command, uint8_t rx[I2C_CONSOLE_MAX_BYTES],
                              i2c_result_t *result);

#endif

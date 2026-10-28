#include "i2c_console.h"
#include "spi_console.h"
#include <string.h>

static bool field(const char *text, size_t size, size_t *offset, const char *name, uint32_t *value)
{
    const size_t length = strlen(name);
    size_t digits = 0U;
    *value = 0U;
    if (*offset > size || length > size - *offset || memcmp(text + *offset, name, length) != 0)
    {
        return false;
    }
    *offset += length;
    while (*offset < size && text[*offset] >= '0' && text[*offset] <= '9')
    {
        const uint32_t digit = (uint32_t)(text[(*offset)++] - '0');
        if (*value > (UINT32_MAX - digit) / 10U)
        {
            return false;
        }
        *value = *value * 10U + digit;
        ++digits;
    }
    return digits != 0U && *offset < size && text[(*offset)++] == ' ';
}

bool i2c_command_valid(const i2c_command_t *command)
{
    return (command->hz == 100000U || command->hz == 400000U) && command->address >= 8U &&
           command->address <= 0x77U && command->operation <= 2U && command->ascii <= 1U &&
           command->size <= I2C_CONSOLE_MAX_BYTES && command->read_size <= I2C_CONSOLE_MAX_BYTES &&
           (command->operation == 1U ? command->size == 0U : command->size != 0U) &&
           (command->operation == 0U ? command->read_size == 0U : command->read_size != 0U);
}

bool i2c_command_parse(const char *text, size_t size, i2c_command_t *command)
{
    size_t offset = 0U;
    uint32_t bus;
    memset(command, 0, sizeof(*command));
    if (!field(text, size, &offset, "bus=", &bus) || bus != 1U ||
        !field(text, size, &offset, "hz=", &command->hz) ||
        !field(text, size, &offset, "addr=", &command->address) ||
        !field(text, size, &offset, "op=", &command->operation) ||
        !field(text, size, &offset, "read=", &command->read_size))
    {
        return false;
    }
    if (size - offset >= 11U && memcmp(text + offset, "format=hex\n", 11U) == 0)
    {
        offset += 11U;
    }
    else if (size - offset >= 13U && memcmp(text + offset, "format=ascii\n", 13U) == 0)
    {
        command->ascii = 1U;
        offset += 13U;
    }
    else
    {
        return false;
    }
    return console_bytes_parse(text + offset, size - offset, command->ascii != 0U, command->tx,
                               &command->size, command->operation == 1U) &&
           i2c_command_valid(command);
}

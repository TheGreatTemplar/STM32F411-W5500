#include "spi_console.h"
#include <string.h>

static int hex_digit(char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f'))
    {
        return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F'))
    {
        return c - 'A' + 10;
    }
    return -1;
}

static bool decimal(const char *text, size_t size, uint32_t *value)
{
    size_t i;
    *value = 0U;
    if (size == 0U)
    {
        return false;
    }
    for (i = 0U; i < size; ++i)
    {
        uint32_t digit;
        if ((text[i] < '0') || (text[i] > '9'))
        {
            return false;
        }
        digit = (uint32_t)(text[i] - '0');
        if (*value > (UINT32_MAX - digit) / 10U)
        {
            return false;
        }
        *value = *value * 10U + digit;
    }
    return true;
}

bool spi_clock_plan(uint32_t hz, spi_clock_plan_t *plan)
{
    uint32_t divider = 16000000U;
    uint32_t prescaler;
    if ((hz != 62500U) && (hz != 125000U) && (hz != 250000U) && (hz != 500000U) &&
        (hz != 1000000U) && (hz != 25000000U) &&
        ((hz < 2000000U) || (hz > 24000000U) || (hz % 2000000U != 0U)))
    {
        return false;
    }
    for (prescaler = 0U; prescaler < 8U; ++prescaler)
    {
        divider /= 2U;
        if (divider == hz)
        {
            plan->system_hz = 16000000U;
            plan->pclk1_hz = 16000000U;
            plan->prescaler = prescaler;
            return true;
        }
    }
    switch (hz)
    {
    case 6000000U:
        plan->system_hz = 96000000U;
        plan->prescaler = 2U;
        break;
    case 10000000U:
        plan->system_hz = 80000000U;
        plan->prescaler = 1U;
        break;
    case 12000000U:
        plan->system_hz = 96000000U;
        plan->prescaler = 1U;
        break;
    case 14000000U:
        plan->system_hz = 56000000U;
        plan->prescaler = 0U;
        break;
    case 16000000U:
        plan->system_hz = 64000000U;
        plan->prescaler = 0U;
        break;
    case 18000000U:
        plan->system_hz = 72000000U;
        plan->prescaler = 0U;
        break;
    case 20000000U:
        plan->system_hz = 80000000U;
        plan->prescaler = 0U;
        break;
    case 22000000U:
        plan->system_hz = 88000000U;
        plan->prescaler = 0U;
        break;
    case 24000000U:
        plan->system_hz = 96000000U;
        plan->prescaler = 0U;
        break;
    case 25000000U:
        plan->system_hz = 100000000U;
        plan->prescaler = 0U;
        break;
    default:
        return false;
    }
    plan->pclk1_hz = plan->system_hz / 2U;
    return true;
}

bool spi_command_parse(const char *text, size_t size, spi_command_t *command)
{
    static const char *const names[6] = {"bus", "mode", "hz", "lsb", "cs_high", "format"};
    uint32_t values[5] = {0U};
    uint32_t seen = 0U;
    bool ascii = false;
    size_t offset = 0U;
    memset(command, 0, sizeof(*command));
    while ((offset < size) && (text[offset] != '\n'))
    {
        size_t start;
        size_t equal;
        size_t stop;
        size_t key;
        while ((offset < size) && (text[offset] == ' '))
        {
            ++offset;
        }
        start = offset;
        while ((offset < size) && (text[offset] != '=') && (text[offset] != ' ') &&
               (text[offset] != '\n'))
        {
            ++offset;
        }
        if ((offset == size) || (text[offset] != '='))
        {
            return false;
        }
        equal = offset++;
        while ((offset < size) && (text[offset] != ' ') && (text[offset] != '\n'))
        {
            ++offset;
        }
        stop = offset;
        for (key = 0U; key < 6U; ++key)
        {
            if ((strlen(names[key]) == equal - start) &&
                (memcmp(text + start, names[key], equal - start) == 0))
            {
                break;
            }
        }
        if ((key == 6U) || ((seen & (1UL << key)) != 0U))
        {
            return false;
        }
        seen |= 1UL << key;
        if (key == 5U)
        {
            const size_t length = stop - equal - 1U;
            ascii = (length == 5U) && (memcmp(text + equal + 1U, "ascii", 5U) == 0);
            if (!ascii && !((length == 3U) && (memcmp(text + equal + 1U, "hex", 3U) == 0)))
            {
                return false;
            }
        }
        else if (!decimal(text + equal + 1U, stop - equal - 1U, &values[key]))
        {
            return false;
        }
    }
    if ((seen != 63U) || (offset >= size) || (values[0] != 2U) || (values[1] > 3U) ||
        (values[3] > 1U) || (values[4] > 1U))
    {
        return false;
    }
    command->mode = values[1];
    command->hz = values[2];
    command->lsb_first = values[3];
    command->cs_high = values[4];
    {
        spi_clock_plan_t plan;
        if (!spi_clock_plan(command->hz, &plan))
        {
            return false;
        }
    }
    command->ascii = ascii ? 1U : 0U;
    return console_bytes_parse(text + offset + 1U, size - offset - 1U, ascii, command->tx,
                               &command->size, false);
}

bool console_bytes_parse(const char *text, size_t size, bool ascii, uint8_t bytes[256],
                         size_t *count, bool allow_empty)
{
    size_t offset = 0U;
    *count = 0U;
    while (offset < size)
    {
        uint8_t byte;
        if (ascii)
        {
            byte = (uint8_t)text[offset++];
            if ((byte == 0U) || (byte > 127U))
            {
                return false;
            }
        }
        else
        {
            int high;
            int low;
            while ((offset < size) && ((text[offset] == ' ') || (text[offset] == '\t') ||
                                       (text[offset] == '\r') || (text[offset] == '\n')))
            {
                ++offset;
            }
            if (offset == size)
            {
                break;
            }
            if ((size - offset < 2U) || ((high = hex_digit(text[offset])) < 0) ||
                ((low = hex_digit(text[offset + 1U])) < 0))
            {
                return false;
            }
            byte = (uint8_t)(high * 16 + low);
            offset += 2U;
        }
        if (*count == SPI_CONSOLE_MAX_BYTES)
        {
            return false;
        }
        bytes[(*count)++] = byte;
    }
    return allow_empty || *count != 0U;
}

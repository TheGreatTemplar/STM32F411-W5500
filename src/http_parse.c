#include "config.h"
#include "http.h"
#include <string.h>

static bool header_equals(const char *text, size_t size, const char *expected)
{
    size_t i;
    if (strlen(expected) != size)
    {
        return false;
    }
    for (i = 0U; i < size; ++i)
    {
        char c = text[i];
        if ((c >= 'A') && (c <= 'Z'))
        {
            c = (char)(c + ('a' - 'A'));
        }
        if (c != expected[i])
        {
            return false;
        }
    }
    return true;
}

static bool command_headers_valid(const char *request, size_t start, size_t end, size_t *body_size)
{
    bool control = false;
    bool length_seen = false;
    *body_size = 0U;
    while (start + 2U < end)
    {
        size_t stop = start;
        const char *colon;
        size_t value;
        size_t value_end;
        while ((stop + 1U < end) && !((request[stop] == '\r') && (request[stop + 1U] == '\n')))
        {
            ++stop;
        }
        colon = memchr(request + start, ':', stop - start);
        if ((colon == NULL) || (colon == request + start))
        {
            return false;
        }
        value = (size_t)(colon - request) + 1U;
        value_end = stop;
        while ((value < value_end) && ((request[value] == ' ') || (request[value] == '\t')))
        {
            ++value;
        }
        while ((value_end > value) &&
               ((request[value_end - 1U] == ' ') || (request[value_end - 1U] == '\t')))
        {
            --value_end;
        }
        if (header_equals(request + start, (size_t)(colon - (request + start)), "x-device-control"))
        {
            if (control || (value_end - value != 1U) || (request[value] != '1'))
            {
                return false;
            }
            control = true;
        }
        if (header_equals(request + start, (size_t)(colon - (request + start)),
                          "transfer-encoding"))
        {
            return false;
        }
        if (header_equals(request + start, (size_t)(colon - (request + start)), "content-length"))
        {
            if (length_seen || (value == value_end))
            {
                return false;
            }
            length_seen = true;
            while (value < value_end)
            {
                size_t digit;
                if ((request[value] < '0') || (request[value] > '9'))
                {
                    return false;
                }
                digit = (size_t)(request[value++] - '0');
                if (*body_size > (APP_REQUEST_CAPACITY - digit) / 10U)
                {
                    return false;
                }
                *body_size = *body_size * 10U + digit;
            }
        }
        start = stop + 2U;
    }
    return control;
}

http_route_t http_parse(const char *request, size_t size)
{
    size_t i;
    size_t end = 0U;
    size_t line_end = 0U;
    const char *path;
    const char *version;
    size_t path_size;
    size_t method_size;
    bool post;
    for (i = 0U; i < size; ++i)
    {
        const unsigned char c = (unsigned char)request[i];
        if (((c < 32U) && (c != '\r') && (c != '\n') && (c != '\t')) || (c == 127U))
        {
            return HTTP_BAD_REQUEST;
        }
        if ((i >= 3U) && (memcmp(request + i - 3U, "\r\n\r\n", 4U) == 0))
        {
            end = i + 1U;
            break;
        }
    }
    if (end == 0U)
    {
        return HTTP_INCOMPLETE;
    }
    for (i = 0U; i + 1U < end; ++i)
    {
        if ((request[i] == '\r') && (request[i + 1U] == '\n'))
        {
            line_end = i;
            break;
        }
    }
    if (line_end < 14U)
    {
        return HTTP_BAD_REQUEST;
    }
    post = memcmp(request, "POST ", 5U) == 0;
    if (!post && (memcmp(request, "GET ", 4U) != 0))
    {
        return HTTP_METHOD_NOT_ALLOWED;
    }
    method_size = post ? 5U : 4U;
    path = request + method_size;
    version = memchr(path, ' ', line_end - method_size);
    if ((version == NULL) || ((size_t)(request + line_end - version) != 9U) ||
        ((memcmp(version, " HTTP/1.1", 9U) != 0) && (memcmp(version, " HTTP/1.0", 9U) != 0)))
    {
        return HTTP_BAD_REQUEST;
    }
    path_size = (size_t)(version - path);
    if (!post && path_size >= 16U && memcmp(path, "/api/usart?from=", 16U) == 0)
    {
        size_t p;
        uint32_t cursor = 0U;
        if (path_size == 16U)
        {
            return HTTP_BAD_REQUEST;
        }
        for (p = 16U; p < path_size; ++p)
        {
            const uint32_t digit = (uint32_t)(path[p] - '0');
            if (digit > 9U || cursor > (UINT32_MAX - digit) / 10U)
            {
                return HTTP_BAD_REQUEST;
            }
            cursor = cursor * 10U + digit;
        }
        return HTTP_USART;
    }
    if (!post && path_size == 13U && memcmp(path, "/api/settings", 13U) == 0)
    {
        return HTTP_SETTINGS;
    }
    if (!post && path_size == 12U && memcmp(path, "/api/library", 12U) == 0)
    {
        return HTTP_LIBRARY;
    }
    if (!post && (path_size == 8U) && (memcmp(path, "/api/rtc", 8U) == 0))
    {
        return HTTP_RTC;
    }
    if (!post && (path_size == 13U) && (memcmp(path, "/api/commands", 13U) == 0))
    {
        return HTTP_COMMANDS;
    }
    if ((path_size == 10U && memcmp(path, "/api/usart", 10U) == 0) ||
        (path_size == 19U && memcmp(path, "/api/settings/apply", 19U) == 0) ||
        (path_size == 13U && memcmp(path, "/api/settings", 13U) == 0) ||
        ((path_size == 11U) && (memcmp(path, "/api/led/on", 11U) == 0)) ||
        ((path_size == 12U) && (memcmp(path, "/api/led/off", 12U) == 0)) ||
        ((path_size == 8U) && (memcmp(path, "/api/spi", 8U) == 0)) ||
        ((path_size == 15U) && (memcmp(path, "/api/spi/config", 15U) == 0)) ||
        ((path_size == 8U) && (memcmp(path, "/api/i2c", 8U) == 0)) ||
        ((path_size == 8U) && (memcmp(path, "/api/rtc", 8U) == 0)) ||
        ((path_size == 12U) && (memcmp(path, "/api/library", 12U) == 0)) ||
        ((path_size == 13U) && (memcmp(path, "/api/commands", 13U) == 0)))
    {
        size_t body_size;
        const bool payload = (path_size == 10U) || (path_size == 19U) || (path_size == 15U) ||
                             (path_size == 8U) || (path_size == 13U) ||
                             (path_size == 12U && memcmp(path, "/api/library", 12U) == 0);
        if (!post)
        {
            return HTTP_POST_REQUIRED;
        }
        if (!command_headers_valid(request, line_end + 2U, end, &body_size) ||
            (end > APP_REQUEST_CAPACITY) || (body_size > APP_REQUEST_CAPACITY - end) ||
            (!payload && (body_size != 0U)) || (payload && (body_size == 0U)))
        {
            return HTTP_BAD_REQUEST;
        }
        if (size < end + body_size)
        {
            return HTTP_INCOMPLETE;
        }
        if (size != end + body_size)
        {
            return HTTP_BAD_REQUEST;
        }
        if (payload)
        {
            if (path_size == 10U)
            {
                return HTTP_USART_CONTROL;
            }
            if (path_size == 19U)
            {
                return HTTP_APPLY_SETTINGS;
            }
            if (path_size == 13U && memcmp(path, "/api/settings", 13U) == 0)
            {
                return HTTP_SAVE_SETTINGS;
            }
            if (path_size == 15U)
            {
                return HTTP_SPI_CONFIG;
            }
            if (path_size == 12U)
            {
                return HTTP_LIBRARY_COMMAND;
            }
            if ((path_size == 8U) && (memcmp(path, "/api/rtc", 8U) == 0))
            {
                return HTTP_SET_RTC;
            }
            if (path_size == 8U && memcmp(path, "/api/i2c", 8U) == 0)
            {
                return HTTP_I2C;
            }
            return path_size == 8U ? HTTP_SPI : HTTP_SAVE_COMMANDS;
        }
        return path_size == 11U ? HTTP_LED_ON : HTTP_LED_OFF;
    }
    if (post)
    {
        return HTTP_METHOD_NOT_ALLOWED;
    }
    if ((path_size == 1U) && (path[0] == '/'))
    {
        return HTTP_PAGE;
    }
    if ((path_size == 8U) && (memcmp(path, "/api/adc", 8U) == 0))
    {
        return HTTP_ADC;
    }
    if ((path_size == 8U) && (memcmp(path, "/api/mcu", 8U) == 0))
    {
        return HTTP_MCU;
    }
    if ((path_size == 16U) && (memcmp(path, "/api/diagnostics", 16U) == 0))
    {
        return HTTP_DIAGNOSTICS;
    }
    return HTTP_NOT_FOUND;
}

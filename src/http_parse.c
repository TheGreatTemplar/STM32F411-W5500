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

static bool led_headers_valid(const char *request, size_t start, size_t end)
{
    bool control = false;
    bool length_seen = false;
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
                if (request[value++] != '0')
                {
                    return false;
                }
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
    if (((path_size == 11U) && (memcmp(path, "/api/led/on", 11U) == 0)) ||
        ((path_size == 12U) && (memcmp(path, "/api/led/off", 12U) == 0)))
    {
        if (!post)
        {
            return HTTP_POST_REQUIRED;
        }
        if ((size != end) || !led_headers_valid(request, line_end + 2U, end))
        {
            return HTTP_BAD_REQUEST;
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
    return HTTP_NOT_FOUND;
}

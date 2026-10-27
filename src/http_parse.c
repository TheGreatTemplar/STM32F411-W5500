#include "http.h"
#include <string.h>

http_route_t http_parse(const char *request, size_t size)
{
    size_t i;
    size_t end = 0U;
    size_t line_end = 0U;
    const char *path;
    const char *version;
    size_t path_size;
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
    if (memcmp(request, "GET ", 4U) != 0)
    {
        return HTTP_METHOD_NOT_ALLOWED;
    }
    path = request + 4U;
    version = memchr(path, ' ', line_end - 4U);
    if ((version == NULL) || ((size_t)(request + line_end - version) != 9U) ||
        ((memcmp(version, " HTTP/1.1", 9U) != 0) && (memcmp(version, " HTTP/1.0", 9U) != 0)))
    {
        return HTTP_BAD_REQUEST;
    }
    path_size = (size_t)(version - path);
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

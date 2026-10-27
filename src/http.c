#include "http.h"
#include "board.h"
#include "config.h"
#include "w5500.h"
#include "web_page.h"
#include <stdio.h>
#include <string.h>

static char request[APP_REQUEST_CAPACITY];
static size_t used;
static uint32_t connection_start;
static bool active;
static bool closing;

void http_reset(void)
{
    used = 0U;
    active = false;
    closing = false;
}

static bool response(unsigned int status, const char *reason, const char *type, const char *body,
                     size_t size)
{
    char header[384];
    const int count =
        snprintf(header, sizeof(header),
                 "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                 "Connection: close\r\nCache-Control: no-store\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
                 "style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n"
                 "%s\r\n",
                 status, reason, type, (unsigned long)size, status == 405U ? "Allow: GET\r\n" : "");
    size_t offset = 0U;
    if ((count <= 0) || ((size_t)count >= sizeof(header)) || !w5500_send(header, (size_t)count))
    {
        return false;
    }
    while (offset < size)
    {
        const size_t remaining = size - offset;
        const size_t chunk = remaining > 1024U ? 1024U : remaining;
        if (!w5500_send(body + offset, chunk))
        {
            return false;
        }
        offset += chunk;
    }
    return true;
}

static bool adc_response(const adc_snapshot_t *adc)
{
    char json[384];
    const uint32_t mv0 = ((uint32_t)adc->raw[0] * APP_ADC_VREF_MV + 2047U) / 4095U;
    const uint32_t mv1 = ((uint32_t)adc->raw[1] * APP_ADC_VREF_MV + 2047U) / 4095U;
    const int count =
        snprintf(json, sizeof(json),
                 "{\"raw\":[%u,%u],\"mv\":[%lu,%lu],\"vref_mv\":%u,"
                 "\"adc_ok\":%s,\"link\":%s,\"sample_ms\":%lu,"
                 "\"sample_count\":%lu,\"uptime_ms\":%lu}",
                 (unsigned int)adc->raw[0], (unsigned int)adc->raw[1], (unsigned long)mv0,
                 (unsigned long)mv1, APP_ADC_VREF_MV, adc->valid ? "true" : "false",
                 w5500_link() ? "true" : "false", (unsigned long)adc->sample_ms,
                 (unsigned long)adc->sample_count, (unsigned long)board_millis());
    return (count > 0) && ((size_t)count < sizeof(json)) &&
           response(200U, "OK", "application/json", json, (size_t)count);
}

static bool dispatch(http_route_t route, const adc_snapshot_t *adc)
{
    switch (route)
    {
    case HTTP_PAGE:
        return response(200U, "OK", "text/html; charset=utf-8", web_page, sizeof(web_page) - 1U);
    case HTTP_ADC:
        return adc_response(adc);
    case HTTP_NOT_FOUND:
        return response(404U, "Not Found", "text/plain", "Not found\n", 10U);
    case HTTP_METHOD_NOT_ALLOWED:
        return response(405U, "Method Not Allowed", "text/plain", "GET only\n", 9U);
    case HTTP_INCOMPLETE:
    case HTTP_BAD_REQUEST:
        return response(400U, "Bad Request", "text/plain", "Bad request\n", 12U);
    }
    return false;
}

void http_poll(const adc_snapshot_t *adc)
{
    const uint8_t state = w5500_state();
    const uint32_t now = board_millis();
    if (state == W5500_CLOSED)
    {
        http_reset();
        (void)w5500_listen();
        return;
    }
    if (state == W5500_LISTEN)
    {
        http_reset();
        return;
    }
    if (!active)
    {
        connection_start = now;
        active = true;
    }
    if ((uint32_t)(now - connection_start) >= APP_REQUEST_TIMEOUT_MS)
    {
        w5500_close();
        http_reset();
        return;
    }
    if (closing || ((state != W5500_ESTABLISHED) && (state != W5500_CLOSE_WAIT)))
    {
        return;
    }
    {
        const int32_t count = w5500_receive((uint8_t *)request + used, sizeof(request) - used);
        http_route_t route;
        if (count < 0)
        {
            w5500_close();
            http_reset();
            return;
        }
        used += (size_t)count;
        route = http_parse(request, used);
        if (route != HTTP_INCOMPLETE)
        {
            if (dispatch(route, adc))
            {
                w5500_disconnect();
                closing = true;
                connection_start = board_millis();
            }
            else
            {
                w5500_close();
                http_reset();
            }
        }
        else if ((used == sizeof(request)) || ((state == W5500_CLOSE_WAIT) && (count == 0)))
        {
            if (used == sizeof(request))
            {
                (void)response(431U, "Request Header Fields Too Large", "text/plain",
                               "Header too large\n", 17U);
            }
            w5500_close();
            http_reset();
        }
    }
}

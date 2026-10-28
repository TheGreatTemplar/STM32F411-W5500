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
                     size_t size, const char *extra_headers)
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
                 status, reason, type, (unsigned long)size, extra_headers);
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
    const uint32_t link_speed_mbps = w5500_link_speed_mbps();
    const uint32_t mv0 = ((uint32_t)adc->raw[0] * APP_ADC_VREF_MV + 2047U) / 4095U;
    const uint32_t mv1 = ((uint32_t)adc->raw[1] * APP_ADC_VREF_MV + 2047U) / 4095U;
    const int count =
        snprintf(json, sizeof(json),
                 "{\"raw\":[%u,%u],\"mv\":[%lu,%lu],\"vref_mv\":%u,"
                 "\"adc_ok\":%s,\"link\":%s,\"link_speed_mbps\":%lu,\"sample_ms\":%lu,"
                 "\"sample_count\":%lu,\"uptime_ms\":%lu,\"led_on\":%s}",
                 (unsigned int)adc->raw[0], (unsigned int)adc->raw[1], (unsigned long)mv0,
                 (unsigned long)mv1, APP_ADC_VREF_MV, adc->valid ? "true" : "false",
                 link_speed_mbps != 0U ? "true" : "false", (unsigned long)link_speed_mbps,
                 (unsigned long)adc->sample_ms, (unsigned long)adc->sample_count,
                 (unsigned long)board_millis(), board_led_is_on() ? "true" : "false");
    return (count > 0) && ((size_t)count < sizeof(json)) &&
           response(200U, "OK", "application/json", json, (size_t)count, "");
}

static bool led_response(bool on)
{
    const char *json;
    board_led(on);
    json = board_led_is_on() ? "{\"led_on\":true}" : "{\"led_on\":false}";
    return response(200U, "OK", "application/json", json, strlen(json), "");
}

static void format_ip(char text[16], const uint8_t ip[4])
{
    (void)snprintf(text, 16U, "%u.%u.%u.%u", (unsigned int)ip[0], (unsigned int)ip[1],
                   (unsigned int)ip[2], (unsigned int)ip[3]);
}

static bool diagnostics_response(void)
{
    static char json[3072];
    w5500_diag_t diag;
    char ip[16];
    char mask[16];
    char gateway[16];
    size_t json_used;
    uint32_t socket;
    int count;
    if (!w5500_diagnostics(&diag))
    {
        return response(503U, "Service Unavailable", "text/plain", "Diagnostics unavailable\n", 24U,
                        "");
    }
    format_ip(ip, diag.ip);
    format_ip(mask, diag.mask);
    format_ip(gateway, diag.gateway);
    count = snprintf(
        json, sizeof(json),
        "{\"uptime_ms\":%lu,\"version\":%lu,\"phy\":%lu,\"mode\":%lu,\"interrupts\":%lu,"
        "\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"retry_ms\":%lu,\"retry_count\":%lu,"
        "\"init_attempts\":%lu,\"spi_errors\":%lu,\"tx_timeouts\":%lu,"
        "\"rx_bytes\":%lu,\"tx_bytes\":%lu,\"http_sockets\":1,\"http_port\":%u,\"sockets\":[",
        (unsigned long)board_millis(), (unsigned long)diag.version, (unsigned long)diag.phy,
        (unsigned long)diag.mode, (unsigned long)diag.interrupts, ip, mask, gateway,
        (unsigned int)diag.mac[0], (unsigned int)diag.mac[1], (unsigned int)diag.mac[2],
        (unsigned int)diag.mac[3], (unsigned int)diag.mac[4], (unsigned int)diag.mac[5],
        (unsigned long)(diag.retry_100us / 10U), (unsigned long)diag.retry_count,
        (unsigned long)diag.init_attempts, (unsigned long)diag.spi_errors,
        (unsigned long)diag.tx_timeouts, (unsigned long)diag.rx_bytes, (unsigned long)diag.tx_bytes,
        APP_HTTP_PORT);
    if ((count <= 0) || ((size_t)count >= sizeof(json)))
    {
        return false;
    }
    json_used = (size_t)count;
    for (socket = 0U; socket < 8U; ++socket)
    {
        const w5500_socket_diag_t *entry = &diag.sockets[socket];
        format_ip(ip, entry->remote_ip);
        count = snprintf(
            json + json_used, sizeof(json) - json_used,
            "%s{\"id\":%lu,\"state\":%lu,\"irq\":%lu,\"port\":%lu,\"peer\":\"%s\","
            "\"peer_port\":%lu,\"rx_kb\":%lu,\"tx_kb\":%lu,\"rx_waiting\":%lu,\"tx_free\":%lu}",
            socket == 0U ? "" : ",", (unsigned long)socket, (unsigned long)entry->state,
            (unsigned long)entry->interrupts, (unsigned long)entry->local_port, ip,
            (unsigned long)entry->remote_port, (unsigned long)entry->rx_kb,
            (unsigned long)entry->tx_kb, (unsigned long)entry->rx_waiting,
            (unsigned long)entry->tx_free);
        if ((count <= 0) || ((size_t)count >= sizeof(json) - json_used))
        {
            return false;
        }
        json_used += (size_t)count;
    }
    if (sizeof(json) - json_used < 3U)
    {
        return false;
    }
    json[json_used++] = ']';
    json[json_used++] = '}';
    return response(200U, "OK", "application/json", json, json_used, "");
}

static bool dispatch(http_route_t route, const adc_snapshot_t *adc)
{
    switch (route)
    {
    case HTTP_PAGE:
        return response(200U, "OK", "text/html; charset=utf-8", web_page, sizeof(web_page) - 1U,
                        "");
    case HTTP_ADC:
        return adc_response(adc);
    case HTTP_DIAGNOSTICS:
        return diagnostics_response();
    case HTTP_LED_ON:
        return led_response(true);
    case HTTP_LED_OFF:
        return led_response(false);
    case HTTP_NOT_FOUND:
        return response(404U, "Not Found", "text/plain", "Not found\n", 10U, "");
    case HTTP_METHOD_NOT_ALLOWED:
        return response(405U, "Method Not Allowed", "text/plain", "GET only\n", 9U,
                        "Allow: GET\r\n");
    case HTTP_POST_REQUIRED:
        return response(405U, "Method Not Allowed", "text/plain", "POST only\n", 10U,
                        "Allow: POST\r\n");
    case HTTP_INCOMPLETE:
    case HTTP_BAD_REQUEST:
        return response(400U, "Bad Request", "text/plain", "Bad request\n", 12U, "");
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
                               "Header too large\n", 17U, "");
            }
            w5500_close();
            http_reset();
        }
    }
}

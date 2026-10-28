#include "http.h"
#include "board.h"
#include "command_store.h"
#include "config.h"
#include "rtc.h"
#include "spi_console.h"
#include "w5500.h"
#include "web_page.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    char request[APP_REQUEST_CAPACITY];
    char header[384];
    char body[APP_RESPONSE_CAPACITY];
    const char *reply_body;
    size_t used;
    size_t header_size;
    size_t reply_size;
    size_t body_offset;
    size_t chunk_size;
    uint32_t accepted_at;
    uint32_t last_progress;
    uint32_t stage;
    bool active;
    bool closing;
    bool queued;
    bool waiting_tx;
} http_client_t;

static http_client_t clients[APP_HTTP_SOCKETS];
static uint8_t next_socket;

static void reset_client(http_client_t *client)
{
    client->used = 0U;
    client->active = false;
    client->closing = false;
    client->queued = false;
    client->waiting_tx = false;
    client->stage = 0U;
    client->body_offset = 0U;
}

void http_reset(void)
{
    uint32_t socket;
    for (socket = 0U; socket < APP_HTTP_SOCKETS; ++socket)
    {
        reset_client(&clients[socket]);
    }
    next_socket = 0U;
}

static bool response(http_client_t *client, unsigned int status, const char *reason,
                     const char *type, const char *body, size_t size, const char *extra_headers)
{
    const int count =
        snprintf(client->header, sizeof(client->header),
                 "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                 "Connection: close\r\nCache-Control: no-store\r\n"
                 "X-Content-Type-Options: nosniff\r\n"
                 "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
                 "style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n"
                 "%s\r\n",
                 status, reason, type, (unsigned long)size, extra_headers);
    if ((count <= 0) || ((size_t)count >= sizeof(client->header)))
    {
        return false;
    }
    if (body == web_page)
    {
        client->reply_body = web_page;
    }
    else
    {
        if (size > sizeof(client->body))
        {
            return false;
        }
        memmove(client->body, body, size);
        client->reply_body = client->body;
    }
    client->header_size = (size_t)count;
    client->reply_size = size;
    client->body_offset = 0U;
    client->stage = 0U;
    client->queued = true;
    client->waiting_tx = false;
    client->last_progress = board_millis();
    return true;
}

static bool adc_response(http_client_t *client, const adc_snapshot_t *adc)
{
    char json[640];
    char clock[160];
    const uint32_t link_speed_mbps = w5500_link_speed_mbps();
    const uint32_t mv0 = ((uint32_t)adc->raw[0] * APP_ADC_VREF_MV + 2047U) / 4095U;
    const uint32_t mv1 = ((uint32_t)adc->raw[1] * APP_ADC_VREF_MV + 2047U) / 4095U;
    int count;
    if (rtc_json(clock, sizeof(clock)) == 0U)
    {
        return false;
    }
    count = snprintf(json, sizeof(json),
                     "{\"raw\":[%u,%u],\"mv\":[%lu,%lu],\"vref_mv\":%u,"
                     "\"adc_ok\":%s,\"link\":%s,\"link_speed_mbps\":%lu,\"sample_ms\":%lu,"
                     "\"sample_count\":%lu,\"uptime_ms\":%lu,\"led_on\":%s,\"rtc\":%s}",
                     (unsigned int)adc->raw[0], (unsigned int)adc->raw[1], (unsigned long)mv0,
                     (unsigned long)mv1, APP_ADC_VREF_MV, adc->valid ? "true" : "false",
                     link_speed_mbps != 0U ? "true" : "false", (unsigned long)link_speed_mbps,
                     (unsigned long)adc->sample_ms, (unsigned long)adc->sample_count,
                     (unsigned long)board_millis(), board_led_is_on() ? "true" : "false", clock);
    return (count > 0) && ((size_t)count < sizeof(json)) &&
           response(client, 200U, "OK", "application/json", json, (size_t)count, "");
}

static bool led_response(http_client_t *client, bool on)
{
    const char *json;
    board_led(on);
    json = board_led_is_on() ? "{\"led_on\":true}" : "{\"led_on\":false}";
    return response(client, 200U, "OK", "application/json", json, strlen(json), "");
}

static void format_ip(char text[16], const uint8_t ip[4])
{
    (void)snprintf(text, 16U, "%u.%u.%u.%u", (unsigned int)ip[0], (unsigned int)ip[1],
                   (unsigned int)ip[2], (unsigned int)ip[3]);
}

static bool diagnostics_response(http_client_t *client)
{
    char *json = client->body;
    w5500_diag_t diag;
    char ip[16];
    char mask[16];
    char gateway[16];
    size_t json_used;
    uint32_t socket;
    int count;
    if (!w5500_diagnostics(&diag))
    {
        return response(client, 503U, "Service Unavailable", "text/plain",
                        "Diagnostics unavailable\n", 24U, "");
    }
    format_ip(ip, diag.ip);
    format_ip(mask, diag.mask);
    format_ip(gateway, diag.gateway);
    count = snprintf(
        json, sizeof(client->body),
        "{\"uptime_ms\":%lu,\"version\":%lu,\"phy\":%lu,\"mode\":%lu,\"interrupts\":%lu,"
        "\"ip\":\"%s\",\"mask\":\"%s\",\"gateway\":\"%s\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"retry_ms\":%lu,\"retry_count\":%lu,"
        "\"init_attempts\":%lu,\"spi_errors\":%lu,\"tx_timeouts\":%lu,"
        "\"rx_bytes\":%lu,\"tx_bytes\":%lu,\"http_sockets\":%u,\"http_port\":%u,\"sockets\":[",
        (unsigned long)board_millis(), (unsigned long)diag.version, (unsigned long)diag.phy,
        (unsigned long)diag.mode, (unsigned long)diag.interrupts, ip, mask, gateway,
        (unsigned int)diag.mac[0], (unsigned int)diag.mac[1], (unsigned int)diag.mac[2],
        (unsigned int)diag.mac[3], (unsigned int)diag.mac[4], (unsigned int)diag.mac[5],
        (unsigned long)(diag.retry_100us / 10U), (unsigned long)diag.retry_count,
        (unsigned long)diag.init_attempts, (unsigned long)diag.spi_errors,
        (unsigned long)diag.tx_timeouts, (unsigned long)diag.rx_bytes, (unsigned long)diag.tx_bytes,
        APP_HTTP_SOCKETS, APP_HTTP_PORT);
    if ((count <= 0) || ((size_t)count >= sizeof(client->body)))
    {
        return false;
    }
    json_used = (size_t)count;
    for (socket = 0U; socket < 8U; ++socket)
    {
        const w5500_socket_diag_t *entry = &diag.sockets[socket];
        format_ip(ip, entry->remote_ip);
        count = snprintf(
            json + json_used, sizeof(client->body) - json_used,
            "%s{\"id\":%lu,\"state\":%lu,\"irq\":%lu,\"port\":%lu,\"peer\":\"%s\","
            "\"peer_port\":%lu,\"rx_kb\":%lu,\"tx_kb\":%lu,\"rx_waiting\":%lu,\"tx_free\":%lu}",
            socket == 0U ? "" : ",", (unsigned long)socket, (unsigned long)entry->state,
            (unsigned long)entry->interrupts, (unsigned long)entry->local_port, ip,
            (unsigned long)entry->remote_port, (unsigned long)entry->rx_kb,
            (unsigned long)entry->tx_kb, (unsigned long)entry->rx_waiting,
            (unsigned long)entry->tx_free);
        if ((count <= 0) || ((size_t)count >= sizeof(client->body) - json_used))
        {
            return false;
        }
        json_used += (size_t)count;
    }
    if (sizeof(client->body) - json_used < 3U)
    {
        return false;
    }
    json[json_used++] = ']';
    json[json_used++] = '}';
    return response(client, 200U, "OK", "application/json", json, json_used, "");
}

static bool spi_response(http_client_t *client)
{
    static const char digits[] = "0123456789ABCDEF";
    spi_command_t command;
    uint8_t rx[SPI_CONSOLE_MAX_BYTES];
    char tx_hex[SPI_CONSOLE_MAX_BYTES * 2U + 1U];
    char rx_hex[SPI_CONSOLE_MAX_BYTES * 2U + 1U];
    size_t offset;
    size_t i;
    int count;
    for (offset = 0U; offset + 4U <= client->used; ++offset)
    {
        if (memcmp(client->request + offset, "\r\n\r\n", 4U) == 0)
        {
            break;
        }
    }
    offset += 4U;
    if ((offset > client->used) ||
        !spi_command_parse(client->request + offset, client->used - offset, &command))
    {
        return response(client, 400U, "Bad Request", "text/plain", "Invalid SPI command\n", 20U,
                        "");
    }
    if (!board_spi_console(&command, rx))
    {
        return response(client, 504U, "Gateway Timeout", "text/plain",
                        "SPI timeout; partial transfer possible\n", 39U, "");
    }
    for (i = 0U; i < command.size; ++i)
    {
        tx_hex[i * 2U] = digits[command.tx[i] >> 4U];
        tx_hex[i * 2U + 1U] = digits[command.tx[i] & 15U];
        rx_hex[i * 2U] = digits[rx[i] >> 4U];
        rx_hex[i * 2U + 1U] = digits[rx[i] & 15U];
    }
    tx_hex[command.size * 2U] = '\0';
    rx_hex[command.size * 2U] = '\0';
    count = snprintf(client->body, sizeof(client->body),
                     "{\"bus\":2,\"mode\":%lu,\"hz\":%lu,\"lsb\":%lu,\"cs_high\":%lu,"
                     "\"count\":%lu,\"tx\":\"%s\",\"rx\":\"%s\",\"transfer_us\":%lu}",
                     (unsigned long)command.mode, (unsigned long)command.hz,
                     (unsigned long)command.lsb_first, (unsigned long)command.cs_high,
                     (unsigned long)command.size, tx_hex, rx_hex,
                     (unsigned long)board_spi_console_duration_us());
    return (count > 0) && ((size_t)count < sizeof(client->body)) &&
           response(client, 200U, "OK", "application/json", client->body, (size_t)count, "");
}

static bool commands_response(http_client_t *client, bool save)
{
    if (save)
    {
        size_t offset;
        store_error_t error;
        for (offset = 0U; offset + 4U <= client->used; ++offset)
        {
            if (memcmp(client->request + offset, "\r\n\r\n", 4U) == 0)
            {
                break;
            }
        }
        offset += 4U;
        error = offset <= client->used
                    ? command_store_save(client->request + offset, client->used - offset)
                    : STORE_INVALID;
        if (error != STORE_OK)
        {
            char text[48];
            const int count =
                snprintf(text, sizeof(text), "Command store error: %u\n", (unsigned int)error);
            return (count > 0) && ((size_t)count < sizeof(text)) &&
                   response(client, error == STORE_INVALID ? 400U : 409U, "Command Store Error",
                            "text/plain", text, (size_t)count, "");
        }
    }
    {
        const size_t size = command_store_json(client->body, sizeof(client->body));
        return size != 0U && response(client, save ? 202U : 200U, save ? "Accepted" : "OK",
                                      "application/json", client->body, size, "");
    }
}

static bool rtc_response(http_client_t *client, bool set)
{
    if (set)
    {
        size_t offset;
        rtc_date_t date;
        for (offset = 0U; offset + 4U <= client->used; ++offset)
        {
            if (memcmp(client->request + offset, "\r\n\r\n", 4U) == 0)
            {
                break;
            }
        }
        offset += 4U;
        if (offset > client->used ||
            !rtc_parse(client->request + offset, client->used - offset, &date))
        {
            return response(client, 400U, "Bad Request", "text/plain", "Invalid date/time\n", 18U,
                            "");
        }
        if (!rtc_set(&date))
        {
            return response(client, 503U, "Service Unavailable", "text/plain", "RTC unavailable\n",
                            16U, "");
        }
    }
    {
        const size_t size = rtc_json(client->body, sizeof(client->body));
        return size != 0U &&
               response(client, 200U, "OK", "application/json", client->body, size, "");
    }
}

static bool dispatch(http_client_t *client, http_route_t route, const adc_snapshot_t *adc)
{
    switch (route)
    {
    case HTTP_RTC:
        return rtc_response(client, false);
    case HTTP_SET_RTC:
        return rtc_response(client, true);
    case HTTP_PAGE:
        return response(client, 200U, "OK", "text/html; charset=utf-8", web_page,
                        sizeof(web_page) - 1U, "");
    case HTTP_ADC:
        return adc_response(client, adc);
    case HTTP_DIAGNOSTICS:
        return diagnostics_response(client);
    case HTTP_SPI:
        return spi_response(client);
    case HTTP_COMMANDS:
        return commands_response(client, false);
    case HTTP_SAVE_COMMANDS:
        return commands_response(client, true);
    case HTTP_LED_ON:
        return led_response(client, true);
    case HTTP_LED_OFF:
        return led_response(client, false);
    case HTTP_NOT_FOUND:
        return response(client, 404U, "Not Found", "text/plain", "Not found\n", 10U, "");
    case HTTP_METHOD_NOT_ALLOWED:
        return response(client, 405U, "Method Not Allowed", "text/plain", "GET only\n", 9U,
                        "Allow: GET\r\n");
    case HTTP_POST_REQUIRED:
        return response(client, 405U, "Method Not Allowed", "text/plain", "POST only\n", 10U,
                        "Allow: POST\r\n");
    case HTTP_INCOMPLETE:
    case HTTP_BAD_REQUEST:
        return response(client, 400U, "Bad Request", "text/plain", "Bad request\n", 12U, "");
    }
    return false;
}

static void transmit_step(uint8_t socket, http_client_t *client)
{
    int32_t result;
    const char *data;
    size_t size;
    if (client->waiting_tx)
    {
        result = w5500_send_poll(socket);
        if (result < 0)
        {
            w5500_close(socket);
            reset_client(client);
            return;
        }
        if (result == 0)
        {
            return;
        }
        client->waiting_tx = false;
        client->last_progress = board_millis();
        if (client->stage == 0U)
        {
            client->stage = 1U;
        }
        else
        {
            client->body_offset += client->chunk_size;
        }
    }
    if ((client->stage == 1U) && (client->body_offset == client->reply_size))
    {
        w5500_disconnect(socket);
        client->queued = false;
        client->closing = true;
        client->last_progress = board_millis();
        return;
    }
    if (client->stage == 0U)
    {
        data = client->header;
        size = client->header_size;
    }
    else
    {
        data = client->reply_body + client->body_offset;
        size = client->reply_size - client->body_offset;
        if (size > 1024U)
        {
            size = 1024U;
        }
    }
    result = w5500_send_start(socket, data, size);
    if (result < 0)
    {
        w5500_close(socket);
        reset_client(client);
    }
    else if (result > 0)
    {
        client->waiting_tx = true;
        client->chunk_size = size;
    }
}

static void poll_client(uint8_t socket, const adc_snapshot_t *adc)
{
    http_client_t *client = &clients[socket];
    const uint8_t state = w5500_state(socket);
    const uint32_t now = board_millis();
    if (state == W5500_CLOSED)
    {
        reset_client(client);
        (void)w5500_listen(socket);
        return;
    }
    if (state == W5500_LISTEN)
    {
        reset_client(client);
        return;
    }
    if (!client->active)
    {
        client->accepted_at = now;
        client->last_progress = now;
        client->active = true;
    }
    if ((uint32_t)(now - (client->queued || client->closing ? client->last_progress
                                                            : client->accepted_at)) >=
        APP_REQUEST_TIMEOUT_MS)
    {
        w5500_close(socket);
        reset_client(client);
        return;
    }
    if (client->queued)
    {
        transmit_step(socket, client);
        return;
    }
    if (client->closing || ((state != W5500_ESTABLISHED) && (state != W5500_CLOSE_WAIT)))
    {
        return;
    }
    {
        const int32_t count = w5500_receive(socket, (uint8_t *)client->request + client->used,
                                            sizeof(client->request) - client->used);
        http_route_t route;
        if (count < 0)
        {
            w5500_close(socket);
            reset_client(client);
            return;
        }
        client->used += (size_t)count;
        route = http_parse(client->request, client->used);
        if (route != HTTP_INCOMPLETE)
        {
            if (!dispatch(client, route, adc))
            {
                w5500_close(socket);
                reset_client(client);
            }
        }
        else if ((client->used == sizeof(client->request)) ||
                 ((state == W5500_CLOSE_WAIT) && (count == 0)))
        {
            if (client->used == sizeof(client->request))
            {
                if (response(client, 431U, "Request Header Fields Too Large", "text/plain",
                             "Request too large\n", 18U, ""))
                {
                    return;
                }
            }
            w5500_close(socket);
            reset_client(client);
        }
    }
}

void http_poll(const adc_snapshot_t *adc)
{
    uint8_t i;
    for (i = 0U; i < APP_HTTP_SOCKETS; ++i)
    {
        const uint8_t socket = (uint8_t)((next_socket + i) % APP_HTTP_SOCKETS);
        poll_client(socket, adc);
    }
    next_socket = (uint8_t)((next_socket + 1U) % APP_HTTP_SOCKETS);
}

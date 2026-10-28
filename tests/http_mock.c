#include "config.h"
#include "http.h"
#include "w5500.h"
#include <string.h>

static uint32_t now;
static uint8_t state;
static char input[2048];
static size_t input_size;
static size_t input_offset;
static size_t fragment_size;
static char output[24576];
static size_t output_size;
static bool fail_send;
static uint32_t link_speed_mbps;
static bool led_on;
static uint32_t led_writes;
static bool fail_diagnostics;

bool w5500_diagnostics(w5500_diag_t *diag)
{
    uint32_t i;
    memset(diag, 0, sizeof(*diag));
    diag->version = 4U;
    diag->phy = 7U;
    diag->init_attempts = 1U;
    diag->retry_100us = 2000U;
    diag->retry_count = 3U;
    diag->ip[0] = 172U;
    diag->ip[1] = 16U;
    diag->ip[2] = 236U;
    diag->ip[3] = 220U;
    for (i = 0U; i < 8U; ++i)
    {
        /* Exercise serializer bounds with the largest register values. */
        diag->sockets[i].state = 255U;
        diag->sockets[i].interrupts = 255U;
        diag->sockets[i].local_port = 65535U;
        diag->sockets[i].remote_port = 65535U;
        memset(diag->sockets[i].remote_ip, 255, 4U);
        diag->sockets[i].rx_waiting = 65535U;
        diag->sockets[i].tx_free = 65535U;
        diag->sockets[i].rx_kb = 16U;
        diag->sockets[i].tx_kb = 16U;
    }
    return !fail_diagnostics;
}

void board_led(bool on);
bool board_led_is_on(void);

void board_led(bool on)
{
    led_on = on;
    ++led_writes;
}

bool board_led_is_on(void)
{
    return led_on;
}

uint32_t board_millis(void);
uint32_t board_millis(void)
{
    return now;
}

bool w5500_link(void)
{
    return true;
}

uint32_t w5500_link_speed_mbps(void)
{
    return link_speed_mbps;
}

uint8_t w5500_state(void)
{
    return state;
}

bool w5500_listen(void)
{
    state = W5500_LISTEN;
    return true;
}

void w5500_close(void)
{
    state = W5500_CLOSED;
}

void w5500_disconnect(void)
{
    state = 0x18U;
}

int32_t w5500_receive(uint8_t *data, size_t capacity)
{
    size_t count = input_size - input_offset;
    if (count > capacity)
    {
        count = capacity;
    }
    if (count > fragment_size)
    {
        count = fragment_size;
    }
    memcpy(data, input + input_offset, count);
    input_offset += count;
    return (int32_t)count;
}

bool w5500_send(const void *data, size_t size)
{
    if (fail_send || (output_size + size >= sizeof(output)))
    {
        return false;
    }
    memcpy(output + output_size, data, size);
    output_size += size;
    output[output_size] = '\0';
    return true;
}

static void prepare(const char *text)
{
    input_size = strlen(text);
    memcpy(input, text, input_size);
    input_offset = 0U;
    output_size = 0U;
    output[0] = '\0';
    fragment_size = sizeof(input);
    now = 0U;
    fail_send = false;
    fail_diagnostics = false;
    link_speed_mbps = 100U;
    state = W5500_ESTABLISHED;
    http_reset();
}

static bool framing_ok(void)
{
    const char *length = strstr(output, "Content-Length: ");
    const char *body = strstr(output, "\r\n\r\n");
    size_t declared = 0U;
    if ((length == NULL) || (body == NULL))
    {
        return false;
    }
    length += 16U;
    while ((*length >= '0') && (*length <= '9'))
    {
        declared = declared * 10U + (size_t)(*length - '0');
        ++length;
    }
    return declared == output_size - (size_t)(body + 4U - output);
}

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)

int test_entry(void);
int test_entry(void)
{
    adc_snapshot_t adc = {{0U, 4095U}, 40U, 3U, 1U};
    uint32_t i;
    prepare("GET /api/adc HTTP/1.1\r\nHost: board\r\n\r\n");
    fragment_size = 1U;
    for (i = 0U; i < input_size; ++i)
    {
        http_poll(&adc);
    }
    CHECK(strstr(output, "HTTP/1.1 200 OK") != NULL);
    CHECK(strstr(output, "\"raw\":[0,4095]") != NULL);
    CHECK(strstr(output, "\"mv\":[0,3300]") != NULL);
    CHECK(strstr(output, "\"adc_ok\":true") != NULL);
    CHECK(strstr(output, "\"led_on\":false") != NULL);
    CHECK(strstr(output, "\"link\":true,\"link_speed_mbps\":100") != NULL);
    CHECK(framing_ok());
    CHECK(state == 0x18U);
    now = APP_REQUEST_TIMEOUT_MS;
    http_poll(&adc);
    CHECK(state == W5500_CLOSED);
    http_poll(&adc);
    CHECK(state == W5500_LISTEN);

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    link_speed_mbps = 10U;
    http_poll(&adc);
    CHECK(strstr(output, "\"link\":true,\"link_speed_mbps\":10") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    link_speed_mbps = 0U;
    http_poll(&adc);
    CHECK(strstr(output, "\"link\":false,\"link_speed_mbps\":0") != NULL);
    CHECK(framing_ok());

    prepare("GET / HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "<!doctype html>") != NULL);
    CHECK(strstr(output, "fetch('/api/adc'") != NULL);
    CHECK(framing_ok());

    prepare("POST / HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "405 Method Not Allowed") != NULL);
    CHECK(strstr(output, "Allow: GET\r\n") != NULL);
    CHECK(framing_ok());

    prepare("GET /missing HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "404 Not Found") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    adc.valid = 0U;
    http_poll(&adc);
    CHECK(strstr(output, "\"adc_ok\":false") != NULL);

    prepare("GET / HTTP/1.1\r\n");
    http_poll(&adc);
    now = APP_REQUEST_TIMEOUT_MS;
    http_poll(&adc);
    CHECK(state == W5500_CLOSED && output_size == 0U);

    prepare("GET / HTTP/1.1\r\n");
    state = W5500_CLOSE_WAIT;
    http_poll(&adc);
    http_poll(&adc);
    CHECK(state == W5500_CLOSED);

    prepare("GET / HTTP/1.1\r\n\r\n");
    fail_send = true;
    http_poll(&adc);
    CHECK(state == W5500_CLOSED);

    prepare("GET / HTTP/1.1\r\nX: ");
    memset(input + input_size, 'a', sizeof(input) - input_size);
    input_size = sizeof(input);
    http_poll(&adc);
    CHECK(strstr(output, "431 Request Header Fields Too Large") != NULL);
    CHECK(state == W5500_CLOSED);
    CHECK(framing_ok());
    prepare("POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 0\r\n\r\n");
    fragment_size = 1U;
    for (i = 0U; i < input_size; ++i)
    {
        http_poll(&adc);
        CHECK(led_writes == (i + 1U == input_size ? 1U : 0U));
    }
    CHECK(led_on && led_writes == 1U);
    CHECK(strstr(output, "{\"led_on\":true}") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "\"led_on\":true") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("GET /api/led/off HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "405 Method Not Allowed") != NULL);
    CHECK(strstr(output, "Allow: POST\r\n") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("POST /api/led/off HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "400 Bad Request") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("POST /api/led/off HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 1\r\n\r\nx");
    http_poll(&adc);
    CHECK(strstr(output, "400 Bad Request") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("POST /api/led/off HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 0\r\n\r\n");
    http_poll(&adc);
    CHECK(!led_on && led_writes == 2U);
    CHECK(strstr(output, "{\"led_on\":false}") != NULL);
    CHECK(framing_ok());
    http_reset();
    CHECK(!led_on && led_writes == 2U);
    prepare("GET /api/diagnostics HTTP/1.1\r\n\r\n");
    http_poll(&adc);
    CHECK(strstr(output, "200 OK") != NULL);
    CHECK(strstr(output, "\"version\":4") != NULL);
    CHECK(strstr(output, "\"ip\":\"172.16.236.220\"") != NULL);
    CHECK(strstr(output, "\"id\":7") != NULL);
    CHECK(strstr(output, "\"peer\":\"255.255.255.255\"") != NULL);
    CHECK(strstr(output, "\"retry_ms\":200,\"retry_count\":3") != NULL);
    CHECK(framing_ok());
    CHECK(led_writes == 2U);
    prepare("GET /api/diagnostics HTTP/1.1\r\n\r\n");
    fail_diagnostics = true;
    http_poll(&adc);
    CHECK(strstr(output, "503 Service Unavailable") != NULL);
    CHECK(framing_ok());
    return 0;
}

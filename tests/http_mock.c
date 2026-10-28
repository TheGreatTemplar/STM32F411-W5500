#include "command_store.h"
#include "config.h"
#include "http.h"
#include "rtc.h"
#include "spi_console.h"
#include "w5500.h"
#include <stdio.h>
#include <string.h>

static uint32_t now;
static uint8_t state[4];
static bool hold_tx[4];
static bool mock_sending[4];
static uint32_t spi_calls;
static bool fail_console;
static char secondary_input[3][2048];
static size_t secondary_input_size[3];
static size_t secondary_input_offset[3];
static char secondary_output[3][4096];
static size_t secondary_output_size[3];
static char input[APP_REQUEST_CAPACITY];
static size_t input_size;
static size_t input_offset;
static size_t fragment_size;
static char output[131072];
static size_t output_size;
static bool fail_send;
static uint32_t link_speed_mbps;
static bool led_on;
static uint32_t led_writes;
static bool fail_diagnostics;
static uint32_t save_calls;
static uint32_t rtc_writes;
static uint32_t library_loads;
static uint32_t library_deletes;
static bool library_name_ok;

store_error_t command_library_load(uint32_t id)
{
    ++library_loads;
    return id < 64U ? STORE_OK : STORE_NOT_FOUND;
}

store_error_t command_library_save(uint32_t id, const char *name, size_t name_size,
                                   const char *body, size_t size)
{
    library_name_ok = id == 64U && name_size == 6U && memcmp(name, "日本", 6U) == 0;
    return command_store_save(body, size);
}

store_error_t command_library_delete(uint32_t id)
{
    ++library_deletes;
    return id < 64U ? STORE_OK : STORE_NOT_FOUND;
}

size_t command_library_json(char *json, size_t capacity, uint32_t offset)
{
    (void)offset;
    if (capacity < 12U)
    {
        return 0U;
    }
    strcpy(json, "{\"sets\":[]}");
    return strlen(json);
}

uint32_t board_spi_console_duration_us(void);
uint32_t board_spi_console_duration_us(void)
{
    return 42U;
}

bool rtc_parse(const char *text, size_t size, rtc_date_t *date)
{
    memset(date, 0, sizeof(*date));
    return size == 19U && memcmp(text, "2032-02-29T12:34:56", 19U) == 0;
}

bool rtc_set(const rtc_date_t *date)
{
    (void)date;
    ++rtc_writes;
    return true;
}

size_t rtc_json(char *json, size_t capacity)
{
    const char *text = "{\"ready\":true,\"set\":true,\"lse_ready\":true,\"error\":0,\"datetime\":"
                       "\"2032-02-29T12:34:56\"}";
    if (capacity <= strlen(text))
    {
        return 0U;
    }
    strcpy(json, text);
    return strlen(text);
}

store_error_t command_store_save(const char *body, size_t size)
{
    (void)body;
    (void)size;
    ++save_calls;
    return STORE_OK;
}

size_t command_store_json(char *json, size_t capacity)
{
    const char *text = "{\"present\":true,\"busy\":false}";
    if (capacity <= strlen(text))
    {
        return 0U;
    }
    strcpy(json, text);
    return strlen(text);
}

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

bool board_spi_console(const spi_command_t *command, uint8_t rx[SPI_CONSOLE_MAX_BYTES])
{
    size_t i;
    ++spi_calls;
    for (i = 0U; i < command->size; ++i)
    {
        rx[i] = command->tx[i] ^ 0xFFU;
    }
    return !fail_console;
}

uint8_t w5500_state(uint8_t socket)
{
    return state[socket];
}

bool w5500_listen(uint8_t socket)
{
    state[socket] = W5500_LISTEN;
    return true;
}

void w5500_close(uint8_t socket)
{
    state[socket] = W5500_CLOSED;
    mock_sending[socket] = false;
}

void w5500_disconnect(uint8_t socket)
{
    state[socket] = 0x18U;
}

int32_t w5500_receive(uint8_t socket, uint8_t *data, size_t capacity)
{
    const char *source = socket == 0U ? input : secondary_input[socket - 1U];
    size_t *offset = socket == 0U ? &input_offset : &secondary_input_offset[socket - 1U];
    const size_t size = socket == 0U ? input_size : secondary_input_size[socket - 1U];
    size_t count = size - *offset;
    if (count > capacity)
    {
        count = capacity;
    }
    if (count > fragment_size)
    {
        count = fragment_size;
    }
    memcpy(data, source + *offset, count);
    *offset += count;
    return (int32_t)count;
}

int32_t w5500_send_start(uint8_t socket, const void *data, size_t size)
{
    char *destination = socket == 0U ? output : secondary_output[socket - 1U];
    size_t *offset = socket == 0U ? &output_size : &secondary_output_size[socket - 1U];
    const size_t capacity = socket == 0U ? sizeof(output) : sizeof(secondary_output[0]);
    if (fail_send || (*offset + size >= capacity))
    {
        return -1;
    }
    if (mock_sending[socket])
    {
        return 0;
    }
    memcpy(destination + *offset, data, size);
    *offset += size;
    destination[*offset] = '\0';
    mock_sending[socket] = true;
    return 1;
}

int32_t w5500_send_poll(uint8_t socket)
{
    if (!mock_sending[socket])
    {
        return -1;
    }
    if (hold_tx[socket])
    {
        return 0;
    }
    mock_sending[socket] = false;
    return 1;
}

static void pump(const adc_snapshot_t *adc)
{
    uint32_t i;
    for (i = 0U; i < 256U; ++i)
    {
        http_poll(adc);
        if ((state[0] == 0x18U) || (state[0] == W5500_CLOSED))
        {
            return;
        }
    }
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
    memset(state, W5500_LISTEN, sizeof(state));
    state[0] = W5500_ESTABLISHED;
    memset(hold_tx, 0, sizeof(hold_tx));
    memset(mock_sending, 0, sizeof(mock_sending));
    memset(secondary_input_size, 0, sizeof(secondary_input_size));
    memset(secondary_input_offset, 0, sizeof(secondary_input_offset));
    memset(secondary_output_size, 0, sizeof(secondary_output_size));
    memset(secondary_output, 0, sizeof(secondary_output));
    fail_console = false;
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
    pump(&adc);
    CHECK(strstr(output, "HTTP/1.1 200 OK") != NULL);
    CHECK(strstr(output, "\"raw\":[0,4095]") != NULL);
    CHECK(strstr(output, "\"mv\":[0,3300]") != NULL);
    CHECK(strstr(output, "\"adc_ok\":true") != NULL);
    CHECK(strstr(output, "\"led_on\":false") != NULL);
    CHECK(strstr(output, "\"link\":true,\"link_speed_mbps\":100") != NULL);
    CHECK(framing_ok());
    CHECK(state[0] == 0x18U);
    now = APP_REQUEST_TIMEOUT_MS;
    pump(&adc);
    CHECK(state[0] == W5500_CLOSED);
    pump(&adc);
    CHECK(state[0] == W5500_LISTEN);

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    link_speed_mbps = 10U;
    pump(&adc);
    CHECK(strstr(output, "\"link\":true,\"link_speed_mbps\":10") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    link_speed_mbps = 0U;
    pump(&adc);
    CHECK(strstr(output, "\"link\":false,\"link_speed_mbps\":0") != NULL);
    CHECK(framing_ok());

    prepare("GET / HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "<!doctype html>") != NULL);
    CHECK(strstr(output, "fetch('/api/adc'") != NULL);
    CHECK(framing_ok());

    prepare("POST / HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "405 Method Not Allowed") != NULL);
    CHECK(strstr(output, "Allow: GET\r\n") != NULL);
    CHECK(framing_ok());

    prepare("GET /missing HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "404 Not Found") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    adc.valid = 0U;
    pump(&adc);
    CHECK(strstr(output, "\"adc_ok\":false") != NULL);

    prepare("GET / HTTP/1.1\r\n");
    pump(&adc);
    now = APP_REQUEST_TIMEOUT_MS;
    pump(&adc);
    CHECK(state[0] == W5500_CLOSED && output_size == 0U);

    prepare("GET / HTTP/1.1\r\n");
    state[0] = W5500_CLOSE_WAIT;
    pump(&adc);
    CHECK(state[0] == W5500_CLOSED);

    prepare("GET / HTTP/1.1\r\n\r\n");
    fail_send = true;
    pump(&adc);
    CHECK(state[0] == W5500_CLOSED);

    prepare("GET / HTTP/1.1\r\nX: ");
    memset(input + input_size, 'a', sizeof(input) - input_size);
    input_size = sizeof(input);
    pump(&adc);
    CHECK(strstr(output, "431 Request Header Fields Too Large") != NULL);
    CHECK(state[0] == 0x18U);
    CHECK(framing_ok());
    prepare("POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 0\r\n\r\n");
    fragment_size = 1U;
    for (i = 0U; i < input_size; ++i)
    {
        http_poll(&adc);
        CHECK(led_writes == (i + 1U == input_size ? 1U : 0U));
    }
    pump(&adc);
    CHECK(led_on && led_writes == 1U);
    CHECK(strstr(output, "{\"led_on\":true}") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/adc HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "\"led_on\":true") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("GET /api/led/off HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "405 Method Not Allowed") != NULL);
    CHECK(strstr(output, "Allow: POST\r\n") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("POST /api/led/off HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "400 Bad Request") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("POST /api/led/off HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 1\r\n\r\nx");
    pump(&adc);
    CHECK(strstr(output, "400 Bad Request") != NULL);
    CHECK(led_on && led_writes == 1U);

    prepare("POST /api/led/off HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 0\r\n\r\n");
    pump(&adc);
    CHECK(!led_on && led_writes == 2U);
    CHECK(strstr(output, "{\"led_on\":false}") != NULL);
    CHECK(framing_ok());
    http_reset();
    CHECK(!led_on && led_writes == 2U);
    prepare("GET /api/diagnostics HTTP/1.1\r\n\r\n");
    pump(&adc);
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
    pump(&adc);
    CHECK(strstr(output, "503 Service Unavailable") != NULL);
    CHECK(framing_ok());
    prepare("POST /api/spi HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 70\r\n\r\n");
    {
        const char *body = "bus=2 mode=3 hz=1000000 lsb=1 cs_high=0 format=hex\n9F 00 AA FF";
        const int length = snprintf(
            input, sizeof(input),
            "POST /api/spi HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: %lu\r\n\r\n%s",
            (unsigned long)strlen(body), body);
        input_size = (size_t)length;
        fragment_size = 1U;
        for (i = 0U; i + 1U < input_size; ++i)
        {
            http_poll(&adc);
            CHECK(spi_calls == 0U);
        }
        pump(&adc);
        CHECK(spi_calls == 1U);
        CHECK(strstr(output, "\"tx\":\"9F00AAFF\",\"rx\":\"60FF5500\"") != NULL);
        CHECK(strstr(output, "\"transfer_us\":42") != NULL);
        CHECK(framing_ok());
        for (i = 0U; i < 20U; ++i)
        {
            http_poll(&adc);
        }
        CHECK(spi_calls == 1U); /* Retransmitting HTTP data must not re-execute SPI. */
    }

    prepare("GET /api/commands HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "\"present\":true") != NULL);
    CHECK(framing_ok());
    prepare("POST /api/commands HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 4\r\n\r\n1234");
    fragment_size = 1U;
    for (i = 0U; i + 1U < input_size; ++i)
    {
        http_poll(&adc);
        CHECK(save_calls == 0U);
    }
    pump(&adc);
    CHECK(save_calls == 1U && spi_calls == 1U);
    CHECK(strstr(output, "202 Accepted") != NULL);
    CHECK(framing_ok());

    prepare("GET /api/rtc HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "2032-02-29T12:34:56") != NULL);
    CHECK(rtc_writes == 0U && framing_ok());
    prepare("POST /api/rtc HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: "
            "19\r\n\r\n2032-02-29T12:34:56");
    fragment_size = 1U;
    for (i = 0U; i + 1U < input_size; ++i)
    {
        http_poll(&adc);
        CHECK(rtc_writes == 0U);
    }
    pump(&adc);
    CHECK(rtc_writes == 1U && framing_ok());
    for (i = 0U; i < 20U; ++i)
    {
        http_poll(&adc);
    }
    CHECK(rtc_writes == 1U);
    prepare("POST /api/rtc HTTP/1.1\r\nContent-Length: 19\r\n\r\n2032-02-29T12:34:56");
    pump(&adc);
    CHECK(strstr(output, "400 Bad Request") != NULL && rtc_writes == 1U);
    prepare("POST /api/rtc HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 3\r\n\r\nbad");
    pump(&adc);
    CHECK(strstr(output, "400 Bad Request") != NULL && rtc_writes == 1U && framing_ok());

    prepare("GET /api/library HTTP/1.1\r\n\r\n");
    pump(&adc);
    CHECK(strstr(output, "\"sets\":[]") != NULL && framing_ok());
    prepare(
        "POST /api/library HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 8\r\n\r\nload 31\n");
    fragment_size = 1U;
    for (i = 0U; i + 1U < input_size; ++i)
    {
        http_poll(&adc);
        CHECK(library_loads == 0U);
    }
    pump(&adc);
    CHECK(library_loads == 1U && framing_ok());
    prepare(
        "POST /api/library HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 9\r\n\r\nload 31\nx");
    pump(&adc);
    CHECK(strstr(output, "\"error\":5") != NULL && library_loads == 1U && framing_ok());
    prepare("POST /api/library HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 10\r\n\r\ndelete "
            "31\n");
    pump(&adc);
    CHECK(library_deletes == 1U && strstr(output, "202 Accepted") != NULL && framing_ok());
    prepare("GET /api/library HTTP/1.1\r\n\r\n");
    {
        const char body[] = "save 64 6\n日本\n1234";
        input_size = (size_t)snprintf(
            input, sizeof(input),
            "POST /api/library HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: %lu\r\n\r\n%s",
            (unsigned long)(sizeof(body) - 1U), body);
    }
    pump(&adc);
    CHECK(library_name_ok && strstr(output, "202 Accepted") != NULL && framing_ok());

    prepare("GET / HTTP/1.1\r\n"); /* Socket 0 holds an incomplete request. */
    strcpy(secondary_input[0], "GET /api/adc HTTP/1.1\r\n\r\n");
    strcpy(secondary_input[1], "GET /api/diagnostics HTTP/1.1\r\n\r\n");
    strcpy(secondary_input[2], "GET /api/adc HTTP/1.1\r\n\r\n");
    for (i = 0U; i < 3U; ++i)
    {
        secondary_input_size[i] = strlen(secondary_input[i]);
        state[i + 1U] = W5500_ESTABLISHED;
    }
    hold_tx[1] = true; /* One client does not acknowledge its response. */
    for (i = 0U; i < 100U; ++i)
    {
        http_poll(&adc);
    }
    CHECK(state[1] == W5500_ESTABLISHED);
    CHECK(state[2] == 0x18U && state[3] == 0x18U);
    CHECK(strstr(secondary_output[1], "\"http_sockets\":4") != NULL);
    CHECK(strstr(secondary_output[2], "\"raw\":[0,4095]") != NULL);
    CHECK(output_size == 0U);
    adc.raw[0] = 1234U; /* Already queued replies must retain their own snapshot. */
    hold_tx[1] = false;
    for (i = 0U; i < 20U; ++i)
    {
        http_poll(&adc);
    }
    CHECK(state[1] == 0x18U);
    CHECK(strstr(secondary_output[0], "\"raw\":[0,4095]") != NULL);
    CHECK(strstr(secondary_output[0], "\"version\"") == NULL);
    now = APP_REQUEST_TIMEOUT_MS;
    http_poll(&adc);
    CHECK(state[0] == W5500_CLOSED);
    return 0;
}

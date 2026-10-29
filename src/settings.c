#include "settings.h"
#include "board.h"
#include "config.h"
#include <string.h>

static bench_settings_t active;
static bench_settings_t next_network;
static uint32_t apply_started;
static bool apply_pending;

void bench_settings_defaults(bench_settings_t *settings)
{
    const bench_settings_t defaults = {APP_IP_ADDRESS, APP_NETMASK, APP_GATEWAY, APP_HTTP_PORT, 0};
    *settings = defaults;
}

static uint32_t address_word(const uint8_t address[4])
{
    return ((uint32_t)address[0] << 24U) | ((uint32_t)address[1] << 16U) |
           ((uint32_t)address[2] << 8U) | address[3];
}

static bool unicast(const uint8_t address[4])
{
    return address[0] != 0U && address[0] != 127U && address[0] < 224U;
}

bool bench_settings_valid(const bench_settings_t *settings)
{
    const uint32_t ip = address_word(settings->ip);
    const uint32_t mask = address_word(settings->mask);
    const uint32_t host = ~mask;
    const uint32_t gateway = address_word(settings->gateway);
    if (!unicast(settings->ip) || mask == 0U || host < 3U || (host & (host + 1U)) != 0U ||
        (ip & host) == 0U || (ip & host) == host || settings->port == 0U ||
        settings->port > 65535U || settings->calibration < -511 || settings->calibration > 512)
    {
        return false;
    }
    return gateway == 0U ||
           (unicast(settings->gateway) && gateway != ip && (gateway & mask) == (ip & mask) &&
            (gateway & host) != 0U && (gateway & host) != host);
}

static bool number(const char *body, size_t size, size_t *offset, uint32_t *value, char end)
{
    size_t digits = 0U;
    *value = 0U;
    while (*offset < size && body[*offset] >= '0' && body[*offset] <= '9')
    {
        const uint32_t digit = (uint32_t)(body[(*offset)++] - '0');
        if (*value > (UINT32_MAX - digit) / 10U)
        {
            return false;
        }
        *value = *value * 10U + digit;
        ++digits;
    }
    return digits != 0U && *offset < size && body[(*offset)++] == end;
}

static bool key(const char *body, size_t size, size_t *offset, const char *name)
{
    const size_t length = strlen(name);
    if (length > size - *offset || memcmp(body + *offset, name, length) != 0)
    {
        return false;
    }
    *offset += length;
    return true;
}

bool bench_settings_parse(const char *body, size_t size, bench_settings_t *settings,
                          uint32_t *sequence)
{
    static const char *const keys[] = {"ip=", "mask=", "gateway="};
    uint8_t *addresses[] = {settings->ip, settings->mask, settings->gateway};
    uint32_t i;
    uint32_t j;
    uint32_t value;
    size_t offset = 0U;
    bool negative = false;
    for (i = 0U; i < 3U; ++i)
    {
        if (!key(body, size, &offset, keys[i]))
        {
            return false;
        }
        for (j = 0U; j < 4U; ++j)
        {
            if (!number(body, size, &offset, &value, j == 3U ? '\n' : '.') || value > 255U)
            {
                return false;
            }
            addresses[i][j] = (uint8_t)value;
        }
    }
    if (!key(body, size, &offset, "port=") || !number(body, size, &offset, &settings->port, '\n') ||
        !key(body, size, &offset, "cal="))
    {
        return false;
    }
    if (offset < size && body[offset] == '-')
    {
        negative = true;
        ++offset;
    }
    if (!number(body, size, &offset, &value, '\n') || value > 512U)
    {
        return false;
    }
    settings->calibration = negative ? -(int32_t)value : (int32_t)value;
    return key(body, size, &offset, "seq=") && number(body, size, &offset, sequence, '\n') &&
           offset == size && bench_settings_valid(settings);
}

const bench_settings_t *bench_settings_active(void)
{
    if (active.port == 0U)
    {
        bench_settings_defaults(&active);
    }
    return &active;
}

void bench_settings_activate(const bench_settings_t *settings)
{
    active = *settings;
    apply_pending = false;
}

void bench_network_schedule(const bench_settings_t *settings)
{
    next_network = *settings;
    apply_started = board_millis();
    apply_pending = true;
}

bool bench_network_poll(void)
{
    if (!apply_pending || (uint32_t)(board_millis() - apply_started) < 2000U)
    {
        return false;
    }
    bench_settings_activate(&next_network);
    return true;
}

bool bench_network_pending(void)
{
    return apply_pending;
}

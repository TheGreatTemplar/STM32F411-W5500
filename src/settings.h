#ifndef BENCH_SETTINGS_H
#define BENCH_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint32_t port;
    int32_t calibration;
} bench_settings_t;

void bench_settings_defaults(bench_settings_t *settings);
bool bench_settings_valid(const bench_settings_t *settings);
bool bench_settings_parse(const char *body, size_t size, bench_settings_t *settings,
                          uint32_t *sequence);
const bench_settings_t *bench_settings_active(void);
void bench_settings_activate(const bench_settings_t *settings);
void bench_network_schedule(const bench_settings_t *settings);
bool bench_network_poll(void);
bool bench_network_pending(void);

#endif

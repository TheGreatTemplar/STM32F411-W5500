#ifndef BENCH_RTC_H
#define BENCH_RTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
} rtc_date_t;

void rtc_init(void);
void rtc_poll(void);
bool rtc_parse(const char *text, size_t size, rtc_date_t *date);
bool rtc_set(const rtc_date_t *date);
size_t rtc_json(char *json, size_t capacity);
void rtc_calibration_request(int32_t steps);
int32_t rtc_calibration_applied(void);
bool rtc_calibration_pending(void);
uint32_t rtc_calibration_error(void);

#endif

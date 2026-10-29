#include "rtc.h"
#include "board.h"
#include <stdio.h>

#define RTC_MARKER 0x43525443UL
#define RTC_DIVIDERS ((127UL << 16U) | 255U)

/* Errors: 0 none, 1 different clock source, 2 LSE timeout, 3 INIT timeout,
 * 4 incompatible retained configuration, 5 inconsistent calendar. */
static uint32_t error_code;
static uint32_t started_at;
static bool waiting;
static bool ready;
static int32_t calibration_steps;
static bool calibration_pending;
static bool calibration_waiting;
static uint32_t calibration_started;
static uint32_t calibration_error;

void rtc_calibration_request(int32_t steps)
{
    if (steps < -511 || steps > 512)
    {
        calibration_error = 1U;
        return;
    }
    calibration_steps = steps;
    calibration_pending = true;
    calibration_waiting = false;
    calibration_error = 0U;
}

int32_t rtc_calibration_applied(void)
{
    const uint32_t value = RTC->CALR;
    return ((value & RTC_CALR_CALP) != 0U ? 512 : 0) - (int32_t)(value & RTC_CALR_CALM);
}

bool rtc_calibration_pending(void)
{
    return calibration_pending;
}
uint32_t rtc_calibration_error(void)
{
    return calibration_error;
}

static uint32_t month_days(uint32_t year, uint32_t month)
{
    static const uint8_t days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    return days[month - 1U] + ((month == 2U && year % 4U == 0U) ? 1U : 0U);
}

static bool valid_date(const rtc_date_t *date)
{
    return date->year >= 2000U && date->year <= 2099U && date->month >= 1U && date->month <= 12U &&
           date->day >= 1U && date->day <= month_days(date->year, date->month) &&
           date->hour < 24U && date->minute < 60U && date->second < 60U;
}

bool rtc_parse(const char *text, size_t size, rtc_date_t *date)
{
    uint32_t fields[6] = {0U};
    size_t i;
    uint32_t field = 0U;
    if (size != 19U)
    {
        return false;
    }
    for (i = 0U; i < size; ++i)
    {
        if (i == 4U || i == 7U || i == 10U || i == 13U || i == 16U)
        {
            const char separator = i == 10U ? 'T' : (i < 10U ? '-' : ':');
            if (text[i] != separator)
            {
                return false;
            }
            ++field;
        }
        else
        {
            if (text[i] < '0' || text[i] > '9')
            {
                return false;
            }
            fields[field] = fields[field] * 10U + (uint32_t)(text[i] - '0');
        }
    }
    date->year = fields[0];
    date->month = fields[1];
    date->day = fields[2];
    date->hour = fields[3];
    date->minute = fields[4];
    date->second = fields[5];
    return valid_date(date);
}

static uint32_t bcd(uint32_t number)
{
    return ((number / 10U) << 4U) | (number % 10U);
}

static uint32_t decimal(uint32_t number)
{
    if ((number & 15U) > 9U || (number >> 4U) > 9U)
    {
        return UINT32_MAX;
    }
    return (number >> 4U) * 10U + (number & 15U);
}

static void unlock(void)
{
    RTC->WPR = 0xCAU;
    RTC->WPR = 0x53U;
}

static bool enter_init(void)
{
    const uint32_t start = board_millis();
    RTC->ISR |= RTC_ISR_INIT;
    while ((RTC->ISR & RTC_ISR_INITF) == 0U)
    {
        if ((uint32_t)(board_millis() - start) >= 100U)
        {
            RTC->ISR &= ~RTC_ISR_INIT;
            RTC->WPR = 0xFFU;
            ready = false;
            error_code = 3U;
            return false;
        }
    }
    return true;
}

static void calibration_poll(void)
{
    uint32_t value;
    if (!calibration_pending)
    {
        return;
    }
    if (!ready || (RCC->BDCR & RCC_BDCR_LSERDY) == 0U)
    {
        if (!waiting || error_code != 0U)
        {
            calibration_error = 3U;
            calibration_pending = false;
        }
        return;
    }
    if (!calibration_waiting)
    {
        calibration_started = board_millis();
        calibration_waiting = true;
    }
    if ((RTC->ISR & RTC_ISR_RECALPF) != 0U)
    {
        if ((uint32_t)(board_millis() - calibration_started) >= 35000U)
        {
            calibration_error = 2U;
            calibration_pending = false;
        }
        return;
    }
    value = calibration_steps > 0 ? RTC_CALR_CALP | (uint32_t)(512 - calibration_steps)
                                  : (uint32_t)(-calibration_steps);
    unlock();
    RTC->CALR = value; /* 32-second window; calendar and backup markers are untouched. */
    RTC->WPR = 0xFFU;
    calibration_pending = false;
    calibration_error =
        (RTC->CALR & (RTC_CALR_CALP | RTC_CALR_CALM | RTC_CALR_CALW8 | RTC_CALR_CALW16)) == value
            ? 0U
            : 4U;
}

void rtc_init(void)
{
    const uint32_t source = RCC->BDCR & RCC_BDCR_RTCSEL;
    ready = false;
    waiting = false;
    error_code = 0U;
    calibration_pending = false;
    calibration_waiting = false;
    calibration_error = 0U;
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_DBP;
    (void)PWR->CR;
    if (source != 0U && source != RCC_BDCR_RTCSEL_0)
    {
        error_code = 1U; /* Never reset the backup domain implicitly. */
        return;
    }
    RCC->BDCR |= RCC_BDCR_LSEON;
    started_at = board_millis();
    waiting = true;
}

void rtc_poll(void)
{
    bool retained;
    calibration_poll();
    if (!waiting)
    {
        return;
    }
    if ((RCC->BDCR & RCC_BDCR_LSERDY) == 0U)
    {
        if ((uint32_t)(board_millis() - started_at) >= 8000U)
        {
            waiting = false;
            error_code = 2U;
        }
        return;
    }
    waiting = false;
    RCC->BDCR |= RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCEN;
    retained = RTC->BKP0R == RTC_MARKER && RTC->BKP1R == ~RTC_MARKER;
    if (retained && ((RTC->PRER != RTC_DIVIDERS) || ((RTC->CR & RTC_CR_FMT) != 0U)))
    {
        error_code = 4U;
        return;
    }
    unlock();
    if (!retained)
    {
        if (!enter_init())
        {
            return;
        }
        RTC->BKP0R = 0U;
        RTC->BKP1R = 0U;
        RTC->CR = (RTC->CR & ~RTC_CR_FMT) | RTC_CR_BYPSHAD;
        /* RTC prescalers require two separate write accesses in INIT mode. */
        RTC->PRER = 255U;
        RTC->PRER = RTC_DIVIDERS;
        RTC->TR = 0U;
        RTC->DR = (6UL << 13U) | (1UL << 8U) | 1U;
        RTC->ISR &= ~RTC_ISR_INIT;
    }
    else
    {
        RTC->CR |= RTC_CR_BYPSHAD;
        RTC->ISR &= ~RTC_ISR_INIT; /* Resume a set interrupted by a MCU reset. */
    }
    RTC->WPR = 0xFFU;
    ready = true;
}

bool rtc_set(const rtc_date_t *date)
{
    uint32_t year;
    uint32_t month;
    uint32_t days = 0U;
    if (!valid_date(date) || !ready || (RCC->BDCR & RCC_BDCR_LSERDY) == 0U)
    {
        return false;
    }
    for (year = 2000U; year < date->year; ++year)
    {
        days += year % 4U == 0U ? 366U : 365U;
    }
    for (month = 1U; month < date->month; ++month)
    {
        days += month_days(date->year, month);
    }
    days += date->day - 1U;
    unlock();
    if (!enter_init())
    {
        return false;
    }
    RTC->BKP0R = 0U;
    RTC->BKP1R = 0U;
    RTC->TR = (bcd(date->hour) << 16U) | (bcd(date->minute) << 8U) | bcd(date->second);
    RTC->DR = (bcd(date->year - 2000U) << 16U) | (((days + 5U) % 7U + 1U) << 13U) |
              (bcd(date->month) << 8U) | bcd(date->day);
    RTC->ISR &= ~RTC_ISR_INIT;
    RTC->BKP1R = ~RTC_MARKER;
    RTC->BKP0R = RTC_MARKER;
    RTC->WPR = 0xFFU;
    return true;
}

static bool read_calendar(rtc_date_t *date)
{
    uint32_t attempt;
    for (attempt = 0U; attempt < 4U; ++attempt)
    {
        const uint32_t time = RTC->TR;
        const uint32_t day = RTC->DR;
        const uint32_t time_again = RTC->TR;
        const uint32_t day_again = RTC->DR;
        if (time != time_again || day != day_again)
        {
            continue;
        }
        date->year = 2000U + decimal((day >> 16U) & 255U);
        date->month = decimal((day >> 8U) & 31U);
        date->day = decimal(day & 63U);
        date->hour = decimal((time >> 16U) & 63U);
        date->minute = decimal((time >> 8U) & 127U);
        date->second = decimal(time & 127U);
        return valid_date(date);
    }
    return false;
}

size_t rtc_json(char *json, size_t capacity)
{
    rtc_date_t date;
    char text[20] = "";
    const bool lse = (RCC->BDCR & RCC_BDCR_LSERDY) != 0U;
    const bool available = ready && lse;
    const bool marked = available && RTC->BKP0R == RTC_MARKER && RTC->BKP1R == ~RTC_MARKER;
    const bool valid = marked && read_calendar(&date);
    int count;
    if (valid)
    {
        (void)snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u", (unsigned int)date.year,
                       (unsigned int)date.month, (unsigned int)date.day, (unsigned int)date.hour,
                       (unsigned int)date.minute, (unsigned int)date.second);
    }
    count = snprintf(json, capacity,
                     "{\"ready\":%s,\"set\":%s,\"lse_ready\":%s,\"error\":%u,\"datetime\":\"%s\"}",
                     available ? "true" : "false", valid ? "true" : "false", lse ? "true" : "false",
                     (unsigned int)(marked && !valid ? 5U : error_code), text);
    return count > 0 && (size_t)count < capacity ? (size_t)count : 0U;
}

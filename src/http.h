#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    HTTP_INCOMPLETE,
    HTTP_PAGE,
    HTTP_ADC,
    HTTP_NOT_FOUND,
    HTTP_BAD_REQUEST,
    HTTP_METHOD_NOT_ALLOWED,
    HTTP_LED_ON,
    HTTP_LED_OFF,
    HTTP_POST_REQUIRED,
    HTTP_DIAGNOSTICS,
    HTTP_SPI,
    HTTP_COMMANDS,
    HTTP_SAVE_COMMANDS,
    HTTP_RTC,
    HTTP_SET_RTC,
    HTTP_LIBRARY,
    HTTP_LIBRARY_COMMAND,
    HTTP_I2C,
    HTTP_SPI_CONFIG,
    HTTP_SETTINGS,
    HTTP_SAVE_SETTINGS,
    HTTP_APPLY_SETTINGS
} http_route_t;

typedef struct
{
    uint16_t raw[2];
    uint32_t sample_ms;
    uint32_t sample_count;
    uint32_t valid;
} adc_snapshot_t;

http_route_t http_parse(const char *request, size_t size);
void http_reset(void);
void http_poll(const adc_snapshot_t *adc);

#endif

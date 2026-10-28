#ifndef COMMAND_STORE_H
#define COMMAND_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMMAND_STORE_TEXT_MAX 768U
#define COMMAND_STORE_COUNT 4U

typedef enum
{
    STORE_OK,
    STORE_NO_FLASH,
    STORE_UNSUPPORTED,
    STORE_FOREIGN_DATA,
    STORE_BUSY,
    STORE_INVALID,
    STORE_IO,
    STORE_TIMEOUT,
    STORE_VERIFY
} store_error_t;

void command_store_init(void);
void command_store_poll(void);
store_error_t command_store_save(const char *body, size_t size);
size_t command_store_json(char *json, size_t capacity);

#endif

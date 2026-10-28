#ifndef COMMAND_STORE_H
#define COMMAND_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COMMAND_STORE_TEXT_MAX 768U
#define COMMAND_STORE_COUNT 4U
#define COMMAND_LIBRARY_SLOTS 64U
#define COMMAND_LIBRARY_NAME_BYTES 128U

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
    STORE_VERIFY,
    STORE_NOT_FOUND,
    STORE_FULL,
    STORE_NAME,
    STORE_EXISTS
} store_error_t;

void command_store_init(void);
void command_store_poll(void);
store_error_t command_store_save(const char *body, size_t size);
size_t command_store_json(char *json, size_t capacity);
bool command_library_name_valid(const char *name, size_t size);
store_error_t command_library_load(uint32_t id);
store_error_t command_library_save(uint32_t id, const char *name, size_t name_size,
                                   const char *body, size_t size);
store_error_t command_library_delete(uint32_t id);
size_t command_library_json(char *json, size_t capacity, uint32_t offset);

#endif

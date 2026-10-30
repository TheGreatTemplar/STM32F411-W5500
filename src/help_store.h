#ifndef HELP_STORE_H
#define HELP_STORE_H

#include "command_store.h"

void help_store_init(void);
void help_store_poll(void);
bool help_store_ready(void);
size_t help_store_size(void);
store_error_t help_store_error(void);
store_error_t help_store_read(size_t offset, void *data, size_t size);

#endif

#ifndef W5500_H
#define W5500_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define W5500_CLOSED 0x00U
#define W5500_LISTEN 0x14U
#define W5500_ESTABLISHED 0x17U
#define W5500_CLOSE_WAIT 0x1CU

bool w5500_init(void);
bool w5500_healthy(void);
bool w5500_link(void);
uint32_t w5500_link_speed_mbps(void);
uint8_t w5500_state(void);
bool w5500_listen(void);
void w5500_close(void);
void w5500_disconnect(void);
int32_t w5500_receive(uint8_t *data, size_t capacity);
bool w5500_send(const void *data, size_t size);

#endif

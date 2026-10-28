#ifndef W5500_H
#define W5500_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define W5500_CLOSED 0x00U
#define W5500_LISTEN 0x14U
#define W5500_ESTABLISHED 0x17U
#define W5500_CLOSE_WAIT 0x1CU

typedef struct
{
    uint32_t state;
    uint32_t interrupts;
    uint32_t local_port;
    uint32_t remote_port;
    uint32_t rx_waiting;
    uint32_t tx_free;
    uint32_t rx_kb;
    uint32_t tx_kb;
    uint8_t remote_ip[4];
} w5500_socket_diag_t;

typedef struct
{
    uint32_t version;
    uint32_t phy;
    uint32_t mode;
    uint32_t interrupts;
    uint32_t retry_100us;
    uint32_t retry_count;
    uint32_t init_attempts;
    uint32_t spi_errors;
    uint32_t tx_timeouts;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint8_t mac[6];
    uint8_t reserved[2];
    w5500_socket_diag_t sockets[8];
} w5500_diag_t;

bool w5500_diagnostics(w5500_diag_t *diag);
bool w5500_init(void);
bool w5500_healthy(void);
bool w5500_link(void);
uint32_t w5500_link_speed_mbps(void);
uint8_t w5500_state(uint8_t socket);
bool w5500_listen(uint8_t socket);
void w5500_close(uint8_t socket);
void w5500_disconnect(uint8_t socket);
int32_t w5500_receive(uint8_t socket, uint8_t *data, size_t capacity);
/* Start: 1 = queued, 0 = no buffer space yet, -1 = error.
 * Poll:  1 = acknowledged, 0 = pending, -1 = error/timeout. */
int32_t w5500_send_start(uint8_t socket, const void *data, size_t size);
int32_t w5500_send_poll(uint8_t socket);

#endif

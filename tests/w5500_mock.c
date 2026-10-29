#include "board.h"
#include "config.h"
#include "settings.h"
#include "w5500.h"
#include <string.h>

static uint8_t common[64];
static uint8_t socket_regs[8][64];
static uint8_t tx_buffer[4][2048];
static uint8_t rx_buffer[4][2048];
static uint32_t now;
static bool fail_spi;
static uint8_t send_result;
static uint32_t register_writes;

uint32_t board_millis(void)
{
    return ++now;
}

void board_w5500_reset(void)
{
    memset(common, 0, sizeof(common));
    memset(socket_regs, 0, sizeof(socket_regs));
    common[0x39] = 4U;
    common[0x2E] = 1U;
    {
        uint32_t i;
        for (i = 0U; i < 4U; ++i)
        {
            socket_regs[i][0x20] = 8U;
        }
    }
}

bool board_spi_transfer(uint16_t address, uint8_t control, const uint8_t *tx, uint8_t *rx,
                        size_t size)
{
    size_t i;
    const uint8_t block = control >> 3U;
    const uint8_t socket = block == 0U ? 0U : (uint8_t)((block - 1U) / 4U);
    if (fail_spi)
    {
        return false;
    }
    for (i = 0U; i < size; ++i)
    {
        const uint16_t offset = (uint16_t)(address + i);
        uint8_t *cell;
        if (block == 0U)
        {
            cell = &common[offset % sizeof(common)];
        }
        else if ((block & 3U) == 2U)
        {
            cell = &tx_buffer[socket][offset & 2047U];
        }
        else if ((block & 3U) == 3U)
        {
            cell = &rx_buffer[socket][offset & 2047U];
        }
        else
        {
            cell = &socket_regs[(block - 1U) / 4U][offset % 64U];
        }
        if ((control & 4U) != 0U)
        {
            ++register_writes;
            if (((block & 3U) == 1U) && (offset == 2U))
            {
                *cell &= (uint8_t)~tx[i]; /* write-one-to-clear */
            }
            else
            {
                *cell = tx[i];
            }
            if (((block & 3U) == 1U) && (offset == 1U))
            {
                *cell = 0U;
                switch (tx[i])
                {
                case 0x01U:
                    socket_regs[socket][3] = 0x13U;
                    break;
                case 0x02U:
                    socket_regs[socket][3] = W5500_LISTEN;
                    break;
                case 0x10U:
                    socket_regs[socket][3] = W5500_CLOSED;
                    break;
                case 0x08U:
                    socket_regs[socket][3] = 0x18U;
                    break;
                case 0x20U:
                    socket_regs[socket][2] |= send_result;
                    break;
                case 0x40U:
                    socket_regs[socket][0x26] = 0U;
                    socket_regs[socket][0x27] = 0U;
                    break;
                default:
                    break;
                }
            }
        }
        else
        {
            rx[i] = *cell;
        }
    }
    return true;
}

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)

int test_entry(void);
int test_entry(void)
{
    static const uint8_t ip[4] = APP_IP_ADDRESS;
    static const uint8_t mask[4] = APP_NETMASK;
    static const uint8_t gateway[4] = APP_GATEWAY;
    static const uint8_t payload[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    uint8_t received[8] = {0U};
    uint32_t i;
    uint32_t writes_before;
    w5500_diag_t diag;
    now = 0U;
    fail_spi = false;
    send_result = 0x10U;
    CHECK(w5500_init());
    CHECK(memcmp(common + 15U, ip, 4U) == 0);
    CHECK(memcmp(common + 5U, mask, 4U) == 0);
    CHECK(memcmp(common + 1U, gateway, 4U) == 0);
    CHECK(socket_regs[0][0x1E] == 2U && socket_regs[3][0x1E] == 2U && socket_regs[4][0x1E] == 0U);
    CHECK(w5500_link());
    CHECK(w5500_link_speed_mbps() == 10U);
    common[0x2E] = 3U;
    CHECK(w5500_link_speed_mbps() == 100U);
    common[0x2E] = 7U; /* Duplex does not affect the speed. */
    CHECK(w5500_link_speed_mbps() == 100U);
    common[0x2E] = 2U; /* Ignore the speed bit while the link is down. */
    CHECK(w5500_link_speed_mbps() == 0U);
    common[0x2E] = 0U;
    CHECK(w5500_link_speed_mbps() == 0U);
    common[0x2E] = 1U;
    CHECK(w5500_listen(0U));
    CHECK(w5500_state(0U) == W5500_LISTEN);
    CHECK(socket_regs[0][4] == (APP_HTTP_PORT >> 8U));
    CHECK(socket_regs[0][5] == (APP_HTTP_PORT & 255U));
    common[0x15] = 0x80U;
    socket_regs[0][2] = 0x14U;
    socket_regs[0][0x0C] = 172U;
    socket_regs[0][0x0D] = 16U;
    socket_regs[0][0x0E] = 236U;
    socket_regs[0][0x0F] = 100U;
    socket_regs[0][0x10] = 0xC0U;
    socket_regs[0][0x11] = 0x01U;
    writes_before = register_writes;
    CHECK(w5500_diagnostics(&diag));
    CHECK(register_writes == writes_before);
    CHECK(diag.version == 4U && diag.init_attempts == 1U);
    CHECK(diag.interrupts == 0x80U && socket_regs[0][2] == 0x14U);
    CHECK(diag.retry_100us == 2000U && diag.retry_count == 3U);
    CHECK(memcmp(diag.ip, ip, 4U) == 0 && memcmp(diag.mask, mask, 4U) == 0);
    CHECK(diag.sockets[0].local_port == APP_HTTP_PORT);
    CHECK(diag.sockets[0].remote_port == 49153U && diag.sockets[0].remote_ip[3] == 100U);
    CHECK(diag.sockets[0].rx_kb == 2U && diag.sockets[0].tx_kb == 2U);
    CHECK(diag.sockets[7].state == 0U && diag.sockets[7].rx_kb == 0U);
    socket_regs[0][3] = W5500_ESTABLISHED;
    socket_regs[0][0x28] = 0xFFU;
    socket_regs[0][0x29] = 0xFCU;
    socket_regs[0][0x27] = 6U;
    for (i = 0U; i < 6U; ++i)
    {
        rx_buffer[0][(0xFFFCU + i) & 2047U] = payload[i];
    }
    CHECK(w5500_receive(0U, received, sizeof(received)) == 6);
    CHECK(memcmp(received, payload, 6U) == 0);
    CHECK(socket_regs[0][0x28] == 0U && socket_regs[0][0x29] == 2U);
    CHECK(w5500_receive(0U, received, sizeof(received)) == 0);
    socket_regs[0][0x24] = 0xFFU;
    socket_regs[0][0x25] = 0xFCU;
    CHECK(w5500_send_start(0U, payload, sizeof(payload)) == 1);
    CHECK(w5500_send_poll(0U) == 1);
    CHECK(socket_regs[0][0x24] == 0U && socket_regs[0][0x25] == 2U);
    for (i = 0U; i < 6U; ++i)
    {
        CHECK(tx_buffer[0][(0xFFFCU + i) & 2047U] == payload[i]);
    }
    send_result = 0x08U;
    CHECK(w5500_send_start(0U, payload, sizeof(payload)) == 1);
    CHECK(w5500_send_poll(0U) == -1);
    send_result = 0U;
    CHECK(w5500_send_start(0U, payload, sizeof(payload)) == 1);
    CHECK(w5500_send_poll(0U) == 0);
    now += 1000U;
    CHECK(w5500_send_poll(0U) == -1); /* no completion IRQ */
    CHECK(w5500_send_start(0U, payload, 2049U) == -1);
    socket_regs[0][0x20] = 0U;
    CHECK(w5500_send_start(0U, payload, sizeof(payload)) == 0); /* no TX space, no blocking */
    CHECK(w5500_diagnostics(&diag));
    CHECK(diag.tx_timeouts == 2U && diag.rx_bytes == 6U && diag.tx_bytes == 6U);
    for (i = 1U; i < 4U; ++i)
    {
        const uint8_t n = (uint8_t)i;
        send_result = 0x10U;
        CHECK(w5500_listen(n));
        CHECK(w5500_state(n) == W5500_LISTEN);
        CHECK(socket_regs[n][4] == (APP_HTTP_PORT >> 8U));
        socket_regs[n][3] = W5500_ESTABLISHED;
        socket_regs[n][0x27] = 1U;
        rx_buffer[n][0] = (uint8_t)(0xA0U + i);
        CHECK(w5500_receive(n, received, sizeof(received)) == 1);
        CHECK(received[0] == 0xA0U + i);
        CHECK(w5500_send_start(n, payload, sizeof(payload)) == 1);
        CHECK(w5500_send_poll(n) == 1);
        CHECK(tx_buffer[n][0] == payload[0]);
    }
    CHECK(w5500_send_start(4U, payload, sizeof(payload)) == -1);
    CHECK(w5500_send_poll(4U) == -1);
    CHECK(w5500_receive(4U, received, sizeof(received)) == -1);
    fail_spi = true;
    CHECK(w5500_link_speed_mbps() == 0U);
    CHECK(w5500_receive(0U, received, sizeof(received)) == -1);
    CHECK(!w5500_healthy());
    CHECK(!w5500_diagnostics(&diag));
    CHECK(diag.spi_errors == 1U);
    fail_spi = false;
    CHECK(w5500_init());
    CHECK(w5500_healthy());
    CHECK(w5500_diagnostics(&diag));
    CHECK(diag.init_attempts == 2U && diag.spi_errors == 1U && diag.tx_timeouts == 2U);
    w5500_disconnect(0U);
    CHECK(w5500_state(0U) == 0x18U);
    w5500_close(0U);
    CHECK(w5500_state(0U) == W5500_CLOSED);
    {
        bench_settings_t settings;
        bench_settings_defaults(&settings);
        settings.ip[3] = 221U;
        settings.port = 5026U;
        now = 100U;
        bench_network_schedule(&settings);
        now = 2099U;
        CHECK(!bench_network_poll());
        CHECK(bench_settings_active()->port == APP_HTTP_PORT);
        now = 2100U;
        CHECK(bench_network_poll());
        CHECK(w5500_init());
        CHECK(common[18] == 221U);
        CHECK(w5500_listen(0U));
        CHECK(((uint32_t)socket_regs[0][4] << 8U | socket_regs[0][5]) == 5026U);
    }
    return 0;
}

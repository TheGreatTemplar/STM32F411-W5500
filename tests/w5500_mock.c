#include "board.h"
#include "config.h"
#include "w5500.h"
#include <string.h>

static uint8_t common[64];
static uint8_t socket_regs[8][64];
static uint8_t tx_buffer[8192];
static uint8_t rx_buffer[8192];
static uint32_t now;
static bool fail_spi;
static uint8_t send_result;

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
    socket_regs[0][0x20] = 0x20U;
}

bool board_spi_transfer(uint16_t address, uint8_t control, const uint8_t *tx, uint8_t *rx,
                        size_t size)
{
    size_t i;
    const uint8_t block = control >> 3U;
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
        else if (block == 2U)
        {
            cell = &tx_buffer[offset & 8191U];
        }
        else if (block == 3U)
        {
            cell = &rx_buffer[offset & 8191U];
        }
        else
        {
            cell = &socket_regs[(block - 1U) / 4U][offset % 64U];
        }
        if ((control & 4U) != 0U)
        {
            if ((block == 1U) && (offset == 2U))
            {
                *cell &= (uint8_t)~tx[i]; /* write-one-to-clear */
            }
            else
            {
                *cell = tx[i];
            }
            if ((block == 1U) && (offset == 1U))
            {
                *cell = 0U;
                switch (tx[i])
                {
                case 0x01U:
                    socket_regs[0][3] = 0x13U;
                    break;
                case 0x02U:
                    socket_regs[0][3] = W5500_LISTEN;
                    break;
                case 0x10U:
                    socket_regs[0][3] = W5500_CLOSED;
                    break;
                case 0x08U:
                    socket_regs[0][3] = 0x18U;
                    break;
                case 0x20U:
                    socket_regs[0][2] |= send_result;
                    break;
                case 0x40U:
                    socket_regs[0][0x26] = 0U;
                    socket_regs[0][0x27] = 0U;
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
    now = 0U;
    fail_spi = false;
    send_result = 0x10U;
    CHECK(w5500_init());
    CHECK(memcmp(common + 15U, ip, 4U) == 0);
    CHECK(memcmp(common + 5U, mask, 4U) == 0);
    CHECK(memcmp(common + 1U, gateway, 4U) == 0);
    CHECK(socket_regs[0][0x1E] == 8U && socket_regs[1][0x1E] == 0U);
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
    CHECK(w5500_listen());
    CHECK(w5500_state() == W5500_LISTEN);
    CHECK(socket_regs[0][4] == (APP_HTTP_PORT >> 8U));
    CHECK(socket_regs[0][5] == (APP_HTTP_PORT & 255U));
    socket_regs[0][3] = W5500_ESTABLISHED;
    socket_regs[0][0x28] = 0xFFU;
    socket_regs[0][0x29] = 0xFCU;
    socket_regs[0][0x27] = 6U;
    for (i = 0U; i < 6U; ++i)
    {
        rx_buffer[(0xFFFCU + i) & 8191U] = payload[i];
    }
    CHECK(w5500_receive(received, sizeof(received)) == 6);
    CHECK(memcmp(received, payload, 6U) == 0);
    CHECK(socket_regs[0][0x28] == 0U && socket_regs[0][0x29] == 2U);
    CHECK(w5500_receive(received, sizeof(received)) == 0);
    socket_regs[0][0x24] = 0xFFU;
    socket_regs[0][0x25] = 0xFCU;
    CHECK(w5500_send(payload, sizeof(payload)));
    CHECK(socket_regs[0][0x24] == 0U && socket_regs[0][0x25] == 2U);
    for (i = 0U; i < 6U; ++i)
    {
        CHECK(tx_buffer[(0xFFFCU + i) & 8191U] == payload[i]);
    }
    send_result = 0x08U;
    CHECK(!w5500_send(payload, sizeof(payload)));
    send_result = 0U;
    CHECK(!w5500_send(payload, sizeof(payload))); /* no completion IRQ */
    CHECK(!w5500_send(payload, 8193U));
    socket_regs[0][0x20] = 0U;
    CHECK(!w5500_send(payload, sizeof(payload))); /* no TX space */
    fail_spi = true;
    CHECK(w5500_link_speed_mbps() == 0U);
    CHECK(w5500_receive(received, sizeof(received)) == -1);
    CHECK(!w5500_healthy());
    fail_spi = false;
    CHECK(w5500_init());
    CHECK(w5500_healthy());
    w5500_disconnect();
    CHECK(w5500_state() == 0x18U);
    w5500_close();
    CHECK(w5500_state() == W5500_CLOSED);
    return 0;
}

#include "w5500.h"
#include "board.h"
#include "config.h"
#include "settings.h"
#include <string.h>

/* W5500 variable-length SPI frames. HTTP uses sockets 0 through 3. */
#define COMMON_BLOCK 0U
#define SOCKET_BLOCK(n) ((uint8_t)(1U + 4U * (uint32_t)(n)))
#define TX_BLOCK(n) ((uint8_t)(2U + 4U * (uint32_t)(n)))
#define RX_BLOCK(n) ((uint8_t)(3U + 4U * (uint32_t)(n)))
#define SN_MR 0x0000U
#define SN_CR 0x0001U
#define SN_IR 0x0002U
#define SN_SR 0x0003U
#define SN_PORT 0x0004U
#define SN_RXBUF_SIZE 0x001EU
#define SN_TXBUF_SIZE 0x001FU
#define SN_TX_FSR 0x0020U
#define SN_TX_WR 0x0024U
#define SN_RX_RSR 0x0026U
#define SN_RX_RD 0x0028U

static bool io_ok;
static bool sending[APP_HTTP_SOCKETS];
static uint32_t send_started[APP_HTTP_SOCKETS];
static uint32_t send_size[APP_HTTP_SOCKETS];
static uint32_t init_attempts;
static uint32_t spi_errors;
static uint32_t tx_timeouts;
static uint32_t rx_bytes;
static uint32_t tx_bytes;

static void read_bytes(uint8_t block, uint16_t address, uint8_t *data, size_t size)
{
    if (io_ok && !board_spi_transfer(address, (uint8_t)(block << 3U), NULL, data, size))
    {
        ++spi_errors;
        io_ok = false;
    }
}

static void write_bytes(uint8_t block, uint16_t address, const uint8_t *data, size_t size)
{
    if (io_ok &&
        !board_spi_transfer(address, (uint8_t)(((uint32_t)block << 3U) | 4U), data, NULL, size))
    {
        ++spi_errors;
        io_ok = false;
    }
}

static uint8_t read8(uint8_t block, uint16_t address)
{
    uint8_t value = 0U;
    read_bytes(block, address, &value, 1U);
    return value;
}

static void write8(uint8_t block, uint16_t address, uint8_t value)
{
    write_bytes(block, address, &value, 1U);
}

static uint16_t read16(uint8_t socket, uint16_t address)
{
    uint8_t bytes[2] = {0U, 0U};
    read_bytes(SOCKET_BLOCK(socket), address, bytes, sizeof(bytes));
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static void write16(uint8_t socket, uint16_t address, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)(value >> 8U), (uint8_t)value};
    write_bytes(SOCKET_BLOCK(socket), address, bytes, sizeof(bytes));
}

static uint16_t stable16(uint8_t socket, uint16_t address)
{
    uint32_t attempt;
    uint16_t old = read16(socket, address);
    for (attempt = 0U; io_ok && (attempt < 16U); ++attempt)
    {
        const uint16_t value = read16(socket, address);
        if (old == value)
        {
            return value;
        }
        old = value;
    }
    io_ok = false;
    return 0U;
}

static bool command(uint8_t socket, uint8_t value)
{
    const uint32_t start = board_millis();
    write8(SOCKET_BLOCK(socket), SN_CR, value);
    while (io_ok && (read8(SOCKET_BLOCK(socket), SN_CR) != 0U))
    {
        if ((uint32_t)(board_millis() - start) >= 20U)
        {
            io_ok = false;
        }
    }
    return io_ok;
}

bool w5500_init(void)
{
    static const uint8_t mac[6] = APP_MAC_ADDRESS;
    const bench_settings_t *settings = bench_settings_active();
    const uint8_t *ip = settings->ip;
    const uint8_t *mask = settings->mask;
    const uint8_t *gateway = settings->gateway;
    uint32_t socket;
    uint8_t check[4] = {0U, 0U, 0U, 0U};
    ++init_attempts;
    board_w5500_reset();
    memset(sending, 0, sizeof(sending));
    io_ok = true;
    if (read8(COMMON_BLOCK, 0x0039U) != 0x04U)
    {
        io_ok = false;
        return false;
    }
    write_bytes(COMMON_BLOCK, 0x0001U, gateway, 4U);
    write_bytes(COMMON_BLOCK, 0x0005U, mask, 4U);
    write_bytes(COMMON_BLOCK, 0x0009U, mac, sizeof(mac));
    write_bytes(COMMON_BLOCK, 0x000FU, ip, 4U);
    /* Retransmission interval 200 ms, 3 retries. DHCP/DNS are not used. */
    write8(COMMON_BLOCK, 0x0019U, 0x07U);
    write8(COMMON_BLOCK, 0x001AU, 0xD0U);
    write8(COMMON_BLOCK, 0x001BU, 3U);
    for (socket = 0U; socket < 8U; ++socket)
    {
        const uint8_t block = (uint8_t)(1U + 4U * socket);
        write8(block, SN_RXBUF_SIZE, socket < APP_HTTP_SOCKETS ? 2U : 0U);
        write8(block, SN_TXBUF_SIZE, socket < APP_HTTP_SOCKETS ? 2U : 0U);
    }
    read_bytes(COMMON_BLOCK, 0x000FU, check, sizeof(check));
    for (socket = 0U; socket < 4U; ++socket)
    {
        if (check[socket] != ip[socket])
        {
            io_ok = false;
        }
    }
    return io_ok;
}

bool w5500_healthy(void)
{
    return io_ok;
}

bool w5500_link(void)
{
    return (read8(COMMON_BLOCK, 0x002EU) & 1U) != 0U;
}

uint32_t w5500_link_speed_mbps(void)
{
    /* PHYCFGR: bit 0 = link, bit 1 = negotiated speed (1: 100, 0: 10 Mbps). */
    const uint8_t phy = read8(COMMON_BLOCK, 0x002EU);
    if (!io_ok || ((phy & 1U) == 0U))
    {
        return 0U;
    }
    return (phy & 2U) != 0U ? 100U : 10U;
}

uint8_t w5500_state(uint8_t socket)
{
    if (socket >= APP_HTTP_SOCKETS)
    {
        return W5500_CLOSED;
    }
    return read8(SOCKET_BLOCK(socket), SN_SR);
}

void w5500_close(uint8_t socket)
{
    if (socket >= APP_HTTP_SOCKETS)
    {
        return;
    }
    sending[socket] = false;
    (void)command(socket, 0x10U);
    write8(SOCKET_BLOCK(socket), SN_IR, 0xFFU);
}

bool w5500_listen(uint8_t socket)
{
    if (socket >= APP_HTTP_SOCKETS)
    {
        return false;
    }
    w5500_close(socket);
    write8(SOCKET_BLOCK(socket), SN_MR, 0x01U); /* TCP */
    write16(socket, SN_PORT, (uint16_t)bench_settings_active()->port);
    if (!command(socket, 0x01U) || (w5500_state(socket) != 0x13U))
    {
        io_ok = false;
        return false;
    }
    return command(socket, 0x02U);
}

void w5500_disconnect(uint8_t socket)
{
    if (socket >= APP_HTTP_SOCKETS)
    {
        return;
    }
    (void)command(socket, 0x08U);
}

int32_t w5500_receive(uint8_t socket, uint8_t *data, size_t capacity)
{
    uint16_t count;
    uint16_t pointer;
    if (socket >= APP_HTTP_SOCKETS)
    {
        return -1;
    }
    count = stable16(socket, SN_RX_RSR);
    if (!io_ok)
    {
        return -1;
    }
    if (count > APP_SOCKET_BUFFER_SIZE)
    {
        io_ok = false;
        return -1;
    }
    if (count > capacity)
    {
        count = (uint16_t)capacity;
    }
    if (count == 0U)
    {
        return 0;
    }
    pointer = read16(socket, SN_RX_RD);
    /* W5500 wraps its ring buffer internally, including the 16-bit pointer wrap. */
    read_bytes(RX_BLOCK(socket), pointer, data, count);
    write16(socket, SN_RX_RD, (uint16_t)(pointer + count));
    if (!command(socket, 0x40U))
    {
        return -1;
    }
    rx_bytes += count;
    return (int32_t)count;
}

int32_t w5500_send_start(uint8_t socket, const void *data, size_t size)
{
    uint16_t pointer;
    uint16_t free_size;
    uint8_t state;
    if (!io_ok || (socket >= APP_HTTP_SOCKETS) || (size == 0U) || (size > APP_SOCKET_BUFFER_SIZE))
    {
        return -1;
    }
    if (sending[socket])
    {
        return 0;
    }
    state = w5500_state(socket);
    if ((state != W5500_ESTABLISHED) && (state != W5500_CLOSE_WAIT))
    {
        return -1;
    }
    free_size = stable16(socket, SN_TX_FSR);
    if (!io_ok)
    {
        return -1;
    }
    if (free_size < size)
    {
        return 0;
    }
    pointer = read16(socket, SN_TX_WR);
    write_bytes(TX_BLOCK(socket), pointer, data, size);
    write16(socket, SN_TX_WR, (uint16_t)(pointer + size));
    write8(SOCKET_BLOCK(socket), SN_IR, 0x18U);
    if (!command(socket, 0x20U))
    {
        return -1;
    }
    sending[socket] = true;
    send_started[socket] = board_millis();
    send_size[socket] = (uint32_t)size;
    return 1;
}

int32_t w5500_send_poll(uint8_t socket)
{
    uint8_t irq;
    uint8_t state;
    if (!io_ok || (socket >= APP_HTTP_SOCKETS) || !sending[socket])
    {
        return -1;
    }
    irq = read8(SOCKET_BLOCK(socket), SN_IR);
    if (!io_ok)
    {
        return -1;
    }
    if (((irq & 0x08U) != 0U) || ((uint32_t)(board_millis() - send_started[socket]) >= 1000U))
    {
        ++tx_timeouts;
        sending[socket] = false;
        return -1;
    }
    if ((irq & 0x10U) != 0U)
    {
        write8(SOCKET_BLOCK(socket), SN_IR, 0x10U);
        sending[socket] = false;
        if (!io_ok)
        {
            return -1;
        }
        tx_bytes += send_size[socket];
        return 1;
    }
    state = w5500_state(socket);
    if ((state != W5500_ESTABLISHED) && (state != W5500_CLOSE_WAIT))
    {
        sending[socket] = false;
        return -1;
    }
    return io_ok ? 0 : -1;
}

static uint16_t diagnostic_read16(uint8_t block, uint16_t address, bool changing)
{
    uint32_t attempt;
    uint16_t previous = 0U;
    for (attempt = 0U; io_ok && (attempt < 16U); ++attempt)
    {
        uint8_t bytes[2] = {0U, 0U};
        uint16_t value;
        read_bytes(block, address, bytes, sizeof(bytes));
        value = (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
        if (!changing || ((attempt != 0U) && (value == previous)))
        {
            return value;
        }
        previous = value;
    }
    io_ok = false;
    return 0U;
}

bool w5500_diagnostics(w5500_diag_t *diag)
{
    uint32_t socket;
    memset(diag, 0, sizeof(*diag));
    diag->version = read8(COMMON_BLOCK, 0x0039U);
    diag->phy = read8(COMMON_BLOCK, 0x002EU);
    diag->mode = read8(COMMON_BLOCK, 0x0000U);
    diag->interrupts = read8(COMMON_BLOCK, 0x0015U);
    diag->retry_100us = diagnostic_read16(COMMON_BLOCK, 0x0019U, false);
    diag->retry_count = read8(COMMON_BLOCK, 0x001BU);
    read_bytes(COMMON_BLOCK, 0x0001U, diag->gateway, sizeof(diag->gateway));
    read_bytes(COMMON_BLOCK, 0x0005U, diag->mask, sizeof(diag->mask));
    read_bytes(COMMON_BLOCK, 0x0009U, diag->mac, sizeof(diag->mac));
    read_bytes(COMMON_BLOCK, 0x000FU, diag->ip, sizeof(diag->ip));
    for (socket = 0U; socket < 8U; ++socket)
    {
        const uint8_t block = (uint8_t)(1U + 4U * socket);
        w5500_socket_diag_t *entry = &diag->sockets[socket];
        entry->state = read8(block, SN_SR);
        entry->interrupts = read8(block, SN_IR);
        entry->local_port = diagnostic_read16(block, SN_PORT, false);
        entry->remote_port = diagnostic_read16(block, 0x0010U, false);
        entry->rx_waiting = diagnostic_read16(block, SN_RX_RSR, true);
        entry->tx_free = diagnostic_read16(block, SN_TX_FSR, true);
        entry->rx_kb = read8(block, SN_RXBUF_SIZE);
        entry->tx_kb = read8(block, SN_TXBUF_SIZE);
        read_bytes(block, 0x000CU, entry->remote_ip, sizeof(entry->remote_ip));
    }
    diag->init_attempts = init_attempts;
    diag->spi_errors = spi_errors;
    diag->tx_timeouts = tx_timeouts;
    diag->rx_bytes = rx_bytes;
    diag->tx_bytes = tx_bytes;
    /* Read-only snapshot: do not acknowledge or clear interrupt flags. */
    return io_ok;
}

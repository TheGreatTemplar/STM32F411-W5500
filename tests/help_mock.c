#include "board.h"
#include "flash_layout.h"
#include "help_seed.h"
#include "help_store.h"
#include <string.h>

static uint8_t media[FLASH_HELP_BYTES];
static uint8_t baseline[FLASH_HELP_BYTES];
uint8_t help_captured[32768];
uint32_t help_captured_size;
static uint32_t now;
static uint32_t writes;
static uint32_t busy;
static bool claimed;
static bool latch;
static bool io_failure;
static bool corrupt_write;
static bool stuck;
static store_error_t claim_error;

#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            return __LINE__;                                                                       \
        }                                                                                          \
    } while (0)

uint32_t board_millis(void)
{
    return ++now;
}

store_error_t command_flash_claim(void)
{
    if (claimed)
    {
        return STORE_BUSY;
    }
    if (claim_error != STORE_OK)
    {
        return claim_error;
    }
    if (busy != 0U)
    {
        --busy;
        return STORE_BUSY;
    }
    claimed = true;
    return STORE_OK;
}

void command_flash_release(void)
{
    claimed = false;
}

bool board_flash_transfer(const uint8_t *prefix, size_t prefix_size, const uint8_t *tx, uint8_t *rx,
                          size_t size)
{
    uint32_t address;
    uint32_t offset;
    size_t i;
    if (!claimed || io_failure)
    {
        return false;
    }
    if (prefix_size == 1U && prefix[0] == 0x05U)
    {
        *rx = (busy != 0U ? 1U : 0U) | (latch ? 2U : 0U);
        if (busy != 0U && !stuck)
        {
            --busy;
        }
        return true;
    }
    if (prefix_size == 1U && prefix[0] == 0x06U && busy == 0U)
    {
        latch = true;
        return true;
    }
    if (prefix_size != 4U || busy != 0U)
    {
        return false;
    }
    address = ((uint32_t)prefix[1] << 16U) | ((uint32_t)prefix[2] << 8U) | prefix[3];
    if (address < FLASH_HELP_BASE || address >= FLASH_HELP_BASE + FLASH_HELP_BYTES ||
        size > FLASH_HELP_BASE + FLASH_HELP_BYTES - address)
    {
        return false;
    }
    offset = address - FLASH_HELP_BASE;
    if (prefix[0] == 0x03U)
    {
        memcpy(rx, media + offset, size);
        return true;
    }
    if (!latch)
    {
        return false;
    }
    if (prefix[0] == 0x20U)
    {
        if ((address & 4095U) != 0U)
        {
            return false;
        }
        memset(media + offset, 0xFF, 4096U);
    }
    else if (prefix[0] == 0x02U)
    {
        if (size == 0U || size > 256U || (address & 255U) + size > 256U)
        {
            return false;
        }
        for (i = 0U; i < size; ++i)
        {
            media[offset + i] &= tx[i];
        }
        if (corrupt_write && (offset % FLASH_HELP_BANK_BYTES) >= 256U)
        {
            media[offset] ^= 1U;
            corrupt_write = false;
        }
    }
    else
    {
        return false;
    }
    ++writes;
    busy = 2U;
    latch = false;
    return true;
}

static void restart(void)
{
    claimed = false;
    latch = false;
    io_failure = false;
    corrupt_write = false;
    stuck = false;
    claim_error = STORE_OK;
    help_store_init();
}

static bool finish(void)
{
    uint32_t i;
    for (i = 0U; i < 4000U; ++i)
    {
        help_store_poll();
        if (!claimed && help_store_error() != STORE_BUSY)
        {
            return true;
        }
    }
    return false;
}

static uint32_t crc(const uint8_t *data, size_t size)
{
    uint32_t value = UINT32_MAX;
    size_t i;
    for (i = 0U; i < size; ++i)
    {
        uint32_t bit;
        value ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return value ^ UINT32_MAX;
}

int test_entry(void)
{
    uint32_t before;
    uint32_t i;
    uint32_t j;
    uint32_t header[6];
    static const uint32_t cuts[] = {1U, 3U, 4U, 5U, 20U, 49U, 50U, 51U};
    memset(media, 0xFF, sizeof(media));
    restart();
    CHECK(finish());
    CHECK(help_store_ready() && help_store_error() == STORE_OK);
    CHECK(help_store_size() == sizeof(help_seed));
    CHECK(help_store_read(0U, help_captured, sizeof(help_seed)) == STORE_OK);
    CHECK(memcmp(help_captured, help_seed, sizeof(help_seed)) == 0);
    help_captured_size = sizeof(help_seed);
    CHECK(help_store_read(sizeof(help_seed), help_captured, 1U) == STORE_INVALID);
    CHECK(help_store_read(SIZE_MAX, help_captured, 1U) == STORE_INVALID);
    claimed = true;
    CHECK(help_store_read(0U, help_captured, 1U) == STORE_BUSY);
    claimed = false;
    before = writes;
    restart();
    CHECK(finish() && writes == before); /* No reflash of the same valid document. */

    /* A valid older gzip stream differs only in its timestamp. */
    media[256U + 4U] ^= 1U;
    memcpy(header, media, sizeof(header));
    header[2] = 7U;
    header[4] = crc(media + 256U, sizeof(help_seed));
    memcpy(media, header, sizeof(header));
    memcpy(baseline, media, sizeof(media));
    for (i = 0U; i < sizeof(cuts) / sizeof(cuts[0]); ++i)
    {
        memcpy(media, baseline, sizeof(media));
        busy = 0U;
        before = writes;
        restart();
        for (j = 0U; j < 4000U && writes < before + cuts[i]; ++j)
        {
            help_store_poll();
        }
        CHECK(writes >= before + cuts[i]);
        CHECK(memcmp(media, baseline, FLASH_HELP_BANK_BYTES) == 0);
        restart(); /* MCU reset, including while NOR is physically busy. */
        CHECK(finish() && help_store_ready() && help_store_error() == STORE_OK);
        CHECK(help_store_read(0U, help_captured, sizeof(help_seed)) == STORE_OK);
        CHECK(memcmp(help_captured, help_seed, sizeof(help_seed)) == 0);
        CHECK(memcmp(media, baseline, FLASH_HELP_BANK_BYTES) == 0);
    }

    /* An interrupted commit is owned, but must not be used until verified again. */
    media[FLASH_HELP_BANK_BYTES + 20U] = 0x3CU;
    media[FLASH_HELP_BANK_BYTES + 21U] = 0xC3U;
    media[FLASH_HELP_BANK_BYTES + 22U] = 0xFFU;
    media[FLASH_HELP_BANK_BYTES + 23U] = 0xFFU;
    restart();
    CHECK(finish() && help_store_ready() && help_store_error() == STORE_OK);

    memcpy(media, baseline, sizeof(media));
    restart();
    corrupt_write = true;
    CHECK(finish());
    CHECK(help_store_ready() && help_store_error() == STORE_VERIFY);
    CHECK(memcmp(media, baseline, FLASH_HELP_BANK_BYTES) == 0);

    memset(media, 0x5A, sizeof(media));
    before = writes;
    restart();
    CHECK(finish());
    CHECK(!help_store_ready() && help_store_error() == STORE_FOREIGN_DATA);
    CHECK(writes == before && media[0] == 0x5AU && media[sizeof(media) - 1U] == 0x5AU);

    memset(media, 0xFF, sizeof(media));
    restart();
    stuck = true;
    for (i = 0U; i < 5000U && help_store_error() != STORE_TIMEOUT; ++i)
    {
        help_store_poll();
    }
    CHECK(help_store_error() == STORE_TIMEOUT && claimed);
    CHECK(command_flash_claim() == STORE_BUSY);
    busy = 0U;
    stuck = false;
    help_store_poll();
    CHECK(!claimed && !help_store_ready());
    restart();
    CHECK(finish() && help_store_ready());

    restart();
    claim_error = STORE_NO_FLASH;
    CHECK(finish() && !help_store_ready() && help_store_error() == STORE_NO_FLASH);
    restart();
    claim_error = STORE_UNSUPPORTED;
    CHECK(finish() && !help_store_ready() && help_store_error() == STORE_UNSUPPORTED);
    restart();
    help_store_poll(); /* Acquire; a failing read must retain ownership. */
    io_failure = true;
    help_store_poll();
    CHECK(claimed && help_store_error() == STORE_IO);
    io_failure = false;
    help_store_poll();
    CHECK(!claimed);
    return 0;
}

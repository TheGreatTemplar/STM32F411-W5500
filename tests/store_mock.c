#include "board.h"
#include "command_store.h"
#include <string.h>

static uint8_t media[8192];
static uint32_t now;
static uint32_t busy_polls;
static uint32_t writes;
static bool write_latch;
static bool fail_io;
static bool unsupported;
static bool alternate_id;

uint32_t board_millis(void)
{
    return ++now;
}

void board_delay(uint32_t duration)
{
    now += duration;
}

bool board_flash_transfer(const uint8_t *prefix, size_t prefix_size, const uint8_t *tx, uint8_t *rx,
                          size_t size)
{
    uint32_t address;
    uint32_t offset;
    size_t i;
    if (fail_io || (prefix_size == 0U))
    {
        return false;
    }
    if (prefix[0] == 0xABU)
    {
        return true;
    }
    if (prefix[0] == 0x9FU)
    {
        if ((rx == NULL) || (size != 3U))
        {
            return false;
        }
        if (busy_polls != 0U)
        {
            memset(rx, 0, size);
            return true;
        }
        rx[0] = 0xEFU;
        rx[1] = alternate_id ? 0x70U : 0x40U;
        rx[2] = unsupported ? 0x18U : 0x17U;
        return true;
    }
    if (prefix[0] == 0x05U)
    {
        rx[0] = (busy_polls != 0U ? 1U : 0U) | (write_latch ? 2U : 0U);
        if (busy_polls != 0U)
        {
            --busy_polls;
        }
        return true;
    }
    if (prefix[0] == 0x06U)
    {
        if (busy_polls != 0U)
        {
            return false;
        }
        write_latch = true;
        return true;
    }
    if (prefix_size != 4U)
    {
        return false;
    }
    address = ((uint32_t)prefix[1] << 16U) | ((uint32_t)prefix[2] << 8U) | prefix[3];
    if ((address < 8380416UL) || (address >= 8388608UL) || (size > 8388608UL - address))
    {
        return false;
    }
    offset = address - 8380416UL;
    if (prefix[0] == 0x03U)
    {
        memcpy(rx, media + offset, size);
        return true;
    }
    if (!write_latch || (busy_polls != 0U))
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
        busy_polls = 3U;
    }
    else if (prefix[0] == 0x02U)
    {
        if ((size == 0U) || (size > 256U) || ((address & 255U) + size > 256U))
        {
            return false;
        }
        for (i = 0U; i < size; ++i)
        {
            media[offset + i] &= tx[i];
        }
        busy_polls = 1U;
    }
    else
    {
        return false;
    }
    ++writes;
    write_latch = false;
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

static char json[8192];
static char maximum[4096];

static bool finish_save(void)
{
    uint32_t i;
    for (i = 0U; i < 500U; ++i)
    {
        command_store_poll();
        if (command_store_json(json, sizeof(json)) == 0U)
        {
            return false;
        }
        if (strstr(json, "\"busy\":false") != NULL)
        {
            return strstr(json, "\"save_ok\":true") != NULL;
        }
    }
    return false;
}

int test_entry(void);
int test_entry(void)
{
    static const char a[] =
        "bus=2 mode=0 hz=20000000 lsb=0 cs_high=0 format=hex\nlengths=5,2,0,5\n9F 00AA01 02";
    static const char b[] =
        "bus=2 mode=3 hz=18000000 lsb=1 cs_high=1 format=hex\nlengths=5,2,2,0\n0A 0BFF00";
    static const char invalid[] =
        "bus=2 mode=0 hz=1000000 lsb=0 cs_high=0 format=hex\nlengths=2,0,0,0\nGG";
    uint32_t before;
    uint32_t i;
    memset(media, 0xFF, sizeof(media));
    busy_polls = 5U; /* Erase still active after a MCU-only reset. */
    command_store_init();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"ready\":true") != NULL);
    CHECK(strstr(json, "\"has_saved\":false") != NULL);
    CHECK(writes == 0U);
    CHECK(command_store_save(invalid, sizeof(invalid) - 1U) == STORE_INVALID);
    CHECK(writes == 0U);
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    CHECK(command_store_save(b, sizeof(b) - 1U) == STORE_BUSY);
    CHECK(finish_save());
    CHECK(strstr(json, "\"sequence\":1") != NULL);
    CHECK(strstr(json, "\"commands\":[\"9F 00\",\"AA\",\"\",\"01 02\"]") != NULL);
    command_store_init();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"hz\":20000000") != NULL);
    before = writes;
    CHECK(command_store_save(b, sizeof(b) - 1U) == STORE_OK);
    for (i = 0U; i < 7U; ++i)
    {
        command_store_poll();
    }
    CHECK(writes > before);
    /* Simulated power loss before the new commit word: old data must survive. */
    command_store_init();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"sequence\":1") != NULL);
    CHECK(strstr(json, "\"hz\":20000000") != NULL);
    CHECK(command_store_save(b, sizeof(b) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(strstr(json, "\"sequence\":2") != NULL);
    CHECK(strstr(json, "\"commands\":[\"0A 0B\",\"FF\",\"00\",\"\"]") != NULL);
    media[4096U + 100U] ^= 1U;
    command_store_init();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"sequence\":1") != NULL); /* CRC rejects damaged newer copy. */
    CHECK(command_store_json(json, 16U) == 0U);
    memset(media, 0xFF, sizeof(media));
    media[3] = 0U;
    before = writes;
    command_store_init();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_FOREIGN_DATA);
    CHECK(writes == before);
    unsupported = true;
    command_store_init();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_UNSUPPORTED);
    CHECK(writes == before);
    unsupported = false;
    alternate_id = true;
    memset(media, 0xFF, sizeof(media));
    command_store_init();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"jedec\":\"EF7017\"") != NULL);
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    fail_io = true;
    command_store_poll();
    fail_io = false;
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"error\":6") != NULL);
    memset(media, 0xFF, sizeof(media));
    command_store_init();
    {
        const char *header =
            "bus=2 mode=2 hz=16000000 lsb=0 cs_high=0 format=hex\nlengths=768,768,768,768\n";
        const size_t length = strlen(header);
        memcpy(maximum, header, length);
        for (i = 0U; i < 4U * 768U; i += 3U)
        {
            memcpy(maximum + length + i, "A5 ", 3U);
        }
        CHECK(command_store_save(maximum, length + 4U * 768U) == STORE_OK);
        CHECK(finish_save());
        command_store_init();
        CHECK(command_store_json(json, sizeof(json)) != 0U);
        CHECK(strstr(json, "\"has_saved\":true") != NULL);
        CHECK(strstr(json, "\"hz\":16000000") != NULL);
    }
    {
        const char *header =
            "bus=2 mode=0 hz=1000000 lsb=0 cs_high=0 format=ascii\nlengths=256,256,256,256\n";
        const size_t length = strlen(header);
        memcpy(maximum, header, length);
        memset(maximum + length, 1, 1024U);
        CHECK(command_store_save(maximum, length + 1024U) == STORE_OK);
        CHECK(finish_save());
        CHECK(strstr(json, "\\u0001\\u0001") != NULL);
        CHECK(strlen(json) < sizeof(json));
    }
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    busy_polls = 2000U;
    for (i = 0U; i < 1100U; ++i)
    {
        command_store_poll();
    }
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"busy\":false") != NULL);
    CHECK(strstr(json, "\"error\":7") != NULL);
    return 0;
}

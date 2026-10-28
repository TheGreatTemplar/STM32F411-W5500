#include "board.h"
#include "command_store.h"
#include <string.h>

#define MEDIA_BASE 7864320UL
#define LAST_PAIR (63U * 8192U)
static uint8_t media[524288];
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
    if ((address < MEDIA_BASE) || (address >= 8388608UL) || (size > 8388608UL - address))
    {
        return false;
    }
    offset = address - MEDIA_BASE;
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

static void init_store(void)
{
    uint32_t i;
    command_store_init();
    for (i = 0U; i < 2200U; ++i)
    {
        command_store_poll();
    }
}

static uint32_t legacy_crc(const uint8_t *data)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    uint32_t bit;
    for (i = 0U; i < 3132U; ++i)
    {
        if (i >= 16U && i < 24U)
        {
            continue;
        }
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return ~crc;
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
    init_store();
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
    init_store();
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
    init_store();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"sequence\":1") != NULL);
    CHECK(strstr(json, "\"hz\":20000000") != NULL);
    CHECK(command_store_save(b, sizeof(b) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(strstr(json, "\"sequence\":2") != NULL);
    CHECK(strstr(json, "\"commands\":[\"0A 0B\",\"FF\",\"00\",\"\"]") != NULL);
    media[LAST_PAIR + 4096U + 100U] ^= 1U;
    init_store();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"sequence\":1") != NULL); /* CRC rejects damaged newer copy. */
    CHECK(command_store_json(json, 16U) == 0U);
    memset(media, 0xFF, sizeof(media));
    media[LAST_PAIR + 3U] = 0U;
    before = writes;
    init_store();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_FOREIGN_DATA);
    CHECK(writes == before);
    unsupported = true;
    init_store();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_UNSUPPORTED);
    CHECK(writes == before);
    unsupported = false;
    alternate_id = true;
    memset(media, 0xFF, sizeof(media));
    init_store();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"jedec\":\"EF7017\"") != NULL);
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    fail_io = true;
    command_store_poll();
    fail_io = false;
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"error\":6") != NULL);
    memset(media, 0xFF, sizeof(media));
    init_store();
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
        init_store();
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
    busy_polls = 0U;
    init_store();
    CHECK(command_library_name_valid("RFIC03", 6U));
    CHECK(command_library_name_valid("日本語", sizeof("日本語") - 1U));
    CHECK(command_library_name_valid("Проверка", sizeof("Проверка") - 1U));
    CHECK(!command_library_name_valid("../x", 4U));
    CHECK(!command_library_name_valid(" bad", 4U));
    CHECK(!command_library_name_valid("\xC0\xAF", 2U));
    CHECK(!command_library_name_valid("\xED\xA0\x80", 3U));
    CHECK(!command_library_name_valid("\xF4\x90\x80\x80", 4U));
    CHECK(!command_library_name_valid("123456789012345678901234567890123", 33U));
    CHECK(command_library_save(64U, "日本語", sizeof("日本語") - 1U, a, sizeof(a) - 1U) ==
          STORE_OK);
    CHECK(finish_save());
    CHECK(strstr(json, "日本語") != NULL);
    CHECK(command_library_json(json, sizeof(json), 0U) != 0U);
    CHECK(strstr(json, "日本語") != NULL);
    CHECK(command_library_save(64U, "日本語", sizeof("日本語") - 1U, b, sizeof(b) - 1U) ==
          STORE_EXISTS);
    CHECK(command_library_load(0U) == STORE_OK);
    CHECK(command_library_save(0U, "Проверка", sizeof("Проверка") - 1U, b, sizeof(b) - 1U) ==
          STORE_OK);
    CHECK(finish_save());
    init_store();
    CHECK(command_library_load(0U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "Проверка") != NULL);
    CHECK(command_library_delete(0U) == STORE_OK);
    for (i = 0U; i < 7U; ++i)
    {
        command_store_poll();
    }
    init_store();
    CHECK(command_library_load(0U) == STORE_OK); /* Interrupted deletion keeps the live copy. */
    CHECK(command_library_delete(0U) == STORE_OK);
    CHECK(finish_save());
    init_store();
    CHECK(command_library_load(0U) == STORE_NOT_FOUND);
    CHECK(command_library_save(64U, "New", 3U, a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(command_library_load(0U) == STORE_OK);
    CHECK(command_library_json(json, sizeof(json), 64U) == 0U);
    memset(media, 0xFF, sizeof(media));
    init_store();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    {
        uint32_t word = 1U;
        memcpy(media + LAST_PAIR + 4U, &word, 4U);
        word = 3108U;
        memcpy(media + LAST_PAIR + 12U, &word, 4U);
        word = legacy_crc(media + LAST_PAIR);
        memcpy(media + LAST_PAIR + 16U, &word, 4U);
    }
    init_store();
    CHECK(command_library_load(63U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"name\":\"Default\"") != NULL);
    CHECK(command_library_save(63U, "Миграция", sizeof("Миграция") - 1U, b, sizeof(b) - 1U) ==
          STORE_OK);
    CHECK(finish_save());
    CHECK(media[LAST_PAIR + 4U] == 1U); /* Old record survives until new commit. */
    init_store();
    CHECK(command_library_load(63U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "Миграция") != NULL);
    memset(media, 0xFF, sizeof(media));
    media[4000U] = 0x5AU; /* Foreign bytes outside the used record length must also be protected. */
    init_store();
    CHECK(command_library_save(0U, "Foreign", 7U, a, sizeof(a) - 1U) == STORE_FOREIGN_DATA);
    CHECK(command_library_save(64U, "Safe", 4U, a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(media[4000U] == 0x5AU);
    CHECK(command_library_load(1U) == STORE_OK);
    memset(media, 0xFF, sizeof(media));
    init_store();
    for (i = 0U; i < 64U; ++i)
    {
        const char name[3] = {'S', (char)('0' + i / 10U), (char)('0' + i % 10U)};
        CHECK(command_library_save(64U, name, sizeof(name), a, sizeof(a) - 1U) == STORE_OK);
        CHECK(finish_save());
    }
    CHECK(command_library_save(64U, "Overflow", 8U, a, sizeof(a) - 1U) == STORE_FULL);
    init_store();
    CHECK(command_library_json(json, sizeof(json), 56U) != 0U);
    CHECK(strstr(json, "\"used\":64") != NULL && strstr(json, "\"next\":null") != NULL);
    CHECK(command_library_load(17U) == STORE_OK);
    CHECK(command_library_delete(17U) == STORE_OK);
    CHECK(finish_save());
    CHECK(command_library_save(64U, "Reuse", 5U, b, sizeof(b) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(command_library_load(17U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "Reuse") != NULL);
    CHECK(command_library_load(16U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "S16") != NULL);
    CHECK(command_library_delete(17U) == STORE_OK);
    CHECK(finish_save());
    CHECK(strstr(json, "S16") != NULL); /* Deleting another file does not change the working set. */
    return 0;
}

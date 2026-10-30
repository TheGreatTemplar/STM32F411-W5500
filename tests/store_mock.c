#include "board.h"
#include "command_store.h"
#include <string.h>

#define MEDIA_BASE 7340032UL
#define OLD_BASE 524288U
#define LAST_PAIR (OLD_BASE + 63U * 8192U)
static uint8_t media[1056768];
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
    if ((address < MEDIA_BASE - 8192U) || (address >= 8388608UL) || (size > 8388608UL - address))
    {
        return false;
    }
    offset =
        address >= MEDIA_BASE ? address - MEDIA_BASE : 1048576U + address - (MEDIA_BASE - 8192U);
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
    for (i = 0U; i < 9000U; ++i)
    {
        command_store_poll();
    }
}

static uint32_t sized_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    uint32_t bit;
    for (i = 0U; i < size; ++i)
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

static bool finish_system(void)
{
    uint32_t i;
    for (i = 0U; i < 500U; ++i)
    {
        bench_settings_t settings;
        uint32_t sequence;
        bool busy;
        store_error_t error;
        command_store_poll();
        command_system_get(&settings, &sequence, &busy, &error);
        if (!busy)
        {
            return error == STORE_OK;
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
    init_store();
    CHECK(command_store_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"ready\":true") != NULL);
    CHECK(strstr(json, "\"has_saved\":false") != NULL);
    CHECK(writes == 0U);
    CHECK(command_storage_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"complete\":true") != NULL);
    CHECK(strstr(json, "\"capacity\":8388608") != NULL);
    CHECK(strstr(json, "\"unallocated_bytes\":7266304") != NULL);
    CHECK(strstr(json, "\"spi\":{\"used\":0,\"blocked\":0,\"free\":64}") != NULL);
    CHECK(strstr(json, "\"i2c\":{\"used\":0,\"blocked\":0,\"free\":64}") != NULL);
    CHECK(strstr(json, "\"help_bytes\":65536") != NULL);
    CHECK(command_storage_json(json, 8U) == 0U);
    CHECK(command_flash_claim() == STORE_OK);
    CHECK(command_flash_claim() == STORE_BUSY);
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_BUSY);
    command_store_poll();
    CHECK(writes == 0U);
    command_flash_release();
    CHECK(command_store_save(invalid, sizeof(invalid) - 1U) == STORE_INVALID);
    CHECK(writes == 0U);
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    CHECK(command_store_save(b, sizeof(b) - 1U) == STORE_BUSY);
    CHECK(finish_save());
    CHECK(command_storage_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"spi\":{\"used\":1,\"blocked\":0,\"free\":63}") != NULL);
    CHECK(command_store_json(json, sizeof(json)) != 0U);
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
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "日本語", sizeof("日本語") - 1U, a,
                               sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(strstr(json, "日本語") != NULL);
    CHECK(command_library_json(json, sizeof(json), 0U) != 0U);
    CHECK(strstr(json, "日本語") != NULL);
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "日本語", sizeof("日本語") - 1U, b,
                               sizeof(b) - 1U) == STORE_EXISTS);
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
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "New", 3U, a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(command_library_load(0U) == STORE_OK);
    CHECK(command_library_json(json, sizeof(json), 128U) == 0U);
    memset(media, 0xFF, sizeof(media));
    init_store();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    {
        uint32_t word = 1U;
        memcpy(media + LAST_PAIR + 4U, &word, 4U);
        word = 3108U;
        memcpy(media + LAST_PAIR + 12U, &word, 4U);
        word = sized_crc(media + LAST_PAIR, 3132U);
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
    media[OLD_BASE + 4000U] =
        0x5AU; /* Foreign bytes outside the used record length must also be protected. */
    init_store();
    CHECK(command_library_save(0U, "Foreign", 7U, a, sizeof(a) - 1U) == STORE_FOREIGN_DATA);
    CHECK(command_storage_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"spi\":{\"used\":0,\"blocked\":1,\"free\":63}") != NULL);
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Safe", 4U, a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(media[OLD_BASE + 4000U] == 0x5AU);
    CHECK(command_library_load(1U) == STORE_OK);
    memset(media, 0xFF, sizeof(media));
    init_store();
    for (i = 0U; i < 64U; ++i)
    {
        const char name[3] = {'S', (char)('0' + i / 10U), (char)('0' + i % 10U)};
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, name, sizeof(name), a, sizeof(a) - 1U) ==
              STORE_OK);
        CHECK(finish_save());
    }
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Overflow", 8U, a, sizeof(a) - 1U) ==
          STORE_FULL);
    init_store();
    CHECK(command_library_json(json, sizeof(json), 56U) != 0U);
    CHECK(strstr(json, "\"used\":64") != NULL && strstr(json, "\"next\":64") != NULL);
    CHECK(command_library_load(17U) == STORE_OK);
    CHECK(command_library_delete(17U) == STORE_OK);
    CHECK(finish_save());
    CHECK(command_storage_json(json, sizeof(json)) != 0U);
    CHECK(strstr(json, "\"spi\":{\"used\":63,\"blocked\":0,\"free\":1}") != NULL);
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Reuse", 5U, b, sizeof(b) - 1U) == STORE_OK);
    CHECK(finish_save());
    CHECK(command_library_load(17U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "Reuse") != NULL);
    CHECK(command_library_load(16U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "S16") != NULL);
    CHECK(command_library_delete(17U) == STORE_OK);
    CHECK(finish_save());
    CHECK(strstr(json, "S16") != NULL); /* Deleting another file does not change the working set. */
    memset(media, 0xFF, sizeof(media));
    init_store();
    CHECK(command_store_save(a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save());
    {
        uint32_t word = 2U;
        memcpy(media + LAST_PAIR + 4U, &word, 4U);
        word = 3268U - 24U;
        memcpy(media + LAST_PAIR + 12U, &word, 4U);
        word = sized_crc(media + LAST_PAIR, 3268U);
        memcpy(media + LAST_PAIR + 16U, &word, 4U);
    }
    init_store();
    CHECK(command_library_load(63U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "\"bus\":2") != NULL);
    {
        const char i2c[] =
            "bus=1 hz=400000 addr=80 op=2 read=2 format=hex\nlengths=2,5,0,2\n00AB CD10";
        const char read_only[] =
            "bus=1 hz=100000 addr=8 op=1 read=256 format=hex\nlengths=0,0,0,0\n";
        const char bad_read[] =
            "bus=1 hz=100000 addr=8 op=1 read=256 format=hex\nlengths=2,0,0,0\n00";
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, "I2C", 3U, i2c, sizeof(i2c) - 1U) ==
              STORE_OK);
        CHECK(finish_save());
        init_store();
        CHECK(command_library_load(64U) == STORE_OK);
        CHECK(command_store_json(json, sizeof(json)) != 0U);
        CHECK(strstr(json, "\"bus\":1") != NULL && strstr(json, "\"address\":80") != NULL);
        CHECK(strstr(json, "\"read_size\":2") != NULL && strstr(json, "AB CD") != NULL);
        CHECK(command_library_save(64U, "I2C", 3U, bad_read, sizeof(bad_read) - 1U) ==
              STORE_INVALID);
        CHECK(command_library_save(64U, "I2C", 3U, read_only, sizeof(read_only) - 1U) == STORE_OK);
        CHECK(finish_save());
        init_store();
        CHECK(command_library_load(64U) == STORE_OK);
        CHECK(command_store_json(json, sizeof(json)) != 0U &&
              strstr(json, "\"read_size\":256") != NULL);
        CHECK(command_library_load(63U) == STORE_OK); /* Legacy SPI remains intact. */
        CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "\"bus\":2") != NULL);
    }
    /* A v3 I2C file from the former shared region migrates copy-first. */
    CHECK(command_library_save(63U, "Default", 7U, a, sizeof(a) - 1U) == STORE_OK);
    CHECK(finish_save()); /* This newer SPI file must remain the default after migration. */
    memcpy(media + OLD_BASE, media, 8192U);
    /* Build genuine v3 fixtures: older CRC/payload excludes v4 command names. */
    for (i = 0U; i < 2U; ++i)
    {
        uint8_t *record = media + OLD_BASE + i * 4096U;
        uint32_t word;
        if (record[4] != 5U)
        {
            continue;
        }
        word = 3U;
        memcpy(record + 4U, &word, 4U);
        word = 3284U - 24U;
        memcpy(record + 12U, &word, 4U);
        word = sized_crc(record, 3284U);
        memcpy(record + 16U, &word, 4U);
    }
    memset(media, 0xFF, 8192U);
    command_store_init();
    for (i = 0U; i < 4106U; ++i)
    {
        command_store_poll();
    }
    CHECK(media[OLD_BASE + 4U] == 3U); /* Interrupted destination write keeps the source. */
    init_store();
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "\"id\":63") != NULL);
    CHECK(command_library_load(0U) == STORE_NOT_FOUND);
    CHECK(command_library_load(65U) == STORE_OK); /* Torn first destination is protected. */
    CHECK(command_store_json(json, sizeof(json)) != 0U &&
          strstr(json, "\"read_size\":256") != NULL);

    /* Power loss after destination commit must reuse it, not create another file. */
    memcpy(media + OLD_BASE, media + 8192U, 8192U);
    memset(media, 0xFF, 16384U);
    command_store_init();
    for (i = 0U; i < 4096U; ++i)
    {
        command_store_poll();
    }
    for (i = 0U; i < 1000U; ++i)
    {
        uint32_t marker;
        command_store_poll();
        memcpy(&marker, media + 20U, sizeof(marker));
        if (marker == 0xA55AC33CUL)
        {
            break;
        }
    }
    CHECK(i < 1000U);
    {
        uint32_t sequence;
        uint32_t retained;
        memcpy(&sequence, media + 8U, sizeof(sequence));
        init_store();
        memcpy(&retained, media + 8U, sizeof(retained));
        CHECK(sequence == retained);
        CHECK(command_library_load(64U) == STORE_OK && command_library_load(0U) == STORE_NOT_FOUND);
    }

    /* Both buses have their own full quota; equal names across buses are allowed. */
    memset(media, 0xFF, sizeof(media));
    init_store();
    for (i = 0U; i < 64U; ++i)
    {
        const char name[3] = {'S', (char)('0' + i / 10U), (char)('0' + i % 10U)};
        const char read_only[] =
            "bus=1 hz=100000 addr=80 op=1 read=4 format=hex\nlengths=0,0,0,0\n";
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, name, sizeof(name), a, sizeof(a) - 1U) ==
              STORE_OK);
        CHECK(finish_save());
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, name, sizeof(name), read_only,
                                   sizeof(read_only) - 1U) == STORE_OK);
        CHECK(finish_save());
    }
    CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Full", 4U, a, sizeof(a) - 1U) == STORE_FULL);
    {
        const char read_only[] =
            "bus=1 hz=100000 addr=80 op=1 read=4 format=hex\nlengths=0,0,0,0\n";
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Full", 4U, read_only,
                                   sizeof(read_only) - 1U) == STORE_FULL);
        CHECK(command_library_save(0U, "Wrong", 5U, read_only, sizeof(read_only) - 1U) ==
              STORE_INVALID);
    }
    init_store();
    CHECK(command_library_load(63U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "\"bus\":2") != NULL);
    CHECK(command_library_load(127U) == STORE_OK);
    CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "\"bus\":1") != NULL);
    CHECK(command_library_json(json, sizeof(json), 120U) != 0U);
    CHECK(strstr(json, "\"used\":128") != NULL && strstr(json, "\"next\":null") != NULL);
    /* v4 names survive reboot and preserve arbitrary printable UTF-8. */
    memset(media, 0xFF, sizeof(media));
    init_store();
    {
        const char named[] =
            "bus=2 mode=0 hz=25000000 lsb=0 cs_high=0 format=hex\nlengths=2,0,0,0\n9F"
            "\nnames=6,16,9,11\nRFIC03Проверка日本語RD / status";
        const char i2c_named[] = "bus=1 hz=100000 addr=8 op=1 read=2 format=hex\nlengths=0,0,0,0\n"
                                 "\nnames=6,0,0,0\nRead 2";
        const char *bad_names[] = {
            "\nnames=1,0,0,0\n\001", "\nnames=2,0,0,0\n\xC0\xAF",
            "\nnames=4,0,0,0\n\xF4\x90\x80\x80", "\nnames=2,0,0,0\nA",
            "\nnames=49,0,0,0\nAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Named", 5U, named, sizeof(named) - 1U) ==
              STORE_OK);
        CHECK(finish_save());
        /* Convert the new record to genuine v4 bytes before testing upgrade. */
        {
            uint8_t *record = media + OLD_BASE;
            uint32_t word = 4U;
            uint32_t n;
            for (n = 0U; n < 4U; ++n)
            {
                uint32_t bytes;
                memcpy(&bytes, record + 3284U + n * 4U, 4U);
                memmove(record + 3300U + n * 128U, record + 3300U + n * 192U, bytes);
            }
            memcpy(record + 4U, &word, 4U);
            word = 3812U - 24U;
            memcpy(record + 12U, &word, 4U);
            word = sized_crc(record, 3812U);
            memcpy(record + 16U, &word, 4U);
        }
        init_store();
        CHECK(command_library_load(0U) == STORE_OK);
        CHECK(command_store_json(json, sizeof(json)) != 0U);
        CHECK(strstr(json,
                     "\"command_names\":[\"RFIC03\",\"Проверка\",\"日本語\",\"RD / status\"]") !=
              NULL);
        CHECK(command_library_save(COMMAND_LIBRARY_NEW, "Named", 5U, i2c_named,
                                   sizeof(i2c_named) - 1U) == STORE_OK);
        CHECK(finish_save());
        init_store();
        CHECK(command_library_load(64U) == STORE_OK);
        CHECK(command_store_json(json, sizeof(json)) != 0U && strstr(json, "Read 2") != NULL);
        before = writes;
        for (i = 0U; i < sizeof(bad_names) / sizeof(bad_names[0]); ++i)
        {
            memcpy(maximum, a, sizeof(a) - 1U);
            memcpy(maximum + sizeof(a) - 1U, bad_names[i], strlen(bad_names[i]));
            CHECK(command_store_save(maximum, sizeof(a) - 1U + strlen(bad_names[i])) ==
                  STORE_INVALID);
        }
        CHECK(writes == before);
    }
    {
        const char *header =
            "bus=2 mode=0 hz=25000000 lsb=0 cs_high=0 format=hex\nlengths=768,768,768,768\n";
        const char *names = "\nnames=192,192,192,192\n";
        size_t length = strlen(header);
        memcpy(maximum, header, length);
        for (i = 0U; i < 3072U; i += 3U)
        {
            memcpy(maximum + length + i, "A5 ", 3U);
        }
        length += 3072U;
        memcpy(maximum + length, names, strlen(names));
        length += strlen(names);
        for (i = 0U; i < 768U; i += 4U)
        {
            memcpy(maximum + length + i, "\xF0\x9F\x98\x80", 4U);
        }
        length += 768U;
        CHECK(command_library_save(0U, "Named", 5U, maximum, length) == STORE_OK);
        CHECK(finish_save());
        init_store();
        CHECK(command_library_load(0U) == STORE_OK);
        CHECK(command_store_json(json, sizeof(json)) != 0U &&
              strstr(json, "\xF0\x9F\x98\x80") != NULL);
        /* Interrupt a replacement: names and data must remain from the committed copy. */
        CHECK(command_library_save(0U, "Named", 5U, a, sizeof(a) - 1U) == STORE_OK);
        for (i = 0U; i < 7U; ++i)
        {
            command_store_poll();
        }
        init_store();
        CHECK(command_library_load(0U) == STORE_OK);
        CHECK(command_store_json(json, sizeof(json)) != 0U &&
              strstr(json, "\xF0\x9F\x98\x80") != NULL);
    }
    memset(media, 0xFF, sizeof(media));
    init_store();
    {
        bench_settings_t settings;
        bench_settings_t loaded;
        uint32_t sequence;
        bool busy;
        store_error_t error;
        bench_settings_defaults(&settings);
        CHECK(bench_settings_valid(&settings));
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 0U && error == STORE_OK && !busy);
        settings.ip[3] = 221U;
        settings.port = 5026U;
        settings.calibration = -36;
        CHECK(command_system_save(&settings, 0U) == STORE_OK);
        CHECK(command_library_save(0U, "Safe", 4U, a, sizeof(a) - 1U) == STORE_BUSY);
        command_store_poll();
        init_store();
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 0U && loaded.ip[3] == 220U);
        CHECK(command_system_save(&settings, 0U) == STORE_OK);
        CHECK(finish_system());
        init_store();
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 1U && loaded.ip[3] == 221U && loaded.port == 5026U &&
              loaded.calibration == -36);
        CHECK(command_system_save(&settings, 0U) == STORE_EXISTS);
        settings.ip[3] = 222U;
        settings.calibration = 512;
        CHECK(command_system_save(&settings, 1U) == STORE_OK);
        for (i = 0U; i < 9U; ++i)
        {
            command_store_poll();
        }
        init_store();
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 1U && loaded.ip[3] == 221U && loaded.calibration == -36);
        CHECK(command_system_save(&settings, 1U) == STORE_OK);
        CHECK(finish_system());
        init_store();
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 2U && loaded.ip[3] == 222U && loaded.calibration == 512);
        media[1048576U + 4096U + 24U] ^= 1U;
        init_store();
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 1U && loaded.ip[3] == 221U);
        CHECK(command_system_save(&settings, 1U) == STORE_OK);
        CHECK(finish_system());
        CHECK(command_system_save(&settings, 2U) == STORE_OK);
        fail_io = true;
        command_store_poll();
        fail_io = false;
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(!busy && error == STORE_IO && sequence == 2U);
        init_store();

        CHECK(command_library_save(0U, "Safe", 4U, a, sizeof(a) - 1U) == STORE_OK);
        CHECK(finish_save());
        command_system_get(&loaded, &sequence, &busy, &error);
        CHECK(sequence == 2U && loaded.port == 5026U);
        settings.mask[2] = 250U;
        CHECK(!bench_settings_valid(&settings));
        CHECK(command_system_save(&settings, 2U) == STORE_INVALID);
        bench_settings_defaults(&settings);
        settings.ip[3] = 0U;
        CHECK(!bench_settings_valid(&settings));
        settings.ip[3] = 255U;
        CHECK(!bench_settings_valid(&settings));
        bench_settings_defaults(&settings);
        settings.gateway[2] = 235U;
        CHECK(!bench_settings_valid(&settings));
        memset(settings.gateway, 0, 4U);
        CHECK(bench_settings_valid(&settings));
        settings.calibration = -512;
        CHECK(!bench_settings_valid(&settings));
        memset(media + 1048576U, 0x5A, 8192U);
        init_store();
        settings.calibration = 0;
        before = writes;
        CHECK(command_system_save(&settings, 0U) == STORE_FOREIGN_DATA);
        CHECK(writes == before && media[1048576U] == 0x5AU);
    }
    return 0;
}

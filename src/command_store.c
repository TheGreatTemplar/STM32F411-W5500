#include "command_store.h"
#include "board.h"
#include "clock.h"
#include "flash_layout.h"
#include "i2c_console.h"
#include "spi_console.h"
#include <stdio.h>
#include <string.h>

#define FLASH_BYTES 8388608UL
#define SECTOR_BYTES 4096UL
#define STORE_BASE 0x00700000UL
#define UART_STORE_BASE 0x0066E000UL
#define SPI_STORE_BASE (FLASH_BYTES - COMMAND_LIBRARY_SLOTS * 2UL * SECTOR_BYTES)
#define STORE_MAGIC 0x34444D43UL
#define STORE_COMMIT 0xA55AC33CUL
#define SYSTEM_BASE (STORE_BASE - 2UL * SECTOR_BYTES)
#define SYSTEM_MAGIC 0x31535953UL

typedef char
    help_partition_before_system[(FLASH_HELP_BASE + FLASH_HELP_BYTES == SYSTEM_BASE) ? 1 : -1];

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t payload_size;
    uint32_t crc;
    uint32_t commit;
    uint32_t hz;
    uint32_t mode;
    uint32_t lsb;
    uint32_t cs_high;
    uint32_t ascii;
    uint32_t lengths[COMMAND_STORE_COUNT];
    char texts[COMMAND_STORE_COUNT][COMMAND_STORE_TEXT_MAX];
    uint32_t deleted;
    uint32_t name_size;
    char name[COMMAND_LIBRARY_NAME_BYTES];
    uint32_t bus;
    uint32_t address;
    uint32_t operation;
    uint32_t read_size;
    uint32_t command_name_sizes[COMMAND_STORE_COUNT];
    char command_names[COMMAND_STORE_COUNT][COMMAND_STORE_NAME_BYTES];
} store_record_t;

#define V3_BYTES offsetof(store_record_t, command_name_sizes)
#define V4_BYTES (V3_BYTES + 16U + 4U * 128U)
#define V2_BYTES offsetof(store_record_t, bus)

#define LEGACY_BYTES offsetof(store_record_t, deleted)

typedef struct
{
    uint32_t sequence;
    uint32_t copy;
    uint32_t bus;
    uint32_t kind; /* 0 free, 1 live, 2 deleted, 3 foreign/damaged */
    uint32_t name_size;
    char name[COMMAND_LIBRARY_NAME_BYTES];
} library_entry_t;

static library_entry_t catalog[COMMAND_LIBRARY_TOTAL];
static bool scanning;
static bool migrating;
static uint32_t scan_sector;
static size_t scan_offset;
static bool scan_blank;
static uint32_t generation;
static uint32_t selected_id;
static uint32_t target_id;

typedef char record_must_fit_sector[(sizeof(store_record_t) < SECTOR_BYTES) ? 1 : -1];

typedef enum
{
    JOB_IDLE,
    JOB_ERASE,
    JOB_ERASE_WAIT,
    JOB_PROGRAM,
    JOB_PROGRAM_WAIT,
    JOB_VERIFY,
    JOB_COMMIT,
    JOB_COMMIT_WAIT
} store_job_t;

static store_record_t current;
static store_record_t candidate;
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t payload_size;
    uint32_t crc;
    uint32_t commit;
    bench_settings_t settings;
} system_record_t;
typedef char system_record_must_be_44[(sizeof(system_record_t) == 44U) ? 1 : -1];
static system_record_t system_current;
static bool system_reusable[2];
static bool system_job;
static bool external_lock;
static uint32_t system_copy;
static store_error_t system_error;
static size_t job_bytes;
static uint8_t jedec[3];
static bool present;
static bool ready;
static bool has_saved;
static bool save_ok;
static store_error_t last_error;
static store_job_t job;
static uint32_t target_address;
static uint32_t operation_start;
static size_t position;
static size_t pending_size;

static uint32_t crc_byte(uint32_t crc, uint8_t value)
{
    uint32_t bit;
    crc ^= value;
    for (bit = 0U; bit < 8U; ++bit)
    {
        crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
    return crc;
}

static size_t record_bytes(const store_record_t *record)
{
    return record->version == 1U   ? LEGACY_BYTES
           : record->version == 2U ? V2_BYTES
           : record->version == 3U ? V3_BYTES
           : record->version == 4U ? V4_BYTES
                                   : sizeof(*record);
}

static void normalize(store_record_t *record)
{
    uint32_t i;
    if (record->version == 4U)
    {
        for (i = COMMAND_STORE_COUNT; i-- != 0U;)
        {
            memmove(record->command_names[i], (char *)record->command_names + i * 128U,
                    record->command_name_sizes[i]);
        }
    }
    if (record->version < 4U)
    {
        memset(record->command_name_sizes, 0, sizeof(record->command_name_sizes));
        memset(record->command_names, 0, sizeof(record->command_names));
    }
    if (record->version < 3U)
    {
        record->bus = 2U;
        record->address = 0U;
        record->operation = 0U;
        record->read_size = 0U;
    }
    if (record->version == 1U)
    {
        record->deleted = 0U;
        record->name_size = 7U;
        memcpy(record->name, "Default", 7U);
    }
    record->version = 5U;
}

static uint32_t block_crc(const void *record, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = UINT32_MAX;
    size_t i;
    for (i = 0U; i < size; ++i)
    {
        if ((i < 16U) || (i >= 24U))
        {
            crc = crc_byte(crc, bytes[i]);
        }
    }
    return ~crc;
}

static uint32_t record_crc(const store_record_t *record)
{
    return block_crc(record, record_bytes(record));
}

static bool flash_command(uint8_t command)
{
    return board_flash_transfer(&command, 1U, NULL, NULL, 0U);
}

static bool status(uint8_t *value)
{
    const uint8_t command = 0x05U;
    return board_flash_transfer(&command, 1U, NULL, value, 1U);
}

static bool addressed(uint8_t command, uint32_t address, const uint8_t *tx, uint8_t *rx,
                      size_t size)
{
    const uint8_t prefix[4] = {command, (uint8_t)(address >> 16U), (uint8_t)(address >> 8U),
                               (uint8_t)address};
    return board_flash_transfer(prefix, sizeof(prefix), tx, rx, size);
}

static bool write_enable(void)
{
    uint8_t value;
    return flash_command(0x06U) && status(&value) && ((value & 3U) == 2U);
}

static bool label_valid(const char *name, size_t size, bool filename)
{
    size_t i = 0U;
    uint32_t count = 0U;
    if (size == 0U)
    {
        return !filename;
    }
    if (size > (filename ? COMMAND_LIBRARY_NAME_BYTES : COMMAND_STORE_NAME_BYTES) ||
        (filename && (name[0] == ' ' || name[size - 1U] == ' ' || (size == 1U && name[0] == '.') ||
                      (size == 2U && name[0] == '.' && name[1] == '.'))))
    {
        return false;
    }
    while (i < size)
    {
        uint32_t code;
        uint32_t minimum;
        uint32_t tail;
        const uint8_t first = (uint8_t)name[i++];
        if (first < 0x80U)
        {
            code = first;
            minimum = 0U;
            tail = 0U;
        }
        else if (first >= 0xC2U && first <= 0xDFU)
        {
            code = first & 31U;
            minimum = 0x80U;
            tail = 1U;
        }
        else if (first >= 0xE0U && first <= 0xEFU)
        {
            code = first & 15U;
            minimum = 0x800U;
            tail = 2U;
        }
        else if (first >= 0xF0U && first <= 0xF4U)
        {
            code = first & 7U;
            minimum = 0x10000U;
            tail = 3U;
        }
        else
        {
            return false;
        }
        while (tail-- != 0U)
        {
            uint8_t next;
            if (i == size)
            {
                return false;
            }
            next = (uint8_t)name[i++];
            if ((next & 0xC0U) != 0x80U)
            {
                return false;
            }
            code = (code << 6U) | (next & 63U);
        }
        if (code < minimum || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU) ||
            code < 32U || (code >= 127U && code <= 159U) ||
            (filename && (code == '/' || code == '\\')) || ++count > (filename ? 32U : 48U))
        {
            return false;
        }
    }
    return true;
}

bool command_library_name_valid(const char *name, size_t size)
{
    return label_valid(name, size, true);
}

static bool newer(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < 0x80000000UL;
}

static uint32_t slot_address(uint32_t id, uint32_t copy)
{
    return (id < COMMAND_LIBRARY_SLOTS ? SPI_STORE_BASE :
            id < 2U * COMMAND_LIBRARY_SLOTS ? STORE_BASE : UART_STORE_BASE) +
           ((id % COMMAND_LIBRARY_SLOTS) * 2U + copy) * SECTOR_BYTES;
}

/* UART settings use hz=baud, mode=parity, lsb=7-bit, cs_high=two stops,
 * address=line ending. The v5 record size and existing partitions stay compatible. */
static bool uart_settings_valid(const store_record_t *record)
{
    static const uint32_t rates[] = {300U, 600U, 1200U, 2400U, 4800U, 9600U,
        19200U, 38400U, 57600U, 115200U, 230400U, 460800U, 500000U, 921600U, 1000000U};
    size_t i;
    for (i = 0U; i < sizeof(rates) / sizeof(rates[0]); ++i)
    {
        if (record->hz == rates[i])
        {
            return record->mode <= 2U && record->lsb <= 1U && record->cs_high <= 1U &&
                   record->ascii <= 1U && record->address <= 3U &&
                   (record->lsb == 0U || record->mode != 0U);
        }
    }
    return false;
}

static bool valid_record(const store_record_t *record)
{
    uint32_t i;
    if ((record->magic != STORE_MAGIC) || (record->version < 1U || record->version > 5U) ||
        (record->commit != STORE_COMMIT) || (record->payload_size != record_bytes(record) - 24U) ||
        (record->crc != record_crc(record)) || (record->mode > 3U) || (record->lsb > 1U) ||
        (record->cs_high > 1U) || (record->ascii > 1U))
    {
        return false;
    }
    if (record->version >= 5U && record->bus == 3U)
    {
        if (!uart_settings_valid(record))
        {
            return false;
        }
    }
    else if (record->version >= 3U && record->bus == 1U)
    {
        i2c_command_t command = {0};
        command.hz = record->hz;
        command.address = record->address;
        command.operation = record->operation;
        command.read_size = record->read_size;
        command.size = command.operation == 1U ? 0U : 1U;
        command.ascii = record->ascii;
        if (!i2c_command_valid(&command))
        {
            return false;
        }
    }
    else if ((record->version >= 3U && record->bus != 2U) || !spi_stored_hz_valid(record->hz))
    {
        return false;
    }
    if (record->version >= 2U &&
        (record->deleted > 1U || !command_library_name_valid(record->name, record->name_size)))
    {
        return false;
    }
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        if (record->lengths[i] > COMMAND_STORE_TEXT_MAX ||
            (record->version >= 4U &&
             ((record->version == 4U && record->command_name_sizes[i] > 128U) ||
              !label_valid((const char *)record->command_names +
                               i * (record->version == 4U ? 128U : COMMAND_STORE_NAME_BYTES),
                           record->command_name_sizes[i], false))))
        {
            return false;
        }
    }
    return true;
}

static bool supported_flash(void)
{
    return (jedec[0] == 0xEFU) && ((jedec[1] == 0x40U) || (jedec[1] == 0x70U)) &&
           (jedec[2] == 0x17U);
}

static void defaults(void)
{
    memset(&current, 0, sizeof(current));
    current.hz = BOARD_SPI_DEFAULT_HZ;
    current.bus = 2U;
    selected_id = COMMAND_LIBRARY_NEW;
    has_saved = false;
}

static void migration_step(void);

static void scan_step(void)
{
    uint8_t bytes[256];
    const uint32_t id = scan_sector / 2U;
    const uint32_t copy = scan_sector % 2U;
    library_entry_t *entry = &catalog[id];
    size_t i;
    size_t count;
    if (scan_offset == 0U)
    {
        memset(&candidate, 0xFF, sizeof(candidate));
        scan_blank = true;
    }
    if (!addressed(0x03U, slot_address(id, copy) + scan_offset, NULL, bytes, sizeof(bytes)))
    {
        scanning = false;
        last_error = STORE_IO;
        return;
    }
    for (i = 0U; i < sizeof(bytes); ++i)
    {
        scan_blank = scan_blank && bytes[i] == 0xFFU;
    }
    if (scan_offset < sizeof(candidate))
    {
        count = sizeof(candidate) - scan_offset;
        if (count > sizeof(bytes))
        {
            count = sizeof(bytes);
        }
        memcpy((uint8_t *)&candidate + scan_offset, bytes, count);
    }
    scan_offset += sizeof(bytes);
    if (scan_offset < SECTOR_BYTES)
    {
        return;
    }
    if ((candidate.version >= 2U || id == COMMAND_LIBRARY_SLOTS - 1U) &&
        (id < COMMAND_LIBRARY_SLOTS || (candidate.version >= 3U &&
         candidate.bus == (id < 2U * COMMAND_LIBRARY_SLOTS ? 1U : 3U))) &&
        valid_record(&candidate))
    {
        normalize(&candidate);
        if (entry->kind == 0U || entry->kind == 3U || newer(candidate.sequence, entry->sequence))
        {
            entry->kind = candidate.deleted != 0U ? 2U : 1U;
            entry->copy = copy;
            entry->sequence = candidate.sequence;
            entry->bus = candidate.bus;
            entry->name_size = candidate.name_size;
            memcpy(entry->name, candidate.name, candidate.name_size);
        }
        if (newer(candidate.sequence, generation))
        {
            generation = candidate.sequence;
        }
        if (candidate.deleted == 0U && (!has_saved || newer(candidate.sequence, current.sequence)))
        {
            current = candidate;
            selected_id = id;
            has_saved = true;
        }
    }
    else if (!scan_blank && entry->kind == 0U)
    {
        entry->kind = 3U;
    }
    ++scan_sector;
    scan_offset = 0U;
    if (copy == 1U && has_saved && selected_id == id && entry->kind == 2U)
    {
        defaults(); /* The newer tombstone hides the old live copy. */
    }
    if (scan_sector == COMMAND_LIBRARY_TOTAL * 2U)
    {
        scanning = false;
        migrating = true;
        migration_step();
    }
}

static void system_load(void)
{
    uint8_t bytes[256];
    uint32_t copy;
    uint32_t offset;
    system_record_t record;
    for (copy = 0U; copy < 2U; ++copy)
    {
        bool blank = true;
        memset(&record, 0xFF, sizeof(record));
        for (offset = 0U; offset < SECTOR_BYTES; offset += sizeof(bytes))
        {
            uint32_t i;
            if (!addressed(0x03U, SYSTEM_BASE + copy * SECTOR_BYTES + offset, NULL, bytes,
                           sizeof(bytes)))
            {
                system_error = STORE_IO;
                return;
            }
            if (offset == 0U)
            {
                memcpy(&record, bytes, sizeof(record));
            }
            for (i = 0U; i < sizeof(bytes); ++i)
            {
                blank = blank && bytes[i] == 0xFFU;
            }
        }
        system_reusable[copy] = blank || (record.magic == SYSTEM_MAGIC && record.version == 1U &&
                                          record.payload_size == sizeof(record) - 24U);
        if (record.magic == SYSTEM_MAGIC && record.version == 1U && record.commit == STORE_COMMIT &&
            record.payload_size == sizeof(record) - 24U &&
            record.crc == block_crc(&record, sizeof(record)) &&
            bench_settings_valid(&record.settings) &&
            (system_current.version == 0U || newer(record.sequence, system_current.sequence)))
        {
            system_current = record;
            system_copy = copy;
        }
    }
    system_error = system_reusable[0] || system_reusable[1] ? STORE_OK : STORE_FOREIGN_DATA;
}

void command_store_init(void)
{
    const uint8_t read_id = 0x9FU;
    uint8_t value;
    uint32_t start;

    defaults();
    memset(&system_current, 0, sizeof(system_current));
    bench_settings_defaults(&system_current.settings);
    system_reusable[0] = false;
    system_reusable[1] = false;
    system_copy = 0U;
    system_job = false;
    job_bytes = sizeof(candidate);
    system_error = STORE_NO_FLASH;
    memset(catalog, 0, sizeof(catalog));
    scanning = false;
    migrating = false;
    generation = 0U;
    scan_sector = 0U;
    scan_offset = 0U;
    memset(jedec, 0, sizeof(jedec));
    external_lock = false;
    present = false;
    ready = false;
    has_saved = false;
    save_ok = false;
    job = JOB_IDLE;
    last_error = STORE_NO_FLASH;
    if (!flash_command(0xABU))
    {
        return;
    }
    board_delay(1U);
    /* A MCU-only reset can leave an erase active; JEDEC reads are then ignored. */
    start = board_millis();
    do
    {
        if (!status(&value))
        {
            last_error = STORE_IO;
            return;
        }
        if (((value & 1U) == 0U) || (value == 0xFFU))
        {
            break;
        }
        board_delay(1U);
    } while ((uint32_t)(board_millis() - start) < 1000U);
    if (!board_flash_transfer(&read_id, 1U, NULL, jedec, sizeof(jedec)))
    {
        return;
    }
    present = (jedec[0] != 0U) && (jedec[0] != 0xFFU);
    if (!present)
    {
        return;
    }
    if (!supported_flash())
    {
        last_error = STORE_UNSUPPORTED;
        return;
    }
    if ((value & 1U) != 0U)
    {
        last_error = STORE_TIMEOUT;
        return;
    }
    system_load();
    scanning = true;
    last_error = STORE_OK;
}

static bool parse_snapshot(const char *body, size_t size)
{
    char command_text[1024];
    spi_command_t command;
    i2c_command_t i2c;
    const bool is_uart = size >= 6U && memcmp(body, "bus=3 ", 6U) == 0;
    const bool is_i2c = size >= 6U && memcmp(body, "bus=1 ", 6U) == 0;
    size_t header = 0U;
    size_t offset;
    uint32_t i;
    while ((header < size) && (body[header] != '\n'))
    {
        ++header;
    }
    if ((header == size) || (header > 127U))
    {
        return false;
    }
    memcpy(command_text, body, header + 1U);
    memcpy(command_text + header + 1U, "00", 2U);
    memset(&candidate, 0, sizeof(candidate));
    if (is_uart)
    {
        unsigned int baud, parity, seven, stops, ascii, eol;
        int consumed = 0;
        command_text[header] = '\0';
        if (sscanf(command_text, "bus=3 baud=%u parity=%u seven=%u stops=%u ascii=%u eol=%u%n",
                   &baud, &parity, &seven, &stops, &ascii, &eol, &consumed) != 6 ||
            consumed != (int)header)
        {
            return false;
        }
        candidate.bus = 3U;
        candidate.hz = baud;
        candidate.mode = parity;
        candidate.lsb = seven;
        candidate.cs_high = stops;
        candidate.ascii = ascii;
        candidate.address = eol;
        if (!uart_settings_valid(&candidate))
        {
            return false;
        }
    }
    else if (is_i2c)
    {
        if (!i2c_command_parse(command_text, header + 1U, &i2c) &&
            !i2c_command_parse(command_text, header + 3U, &i2c))
        {
            return false;
        }
        candidate.bus = 1U;
        candidate.hz = i2c.hz;
        candidate.ascii = i2c.ascii;
        candidate.address = i2c.address;
        candidate.operation = i2c.operation;
        candidate.read_size = i2c.read_size;
    }
    else
    {
        if (!spi_stored_command_parse(command_text, header + 3U, &command))
        {
            return false;
        }
        candidate.bus = 2U;
        candidate.hz = command.hz;
        candidate.mode = command.mode;
        candidate.lsb = command.lsb_first;
        candidate.cs_high = command.cs_high;
        candidate.ascii = command.ascii;
    }
    offset = header + 1U;
    if ((size - offset < 8U) || (memcmp(body + offset, "lengths=", 8U) != 0))
    {
        return false;
    }
    offset += 8U;
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        uint32_t length = 0U;
        size_t digits = 0U;
        while ((offset < size) && (body[offset] >= '0') && (body[offset] <= '9'))
        {
            length = length * 10U + (uint32_t)(body[offset++] - '0');
            if (length > COMMAND_STORE_TEXT_MAX)
            {
                return false;
            }
            ++digits;
        }
        if ((digits == 0U) || (offset == size) || (body[offset++] != (i == 3U ? '\n' : ',')))
        {
            return false;
        }
        candidate.lengths[i] = length;
    }
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        const size_t length = candidate.lengths[i];
        if (length > size - offset)
        {
            return false;
        }
        memcpy(candidate.texts[i], body + offset, length);
        if (length != 0U)
        {
            memcpy(command_text + header + 1U, body + offset, length);
            if (is_uart)
            {
                const int count = snprintf(command_text, sizeof(command_text),
                    "bus=2 mode=0 hz=750000 lsb=0 cs_high=0 format=%s\n",
                    candidate.ascii != 0U ? "ascii" : "hex");
                const size_t prefix = count > 0 ? (size_t)count : 0U;
                memcpy(command_text + prefix, body + offset, length);
                if (prefix == 0U || !spi_stored_command_parse(command_text, prefix + length, &command))
                {
                    return false;
                }
            }
            else if (is_i2c ? !i2c_command_parse(command_text, header + 1U + length, &i2c)
                       : !spi_stored_command_parse(command_text, header + 1U + length, &command))
            {
                return false;
            }
        }
        offset += length;
    }
    if (offset == size)
    {
        return true; /* Legacy clients omit names. */
    }
    if (size - offset < 7U || memcmp(body + offset, "\nnames=", 7U) != 0)
    {
        return false;
    }
    offset += 7U;
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        uint32_t length = 0U;
        size_t digits = 0U;
        while (offset < size && body[offset] >= '0' && body[offset] <= '9')
        {
            length = length * 10U + (uint32_t)(body[offset++] - '0');
            if (length > COMMAND_STORE_NAME_BYTES)
            {
                return false;
            }
            ++digits;
        }
        if (digits == 0U || offset == size || body[offset++] != (i == 3U ? '\n' : ','))
        {
            return false;
        }
        candidate.command_name_sizes[i] = length;
    }
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        const size_t length = candidate.command_name_sizes[i];
        if (length > size - offset || !label_valid(body + offset, length, false))
        {
            return false;
        }
        memcpy(candidate.command_names[i], body + offset, length);
        offset += length;
    }
    return offset == size;
}

static store_error_t available(void)
{
    if (job != JOB_IDLE || scanning || migrating || external_lock)
    {
        return STORE_BUSY;
    }
    if (!present)
    {
        return STORE_NO_FLASH;
    }
    return ready ? STORE_OK : last_error;
}

static void begin_write(uint32_t id)
{
    system_job = false;
    job_bytes = sizeof(candidate);
    candidate.magic = STORE_MAGIC;
    candidate.version = 5U;
    candidate.sequence = generation + 1U;
    candidate.payload_size = sizeof(candidate) - 24U;
    candidate.commit = UINT32_MAX;
    candidate.crc = record_crc(&candidate);
    target_id = id;
    target_address = slot_address(id, catalog[id].kind == 0U ? 0U : 1U - catalog[id].copy);
    position = 0U;
    last_error = STORE_OK;
    save_ok = false;
    operation_start = board_millis();
    job = JOB_ERASE;
}

void command_system_get(bench_settings_t *settings, uint32_t *sequence, bool *busy,
                        store_error_t *error)
{
    *settings = system_current.settings;
    *sequence = system_current.sequence;
    *busy = job != JOB_IDLE || scanning || migrating || external_lock;
    *error = system_error;
}

bool command_system_saved(void)
{
    return system_current.version == 1U && system_current.commit == STORE_COMMIT;
}

store_error_t command_system_save(const bench_settings_t *settings, uint32_t sequence)
{
    system_record_t record;
    uint32_t copy;
    const store_error_t error = available();
    if (error != STORE_OK)
    {
        return error;
    }
    if (!bench_settings_valid(settings))
    {
        return STORE_INVALID;
    }
    if (sequence != system_current.sequence)
    {
        return STORE_EXISTS;
    }
    copy = system_current.version != 0U ? 1U - system_copy : (system_reusable[0] ? 0U : 1U);
    if (!system_reusable[copy])
    {
        return STORE_FOREIGN_DATA;
    }
    memset(&record, 0, sizeof(record));
    record.magic = SYSTEM_MAGIC;
    record.version = 1U;
    record.sequence = sequence + 1U;
    record.payload_size = sizeof(record) - 24U;
    record.commit = UINT32_MAX;
    record.settings = *settings;
    record.crc = block_crc(&record, sizeof(record));
    memset(&candidate, 0, sizeof(candidate));
    memcpy(&candidate, &record, sizeof(record));
    system_job = true;
    job_bytes = sizeof(record);
    target_address = SYSTEM_BASE + copy * SECTOR_BYTES;
    position = 0U;
    operation_start = board_millis();
    system_error = STORE_OK;
    job = JOB_ERASE;
    return STORE_OK;
}

store_error_t command_library_load(uint32_t id)
{
    const store_error_t error = available();
    if (error != STORE_OK)
    {
        return error;
    }
    if (id >= COMMAND_LIBRARY_TOTAL || catalog[id].kind != 1U)
    {
        return STORE_NOT_FOUND;
    }
    if (!addressed(0x03U, slot_address(id, catalog[id].copy), NULL, (uint8_t *)&candidate,
                   sizeof(candidate)))
    {
        return STORE_IO;
    }
    if (!valid_record(&candidate))
    {
        return STORE_VERIFY;
    }
    normalize(&candidate);
    current = candidate;
    selected_id = id;
    has_saved = true;
    last_error = STORE_OK;
    return STORE_OK;
}

store_error_t command_library_save(uint32_t id, const char *name, size_t name_size,
                                   const char *body, size_t size)
{
    const store_error_t error = available();
    uint32_t i;
    if (error != STORE_OK)
    {
        return error;
    }
    if (!command_library_name_valid(name, name_size))
    {
        return STORE_NAME;
    }
    if (!parse_snapshot(body, size))
    {
        return STORE_INVALID;
    }
    {
        const uint32_t first = candidate.bus == 2U ? 0U :
                               candidate.bus == 1U ? COMMAND_LIBRARY_SLOTS : 2U * COMMAND_LIBRARY_SLOTS;
        const uint32_t end = first + COMMAND_LIBRARY_SLOTS;
        if (id != COMMAND_LIBRARY_NEW && (id < first || id >= end))
        {
            return STORE_INVALID;
        }
        if (id < COMMAND_LIBRARY_TOTAL && catalog[id].kind == 3U)
        {
            return STORE_FOREIGN_DATA;
        }
        for (i = first; i < end; ++i)
        {
            if (i != id && catalog[i].kind == 1U && catalog[i].bus == candidate.bus &&
                catalog[i].name_size == name_size && memcmp(catalog[i].name, name, name_size) == 0)
            {
                return STORE_EXISTS;
            }
        }
        if (id == COMMAND_LIBRARY_NEW)
        {
            for (id = first; id < end; ++id)
            {
                if (catalog[id].kind == 0U || catalog[id].kind == 2U)
                {
                    break;
                }
            }
            if (id == end)
            {
                return STORE_FULL;
            }
        }
    }
    candidate.name_size = (uint32_t)name_size;
    memcpy(candidate.name, name, name_size);
    begin_write(id);
    return STORE_OK;
}

store_error_t command_store_save(const char *body, size_t size)
{
    const uint32_t id =
        has_saved ? selected_id
                  : (size >= 6U && memcmp(body, "bus=1 ", 6U) == 0 ? COMMAND_LIBRARY_NEW
                                                                   : COMMAND_LIBRARY_SLOTS - 1U);
    const char *name = has_saved ? current.name : "Default";
    const size_t name_size = has_saved ? current.name_size : 7U;
    return command_library_save(id, name, name_size, body, size);
}

store_error_t command_library_delete(uint32_t id)
{
    const store_error_t error = available();
    if (error != STORE_OK)
    {
        return error;
    }
    if (id >= COMMAND_LIBRARY_TOTAL || catalog[id].kind != 1U)
    {
        return STORE_NOT_FOUND;
    }
    if (!addressed(0x03U, slot_address(id, catalog[id].copy), NULL, (uint8_t *)&candidate,
                   sizeof(candidate)))
    {
        return STORE_IO;
    }
    if (!valid_record(&candidate))
    {
        return STORE_VERIFY;
    }
    normalize(&candidate);
    candidate.deleted = 1U;
    begin_write(id);
    return STORE_OK;
}

/* Resume migration from catalog evidence, without a separate fragile progress record.
 * First commit the I2C copy, then commit a tombstone in its legacy SPI pair. */
static void migration_step(void)
{
    uint32_t source;
    uint32_t target;
    uint32_t free_id;
    uint32_t source_sequence;
    uint32_t latest = COMMAND_LIBRARY_NEW;
    for (source = 0U; source < COMMAND_LIBRARY_SLOTS; ++source)
    {
        if (catalog[source].kind != 1U || catalog[source].bus != 1U)
        {
            continue;
        }
        free_id = COMMAND_LIBRARY_NEW;
        for (target = COMMAND_LIBRARY_SLOTS; target < 2U * COMMAND_LIBRARY_SLOTS; ++target)
        {
            if (catalog[target].kind == 1U && catalog[target].bus == 1U &&
                catalog[target].name_size == catalog[source].name_size &&
                memcmp(catalog[target].name, catalog[source].name, catalog[source].name_size) == 0)
            {
                break;
            }
            if (free_id == COMMAND_LIBRARY_NEW &&
                (catalog[target].kind == 0U || catalog[target].kind == 2U))
            {
                free_id = target;
            }
        }
        if (!addressed(0x03U, slot_address(source, catalog[source].copy), NULL,
                       (uint8_t *)&candidate, sizeof(candidate)) ||
            !valid_record(&candidate))
        {
            migrating = false;
            last_error = STORE_VERIFY;
            return;
        }
        normalize(&candidate);
        source_sequence = candidate.sequence;
        if (target < 2U * COMMAND_LIBRARY_SLOTS &&
            !newer(catalog[source].sequence, catalog[target].sequence))
        {
            candidate.deleted = 1U;
            begin_write(source);
        }
        else if (target < 2U * COMMAND_LIBRARY_SLOTS || free_id < COMMAND_LIBRARY_TOTAL)
        {
            begin_write(target < 2U * COMMAND_LIBRARY_SLOTS ? target : free_id);
            candidate.sequence = source_sequence;
            candidate.crc = record_crc(&candidate);
        }
        else
        {
            migrating = false;
            last_error = STORE_FULL;
        }
        return;
    }
    migrating = false;
    ready = true;
    last_error = STORE_OK;
    for (source = 0U; source < COMMAND_LIBRARY_TOTAL; ++source)
    {
        if (catalog[source].kind == 1U &&
            (latest == COMMAND_LIBRARY_NEW ||
             newer(catalog[source].sequence, catalog[latest].sequence)))
        {
            latest = source;
        }
    }
    if (latest == COMMAND_LIBRARY_NEW)
    {
        defaults();
    }
    else
    {
        last_error = command_library_load(latest);
    }
    save_ok = has_saved;
}

static void fail_job(store_error_t error)
{
    if (system_job)
    {
        system_error = error;
        system_job = false;
        job = JOB_IDLE;
        return;
    }
    last_error = error;
    migrating = false;
    save_ok = false;
    job = JOB_IDLE;
}

void command_store_poll(void)
{
    uint8_t value;
    uint8_t bytes[256];
    if (external_lock)
    {
        return;
    }
    if (scanning)
    {
        scan_step();
        return;
    }
    if (job == JOB_IDLE)
    {
        if (migrating)
        {
            migration_step();
        }
        return;
    }
    if (!status(&value))
    {
        fail_job(STORE_IO);
        return;
    }
    if ((value & 1U) != 0U)
    {
        if ((uint32_t)(board_millis() - operation_start) > 1000U)
        {
            fail_job(STORE_TIMEOUT);
        }
        return;
    }
    switch (job)
    {
    case JOB_ERASE:
        if (!write_enable() || !addressed(0x20U, target_address, NULL, NULL, 0U))
        {
            fail_job(STORE_IO);
            return;
        }
        operation_start = board_millis();
        job = JOB_ERASE_WAIT;
        break;
    case JOB_ERASE_WAIT:
        job = JOB_PROGRAM;
        break;
    case JOB_PROGRAM:
        pending_size = job_bytes - position;
        if (pending_size > 256U)
        {
            pending_size = 256U;
        }
        if (!write_enable() ||
            !addressed(0x02U, target_address + position, (const uint8_t *)&candidate + position,
                       NULL, pending_size))
        {
            fail_job(STORE_IO);
            return;
        }
        operation_start = board_millis();
        job = JOB_PROGRAM_WAIT;
        break;
    case JOB_PROGRAM_WAIT:
        position += pending_size;
        if (position == job_bytes)
        {
            position = 0U;
            job = JOB_VERIFY;
        }
        else
        {
            job = JOB_PROGRAM;
        }
        break;
    case JOB_VERIFY:
        pending_size = job_bytes - position;
        if (pending_size > sizeof(bytes))
        {
            pending_size = sizeof(bytes);
        }
        if (!addressed(0x03U, target_address + position, NULL, bytes, pending_size))
        {
            fail_job(STORE_IO);
            return;
        }
        if (memcmp(bytes, (const uint8_t *)&candidate + position, pending_size) != 0)
        {
            fail_job(STORE_VERIFY);
            return;
        }
        position += pending_size;
        if (position == job_bytes)
        {
            job = JOB_COMMIT;
        }
        break;
    case JOB_COMMIT:
    {
        const uint32_t commit = STORE_COMMIT;
        if (!write_enable() ||
            !addressed(0x02U, target_address + 20U, (const uint8_t *)&commit, NULL, sizeof(commit)))
        {
            fail_job(STORE_IO);
            return;
        }
        operation_start = board_millis();
        job = JOB_COMMIT_WAIT;
    }
    break;
    case JOB_COMMIT_WAIT:
    {
        uint32_t commit = 0U;
        library_entry_t *entry;
        if (!addressed(0x03U, target_address + 20U, NULL, (uint8_t *)&commit, sizeof(commit)) ||
            (commit != STORE_COMMIT))
        {
            fail_job(STORE_VERIFY);
            return;
        }
        candidate.commit = STORE_COMMIT;
        if (system_job)
        {
            memcpy(&system_current, &candidate, sizeof(system_current));
            system_copy = (target_address - SYSTEM_BASE) / SECTOR_BYTES;
            system_reusable[system_copy] = true;
            system_error = STORE_OK;
            system_job = false;
            job = JOB_IDLE;
            return;
        }
        entry = &catalog[target_id];
        entry->kind = candidate.deleted != 0U ? 2U : 1U;
        entry->sequence = candidate.sequence;
        entry->copy = (target_address - slot_address(target_id, 0U)) / SECTOR_BYTES;
        entry->bus = candidate.bus;
        entry->name_size = candidate.name_size;
        memcpy(entry->name, candidate.name, candidate.name_size);
        if (newer(candidate.sequence, generation))
        {
            generation = candidate.sequence;
        }
        if (candidate.deleted != 0U)
        {
            if (has_saved && selected_id == target_id)
            {
                defaults();
            }
        }
        else
        {
            current = candidate;
            selected_id = target_id;
            has_saved = true;
        }
        save_ok = true;
        last_error = STORE_OK;
        job = JOB_IDLE;
    }
    break;
    case JOB_IDLE:
        break;
    }
}

typedef struct
{
    char *text;
    size_t capacity;
    size_t used;
    uint32_t ok;
} json_writer_t;

static void put_char(json_writer_t *writer, char c)
{
    if (writer->used + 1U >= writer->capacity)
    {
        writer->ok = 0U;
        return;
    }
    writer->text[writer->used++] = c;
}

static void put_text(json_writer_t *writer, const char *text)
{
    while (*text != '\0')
    {
        put_char(writer, *text++);
    }
}

static void put_uint(json_writer_t *writer, uint32_t value)
{
    char digits[10];
    size_t size = 0U;
    do
    {
        digits[size++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (size != 0U)
    {
        put_char(writer, digits[--size]);
    }
}

static void put_string(json_writer_t *writer, const char *text, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    put_char(writer, '"');
    for (i = 0U; i < size; ++i)
    {
        const uint8_t c = (uint8_t)text[i];
        if ((c == '"') || (c == '\\'))
        {
            put_char(writer, '\\');
            put_char(writer, (char)c);
        }
        else if ((c < 32U) || (c == 127U))
        {
            put_text(writer, "\\u00");
            put_char(writer, hex[c >> 4U]);
            put_char(writer, hex[c & 15U]);
        }
        else
        {
            put_char(writer, (char)c);
        }
    }
    put_char(writer, '"');
}

size_t command_storage_json(char *json, size_t capacity)
{
    uint32_t used[3] = {0U, 0U, 0U};
    uint32_t blocked[3] = {0U, 0U, 0U};
    uint32_t i;
    int count;
    for (i = 0U; i < COMMAND_LIBRARY_TOTAL; ++i)
    {
        const uint32_t bank = i / COMMAND_LIBRARY_SLOTS;
        used[bank] += catalog[i].kind == 1U ? 1U : 0U;
        blocked[bank] += catalog[i].kind == 3U ? 1U : 0U;
    }
    count = snprintf(
        json, capacity,
        "{\"present\":%s,\"supported\":%s,\"complete\":%s,\"busy\":%s,"
        "\"capacity\":%lu,\"sector_bytes\":%lu,\"slot_bytes\":%lu,"
        "\"bank_bytes\":%lu,\"slots\":%u,\"system_bytes\":%lu,"
        "\"unallocated_bytes\":%lu,\"help_bytes\":%lu,\"system_sequence\":%lu,"
        "\"record_version\":5,\"record_bytes\":%lu,"
        "\"jedec\":\"%02X%02X%02X\","
        "\"spi\":{\"used\":%lu,\"blocked\":%lu,\"free\":%lu},"
        "\"i2c\":{\"used\":%lu,\"blocked\":%lu,\"free\":%lu},\"usart\":{\"used\":%lu,\"blocked\":%lu,\"free\":%lu}}",
        present ? "true" : "false", supported_flash() ? "true" : "false",
        ready && !scanning && !migrating ? "true" : "false",
        (job != JOB_IDLE || external_lock) ? "true" : "false",
        (unsigned long)(supported_flash() ? FLASH_BYTES : 0U), (unsigned long)SECTOR_BYTES,
        (unsigned long)(2U * SECTOR_BYTES),
        (unsigned long)(COMMAND_LIBRARY_SLOTS * 2U * SECTOR_BYTES), COMMAND_LIBRARY_SLOTS,
        (unsigned long)(2U * SECTOR_BYTES), (unsigned long)UART_STORE_BASE,
        (unsigned long)FLASH_HELP_BYTES, (unsigned long)system_current.sequence,
        (unsigned long)sizeof(store_record_t), (unsigned int)jedec[0], (unsigned int)jedec[1],
        (unsigned int)jedec[2], (unsigned long)used[0], (unsigned long)blocked[0],
        (unsigned long)(COMMAND_LIBRARY_SLOTS - used[0] - blocked[0]), (unsigned long)used[1],
        (unsigned long)blocked[1], (unsigned long)(COMMAND_LIBRARY_SLOTS - used[1] - blocked[1]),
        (unsigned long)used[2], (unsigned long)blocked[2],
        (unsigned long)(COMMAND_LIBRARY_SLOTS - used[2] - blocked[2]));
    return count > 0 && (size_t)count < capacity ? (size_t)count : 0U;
}

size_t command_store_json(char *json, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    json_writer_t writer = {json, capacity, 0U, 1U};
    uint32_t i;
    put_text(&writer, "{\"scanning\":");
    put_text(&writer, (scanning || migrating) ? "true" : "false");
    put_text(&writer, ",\"migrating\":");
    put_text(&writer, migrating ? "true" : "false");
    put_text(&writer, ",\"scan\":");
    put_uint(&writer, scan_sector / 2U);
    put_text(&writer, ",\"id\":");
    put_uint(&writer, has_saved ? selected_id : COMMAND_LIBRARY_NEW);
    put_text(&writer, ",\"name\":");
    put_string(&writer, has_saved ? current.name : "", has_saved ? current.name_size : 0U);
    put_text(&writer, ",\"present\":");
    put_text(&writer, present ? "true" : "false");
    put_text(&writer, ",\"ready\":");
    put_text(&writer, ready ? "true" : "false");
    put_text(&writer, ",\"busy\":");
    put_text(&writer, (job != JOB_IDLE || external_lock) ? "true" : "false");
    put_text(&writer, ",\"has_saved\":");
    put_text(&writer, has_saved ? "true" : "false");
    put_text(&writer, ",\"save_ok\":");
    put_text(&writer, save_ok ? "true" : "false");
    put_text(&writer, ",\"error\":");
    put_uint(&writer, (uint32_t)last_error);
    put_text(&writer, ",\"sequence\":");
    put_uint(&writer, has_saved ? current.sequence : 0U);
    put_text(&writer, ",\"capacity\":");
    put_uint(&writer, supported_flash() ? FLASH_BYTES : 0U);
    put_text(&writer, ",\"reserved_start\":");
    put_uint(&writer, STORE_BASE);
    put_text(&writer, ",\"jedec\":\"");
    for (i = 0U; i < 3U; ++i)
    {
        put_char(&writer, hex[jedec[i] >> 4U]);
        put_char(&writer, hex[jedec[i] & 15U]);
    }
    put_text(&writer, "\",\"settings\":{\"mode\":");
    put_uint(&writer, current.mode);
    put_text(&writer, ",\"hz\":");
    put_uint(&writer, current.hz);
    put_text(&writer, ",\"lsb\":");
    put_uint(&writer, current.lsb);
    put_text(&writer, ",\"cs_high\":");
    put_uint(&writer, current.cs_high);
    put_text(&writer, ",\"format\":");
    put_text(&writer, current.ascii != 0U ? "\"ascii\"" : "\"hex\"");
    put_text(&writer, ",\"bus\":");
    put_uint(&writer, current.bus);
    put_text(&writer, ",\"address\":");
    put_uint(&writer, current.address);
    put_text(&writer, ",\"operation\":");
    put_uint(&writer, current.operation);
    put_text(&writer, ",\"read_size\":");
    put_uint(&writer, current.read_size);
    put_text(&writer, "},\"commands\":[");
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        if (i != 0U)
        {
            put_char(&writer, ',');
        }
        put_string(&writer, current.texts[i], current.lengths[i]);
    }
    put_text(&writer, "],\"command_names\":[");
    for (i = 0U; i < COMMAND_STORE_COUNT; ++i)
    {
        if (i != 0U)
        {
            put_char(&writer, ',');
        }
        put_string(&writer, current.command_names[i], current.command_name_sizes[i]);
    }
    put_text(&writer, "]}");
    if (writer.ok == 0U || writer.used >= capacity)
    {
        return 0U;
    }
    json[writer.used] = '\0';
    return writer.used;
}

size_t command_library_json(char *json, size_t capacity, uint32_t offset)
{
    json_writer_t writer = {json, capacity, 0U, 1U};
    uint32_t i;
    uint32_t used = 0U;
    uint32_t blocked = 0U;
    uint32_t count = 0U;
    uint32_t end = offset + 8U;
    if (offset >= COMMAND_LIBRARY_TOTAL)
    {
        return 0U;
    }
    if (end > COMMAND_LIBRARY_TOTAL)
    {
        end = COMMAND_LIBRARY_TOTAL;
    }
    for (i = 0U; i < COMMAND_LIBRARY_TOTAL; ++i)
    {
        used += catalog[i].kind == 1U ? 1U : 0U;
        blocked += catalog[i].kind == 3U ? 1U : 0U;
    }
    put_text(&writer, "{\"scanning\":");
    put_text(&writer, (scanning || migrating) ? "true" : "false");
    put_text(&writer, ",\"used\":");
    put_uint(&writer, used);
    put_text(&writer, ",\"blocked\":");
    put_uint(&writer, blocked);
    put_text(&writer, ",\"blocked_spi\":");
    blocked = 0U;
    for (i = 0U; i < COMMAND_LIBRARY_SLOTS; ++i)
    {
        blocked += catalog[i].kind == 3U ? 1U : 0U;
    }
    put_uint(&writer, blocked);
    put_text(&writer, ",\"blocked_i2c\":");
    blocked = 0U;
    for (i = COMMAND_LIBRARY_SLOTS; i < 2U * COMMAND_LIBRARY_SLOTS; ++i)
    {
        blocked += catalog[i].kind == 3U ? 1U : 0U;
    }
    put_uint(&writer, blocked);
    put_text(&writer, ",\"blocked_usart\":");
    blocked = 0U;
    for (i = 2U * COMMAND_LIBRARY_SLOTS; i < COMMAND_LIBRARY_TOTAL; ++i)
    {
        blocked += catalog[i].kind == 3U ? 1U : 0U;
    }
    put_uint(&writer, blocked);
    put_text(&writer, ",\"next\":");
    if (end == COMMAND_LIBRARY_TOTAL)
    {
        put_text(&writer, "null");
    }
    else
    {
        put_uint(&writer, end);
    }
    put_text(&writer, ",\"sets\":[");
    if (!scanning)
    {
        for (i = offset; i < end; ++i)
        {
            if (catalog[i].kind != 1U)
            {
                continue;
            }
            if (count++ != 0U)
            {
                put_char(&writer, ',');
            }
            put_text(&writer, "{\"id\":");
            put_uint(&writer, i);
            put_text(&writer, ",\"name\":");
            put_string(&writer, catalog[i].name, catalog[i].name_size);
            put_text(&writer, ",\"bus\":");
            put_uint(&writer, catalog[i].bus);
            put_char(&writer, '}');
        }
    }
    put_text(&writer, "]}");
    if (writer.ok == 0U || writer.used >= capacity)
    {
        return 0U;
    }
    json[writer.used] = '\0';
    return writer.used;
}

store_error_t command_flash_claim(void)
{
    uint8_t value;
    const store_error_t error = available();
    if (error != STORE_OK)
    {
        return error;
    }
    if (!status(&value) || value == 0xFFU)
    {
        return STORE_IO;
    }
    if ((value & 1U) != 0U)
    {
        return STORE_BUSY;
    }
    external_lock = true;
    return STORE_OK;
}

void command_flash_release(void)
{
    external_lock = false;
}

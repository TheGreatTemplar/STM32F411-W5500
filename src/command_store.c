#include "command_store.h"
#include "board.h"
#include "i2c_console.h"
#include "spi_console.h"
#include <string.h>

#define FLASH_BYTES 8388608UL
#define SECTOR_BYTES 4096UL
#define STORE_BASE (FLASH_BYTES - COMMAND_LIBRARY_SLOTS * 2UL * SECTOR_BYTES)
#define STORE_MAGIC 0x34444D43UL
#define STORE_COMMIT 0xA55AC33CUL

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
} store_record_t;

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

static library_entry_t catalog[COMMAND_LIBRARY_SLOTS];
static bool scanning;
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
                                   : sizeof(*record);
}

static void normalize(store_record_t *record)
{
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
}

static uint32_t record_crc(const store_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = UINT32_MAX;
    size_t i;
    for (i = 0U; i < record_bytes(record); ++i)
    {
        if ((i < 16U) || (i >= 24U))
        {
            crc = crc_byte(crc, bytes[i]);
        }
    }
    return ~crc;
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

bool command_library_name_valid(const char *name, size_t size)
{
    size_t i = 0U;
    uint32_t count = 0U;
    if (size == 0U || size > COMMAND_LIBRARY_NAME_BYTES || name[0] == ' ' ||
        name[size - 1U] == ' ' || (size == 1U && name[0] == '.') ||
        (size == 2U && name[0] == '.' && name[1] == '.'))
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
            code < 32U || (code >= 127U && code <= 159U) || code == '/' || code == '\\' ||
            ++count > 32U)
        {
            return false;
        }
    }
    return true;
}

static bool newer(uint32_t a, uint32_t b)
{
    return a != b && (uint32_t)(a - b) < 0x80000000UL;
}

static uint32_t slot_address(uint32_t id, uint32_t copy)
{
    return STORE_BASE + (id * 2U + copy) * SECTOR_BYTES;
}

static bool valid_record(const store_record_t *record)
{
    uint32_t i;
    spi_clock_plan_t plan;
    if ((record->magic != STORE_MAGIC) || (record->version < 1U || record->version > 3U) ||
        (record->commit != STORE_COMMIT) || (record->payload_size != record_bytes(record) - 24U) ||
        (record->crc != record_crc(record)) || (record->mode > 3U) || (record->lsb > 1U) ||
        (record->cs_high > 1U) || (record->ascii > 1U))
    {
        return false;
    }
    if (record->version == 3U && record->bus == 1U)
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
    else if ((record->version == 3U && record->bus != 2U) || !spi_clock_plan(record->hz, &plan))
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
        if (record->lengths[i] > COMMAND_STORE_TEXT_MAX)
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
    current.hz = 1000000U;
    current.bus = 2U;
    selected_id = COMMAND_LIBRARY_SLOTS;
    has_saved = false;
}

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
    if (!addressed(0x03U, STORE_BASE + scan_sector * SECTOR_BYTES + scan_offset, NULL, bytes,
                   sizeof(bytes)))
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
    if ((candidate.version >= 2U || id == COMMAND_LIBRARY_SLOTS - 1U) && valid_record(&candidate))
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
    if (scan_sector == COMMAND_LIBRARY_SLOTS * 2U)
    {
        uint32_t latest = COMMAND_LIBRARY_SLOTS;
        for (i = 0U; i < COMMAND_LIBRARY_SLOTS; ++i)
        {
            if (catalog[i].kind == 1U && (latest == COMMAND_LIBRARY_SLOTS ||
                                          newer(catalog[i].sequence, catalog[latest].sequence)))
            {
                latest = (uint32_t)i;
            }
        }
        scanning = false;
        ready = true;
        last_error = STORE_OK;
        if (latest != COMMAND_LIBRARY_SLOTS)
        {
            last_error = command_library_load(latest);
        }
        else
        {
            defaults();
        }
        save_ok = has_saved;
    }
}

void command_store_init(void)
{
    const uint8_t read_id = 0x9FU;
    uint8_t value;
    uint32_t start;

    defaults();
    memset(catalog, 0, sizeof(catalog));
    scanning = false;
    generation = 0U;
    scan_sector = 0U;
    scan_offset = 0U;
    memset(jedec, 0, sizeof(jedec));
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
    scanning = true;
    last_error = STORE_OK;
}

static bool parse_snapshot(const char *body, size_t size)
{
    char command_text[1024];
    spi_command_t command;
    i2c_command_t i2c;
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
    if (is_i2c)
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
        if (!spi_command_parse(command_text, header + 3U, &command))
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
            if (is_i2c ? !i2c_command_parse(command_text, header + 1U + length, &i2c)
                       : !spi_command_parse(command_text, header + 1U + length, &command))
            {
                return false;
            }
        }
        offset += length;
    }
    return offset == size;
}

static store_error_t available(void)
{
    if (job != JOB_IDLE || scanning)
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
    candidate.magic = STORE_MAGIC;
    candidate.version = 3U;
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

store_error_t command_library_load(uint32_t id)
{
    const store_error_t error = available();
    if (error != STORE_OK)
    {
        return error;
    }
    if (id >= COMMAND_LIBRARY_SLOTS || catalog[id].kind != 1U)
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
    if (id > COMMAND_LIBRARY_SLOTS)
    {
        return STORE_NOT_FOUND;
    }
    if (id < COMMAND_LIBRARY_SLOTS && catalog[id].kind == 3U)
    {
        return STORE_FOREIGN_DATA;
    }
    for (i = 0U; i < COMMAND_LIBRARY_SLOTS; ++i)
    {
        if (i != id && catalog[i].kind == 1U && catalog[i].name_size == name_size &&
            memcmp(catalog[i].name, name, name_size) == 0)
        {
            return STORE_EXISTS;
        }
    }
    if (id == COMMAND_LIBRARY_SLOTS)
    {
        for (id = 0U; id < COMMAND_LIBRARY_SLOTS; ++id)
        {
            if (catalog[id].kind == 0U || catalog[id].kind == 2U)
            {
                break;
            }
        }
        if (id == COMMAND_LIBRARY_SLOTS)
        {
            return STORE_FULL;
        }
    }
    if (!parse_snapshot(body, size))
    {
        return STORE_INVALID;
    }
    candidate.name_size = (uint32_t)name_size;
    memcpy(candidate.name, name, name_size);
    begin_write(id);
    return STORE_OK;
}

store_error_t command_store_save(const char *body, size_t size)
{
    const uint32_t id = has_saved ? selected_id : COMMAND_LIBRARY_SLOTS - 1U;
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
    if (id >= COMMAND_LIBRARY_SLOTS || catalog[id].kind != 1U)
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

static void fail_job(store_error_t error)
{
    last_error = error;
    save_ok = false;
    job = JOB_IDLE;
}

void command_store_poll(void)
{
    uint8_t value;
    uint8_t bytes[256];
    if (scanning)
    {
        scan_step();
        return;
    }
    if (job == JOB_IDLE)
    {
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
        pending_size = sizeof(candidate) - position;
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
        if (position == sizeof(candidate))
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
        pending_size = sizeof(candidate) - position;
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
        if (position == sizeof(candidate))
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
        library_entry_t *entry = &catalog[target_id];
        if (!addressed(0x03U, target_address + 20U, NULL, (uint8_t *)&commit, sizeof(commit)) ||
            (commit != STORE_COMMIT))
        {
            fail_job(STORE_VERIFY);
            return;
        }
        candidate.commit = STORE_COMMIT;
        entry->kind = candidate.deleted != 0U ? 2U : 1U;
        entry->sequence = candidate.sequence;
        entry->copy = (target_address - slot_address(target_id, 0U)) / SECTOR_BYTES;
        entry->bus = candidate.bus;
        entry->name_size = candidate.name_size;
        memcpy(entry->name, candidate.name, candidate.name_size);
        generation = candidate.sequence;
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

size_t command_store_json(char *json, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    json_writer_t writer = {json, capacity, 0U, 1U};
    uint32_t i;
    put_text(&writer, "{\"scanning\":");
    put_text(&writer, scanning ? "true" : "false");
    put_text(&writer, ",\"scan\":");
    put_uint(&writer, scan_sector / 2U);
    put_text(&writer, ",\"id\":");
    put_uint(&writer, has_saved ? selected_id : COMMAND_LIBRARY_SLOTS);
    put_text(&writer, ",\"name\":");
    put_string(&writer, has_saved ? current.name : "", has_saved ? current.name_size : 0U);
    put_text(&writer, ",\"present\":");
    put_text(&writer, present ? "true" : "false");
    put_text(&writer, ",\"ready\":");
    put_text(&writer, ready ? "true" : "false");
    put_text(&writer, ",\"busy\":");
    put_text(&writer, job != JOB_IDLE ? "true" : "false");
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
    if (offset >= COMMAND_LIBRARY_SLOTS)
    {
        return 0U;
    }
    if (end > COMMAND_LIBRARY_SLOTS)
    {
        end = COMMAND_LIBRARY_SLOTS;
    }
    for (i = 0U; i < COMMAND_LIBRARY_SLOTS; ++i)
    {
        used += catalog[i].kind == 1U ? 1U : 0U;
        blocked += catalog[i].kind == 3U ? 1U : 0U;
    }
    put_text(&writer, "{\"scanning\":");
    put_text(&writer, scanning ? "true" : "false");
    put_text(&writer, ",\"used\":");
    put_uint(&writer, used);
    put_text(&writer, ",\"blocked\":");
    put_uint(&writer, blocked);
    put_text(&writer, ",\"next\":");
    if (end == COMMAND_LIBRARY_SLOTS)
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

#include "command_store.h"
#include "board.h"
#include "spi_console.h"
#include <string.h>

#define FLASH_BYTES 8388608UL
#define SECTOR_BYTES 4096UL
#define STORE_BASE (FLASH_BYTES - 2UL * SECTOR_BYTES)
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
} store_record_t;

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
static uint32_t active_sector;
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

static uint32_t record_crc(const store_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = UINT32_MAX;
    size_t i;
    for (i = 0U; i < sizeof(*record); ++i)
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

static bool valid_record(const store_record_t *record)
{
    uint32_t i;
    spi_clock_plan_t plan;
    if ((record->magic != STORE_MAGIC) || (record->version != 1U) ||
        (record->commit != STORE_COMMIT) || (record->payload_size != sizeof(*record) - 24U) ||
        (record->crc != record_crc(record)) || (record->mode > 3U) || (record->lsb > 1U) ||
        (record->cs_high > 1U) || (record->ascii > 1U) || !spi_clock_plan(record->hz, &plan))
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

static store_error_t blank_region_status(void)
{
    uint8_t bytes[256];
    uint32_t address;
    size_t i;
    for (address = STORE_BASE; address < FLASH_BYTES; address += sizeof(bytes))
    {
        if (!addressed(0x03U, address, NULL, bytes, sizeof(bytes)))
        {
            return STORE_IO;
        }
        for (i = 0U; i < sizeof(bytes); ++i)
        {
            if (bytes[i] != 0xFFU)
            {
                return STORE_FOREIGN_DATA;
            }
        }
    }
    return STORE_OK;
}

void command_store_init(void)
{
    const uint8_t read_id = 0x9FU;
    uint8_t value;
    uint32_t start;
    bool a_valid;
    bool b_valid;
    memset(&current, 0, sizeof(current));
    current.hz = 1000000U;
    memset(jedec, 0, sizeof(jedec));
    present = false;
    ready = false;
    has_saved = false;
    save_ok = false;
    active_sector = 0U;
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
    if (!addressed(0x03U, STORE_BASE, NULL, (uint8_t *)&current, sizeof(current)) ||
        !addressed(0x03U, STORE_BASE + SECTOR_BYTES, NULL, (uint8_t *)&candidate,
                   sizeof(candidate)))
    {
        last_error = STORE_IO;
        return;
    }
    a_valid = valid_record(&current);
    b_valid = valid_record(&candidate);
    if (b_valid &&
        (!a_valid || ((candidate.sequence != current.sequence) &&
                      ((uint32_t)(candidate.sequence - current.sequence) < 0x80000000UL))))
    {
        current = candidate;
        active_sector = 1U;
    }
    has_saved = a_valid || b_valid;
    last_error = has_saved ? STORE_OK : blank_region_status();
    ready = last_error == STORE_OK;
    if (!has_saved)
    {
        memset(&current, 0, sizeof(current));
        current.hz = 1000000U;
    }
    save_ok = has_saved;
}

static bool parse_snapshot(const char *body, size_t size)
{
    char command_text[1024];
    spi_command_t command;
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
    if (!spi_command_parse(command_text, header + 3U, &command))
    {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.hz = command.hz;
    candidate.mode = command.mode;
    candidate.lsb = command.lsb_first;
    candidate.cs_high = command.cs_high;
    candidate.ascii = command.ascii;
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
            if (!spi_command_parse(command_text, header + 1U + length, &command))
            {
                return false;
            }
        }
        offset += length;
    }
    return offset == size;
}

store_error_t command_store_save(const char *body, size_t size)
{
    if (job != JOB_IDLE)
    {
        return STORE_BUSY;
    }
    if (!present)
    {
        return STORE_NO_FLASH;
    }
    if (!ready)
    {
        return last_error;
    }
    if (!parse_snapshot(body, size))
    {
        return STORE_INVALID;
    }
    candidate.magic = STORE_MAGIC;
    candidate.version = 1U;
    candidate.sequence = has_saved ? current.sequence + 1U : 1U;
    candidate.payload_size = sizeof(candidate) - 24U;
    candidate.commit = UINT32_MAX;
    candidate.crc = record_crc(&candidate);
    target_address = STORE_BASE + (has_saved ? (1U - active_sector) * SECTOR_BYTES : 0U);
    position = 0U;
    last_error = STORE_OK;
    save_ok = false;
    operation_start = board_millis();
    job = JOB_ERASE;
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
        if (!addressed(0x03U, target_address + 20U, NULL, (uint8_t *)&commit, sizeof(commit)) ||
            (commit != STORE_COMMIT))
        {
            fail_job(STORE_VERIFY);
            return;
        }
        candidate.commit = STORE_COMMIT;
        current = candidate;
        active_sector = (target_address - STORE_BASE) / SECTOR_BYTES;
        has_saved = true;
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
    put_text(&writer, "{\"present\":");
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

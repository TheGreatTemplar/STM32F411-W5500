#include "help_store.h"
#include "board.h"
#include "flash_layout.h"
#include "help_seed.h"
#include <string.h>

#define HELP_MAGIC 0x31504C48UL
#define HELP_COMMIT 0xA55AC33CUL
#define HELP_PAYLOAD_OFFSET 256U

typedef struct
{
    uint32_t magic;
    uint32_t format;
    uint32_t sequence;
    uint32_t bytes;
    uint32_t crc;
    uint32_t commit;
} help_header_t;

typedef char help_header_size[(sizeof(help_header_t) == 24U) ? 1 : -1];
typedef char
    help_payload_fits[(sizeof(help_seed) <= FLASH_HELP_BANK_BYTES - HELP_PAYLOAD_OFFSET) ? 1 : -1];

typedef enum
{
    HELP_CLAIM,
    HELP_HEADER,
    HELP_SCAN,
    HELP_CHOOSE,
    HELP_ERASE,
    HELP_WAIT,
    HELP_WRITE_HEADER,
    HELP_WRITE_PAYLOAD,
    HELP_VERIFY_HEADER,
    HELP_VERIFY_PAYLOAD,
    HELP_WRITE_COMMIT,
    HELP_VERIFY_COMMIT,
    HELP_DONE,
    HELP_FAULT
} help_state_t;

static help_state_t state;
static help_state_t after_wait;
static help_header_t headers[2];
static help_header_t candidate;
static bool valid[2];
static bool reusable[2];
static bool scanning_payload;
static bool blank;
static uint32_t bank;
static uint32_t active;
static uint32_t target;
static uint32_t position;
static uint32_t checksum;
static uint32_t operation_start;
static store_error_t last_error;

static uint32_t bank_address(uint32_t index)
{
    return FLASH_HELP_BASE + index * FLASH_HELP_BANK_BYTES;
}

static uint32_t crc_bytes(uint32_t crc, const uint8_t *bytes, size_t size)
{
    size_t i;
    for (i = 0U; i < size; ++i)
    {
        uint32_t bit;
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
        }
    }
    return crc;
}

static bool status(uint8_t *value)
{
    const uint8_t command = 0x05U;
    return board_flash_transfer(&command, 1U, NULL, value, 1U);
}

static bool addressed(uint8_t command, uint32_t address, const void *tx, void *rx, size_t size)
{
    const uint8_t prefix[4] = {command, (uint8_t)(address >> 16U), (uint8_t)(address >> 8U),
                               (uint8_t)address};
    return board_flash_transfer(prefix, sizeof(prefix), tx, rx, size);
}

static bool write_enable(void)
{
    const uint8_t command = 0x06U;
    uint8_t value;
    return board_flash_transfer(&command, 1U, NULL, NULL, 0U) && status(&value) &&
           (value & 3U) == 2U;
}

static void fail(store_error_t error)
{
    last_error = error;
    /* Keep NOR ownership until an outstanding physical write has really stopped. */
    state = HELP_FAULT;
}

static bool start_write(uint8_t command, uint32_t address, const void *data, size_t size,
                        help_state_t next)
{
    if (!write_enable() || !addressed(command, address, data, NULL, size))
    {
        fail(STORE_IO);
        return false;
    }
    operation_start = board_millis();
    after_wait = next;
    state = HELP_WAIT;
    return true;
}

static void next_bank(void)
{
    ++bank;
    state = bank == 2U ? HELP_CHOOSE : HELP_HEADER;
}

void help_store_init(void)
{
    memset(headers, 0, sizeof(headers));
    memset(valid, 0, sizeof(valid));
    memset(reusable, 0, sizeof(reusable));
    active = 2U;
    bank = 0U;
    last_error = STORE_BUSY;
    state = HELP_CLAIM;
}

bool help_store_ready(void)
{
    return state == HELP_DONE && active < 2U;
}

size_t help_store_size(void)
{
    return help_store_ready() ? headers[active].bytes : 0U;
}

store_error_t help_store_error(void)
{
    return last_error;
}

store_error_t help_store_read(size_t offset, void *data, size_t size)
{
    store_error_t error;
    if (!help_store_ready())
    {
        return last_error;
    }
    if (offset > headers[active].bytes || size > headers[active].bytes - offset)
    {
        return STORE_INVALID;
    }
    error = command_flash_claim();
    if (error != STORE_OK)
    {
        return error;
    }
    error = addressed(0x03U, bank_address(active) + HELP_PAYLOAD_OFFSET + (uint32_t)offset, NULL,
                      data, size)
                ? STORE_OK
                : STORE_IO;
    command_flash_release();
    return error;
}

void help_store_poll(void)
{
    uint8_t bytes[256];
    uint8_t value;
    size_t count;
    uint32_t i;
    if (state == HELP_DONE)
    {
        return;
    }
    if (state == HELP_CLAIM)
    {
        const store_error_t error = command_flash_claim();
        if (error == STORE_BUSY)
        {
            return;
        }
        if (error != STORE_OK)
        {
            last_error = error;
            state = HELP_DONE;
            return;
        }
        state = HELP_HEADER;
        return;
    }
    if (!status(&value) || value == 0xFFU)
    {
        fail(STORE_IO);
        return;
    }
    if (state == HELP_FAULT)
    {
        if ((value & 1U) == 0U)
        {
            command_flash_release();
            state = HELP_DONE;
        }
        return;
    }
    if ((value & 1U) != 0U)
    {
        if ((uint32_t)(board_millis() - operation_start) >= 1000U)
        {
            fail(STORE_TIMEOUT);
        }
        return;
    }
    switch (state)
    {
    case HELP_HEADER:
    {
        help_header_t *header = &headers[bank];
        if (!addressed(0x03U, bank_address(bank), NULL, header, sizeof(*header)))
        {
            fail(STORE_IO);
            return;
        }
        scanning_payload = header->magic == HELP_MAGIC && header->format == 1U &&
                           header->bytes > 0U &&
                           header->bytes <= FLASH_HELP_BANK_BYTES - HELP_PAYLOAD_OFFSET;
        if (scanning_payload && header->commit != HELP_COMMIT)
        {
            reusable[bank] = true; /* Interrupted owned installation. */
            next_bank();
            return;
        }
        scanning_payload = scanning_payload && header->commit == HELP_COMMIT;
        position = 0U;
        checksum = UINT32_MAX;
        blank = true;
        state = HELP_SCAN;
    }
    break;
    case HELP_SCAN:
        count = (scanning_payload ? headers[bank].bytes : FLASH_HELP_BANK_BYTES) - position;
        if (count > sizeof(bytes))
        {
            count = sizeof(bytes);
        }
        if (!addressed(0x03U,
                       bank_address(bank) + position +
                           (scanning_payload ? HELP_PAYLOAD_OFFSET : 0U),
                       NULL, bytes, count))
        {
            fail(STORE_IO);
            return;
        }
        if (scanning_payload)
        {
            checksum = crc_bytes(checksum, bytes, count);
        }
        else
        {
            for (i = 0U; i < count; ++i)
            {
                blank = blank && bytes[i] == 0xFFU;
            }
        }
        position += (uint32_t)count;
        if (position == (scanning_payload ? headers[bank].bytes : FLASH_HELP_BANK_BYTES))
        {
            valid[bank] = scanning_payload && (checksum ^ UINT32_MAX) == headers[bank].crc;
            reusable[bank] = valid[bank] || (!scanning_payload && blank);
            next_bank();
        }
        break;
    case HELP_CHOOSE:
        for (i = 0U; i < 2U; ++i)
        {
            if (valid[i] &&
                (active == 2U || (int32_t)(headers[i].sequence - headers[active].sequence) > 0))
            {
                active = i;
            }
        }
        if (active < 2U && headers[active].bytes == sizeof(help_seed) &&
            headers[active].crc == HELP_CONTENT_CRC)
        {
            last_error = STORE_OK;
            command_flash_release();
            state = HELP_DONE;
            return;
        }
        target = active < 2U ? 1U - active : (reusable[0] ? 0U : 1U);
        if (!reusable[target])
        {
            last_error = STORE_FOREIGN_DATA;
            command_flash_release();
            state = HELP_DONE;
            return;
        }
        candidate.magic = HELP_MAGIC;
        candidate.format = 1U;
        candidate.sequence = active < 2U ? headers[active].sequence + 1U : 1U;
        candidate.bytes = sizeof(help_seed);
        candidate.crc = HELP_CONTENT_CRC;
        candidate.commit = UINT32_MAX;
        position = 0U;
        state = HELP_ERASE;
        break;
    case HELP_ERASE:
        if (position >= HELP_PAYLOAD_OFFSET + sizeof(help_seed))
        {
            state = HELP_WRITE_HEADER;
            break;
        }
        if (start_write(0x20U, bank_address(target) + position, NULL, 0U, HELP_ERASE))
        {
            position += 4096U;
        }
        break;
    case HELP_WAIT:
        state = after_wait;
        break;
    case HELP_WRITE_HEADER:
        position = 0U;
        (void)start_write(0x02U, bank_address(target), &candidate, sizeof(candidate),
                          HELP_WRITE_PAYLOAD);
        break;
    case HELP_WRITE_PAYLOAD:
        if (position == sizeof(help_seed))
        {
            state = HELP_VERIFY_HEADER;
            break;
        }
        count = sizeof(help_seed) - position;
        if (count > 256U)
        {
            count = 256U;
        }
        if (start_write(0x02U, bank_address(target) + HELP_PAYLOAD_OFFSET + position,
                        help_seed + position, count, HELP_WRITE_PAYLOAD))
        {
            position += (uint32_t)count;
        }
        break;
    case HELP_VERIFY_HEADER:
        if (!addressed(0x03U, bank_address(target), NULL, bytes, sizeof(candidate)) ||
            memcmp(bytes, &candidate, sizeof(candidate)) != 0)
        {
            fail(STORE_VERIFY);
            return;
        }
        position = 0U;
        checksum = UINT32_MAX;
        state = HELP_VERIFY_PAYLOAD;
        break;
    case HELP_VERIFY_PAYLOAD:
        count = sizeof(help_seed) - position;
        if (count > sizeof(bytes))
        {
            count = sizeof(bytes);
        }
        if (!addressed(0x03U, bank_address(target) + HELP_PAYLOAD_OFFSET + position, NULL, bytes,
                       count))
        {
            fail(STORE_IO);
            return;
        }
        checksum = crc_bytes(checksum, bytes, count);
        position += (uint32_t)count;
        if (position == sizeof(help_seed))
        {
            if ((checksum ^ UINT32_MAX) != candidate.crc)
            {
                fail(STORE_VERIFY);
                return;
            }
            state = HELP_WRITE_COMMIT;
        }
        break;
    case HELP_WRITE_COMMIT:
    {
        const uint32_t commit = HELP_COMMIT;
        (void)start_write(0x02U, bank_address(target) + 20U, &commit, sizeof(commit),
                          HELP_VERIFY_COMMIT);
    }
    break;
    case HELP_VERIFY_COMMIT:
        if (!addressed(0x03U, bank_address(target), NULL, bytes, sizeof(candidate)))
        {
            fail(STORE_IO);
            return;
        }
        candidate.commit = HELP_COMMIT;
        if (memcmp(bytes, &candidate, sizeof(candidate)) != 0)
        {
            fail(STORE_VERIFY);
            return;
        }
        headers[target] = candidate;
        active = target;
        last_error = STORE_OK;
        command_flash_release();
        state = HELP_DONE;
        break;
    case HELP_CLAIM:
    case HELP_DONE:
    case HELP_FAULT:
        break;
    }
}

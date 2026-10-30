#include "usart_console.h"
#include "board.h"
#include "spi_console.h"
#include <stdio.h>
#include <string.h>

#define DMA_RX_SIZE 2048U
#define HISTORY_SIZE 8192U
#define RX_CHUNK 1024U
#define RX_FLAGS                                                                                   \
    (DMA_LISR_TCIF2 | DMA_LISR_HTIF2 | DMA_LISR_TEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_FEIF2)
#define TX_FLAGS                                                                                   \
    (DMA_HISR_TCIF7 | DMA_HISR_HTIF7 | DMA_HISR_TEIF7 | DMA_HISR_DMEIF7 | DMA_HISR_FEIF7)

static uint8_t dma_rx[DMA_RX_SIZE];
static uint8_t history[HISTORY_SIZE];
static uint8_t dma_tx[258];
static volatile uint32_t head;
static volatile uint32_t retained;
static uint32_t rx_position;
static uint32_t baud;
static uint32_t actual_baud;
static uint32_t data_bits;
static uint32_t parity;
static uint32_t stops;
static uint32_t tx_count;
static uint32_t tx_size;
static uint32_t tx_start;
static volatile uint32_t errors[6]; /* PE, FE, NE, ORE, DMA, TX timeout. */
static volatile bool enabled;
static volatile bool transmitting;

bool usart_enabled(void)
{
    return enabled;
}

static bool stop_stream(DMA_Stream_TypeDef *stream)
{
    uint32_t attempts = 4096U;
    stream->CR &= ~DMA_SxCR_EN;
    while ((stream->CR & DMA_SxCR_EN) != 0U && attempts-- != 0U)
    {
        __NOP();
    }
    return (stream->CR & DMA_SxCR_EN) == 0U;
}

static void fatal_dma(void)
{
    USART1->CR1 = 0U;
    USART1->CR3 = 0U;
    /* DMA2 is dedicated to USART1; ADC and network SPI use CPU accesses. */
    RCC->AHB1RSTR |= RCC_AHB1RSTR_DMA2RST;
    (void)RCC->AHB1RSTR;
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_DMA2RST;
    enabled = false;
    transmitting = false;
    ++errors[4];
}

static void receive_poll(void)
{
    uint32_t position;
    if (!enabled)
    {
        return;
    }
    position = (DMA_RX_SIZE - DMA2_Stream2->NDTR) & (DMA_RX_SIZE - 1U);
    __DMB();
    while (rx_position != position)
    {
        history[head & (HISTORY_SIZE - 1U)] =
            dma_rx[rx_position] & (data_bits == 7U ? 0x7FU : 0xFFU);
        ++head;
        if (retained < HISTORY_SIZE)
        {
            ++retained;
        }
        rx_position = (rx_position + 1U) & (DMA_RX_SIZE - 1U);
    }
}

void DMA2_Stream2_IRQHandler(void)
{
    const uint32_t flags = DMA2->LISR;
    DMA2->LIFCR = RX_FLAGS;
    if ((flags & (DMA_LISR_TEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_FEIF2)) != 0U)
    {
        fatal_dma();
        return;
    }
    receive_poll();
}

void USART1_IRQHandler(void)
{
    const uint32_t status = USART1->SR;
    uint32_t i;
    if ((status & (USART_SR_IDLE | USART_SR_PE | USART_SR_FE | USART_SR_NE | USART_SR_ORE)) != 0U)
    {
        /* SR then DR is the STM32F4 acknowledgement sequence. An error byte
         * can already have reached DMA; error counters qualify the RX log. */
        (void)USART1->DR;
        for (i = 0U; i < 4U; ++i)
        {
            if ((status & (1UL << i)) != 0U)
            {
                ++errors[i];
            }
        }
        receive_poll();
    }
}

void usart_init(void)
{
    uint32_t i;
    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_DisableIRQ(DMA2_Stream2_IRQn);
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_DMA2EN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    USART1->CR1 = 0U;
    USART1->CR3 = 0U;
    RCC->AHB1RSTR |= RCC_AHB1RSTR_DMA2RST;
    (void)RCC->AHB1RSTR;
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_DMA2RST;
    GPIOA->BSRR = 1UL << 9U;
    GPIOA->MODER = (GPIOA->MODER & ~(15UL << 18U)) | (10UL << 18U);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(255UL << 4U)) | (0x77UL << 4U);
    GPIOA->OTYPER &= ~(3UL << 9U);
    GPIOA->OSPEEDR |= 3UL << 18U;
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(15UL << 18U)) | (5UL << 18U);
    enabled = false;
    transmitting = false;
    head = 0U;
    retained = 0U;
    tx_count = 0U;
    baud = 115200U;
    actual_baud = 0U;
    data_bits = 8U;
    parity = 0U;
    stops = 1U;
    for (i = 0U; i < 6U; ++i)
    {
        errors[i] = 0U;
    }
    NVIC_SetPriority(USART1_IRQn, 4U);
    NVIC_SetPriority(DMA2_Stream2_IRQn, 4U);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_ClearPendingIRQ(DMA2_Stream2_IRQn);
    NVIC_EnableIRQ(USART1_IRQn);
    NVIC_EnableIRQ(DMA2_Stream2_IRQn);
}

void usart_poll(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    receive_poll();
    if (transmitting)
    {
        const uint32_t flags = DMA2->HISR;
        if ((flags & (DMA_HISR_TEIF7 | DMA_HISR_DMEIF7 | DMA_HISR_FEIF7)) != 0U)
        {
            fatal_dma();
        }
        else if (DMA2_Stream7->NDTR == 0U && (USART1->SR & USART_SR_TC) != 0U)
        {
            if (!stop_stream(DMA2_Stream7))
            {
                fatal_dma();
            }
            else
            {
                USART1->CR3 &= ~USART_CR3_DMAT;
                DMA2->HIFCR = TX_FLAGS;
                tx_count += tx_size;
                transmitting = false;
            }
        }
        else if ((uint32_t)(board_millis() - tx_start) > (tx_size * 12000U / baud) + 20U)
        {
            ++errors[5];
            fatal_dma();
        }
    }
    __set_PRIMASK(primask);
}

static bool decimal(const char **text, const char *end, uint32_t *value)
{
    const char *start = *text;
    *value = 0U;
    while (*text < end && **text >= '0' && **text <= '9')
    {
        const uint32_t digit = (uint32_t)(*(*text)++ - '0');
        if (*value > (UINT32_MAX - digit) / 10U)
        {
            return false;
        }
        *value = *value * 10U + digit;
    }
    return *text != start;
}

static bool literal(const char **text, const char *end, const char *token)
{
    const size_t size = strlen(token);
    if ((size_t)(end - *text) < size || memcmp(*text, token, size) != 0)
    {
        return false;
    }
    *text += size;
    return true;
}

bool usart_cursor(const char *request, size_t size, uint32_t *cursor)
{
    const char *end = request + size;
    const char *text = request;
    if (!literal(&text, end, "GET /api/usart?from=") || !decimal(&text, end, cursor))
    {
        return false;
    }
    return literal(&text, end, " HTTP/1.");
}

uint32_t usart_control(const char *body, size_t size)
{
    const char *text = body;
    const char *end = body + size;
    uint32_t new_baud;
    uint32_t bits;
    uint32_t par;
    uint32_t stop;
    uint32_t divider;
    uint32_t primask;
    uint8_t bytes[256];
    size_t count;
    if (size == 5U && memcmp(body, "stop\n", 5U) == 0)
    {
        bool stopped;
        if (transmitting)
        {
            return 2U;
        }
        primask = __get_PRIMASK();
        __disable_irq();
        receive_poll();
        USART1->CR1 = 0U;
        USART1->CR3 = 0U;
        stopped = stop_stream(DMA2_Stream2);
        if (!stopped)
        {
            fatal_dma();
        }
        enabled = false;
        __set_PRIMASK(primask);
        return stopped ? 0U : 4U;
    }
    if (literal(&text, end, "start baud=") && decimal(&text, end, &new_baud) &&
        literal(&text, end, " bits=") && decimal(&text, end, &bits) &&
        literal(&text, end, " parity=") && decimal(&text, end, &par) &&
        literal(&text, end, " stop=") && decimal(&text, end, &stop) && literal(&text, end, "\n") &&
        text == end)
    {
        if ((new_baud != 300U && new_baud != 600U && new_baud != 1200U && new_baud != 2400U &&
             new_baud != 4800U && new_baud != 9600U && new_baud != 19200U && new_baud != 38400U &&
             new_baud != 57600U && new_baud != 115200U && new_baud != 230400U &&
             new_baud != 460800U && new_baud != 500000U && new_baud != 921600U &&
             new_baud != 1000000U) ||
            (bits != 7U && bits != 8U) || par > 2U || (bits == 7U && par == 0U) ||
            (stop != 1U && stop != 2U))
        {
            return 1U;
        }
        if (transmitting)
        {
            return 2U;
        }
        divider = (BOARD_PCLK2_HZ + new_baud / 2U) / new_baud;
        primask = __get_PRIMASK();
        __disable_irq();
        receive_poll();
        USART1->CR1 = 0U;
        USART1->CR3 = 0U;
        if (!stop_stream(DMA2_Stream2) || !stop_stream(DMA2_Stream7))
        {
            fatal_dma();
            __set_PRIMASK(primask);
            return 4U;
        }
        DMA2->LIFCR = RX_FLAGS;
        DMA2->HIFCR = TX_FLAGS;
        (void)USART1->SR;
        (void)USART1->DR;
        USART1->BRR = divider < 16U ? ((divider / 8U) << 4U) | (divider % 8U) : divider;
        USART1->CR2 = stop == 2U ? USART_CR2_STOP_1 : 0U;
        DMA2_Stream2->PAR = (uint32_t)&USART1->DR;
        DMA2_Stream2->M0AR = (uint32_t)dma_rx;
        DMA2_Stream2->NDTR = DMA_RX_SIZE;
        DMA2_Stream2->FCR = 0U;
        DMA2_Stream2->CR = (4UL << 25U) | DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_PL_1 |
                           DMA_SxCR_HTIE | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
        rx_position = 0U;
        DMA2_Stream2->CR |= DMA_SxCR_EN;
        USART1->CR3 = USART_CR3_DMAR | USART_CR3_EIE;
        USART1->CR1 =
            USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_IDLEIE |
            (divider < 16U ? USART_CR1_OVER8 : 0U) | (bits == 8U && par != 0U ? USART_CR1_M : 0U) |
            (par != 0U ? USART_CR1_PCE | USART_CR1_PEIE : 0U) | (par == 1U ? USART_CR1_PS : 0U);
        baud = new_baud;
        actual_baud = BOARD_PCLK2_HZ / divider;
        data_bits = bits;
        parity = par;
        stops = stop;
        enabled = true;
        __set_PRIMASK(primask);
        return 0U;
    }
    text = body;
    if (!literal(&text, end, "tx format=") || text == end || (*text != '0' && *text != '1'))
    {
        return 1U;
    }
    par = (uint32_t)(*text++ - '0'); /* 0 HEX, 1 ASCII. */
    if (!literal(&text, end, " eol=") || !decimal(&text, end, &stop) || stop > 3U ||
        !literal(&text, end, "\n") ||
        !console_bytes_parse(text, (size_t)(end - text), par != 0U, bytes, &count, false))
    {
        return 1U;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (!enabled)
    {
        __set_PRIMASK(primask);
        return 3U;
    }
    if (transmitting)
    {
        __set_PRIMASK(primask);
        return 2U;
    }
    if (data_bits == 7U)
    {
        size_t i;
        for (i = 0U; i < count; ++i)
        {
            if (bytes[i] > 127U)
            {
                __set_PRIMASK(primask);
                return 1U;
            }
        }
    }
    if (!stop_stream(DMA2_Stream7))
    {
        fatal_dma();
        __set_PRIMASK(primask);
        return 4U;
    }
    memcpy(dma_tx, bytes, count);
    if (stop == 1U || stop == 3U)
    {
        dma_tx[count++] = '\r';
    }
    if (stop == 2U || stop == 3U)
    {
        dma_tx[count++] = '\n';
    }
    DMA2->HIFCR = TX_FLAGS;
    DMA2_Stream7->PAR = (uint32_t)&USART1->DR;
    DMA2_Stream7->M0AR = (uint32_t)dma_tx;
    DMA2_Stream7->NDTR = count;
    DMA2_Stream7->FCR = 0U;
    DMA2_Stream7->CR = (4UL << 25U) | DMA_SxCR_MINC | DMA_SxCR_DIR_0 | DMA_SxCR_PL_0;
    USART1->SR = ~USART_SR_TC;
    tx_size = count;
    tx_start = board_millis();
    transmitting = true;
    __DMB();
    DMA2_Stream7->CR |= DMA_SxCR_EN;
    USART1->CR3 |= USART_CR3_DMAT;
    __set_PRIMASK(primask);
    return 0U;
}

size_t usart_json(char *json, size_t capacity, uint32_t cursor)
{
    static const char digits[] = "0123456789ABCDEF";
    uint8_t bytes[RX_CHUNK];
    uint32_t lost = 0U;
    uint32_t size;
    uint32_t next;
    uint32_t total;
    uint32_t more;
    uint32_t i;
    int count;
    size_t used;
    const uint32_t primask = __get_PRIMASK();
    usart_poll();
    __disable_irq();
    total = head;
    if (total - cursor > 0x7FFFFFFFU)
    {
        cursor = total - retained; /* A client cursor can survive an MCU reset. */
    }
    if (total - cursor > retained)
    {
        lost = total - cursor - retained;
        cursor = total - retained;
    }
    size = total - cursor;
    if (size > RX_CHUNK)
    {
        size = RX_CHUNK;
    }
    for (i = 0U; i < size; ++i)
    {
        bytes[i] = history[(cursor + i) & (HISTORY_SIZE - 1U)];
    }
    next = cursor + size;
    more = total - next;
    __set_PRIMASK(primask);
    count =
        snprintf(json, capacity,
                 "{\"enabled\":%s,\"tx_busy\":%s,\"baud\":%lu,\"actual_baud\":%lu,"
                 "\"bits\":%lu,\"parity\":%lu,\"stop\":%lu,\"rx_total\":%lu,"
                 "\"tx_total\":%lu,\"next\":%lu,\"lost\":%lu,\"more\":%lu,"
                 "\"errors\":[%lu,%lu,%lu,%lu,%lu,%lu],\"rx\":\"",
                 enabled ? "true" : "false", transmitting ? "true" : "false", (unsigned long)baud,
                 (unsigned long)actual_baud, (unsigned long)data_bits, (unsigned long)parity,
                 (unsigned long)stops, (unsigned long)total, (unsigned long)tx_count,
                 (unsigned long)next, (unsigned long)lost, (unsigned long)more,
                 (unsigned long)errors[0], (unsigned long)errors[1], (unsigned long)errors[2],
                 (unsigned long)errors[3], (unsigned long)errors[4], (unsigned long)errors[5]);
    if (count <= 0 || (size_t)count + size * 2U + 3U >= capacity)
    {
        return 0U;
    }
    used = (size_t)count;
    for (i = 0U; i < size; ++i)
    {
        json[used++] = digits[bytes[i] >> 4U];
        json[used++] = digits[bytes[i] & 15U];
    }
    memcpy(json + used, "\"}", 3U);
    return used + 2U;
}

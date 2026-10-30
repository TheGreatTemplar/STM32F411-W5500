#include "board.h"
#include "i2c_console.h"
#include "spi_console.h"

static volatile uint32_t tick_ms;
static uint32_t spi_console_duration_us;

uint32_t board_spi_console_duration_us(void)
{
    return spi_console_duration_us;
}

void SystemInit(void)
{
    /* Start on reset HSI. board_init selects the confirmed 25 MHz HSE plan. */
    SCB->VTOR = 0x08000000U;
}

void SysTick_Handler(void)
{
    ++tick_ms;
}

uint32_t board_millis(void)
{
    return tick_ms;
}

void board_delay(uint32_t duration_ms)
{
    const uint32_t start = board_millis();
    while ((uint32_t)(board_millis() - start) < duration_ms)
    {
        __WFI();
    }
}

static void output_mode(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
    port->OTYPER &= ~(1UL << pin);
    port->PUPDR &= ~(3UL << (pin * 2U));
}

bool board_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
    (void)RCC->AHB1ENR;
    /* Set inactive network control levels before changing direction. */
    GPIOA->BSRR = (1UL << 15U) | (1UL << 4U); /* W5500 / Flash CS */
    GPIOB->BSRR = 1UL << (2U + 16U);
    GPIOC->BSRR = 1UL << 13U; /* LED off, active low */
    output_mode(GPIOA, 15U);
    output_mode(GPIOA, 4U);
    output_mode(GPIOB, 2U); /* W5500 reset */
    output_mode(GPIOC, 13U);
    GPIOB->BSRR = 1UL << 12U;
    output_mode(GPIOB, 12U);
    if (!board_clock_init())
    {
        GPIOC->BSRR = 1UL << 29U;
        return false;
    }
    /* Standalone bench: PB12 CS, PB13/14/15 SPI2 AF5. */
    GPIOB->BSRR = 1UL << 12U;
    output_mode(GPIOB, 12U);
    GPIOB->MODER = (GPIOB->MODER & ~0xFC000000UL) | 0xA8000000UL;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~0xFFF00000UL) | 0x55500000UL;
    GPIOB->OSPEEDR |= 0xFC000000UL; /* Very high speed for up to 24 MHz SPI2. */
    GPIOB->PUPDR &= ~0xFC000000UL;
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    (void)RCC->APB1ENR;

    board_i2c_init();

    /* PA5/6/7: AF5, SPI1 mode 0, APB2 12 MHz / 4 = 3 MHz. */
    GPIOA->MODER = (GPIOA->MODER & ~0x0000FC00UL) | 0x0000A800UL;
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~0xFFF00000UL) | 0x55500000UL;
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~0x0000FC00UL) | 0x00005400UL;
    GPIOA->PUPDR &= ~0x0000FC00UL;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN | RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_0 | SPI_CR1_SPE;

    /* PA0/PA1: analog, no pull. ADC clock 6 MHz, 12 bit, software-triggered. */
    GPIOA->MODER |= 0xFUL;
    GPIOA->PUPDR &= ~0xFUL;
    ADC->CCR = 0U;
    ADC1->CR1 = 0U;
    ADC1->CR2 = ADC_CR2_ADON;
    ADC1->SMPR2 = (7UL << 0U) | (7UL << 3U); /* 480 cycles per channel */
    ADC1->SQR1 = 0U;
    ADC1->SQR2 = 0U;
    (void)SysTick_Config(SystemCoreClock / 1000U);
    board_delay(2U);
    /* PC14/PC15 are untouched: reserved for LSE. */
    return true;
}

void board_led(bool on)
{
    GPIOC->BSRR = 1UL << (on ? 29U : 13U);
}

bool board_led_is_on(void)
{
    return (GPIOC->ODR & (1UL << 13U)) == 0U;
}

void board_w5500_reset(void)
{
    GPIOB->BSRR = 1UL << 18U;
    board_delay(2U);
    GPIOB->BSRR = 1UL << 2U;
    board_delay(100U);
}

static bool spi_byte(uint8_t tx, uint8_t *rx)
{
    const uint32_t start = board_millis();
    while ((SPI1->SR & SPI_SR_TXE) == 0U)
    {
        if ((uint32_t)(board_millis() - start) >= 5U)
        {
            return false;
        }
    }
    *(__IO uint8_t *)&SPI1->DR = tx;
    while ((SPI1->SR & SPI_SR_RXNE) == 0U)
    {
        if ((uint32_t)(board_millis() - start) >= 5U)
        {
            return false;
        }
    }
    *rx = *(__IO uint8_t *)&SPI1->DR;
    return true;
}

bool board_spi_transfer(uint16_t address, uint8_t control, const uint8_t *tx, uint8_t *received,
                        size_t size)
{
    uint8_t rx;
    size_t i;
    bool ok;
    uint32_t start;
    GPIOA->BSRR = 1UL << 4U; /* Flash must be deselected during Ethernet frames. */
    GPIOA->BSRR = 1UL << 31U;
    ok = spi_byte((uint8_t)(address >> 8U), &rx) && spi_byte((uint8_t)address, &rx) &&
         spi_byte(control, &rx);
    for (i = 0U; ok && (i < size); ++i)
    {
        ok = spi_byte(tx != NULL ? tx[i] : 0U, &rx);
        if (received != NULL)
        {
            received[i] = rx;
        }
    }
    start = board_millis();
    while (ok && ((SPI1->SR & SPI_SR_BSY) != 0U))
    {
        ok = (uint32_t)(board_millis() - start) < 5U;
    }
    GPIOA->BSRR = 1UL << 15U;
    if (!ok)
    {
        /* Recover the peripheral; caller restarts the W5500 after an I/O error. */
        RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
        RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
        SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_0 | SPI_CR1_SPE;
    }
    return ok;
}

bool board_flash_transfer(const uint8_t *prefix, size_t prefix_size, const uint8_t *tx,
                          uint8_t *received, size_t size)
{
    uint8_t rx = 0U;
    size_t i;
    bool ok = true;
    uint32_t start;
    GPIOA->BSRR = 1UL << 15U;
    GPIOA->BSRR = 1UL << 20U;
    for (i = 0U; ok && (i < prefix_size); ++i)
    {
        ok = spi_byte(prefix[i], &rx);
    }
    for (i = 0U; ok && (i < size); ++i)
    {
        ok = spi_byte(tx != NULL ? tx[i] : 0U, &rx);
        if (received != NULL)
        {
            received[i] = rx;
        }
    }
    start = board_millis();
    while (ok && ((SPI1->SR & SPI_SR_BSY) != 0U))
    {
        ok = (uint32_t)(board_millis() - start) < 5U;
    }
    GPIOA->BSRR = 1UL << 4U;
    if (!ok)
    {
        RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
        RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
        SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_0 | SPI_CR1_SPE;
    }
    return ok;
}

bool board_adc_read(uint16_t values[2])
{
    uint32_t channel;
    for (channel = 0U; channel < 2U; ++channel)
    {
        const uint32_t start = board_millis();
        ADC1->SQR3 = channel;
        ADC1->SR = 0U;
        ADC1->CR2 |= ADC_CR2_SWSTART;
        while ((ADC1->SR & ADC_SR_EOC) == 0U)
        {
            if ((uint32_t)(board_millis() - start) >= 5U)
            {
                return false;
            }
        }
        values[channel] = (uint16_t)ADC1->DR;
    }
    return true;
}

bool board_spi_console_config(const spi_command_t *command)
{
    spi_clock_plan_t plan;
    uint32_t cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
    if (command->mode > 3U || command->lsb_first > 1U || command->cs_high > 1U ||
        !spi_clock_plan(command->hz, &plan) || (SPI2->SR & SPI_SR_BSY) != 0U)
    {
        return false;
    }
    GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 28U : 12U);
    SPI2->CR1 = 0U;
    if ((command->mode & 2U) != 0U)
    {
        cr1 |= SPI_CR1_CPOL;
    }
    if ((command->mode & 1U) != 0U)
    {
        cr1 |= SPI_CR1_CPHA;
    }
    if (command->lsb_first != 0U)
    {
        cr1 |= SPI_CR1_LSBFIRST;
    }
    /* No DR access and no active CS: CPOL becomes visible on the idle SCK pin.
     * The PLL and bus clocks remain fixed for every transfer. */
    SPI2->CR1 = cr1 | (plan.prescaler << 3U) | SPI_CR1_SPE;
    return true;
}

/* DWT runs at the selected core clock, independently of the millisecond tick.
 * Inline the ready path: do not call a timer function between data accesses. */
__STATIC_FORCEINLINE bool spi2_wait(uint32_t mask, uint32_t value, uint32_t start, uint32_t budget)
{
    for (;;)
    {
        const uint32_t status = SPI2->SR;
        if ((status & (SPI_SR_OVR | SPI_SR_MODF)) != 0U)
        {
            return false;
        }
        if ((status & mask) == value)
        {
            return true;
        }
        if ((uint32_t)(DWT->CYCCNT - start) >= budget)
        {
            return false;
        }
    }
}

/* DMA1 is currently dedicated to SPI2. If a stream cannot stop, resetting DMA1
 * is required before returning: it must never retain a pointer into caller RAM. */
static bool spi2_dma_stop(uint32_t hz)
{
    const uint32_t start = DWT->CYCCNT;
    DMA1_Stream3->CR &= ~DMA_SxCR_EN;
    DMA1_Stream4->CR &= ~DMA_SxCR_EN;
    while (((DMA1_Stream3->CR | DMA1_Stream4->CR) & DMA_SxCR_EN) != 0U)
    {
        if ((uint32_t)(DWT->CYCCNT - start) >= hz / 10000U)
        {
            RCC->AHB1RSTR |= RCC_AHB1RSTR_DMA1RST;
            (void)RCC->AHB1RSTR;
            RCC->AHB1RSTR &= ~RCC_AHB1RSTR_DMA1RST;
            __DSB();
            return false;
        }
    }
    DMA1->LIFCR = DMA_LIFCR_CTCIF3 | DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTEIF3 | DMA_LIFCR_CDMEIF3 |
                  DMA_LIFCR_CFEIF3;
    DMA1->HIFCR = DMA_HIFCR_CTCIF4 | DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTEIF4 | DMA_HIFCR_CDMEIF4 |
                  DMA_HIFCR_CFEIF4;
    __DSB();
    return true;
}

bool board_spi_console(const spi_command_t *command, uint8_t rx[SPI_CONSOLE_MAX_BYTES])
{
    spi_clock_plan_t plan;
    uint32_t cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
    uint32_t budget;
    uint32_t cycles;
    bool ok = true;
    spi_console_duration_us = 0U;
    if ((command->mode > 3U) || (command->lsb_first > 1U) || (command->cs_high > 1U) ||
        (command->size == 0U) || (command->size > SPI_CONSOLE_MAX_BYTES))
    {
        return false;
    }
    if (!spi_clock_plan(command->hz, &plan))
    {
        return false;
    }
    SPI2->CR2 = 0U;
    SPI2->CR1 = 0U;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 28U : 12U);
    if ((command->mode & 2U) != 0U)
    {
        cr1 |= SPI_CR1_CPOL;
    }
    if ((command->mode & 1U) != 0U)
    {
        cr1 |= SPI_CR1_CPHA;
    }
    if (command->lsb_first != 0U)
    {
        cr1 |= SPI_CR1_LSBFIRST;
    }
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    (void)RCC->AHB1ENR;
    ok = spi2_dma_stop(plan.system_hz);
    SPI2->CR1 = cr1 | (plan.prescaler << 3U) | SPI_CR1_SPE;
    /* Expected wire time plus 100 us of software/peripheral margin.
     * The maximum budget is well below one CYCCNT wrap at every supported rate. */
    budget =
        (uint32_t)command->size * 8U * (plan.system_hz / command->hz) + plan.system_hz / 10000U;
    cycles = DWT->CYCCNT;
    if (ok)
    {
        /* RM0383: SPI2 RX = DMA1 stream 3/channel 0, TX = stream 4/channel 0.
         * Normal mode, 8-bit memory/peripheral, MINC, direct mode, no IRQ. */
        DMA1_Stream3->CR = DMA_SxCR_MINC | DMA_SxCR_PL;
        DMA1_Stream4->CR = DMA_SxCR_MINC | DMA_SxCR_PL_1 | DMA_SxCR_DIR_0;
        DMA1_Stream3->FCR = 0U;
        DMA1_Stream4->FCR = 0U;
        DMA1_Stream3->PAR = (uint32_t)&SPI2->DR;
        DMA1_Stream4->PAR = (uint32_t)&SPI2->DR;
        DMA1_Stream3->M0AR = (uint32_t)rx;
        DMA1_Stream4->M0AR = (uint32_t)command->tx;
        DMA1_Stream3->NDTR = (uint32_t)command->size;
        DMA1_Stream4->NDTR = (uint32_t)command->size;
        __DMB();
        DMA1_Stream3->CR |= DMA_SxCR_EN;
        SPI2->CR2 = SPI_CR2_RXDMAEN;
        DMA1_Stream4->CR |= DMA_SxCR_EN;
        GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 12U : 28U);
        __DSB();
        cycles = DWT->CYCCNT;
        SPI2->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
        for (;;)
        {
            const uint32_t low = DMA1->LISR;
            const uint32_t high = DMA1->HISR;
            if ((low & (DMA_LISR_TEIF3 | DMA_LISR_DMEIF3 | DMA_LISR_FEIF3)) != 0U ||
                (high & (DMA_HISR_TEIF4 | DMA_HISR_DMEIF4 | DMA_HISR_FEIF4)) != 0U ||
                (SPI2->SR & (SPI_SR_OVR | SPI_SR_MODF)) != 0U)
            {
                ok = false;
                break;
            }
            if ((low & DMA_LISR_TCIF3) != 0U && (high & DMA_HISR_TCIF4) != 0U &&
                DMA1_Stream3->NDTR == 0U && DMA1_Stream4->NDTR == 0U)
            {
                ok = spi2_wait(SPI_SR_TXE | SPI_SR_BSY, SPI_SR_TXE, cycles, budget);
                break;
            }
            if ((uint32_t)(DWT->CYCCNT - cycles) >= budget)
            {
                ok = false;
                break;
            }
        }
    }
    GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 28U : 12U);
    spi_console_duration_us =
        (DWT->CYCCNT - cycles + plan.system_hz / 1000000U - 1U) / (plan.system_hz / 1000000U);
    SPI2->CR2 = 0U;
    SPI2->CR1 &= ~SPI_CR1_SPE;
    if (!spi2_dma_stop(plan.system_hz))
    {
        ok = false;
    }
    if (!ok)
    {
        RCC->APB1RSTR |= RCC_APB1RSTR_SPI2RST;
        RCC->APB1RSTR &= ~RCC_APB1RSTR_SPI2RST;
    }
    (void)board_spi_console_config(command);
    return ok;
}

#include "board.h"
#include "i2c_console.h"
#include "spi_console.h"

uint32_t SystemCoreClock = 16000000U;
static volatile uint32_t tick_ms;
static uint32_t spi_console_duration_us;

uint32_t board_spi_console_duration_us(void)
{
    return spi_console_duration_us;
}

void SystemInit(void)
{
    /* Reset clock is HSI, 16 MHz. No dependence on the board's HSE crystal. */
    SCB->VTOR = 0x08000000U;
}

void SystemCoreClockUpdate(void)
{
    SystemCoreClock = 16000000U;
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

void board_init(void)
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
    /* Standalone bench: PB12 CS, PB13/14/15 SPI2 AF5. */
    GPIOB->BSRR = 1UL << 12U;
    output_mode(GPIOB, 12U);
    GPIOB->MODER = (GPIOB->MODER & ~0xFC000000UL) | 0xA8000000UL;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~0xFFF00000UL) | 0x55500000UL;
    GPIOB->OSPEEDR |= 0xFC000000UL; /* Very high speed for up to 20 MHz SPI2. */
    GPIOB->PUPDR &= ~0xFC000000UL;
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    (void)RCC->APB1ENR;

    board_i2c_init();

    /* PA5/6/7: AF5, SPI1 mode 0 at HSI/16 = 1 MHz. */
    GPIOA->MODER = (GPIOA->MODER & ~0x0000FC00UL) | 0x0000A800UL;
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~0xFFF00000UL) | 0x55500000UL;
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~0x0000FC00UL) | 0x00005400UL;
    GPIOA->PUPDR &= ~0x0000FC00UL;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN | RCC_APB2ENR_ADC1EN;
    (void)RCC->APB2ENR;
    SPI1->CR1 =
        SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1 | SPI_CR1_BR_0 | SPI_CR1_SPE;

    /* PA0/PA1: analog, no pull. ADC clock 8 MHz, 12 bit, software-triggered. */
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
        SPI1->CR1 =
            SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1 | SPI_CR1_BR_0 | SPI_CR1_SPE;
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
        SPI1->CR1 =
            SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1 | SPI_CR1_BR_0 | SPI_CR1_SPE;
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

static bool clock_wait(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    const uint32_t start = board_millis();
    while ((*reg & mask) != value)
    {
        if ((uint32_t)(board_millis() - start) >= 20U)
        {
            return false;
        }
    }
    return true;
}

static void set_tick_clock(uint32_t hz)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    SystemCoreClock = hz;
    SysTick->LOAD = hz / 1000U - 1U;
    SysTick->VAL = 0U;
    __set_PRIMASK(primask);
}

static void restore_clock(uint32_t cfgr, uint32_t acr, uint32_t adc_ccr)
{
    RCC->CFGR &= ~RCC_CFGR_SW;
    if (!clock_wait(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_HSI))
    {
        NVIC_SystemReset();
    }
    set_tick_clock(16000000U);
    RCC->CR &= ~RCC_CR_PLLON;
    RCC->CFGR = cfgr;
    FLASH->ACR = acr;
    ADC->CCR = adc_ccr;
}

static bool boost_clock(const spi_clock_plan_t *plan)
{
    uint32_t n;
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_3WS;
    if (!clock_wait(&FLASH->ACR, FLASH_ACR_LATENCY, FLASH_ACR_LATENCY_3WS))
    {
        return false;
    }
    ADC->CCR = (ADC->CCR & ~ADC_CCR_ADCPRE) | ADC_CCR_ADCPRE;
    RCC->CR &= ~RCC_CR_PLLON;
    if (!clock_wait(&RCC->CR, RCC_CR_PLLRDY, 0U))
    {
        return false;
    }
    PWR->CR |= PWR_CR_VOS;
    n = plan->system_hz / 500000U; /* HSI / 16 * N / 2. VCO 112..192 MHz. */
    RCC->PLLCFGR = 16U | (n << 6U) | (4UL << 24U);
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) | RCC_CFGR_PPRE1_DIV2;
    RCC->CR |= RCC_CR_PLLON;
    if (!clock_wait(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY))
    {
        return false;
    }
    if (!clock_wait(&PWR->CSR, PWR_CSR_VOSRDY, PWR_CSR_VOSRDY))
    {
        return false;
    }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    if (!clock_wait(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL))
    {
        return false;
    }
    set_tick_clock(plan->system_hz);
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
     * The transfer path selects the required PLL before producing any clocks. */
    SPI2->CR1 = cr1 | (plan.prescaler << 3U) | SPI_CR1_SPE;
    return true;
}

bool board_spi_console(const spi_command_t *command, uint8_t rx[SPI_CONSOLE_MAX_BYTES])
{
    spi_clock_plan_t plan;
    const uint32_t saved_cfgr = RCC->CFGR;
    const uint32_t saved_acr = FLASH->ACR;
    const uint32_t saved_adc = ADC->CCR;
    uint32_t cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
    uint32_t start;
    uint32_t cycles;
    size_t i;
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
    SPI2->CR1 = 0U;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 28U : 12U);
    if ((plan.system_hz != 16000000U) && !boost_clock(&plan))
    {
        restore_clock(saved_cfgr, saved_acr, saved_adc);
        return false;
    }
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
    SPI2->CR1 = cr1 | (plan.prescaler << 3U) | SPI_CR1_SPE;
    /* Let the configured idle clock settle before asserting CS. */
    for (i = 0U; i < 16U; ++i)
    {
        __NOP();
    }
    cycles = DWT->CYCCNT;
    GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 12U : 28U);
    start = board_millis();
    for (i = 0U; ok && (i < command->size); ++i)
    {
        while ((SPI2->SR & SPI_SR_TXE) == 0U)
        {
            if ((uint32_t)(board_millis() - start) >= 100U)
            {
                ok = false;
                break;
            }
        }
        if (!ok)
        {
            break;
        }
        *(__IO uint8_t *)&SPI2->DR = command->tx[i];
        while ((SPI2->SR & SPI_SR_RXNE) == 0U)
        {
            if ((uint32_t)(board_millis() - start) >= 100U)
            {
                ok = false;
                break;
            }
        }
        rx[i] = *(__IO uint8_t *)&SPI2->DR;
    }
    while (ok && ((SPI2->SR & SPI_SR_BSY) != 0U))
    {
        ok = (uint32_t)(board_millis() - start) < 100U;
    }
    GPIOB->BSRR = 1UL << (command->cs_high != 0U ? 28U : 12U);
    spi_console_duration_us =
        (DWT->CYCCNT - cycles + plan.system_hz / 1000000U - 1U) / (plan.system_hz / 1000000U);
    SPI2->CR1 &= ~SPI_CR1_SPE;
    if (!ok)
    {
        RCC->APB1RSTR |= RCC_APB1RSTR_SPI2RST;
        RCC->APB1RSTR &= ~RCC_APB1RSTR_SPI2RST;
    }
    if (plan.system_hz != 16000000U)
    {
        restore_clock(saved_cfgr, saved_acr, saved_adc);
    }
    (void)board_spi_console_config(command);
    return ok;
}

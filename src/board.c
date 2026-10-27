#include "board.h"

uint32_t SystemCoreClock = 16000000U;
static volatile uint32_t tick_ms;

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
    /* Set inactive levels before changing direction. Do not latch unknown 595 data. */
    GPIOA->BSRR = 1UL << 15U; /* W5500 CS */
    GPIOB->BSRR = (1UL << 10U) | (1UL << (15U + 16U)) | (1UL << (2U + 16U));
    GPIOC->BSRR = 1UL << 13U; /* LED off, active low */
    output_mode(GPIOA, 15U);
    output_mode(GPIOB, 10U); /* LCD deselected */
    output_mode(GPIOB, 15U); /* 595 latch stays low */
    output_mode(GPIOB, 2U);  /* W5500 reset */
    output_mode(GPIOC, 13U);

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

#include "clock.h"
#include "board.h"

uint32_t SystemCoreClock = 16000000U;
static volatile uint32_t clock_error;

uint32_t board_clock_error(void)
{
    return clock_error;
}

void SystemCoreClockUpdate(void)
{
    static const uint8_t shifts[16] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                       1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U};
    const uint32_t cfgr = RCC->CFGR;
    uint32_t hz;
    switch (cfgr & RCC_CFGR_SWS)
    {
    case RCC_CFGR_SWS_HSI:
        hz = 16000000U;
        break;
    case RCC_CFGR_SWS_HSE:
        hz = BOARD_HSE_HZ;
        break;
    case RCC_CFGR_SWS_PLL:
    {
        const uint32_t pll = RCC->PLLCFGR;
        const uint32_t input = (pll & RCC_PLLCFGR_PLLSRC_HSE) != 0U ? BOARD_HSE_HZ : 16000000U;
        const uint32_t m = pll & RCC_PLLCFGR_PLLM;
        const uint32_t n = (pll & RCC_PLLCFGR_PLLN) >> 6U;
        const uint32_t p = (((pll & RCC_PLLCFGR_PLLP) >> 16U) + 1U) * 2U;
        hz = m != 0U ? (uint32_t)((uint64_t)input * n / m / p) : 0U;
        break;
    }
    default:
        hz = 0U;
        break;
    }
    SystemCoreClock = hz >> shifts[(cfgr & RCC_CFGR_HPRE) >> 4U];
}

static bool wait_flag(volatile uint32_t *reg, uint32_t mask, uint32_t value, uint32_t cycles)
{
    const uint32_t start = DWT->CYCCNT;
    uint32_t attempts = 4000000U; /* Bound even if a debugger freezes CYCCNT. */
    while ((*reg & mask) != value)
    {
        if ((uint32_t)(DWT->CYCCNT - start) >= cycles || attempts-- == 0U)
        {
            return false;
        }
    }
    return true;
}

bool board_clock_init(void)
{
    clock_error = 1U;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    RCC->CR |= RCC_CR_HSION;
    if (!wait_flag(&RCC->CR, RCC_CR_HSIRDY, RCC_CR_HSIRDY, 1600000U))
    {
        return false;
    }
    RCC->CFGR &= ~RCC_CFGR_SW;
    if (!wait_flag(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_HSI, 1600000U))
    {
        return false;
    }
    RCC->CR &= ~(RCC_CR_CSSON | RCC_CR_PLLON | RCC_CR_PLLI2SON);
    clock_error = 2U;
    if (!wait_flag(&RCC->CR, RCC_CR_PLLRDY | RCC_CR_PLLI2SRDY, 0U, 1600000U))
    {
        return false;
    }
    RCC->CR &= ~RCC_CR_HSEON;
    if (!wait_flag(&RCC->CR, RCC_CR_HSERDY, 0U, 1600000U))
    {
        return false;
    }
    RCC->CR &= ~RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    clock_error = 3U;
    if (!wait_flag(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY, 1600000U))
    {
        return false;
    }
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_VOS; /* Scale 1, valid for the fixed 96 MHz core. */
    clock_error = 4U;
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_3WS | FLASH_ACR_PRFTEN |
                 FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    if (!wait_flag(&FLASH->ACR, FLASH_ACR_LATENCY, FLASH_ACR_LATENCY_3WS, 1600000U))
    {
        return false;
    }
    /* 25 MHz / M25 * N192 / P2 = 96 MHz; Q4 provides 48 MHz independently. */
    RCC->PLLCFGR = 25U | (192UL << 6U) | RCC_PLLCFGR_PLLSRC_HSE | (4UL << 24U);
    RCC->DCKCFGR &= ~RCC_DCKCFGR_TIMPRE;
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2)) |
                RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV8;
    RCC->CR |= RCC_CR_PLLON;
    clock_error = 5U;
    if (!wait_flag(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY, 1600000U))
    {
        return false;
    }
    /* On F411 VOS takes effect when PLL is enabled, not while PLL is off. */
    if (!wait_flag(&PWR->CSR, PWR_CSR_VOSRDY, PWR_CSR_VOSRDY, 1600000U))
    {
        clock_error = 4U;
        return false;
    }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    clock_error = 6U;
    if (!wait_flag(&RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL, 1600000U))
    {
        return false;
    }
    SystemCoreClockUpdate();
    if (SystemCoreClock != BOARD_CORE_HZ)
    {
        return false;
    }
    RCC->CR |= RCC_CR_CSSON;
    clock_error = 0U;
    return true;
}

__NO_RETURN void NMI_Handler(void)
{
    if ((RCC->CIR & RCC_CIR_CSSF) != 0U)
    {
        /* HSE failure forces HSI. Stop traffic before restarting the clock tree. */
        USART1->CR1 = 0U;
        USART1->CR3 = 0U;
        SPI1->CR1 = 0U;
        SPI2->CR2 = 0U;
        SPI2->CR1 = 0U;
        I2C1->CR1 = 0U;
        RCC->CIR = RCC_CIR_CSSC;
        NVIC_SystemReset();
    }
    for (;;)
    {
        __WFI();
    }
}


#include "board.h"
#include "i2c_console.h"

#define I2C_LINES ((1UL << 8U) | (1UL << 9U))
#define I2C_ERRORS (I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR)

static uint32_t transfer_start;
static i2c_error_t transfer_error;

void board_i2c_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;
    GPIOB->BSRR = I2C_LINES;
    GPIOB->OTYPER |= I2C_LINES;
    GPIOB->PUPDR &= ~(15UL << 16U); /* External pull-ups to 3.3 V. */
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR & ~(15UL << 16U)) | (5UL << 16U);
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~255UL) | 0x44U;
    GPIOB->MODER = (GPIOB->MODER & ~(15UL << 16U)) | (10UL << 16U);
}

static void configure(uint32_t hz)
{
    RCC->APB1RSTR |= RCC_APB1RSTR_I2C1RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;
    I2C1->CR1 = 0U;
    I2C1->CR2 = BOARD_PCLK1_HZ / 1000000U;
    I2C1->OAR1 = 1UL << 14U;
    I2C1->CCR = hz == 100000U ? 240U : I2C_CCR_FS | 40U;
    I2C1->TRISE = hz == 100000U ? 49U : 15U;
    I2C1->FLTR = 0U;
    I2C1->CR1 = I2C_CR1_PE;
}

static bool wait_flag(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
    for (;;)
    {
        const uint32_t status = I2C1->SR1;
        if ((status & I2C_ERRORS) != 0U)
        {
            transfer_error = (status & I2C_SR1_ARLO) != 0U ? I2C_ARBITRATION_LOST
                             : (status & I2C_SR1_AF) != 0U ? I2C_NACK
                                                           : I2C_BUS_ERROR;
            return false;
        }
        if ((uint32_t)(board_millis() - transfer_start) >= 100U)
        {
            transfer_error = I2C_TIMEOUT;
            return false;
        }
        if ((*reg & mask) == value)
        {
            return true;
        }
    }
}

static void clear_address(void)
{
    (void)I2C1->SR1;
    (void)I2C1->SR2;
}

static bool start_address(uint32_t address, bool read)
{
    I2C1->CR1 |= I2C_CR1_START;
    if (!wait_flag(&I2C1->SR1, I2C_SR1_SB, I2C_SR1_SB))
    {
        return false;
    }
    I2C1->DR = (address << 1U) | (read ? 1U : 0U);
    return wait_flag(&I2C1->SR1, I2C_SR1_ADDR, I2C_SR1_ADDR);
}

static bool receive_bytes(uint8_t *rx, size_t size, i2c_result_t *result)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (size <= 2U)
    {
        I2C1->CR1 &= ~I2C_CR1_ACK;
        if (size == 2U)
        {
            I2C1->CR1 |= I2C_CR1_POS;
        }
    }
    else
    {
        I2C1->CR1 |= I2C_CR1_ACK;
    }
    clear_address();
    if (size == 1U)
    {
        I2C1->CR1 |= I2C_CR1_STOP;
    }
    __set_PRIMASK(primask);
    while (size > 0U)
    {
        if (!wait_flag(&I2C1->SR1, size <= 3U && size != 1U ? I2C_SR1_BTF : I2C_SR1_RXNE,
                       size <= 3U && size != 1U ? I2C_SR1_BTF : I2C_SR1_RXNE))
        {
            return false;
        }
        if (size == 3U)
        {
            primask = __get_PRIMASK();
            __disable_irq();
            I2C1->CR1 &= ~I2C_CR1_ACK;
            rx[result->rx_done++] = (uint8_t)I2C1->DR;
            __set_PRIMASK(primask);
            --size;
            continue;
        }
        if (size == 2U)
        {
            primask = __get_PRIMASK();
            __disable_irq();
            I2C1->CR1 |= I2C_CR1_STOP;
            rx[result->rx_done++] = (uint8_t)I2C1->DR;
            rx[result->rx_done++] = (uint8_t)I2C1->DR;
            __set_PRIMASK(primask);
            size = 0U;
        }
        else
        {
            rx[result->rx_done++] = (uint8_t)I2C1->DR;
            --size;
        }
    }
    return true;
}

i2c_error_t board_i2c_console(const i2c_command_t *command, uint8_t rx[I2C_CONSOLE_MAX_BYTES],
                              i2c_result_t *result)
{
    uint32_t cycles;
    size_t i;
    bool ok = true;
    result->tx_done = 0U;
    result->rx_done = 0U;
    result->duration_us = 0U;
    result->actual_hz = command->hz;
    if (!i2c_command_valid(command) || SystemCoreClock != BOARD_CORE_HZ)
    {
        return I2C_INVALID;
    }
    if ((GPIOB->IDR & I2C_LINES) != I2C_LINES || (I2C1->SR2 & I2C_SR2_BUSY) != 0U)
    {
        return I2C_BUSY; /* Never pulse SCL or disturb a bus owned by another master. */
    }
    configure(command->hz);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cycles = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - cycles) < 5U * BOARD_CYCLES_PER_US)
    {
        __NOP(); /* Bus-free time >= 5 us at fixed HCLK. */
    }
    transfer_start = board_millis();
    transfer_error = I2C_OK;
    cycles = DWT->CYCCNT;
    if (command->size != 0U)
    {
        ok = start_address(command->address, false);
        if (ok)
        {
            clear_address();
        }
        for (i = 0U; ok && i < command->size; ++i)
        {
            ok = wait_flag(&I2C1->SR1, I2C_SR1_TXE, I2C_SR1_TXE);
            if (ok)
            {
                I2C1->DR = command->tx[i];
                ok = wait_flag(&I2C1->SR1, I2C_SR1_BTF, I2C_SR1_BTF);
                result->tx_done += ok ? 1U : 0U;
            }
        }
    }
    if (ok && command->read_size != 0U)
    {
        ok = start_address(command->address, true);
        if (ok)
        {
            ok = receive_bytes(rx, command->read_size, result);
        }
    }
    else if (ok)
    {
        I2C1->CR1 |= I2C_CR1_STOP;
    }
    if (ok)
    {
        ok = wait_flag(&I2C1->SR2, I2C_SR2_BUSY, 0U) && wait_flag(&I2C1->CR1, I2C_CR1_STOP, 0U);
    }
    result->duration_us = (uint32_t)(DWT->CYCCNT - cycles) / BOARD_CYCLES_PER_US;
    if (!ok && transfer_error != I2C_ARBITRATION_LOST && (I2C1->SR2 & I2C_SR2_MSL) != 0U)
    {
        I2C1->CR1 |= I2C_CR1_STOP;
        I2C1->SR1 &= ~I2C_ERRORS;
        /* Bound cleanup too; no retry of payload after an uncertain partial write. */
        transfer_start = board_millis();
        while ((I2C1->SR2 & I2C_SR2_BUSY) != 0U && (uint32_t)(board_millis() - transfer_start) < 2U)
        {
            __NOP();
        }
    }
    I2C1->CR1 = 0U; /* Release peripheral outputs; next explicit request reinitializes it. */
    I2C1->SR1 = 0U;
    return ok ? I2C_OK : transfer_error;
}

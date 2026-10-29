"""The immediate settings path must not write SPI2 DR or assert active CS."""
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2


def run(uc, symbols, call):
    writes = []
    hook = uc.hook_add(UC_HOOK_MEM_WRITE, lambda m, a, addr, size, value, data: writes.append(value),
                       begin=0x4000380C, end=0x4000380F)
    for mode in range(4):
        text = f'bus=2 mode={mode} hz=20000000 lsb=1 cs_high=0 format=hex\n00'.encode()
        uc.mem_write(0x20010000, text)
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, len(text))
        uc.reg_write(UC_ARM_REG_R2, 0x20018000)
        assert call(uc, symbols['spi_command_parse']) == 1
        uc.mem_write(0x40003808, (0).to_bytes(4, 'little'))
        uc.reg_write(UC_ARM_REG_R0, 0x20018000)
        assert call(uc, symbols['board_spi_console_config']) == 1
        cr1 = int.from_bytes(uc.mem_read(0x40003800, 4), 'little')
        assert cr1 & 3 == mode and cr1 & 64 and cr1 & 128
        assert int.from_bytes(uc.mem_read(0x40020418, 4), 'little') == 1 << 12
        assert int.from_bytes(uc.mem_read(symbols['SystemCoreClock'], 4), 'little') == 16000000
    before = bytes(uc.mem_read(0x40003800, 4))
    uc.mem_write(0x40003808, (128).to_bytes(4, 'little'))
    uc.reg_write(UC_ARM_REG_R0, 0x20018000)
    assert call(uc, symbols['board_spi_console_config']) == 0
    assert bytes(uc.mem_read(0x40003800, 4)) == before
    uc.mem_write(0x40003808, (0).to_bytes(4, 'little'))
    assert not writes
    uc.hook_del(hook)
    print('Immediate SPI config: CPOL/CPHA/LSB, inactive CS, no DR writes, no PLL change and busy rejection passed')

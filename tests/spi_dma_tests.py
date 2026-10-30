"""DMA register/ordering model; not a cycle-accurate STM32 bus simulation."""
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2

DMA = 0x40026000
RX = DMA + 0x58
TX = DMA + 0x70
SPI = 0x40003800


class DmaModel:
    def __init__(self, uc, fault=None, xor=0):
        self.uc, self.fault, self.xor = uc, fault, xor
        self.started = False
        self.phase = 0
        self.tail = 0
        self.resets = 0
        self.starts = 0
        self.last = b''
        self.active_cs = 0
        self.hooks = [uc.hook_add(UC_HOOK_MEM_WRITE, self.write, begin=DMA, end=DMA + 0xFF),
                      uc.hook_add(UC_HOOK_MEM_WRITE, self.start, begin=SPI + 4, end=SPI + 7),
                      uc.hook_add(UC_HOOK_MEM_READ, self.progress, begin=DMA, end=DMA + 7),
                      uc.hook_add(UC_HOOK_MEM_READ, self.status, begin=SPI + 8, end=SPI + 11),
                      uc.hook_add(UC_HOOK_MEM_READ, self.stream, begin=RX, end=TX + 3),
                      uc.hook_add(UC_HOOK_MEM_WRITE, self.reset, begin=0x40023810, end=0x40023813),
                      uc.hook_add(UC_HOOK_MEM_WRITE, self.cs, begin=0x40020418, end=0x4002041B),
                      uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.no_cpu_data,
                                  begin=SPI + 12, end=SPI + 15)]

    def get(self, addr):
        return int.from_bytes(self.uc.mem_read(addr, 4), 'little')

    def put(self, addr, value):
        self.uc.mem_write(addr, value.to_bytes(4, 'little'))

    def close(self):
        for hook in self.hooks:
            self.uc.hook_del(hook)

    def no_cpu_data(self, *args):
        raise AssertionError('CPU touched SPI2 DR during DMA transfer')

    def write(self, uc, access, addr, size, value, data):
        if addr == DMA + 8:
            self.put(DMA, self.get(DMA) & ~value)
        elif addr == DMA + 12:
            self.put(DMA + 4, self.get(DMA + 4) & ~value)

    def start(self, uc, access, addr, size, value, data):
        if value & 2:
            assert self.get(DMA) & 0x0F400000 == 0
            assert self.get(DMA + 4) & 0x3D == 0
            assert value & 1 and self.get(RX) & 1 and self.get(TX) & 1
            assert self.get(RX + 8) == SPI + 12 == self.get(TX + 8)
            assert self.get(RX) == 0x30401 and self.get(TX) == 0x20441
            assert self.get(RX + 20) == self.get(TX + 20) == 0
            count = self.get(RX + 4)
            assert 1 <= count <= 256 and self.get(TX + 4) == count
            cs = self.get(0x40020418)
            assert cs in (1 << 12, 1 << 28)
            self.active_cs = cs
            self.last = bytes(uc.mem_read(self.get(TX + 12), count))
            self.started, self.phase, self.tail = True, 0, 0
            self.starts += 1
            self.put(SPI + 8, 130)
        elif value & 1:
            assert self.get(RX) & 1 and not self.get(TX) & 1, 'RX must be armed first'
        elif self.started:
            if self.fault is None:
                assert self.phase >= 3 and self.tail >= 3, 'CS/requests ended before final bit'
            self.started = False

    def cs(self, uc, access, addr, size, value, data):
        if self.started and value != self.active_cs and self.fault is None:
            assert self.phase >= 3 and self.tail >= 3, 'CS released before final bit'

    def progress(self, uc, access, addr, size, value, data):
        if not self.started:
            return
        self.phase += 1
        errors = {'rx_te': (DMA, 1 << 25), 'rx_dme': (DMA, 1 << 24),
                  'rx_fe': (DMA, 1 << 22), 'tx_te': (DMA + 4, 8),
                  'tx_dme': (DMA + 4, 4), 'tx_fe': (DMA + 4, 1)}
        if self.fault in errors:
            address, flag = errors[self.fault]
            self.put(address, self.get(address) | flag)
            return
        if self.fault in ('stall', 'stop'):
            return
        if self.fault != 'tx_stall':
            self.put(TX + 4, 0)
            self.put(DMA + 4, self.get(DMA + 4) | 32)
        if self.phase >= 3 and self.fault != 'rx_stall':
            uc.mem_write(self.get(RX + 12), bytes(v ^ self.xor for v in self.last))
            self.put(RX + 4, 0)
            self.put(DMA, self.get(DMA) | (1 << 27))
        if self.fault in ('ovr', 'modf'):
            self.put(SPI + 8, 130 | (64 if self.fault == 'ovr' else 32))

    def status(self, uc, access, addr, size, value, data):
        if self.started and self.phase >= 3 and not self.fault:
            self.tail += 1
            if self.tail >= 3:
                self.put(SPI + 8, 2)

    def stream(self, uc, access, addr, size, value, data):
        if ((self.fault == 'stop' and self.starts) or
                (self.fault == 'prestop' and not self.resets)) and addr in (RX, TX):
            self.put(addr, self.get(addr) | 1)

    def reset(self, uc, access, addr, size, value, data):
        if value & (1 << 21):
            self.resets += 1
            uc.mem_write(DMA, bytes(0x100))


def run(uc, symbols, call):
    for length, hz, fault in [(1, 750000, None), (11, 12000000, None),
                              (11, 24000000, None), (11, 6000000, None),
                              (11, 24000000, None), (11, 24000000, None),
                              (256, 24000000, None),
                              (256, 187500, None)] + [(11, 12000000, f) for f in
                              ('stall', 'rx_stall', 'tx_stall', 'busy', 'rx_te', 'rx_dme', 'rx_fe',
                               'tx_te', 'tx_dme', 'tx_fe', 'ovr', 'modf', 'stop', 'prestop')]:
        uc.mem_write(DMA, bytes(0x100))
        uc.mem_write(SPI + 8, (2).to_bytes(4, 'little'))
        payload = bytes((i * 73 + 19) & 255 for i in range(length))
        body = (f'bus=2 mode=0 hz={hz} lsb=0 cs_high=0 format=hex\n' + payload.hex(' ')).encode()
        uc.mem_write(0x20010000, body)
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, len(body))
        uc.reg_write(UC_ARM_REG_R2, 0x20018000)
        assert call(uc, symbols['spi_command_parse']) == 1
        model = DmaModel(uc, fault, 0xA5)
        # Old TC/error flags must be cleared before reuse; unrelated stream flags survive.
        model.put(DMA, 0x0F40003D)
        model.put(DMA + 4, 0xF7D)
        tick = bytes(uc.mem_read(symbols['tick_ms'], 4))
        uc.mem_write(0xE0001004, (0xFFFFFF00).to_bytes(4, 'little'))
        uc.reg_write(UC_ARM_REG_R0, 0x20018000)
        uc.reg_write(UC_ARM_REG_R1, 0x20018400)
        result = call(uc, symbols['board_spi_console'])
        model.close()
        assert result == (fault is None), (length, hz, fault, result)
        assert model.starts == (0 if fault == 'prestop' else 1)
        assert model.last == (b'' if fault == 'prestop' else payload)
        assert bytes(uc.mem_read(symbols['tick_ms'], 4)) == tick
        assert not model.get(RX) & 1 and not model.get(TX) & 1
        assert model.get(SPI + 4) == 0
        assert model.get(0x40020418) == 1 << 12
        assert model.get(symbols['SystemCoreClock']) == 96000000
        if fault is None:
            assert bytes(uc.mem_read(0x20018400, length)) == bytes(v ^ 0xA5 for v in payload)
        if fault in ('stop', 'prestop'):
            assert model.resets >= 1
        else:
            assert model.get(DMA) & 0x3D == 0x3D
            assert model.get(DMA + 4) & 0xF40 == 0xF40
        # Every failure must be followed by a clean, successful transaction.
        if fault:
            uc.mem_write(SPI + 8, (2).to_bytes(4, 'little'))
            recovery = DmaModel(uc)
            uc.reg_write(UC_ARM_REG_R0, 0x20018000)
            uc.reg_write(UC_ARM_REG_R1, 0x20018400)
            assert call(uc, symbols['board_spi_console']) == 1
            recovery.close()
    print('SPI DMA: 1/11/256 bytes, RX-before-TX, byte widths/channel/priority, TC before BSY, frozen tick/wrap, six DMA errors, OVR/MODF/stalls, stop reset and recovery passed')

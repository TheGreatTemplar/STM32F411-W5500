"""Run the compiled polling pipeline with independent TX/RX registers.

This verifies ordering and failure handling, not physical bus timing.
"""
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2


def run(uc, symbols, call):
    for length, hz, fault in [(1, 1000000, None), (2, 10000000, None),
                              (256, 20000000, None), (256, 62500, None),
                              (4, 1000000, 'tx'), (4, 1000000, 'rx'),
                              (4, 1000000, 'busy'), (4, 1000000, 'ovr'),
                              (4, 1000000, 'modf')]:
        payload = bytes((i * 73 + 19) & 255 for i in range(length))
        body = (f'bus=2 mode=0 hz={hz} lsb=0 cs_high=0 format=hex\n' +
                payload.hex(' ')).encode()
        uc.mem_write(0x20010000, body)
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, len(body))
        uc.reg_write(UC_ARM_REG_R2, 0x20018000)
        assert call(uc, symbols['spi_command_parse']) == 1
        pending, events = [], []
        state = {'tx': 0, 'polls': 0, 'reset': False, 'sr': 0}

        def write(machine, access, address, size, value, data):
            assert size == 1
            assert state['sr'] & 2, 'TX before TXE'
            pending.append(value ^ 0xA5)
            events.append(('tx', value))
            state['tx'] += 1
            assert len(pending) <= 2

        def read(machine, access, address, size, value, data):
            assert size == 1 and pending
            assert state['sr'] & 1, 'RX before RXNE'
            machine.mem_write(address, bytes([pending.pop(0)]))
            events.append(('rx', None))

        def status(machine, access, address, size, value, data):
            state['polls'] += 1
            sr = 0 if state['polls'] % 3 == 1 else 2
            # Delay RX readiness; TX readiness is independent of RX.
            if pending and state['polls'] % 3 == 0:
                sr |= 1
            if fault == 'tx':
                sr = 0
            elif fault == 'rx':
                sr &= ~1
            elif fault == 'busy' and state['tx'] == length and not pending:
                sr |= 128
            elif fault == 'ovr' and len(pending) == 2:
                sr |= 64
            elif fault == 'modf' and pending:
                sr |= 32
            state['sr'] = sr
            machine.mem_write(address, sr.to_bytes(4, 'little'))

        def reset(machine, access, address, size, value, data):
            if value & (1 << 14):
                state['reset'] = True

        hooks = [uc.hook_add(UC_HOOK_MEM_WRITE, write, begin=0x4000380C, end=0x4000380C),
                 uc.hook_add(UC_HOOK_MEM_READ, read, begin=0x4000380C, end=0x4000380C),
                 uc.hook_add(UC_HOOK_MEM_READ, status, begin=0x40003808, end=0x40003808),
                 uc.hook_add(UC_HOOK_MEM_WRITE, reset, begin=0x40023820, end=0x40023820)]
        tick = bytes(uc.mem_read(symbols['tick_ms'], 4))
        # Exercise unsigned deadline arithmetic across the counter wrap.
        uc.mem_write(0xE0001004, (0xFFFFF000).to_bytes(4, 'little'))
        uc.reg_write(UC_ARM_REG_R0, 0x20018000)
        uc.reg_write(UC_ARM_REG_R1, 0x20018400)
        result = call(uc, symbols['board_spi_console'])
        for hook in hooks:
            uc.hook_del(hook)
        assert result == (fault is None), (length, hz, fault, result)
        assert bytes(uc.mem_read(symbols['tick_ms'], 4)) == tick
        assert int.from_bytes(uc.mem_read(0x40020418, 4), 'little') == 1 << 12
        if fault is None:
            assert not pending and not state['reset']
            assert bytes(uc.mem_read(0x20018400, length)) == bytes(b ^ 0xA5 for b in payload)
            assert [v for event, v in events if event == 'tx'] == list(payload)
            if length > 1:
                assert [event for event, v in events[:3]] == ['tx', 'tx', 'rx']
            if length == 256:
                assert int.from_bytes(uc.mem_read(0xE0001004, 4), 'little') < 0xFFFFF000
        else:
            assert state['reset']
        uc.mem_write(0x40003808, (0).to_bytes(4, 'little'))
    print('SPI pipeline: 1/2/256 bytes, lookahead, independent RX, DWT wrap/frozen SysTick, TX/RX/BSY timeout, OVR/MODF and CS cleanup passed')

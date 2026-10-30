"""Run the real I2C1 register driver in Unicorn with a protocol-aware peripheral model."""
import struct
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2


def run(uc, symbols, call):
    base = b'bus=1 hz=100000 addr=80 op=2 read=4 format=hex\n'

    def parse(body):
        uc.mem_write(0x20010000, body or b'\0')
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, len(body))
        uc.reg_write(UC_ARM_REG_R2, 0x20018000)
        ok = call(uc, symbols['i2c_command_parse'])
        return ok, bytes(uc.mem_read(0x20018000, 280))

    assert parse(base + b'00 10')[0] == 1
    assert parse(base + b'AB' * 256)[0] == 1
    for body in [base + b'AA' * 257, base + b'G0', base + b'A', base,
                 base.replace(b'addr=80', b'addr=7') + b'00',
                 base.replace(b'addr=80', b'addr=120') + b'00',
                 base.replace(b'hz=100000', b'hz=1000000') + b'00',
                 base.replace(b'bus=1', b'bus=2') + b'00',
                 base.replace(b'op=2', b'op=9') + b'00',
                 base.replace(b'read=4', b'read=0') + b'00',
                 base.replace(b'read=4', b'read=257') + b'00',
                 base.replace(b'read=4', b'read=42949672960') + b'00',
                 base.replace(b'addr=80', b'addr=0x50') + b'00',
                 base.replace(b'hz=100000', b'hz=100000 hz=100000') + b'00',
                 base.replace(b'format=hex', b'format=ascii') + b'\xD0\x90']:
        assert parse(body)[0] == 0, body
    assert parse(base.replace(b'op=2', b'op=1'))[0] == 1
    assert parse(base.replace(b'op=2', b'op=1') + b'00')[0] == 0
    assert parse(base.replace(b'op=2 read=4', b'op=0 read=0') + b'00')[0] == 1

    uc.mem_map(0x40005000, 0x1000)
    call(uc, symbols['board_i2c_init'])
    assert int.from_bytes(uc.mem_read(0x40020404, 4), 'little') & 0x300 == 0x300
    assert int.from_bytes(uc.mem_read(0x40020424, 4), 'little') & 0xFF == 0x44
    assert int.from_bytes(uc.mem_read(0x40020400, 4), 'little') & (15 << 16) == 10 << 16
    assert int.from_bytes(uc.mem_read(0x4002040C, 4), 'little') & (15 << 16) == 0
    reg = 0x40005400
    state = dict(steps=0, events=[], addressing=False, reading=False, read=0,
                 tx=[], length=0, fault=None)

    def read32(address):
        return int.from_bytes(uc.mem_read(address, 4), 'little')

    def write32(address, value):
        uc.mem_write(address, value.to_bytes(4, 'little'))

    def step(machine, address, size, unused):
        state['steps'] += 1
        if state['steps'] % 1000 == 0:
            write32(symbols['tick_ms'], (read32(symbols['tick_ms']) + 1) & 0xFFFFFFFF)
        cr = read32(reg)
        if cr & 0x100:
            state['events'].append('start')
            state['addressing'] = True
            write32(reg, cr & ~0x100)
            write32(reg + 0x18, 3)
            write32(reg + 0x14, 0 if state['fault'] == 'timeout' else 1)
        if cr & 0x200 and not read32(reg + 0x14) & (1 << 10):
            state['events'].append(('stop', state['read']))
            write32(reg, read32(reg) & ~0x200)
            write32(reg + 0x18, 0)

    def write_dr(machine, access, address, size, value, unused):
        if state['addressing']:
            state['addressing'] = False
            state['reading'] = bool(value & 1)
            state['events'].append(('address', value))
            if state['fault'] in {'nack', 'arlo', 'berr'}:
                write32(reg + 0x14, {'nack': 1 << 10, 'arlo': 1 << 9, 'berr': 1 << 8}[state['fault']])
                if state['fault'] == 'arlo':
                    write32(reg + 0x18, 2)
            else:
                write32(reg + 0x14, 2)  # ADDR, still clock-stretched until SR2 read.
        else:
            state['tx'].append(value & 255)
            write32(reg + 0x14, (1 << 10) if state['fault'] == 'data-nack' else 0x84)

    def read_reg(machine, access, address, size, value, unused):
        if address == reg + 0x18 and read32(reg + 0x14) & 2:
            if state['reading']:
                cr = read32(reg)
                if state['length'] == 1:
                    assert not cr & (0x400 | 0x800)
                elif state['length'] == 2:
                    assert not cr & 0x400 and cr & 0x800
                else:
                    assert cr & 0x400 and not cr & 0x800
            write32(reg + 0x14, 0xC4)  # TXE, RXNE, BTF.
        if address == reg + 0x10:
            assert state['reading']
            write32(reg + 0x10, state['read'] & 255)
            state['read'] += 1

    hooks = [uc.hook_add(UC_HOOK_CODE, step),
             uc.hook_add(UC_HOOK_MEM_WRITE, write_dr, begin=reg + 0x10, end=reg + 0x13),
             uc.hook_add(UC_HOOK_MEM_READ, read_reg, begin=reg + 0x10, end=reg + 0x1B)]

    def exchange(operation, length, hz=100000, fault=None, tx=b'00 10'):
        body = f'bus=1 hz={hz} addr=80 op={operation} read={length} format=hex\n'.encode()
        assert parse(body + (b'' if operation == 1 else tx))[0] == 1
        state.clear()
        state.update(steps=0, events=[], addressing=False, reading=False, read=0,
                     tx=[], length=length, fault=fault)
        write32(reg, 0)
        write32(reg + 0x14, 0)
        write32(reg + 0x18, 2 if fault == 'busy' else 0)
        write32(0x40020410, 0 if fault == 'lines' else 0x300)
        spi1 = read32(0x40013000)
        spi2 = read32(0x40003800)
        uc.reg_write(UC_ARM_REG_R0, 0x20018000)
        uc.reg_write(UC_ARM_REG_R1, 0x20018400)
        uc.reg_write(UC_ARM_REG_R2, 0x20018800)
        result = call(uc, symbols['board_i2c_console'], timeout=10000000)
        stats = struct.unpack('<4I', bytes(uc.mem_read(0x20018800, 16)))
        assert read32(0x40013000) == spi1 and read32(0x40003800) == spi2
        return result, stats

    for hz in [100000, 400000]:
        for op in [1, 2]:
            for length in [1, 2, 3, 4, 16, 256]:
                result, stats = exchange(op, length, hz)
                assert result == 0, (op, length, state)
                assert stats[0] == (2 if op == 2 else 0) and stats[1] == length
                assert stats[2] > 0 and stats[3] == (100000 if hz == 100000 else 400000)
                assert bytes(uc.mem_read(0x20018400, length)) == bytes(range(256))[:length]
                assert state['events'].count('start') == (2 if op == 2 else 1)
                assert [item for item in state['events'] if isinstance(item, tuple) and item[0] == 'stop'] == [('stop', max(0, length - 2))]
                addresses = [item[1] for item in state['events'] if isinstance(item, tuple) and item[0] == 'address']
                assert addresses == ([0xA0, 0xA1] if op == 2 else [0xA1])
                assert read32(reg + 0x1C) == (240 if hz == 100000 else 0x8028)
    assert exchange(0, 0)[0] == 0
    assert state['tx'] == [0, 16]
    assert exchange(0, 0, tx=b'AA' * 256)[0] == 0
    assert state['tx'] == [0xAA] * 256
    for fault, expected in [('nack', 1), ('data-nack', 1), ('lines', 2), ('busy', 2), ('timeout', 3), ('berr', 4), ('arlo', 5)]:
        result, stats = exchange(2, 4, fault=fault)
        assert result == expected, (fault, result, state)
        if fault in {'busy', 'lines'}:
            assert not state['events']
        if fault == 'arlo':
            assert not any(isinstance(item, tuple) and item[0] == 'stop' for item in state['events'])
        if fault in {'nack', 'data-nack'}:
            assert any(isinstance(item, tuple) and item[0] == 'stop' for item in state['events'])
    assert exchange(2, 2)[0] == 0  # A later explicit request recovers after errors.
    for hook in hooks:
        uc.hook_del(hook)
    print('I2C1: strict parser, PB8/PB9 AF4 open-drain, read 1/2/3/256, repeated START, ACK/POS/STOP, NACK/busy/timeout/arbitration and SPI isolation passed')

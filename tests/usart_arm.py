"""USART1 register/DMA tests against the compiled firmware, using Unicorn."""
import json
import struct
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build/test_deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2

root = Path(__file__).resolve().parents[1]
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
for address, size in [(0x08000000, 0x80000), (0x20000000, 0x20000),
                      (0x10000000, 0x1000), (0x40020000, 0x10000),
                      (0x40011000, 0x3000), (0xE000E000, 0x2000)]:
    uc.mem_map(address, size)
with (root / 'build/adc_web.axf').open('rb') as source:
    elf = ELFFile(source)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            uc.mem_write(segment['p_vaddr'], segment.data())
    symbols = {symbol.name: symbol['st_value'] for symbol in elf.get_section_by_name('.symtab').iter_symbols()}

def call(name, *args):
    uc.reg_write(UC_ARM_REG_SP, 0x2001FF00)
    uc.reg_write(UC_ARM_REG_LR, 0x10000001)
    for register, value in zip([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2], args):
        uc.reg_write(register, value)
    uc.emu_start(symbols[name] | 1, 0x10000000, count=3000000)
    assert uc.reg_read(UC_ARM_REG_PC) == 0x10000000, name
    return uc.reg_read(UC_ARM_REG_R0)

def read(address):
    return int.from_bytes(uc.mem_read(address, 4), 'little')

def write(address, value):
    uc.mem_write(address, struct.pack('<I', value))

def command(text):
    data = text.encode()
    uc.mem_write(0x2001B000, data)
    return call('usart_control', 0x2001B000, len(data))

def snapshot(cursor=0):
    size = call('usart_json', 0x2001B000, 4096, cursor)
    assert size
    return json.loads(bytes(uc.mem_read(0x2001B000, size)))

call('usart_init')
for irq, handler in [(37, 'USART1_IRQHandler'), (58, 'DMA2_Stream2_IRQHandler')]:
    assert read(symbols['__Vectors'] + (16 + irq) * 4) == symbols[handler] | 1
assert not snapshot()['enabled']
assert snapshot()['baud'] == 115200
assert read(0x40020024) & 0xFF0 == 0x770  # PA9/PA10 AF7
assert command('start baud=115200 bits=8 parity=0 stop=1\n') == 0
assert snapshot()['enabled']
assert read(0x40011008) == 104
assert read(0x40026440) & (7 << 25) == 4 << 25  # RX stream2 channel4
assert read(0x40026440) & ((1 << 8) | (1 << 4) | (1 << 3)) == (1 << 8) | (1 << 4) | (1 << 3)
before = read(0x40011008)
for invalid in ['start baud=0 bits=8 parity=0 stop=1\n',
                'start baud=115200 bits=7 parity=0 stop=1\n',
                'start baud=115200 bits=9 parity=0 stop=1\n',
                'start baud=115200 bits=8 parity=3 stop=1\n',
                'start baud=115200 bits=8 parity=0 stop=1\nx',
                'tx format=0 eol=0\nGG', 'tx format=0 eol=0\nA',
                'tx format=1 eol=0\n日本', 'tx format=0 eol=4\n55']:
    assert command(invalid) == 1, invalid
    assert read(0x40011008) == before

assert command('tx format=0 eol=3\n55 AA 00 FF') == 0
tx_pointer = read(0x400264C4)
assert read(0x400264BC) == 6
assert bytes(uc.mem_read(tx_pointer, 6)) == b'\x55\xaa\0\xff\r\n'
assert command('tx format=0 eol=0\n01') == 2
assert command('stop\n') == 2
assert command('start baud=9600 bits=8 parity=0 stop=1\n') == 2
write(0x400264BC, 0)  # DMA exhausted, final stop bit has not completed.
write(0x40011000, 0)
assert snapshot()['tx_busy']
write(0x40011000, 1 << 6)
assert snapshot()['tx_total'] == 6
assert not snapshot()['tx_busy']

rx_pointer = read(0x4002644C)
position = 0
total = 0
def receive(data):
    global position, total
    for value in data:
        uc.mem_write(rx_pointer + position, bytes([value]))
        position = (position + 1) % 2048
    total += len(data)
    write(0x40026444, 2048 - position)
    call('DMA2_Stream2_IRQHandler')

receive(b'\x55\xaa\0\xff')
first = snapshot()
assert first['rx'] == '55AA00FF' and first['next'] == 4
assert snapshot()['rx'] == first['rx']  # A second browser retains an independent cursor.
assert snapshot(4)['rx'] == ''
for _ in range(10):
    receive(bytes(range(256)) * 4)
late = snapshot(0)
assert late['lost'] == total - 8192
assert late['more'] == 8192 - 1024 and len(late['rx']) == 2048
assert snapshot(total)['rx'] == ''

# SPI2 DMA1 must preserve an active USART1 DMA2 transfer and RX cursor.
from spi_dma_tests import DmaModel
from unicorn import UC_HOOK_CODE
uc.mem_map(0x40003000, 0x3000)
uc.mem_map(0xE0001000, 0x1000)
write(symbols['SystemCoreClock'], 96000000)
write(0x40023800, 0x030B0003)
write(0x40023808, 0xD00A)
write(0x40003808, 2)
assert command('tx format=0 eol=0\n11 22 33') == 0
saved = {a: read(a) for a in [0x40011008,0x4001100C,0x40011014,
                             0x40026440,0x400264B8,0x400264BC,0x400264C4,
                             0x40023800,0x40023808,0x40013000]}
body = b'bus=2 mode=0 hz=24000000 lsb=0 cs_high=0 format=hex\n55 AA 00 FF'
uc.mem_write(0x2001B000, body)
assert call('spi_command_parse', 0x2001B000, len(body), 0x2001C000)
def cycles(machine, address, size, unused):
    write(0xE0001004, (read(0xE0001004) + 1) & 0xFFFFFFFF)
hook = uc.hook_add(UC_HOOK_CODE, cycles)
model = DmaModel(uc, xor=0xA5)
assert call('board_spi_console', 0x2001C000, 0x2001C400)
model.close()
uc.hook_del(hook)
assert model.last == bytes.fromhex('55 AA 00 FF')
assert all(read(a) == value for a, value in saved.items())
receive(b'\x12\x34')
assert snapshot(total - 2)['rx'] == '1234'
assert snapshot()['tx_busy'] and snapshot()['enabled']
write(0x400264BC, 0)
write(0x40011000, 1 << 6)
assert not snapshot()['tx_busy']

for baud in [300,600,1200,2400,4800,9600,19200,38400,57600,115200,230400,460800,500000,921600,1000000]:
    for bits, par in [(8, 0), (8, 1), (8, 2), (7, 1), (7, 2)]:
        assert command(f'start baud={baud} bits={bits} parity={par} stop=2\n') == 0
        divider = (12000000 + baud // 2) // baud
        assert read(0x40011008) == (((divider//8)<<4)|(divider%8) if divider<16 else divider)
        assert bool(read(0x4001100C) & (1<<15)) == (divider<16)
        assert bool(read(0x4001100C) & (1 << 12)) == (bits == 8 and par != 0)
assert command('tx format=0 eol=0\nFF') == 1  # 7-bit mode rejects truncation.
assert command('stop\n') == 0
assert not snapshot()['enabled']
assert command('tx format=0 eol=0\n55') == 3
assert command('start baud=115200 bits=8 parity=0 stop=1\n') == 0
write(0x40011000, 0xF)
call('USART1_IRQHandler')
assert snapshot(total)['errors'][:4] == [1, 1, 1, 1]
write(0x40026400, 1 << 19)  # RX DMA transfer error.
call('DMA2_Stream2_IRQHandler')
assert not snapshot()['enabled']
assert snapshot(total)['errors'][4] == 1
write(0x40026400, 0)
assert command('start baud=115200 bits=8 parity=0 stop=1\n') == 0
assert command('tx format=0 eol=0\n55') == 0
write(symbols['tick_ms'], 1000)
assert snapshot(total)['errors'][5] == 1
assert not snapshot()['enabled']
assert command('start baud=115200 bits=8 parity=0 stop=1\n') == 0
assert command('stop\n') == 0
assert not snapshot()['enabled']

print('USART1: DMA2 mapping, 75 frame/rate modes, TX completion, independent RX cursors, wrap/overflow, strict input, errors passed')

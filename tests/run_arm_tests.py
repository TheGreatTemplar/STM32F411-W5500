"""Execute compiled ARM code with Unicorn; no STM32/W5500 hardware is emulated.

Install test-only dependencies with: python -m pip install unicorn==2.1.4 pyelftools==0.33
Run after scripts/build.ps1 and the mock build described in tests/README.md.
"""
from pathlib import Path
import sys
import struct

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "build/test_deps"))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC


def load_image(path):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(0x08000000, 0x80000)
    uc.mem_map(0x20000000, 0x200000 if path.name == "store_test.axf" else (0x80000 if path.name == "http_test.axf" else 0x20000))
    uc.mem_map(0x10000000, 0x1000)
    with path.open("rb") as source:
        elf = ELFFile(source)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                uc.mem_write(segment["p_vaddr"], segment.data())
        symbols = {symbol.name: symbol["st_value"]
                   for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    uc.reg_write(UC_ARM_REG_SP, 0x201FF000 if path.name == "store_test.axf" else (0x2007F000 if path.name == "http_test.axf" else 0x2001F000))
    uc.reg_write(UC_ARM_REG_LR, 0x10000001)
    return uc, symbols


def call(uc, address, timeout=5000000, count=20000000):
    uc.reg_write(UC_ARM_REG_LR, 0x10000001)
    uc.emu_start(address | 1, 0x10000000, timeout=timeout, count=count)
    assert uc.reg_read(UC_ARM_REG_PC) == 0x10000000, 'ARM call did not return within the execution limit'
    return uc.reg_read(UC_ARM_REG_R0)


uc, symbols = load_image(root / "build/adc_web.axf")
uc.mem_map(0x40020000, 0x3000)
uc.mem_write(0x40020814, (0).to_bytes(4, "little"))
assert call(uc, symbols["board_led_is_on"]) == 1
uc.mem_write(0x40020814, (1 << 13).to_bytes(4, "little"))
assert call(uc, symbols["board_led_is_on"]) == 0
for on, expected in [(1, 1 << 29), (0, 1 << 13)]:
    uc.reg_write(UC_ARM_REG_R0, on)
    call(uc, symbols["board_led"])
    assert int.from_bytes(uc.mem_read(0x40020818, 4), "little") == expected
print("PC13 LED: active-low GPIO readback and BSRR writes passed")
settings_body = b'ip=172.16.236.221\nmask=255.255.255.0\ngateway=172.16.236.1\nport=5026\ncal=-36\nseq=0\n'
for path, route in [('/api/settings', 20), ('/api/settings/apply', 21)]:
    request = (f'POST {path} HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: {len(settings_body)}\r\n\r\n'.encode() + settings_body)
    for prefix in range(len(request) + 1):
        uc.mem_write(0x20010000, request[:prefix])
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, prefix)
        assert call(uc, symbols['http_parse']) == (route if prefix == len(request) else 0)
    missing_control = request.replace(b'X-Device-Control: 1\r\n', b'')
    uc.mem_write(0x20010000, missing_control)
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, len(missing_control))
    assert call(uc, symbols['http_parse']) == 4
for suffix in [b'', b'x', b'\n']:
    body = settings_body + suffix
    uc.mem_write(0x20010000, body)
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, len(body))
    uc.reg_write(UC_ARM_REG_R2, 0x20018000)
    from unicorn.arm_const import UC_ARM_REG_R3
    uc.reg_write(UC_ARM_REG_R3, 0x20018100)
    assert call(uc, symbols['bench_settings_parse']) == (not suffix)
print('System routes: fragmented protected POST, strict settings parser and trailing-data rejection passed')

spi_body = b"bus=2 mode=0 hz=1000000 lsb=0 cs_high=0 format=hex\n9F 00 00 00"
spi_request = (b"POST /api/spi HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: " +
               str(len(spi_body)).encode() + b"\r\n\r\n" + spi_body)
requests = [
    (b"GET / HTTP/1.1\r\nHost: board\r\n\r\n", 1),
    (b"GET /api/adc HTTP/1.1\r\nHost: board\r\n\r\n", 2),
    (b"GET /api/adc HTTP/1.0\r\n\r\n", 2),
    (b"GET /missing HTTP/1.1\r\n\r\n", 3),
    (b"POST /api/adc HTTP/1.1\r\n\r\n", 5),
    (b"GET / HTTP/2.0\r\n\r\n", 4),
    (b"GET /api/adcX HTTP/1.1\r\n\r\n", 3),
    (b"GET / HTTP/1.1\r\nX: a\x00b\r\n\r\n", 4),
    (b"GET / HTTP/1.1\r\nX: a\x7fb\r\n\r\n", 4),
    (b"\r\n\r\n", 4),
    (b"POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 0\r\n\r\n", 6),
    (b"POST /api/led/off HTTP/1.1\r\nx-device-control: 1\r\ncontent-length: 0\r\n\r\n", 7),
    (b"GET /api/led/on HTTP/1.1\r\n\r\n", 8),
    (b"GET /api/led/off HTTP/1.1\r\n\r\n", 8),
    (b"POST /api/led/on HTTP/1.1\r\n\r\n", 4),
    (b"POST /api/led/on HTTP/1.1\r\nX-Device-Control: 0\r\n\r\n", 4),
    (b"POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\nTransfer-Encoding: chunked\r\n\r\n", 4),
    (b"POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 1\r\n\r\nx", 4),
    (b"POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n", 4),
    (b"POST /api/led/on HTTP/1.1\r\nX-Device-Control: 1\r\n\r\nx", 4),
    (b"POST /api/led/onX HTTP/1.1\r\nX-Device-Control: 1\r\n\r\n", 5),
    (b"GET /api/diagnostics HTTP/1.1\r\n\r\n", 9),
    (b"GET /api/mcu HTTP/1.1\r\n\r\n", 22),
    (b"GET /api/mcuX HTTP/1.1\r\n\r\n", 3),
    (b"POST /api/mcu HTTP/1.1\r\n\r\n", 5),
    (b"GET /api/diagnosticsX HTTP/1.1\r\n\r\n", 3),
    (b"POST /api/diagnostics HTTP/1.1\r\n\r\n", 5),
    (b"GET /api/commands HTTP/1.1\r\n\r\n", 11),
    (b"GET /api/library HTTP/1.1\r\n\r\n", 15),
    (spi_request.replace(b"/api/spi", b"/api/library"), 16),
    (b"GET /api/rtc HTTP/1.1\r\n\r\n", 13),
    (spi_request.replace(b"/api/spi", b"/api/rtc"), 14),
    (spi_request.replace(b"/api/spi", b"/api/commands"), 12),
    (spi_request, 10),
    (spi_request.replace(b"/api/spi", b"/api/i2c"), 17),
    (spi_request.replace(b"/api/spi", b"/api/spi/config"), 18),
    (b"GET /api/spi/config HTTP/1.1\r\n\r\n", 8),
    (b"GET /api/i2c HTTP/1.1\r\n\r\n", 8),
    (spi_request[:-1], 0),
    (b"GET /api/spi HTTP/1.1\r\n\r\n", 8),
    (b"POST /api/spi HTTP/1.1\r\nX-Device-Control: 1\r\nContent-Length: 9999999999999999\r\n\r\n", 4),
]
count = 0
for request, expected in requests:
    uc.mem_write(0x20010000, request)
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, len(request))
    result = call(uc, symbols["http_parse"])
    assert result == expected, (request, result, expected)
    count += 1
for request, route in requests:
    if route not in {1, 2, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18}:
        continue
    for length in range(len(request)):
        uc.mem_write(0x20010000, request)
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, length)
        assert call(uc, symbols["http_parse"]) == 0, (request, length)
        count += 1
print(f"HTTP parser: {count} cases passed (including every fragmented prefix)")

def parse_spi(body):
    uc.mem_write(0x20010000, body or b"\0")
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, len(body))
    from unicorn.arm_const import UC_ARM_REG_R2
    uc.reg_write(UC_ARM_REG_R2, 0x20018000)
    result = call(uc, symbols["spi_command_parse"])
    return result, bytes(uc.mem_read(0x20018000, 280))

from unicorn import UC_HOOK_CODE

def model_clock(machine, address, size, data):
    cycles = int.from_bytes(machine.mem_read(0xE0001004, 4), 'little')
    # Synthetic counter advancement for deadline tests, not a cycle-accurate CPU.
    machine.mem_write(0xE0001004, ((cycles + 1) & 0xFFFFFFFF).to_bytes(4, 'little'))
    cr = int.from_bytes(machine.mem_read(0x40023800, 4), 'little')
    ready = (cr | (1 << 25)) if cr & (1 << 24) else (cr & ~(1 << 25))
    machine.mem_write(0x40023800, ready.to_bytes(4, 'little'))
    cfgr = int.from_bytes(machine.mem_read(0x40023808, 4), 'little')
    machine.mem_write(0x40023808, ((cfgr & ~12) | ((cfgr & 3) << 2)).to_bytes(4, 'little'))
    machine.mem_write(0x40007004, ((1 << 14) if cr & (1 << 24) else 0).to_bytes(4, 'little'))

from spi_dma_tests import DmaModel
spi_cases = 0
for mode in range(4):
    for hz in [62500, 125000, 250000, 500000, 1000000, *range(2000000, 24000001, 2000000), 25000000]:
        body = f"bus=2 mode={mode} hz={hz} lsb=1 cs_high=0 format=hex\n9F 00 aA FF".encode()
        ok, command = parse_spi(body)
        assert ok and struct.unpack_from('<5I', command) == (hz, mode, 1, 0, 4)
        assert command[20:24] == b"\x9F\x00\xAA\xFF"
        spi_cases += 1
        # Exercise the actual SPI2 register code with immediately ready mock registers.
        uc.mem_map(0x40003000, 0x1000) if spi_cases == 1 else None
        uc.mem_map(0x40013000, 0x1000) if spi_cases == 1 else None
        uc.mem_map(0x40023000, 0x1000) if spi_cases == 1 else None
        if spi_cases == 1:
            uc.mem_map(0x40007000, 0x1000)
            uc.mem_map(0x40012000, 0x1000)
            uc.mem_map(0xE000E000, 0x1000)
            uc.mem_map(0xE0001000, 0x1000)
            uc.hook_add(UC_HOOK_CODE, model_clock)
            uc.mem_map(0x40026000, 0x1000)
            dma_model = DmaModel(uc)
        uc.reg_write(UC_ARM_REG_R0, hz)
        uc.reg_write(UC_ARM_REG_R1, 0x20018800)
        assert call(uc, symbols['spi_clock_plan']) == 1
        system_hz, pclk1_hz, divisor = struct.unpack('<3I', bytes(uc.mem_read(0x20018800, 12)))
        assert system_hz <= 100000000 and pclk1_hz <= 50000000
        assert pclk1_hz // (2 << divisor) == hz
        uc.mem_write(0x40012304, (0x00400000).to_bytes(4, 'little'))
        uc.mem_write(0x40003808, (3).to_bytes(4, 'little'))  # RXNE | TXE
        uc.mem_write(0x40013000, (0x035C).to_bytes(4, 'little'))
        uc.reg_write(UC_ARM_REG_R0, 0x20018000)
        uc.reg_write(UC_ARM_REG_R1, 0x20018400)
        assert call(uc, symbols['board_spi_console']) == 1
        assert bytes(uc.mem_read(0x20018400, 4)) == command[20:24]
        assert call(uc, symbols['board_spi_console_duration_us']) > 0
        cr1 = int.from_bytes(uc.mem_read(0x40003800, 4), 'little')
        assert cr1 & 3 == mode and cr1 & 128 and cr1 & 64
        assert pclk1_hz // (2 << ((cr1 >> 3) & 7)) == hz
        if system_hz != 16000000:
            assert int.from_bytes(uc.mem_read(symbols['SystemCoreClock'], 4), 'little') == 16000000
            assert int.from_bytes(uc.mem_read(0xE000E014, 4), 'little') == 15999
            assert int.from_bytes(uc.mem_read(0x40023808, 4), 'little') == 0
            assert int.from_bytes(uc.mem_read(0x40012304, 4), 'little') == 0x00400000
        assert int.from_bytes(uc.mem_read(0x40020418, 4), 'little') == 1 << 12
        assert int.from_bytes(uc.mem_read(0x40013000, 4), 'little') == 0x035C

base = b"bus=2 mode=0 hz=1000000 lsb=0 cs_high=0 format=hex\n"
for invalid_hz in [0, 23000000, 25000001, 26000000, 50000000]:
    assert parse_spi(base.replace(b'hz=1000000', f'hz={invalid_hz}'.encode()) + b'00')[0] == 0
assert parse_spi(base + b'00 ' * 256)[0] == 1
assert parse_spi(base + b'00 ' * 257)[0] == 0
for bad in [b'', base, base + b'9', base + b'GG',
            base.replace(b'bus=2', b'bus=1') + b'00',
            base.replace(b'mode=0', b'mode=4') + b'00',
            base.replace(b'hz=1000000', b'hz=333333') + b'00',
            base.replace(b'lsb=0', b'lsb=2') + b'00',
            base.replace(b'cs_high=0', b'cs_high=9') + b'00',
            base.replace(b'bus=2', b'bus=2 bus=2') + b'00',
            base.replace(b'bus=2', b'hz=2') + b'00',
            base.replace(b'hz=1000000', b'hz=99999999999999999999999') + b'00']:
    assert parse_spi(bad)[0] == 0, bad
ascii_base = base.replace(b'format=hex', b'format=ascii')
assert parse_spi(ascii_base + b'*IDN?\n')[1][20:26] == b'*IDN?\n'
assert parse_spi(ascii_base + b'\xD0\x90')[0] == 0
assert parse_spi(ascii_base + b'\x00')[0] == 0
ok, command = parse_spi(base.replace(b'cs_high=0', b'cs_high=1') + b'55 AA')
assert ok
uc.reg_write(UC_ARM_REG_R0, 0x20018000)
uc.reg_write(UC_ARM_REG_R1, 0x20018400)
assert call(uc, symbols['board_spi_console']) == 1
assert int.from_bytes(uc.mem_read(0x40020418, 4), 'little') == 1 << 28
assert not int.from_bytes(uc.mem_read(0x40003800, 4), 'little') & 128  # MSB first
steps = [0]


def advance_tick(machine, address, size, data):
    steps[0] += 1
    if steps[0] % 64 == 0:
        tick = int.from_bytes(machine.mem_read(symbols['tick_ms'], 4), 'little')
        machine.mem_write(symbols['tick_ms'], (tick + 1).to_bytes(4, 'little'))


dma_model.close()
print('SPI console: 72 mode/rate configurations through 25 MHz, PLL switching/restoration, register loopback, limits and invalid input passed')
from i2c_register_tests import run as run_i2c_register_tests
from spi_config_tests import run as run_spi_config_tests
run_spi_config_tests(uc, symbols, call)
from spi_dma_tests import run as run_spi_dma_tests
run_spi_dma_tests(uc, symbols, call)
run_i2c_register_tests(uc, symbols, call)
uc, symbols = load_image(root / "build/w5500_test.axf")
result = call(uc, symbols["test_entry"])
assert result == 0, f"W5500 mock failed at C source line {result}"
print("W5500 mock: four sockets, static config, RX/TX ring wrap, nonblocking sends and recovery passed")
uc, symbols = load_image(root / "build/http_test.axf")
result = call(uc, symbols["test_entry"])
assert result == 0, f"HTTP integration mock failed at C source line {result}"
print("HTTP integration: four clients, stalled connection isolation, ADC/LED/SPI and response framing passed")

uc, symbols = load_image(root / "build/store_test.axf")
result = call(uc, symbols["test_entry"], timeout=120000000, count=1500000000)
assert result == 0, f"Flash journal mock failed at C source line {result}"
print("W25Q64JV: independent 64+64 banks, UTF-8, full quotas, v1/v2 compatibility, interrupted I2C migration before/after commit, latest-set preservation, CRC and foreign-data protection passed")

# Run the real calendar parser and RTC register code, without a ticking oscillator.
import json
from datetime import date as calendar_date
from unicorn.arm_const import UC_ARM_REG_R2
uc, symbols = load_image(root / "build/adc_web.axf")
for base_address in [0x40002000, 0x40007000, 0x40023000]:
    uc.mem_map(base_address, 0x1000)

def parse_rtc(text):
    body = text.encode()
    uc.mem_write(0x20010000, body or b'\0')
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, len(body))
    uc.reg_write(UC_ARM_REG_R2, 0x20018000)
    return call(uc, symbols['rtc_parse'])

for year in [2000, 2001, 2024, 2032, 2099]:
    for month in range(1, 13):
        for day in [28, 29, 30, 31]:
            try:
                calendar_date(year, month, day)
                valid = True
            except ValueError:
                valid = False
            assert bool(parse_rtc(f'{year:04}-{month:02}-{day:02}T23:59:59')) == valid
for text in ['', '1999-12-31T00:00:00', '2100-01-01T00:00:00', '2026-00-01T00:00:00',
             '2026-13-01T00:00:00', '2026-01-00T00:00:00', '2026-01-01T24:00:00',
             '2026-01-01T00:60:00', '2026-01-01T00:00:60', '2026-01-01 00:00:00',
             '2026-01-01T00:00:00Z', '2026-01-01T00:00:0X']:
    assert parse_rtc(text) == 0, text

def write_register(address, value):
    uc.mem_write(address, value.to_bytes(4, 'little'))

def read_register(address):
    return int.from_bytes(uc.mem_read(address, 4), 'little')

def read_rtc():
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, 160)
    size = call(uc, symbols['rtc_json'])
    assert 0 < size < 160
    return json.loads(bytes(uc.mem_read(0x20010000, size)))

write_register(0x40023870, 2) # LSE ready
write_register(0x4000280C, 64) # INITF
call(uc, symbols['rtc_init'])
from unicorn import UC_HOOK_MEM_WRITE
prescaler_writes = []
def watch_prescaler(machine, access, address, size, value, data):
    prescaler_writes.append(value)
prescaler_hook = uc.hook_add(UC_HOOK_MEM_WRITE, watch_prescaler, begin=0x40002810, end=0x40002813)
call(uc, symbols['rtc_poll'])
uc.hook_del(prescaler_hook)
assert prescaler_writes == [255, 0x007F00FF]
assert read_rtc()['ready'] and not read_rtc()['set']
assert read_register(0x40002810) == 0x007F00FF
assert read_register(0x40023870) & 0x8100 == 0x8100
assert parse_rtc('2032-02-29T12:34:56')
uc.reg_write(UC_ARM_REG_R0, 0x20018000)
assert call(uc, symbols['rtc_set']) == 1
assert read_register(0x40002800) == 0x00123456
assert (read_register(0x40002804) >> 13) & 7 == calendar_date(2032, 2, 29).isoweekday()
assert read_rtc()['datetime'] == '2032-02-29T12:34:56'
retained = bytes(uc.mem_read(0x40002800, 8))
call(uc, symbols['rtc_init'])
call(uc, symbols['rtc_poll'])
assert bytes(uc.mem_read(0x40002800, 8)) == retained
assert read_rtc()['set']
assert parse_rtc('2000-01-01T00:00:00')
uc.reg_write(UC_ARM_REG_R0, 0x20018000)
assert call(uc, symbols['rtc_set']) == 1
assert read_rtc()['datetime'] == '2000-01-01T00:00:00'
write_register(0x40002800, 0x002A0000) # Invalid BCD hour
assert not read_rtc()['set'] and read_rtc()['error'] == 5
write_register(0x40002810, 0)
call(uc, symbols['rtc_init'])
call(uc, symbols['rtc_poll'])
assert read_rtc()['error'] == 4
write_register(0x40023870, 0x8200) # Foreign LSI source; no backup-domain reset allowed.
call(uc, symbols['rtc_init'])
assert read_rtc()['error'] == 1 and read_register(0x40023870) == 0x8200
write_register(0x40023870, 0)
call(uc, symbols['rtc_init'])
write_register(symbols['tick_ms'], read_register(symbols['tick_ms']) + 8000)
call(uc, symbols['rtc_poll'])
assert read_rtc()['error'] == 2 and not read_rtc()['ready']
write_register(0x40023870, 2)
write_register(0x40002850, 0) # No retained marker
write_register(0x4000280C, 0) # INITF never arrives
call(uc, symbols['rtc_init'])
hook = uc.hook_add(UC_HOOK_CODE, advance_tick)
call(uc, symbols['rtc_poll'])
uc.hook_del(hook)
assert read_rtc()['error'] == 3 and not read_rtc()['ready']
print('RTC: 252 calendar cases, BCD/weekday, explicit set, retained reboot, year 2000, LSE/INIT failures and foreign clock protection passed')

# Smooth calibration never enters INIT and never changes the calendar/dividers.
write_register(0x40023870, 2)
write_register(0x4000280C, 64)
write_register(0x40002850, 0)
write_register(0x40002854, 0)
call(uc, symbols['rtc_init'])
call(uc, symbols['rtc_poll'])
rtc_state = [read_register(a) for a in [0x40002800, 0x40002804, 0x40002810, 0x40002850, 0x40002854]]
for steps in [-511, -1, 0, 1, 512]:
    uc.reg_write(UC_ARM_REG_R0, steps & 0xFFFFFFFF)
    call(uc, symbols['rtc_calibration_request'])
    write_register(0x4000280C, 64)
    call(uc, symbols['rtc_poll'])
    expected = (32768 | (512 - steps)) if steps > 0 else -steps
    assert read_register(0x4000283C) == expected
    assert call(uc, symbols['rtc_calibration_pending']) == 0
    assert call(uc, symbols['rtc_calibration_error']) == 0
    assert rtc_state == [read_register(a) for a in [0x40002800, 0x40002804, 0x40002810, 0x40002850, 0x40002854]]
uc.reg_write(UC_ARM_REG_R0, 513)
call(uc, symbols['rtc_calibration_request'])
assert call(uc, symbols['rtc_calibration_error']) == 1
uc.reg_write(UC_ARM_REG_R0, 0)
call(uc, symbols['rtc_calibration_request'])
write_register(0x4000280C, 64 | (1 << 16))
call(uc, symbols['rtc_poll'])
assert read_register(0x4000283C) == 32768
assert call(uc, symbols['rtc_calibration_pending']) == 1
write_register(symbols['tick_ms'], read_register(symbols['tick_ms']) + 35000)
call(uc, symbols['rtc_poll'])
assert call(uc, symbols['rtc_calibration_error']) == 2
assert call(uc, symbols['rtc_calibration_pending']) == 0
print('RTC calibration: signed limits, CALP/CALM, 32 s window, calendar preservation and nonblocking RECALPF timeout passed')

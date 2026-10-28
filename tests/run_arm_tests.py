"""Execute compiled ARM code with Unicorn; no STM32/W5500 hardware is emulated.

Install test-only dependencies with: python -m pip install unicorn==2.1.4 pyelftools==0.33
Run after scripts/build.ps1 and the mock build described in tests/README.md.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "build/test_deps"))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1


def load_image(path):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(0x08000000, 0x80000)
    uc.mem_map(0x20000000, 0x20000)
    uc.mem_map(0x10000000, 0x1000)
    with path.open("rb") as source:
        elf = ELFFile(source)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                uc.mem_write(segment["p_vaddr"], segment.data())
        symbols = {symbol.name: symbol["st_value"]
                   for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    uc.reg_write(UC_ARM_REG_SP, 0x2001F000)
    uc.reg_write(UC_ARM_REG_LR, 0x10000001)
    return uc, symbols


def call(uc, address):
    uc.reg_write(UC_ARM_REG_LR, 0x10000001)
    uc.emu_start(address | 1, 0x10000000, timeout=2000000, count=2000000)
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
]
count = 0
for request, expected in requests:
    uc.mem_write(0x20010000, request)
    uc.reg_write(UC_ARM_REG_R0, 0x20010000)
    uc.reg_write(UC_ARM_REG_R1, len(request))
    result = call(uc, symbols["http_parse"])
    assert result == expected, (request, result, expected)
    count += 1
for request, _ in requests[:3] + requests[10:12]:
    for length in range(len(request)):
        uc.mem_write(0x20010000, request)
        uc.reg_write(UC_ARM_REG_R0, 0x20010000)
        uc.reg_write(UC_ARM_REG_R1, length)
        assert call(uc, symbols["http_parse"]) == 0, (request, length)
        count += 1
print(f"HTTP parser: {count} cases passed (including every fragmented prefix)")
uc, symbols = load_image(root / "build/w5500_test.axf")
result = call(uc, symbols["test_entry"])
assert result == 0, f"W5500 mock failed at C source line {result}"
print("W5500 mock: static config, listen, RX/TX ring wrap, timeouts and recovery passed")
uc, symbols = load_image(root / "build/http_test.axf")
result = call(uc, symbols["test_entry"])
assert result == 0, f"HTTP integration mock failed at C source line {result}"
print("HTTP integration: HTML/JSON framing, 404/405/431, ADC, LED commands and connection recovery passed")

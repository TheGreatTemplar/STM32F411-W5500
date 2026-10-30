"""Execute the actual Help/NOR installer against a bounded NOR chip model."""
from pathlib import Path
import sys
import gzip
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build/test_deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0
root = Path(__file__).resolve().parents[1]
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(0x08000000, 0x80000)
uc.mem_map(0x20000000, 0x100000)
uc.mem_map(0x10000000, 0x1000)
with (root / 'build/help_test.axf').open('rb') as source:
    elf = ELFFile(source)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            uc.mem_write(segment['p_vaddr'],segment.data())
    symbols = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
uc.reg_write(UC_ARM_REG_SP,0x200FF000)
uc.reg_write(UC_ARM_REG_LR,0x10000001)
uc.emu_start(symbols['test_entry']|1,0x10000000,timeout=120000000,count=1500000000)
assert uc.reg_read(UC_ARM_REG_PC)==0x10000000,'Help tests exceeded execution budget'
result = uc.reg_read(UC_ARM_REG_R0)
assert result==0, f'Help NOR model failed at C line {result}'
size = int.from_bytes(uc.mem_read(symbols['help_captured_size'],4),'little')
archive = bytes(uc.mem_read(symbols['help_captured'],size))
assert gzip.decompress(archive)==(root/'documentation/help.html').read_bytes()
print('Help NOR: install/readback/gzip, no rewrite on reboot, 8 interrupted updates, partial commit, CRC rollback, foreign data, bounds, lock, missing/unsupported Flash and WIP timeout passed')

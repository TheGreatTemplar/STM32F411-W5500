"""Execute boot clock code with RCC/PWR readiness models; no physical crystal is emulated."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'build/test_deps'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0

root = Path(__file__).resolve().parents[1]

def fixture(fault=None):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for address, size in [(0x08000000,0x80000),(0x20000000,0x20000),(0x10000000,0x1000),
                          (0x40020000,0x10000),(0x40003000,0x3000),(0x40007000,0x1000),
                          (0x40011000,0x3000),(0xE000E000,0x2000),(0xE0001000,0x1000)]:
        uc.mem_map(address,size)
    with (root/'build/adc_web.axf').open('rb') as source:
        elf=ELFFile(source)
        for segment in elf.iter_segments():
            if segment['p_type']=='PT_LOAD':
                uc.mem_write(segment['p_vaddr'],segment.data())
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    def read(address):
        return int.from_bytes(uc.mem_read(address,4),'little')
    def write(address,value):
        uc.mem_write(address,(value&0xFFFFFFFF).to_bytes(4,'little'))
    def ready(machine,access,address,size,value,unused):
        if address==0x40023800:
            v=read(address)
            for enable,flag,blocked in [(1,2,fault=='hsi'),(1<<16,1<<17,fault=='hse'),
                                        (1<<24,1<<25,fault=='pll'),(1<<26,1<<27,False)]:
                v=(v|flag) if v&enable and not blocked else (v&~flag)
            write(address,v)
        elif address==0x40023808:
            v=read(address)
            sw=v&3
            write(address,(v&~12)|((0 if fault=='switch' and sw==2 else sw)<<2))
        elif address==0x40007004:
            write(address,(1<<14) if read(0x40023800)&(1<<24) and fault!='vos' else 0)
    def progress(machine,address,size,unused):
        if bytes(machine.mem_read(address, 2)) == b'\x30\xbf':  # WFI: inject wakeup in this CPU model.
            machine.reg_write(UC_ARM_REG_PC, (address + 2) | 1)
        write(0xE0001004,read(0xE0001004)+1000)
        write(symbols['tick_ms'],read(symbols['tick_ms'])+1)
    uc.hook_add(UC_HOOK_MEM_READ,ready,begin=0x40023800,end=0x4002380B)
    uc.hook_add(UC_HOOK_MEM_READ,ready,begin=0x40007004,end=0x40007007)
    uc.hook_add(UC_HOOK_CODE,progress)
    write(0x40023800,0x83)
    def call(name):
        uc.reg_write(UC_ARM_REG_SP,0x2001F000)
        uc.reg_write(UC_ARM_REG_LR,0x10000001)
        uc.emu_start(symbols[name]|1,0x10000000,count=2000000)
        assert uc.reg_read(UC_ARM_REG_PC)==0x10000000,name
        return uc.reg_read(UC_ARM_REG_R0)
    return uc,symbols,read,write,call

uc,symbols,read,write,call=fixture()
assert call('board_init')==1
assert read(symbols['clock_error'])==0
assert read(symbols['SystemCoreClock'])==96000000
assert read(0x40023804)==0x04403019
assert read(0x40023808)&0xFCFF==0xD00A
assert read(0x40023800)&(1<<19)  # CSS
assert read(0xE000E014)==95999
assert read(0x40023C00)&7==3
assert (read(0x40013000)>>3)&7==1  # SPI1: APB2 12 MHz /4 =3 MHz
assert read(0x40012304)&0x30000==0  # ADC: APB2 /2 =6 MHz
assert call('board_clock_init')==1  # Warm entry also returns the same plan.
for hpre, expected in [(0,96000000),(8,48000000),(9,24000000)]:
    write(0x40023808,0xD00A|(hpre<<4))
    call('SystemCoreClockUpdate')
    assert read(symbols['SystemCoreClock'])==expected

for fault,error in [('hsi',1),('hse',3),('pll',5),('vos',4),('switch',6)]:
    uc,symbols,read,write,call=fixture(fault)
    assert call('board_init')==0, fault
    assert read(symbols['clock_error'])==error, fault
    assert read(0x40020818)==1<<29  # Solid blue LED, no peripherals started.
    assert read(0x40013000)==0

uc,symbols,read,write,call=fixture()
assert read(symbols['__Vectors']+8)==symbols['NMI_Handler']|1
write(0x4002380C,1<<7)  # CSS failure
for address in [0x4001100C,0x40011014,0x40013000,0x40003800,0x40003804,0x40005400]:
    write(address,0xFFFFFFFF)
reset=[]
def reset_hook(machine,access,address,size,value,unused):
    if value&4:
        reset.append(value)
        machine.emu_stop()
uc.hook_add(UC_HOOK_MEM_WRITE,reset_hook,begin=0xE000ED0C,end=0xE000ED0F)
uc.reg_write(UC_ARM_REG_SP,0x2001F000)
uc.emu_start(symbols['NMI_Handler']|1,0x10000000,count=10000)
assert reset
assert all(read(a)==0 for a in [0x4001100C,0x40011014,0x40013000,0x40003800,0x40003804,0x40005400])
print('Fixed clocks: HSE25/PLL96, APB1 48/APB2 12, SysTick/SPI1/ADC, warm boot, five startup faults and CSS reset passed')



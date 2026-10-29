param
(
    [string]$Toolchain = 'C:\Keil_v5_ARM\ARM\ARMCLANG\Bin'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try
{
    & "$PSScriptRoot/build_info.ps1"
    New-Item -ItemType Directory -Force -Path build | Out-Null
    $compiler = Join-Path $Toolchain 'armclang.exe'
    $assembler = Join-Path $Toolchain 'armasm.exe'
    $linker = Join-Path $Toolchain 'armlink.exe'
    $converter = Join-Path $Toolchain 'fromelf.exe'
    $sources = @('board', 'w5500', 'spi_console', 'i2c_console', 'board_i2c', 'usart_console', 'command_store', 'settings', 'rtc', 'http_parse', 'http', 'main')
    $objects = @()
    foreach ($source in $sources)
    {
        $object = "build/$source.o"
        & $compiler --target=arm-arm-none-eabi -mcpu=cortex-m4 -mthumb -mfloat-abi=soft `
            -std=c99 -Oz -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections `
            -DSTM32F411xE -D__MICROLIB -Isrc -Ithird_party/CMSIS/Core/Include `
            -Ithird_party/STM32F4/Include -c "src/$source.c" -o $object
        if ($LASTEXITCODE -ne 0)
        {
            throw "Compilation failed: $source"
        }
        $objects += $object
    }
    & $assembler --cpu Cortex-M4 --apcs=interwork --pd '__MICROLIB SETA 1' `
        -g src/startup_stm32f411.s -o build/startup_stm32f411.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Assembly failed'
    }
    $objects += 'build/startup_stm32f411.o'
    & $linker --cpu Cortex-M4 --library_type=microlib --strict `
        --scatter firmware/stm32f411.sct --map --info sizes --list build/adc_web.map `
        --output build/adc_web.axf @objects
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Link failed'
    }
    & $converter --i32combined --output build/adc_web.hex build/adc_web.axf
    if ($LASTEXITCODE -ne 0)
    {
        throw 'HEX conversion failed'
    }
    & $converter --bin --output build/adc_web.bin build/adc_web.axf
    if ($LASTEXITCODE -ne 0)
    {
        throw 'BIN conversion failed'
    }
    Write-Host 'Built: build/adc_web.axf, build/adc_web.hex, build/adc_web.bin'
}
finally
{
    Pop-Location
}

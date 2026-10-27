param
(
    [string]$Toolchain = 'C:\Keil_v5_ARM\ARM\ARMCLANG\Bin',
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try
{
    & "$PSScriptRoot/build.ps1" -Toolchain $Toolchain
    & (Join-Path $Toolchain 'armclang.exe') --target=arm-arm-none-eabi -mcpu=cortex-m4 `
        -mthumb -mfloat-abi=soft -std=c99 -Oz -g -Wall -Wextra -Werror -DSTM32F411xE `
        -D__MICROLIB -Isrc -Ithird_party/CMSIS/Core/Include -Ithird_party/STM32F4/Include `
        -c tests/w5500_mock.c -o build/w5500_mock.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Mock compilation failed'
    }
    & (Join-Path $Toolchain 'armlink.exe') --cpu Cortex-M4 --library_type=microlib `
        --entry test_entry --ro-base 0x08000000 --rw-base 0x20000000 `
        --output build/w5500_test.axf build/w5500_mock.o build/w5500.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Mock link failed'
    }
    & (Join-Path $Toolchain 'armclang.exe') --target=arm-arm-none-eabi -mcpu=cortex-m4 `
        -mthumb -mfloat-abi=soft -std=c99 -Oz -g -Wall -Wextra -Werror `
        -D__MICROLIB -Isrc -c tests/http_mock.c -o build/http_mock.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'HTTP mock compilation failed'
    }
    & (Join-Path $Toolchain 'armlink.exe') --cpu Cortex-M4 --library_type=microlib `
        --entry test_entry --ro-base 0x08000000 --rw-base 0x20000000 `
        --output build/http_test.axf build/http_mock.o build/http.o build/http_parse.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'HTTP mock link failed'
    }
    & $Python tests/run_arm_tests.py
    if ($LASTEXITCODE -ne 0)
    {
        throw 'ARM tests failed'
    }
}
finally
{
    Pop-Location
}

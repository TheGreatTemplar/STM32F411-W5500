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
        --output build/w5500_test.axf build/w5500_mock.o build/w5500.o build/settings.o
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
        --output build/http_test.axf build/http_mock.o build/http.o build/http_parse.o build/spi_console.o build/i2c_console.o build/settings.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'HTTP mock link failed'
    }
    & (Join-Path $Toolchain 'armclang.exe') --target=arm-arm-none-eabi -mcpu=cortex-m4 `
        -mthumb -mfloat-abi=soft -std=c99 -Oz -g -Wall -Wextra -Werror `
        -D__MICROLIB -DSTM32F411xE -Isrc -Ithird_party/CMSIS/Core/Include -Ithird_party/STM32F4/Include `
        -c tests/store_mock.c -o build/store_mock.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Flash mock compilation failed'
    }
    & (Join-Path $Toolchain 'armlink.exe') --cpu Cortex-M4 --library_type=microlib `
        --entry test_entry --ro-base 0x08000000 --rw-base 0x20000000 `
        --output build/store_test.axf build/store_mock.o build/command_store.o build/spi_console.o build/i2c_console.o build/settings.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Flash mock link failed'
    }
    & (Join-Path $Toolchain 'armclang.exe') --target=arm-arm-none-eabi -mcpu=cortex-m4 `
        -mthumb -mfloat-abi=soft -std=c99 -Oz -g -Wall -Wextra -Werror -DSTM32F411xE `
        -D__MICROLIB -Isrc -Ithird_party/CMSIS/Core/Include -Ithird_party/STM32F4/Include `
        -c tests/help_mock.c -o build/help_mock.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Help mock compilation failed'
    }
    & (Join-Path $Toolchain 'armlink.exe') --cpu Cortex-M4 --library_type=microlib `
        --entry test_entry --ro-base 0x08000000 --rw-base 0x20000000 `
        --output build/help_test.axf build/help_mock.o build/help_store.o
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Help mock link failed'
    }
    & $Python tests/help_arm.py
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Help NOR tests failed'
    }
    & $Python tests/run_arm_tests.py
    if ($LASTEXITCODE -ne 0)
    {
        throw 'ARM tests failed'
    }
    & $Python tests/build_info.py
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Build identity verification failed'
    }
    & $Python tests/clock_arm.py
    if ($LASTEXITCODE -ne 0)
    {
        throw 'Clock tests failed'
    }
    & $Python tests/usart_arm.py
    if ($LASTEXITCODE -ne 0)
    {
        throw 'USART tests failed'
    }
}
finally
{
    Pop-Location
}

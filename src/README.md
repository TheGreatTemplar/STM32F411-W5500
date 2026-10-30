# Исходники

| Файл | Назначение |
| --- | --- |
| `startup_stm32f411.s` | Таблица векторов и вход в C runtime Arm |
| `board.c` | HSI/PLL, SysTick, GPIO, SPI1/SPI2, два канала ADC1 |
| `command_store.c` | W25Q64JV: 64 + 64 набора SPI/I2C, перенос старых I2C, CRC32, фоновая запись |
| `rtc.c` | RTC от LSE, календарь, ручная установка, резервные регистры |
| `w5500.c` | Регистры W5500, статическая сеть, TCP-сокеты 0–3, пошаговая отправка |
| `board_i2c.c` | Аппаратный I2C1 PB8/PB9, повторный START, ACK/POS, ограниченные ожидания |
| `i2c_console.c` | Строгий парсер I2C, адрес/операция/размеры, HEX/ASCII |
| `spi_console.c` | Проверка SPI и общий разбор HEX/ASCII-байтов |
| `http_parse.c` | Ограниченный разбор HTTP-запроса |
| `http.c` | Страница, JSON API, обслуживание соединения |
| `main.c` | Измерения, сеть и повторная инициализация |
| `config.h` | IP, порт, MAC, интервалы и опорное напряжение |
| `web/index.html` | Исходник страницы |
| `web_page.h` | Встроенный gzip страницы, генерируется скриптом; распаковка в браузере |

USART1 реализован в `usart_console.c`: PA9/PA10 AF7, RX DMA2 Stream2 circular,
TX DMA2 Stream7, оба channel4. Векторы USART1 и DMA2 Stream2 подключены в startup.
USART блокирует передачу SPI2, пока включён, чтобы сохранить APB2/BRR.

Используются C99, CMSIS и регистры MCU. Включены SysTick, USART1 и DMA2 Stream2;
остальные IRQ не включаются, их векторы указывают на Default_Handler. При добавлении IRQ
потребуется заменить соответствующие векторы в startup.

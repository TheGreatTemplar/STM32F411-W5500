# CMSIS-зависимости

В репозитории находятся только заголовки, необходимые для сборки этого проекта.
Их исходное форматирование и уведомления об авторских правах сохранены.

| Каталог | Источник | Версия | Лицензия |
| --- | --- | --- | --- |
| `CMSIS/` | ARM CMSIS, установленный официальный pack | 5.9.0, Core для ArmClang | Apache-2.0, `CMSIS/LICENSE.txt` |
| `STM32F4/` | STMicroelectronics/cmsis-device-f4 | Commit в `STM32F4/UPSTREAM.txt` | Apache-2.0, `STM32F4/LICENSE.md` |

Источники: [ARM CMSIS 5](https://github.com/ARM-software/CMSIS_5/tree/5.9.0),
[ST CMSIS Device F4](https://github.com/STMicroelectronics/cmsis-device-f4).
HAL и сторонние сетевые стеки не включены. Драйвер W5500 написан в `src/w5500.c`.

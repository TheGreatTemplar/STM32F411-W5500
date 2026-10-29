#ifndef USART_CONSOLE_H
#define USART_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usart_init(void);
void usart_poll(void);
bool usart_enabled(void);
uint32_t usart_control(const char *body, size_t size);
bool usart_cursor(const char *request, size_t size, uint32_t *cursor);
size_t usart_json(char *json, size_t capacity, uint32_t cursor);
void USART1_IRQHandler(void);
void DMA2_Stream2_IRQHandler(void);

#endif

#ifndef DEBUG_H
#define DEBUG_H

#include <libopencm3/stm32/usart.h>

/* Change this single line to USART1, USART2, or USART3 to switch which
 * peripheral debug output is sent on. Everything else (pin/clock setup)
 * adapts automatically in debug.c. */
#ifndef DEBUG_USART
#define DEBUG_USART USART1
#endif

void debug_setup(void);
/*
 * Lightweight printf-style UART logger.
 *
 * Parses the format string and retrieves variable arguments using
 * va_list/va_arg. Supported format specifiers:
 *   %d, %i  - signed integer
 *   %u      - unsigned integer
 *   %x, %X  - hexadecimal
 *   %o      - octal
 *   %p      - pointer
 *   %s      - string
 *   %c      - character
 *   %%      - literal '%'
 *
 * Supports optional zero-padding, field width, and 'l' modifier
 * (e.g. %08x, %5d, %lu). Formatted output is sent directly through UART.
 */
void debug_write(const char *text, ...);
#endif

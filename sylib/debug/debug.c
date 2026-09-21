#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#define DEBUG_USART USART1
#define NULL_STRING "(null)"
#include "debug.h"

void debug_setup(void) {
  /* RCC_USARTx and the GPIO port/pins can't be derived from DEBUG_USART by
   * name concatenation (RCC_ ## DEBUG_USART isn't a defined macro), so each
   * option is mapped explicitly here. */
#if DEBUG_USART == USART1
  rcc_periph_clock_enable(RCC_GPIOA);
  rcc_periph_clock_enable(RCC_USART1);
  /* PA9 = TX, PA10 = RX */
  gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                GPIO9);
  gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO10);
#elif DEBUG_USART == USART2
  rcc_periph_clock_enable(RCC_GPIOA);
  rcc_periph_clock_enable(RCC_USART2);
  /* PA2 = TX, PA3 = RX */
  gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                GPIO2);
  gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO3);
#elif DEBUG_USART == USART3
  rcc_periph_clock_enable(RCC_GPIOB);
  rcc_periph_clock_enable(RCC_USART3);
  /* PB10 = TX, PB11 = RX */
  gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                GPIO10);
  gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO11);
#else
#error "DEBUG_USART must be USART1, USART2, or USART3"
#endif

  /* USART configuration */
  usart_set_baudrate(DEBUG_USART, 115200);
  usart_set_databits(DEBUG_USART, 8);
  usart_set_stopbits(DEBUG_USART, USART_STOPBITS_1);
  usart_set_mode(DEBUG_USART, USART_MODE_TX_RX);
  usart_set_parity(DEBUG_USART, USART_PARITY_NONE);
  usart_set_flow_control(DEBUG_USART, USART_FLOWCONTROL_NONE);

  usart_enable(DEBUG_USART);
}

static void uart_send_char(char c) { usart_send_blocking(DEBUG_USART, c); }

static void uart_send_string(const char *str) {
  if (str == NULL) {
    uart_send_string(NULL_STRING);
    return;
  }

  while (*str) {
    uart_send_char(*str++);
  }
}

static void uart_send_int(long num, int width, int zero_pad) {
  char buffer[24]; // Buffer to hold the number as a string
  int i = 0;
  int neg = 0;
  unsigned long unum;

  if (num < 0) {
    neg = 1;
    unum = (unsigned long)(-num);
  } else {
    unum = (unsigned long)num;
  }

  if (unum == 0) {
    buffer[i++] = '0';
  } else {
    while (unum > 0) {
      buffer[i++] = (unum % 10) + '0';
      unum /= 10;
    }
  }
  if (neg) {
    buffer[i++] = '-';
  }

  for (int pad = i; pad < width; pad++) {
    uart_send_char(zero_pad ? '0' : ' ');
  }
  while (i > 0) {
    uart_send_char(buffer[--i]); // Print digits in reverse order
  }
}

static void uart_send_uint(unsigned long num, int base, int upper, int width,
                           int zero_pad) {
  static const char digits_lower[] = "0123456789abcdef";
  static const char digits_upper[] = "0123456789ABCDEF";
  const char *digits = upper ? digits_upper : digits_lower;
  char buffer[24]; // Buffer to hold the number as a string
  int i = 0;

  if (num == 0) {
    buffer[i++] = '0';
  } else {
    while (num > 0) {
      buffer[i++] = digits[num % base];
      num /= base;
    }
  }

  for (int pad = i; pad < width; pad++) {
    uart_send_char(zero_pad ? '0' : ' ');
  }
  while (i > 0) {
    uart_send_char(buffer[--i]); // Print digits in reverse order
  }
}

void debug_write(const char *format, ...) {

  va_list args;
  va_start(args, format);

  while (*format) {
    if (*format == '%') {
      format++; // Move to format specifier

      int zero_pad = 0;
      int width = 0;
      int is_long = 0;

      if (*format == '0') {
        zero_pad = 1;
        format++;
      }
      while (*format >= '0' && *format <= '9') {
        width = width * 10 + (*format - '0');
        format++;
      }
      if (*format == 'l') {
        is_long = 1;
        format++;
      }

      switch (*format) {
      case 'd': // Signed integer
      case 'i':
        uart_send_int(is_long ? va_arg(args, long) : va_arg(args, int), width,
                      zero_pad);
        break;
      case 'u': // Unsigned integer
        uart_send_uint(is_long ? va_arg(args, unsigned long)
                               : va_arg(args, unsigned int),
                       10, 0, width, zero_pad);
        break;
      case 'x': // Hex (lowercase)
        uart_send_uint(is_long ? va_arg(args, unsigned long)
                               : va_arg(args, unsigned int),
                       16, 0, width, zero_pad);
        break;
      case 'X': // Hex (uppercase)
        uart_send_uint(is_long ? va_arg(args, unsigned long)
                               : va_arg(args, unsigned int),
                       16, 1, width, zero_pad);
        break;
      case 'o': // Octal
        uart_send_uint(is_long ? va_arg(args, unsigned long)
                               : va_arg(args, unsigned int),
                       8, 0, width, zero_pad);
        break;
      case 'p': // Pointer
        uart_send_string("0x");
        uart_send_uint((unsigned long)(uintptr_t)va_arg(args, void *), 16, 0,
                       sizeof(void *) * 2, 1);
        break;
      case 's': // String
        uart_send_string(va_arg(args, char *));
        break;
      case 'c': // Character
        uart_send_char((char)va_arg(args, int));
        break;
      case '%': // Literal '%'
        uart_send_char('%');
        break;
      default:
        uart_send_char('?'); // Unknown format
        break;
      }
    } else {
      uart_send_char(*format);
    }
    format++;
  }

  va_end(args);
}

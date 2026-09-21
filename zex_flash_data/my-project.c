#include "api-asm.h"
#include "api.h"
#include "debug.h"
#include "esp8266.h"
#include "function.h"
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/rcc.h>

int x = 15;
int y = 10;

/* Reserved flash storage, linked into the "eeprom" region (0x0800F800).
 * "used" keeps the linker's --gc-sections from discarding it, since
 * nothing else in the program reads/writes it directly by name. */

// const uint32_t my_config __attribute__((section(".eeprom_data"), used)) =
//     0xDEADBEEF;

#define EEPROM_ADDR 0x0800F800

// This will rewrite entire page, 1KB in order to write only uint32_t
static void eeprom_write(uint32_t value) {
  flash_unlock();
  flash_erase_page(EEPROM_ADDR);
  flash_program_word(EEPROM_ADDR, value);
  flash_lock();
}

static uint32_t eeprom_read(void) { return *(volatile uint32_t *)EEPROM_ADDR; }

int main(void) {
  /* add your own code */
  rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
  debug_setup();
  esp8266_setup();
  esp8266_check();
  esp8266_connect("NabilaSubri_5G", "19971109@");

  uint32_t rev = 0xaabbccdd;
  rev = rev_bytes(rev);
  tambah(x, y);

  eeprom_write(0x12345678);
  uint32_t stored = eeprom_read();

  return my_func(rev) + (int)stored;

  while (1) {
  }
}

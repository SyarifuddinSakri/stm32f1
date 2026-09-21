#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/rcc.h>
#include <stdint.h>

#include "eeprom.h"

/* Reserved flash storage, linked into the "eeprom" region (0x0800F800).
 * "used" keeps the linker's --gc-sections from discarding it, since
 * nothing else in the program reads/writes it directly by name. */

// const uint32_t my_config __attribute__((section(".eeprom_data"), used)) =
//     0xDEADBEEF;

// This will rewrite entire page, 1KB in order to write anything in this
// memory
void eeprom_write(void *p_data, uint32_t data_len) {
  // To make sure that data is written in 32bit chunk
  uint32_t mult = data_len / 4;
  if (data_len % 4)
    mult++;

  uint32_t *p_words = (uint32_t *)p_data;

  flash_unlock();
  flash_erase_page(EEPROM_BASE_ADDR);
  for (uint32_t i = 0; i < mult; i++) {
    flash_program_word(EEPROM_BASE_ADDR + i * 4, p_words[i]);
  }
  flash_lock();
}

// Return : only pointer to the data stored in flash
// You'll need to cast the data returned to whatever size that you wanna read
uint32_t eeprom_read(uint32_t offset) {
  return *(volatile uint32_t *)(EEPROM_BASE_ADDR + offset);
}

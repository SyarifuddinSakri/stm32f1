#include <stdint.h>

#define EEPROM_BASE_ADDR 0x0800F800
void eeprom_write(void *p_data, uint32_t data_len);
uint32_t eeprom_read(uint32_t offset);

#define READ_FLASH(target_struct) *(volatile target_struct *)(EEPROM_BASE_ADDR)

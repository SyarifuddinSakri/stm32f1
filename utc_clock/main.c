#include "api-asm.h"
#include "api.h"
#include "debug.h"
#include "eeprom.h"
#include "esp8266.h"
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/rcc.h>

int main(void) {
  /* add your own code */
  rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
  debug_setup();
  esp8266_setup();
  esp8266_check();
  esp8266_connect("NabilaSubri_5G", "19971109@");

  while (1) {
  }
}

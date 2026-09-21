#include "api-asm.h"
#include "api.h"
#include "debug.h"
#include "eeprom.h"
#include "esp8266.h"
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/rcc.h>
#include <string.h>

static void on_http_request(uint8_t link_id, const char *request,
                            uint16_t len) {
  (void)len;
  if (strncmp(request, "GET / ", 6) == 0) {
    esp8266_http_send_response(link_id, 200, "text/html",
                               "<h1>Hello from STM32</h1>");
  } else {
    esp8266_http_send_response(link_id, 404, "text/plain", "Not Found");
  }
}

int main(void) {
  /* add your own code */
  rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
  debug_setup();
  esp8266_setup();
  esp8266_check();
  esp8266_connect("NabilaSubri_5G", "19971109@");
  esp8266_set_mode(ESP8266_MODE_CLIENT_HOST);            // CWMODE=3
  esp8266_connect("HomeWiFi", "homepassword");           // join home AP (STA)
  esp8266_set_ap_config("MyESP32", "esp8266pass", 6, 3); // WPA2, ch 6
  esp8266_set_http_request_handler(on_http_request);
  esp8266_server_start(80); // CIPSERVER=1,80

  while (1) {
  }
}

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "eeprom.h"
#include "esp8266.h"
#include "libopencm3/stm32/f1/usart.h"

#define WIFI_USART USART2

static void uart_send_char(char c);
static void uart_send_string(const char *str);
static bool extract_quoted(const char *line, char *out, size_t out_len);
static bool is_terminal_line(const char *line);
static void handle_scan_line(const char *line);
static void handle_connect_line(const char *line);
static void handle_simple_ack(const char *line);
static void connect_no_update(const char *ssid, const char *password);
#if ESP8266_MULTI_CONNECTION
static bool parse_ipd_header(const char *hdr, uint8_t *out_link_id,
                              uint16_t *out_len);
static void handle_ipd_frame(void);
#endif

flash_data_t flash_data = {0};
volatile char g_ssid[33];
volatile char g_password[64];
volatile char buffer_isr[512] = {0};
volatile char buffer_handle[512] = {0};
volatile bool terminated = false;

#if ESP8266_MULTI_CONNECTION
/* +IPD payload reassembly state (ISR context only). Unlike normal AT
 * responses, "+IPD,<link_id>,<len>:<data>" frames carry a byte count
 * instead of being newline-terminated, since <data> (e.g. HTTP request
 * headers/body) may itself contain embedded '\r'/'\n'. Once the
 * "+IPD,<id>,<len>:" header is recognized in the normal line buffer, the
 * ISR switches to counting exactly <len> raw bytes below instead of
 * scanning for '\r'/'\n'. */
static volatile bool receiving_ipd_payload = false;
static volatile uint16_t ipd_bytes_remaining = 0;
static volatile uint8_t ipd_link_id = 0;
static volatile char ipd_payload[ESP8266_HTTP_REQUEST_MAX_LEN];
static volatile uint16_t ipd_payload_len = 0;
static esp8266_http_request_handler_t http_request_handler = NULL;
#endif

/* Identifies which AT command is currently awaiting a response, so
 * esp8266_handle_return() knows how to interpret each incoming line.
 * Add a new value here (and a case in esp8266_handle_return()) whenever a
 * command needs its response parsed instead of just acknowledged. */
typedef enum {
  ESP8266_CMD_NONE = 0,
  ESP8266_CMD_CHECK,
  ESP8266_CMD_SET_MODE,
  ESP8266_CMD_CONNECT,
  ESP8266_CMD_DISCONNECT,
  ESP8266_CMD_RESET,
  ESP8266_CMD_GET_IP,
  ESP8266_CMD_GET_CONNECTION_STATUS,
  ESP8266_CMD_SCAN_WIFI,
  ESP8266_CMD_AP,
} esp8266_pending_cmd_t;

/* Set by each esp8266_* function right before it sends its AT command;
 * cleared by esp8266_handle_return() (directly, or via
 * handle_simple_ack()/esp8266_handle_scan_line()) once that
 * command's response has been fully consumed. */
static volatile esp8266_pending_cmd_t esp8266_pending_cmd = ESP8266_CMD_NONE;

void esp8266_setup(void) {
  /* RCC_USARTx and the GPIO port/pins can't be derived from WIFI_USART by
   * name concatenation (RCC_ ## WIFI_USART isn't a defined macro), so each
   * option is mapped explicitly here. */
#if WIFI_USART == USART1
  rcc_periph_clock_enable(RCC_GPIOA);
  rcc_periph_clock_enable(RCC_USART1);
  /* PA9 = TX, PA10 = RX */
  gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                GPIO9);
  gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO10);
  nvic_enable_irq(NVIC_USART1_IRQ);
#elif WIFI_USART == USART2
  rcc_periph_clock_enable(RCC_GPIOA);
  rcc_periph_clock_enable(RCC_USART2);
  /* PA2 = TX, PA3 = RX */
  gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                GPIO2);
  gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO3);
  nvic_enable_irq(NVIC_USART2_IRQ);
#elif WIFI_USART == USART3
  rcc_periph_clock_enable(RCC_GPIOB);
  rcc_periph_clock_enable(RCC_USART3);
  /* PB10 = TX, PB11 = RX */
  gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL,
                GPIO10);
  gpio_set_mode(GPIOB, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO11);
  nvic_enable_irq(NVIC_USART3_IRQ);
#else
#error "WIFI_USART must be USART1, USART2, or USART3"
#endif

  /* USART configuration */
  usart_set_baudrate(WIFI_USART, 115200);
  usart_set_databits(WIFI_USART, 8);
  usart_set_stopbits(WIFI_USART, USART_STOPBITS_1);
  usart_set_mode(WIFI_USART, USART_MODE_TX_RX);
  usart_set_parity(WIFI_USART, USART_PARITY_NONE);
  usart_set_flow_control(WIFI_USART, USART_FLOWCONTROL_NONE);

  usart_enable_rx_interrupt(WIFI_USART);

  usart_enable(WIFI_USART);

  /* Fix the connection mode (single vs multiplexed) at boot to match the
   * ESP8266_MULTI_CONNECTION build-time selector, so the AT+CIPMUX state
   * always matches the esp8266_tcp_* signatures compiled in below. */
#if ESP8266_MULTI_CONNECTION
  uart_send_string("AT+CIPMUX=1\r\n");
#else
  uart_send_string("AT+CIPMUX=0\r\n");
#endif
}

void esp8266_handle_return(void) {
  /* buffer_handle already holds the just-completed, NUL-terminated line
   * at this point (this is called right after termination is set). */
  const char *line = (const char *)buffer_handle;

  switch (esp8266_pending_cmd) {
  case ESP8266_CMD_SCAN_WIFI:
    handle_scan_line(line);
    break;

  case ESP8266_CMD_CHECK:
  case ESP8266_CMD_SET_MODE:
  case ESP8266_CMD_CONNECT:
    handle_connect_line(line);
    break;
  case ESP8266_CMD_DISCONNECT:
  case ESP8266_CMD_RESET:
  case ESP8266_CMD_GET_IP:
  case ESP8266_CMD_GET_CONNECTION_STATUS:
    /* No response-specific parsing needed yet for these; just clear the
     * pending state once the module finishes responding. Give a command
     * its own case (like ESP8266_CMD_SCAN_WIFI above) and a dedicated
     * esp8266_handle_*_line() helper if it needs custom parsing. */
    handle_simple_ack(line);
    break;

  case ESP8266_CMD_NONE:
  default:
    break;
  }
}

/* Response handler for ESP8266_CMD_SCAN_WIFI (AT+CWLAP): inspects each
 * "+CWLAP:(...)" line for an SSID that matches a saved credential and, on
 * the first match, issues connect_not_update() with it. */
static void handle_scan_line(const char *line) {
  if (strncmp(line, "+CWLAP:(", 8) == 0) {
    char ssid[33];
    if (extract_quoted(line, ssid, sizeof(ssid))) {
      for (uint32_t i = 0; i < MAX_CREDENTIAL; i++) {
        if (flash_data.list[i].ssid[0] != '\0' &&
            strcmp(flash_data.list[i].ssid, ssid) == 0) {
          /* NOTE: esp8266_connect() sends blocking over UART; running
           * this from ISR context stalls interrupts until the AT command
           * is fully transmitted. Acceptable here since it only fires
           * once per scan, but keep in mind if scans need to be
           * time-critical.
           */
          connect_no_update(flash_data.list[i].ssid,
                            flash_data.list[i].password);
          return;
        }
      }
    }
    return;
  }

  if (is_terminal_line(line)) {
    /* Scan finished with no matching saved credential found. */
    esp8266_pending_cmd = ESP8266_CMD_NONE;
  }
}

static void handle_connect_line(const char *line) {
  if (strcmp(line, "OK") == 0) {
    esp8266_update_credential_list((char *)g_ssid, (char *)g_password);
    esp8266_pending_cmd = ESP8266_CMD_NONE;
    return;
  }

  char *p = strstr(line, "+CWJAP:");
  if (p != NULL) {
    p = p + 7;
    uint8_t error_code = atoi(p);
    switch (error_code) {
    case 1:
      debug_write("connect timeout\n");
      break;
    case 2:
      debug_write("wrong password\n");
      break;
    case 3:
      debug_write("AP not found\n");
      break;
    case 4:
      debug_write("Connect Fail\n");
      break;
    default:
      debug_write("Unknown Error\n");
      break;
    }
  }
  esp8266_pending_cmd = ESP8266_CMD_NONE;
}

// The interrupt from UART will goes here
// This act like the return from the wifi module
// Handle the return here using interrupt
#if WIFI_USART == USART1
void usart1_isr(void)
#elif WIFI_USART == USART2
void usart2_isr(void)
#elif WIFI_USART == USART3
void usart3_isr(void)
#endif
{
  if (usart_get_flag(WIFI_USART, USART_SR_RXNE)) {
    uint8_t c = usart_recv(WIFI_USART);
    debug_write("%c", c);
    static uint16_t char_index = 0;

#if ESP8266_MULTI_CONNECTION
    /* Raw byte-count mode: consume exactly ipd_bytes_remaining bytes of
     * +IPD payload verbatim, ignoring '\r'/'\n', since HTTP data may
     * contain either. */
    if (receiving_ipd_payload) {
      if (ipd_payload_len < sizeof(ipd_payload) - 1) {
        ipd_payload[ipd_payload_len++] = (char)c;
      }
      ipd_bytes_remaining--;
      if (ipd_bytes_remaining == 0) {
        ipd_payload[ipd_payload_len] = '\0';
        receiving_ipd_payload = false;
        // This handle should be under different thread in RTOS
        handle_ipd_frame();
        ipd_payload_len = 0;
      }
      return;
    }
#endif

    if (c == '\r' || c == '\n') {
      if (!terminated) {
        buffer_isr[char_index] = '\0';
        terminated = true;
        // buffer_handle is only touched here (ISR context) and briefly by
        for (uint16_t i = 0; i <= char_index && i < sizeof(buffer_handle);
             i++) {
          buffer_handle[i] = buffer_isr[i];
        }
        buffer_handle[sizeof(buffer_handle) - 1] = '\0';
        // This handle should be under different thread in RTOS
        esp8266_handle_return();
        char_index = 0;
      }
    } else {
      /* Drop characters once the line buffer is full instead of
       * overflowing into adjacent memory; the line will be truncated but
       * memory stays safe. */
      if (char_index < sizeof(buffer_isr) - 1) {
        buffer_isr[char_index] = c;
        char_index++;
      }
      terminated = false;

#if ESP8266_MULTI_CONNECTION
      /* Detect the unsolicited "+IPD,<link_id>,<len>:" header, which (in
       * multi-connection/CIPMUX=1 builds) always precedes incoming socket
       * data. It ends with ':' rather than '\r'/'\n', so it's checked here
       * instead of at the line-terminator branch above. */
      if (c == ':' && char_index >= 5 &&
          strncmp((const char *)buffer_isr, "+IPD,", 5) == 0) {
        uint8_t link_id;
        uint16_t len;
        buffer_isr[char_index] = '\0';
        if (parse_ipd_header((const char *)buffer_isr, &link_id, &len) &&
            len > 0) {
          ipd_link_id = link_id;
          ipd_bytes_remaining = len;
          ipd_payload_len = 0;
          receiving_ipd_payload = true;
        }
        char_index = 0;
      }
#endif
    }
  }
}

static void uart_send_char(char c) { usart_send_blocking(WIFI_USART, c); }

static void uart_send_string(const char *str) {
  while (*str) {
    uart_send_char(*str++);
  }
}

/* Extracts the first "..." quoted field from line, e.g. the SSID out of
 * +CWLAP:(4,"MySSID",-42,"aa:bb:cc:dd:ee:ff",6). Copies at most
 * out_len - 1 bytes into out (always NUL-terminated).
 * Returns false if no quoted field is found. */
static bool extract_quoted(const char *line, char *out, size_t out_len) {
  const char *start = strchr(line, '"');
  if (!start) {
    return false;
  }
  start++;
  const char *end = strchr(start, '"');
  if (!end) {
    return false;
  }
  size_t len = (size_t)(end - start);
  if (len >= out_len) {
    len = out_len - 1;
  }
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

/* True for the lines that always terminate an AT command's response,
 * regardless of which command was issued. */
static bool is_terminal_line(const char *line) {
  return strcmp(line, "OK") == 0 || strncmp(line, "ERROR", 5) == 0 ||
         strncmp(line, "FAIL", 4) == 0;
}

/* Default response handler for commands with no response-specific
 * parsing: just clears the pending state once the module's terminal
 * "OK"/"ERROR"/"FAIL" line for that command arrives. */
static void handle_simple_ack(const char *line) {
  if (is_terminal_line(line)) {
    esp8266_pending_cmd = ESP8266_CMD_NONE;
  }
}

/* AT\r\n
 * Check if the uart comm to esp8266 is okay */
void esp8266_check(void) {
  esp8266_pending_cmd = ESP8266_CMD_CHECK;
  uart_send_string("AT\r\n");
}

/* AT+CWMODE=<mode>\r\n
 * 1 = station (client), 2 = AP (host), 3 = station+AP */
void esp8266_set_mode(esp8266_mode_t mode) {
  char at_cmd_str[16] = {0};
  switch (mode) {
  case ESP8266_MODE_RF_OFF:
  case ESP8266_MODE_CLIENT:
  case ESP8266_MODE_HOST:
  case ESP8266_MODE_CLIENT_HOST:
    snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CWMODE=%d\r\n", (int)mode);
    esp8266_pending_cmd = ESP8266_CMD_SET_MODE;
    uart_send_string(at_cmd_str);
    break;
  default:
    break;
  }
}

/* AT+CWJAP="<ssid>","<password>"\r\n
 * SSID max 32 bytes, password max 63 bytes per 802.11/WPA spec, plus
 * quotes, comma, prefix, and CRLF -> 128 bytes gives ample headroom. */
void esp8266_connect(const char *ssid, const char *password) {
  char at_cmd_str[128] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid,
           password);
  esp8266_pending_cmd = ESP8266_CMD_CONNECT;
  uart_send_string(at_cmd_str);
}

static void connect_no_update(const char *ssid, const char *password) {
  char at_cmd_str[128] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid,
           password);
  uart_send_string(at_cmd_str);
}

// Disconnect from current AP
void esp8266_disconnect(void) {
  esp8266_pending_cmd = ESP8266_CMD_DISCONNECT;
  uart_send_string("AT+CWQAP\r\n");
}

// Reset esp8266
void esp8266_reset(void) {
  esp8266_pending_cmd = ESP8266_CMD_RESET;
  uart_send_string("AT+RST\r\n");
}

/* AT+CIFSR\r\n
 * Query the local IP address(es) currently assigned to the module */
void esp8266_get_ip(void) {
  esp8266_pending_cmd = ESP8266_CMD_GET_IP;
  uart_send_string("AT+CIFSR\r\n");
}

/* AT+CWJAP?\r\n
 * Query the currently connected AP; response comes back over the ISR
 * to the debug console */
void esp8266_get_connection_status(void) {
  esp8266_pending_cmd = ESP8266_CMD_GET_CONNECTION_STATUS;
  uart_send_string("AT+CWJAP?\r\n");
}

void esp8266_update_credential_list(const char *ssid, const char *password) {
  flash_data = READ_FLASH(flash_data_t);
  // if during the update then the same ssid is found, update only the password
  for (int i = 0; i < MAX_CREDENTIAL; i++) {
    if (strcmp(flash_data.list[i].ssid, ssid) == 0) {
      snprintf(flash_data.list[i].password, 64, "%s", password);
      eeprom_write(&flash_data, sizeof(flash_data_t));
      return;
    }
  }
  // If the credential is new
  if (flash_data.next_update >= MAX_CREDENTIAL)
    flash_data.next_update = 0;

  snprintf(flash_data.list[flash_data.next_update].ssid, 33, "%s", ssid);
  snprintf(flash_data.list[flash_data.next_update].password, 64, "%s",
           password);

  flash_data.next_update++;

  eeprom_write(&flash_data, sizeof(flash_data_t));
}

/* AT+CWLAP\r\n
 * List available access points. Requires station mode
 * (ESP8266_MODE_CLIENT or ESP8266_MODE_CLIENT_HOST). Loads the saved
 * credential list from flash and sets esp8266_pending_cmd to
 * ESP8266_CMD_SCAN_WIFI; the ISR's esp8266_handle_return() then dispatches
 * to esp8266_handle_scan_line() for each "+CWLAP:(...)" line as it
 * arrives and issues esp8266_connect() automatically on the first SSID
 * match against a saved credential. */
void esp8266_scan_connect_wifi(void) {
  flash_data = READ_FLASH(flash_data_t);
  esp8266_pending_cmd = ESP8266_CMD_SCAN_WIFI;
  uart_send_string("AT+CWLAP\r\n");
}

#if ESP8266_MULTI_CONNECTION

/* AT+CIPSTART=<link_id>,"TCP","<ip>",<port>\r\n
 * Open a TCP connection on the given link_id (0-4) to the given host/port.
 * Requires multi-connection mode (AT+CIPMUX=1), enabled automatically in
 * esp8266_setup() for this build. */
void esp8266_tcp_connect(uint8_t link_id, const char *ip, uint16_t port) {
  if (link_id > ESP8266_MAX_LINK_ID) {
    return;
  }
  char at_cmd_str[64] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str),
           "AT+CIPSTART=%u,\"TCP\",\"%s\",%u\r\n", (unsigned int)link_id, ip,
           (unsigned int)port);
  uart_send_string(at_cmd_str);
}

/* AT+CIPSEND=<link_id>,<length>\r\n<data>
 * Send raw data over the TCP/UDP connection identified by link_id (0-4).
 * The module buffers input until <length> bytes have been received;
 * waiting for its ">" prompt is not implemented here since RX is handled
 * asynchronously via the ISR. */
void esp8266_tcp_send(uint8_t link_id, const uint8_t *data, uint16_t length) {
  if (link_id > ESP8266_MAX_LINK_ID) {
    return;
  }
  char at_cmd_str[32] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CIPSEND=%u,%u\r\n",
           (unsigned int)link_id, (unsigned int)length);
  uart_send_string(at_cmd_str);
  for (uint16_t i = 0; i < length; i++) {
    uart_send_char((char)data[i]);
  }
}

/* AT+CIPCLOSE=<link_id>\r\n
 * Close the TCP/UDP connection identified by link_id (0-4) */
void esp8266_tcp_close(uint8_t link_id) {
  if (link_id > ESP8266_MAX_LINK_ID) {
    return;
  }
  char at_cmd_str[24] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CIPCLOSE=%u\r\n",
           (unsigned int)link_id);
  uart_send_string(at_cmd_str);
}

/* AT+CIPSERVER=1,<port>\r\n
 * Start listening as a TCP server on the given port. Requires
 * multi-connection mode (AT+CIPMUX=1), already enabled automatically in
 * esp8266_setup() for this build. Incoming clients are reported
 * asynchronously over UART as "<link_id>,CONNECT"; use the existing
 * esp8266_tcp_send()/esp8266_tcp_close() with that link_id to talk to
 * them. */
void esp8266_server_start(uint16_t port) {
  char at_cmd_str[32] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CIPSERVER=1,%u\r\n",
           (unsigned int)port);
  uart_send_string(at_cmd_str);
}

/* AT+CIPSERVER=0\r\n
 * Stop listening / close the TCP server */
void esp8266_server_stop(void) { uart_send_string("AT+CIPSERVER=0\r\n"); }

/* AT+CWSAP="<ssid>","<password>",<channel>,<encryption>\r\n
 * channel : 1-4 (pick the channel that you want)
 * encryption : 0:OPEN 2:WPA_PSK 3:WPA2_PSK 4:WPA_WPA2_PSK
 * Configures the softAP SSID/password/channel/encryption so phones or
 * laptops can find and join the module's own network directly (paired
 * with esp8266_server_start() to host a web server on it). Only present
 * when ESP8266_MULTI_CONNECTION is enabled; requires the module already
 * be in a mode with the AP interface active (ESP8266_MODE_HOST or
 * ESP8266_MODE_CLIENT_HOST). */
void esp8266_set_ap_config(const char *ssid, const char *password,
                           uint8_t channel, uint8_t encryption) {
  char at_cmd_str[96] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CWSAP=\"%s\",\"%s\",%u,%u\r\n",
           ssid, password, (unsigned int)channel, (unsigned int)encryption);
  uart_send_string(at_cmd_str);
}

void esp8266_set_http_request_handler(
    esp8266_http_request_handler_t handler) {
  http_request_handler = handler;
}

/* Parses "+IPD,<link_id>,<len>:" out of hdr (which must already end with
 * the ':' and start with "+IPD,"). Returns false on malformed input. */
static bool parse_ipd_header(const char *hdr, uint8_t *out_link_id,
                              uint16_t *out_len) {
  const char *p = hdr + 5; /* skip "+IPD," */
  char *after_id = NULL;
  long id = strtol(p, &after_id, 10);
  if (after_id == p || *after_id != ',') {
    return false;
  }
  char *after_len = NULL;
  long len = strtol(after_id + 1, &after_len, 10);
  if (after_len == after_id + 1 || *after_len != ':') {
    return false;
  }
  if (id < 0 || id > ESP8266_MAX_LINK_ID || len < 0) {
    return false;
  }
  *out_link_id = (uint8_t)id;
  *out_len = (uint16_t)len;
  return true;
}

/* Called from ISR context once a full +IPD payload has been captured;
 * hands it to the registered application callback, if any. NOTE: like
 * connect_no_update() above, this runs the callback (and whatever
 * esp8266_tcp_send()/esp8266_tcp_close() it issues) synchronously inside
 * the UART ISR, which blocks on uart_send_char() until fully transmitted.
 * Keep HTTP responses short, or move this to a main-loop flag/queue if
 * responses need to be large or timing-sensitive. */
static void handle_ipd_frame(void) {
  if (http_request_handler != NULL) {
    http_request_handler(ipd_link_id, (const char *)ipd_payload,
                          ipd_payload_len);
  }
}

void esp8266_http_send_response(uint8_t link_id, uint16_t status_code,
                                 const char *content_type, const char *body) {
  if (content_type == NULL) {
    content_type = "text/html";
  }
  if (body == NULL) {
    body = "";
  }
  const char *status_text = "OK";
  switch (status_code) {
  case 200:
    status_text = "OK";
    break;
  case 204:
    status_text = "No Content";
    break;
  case 400:
    status_text = "Bad Request";
    break;
  case 404:
    status_text = "Not Found";
    break;
  case 500:
    status_text = "Internal Server Error";
    break;
  default:
    status_text = "";
    break;
  }

  char header[128] = {0};
  int header_len = snprintf(header, sizeof(header),
                             "HTTP/1.1 %u %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %u\r\n"
                             "Connection: close\r\n\r\n",
                             (unsigned int)status_code, status_text,
                             content_type, (unsigned int)strlen(body));
  if (header_len < 0) {
    return;
  }

  /* AT+CIPSEND expects a single byte count up front, so header and body
   * are sent as one logical payload split across two blocking writes. */
  size_t body_len = strlen(body);
  char at_cmd_str[32] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CIPSEND=%u,%u\r\n",
           (unsigned int)link_id, (unsigned int)(header_len + body_len));
  uart_send_string(at_cmd_str);
  uart_send_string(header);
  uart_send_string(body);

  esp8266_tcp_close(link_id);
}

#else /* !ESP8266_MULTI_CONNECTION: single-connection build, no link_id */

/* AT+CIPSTART="TCP","<ip>",<port>\r\n
 * Open the single TCP connection to the given host/port. Requires
 * single-connection mode (AT+CIPMUX=0), enabled automatically in
 * esp8266_setup() for this build. */
void esp8266_tcp_connect(const char *ip, uint16_t port) {
  char at_cmd_str[64] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
           ip, (unsigned int)port);
  uart_send_string(at_cmd_str);
}

/* AT+CIPSEND=<length>\r\n<data>
 * Send raw data over the currently open TCP/UDP connection. The module
 * buffers input until <length> bytes have been received; waiting for its
 * ">" prompt is not implemented here since RX is handled asynchronously
 * via the ISR. */
void esp8266_tcp_send(const uint8_t *data, uint16_t length) {
  char at_cmd_str[24] = {0};
  snprintf(at_cmd_str, sizeof(at_cmd_str), "AT+CIPSEND=%u\r\n",
           (unsigned int)length);
  uart_send_string(at_cmd_str);
  for (uint16_t i = 0; i < length; i++) {
    uart_send_char((char)data[i]);
  }
}

/* AT+CIPCLOSE\r\n
 * Close the currently open TCP/UDP connection */
void esp8266_tcp_close(void) { uart_send_string("AT+CIPCLOSE\r\n"); }

#endif /* ESP8266_MULTI_CONNECTION */


#ifndef ESP_8266_H
#define ESP_8266_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  ESP8266_MODE_RF_OFF = 0,
  ESP8266_MODE_CLIENT,
  ESP8266_MODE_HOST,
  ESP8266_MODE_CLIENT_HOST
} esp8266_mode_t;

typedef struct {
  char password[64];
  char ssid[33];
} wifi_credential_t;

#define MAX_CREDENTIAL 10
typedef struct {
  uint32_t next_update;
  wifi_credential_t list[MAX_CREDENTIAL];
} flash_data_t;

void esp8266_check(void);
void esp8266_setup(void);
void esp8266_set_mode(esp8266_mode_t mode);
void esp8266_connect(const char *ssid, const char *password);
void esp8266_disconnect(void);
void esp8266_reset(void);
void esp8266_get_ip(void);
void esp8266_get_connection_status(void);
void esp8266_update_credential_list(const char *ssid, const char *password);
void esp8266_scan_connect_wifi(void);
void esp8266_handle_return(void);

/* Set to 1 to build with multi-connection support (AT+CIPMUX=1, up to 5
 * sockets, link IDs 0-4); set to 0 to build with single-connection support
 * (AT+CIPMUX=0). This selects which esp8266_tcp_* signatures are exposed
 * below and is applied automatically by esp8266_setup(). */
#ifndef ESP8266_MULTI_CONNECTION
#define ESP8266_MULTI_CONNECTION 1
#endif

#if ESP8266_MULTI_CONNECTION
/* link_id selects which connection slot (0-4) a command applies to.
 * ESP8266 AT firmware supports at most 5 simultaneous connections
 * (link IDs 0..4) when multiplexing is enabled via AT+CIPMUX=1. */
#define ESP8266_MAX_LINK_ID 4

void esp8266_tcp_connect(uint8_t link_id, const char *ip, uint16_t port);
void esp8266_tcp_send(uint8_t link_id, const uint8_t *data, uint16_t length);
void esp8266_tcp_close(uint8_t link_id);

/* AT+CIPSERVER requires multi-connection mode (AT+CIPMUX=1), so server
 * mode is only available when ESP8266_MULTI_CONNECTION is enabled. */
void esp8266_server_start(uint16_t port);
void esp8266_server_stop(void);

/* AT+CWSAP=<ssid>,<password>,<channel>,<encryption>\r\n
 * Configures the module's own softAP (used when hosting a server that
 * phones/laptops connect to directly). Only declared under
 * ESP8266_MULTI_CONNECTION since it is only meaningful paired with
 * esp8266_server_start(); calling it without that macro enabled produces
 * a compile-time error instead of a silently unused config.
 * encryption: 0 = OPEN, 2 = WPA_PSK, 3 = WPA2_PSK, 4 = WPA_WPA2_PSK.
 * password must be at least 8 chars unless encryption is 0 (OPEN). */
void esp8266_set_ap_config(const char *ssid, const char *password,
                           uint8_t channel, uint8_t encryption);

/* Called from ISR context once a full "+IPD,<link_id>,<len>:<data>" frame
 * has been received on link_id (see esp8266_set_http_request_handler()).
 * request is NUL-terminated but may be truncated to
 * ESP8266_HTTP_REQUEST_MAX_LEN bytes if the client sent more; it is only
 * valid for the duration of the callback. */
#define ESP8266_HTTP_REQUEST_MAX_LEN 256
typedef void (*esp8266_http_request_handler_t)(uint8_t link_id,
                                                const char *request,
                                                uint16_t length);

/* Registers the callback invoked for every "+IPD" frame the server
 * receives (i.e. every HTTP request from a connected client). Call this
 * once during setup, after esp8266_server_start(). Pass NULL to disable
 * (frames are then silently dropped). */
void esp8266_set_http_request_handler(esp8266_http_request_handler_t handler);

/* Builds a minimal "HTTP/1.1 <status_code> ...\r\nContent-Type:
 * <content_type>\r\nContent-Length: <len>\r\nConnection:
 * close\r\n\r\n<body>" response and sends it over link_id via
 * esp8266_tcp_send(), then closes the connection with
 * esp8266_tcp_close(link_id). Meant to be called from within the
 * esp8266_http_request_handler_t callback. content_type may be NULL
 * (defaults to "text/html"); body may be NULL/empty for e.g. a 204. */
void esp8266_http_send_response(uint8_t link_id, uint16_t status_code,
                                 const char *content_type, const char *body);
#else
void esp8266_tcp_connect(const char *ip, uint16_t port);
void esp8266_tcp_send(const uint8_t *data, uint16_t length);
void esp8266_tcp_close(void);
#endif

#endif

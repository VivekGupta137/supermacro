/* Stubs when MACRO_WEB_UI is on but CONFIG_MACRO_WEB_USE_WS is off. */

#include "config.h"
#include "macro_ws.h"

#include "esp_http_server.h"

void macro_ws_init(httpd_handle_t hd) { (void)hd; }

void macro_ws_httpd_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    (void)sockfd;
}

int macro_ws_active_fd(void) { return -1; }

esp_err_t macro_ws_handler(httpd_req_t *req)
{
    (void)req;
    return ESP_ERR_NOT_SUPPORTED;
}

void macro_ws_request_broadcast(void) {}

#pragma once

#include "sdkconfig.h"

#if CONFIG_MACRO_WEB_UI

#include "esp_err.h"
#include "esp_http_server.h"

void macro_ws_init(httpd_handle_t hd);
void macro_ws_httpd_close_cb(httpd_handle_t hd, int sockfd);
esp_err_t macro_ws_handler(httpd_req_t *req);
void macro_ws_request_broadcast(void);

#else

#include "esp_err.h"
#include "esp_http_server.h"

static inline void macro_ws_init(httpd_handle_t hd) { (void)hd; }
static inline void macro_ws_httpd_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    (void)sockfd;
}
static inline esp_err_t macro_ws_handler(httpd_req_t *req)
{
    (void)req;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void macro_ws_request_broadcast(void) {}

#endif

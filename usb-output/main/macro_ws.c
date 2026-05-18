/*
 * WebSocket status push — only added to the build when CONFIG_HTTPD_WS_SUPPORT is enabled
 * at CMake/configure time (see main/CMakeLists.txt), so esp_http_server.h exposes WS types.
 */

#include "sdkconfig.h"
#include "config.h"
#include "macro_ws.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "macro_profile.h"

static const char *TAG = "macro_ws";

#define MAX_WS_CLIENTS 1

static httpd_handle_t s_httpd;
static int s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mu;
static volatile bool s_broadcast_pending;
/** If true, another broadcast was requested while a send was queued; run one more after it finishes. */
static volatile bool s_broadcast_stale;

struct macro_ws_broadcast_work {
    httpd_handle_t hd;
    char json[2048];
};

static void macro_ws_unregister_fd_locked(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = -1;
            break;
        }
    }
}

void macro_ws_httpd_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (s_ws_mu == NULL) {
        return;
    }
    xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    macro_ws_unregister_fd_locked(sockfd);
    xSemaphoreGive(s_ws_mu);
}

static void macro_ws_register_fd(int fd)
{
    xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == fd) {
            xSemaphoreGive(s_ws_mu);
            return;
        }
    }
    int slot = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] < 0) {
            slot = i;
            break;
        }
    }
    int evict_fd = -1;
    if (slot < 0) {
        evict_fd = s_ws_fds[0];
        for (int i = 1; i < MAX_WS_CLIENTS; i++) {
            s_ws_fds[i - 1] = s_ws_fds[i];
        }
        slot = MAX_WS_CLIENTS - 1;
    }
    s_ws_fds[slot] = fd;
    xSemaphoreGive(s_ws_mu);
    if (evict_fd >= 0 && s_httpd != NULL) {
        ESP_LOGW(TAG, "closing older WebSocket fd=%d for new fd=%d", evict_fd, fd);
        httpd_sess_trigger_close(s_httpd, evict_fd);
    }
}

static void broadcast_work_fn(void *arg)
{
    struct macro_ws_broadcast_work *w = (struct macro_ws_broadcast_work *)arg;
    if (w == NULL || w->hd == NULL) {
        if (w) {
            free(w);
        }
        s_broadcast_pending = false;
        if (s_broadcast_stale) {
            s_broadcast_stale = false;
            macro_ws_request_broadcast();
        }
        return;
    }

    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t *)w->json;
    pkt.len = strlen(w->json);
    pkt.final = true;

    if (s_ws_mu) {
        xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        int fd = s_ws_fds[i];
        if (fd < 0) {
            continue;
        }
        if (httpd_ws_get_fd_info(w->hd, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            s_ws_fds[i] = -1;
            continue;
        }
        esp_err_t err = httpd_ws_send_frame_async(w->hd, fd, &pkt);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "ws_send_frame_async fd=%d err=%s", fd, esp_err_to_name(err));
        }
    }
    if (s_ws_mu) {
        xSemaphoreGive(s_ws_mu);
    }
    free(w);
    s_broadcast_pending = false;
    if (s_broadcast_stale) {
        s_broadcast_stale = false;
        macro_ws_request_broadcast();
    }
}

void macro_ws_request_broadcast(void)
{
    if (s_httpd == NULL || s_ws_mu == NULL) {
        return;
    }
    if (s_broadcast_pending) {
        s_broadcast_stale = true;
        return;
    }
    struct macro_ws_broadcast_work *w = calloc(1, sizeof(*w));
    if (!w) {
        return;
    }
    w->hd = s_httpd;
    macro_profile_build_status_json(w->json, sizeof(w->json));
    s_broadcast_pending = true;
    if (httpd_queue_work(s_httpd, broadcast_work_fn, w) != ESP_OK) {
        s_broadcast_pending = false;
        free(w);
    }
}

void macro_ws_init(httpd_handle_t hd)
{
    s_httpd = hd;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_ws_fds[i] = -1;
    }
    if (s_ws_mu == NULL) {
        s_ws_mu = xSemaphoreCreateMutex();
    }
}

esp_err_t macro_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        macro_ws_register_fd(fd);
        ESP_LOGI(TAG, "WebSocket client fd=%d", fd);
        macro_ws_request_broadcast();
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        if (s_ws_mu) {
            xSemaphoreTake(s_ws_mu, portMAX_DELAY);
            macro_ws_unregister_fd_locked(fd);
            xSemaphoreGive(s_ws_mu);
        }
        return ESP_OK;
    }
    if (ws_pkt.len) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        free(buf);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

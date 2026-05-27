/*
 * WebSocket status push — built when CONFIG_MACRO_WEB_USE_WS is enabled.
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
static volatile bool s_broadcast_stale;

struct macro_ws_broadcast_work {
    httpd_handle_t hd;
    char json[MACRO_PROFILE_STATUS_JSON_MAX];
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

int macro_ws_active_fd(void)
{
    int fd = -1;
    if (s_ws_mu == NULL) {
        return -1;
    }
    xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] >= 0) {
            fd = s_ws_fds[i];
            break;
        }
    }
    xSemaphoreGive(s_ws_mu);
    return fd;
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
    int evict[4];
    int n_evict = 0;

    xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] >= 0 && s_ws_fds[i] != fd && n_evict < (int)(sizeof(evict) / sizeof(evict[0]))) {
            evict[n_evict++] = s_ws_fds[i];
        }
        s_ws_fds[i] = -1;
    }
    s_ws_fds[0] = fd;
    xSemaphoreGive(s_ws_mu);

    if (s_httpd != NULL) {
        for (int i = 0; i < n_evict; i++) {
            ESP_LOGW(TAG, "closing older WebSocket fd=%d for new fd=%d", evict[i], fd);
            httpd_sess_trigger_close(s_httpd, evict[i]);
        }
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

    bool send_ok = false;
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
        if (httpd_ws_send_frame_async(w->hd, fd, &pkt) == ESP_OK) {
            send_ok = true;
        }
    }
    if (s_ws_mu) {
        xSemaphoreGive(s_ws_mu);
    }
    const httpd_handle_t hd = w->hd;
    free(w);
    s_broadcast_pending = false;
    if (!send_ok && hd != NULL && s_httpd != NULL && s_ws_mu) {
        xSemaphoreTake(s_ws_mu, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            int fd = s_ws_fds[i];
            if (fd < 0) {
                continue;
            }
            if (httpd_ws_get_fd_info(hd, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_sess_trigger_close(s_httpd, fd);
            }
            s_ws_fds[i] = -1;
        }
        xSemaphoreGive(s_ws_mu);
    }
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

static void macro_ws_reject_fd(int fd, const char *why)
{
    ESP_LOGW(TAG, "reject WS fd=%d (%s)", fd, why);
    if (s_httpd != NULL) {
        httpd_sess_trigger_close(s_httpd, fd);
    }
}

esp_err_t macro_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        const int fd = httpd_req_to_sockfd(req);
        const int active = macro_ws_active_fd();

        if (active >= 0 && active != fd && s_httpd != NULL &&
            httpd_ws_get_fd_info(s_httpd, active) == HTTPD_WS_CLIENT_WEBSOCKET) {
            macro_ws_reject_fd(fd, "already connected");
            return ESP_OK;
        }

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
    }
    return ret;
}

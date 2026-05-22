#include "config.h"
#include "sdkconfig.h"
#include "macro_web.h"

#if CONFIG_MACRO_WEB_UI

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "cJSON.h"
#include "macro_profile.h"
#include "macpass_macro.h"
#include "macro_ws.h"
#include "doc_assets.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "macro_web";

static SemaphoreHandle_t s_net_ready;
static bool s_http_started;
static httpd_handle_t s_httpd;
static esp_timer_handle_t s_http_health_timer;
static int s_http_max_clients;
static volatile bool s_http_restart_requested;
static volatile bool s_http_purge_only;
static volatile bool s_http_need_wifi_reset;
static volatile bool s_http_want_running;
static volatile bool s_restart_in_progress;
static vprintf_like_t s_prev_log_vprintf;
static int64_t s_restart_cooldown_until_us;
static int64_t s_last_purge_us;
static int64_t s_accept113_window_start_us;
static int s_accept113_in_window;
static TaskHandle_t s_http_recovery_task;

#define HTTP_RESTART_FAIL_COOLDOWN_MS 20000
#define HTTP_WIFI_RESET_COOLDOWN_MS 15000
#define HTTP_ACCEPT113_PURGE_LIMIT 4
#define HTTP_ACCEPT113_WINDOW_US 10000000
#define HTTP_MIN_PURGE_INTERVAL_US 2500000

static void http_server_start(void);
static void http_server_shutdown(void);
static void http_stop_listener(void);
static void http_purge_sessions_only(void);
static void http_schedule_recovery(void);
#if CONFIG_MACRO_WIFI_ROLE_STA
static void http_wifi_lwip_reset(void);
#endif
static void http_set_restart_cooldown_ms(int ms);
static bool http_recovery_allowed(void);
static void http_note_accept113(void);

/** Free sockets quickly on ESP32 (default keep-alive holds slots until max_open_sockets). */
static void http_set_conn_close(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t http_resp_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    http_set_conn_close(req);
    return httpd_resp_send_err(req, code, msg);
}

static esp_err_t http_resp_json_err(httpd_req_t *req, httpd_err_code_t code, const char *err_msg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return http_resp_err(req, code, err_msg);
    }
    cJSON_AddBoolToObject(root, "ok", 0);
    cJSON_AddStringToObject(root, "error", err_msg != NULL ? err_msg : "error");
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return http_resp_err(req, code, err_msg);
    }
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    esp_err_t ret = httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return ret;
}

static esp_err_t http_send_body_chunked(httpd_req_t *req, const char *data, size_t len)
{
    const size_t chunk_sz = 1024;
    for (size_t off = 0; off < len; off += chunk_sz) {
        size_t n = len - off;
        if (n > chunk_sz) {
            n = chunk_sz;
        }
        esp_err_t r = httpd_resp_send_chunk(req, data + off, n);
        if (r != ESP_OK) {
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return r;
        }
        taskYIELD();
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static int http_log_vprintf(const char *fmt, va_list ap)
{
    if (fmt != NULL) {
        char msg[160];
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(msg, sizeof(msg), fmt, copy);
        va_end(copy);
        if (n > 0) {
            if (strstr(msg, "accept") != NULL && strstr(msg, "113") != NULL) {
                http_note_accept113();
            }
            if (strstr(msg, "socket") != NULL && strstr(msg, "105") != NULL) {
                s_http_need_wifi_reset = true;
                http_set_restart_cooldown_ms(HTTP_RESTART_FAIL_COOLDOWN_MS);
                http_schedule_recovery();
            }
        }
    }
    if (s_prev_log_vprintf != NULL) {
        return s_prev_log_vprintf(fmt, ap);
    }
    return vprintf(fmt, ap);
}

static void http_set_restart_cooldown_ms(int ms)
{
    s_restart_cooldown_until_us = esp_timer_get_time() + (int64_t)ms * 1000LL;
}

static bool http_recovery_allowed(void)
{
    return !s_restart_in_progress && esp_timer_get_time() >= s_restart_cooldown_until_us;
}

static void http_note_accept113(void)
{
    const int64_t now = esp_timer_get_time();

    if (s_accept113_window_start_us == 0 || (now - s_accept113_window_start_us) > HTTP_ACCEPT113_WINDOW_US) {
        s_accept113_window_start_us = now;
        s_accept113_in_window = 0;
    }
    s_accept113_in_window++;

    if (s_accept113_in_window <= HTTP_ACCEPT113_PURGE_LIMIT && s_httpd != NULL &&
        (now - s_last_purge_us) >= HTTP_MIN_PURGE_INTERVAL_US) {
        s_http_purge_only = true;
        http_schedule_recovery();
        return;
    }

    ESP_LOGW(TAG, "accept(113) x%d — resetting Wi-Fi TCP stack", s_accept113_in_window);
    s_accept113_in_window = 0;
    s_accept113_window_start_us = 0;
    s_http_purge_only = false;
    s_http_need_wifi_reset = true;
    if (http_recovery_allowed()) {
        http_schedule_recovery();
    } else {
        http_set_restart_cooldown_ms(3000);
    }
}

static void http_close_all_clients(void)
{
    if (!s_httpd) {
        return;
    }
    int client_fds[16];
    size_t count = sizeof(client_fds) / sizeof(client_fds[0]);
    if (httpd_get_client_list(s_httpd, &count, client_fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        httpd_sess_trigger_close(s_httpd, client_fds[i]);
    }
}

static void http_stop_listener(void)
{
    if (s_httpd != NULL) {
        http_close_all_clients();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t err = httpd_stop(s_httpd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "httpd_stop: %s", esp_err_to_name(err));
        }
        s_httpd = NULL;
    }
    s_http_started = false;
}

static void http_purge_sessions_only(void)
{
    if (s_httpd == NULL) {
        return;
    }
    ESP_LOGW(TAG, "Purging HTTP sessions (keep listener)");
    http_close_all_clients();
    vTaskDelay(pdMS_TO_TICKS(300));
    s_last_purge_us = esp_timer_get_time();
    s_http_purge_only = false;
    s_http_restart_requested = false;
}

#if CONFIG_MACRO_WIFI_ROLE_STA
static void http_wifi_lwip_reset(void)
{
    ESP_LOGW(TAG, "Wi-Fi reconnect to free lwIP TCP sockets");
    s_restart_in_progress = true;
    s_http_need_wifi_reset = false;
    s_http_restart_requested = false;
    s_http_purge_only = false;

    http_stop_listener();
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_wifi_connect();

    http_set_restart_cooldown_ms(HTTP_WIFI_RESET_COOLDOWN_MS);
    s_accept113_in_window = 0;
    s_accept113_window_start_us = 0;
    s_restart_in_progress = false;
}
#endif

static void http_recovery_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_http_want_running) {
            continue;
        }

#if CONFIG_MACRO_WIFI_ROLE_STA
        if (s_http_need_wifi_reset && http_recovery_allowed()) {
            http_wifi_lwip_reset();
            continue;
        }
#endif
        if (s_http_need_wifi_reset && http_recovery_allowed()) {
            /* SoftAP / non-STA: restart HTTP only (no 3 s Wi-Fi disconnect). */
            ESP_LOGW(TAG, "Restarting HTTP listener (socket recovery)");
            s_http_need_wifi_reset = false;
            http_stop_listener();
            vTaskDelay(pdMS_TO_TICKS(200));
            http_server_start();
            continue;
        }

        if (s_http_purge_only && s_httpd != NULL) {
            http_purge_sessions_only();
            continue;
        }

        if (s_http_restart_requested && s_httpd != NULL) {
            http_purge_sessions_only();
            continue;
        }

        if (!http_recovery_allowed()) {
            continue;
        }

        if (s_httpd != NULL && s_http_started) {
            continue;
        }

        ESP_LOGW(TAG, "HTTP listener down, bringing server back");
        s_restart_in_progress = true;
        http_server_start();
        if (s_http_started && s_httpd != NULL) {
            ESP_LOGI(TAG, "HTTP server online");
            s_restart_in_progress = false;
            s_http_need_wifi_reset = false;
            s_accept113_in_window = 0;
            s_accept113_window_start_us = 0;
            continue;
        }

        ESP_LOGE(TAG, "HTTP recovery failed; retry in %d s", HTTP_RESTART_FAIL_COOLDOWN_MS / 1000);
        http_set_restart_cooldown_ms(HTTP_RESTART_FAIL_COOLDOWN_MS);
        s_restart_in_progress = false;
#if CONFIG_MACRO_WIFI_ROLE_STA
        s_http_need_wifi_reset = true;
#endif
    }
}

static void http_schedule_recovery(void)
{
    if (s_http_recovery_task != NULL) {
        xTaskNotifyGive(s_http_recovery_task);
    }
}

static void http_health_timer_cb(void *arg)
{
    (void)arg;
    if (!s_http_want_running) {
        return;
    }

    /* Do not mass-close clients at max_open_sockets — lru_purge handles stale slots;
     * closing all sessions mid-request caused wedged API until reboot. */

    if (!http_recovery_allowed()) {
        return;
    }

    if (s_http_purge_only) {
        http_schedule_recovery();
        return;
    }

    if (s_http_need_wifi_reset || s_http_restart_requested || (s_http_want_running && !s_http_started)) {
        http_schedule_recovery();
    }
}

static void http_server_shutdown(void)
{
    s_http_want_running = false;
    if (s_http_health_timer != NULL) {
        esp_timer_stop(s_http_health_timer);
    }
    if (s_prev_log_vprintf != NULL) {
        esp_log_set_vprintf(s_prev_log_vprintf);
        s_prev_log_vprintf = NULL;
    }
    http_stop_listener();
}

#if CONFIG_MACRO_WIFI_ROLE_STA

static uint32_t s_reconnect_delay_ms = 2000;
static esp_timer_handle_t s_wifi_reconnect_timer;
static esp_timer_handle_t s_wifi_connect_timer;

static const char *wifi_disc_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "unspecified";
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth expired (wrong password or WPA mismatch)";
    case WIFI_REASON_AUTH_LEAVE:
        return "auth leave";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "assoc expired";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "AP full";
    case WIFI_REASON_NOT_AUTHED:
        return "not authenticated";
    case WIFI_REASON_NOT_ASSOCED:
        return "not associated";
    case WIFI_REASON_ASSOC_LEAVE:
        return "assoc leave";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "assoc not authed";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "bad power cap";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "bad channel";
    case WIFI_REASON_IE_INVALID:
        return "invalid IE";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4-way handshake timeout (wrong password?)";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "group key timeout";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE differs in 4-way";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "invalid group cipher";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "invalid pairwise cipher";
    case WIFI_REASON_AKMP_INVALID:
        return "invalid AKMP";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "unsupported RSN IE";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "invalid RSN IE cap";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802.1X auth failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "cipher rejected";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout (out of range?)";
    case WIFI_REASON_NO_AP_FOUND:
        return "SSID not found (2.4 GHz? typo?)";
    case WIFI_REASON_AUTH_FAIL:
        return "auth failed (wrong password / WPA mode?)";
    case WIFI_REASON_ASSOC_FAIL:
        return "assoc failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
#if defined(WIFI_REASON_CONNECTION_FAIL)
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection fail (check password / WPA2 vs WPA3)";
#else
    case 205:
        return "connection fail (check password / WPA2 vs WPA3)";
#endif
    default:
        return "see esp_wifi_types.h";
    }
}

static void wifi_connect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void schedule_wifi_connect(uint32_t delay_ms)
{
    if (s_wifi_connect_timer == NULL) {
        esp_wifi_connect();
        return;
    }
    esp_timer_stop(s_wifi_connect_timer);
    esp_timer_start_once(s_wifi_connect_timer, (uint64_t)delay_ms * 1000ULL);
}

static void schedule_wifi_reconnect(void)
{
    if (s_wifi_reconnect_timer == NULL) {
        return;
    }
    esp_timer_stop(s_wifi_reconnect_timer);
    esp_timer_start_once(s_wifi_reconnect_timer, (uint64_t)s_reconnect_delay_ms * 1000ULL);
    ESP_LOGI(TAG, "Wi-Fi reconnect in %lu ms", (unsigned long)s_reconnect_delay_ms);
    uint32_t next = s_reconnect_delay_ms * 2;
    s_reconnect_delay_ms = next > 30000 ? 30000 : next;
}

static void sta_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        s_reconnect_delay_ms = 2000;
        /* Brief delay so the AP/beacon is up before the first scan (avoids NO_AP_FOUND at boot). */
        schedule_wifi_connect(1000);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = disc ? disc->reason : 0;
        ESP_LOGW(TAG, "STA disconnected: %u (%s)", reason, wifi_disc_reason_str(reason));
        if (reason == WIFI_REASON_NO_AP_FOUND) {
            s_reconnect_delay_ms = 5000;
            ESP_LOGW(TAG, "Check menuconfig SSID \"%s\" (exact 2.4 GHz name, no extra spaces)",
                     CONFIG_MACRO_WIFI_STA_SSID);
        }
        schedule_wifi_reconnect();
    }
}

static void sta_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    s_reconnect_delay_ms = 2000;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    if (!s_http_started) {
        http_server_start();
        if (s_http_started) {
            ESP_LOGI(TAG, "Web UI: http://" IPSTR "/ (same LAN as router)", IP2STR(&ev->ip_info.ip));
        }
    }
    if (s_net_ready) {
        xSemaphoreGive(s_net_ready);
    }
}

static bool wifi_start_sta(esp_netif_t **out_netif)
{
    *out_netif = esp_netif_create_default_wifi_sta();
    if (!*out_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return false;
    }

    if (strlen(CONFIG_MACRO_WIFI_STA_HOSTNAME) > 0) {
        esp_netif_set_hostname(*out_netif, CONFIG_MACRO_WIFI_STA_HOSTNAME);
    }

    const esp_timer_create_args_t reconn_args = {
        .callback = &wifi_reconnect_timer_cb,
        .name = "wifi_reconn",
    };
    const esp_timer_create_args_t conn_args = {
        .callback = &wifi_connect_timer_cb,
        .name = "wifi_conn",
    };
    if (esp_timer_create(&reconn_args, &s_wifi_reconnect_timer) != ESP_OK ||
        esp_timer_create(&conn_args, &s_wifi_connect_timer) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi timer create failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_got_ip, NULL));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    wifi_config_t wifi = {0};
    strncpy((char *)wifi.sta.ssid, CONFIG_MACRO_WIFI_STA_SSID, sizeof(wifi.sta.ssid) - 1);
    const char *pw = CONFIG_MACRO_WIFI_STA_PASSWORD;
    size_t pwlen = strlen(pw);
    if (pwlen > 0) {
        strncpy((char *)wifi.sta.password, pw, sizeof(wifi.sta.password) - 1);
#if CONFIG_MACRO_WIFI_STA_SECURITY_WPA2
        wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi.sta.pmf_cfg.capable = true;
        wifi.sta.pmf_cfg.required = false;
#elif CONFIG_MACRO_WIFI_STA_SECURITY_WPA3
        wifi.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
        wifi.sta.pmf_cfg.capable = true;
        wifi.sta.pmf_cfg.required = true;
        wifi.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#else
        /* Auto: no threshold — driver negotiates WPA2/WPA3 with the AP (worked on most home routers). */
#endif
    } else {
        wifi.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    wifi.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi.sta.failure_retry_cnt = 5;

    ESP_LOGI(TAG, "Connecting to SSID \"%s\" (len=%u, %s, 2.4 GHz)",
             CONFIG_MACRO_WIFI_STA_SSID,
             (unsigned)strlen(CONFIG_MACRO_WIFI_STA_SSID),
#if CONFIG_MACRO_WIFI_STA_SECURITY_WPA2
             "WPA2 required"
#elif CONFIG_MACRO_WIFI_STA_SECURITY_WPA3
             "WPA3 required"
#else
             "security auto"
#endif
    );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (xSemaphoreTake(s_net_ready, pdMS_TO_TICKS(90000)) != pdTRUE) {
        ESP_LOGE(TAG, "No IP in 90s — verify menuconfig SSID/password; router 2.4 GHz WPA2");
        return false;
    }

    return true;
}

#else /* CONFIG_MACRO_WIFI_ROLE_SOFTAP */

static void ap_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    ESP_LOGI(TAG, "WIFI_EVENT_AP_START");
    if (s_net_ready) {
        xSemaphoreGive(s_net_ready);
    }
}

static bool wifi_start_ap(esp_netif_t **out_netif)
{
    *out_netif = esp_netif_create_default_wifi_ap();
    if (!*out_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &ap_wifi_event, NULL));

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, CONFIG_MACRO_WIFI_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = (uint8_t)strlen(CONFIG_MACRO_WIFI_AP_SSID);
    ap.ap.channel = CONFIG_MACRO_WIFI_AP_CHANNEL;
    ap.ap.max_connection = 4;
    const char *pw = CONFIG_MACRO_WIFI_AP_PASSWORD;
    size_t pwlen = strlen(pw);
    if (pwlen >= 8) {
        strncpy((char *)ap.ap.password, pw, sizeof(ap.ap.password) - 1);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (pwlen == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ESP_LOGW(TAG, "AP password < 8 chars: using OPEN (set a longer password for WPA2)");
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (xSemaphoreTake(s_net_ready, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "SoftAP did not start in 15s");
        return false;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(*out_netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "Join Wi-Fi SSID \"%s\" then open http://" IPSTR "/",
                 CONFIG_MACRO_WIFI_AP_SSID, IP2STR(&ip.ip));
    } else {
        ESP_LOGI(TAG, "Join Wi-Fi SSID \"%s\" then open http://192.168.4.1/", CONFIG_MACRO_WIFI_AP_SSID);
    }
    return true;
}

#endif /* role */

static esp_err_t h_root_get(httpd_req_t *req)
{
    char etag[24];
    snprintf(etag, sizeof(etag), "\"mp%u\"", (unsigned)WEB_INDEX_HTML_LEN);

    char if_none[48];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none, sizeof(if_none)) == ESP_OK &&
        strcmp(if_none, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        http_set_conn_close(req);
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "ETag", etag);
    http_set_conn_close(req);
    return http_send_body_chunked(req, WEB_INDEX_HTML, WEB_INDEX_HTML_LEN);
}

static esp_err_t h_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    http_set_conn_close(req);
    return httpd_resp_send(req, "", 0);
}

static esp_err_t h_ping_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_status_get(httpd_req_t *req)
{
    char buf[MACRO_PROFILE_STATUS_JSON_MAX];
    macro_profile_build_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_profile_schema_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_PROFILE_SCHEMA_MD, (ssize_t)DOC_PROFILE_SCHEMA_MD_LEN);
}

static esp_err_t h_sidebar_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_SIDEBAR_MD, (ssize_t)DOC_SIDEBAR_MD_LEN);
}

static esp_err_t h_docs_html_get(httpd_req_t *req)
{
    char etag[24];
    snprintf(etag, sizeof(etag), "\"md%u\"", (unsigned)WEB_DOCS_HTML_LEN);

    char if_none[48];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none, sizeof(if_none)) == ESP_OK &&
        strcmp(if_none, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        http_set_conn_close(req);
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "ETag", etag);
    http_set_conn_close(req);
    return http_send_body_chunked(req, WEB_DOCS_HTML, WEB_DOCS_HTML_LEN);
}

static esp_err_t h_doc_profile_schema_get(httpd_req_t *req)
{
    return h_docs_html_get(req);
}

static esp_err_t h_docs_get(httpd_req_t *req)
{
    return h_docs_html_get(req);
}

static esp_err_t h_profile_post(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > (size_t)CONFIG_MACRO_PROFILE_MAX_SIZE) {
        char msg[96];
        snprintf(msg, sizeof(msg), "profile too large (%u bytes, max %d)", (unsigned)len,
                 CONFIG_MACRO_PROFILE_MAX_SIZE);
        return http_resp_json_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    char *body = malloc(len + 1);
    if (!body) {
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            free(body);
            return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        got += (size_t)r;
    }
    body[len] = '\0';

    group_sequence_t *tmp = calloc(1, sizeof(*tmp));
    if (!tmp) {
        free(body);
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (!macro_profile_parse_json(body, tmp)) {
        const char *detail = macro_profile_get_parse_error();
        if (detail == NULL || detail[0] == '\0') {
            detail = "invalid profile";
        }
        free(tmp);
        free(body);
        return http_resp_json_err(req, HTTPD_400_BAD_REQUEST, detail);
    }

    esp_err_t mnt = macro_profile_ensure_spiffs_mounted();
    if (mnt != ESP_OK) {
        free(tmp);
        free(body);
        ESP_LOGE(TAG, "profile save: SPIFFS mount failed: %s", esp_err_to_name(mnt));
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs");
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_MACRO_SPIFFS_MOUNT, CONFIG_MACRO_PROFILE_JSON);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(tmp);
        free(body);
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d %s", path, errno, strerror(errno));
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open");
    }
    if (fwrite(body, 1, len, f) != len) {
        fclose(f);
        free(tmp);
        free(body);
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
    }
    fclose(f);
    free(body);

    macro_sequences_apply(tmp);
    free(tmp);

    macro_ws_request_broadcast();

    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_profile_get(httpd_req_t *req)
{
    esp_err_t mnt = macro_profile_ensure_spiffs_mounted();
    if (mnt != ESP_OK) {
        ESP_LOGE(TAG, "profile load: SPIFFS mount failed: %s", esp_err_to_name(mnt));
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs");
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_MACRO_SPIFFS_MOUNT, CONFIG_MACRO_PROFILE_JSON);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d %s", path, errno, strerror(errno));
        return http_resp_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek");
    }
    long flen = ftell(f);
    if (flen < 0 || flen > CONFIG_MACRO_PROFILE_MAX_SIZE) {
        fclose(f);
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "size");
    }
    rewind(f);

    char *body = malloc((size_t)flen + 1);
    if (!body) {
        fclose(f);
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    size_t got = fread(body, 1, (size_t)flen, f);
    fclose(f);
    if (got != (size_t)flen) {
        free(body);
        return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read");
    }
    body[got] = '\0';

    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    esp_err_t ret = httpd_resp_send(req, body, (ssize_t)got);
    free(body);
    return ret;
}

static esp_err_t h_next_script_post(httpd_req_t *req)
{
    (void)req;
    macro_profile_http_next_script();
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_active_weapon_post(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len > 32) {
        len = 32;
    }
    char body[33] = {0};
    if (len > 0) {
        size_t got = 0;
        while (got < len) {
            int r = httpd_req_recv(req, body + got, len - got);
            if (r <= 0) {
                return http_resp_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
            }
            got += (size_t)r;
        }
        body[len] = '\0';
    }
    int idx = 0;
    const char *colon = strchr(body, ':');
    if (colon != NULL) {
        idx = atoi(colon + 1);
    } else if (body[0] != '\0') {
        idx = atoi(body);
    }
    if (idx < 0) {
        idx = 0;
    }
    macro_profile_http_set_active_weapon((uint8_t)idx);
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_toggle_macros_post(httpd_req_t *req)
{
    (void)req;
    macro_profile_http_toggle_macros();
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void http_recovery_task_start(void)
{
    if (s_http_recovery_task != NULL) {
        return;
    }
    if (xTaskCreate(http_recovery_task, "http_recover", 4096, NULL, 5, &s_http_recovery_task) != pdPASS) {
        ESP_LOGE(TAG, "http recovery task create failed");
        s_http_recovery_task = NULL;
    }
}

static void http_server_start(void)
{
    if (s_http_started) {
        return;
    }
    s_http_want_running = true;
    http_recovery_task_start();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.backlog_conn = 1;
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;
    /* lwIP often returns ENOPROTOOPT (109) for SO_LINGER on close. */
    cfg.enable_so_linger = false;
    /* Leave lwIP headroom for aborted refresh connections (not in httpd client list). */
    cfg.max_open_sockets = 5;
    if (cfg.max_open_sockets > CONFIG_LWIP_MAX_SOCKETS - 6) {
        cfg.max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 6;
    }
    if (cfg.max_open_sockets < 3) {
        cfg.max_open_sockets = 3;
    }
    s_http_max_clients = cfg.max_open_sockets;
    cfg.max_uri_handlers = 24;
#if CONFIG_HTTPD_WS_SUPPORT
    cfg.close_fn = macro_ws_httpd_close_cb;
#endif

    ESP_LOGI(TAG, "HTTP max_open_sockets=%d backlog=1 (CONFIG_LWIP_MAX_SOCKETS=%d)",
             cfg.max_open_sockets, CONFIG_LWIP_MAX_SOCKETS);

    esp_err_t ret = httpd_start(&s_httpd, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s (lwIP may be out of sockets; cooldown %d s)",
                 esp_err_to_name(ret), HTTP_RESTART_FAIL_COOLDOWN_MS / 1000);
        s_httpd = NULL;
        s_http_started = false;
        http_set_restart_cooldown_ms(HTTP_RESTART_FAIL_COOLDOWN_MS);
        s_http_need_wifi_reset = true;
        http_schedule_recovery();
        return;
    }

#if CONFIG_HTTPD_WS_SUPPORT
    macro_ws_init(s_httpd);
#endif

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = h_root_get, .user_ctx = NULL};
    httpd_uri_t u_fav = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = h_favicon_get, .user_ctx = NULL};
    httpd_uri_t u_ping = {.uri = "/api/ping", .method = HTTP_GET, .handler = h_ping_get, .user_ctx = NULL};
    httpd_uri_t u_status = {.uri = "/api/status", .method = HTTP_GET, .handler = h_status_get, .user_ctx = NULL};
    httpd_uri_t u_profile_schema_md = {.uri = "/api/docs/profile-schema.md", .method = HTTP_GET, .handler = h_profile_schema_md_get, .user_ctx = NULL};
    httpd_uri_t u_sidebar_md = {.uri = "/api/docs/_sidebar.md", .method = HTTP_GET, .handler = h_sidebar_md_get, .user_ctx = NULL};
    httpd_uri_t u_docs = {.uri = "/docs", .method = HTTP_GET, .handler = h_docs_get, .user_ctx = NULL};
    httpd_uri_t u_doc_profile_schema = {.uri = "/docs/profile-schema", .method = HTTP_GET, .handler = h_doc_profile_schema_get, .user_ctx = NULL};
    httpd_uri_t u_prof = {.uri = "/api/profile", .method = HTTP_POST, .handler = h_profile_post, .user_ctx = NULL};
    httpd_uri_t u_prof_get = {.uri = "/api/profile", .method = HTTP_GET, .handler = h_profile_get, .user_ctx = NULL};
    httpd_uri_t u_next = {.uri = "/api/next-script", .method = HTTP_POST, .handler = h_next_script_post, .user_ctx = NULL};
    httpd_uri_t u_weapon = {.uri = "/api/active-weapon", .method = HTTP_POST, .handler = h_active_weapon_post, .user_ctx = NULL};
    httpd_uri_t u_tog = {.uri = "/api/toggle-macros", .method = HTTP_POST, .handler = h_toggle_macros_post, .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_fav));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_ping));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_profile_schema_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_sidebar_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_docs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_doc_profile_schema));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_prof));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_prof_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_next));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_weapon));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_tog));
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_uri_t u_ws = {.uri = "/ws", .method = HTTP_GET, .handler = macro_ws_handler, .user_ctx = NULL,
                        .is_websocket = true};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u_ws));
    ESP_LOGI(TAG, "HTTP server on port 80 (WebSocket /ws enabled)");
#else
    ESP_LOGI(TAG, "HTTP server on port 80 (HTTP only, enable CONFIG_HTTPD_WS_SUPPORT for /ws)");
#endif

    if (s_http_health_timer == NULL) {
        const esp_timer_create_args_t health_args = {
            .callback = &http_health_timer_cb,
            .name = "http_health",
        };
        if (esp_timer_create(&health_args, &s_http_health_timer) != ESP_OK) {
            ESP_LOGW(TAG, "HTTP health timer not created");
        }
    }
    if (s_http_health_timer != NULL) {
        esp_timer_stop(s_http_health_timer);
        esp_timer_start_periodic(s_http_health_timer, 2000000);
    }

    if (s_prev_log_vprintf == NULL) {
        s_prev_log_vprintf = esp_log_set_vprintf(http_log_vprintf);
    }

    s_http_started = true;
    s_http_restart_requested = false;
}

void macro_web_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_net_ready = xSemaphoreCreateBinary();
    if (!s_net_ready) {
        ESP_LOGE(TAG, "no heap for semaphore");
        return;
    }

    esp_netif_t *netif = NULL;
#if CONFIG_MACRO_WIFI_ROLE_STA
    (void)wifi_start_sta(&netif);
    if (!s_http_started) {
        ESP_LOGW(TAG, "USB/macros start without web UI; HTTP starts when Wi-Fi gets an IP");
    }
#else
    if (!wifi_start_ap(&netif)) {
        return;
    }
    (void)netif;
    http_server_start();
#endif
}

#else /* !CONFIG_MACRO_WEB_UI */

void macro_web_start(void) {}

#endif

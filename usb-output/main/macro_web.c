#include "config.h"
#include "sdkconfig.h"
#include "macro_web.h"

#if CONFIG_MACRO_WEB_UI

#include <errno.h>
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

static void http_server_start(void);

/** Free sockets quickly on ESP32 (default keep-alive holds slots until max_open_sockets). */
static void http_set_conn_close(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, WEB_INDEX_HTML, (ssize_t)WEB_INDEX_HTML_LEN);
}

static esp_err_t h_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    http_set_conn_close(req);
    return httpd_resp_send(req, "", 0);
}

static esp_err_t h_status_get(httpd_req_t *req)
{
    char buf[2048];
    macro_profile_build_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_documentation_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_QUICK_WALKTHROUGH_MD, (ssize_t)DOC_QUICK_WALKTHROUGH_MD_LEN);
}

static esp_err_t h_profile_schema_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_PROFILE_SCHEMA_MD, (ssize_t)DOC_PROFILE_SCHEMA_MD_LEN);
}

static esp_err_t h_macro_structure_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_MACRO_STRUCTURE_DETAILED_GUIDE_MD,
                           (ssize_t)DOC_MACRO_STRUCTURE_DETAILED_GUIDE_MD_LEN);
}

static esp_err_t h_macro_plan_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_MACRO_WEB_UI_PLAN_MD, (ssize_t)DOC_MACRO_WEB_UI_PLAN_MD_LEN);
}

static esp_err_t h_docs_index_md_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOC_README_MD, (ssize_t)DOC_README_MD_LEN);
}

static esp_err_t send_doc_page(httpd_req_t *req, const char *title, const char *md_api_path)
{
    char buf[4096];
    snprintf(buf, sizeof(buf),
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title>"
             "<style>body{font-family:Arial,sans-serif;max-width:980px;margin:24px auto;padding:0 16px;line-height:1.45}"
             "h1,h2,h3{margin:18px 0 8px} pre{background:#111;color:#eee;padding:12px;overflow:auto}"
             "code{background:#f3f3f3;padding:1px 4px;border-radius:3px} ul{padding-left:24px}</style></head><body>"
             "<p><a href=\"/\">Home</a> | <a href=\"/docs\">Docs</a> | <a href=\"/documentation\">Quick Walkthrough</a></p>"
             "<h1>%s</h1><div id=\"doc\">Loading…</div>"
             "<script>"
             "function esc(s){return s.replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;');}"
             "function inline(s){return esc(s).replace(/`([^`]+)`/g,'<code>$1</code>');}"
             "function render(md){var out=[],inList=false,inCode=false;"
             "md.split(/\\r?\\n/).forEach(function(line){"
             "if(line.startsWith('```')){if(!inCode){out.push('<pre><code>');inCode=true;}else{out.push('</code></pre>');inCode=false;}return;}"
             "if(inCode){out.push(esc(line));return;}"
             "if(line.startsWith('### ')){if(inList){out.push('</ul>');inList=false;}out.push('<h3>'+inline(line.slice(4))+'</h3>');return;}"
             "if(line.startsWith('## ')){if(inList){out.push('</ul>');inList=false;}out.push('<h2>'+inline(line.slice(3))+'</h2>');return;}"
             "if(line.startsWith('# ')){if(inList){out.push('</ul>');inList=false;}out.push('<h1>'+inline(line.slice(2))+'</h1>');return;}"
             "if(line.startsWith('- ')){if(!inList){out.push('<ul>');inList=true;}out.push('<li>'+inline(line.slice(2))+'</li>');return;}"
             "if(line.trim()===''){if(inList){out.push('</ul>');inList=false;}out.push('');return;}"
             "if(inList){out.push('</ul>');inList=false;}out.push('<p>'+inline(line)+'</p>');});"
             "if(inList)out.push('</ul>');if(inCode)out.push('</code></pre>');return out.join('\\n');}"
             "fetch('%s').then(function(r){if(!r.ok)throw r;return r.text();})"
             ".then(function(md){document.getElementById('doc').innerHTML=render(md);})"
             ".catch(function(){document.getElementById('doc').textContent='Failed to load documentation';});"
             "</script></body></html>",
             title, title, md_api_path);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_documentation_get(httpd_req_t *req)
{
    return send_doc_page(req, "Quick Walkthrough", "/api/documentation.md");
}

static esp_err_t h_doc_profile_schema_get(httpd_req_t *req)
{
    return send_doc_page(req, "Profile Schema", "/api/docs/profile-schema.md");
}

static esp_err_t h_doc_macro_structure_get(httpd_req_t *req)
{
    return send_doc_page(req, "Macro Structure Detailed Guide", "/api/docs/macro-structure.md");
}

static esp_err_t h_doc_macro_plan_get(httpd_req_t *req)
{
    return send_doc_page(req, "Macro Web UI Plan", "/api/docs/macro-web-ui-plan.md");
}

static esp_err_t h_doc_index_get(httpd_req_t *req)
{
    return send_doc_page(req, "Documentation Index", "/api/docs/index.md");
}

static const char DOCS_INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Docs</title>"
    "<style>body{font-family:Arial,sans-serif;max-width:980px;margin:24px auto;padding:0 16px;line-height:1.45}"
    "li{margin:8px 0}</style></head><body>"
    "<p><a href=\"/\">Home</a> | <a href=\"/docs\">Docs</a> | <a href=\"/documentation\">Quick Walkthrough</a></p>"
    "<h1>Documentation</h1>"
    "<p>Use these links to navigate available docs in the device UI.</p>"
    "<ul>"
    "<li><a href=\"/documentation\">Quick Walkthrough</a></li>"
    "<li><a href=\"/docs/profile-schema\">Profile Schema</a></li>"
    "<li><a href=\"/docs/macro-structure\">Macro Structure Detailed Guide</a></li>"
    "<li><a href=\"/docs/macro-web-ui-plan\">Macro Web UI Plan</a></li>"
    "<li><a href=\"/docs/index\">Docs Index</a></li>"
    "<li><a href=\"/\">Main control page</a></li>"
    "</ul>"
    "</body></html>";

static esp_err_t h_docs_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    http_set_conn_close(req);
    return httpd_resp_send(req, DOCS_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_profile_post(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > (size_t)CONFIG_MACRO_PROFILE_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    char *body = malloc(len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    body[len] = '\0';

    group_sequence_t *tmp = calloc(1, sizeof(*tmp));
    if (!tmp) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    if (!macro_profile_parse_json(body, tmp)) {
        free(tmp);
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid profile");
        return ESP_FAIL;
    }

    esp_err_t mnt = macro_profile_ensure_spiffs_mounted();
    if (mnt != ESP_OK) {
        free(tmp);
        free(body);
        ESP_LOGE(TAG, "profile save: SPIFFS mount failed: %s", esp_err_to_name(mnt));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs");
        return ESP_FAIL;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_MACRO_SPIFFS_MOUNT, CONFIG_MACRO_PROFILE_JSON);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(tmp);
        free(body);
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d %s", path, errno, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open");
        return ESP_FAIL;
    }
    if (fwrite(body, 1, len, f) != len) {
        fclose(f);
        free(tmp);
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
        return ESP_FAIL;
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
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs");
        return ESP_FAIL;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_MACRO_SPIFFS_MOUNT, CONFIG_MACRO_PROFILE_JSON);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d %s", path, errno, strerror(errno));
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "seek");
        return ESP_FAIL;
    }
    long flen = ftell(f);
    if (flen < 0 || flen > CONFIG_MACRO_PROFILE_MAX_SIZE) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "size");
        return ESP_FAIL;
    }
    rewind(f);

    char *body = malloc((size_t)flen + 1);
    if (!body) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    size_t got = fread(body, 1, (size_t)flen, f);
    fclose(f);
    if (got != (size_t)flen) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read");
        return ESP_FAIL;
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

static esp_err_t h_toggle_macros_post(httpd_req_t *req)
{
    (void)req;
    macro_profile_http_toggle_macros();
    httpd_resp_set_type(req, "application/json");
    http_set_conn_close(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void http_server_start(void)
{
    if (s_http_started) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    /* ESP-IDF reserves 3 lwIP sockets for the HTTP server (see httpd_start error text). */
    cfg.max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - 3;
    if (cfg.max_open_sockets < 1) {
        cfg.max_open_sockets = 1;
    }
    cfg.max_uri_handlers = 24;
#if CONFIG_HTTPD_WS_SUPPORT
    cfg.close_fn = macro_ws_httpd_close_cb;
#endif

    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "HTTP max_open_sockets=%d (CONFIG_LWIP_MAX_SOCKETS=%d)",
             cfg.max_open_sockets, CONFIG_LWIP_MAX_SOCKETS);

    esp_err_t ret = httpd_start(&server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return;
    }

#if CONFIG_HTTPD_WS_SUPPORT && (CONFIG_LWIP_MAX_SOCKETS >= 16)
    macro_ws_init(server);
#endif

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = h_root_get, .user_ctx = NULL};
    httpd_uri_t u_fav = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = h_favicon_get, .user_ctx = NULL};
    httpd_uri_t u_status = {.uri = "/api/status", .method = HTTP_GET, .handler = h_status_get, .user_ctx = NULL};
    httpd_uri_t u_doc_md = {.uri = "/api/documentation.md", .method = HTTP_GET, .handler = h_documentation_md_get, .user_ctx = NULL};
    httpd_uri_t u_profile_schema_md = {.uri = "/api/docs/profile-schema.md", .method = HTTP_GET, .handler = h_profile_schema_md_get, .user_ctx = NULL};
    httpd_uri_t u_macro_structure_md = {.uri = "/api/docs/macro-structure.md", .method = HTTP_GET, .handler = h_macro_structure_md_get, .user_ctx = NULL};
    httpd_uri_t u_macro_plan_md = {.uri = "/api/docs/macro-web-ui-plan.md", .method = HTTP_GET, .handler = h_macro_plan_md_get, .user_ctx = NULL};
    httpd_uri_t u_docs_index_md = {.uri = "/api/docs/index.md", .method = HTTP_GET, .handler = h_docs_index_md_get, .user_ctx = NULL};
    httpd_uri_t u_doc = {.uri = "/documentation", .method = HTTP_GET, .handler = h_documentation_get, .user_ctx = NULL};
    httpd_uri_t u_docs = {.uri = "/docs", .method = HTTP_GET, .handler = h_docs_get, .user_ctx = NULL};
    httpd_uri_t u_doc_profile_schema = {.uri = "/docs/profile-schema", .method = HTTP_GET, .handler = h_doc_profile_schema_get, .user_ctx = NULL};
    httpd_uri_t u_doc_macro_structure = {.uri = "/docs/macro-structure", .method = HTTP_GET, .handler = h_doc_macro_structure_get, .user_ctx = NULL};
    httpd_uri_t u_doc_macro_plan = {.uri = "/docs/macro-web-ui-plan", .method = HTTP_GET, .handler = h_doc_macro_plan_get, .user_ctx = NULL};
    httpd_uri_t u_doc_index = {.uri = "/docs/index", .method = HTTP_GET, .handler = h_doc_index_get, .user_ctx = NULL};
    httpd_uri_t u_prof = {.uri = "/api/profile", .method = HTTP_POST, .handler = h_profile_post, .user_ctx = NULL};
    httpd_uri_t u_prof_get = {.uri = "/api/profile", .method = HTTP_GET, .handler = h_profile_get, .user_ctx = NULL};
    httpd_uri_t u_next = {.uri = "/api/next-script", .method = HTTP_POST, .handler = h_next_script_post, .user_ctx = NULL};
    httpd_uri_t u_tog = {.uri = "/api/toggle-macros", .method = HTTP_POST, .handler = h_toggle_macros_post, .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_fav));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_doc_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_profile_schema_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_macro_structure_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_macro_plan_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_docs_index_md));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_doc));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_docs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_doc_profile_schema));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_doc_macro_structure));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_doc_macro_plan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_doc_index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_prof));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_prof_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_next));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_tog));
#if CONFIG_HTTPD_WS_SUPPORT && (CONFIG_LWIP_MAX_SOCKETS >= 16)
    httpd_uri_t u_ws = {.uri = "/ws", .method = HTTP_GET, .handler = macro_ws_handler, .user_ctx = NULL,
                        .is_websocket = true};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u_ws));
    ESP_LOGI(TAG, "HTTP server on port 80 (WebSocket /ws enabled)");
#elif CONFIG_HTTPD_WS_SUPPORT
    ESP_LOGW(TAG, "HTTP server on port 80 (WebSocket off: CONFIG_LWIP_MAX_SOCKETS=%d, need >=16)",
             CONFIG_LWIP_MAX_SOCKETS);
#else
    ESP_LOGI(TAG, "HTTP server on port 80 (WebSocket disabled in sdkconfig)");
#endif
    s_http_started = true;
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

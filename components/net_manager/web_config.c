#include "net_manager_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "web_config";

#define SOFTAP_DEFAULT_SSID     "Xiaozhi_Setup"
#define SOFTAP_DEFAULT_PASS     "12345678"
#define STA_CFG_TIMEOUT_MS      10000U
#define HTML_PSRAM_BUF_SIZE     4096U
#define POST_MAX_BODY_LEN       1024U

static httpd_handle_t s_httpd = NULL;

static size_t bounded_strlen(const char *s, size_t max_len)
{
    size_t i = 0;
    while (i < max_len && s[i] != '\0') {
        i++;
    }
    return i;
}

static void set_err(char *err_msg, size_t err_len, const char *msg)
{
    if (err_msg != NULL && err_len > 0U) {
        strncpy(err_msg, msg, err_len - 1U);
        err_msg[err_len - 1U] = '\0';
    }
}

esp_err_t web_config_validate(const sys_config_t *cfg, char *err_msg, size_t err_len)
{
    if (cfg == NULL) {
        set_err(err_msg, err_len, "Config pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (bounded_strlen(cfg->wifi_ssid, sizeof(cfg->wifi_ssid)) >= sizeof(cfg->wifi_ssid)) {
        set_err(err_msg, err_len, "wifi_ssid exceeds 31 chars");
        return ESP_ERR_INVALID_ARG;
    }
    size_t pass_len = bounded_strlen(cfg->wifi_pass, sizeof(cfg->wifi_pass));
    if (pass_len >= sizeof(cfg->wifi_pass) || (pass_len > 0U && pass_len < 8U)) {
        set_err(err_msg, err_len, "wifi_pass must be empty or 8~63 chars");
        return ESP_ERR_INVALID_ARG;
    }
    size_t url_len = bounded_strlen(cfg->server_url, sizeof(cfg->server_url));
    if (url_len >= sizeof(cfg->server_url)) {
        set_err(err_msg, err_len, "server_url exceeds 127 chars");
        return ESP_ERR_INVALID_ARG;
    }
    if (url_len > 0U) {
        bool valid_scheme = (strncmp(cfg->server_url, "http://", 7) == 0) ||
                            (strncmp(cfg->server_url, "https://", 8) == 0) ||
                            (strncmp(cfg->server_url, "ws://", 5) == 0) ||
                            (strncmp(cfg->server_url, "wss://", 6) == 0);
        if (!valid_scheme) {
            set_err(err_msg, err_len, "server_url must start with http/https/ws/wss");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (cfg->sample_interval_sec < 5U || cfg->sample_interval_sec > 3600U) {
        set_err(err_msg, err_len, "sample_interval_sec must be 5~3600");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->psram_window_hours < 1U || cfg->psram_window_hours > 24U) {
        set_err(err_msg, err_len, "psram_window_hours must be 1~24");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->eeprom_warn_pct < 50U || cfg->eeprom_warn_pct > 95U) {
        set_err(err_msg, err_len, "eeprom_warn_pct must be 50~95");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sd_warn_free_mb < 50U || cfg->sd_warn_free_mb > 1000U) {
        set_err(err_msg, err_len, "sd_warn_free_mb must be 50~1000");
        return ESP_ERR_INVALID_ARG;
    }
    set_err(err_msg, err_len, "OK");
    return ESP_OK;
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && (di + 1U) < dst_len; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' &&
                   isxdigit((unsigned char)src[si + 1]) &&
                   isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool extract_form_field(const char *body, const char *key,
                               char *out_val, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val_start = p + klen + 1U;
            const char *amp = strchr(val_start, '&');
            size_t raw_len = (amp != NULL) ? (size_t)(amp - val_start) : strlen(val_start);
            char raw_buf[256];
            if (raw_len >= sizeof(raw_buf)) {
                raw_len = sizeof(raw_buf) - 1U;
            }
            memcpy(raw_buf, val_start, raw_len);
            raw_buf[raw_len] = '\0';
            url_decode(out_val, out_len, raw_buf);
            return true;
        }
        const char *next = strchr(p, '&');
        p = (next != NULL) ? (next + 1U) : NULL;
    }
    return false;
}

static esp_err_t http_get_root_handler(httpd_req_t *req)
{
    char *html = (char *)heap_caps_malloc(HTML_PSRAM_BUF_SIZE,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (html == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "PSRAM OOM");
        return ESP_FAIL;
    }

    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    snprintf(html, HTML_PSRAM_BUF_SIZE,
             "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Xiaozhi IoT Setup</title></head><body>"
             "<h2>Xiaozhi IoT 系統設定</h2>"
             "<form method='POST' action='/save'>"
             "Wi-Fi SSID:<br><input name='wifi_ssid' maxlength='31' value='%s'><br><br>"
             "Wi-Fi 密碼:<br><input type='password' name='wifi_pass' maxlength='63' "
             "placeholder='留空則不修改原密碼'><br><br>"
             "Server URL:<br><input name='server_url' maxlength='127' value='%s'><br><br>"
             "採樣週期 (5~3600秒):<br><input type='number' name='sample_interval_sec' "
             "min='5' max='3600' value='%u'><br><br>"
             "PSRAM 統計視窗 (1~24小時):<br><input type='number' name='psram_window_hours' "
             "min='1' max='24' value='%u'><br><br>"
             "EEPROM 警告門檻 (50~95%%):<br><input type='number' name='eeprom_warn_pct' "
             "min='50' max='95' value='%u'><br><br>"
             "SD 卡剩餘警告 (50~1000MB):<br><input type='number' name='sd_warn_free_mb' "
             "min='50' max='1000' value='%u'><br><br>"
             "<button type='submit'>儲存設定</button></form></body></html>",
             cfg.wifi_ssid, cfg.server_url,
             (unsigned)cfg.sample_interval_sec,
             (unsigned)cfg.psram_window_hours,
             (unsigned)cfg.eeprom_warn_pct,
             (unsigned)cfg.sd_warn_free_mb);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t res = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    heap_caps_free(html);
    return res;
}

static esp_err_t http_post_save_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > POST_MAX_BODY_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
        return ESP_FAIL;
    }

    char body[POST_MAX_BODY_LEN + 1U];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int r = httpd_req_recv(req, body + received,
                               (size_t)req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    sys_config_t candidate;
    net_mgr_get_config_copy(&candidate);

    char val[128];
    if (extract_form_field(body, "wifi_ssid", val, sizeof(val))) {
        strncpy(candidate.wifi_ssid, val, sizeof(candidate.wifi_ssid) - 1U);
        candidate.wifi_ssid[sizeof(candidate.wifi_ssid) - 1U] = '\0';
    }
    if (extract_form_field(body, "wifi_pass", val, sizeof(val)) && val[0] != '\0') {
        strncpy(candidate.wifi_pass, val, sizeof(candidate.wifi_pass) - 1U);
        candidate.wifi_pass[sizeof(candidate.wifi_pass) - 1U] = '\0';
    }
    if (extract_form_field(body, "server_url", val, sizeof(val))) {
        strncpy(candidate.server_url, val, sizeof(candidate.server_url) - 1U);
        candidate.server_url[sizeof(candidate.server_url) - 1U] = '\0';
    }
    if (extract_form_field(body, "sample_interval_sec", val, sizeof(val))) {
        candidate.sample_interval_sec = (uint16_t)strtoul(val, NULL, 10);
    }
    if (extract_form_field(body, "psram_window_hours", val, sizeof(val))) {
        candidate.psram_window_hours = (uint8_t)strtoul(val, NULL, 10);
    }
    if (extract_form_field(body, "eeprom_warn_pct", val, sizeof(val))) {
        candidate.eeprom_warn_pct = (uint8_t)strtoul(val, NULL, 10);
    }
    if (extract_form_field(body, "sd_warn_free_mb", val, sizeof(val))) {
        candidate.sd_warn_free_mb = (uint16_t)strtoul(val, NULL, 10);
    }

    char err_msg[96];
    if (web_config_validate(&candidate, err_msg, sizeof(err_msg)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
        return ESP_FAIL;
    }

    if (net_mgr_save_config(&candidate) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, "<h3>設定已儲存成功！</h3><a href='/'>返回</a>",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_status_handler(httpd_req_t *req)
{
    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    uint16_t eeprom_cnt = 0;
    bool sd_mounted = false;
    uint32_t sd_free_mb = 0;
    net_mgr_query_storage_status(&eeprom_cnt, &sd_mounted, &sd_free_mb);

    char json[320];
    snprintf(json, sizeof(json),
             "{\"mode\":%d,\"wifi_ssid\":\"%s\",\"sample_interval_sec\":%u,"
             "\"eeprom_records\":%u,\"sd_mounted\":%s,\"sd_free_mb\":%lu}",
             (int)net_manager_get_mode(), cfg.wifi_ssid,
             (unsigned)cfg.sample_interval_sec, (unsigned)eeprom_cnt,
             sd_mounted ? "true" : "false", (unsigned long)sd_free_mb);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool try_sta_connect_with_timeout(const sys_config_t *cfg)
{
    if (cfg->wifi_ssid[0] == '\0') {
        return false;
    }

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1U);
    strncpy((char *)sta_cfg.sta.password, cfg->wifi_pass, sizeof(sta_cfg.sta.password) - 1U);
    sta_cfg.sta.threshold.authmode = (cfg->wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK
                                                                : WIFI_AUTH_OPEN;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        return false;
    }

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    for (uint32_t elapsed = 0; elapsed < STA_CFG_TIMEOUT_MS; elapsed += 200U) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (sta != NULL && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0U) {
            char ip_str[20];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            net_mgr_notify_mode(NET_MODE_WEB_CFG, ip_str);
            return true;
        }
    }

    (void)esp_wifi_stop();
    return false;
}

static esp_err_t start_softap_fallback(void)
{
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, SOFTAP_DEFAULT_SSID, sizeof(ap_cfg.ap.ssid) - 1U);
    strncpy((char *)ap_cfg.ap.password, SOFTAP_DEFAULT_PASS, sizeof(ap_cfg.ap.password) - 1U);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(SOFTAP_DEFAULT_SSID);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "已啟動 SoftAP: SSID=%s, IP=192.168.4.1", SOFTAP_DEFAULT_SSID);
        net_mgr_notify_mode(NET_MODE_WEB_CFG, "192.168.4.1");
    }
    return err;
}

esp_err_t web_config_start(const sys_config_t *cfg)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    if (!try_sta_connect_with_timeout(cfg)) {
        esp_err_t ap_err = start_softap_fallback();
        if (ap_err != ESP_OK) {
            return ap_err;
        }
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.core_id        = 0;
    httpd_cfg.stack_size     = 8192;

    esp_err_t err = httpd_start(&s_httpd, &httpd_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uri_root   = { .uri = "/",       .method = HTTP_GET,  .handler = http_get_root_handler };
    const httpd_uri_t uri_save   = { .uri = "/save",   .method = HTTP_POST, .handler = http_post_save_handler };
    const httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET,  .handler = http_get_status_handler };

    (void)httpd_register_uri_handler(s_httpd, &uri_root);
    (void)httpd_register_uri_handler(s_httpd, &uri_save);
    (void)httpd_register_uri_handler(s_httpd, &uri_status);
    return ESP_OK;
}

void web_config_stop(void)
{
    if (s_httpd != NULL) {
        (void)httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}
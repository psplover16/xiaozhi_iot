#include "web_api.h"
#include "net_manager_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "web_api";

#define API_POST_MAX_LEN        512U
#define WIFI_OFF_DELAY_MS       180U
#define CORE1_SYNC_YIELD_MS     15U

void web_api_json_escape(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && (di + 2U) < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c >= 0x20U) {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
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

bool web_api_extract_form_field(const char *body, const char *key,
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

bool web_api_recv_post_body(httpd_req_t *req, char *out_body, size_t buf_len)
{
    if (req->content_len == 0 || (size_t)req->content_len >= buf_len) {
        return false;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int r = httpd_req_recv(req, out_body + received,
                               (size_t)req->content_len - received);
        if (r <= 0) {
            return false;
        }
        received += (size_t)r;
    }
    out_body[received] = '\0';
    return true;
}

esp_err_t web_api_send_static(httpd_req_t *req,
                              const uint8_t *start,
                              const uint8_t *end,
                              const char *content_type,
                              const char *cache_ctrl)
{
    size_t len = (size_t)(end - start);
    if (len > 0U && start[len - 1U] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, content_type);
    if (cache_ctrl != NULL) {
        httpd_resp_set_hdr(req, "Cache-Control", cache_ctrl);
    }
    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
}

static void build_state_json(char *out_json, size_t json_len)
{
    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    uint16_t eeprom_cnt = 0;
    bool sd_mounted = false;
    uint32_t sd_free_mb = 0;
    net_mgr_query_storage_status(&eeprom_cnt, &sd_mounted, &sd_free_mb);

    sys_status_snapshot_t snap = {0};
    (void)net_mgr_get_sys_status(&snap);

    char esc_ssid[64], esc_err[128];
    web_api_json_escape(esc_ssid, sizeof(esc_ssid), cfg.wifi_ssid);
    web_api_json_escape(esc_err, sizeof(esc_err), snap.last_err);

    snprintf(out_json, json_len,
             "{\"mode\":%d,\"wifi_en\":%s,\"cloud_en\":%s,\"wifi_state\":%d,"
             "\"wifi_ssid\":\"%s\",\"has_server_url\":%s,\"sample_interval_sec\":%u,"
             "\"eeprom_records\":%u,\"sd_mounted\":%s,\"sd_free_mb\":%lu,"
             "\"sensor_valid\":%s,\"temp_c\":%.1f,\"humi_pct\":%.1f,"
             "\"lux\":%u,\"sensor_ts\":%lu,\"volume_pct\":%d,"
             "\"is_muted\":%s,\"is_music_playing\":%s,\"is_audio_busy\":%s,"
             "\"err_seq\":%lu,\"last_err\":\"%s\"}",
             (int)net_manager_get_mode(),
             net_manager_is_wifi_enabled() ? "true" : "false",
             net_manager_is_cloud_enabled() ? "true" : "false",
             (int)net_manager_get_wifi_state(),
             esc_ssid,
             (cfg.server_url[0] != '\0') ? "true" : "false",
             (unsigned)cfg.sample_interval_sec,
             (unsigned)eeprom_cnt,
             sd_mounted ? "true" : "false",
             (unsigned long)sd_free_mb,
             snap.sensor_valid ? "true" : "false",
             (double)snap.temp_c,
             (double)snap.humi_pct,
             (unsigned)snap.lux,
             (unsigned long)snap.sensor_ts,
             (int)snap.volume_pct,
             snap.is_muted ? "true" : "false",
             snap.is_music_playing ? "true" : "false",
             snap.is_audio_busy ? "true" : "false",
             (unsigned long)snap.err_seq,
             esc_err);
}

static bool parse_control_payload(const char *body, char *out_cmd, size_t cmd_len, int *out_val)
{
    out_cmd[0] = '\0';
    *out_val = 0;

    cJSON *root = cJSON_Parse(body);
    if (root != NULL) {
        const cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        if (cJSON_IsString(j_cmd) && j_cmd->valuestring != NULL) {
            strncpy(out_cmd, j_cmd->valuestring, cmd_len - 1U);
            out_cmd[cmd_len - 1U] = '\0';
        }
        const cJSON *j_val = cJSON_GetObjectItemCaseSensitive(root, "val");
        if (cJSON_IsNumber(j_val)) {
            *out_val = j_val->valueint;
        } else if (cJSON_IsBool(j_val)) {
            *out_val = cJSON_IsTrue(j_val) ? 1 : 0;
        } else if (cJSON_IsString(j_val) && j_val->valuestring != NULL) {
            *out_val = (strcmp(j_val->valuestring, "1") == 0 ||
                        strcasecmp(j_val->valuestring, "true") == 0) ? 1 : 0;
        }
        cJSON_Delete(root);
        return (out_cmd[0] != '\0');
    }

    if (web_api_extract_form_field(body, "cmd", out_cmd, cmd_len)) {
        char val_str[16];
        if (web_api_extract_form_field(body, "val", val_str, sizeof(val_str))) {
            *out_val = (strcmp(val_str, "1") == 0 ||
                        strcasecmp(val_str, "true") == 0) ? 1 : 0;
        }
        return true;
    }
    return false;
}

static esp_err_t send_control_response(httpd_req_t *req, bool ok, const char *msg)
{
    char state_json[640], esc_msg[128], resp_json[860];
    build_state_json(state_json, sizeof(state_json));
    web_api_json_escape(esc_msg, sizeof(esc_msg), msg);

    snprintf(resp_json, sizeof(resp_json),
             "{\"ok\":%s,\"msg\":\"%s\",\"state\":%s}",
             ok ? "true" : "false", esc_msg, state_json);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, resp_json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_get_status_handler(httpd_req_t *req)
{
    char json[640];
    build_state_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_post_control_handler(httpd_req_t *req)
{
    char body[API_POST_MAX_LEN + 1U];
    if (!web_api_recv_post_body(req, body, sizeof(body))) {
        return send_control_response(req, false, "無效的請求內容");
    }

    char cmd[32];
    int val = 0;
    if (!parse_control_payload(body, cmd, sizeof(cmd), &val)) {
        return send_control_response(req, false, "缺少控制指令參數");
    }

    if (strcmp(cmd, "set_cloud") == 0) {
        if (val != 0) {
            sys_config_t cfg;
            net_mgr_get_config_copy(&cfg);
            if (cfg.server_url[0] == '\0') {
                return send_control_response(req, false, "請先於下方設定伺服器網址");
            }
            net_manager_set_cloud_enabled(true);
            return send_control_response(req, true, "雲端連線已開啟");
        }
        net_manager_set_cloud_enabled(false);
        return send_control_response(req, true, "雲端連線已關閉");
    }

    if (strcmp(cmd, "set_wifi") == 0) {
        if (val != 0) {
            (void)net_manager_set_wifi_enabled(true);
            return send_control_response(req, true, "Wi-Fi 已開啟");
        }
        net_mode_ctrl_set_wifi_delayed(false, WIFI_OFF_DELAY_MS);
        return send_control_response(req, true, "Wi-Fi 即將關閉");
    }

    ctrl_cmd_id_t ctrl_id;
    if (strcmp(cmd, "vol_up") == 0) {
        ctrl_id = CTRL_CMD_VOL_UP;
    } else if (strcmp(cmd, "vol_down") == 0) {
        ctrl_id = CTRL_CMD_VOL_DOWN;
    } else if (strcmp(cmd, "mute_toggle") == 0) {
        ctrl_id = CTRL_CMD_MUTE_TOGGLE;
    } else if (strcmp(cmd, "music_toggle") == 0) {
        ctrl_id = CTRL_CMD_MUSIC_TOGGLE;
    } else if (strcmp(cmd, "speak_env") == 0) {
        ctrl_id = CTRL_CMD_SPEAK_ENV;
    } else {
        ESP_LOGW(TAG, "未知的控制指令: %s", cmd);
        return send_control_response(req, false, "不支援的控制指令");
    }

    esp_err_t err = net_mgr_exec_ctrl_cmd(ctrl_id);
    if (err != ESP_OK) {
        return send_control_response(req, false, "系統尚未就緒");
    }
    vTaskDelay(pdMS_TO_TICKS(CORE1_SYNC_YIELD_MS));
    return send_control_response(req, true, "指令已執行");
}

esp_err_t web_api_register_routes(httpd_handle_t httpd)
{
    if (httpd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const httpd_uri_t api_uris[] = {
        { .uri = "/status",      .method = HTTP_GET,  .handler = http_get_status_handler },
        { .uri = "/api/control", .method = HTTP_POST, .handler = http_post_control_handler },
    };
    for (size_t i = 0; i < sizeof(api_uris) / sizeof(api_uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(httpd, &api_uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
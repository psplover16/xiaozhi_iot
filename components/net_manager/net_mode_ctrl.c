#include "net_manager_priv.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "net_mode_ctrl";

#define NVS_NAMESPACE_CFG       "xiaozhi_cfg"
#define NVS_KEY_WIFI_EN         "wifi_en"
#define NVS_KEY_CLOUD_EN        "cloud_en"
#define NVS_COMMIT_DEBOUNCE_MS  1000U
#define STA_MAX_RETRY_COUNT     3U
#define BTN_SHORT_DEBOUNCE_MS   50LL
#define BTN_LONG_PRESS_MS       1500LL
#define SOFTAP_DEFAULT_SSID     "Xiaozhi_Setup"
#define SOFTAP_DEFAULT_PASS     "12345678"
#define MDNS_HOSTNAME           "xiaozhi"

static atomic_bool      s_wifi_en        = true;
static atomic_bool      s_cloud_en       = true;
static atomic_int       s_wifi_state     = WIFI_CONN_STATE_OFF;
static atomic_bool      s_is_ap_mode     = false;
static atomic_bool      s_wifi_dirty     = false;
static atomic_bool      s_nvs_dirty      = false;
static TickType_t       s_nvs_dirty_tick = 0;

static bool             s_force_ap_setup = false;
static bool             s_mdns_started   = false;
static uint8_t          s_sta_retry_cnt  = 0;
static char             s_last_ip[20]    = {0};

static bool             s_delayed_wifi_pending  = false;
static bool             s_delayed_wifi_target   = false;
static TickType_t       s_delayed_wifi_deadline = 0;

static bool             s_btn_tracking     = false;
static bool             s_btn_long_handled = false;
static bool             s_wait_boot_release = false;
static int64_t          s_btn_press_ms     = 0;

static void mark_nvs_dirty(void)
{
    s_nvs_dirty_tick = xTaskGetTickCount();
    atomic_store(&s_nvs_dirty, true);
}

static void load_switch_nvs(void)
{
    uint8_t wifi_val  = 1U;
    uint8_t cloud_val = 1U;
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READONLY, &nvs) == ESP_OK) {
        (void)nvs_get_u8(nvs, NVS_KEY_WIFI_EN, &wifi_val);
        (void)nvs_get_u8(nvs, NVS_KEY_CLOUD_EN, &cloud_val);
        nvs_close(nvs);
    }
    atomic_store(&s_wifi_en, (wifi_val != 0U));
    atomic_store(&s_cloud_en, (cloud_val != 0U));
}

static void flush_switch_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    uint8_t w = atomic_load(&s_wifi_en) ? 1U : 0U;
    uint8_t c = atomic_load(&s_cloud_en) ? 1U : 0U;
    if (nvs_set_u8(nvs, NVS_KEY_WIFI_EN, w) == ESP_OK &&
        nvs_set_u8(nvs, NVS_KEY_CLOUD_EN, c) == ESP_OK) {
        (void)nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static void start_or_update_mdns(void)
{
    if (!s_mdns_started) {
        if (mdns_init() != ESP_OK) {
            ESP_LOGW(TAG, "mDNS 初始化失敗");
            return;
        }
        (void)mdns_instance_name_set("Xiaozhi IoT Node");
        (void)mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_started = true;
    }
    (void)mdns_hostname_set(MDNS_HOSTNAME);
}

static void stop_mdns(void)
{
    if (s_mdns_started) {
        mdns_free();
        s_mdns_started = false;
    }
}

static void enter_softap_mode(void)
{
    (void)esp_wifi_stop();
    net_mgr_set_sta_connected(false, NULL);
    s_last_ip[0] = '\0';

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, SOFTAP_DEFAULT_SSID, sizeof(ap_cfg.ap.ssid) - 1U);
    strncpy((char *)ap_cfg.ap.password, SOFTAP_DEFAULT_PASS, sizeof(ap_cfg.ap.password) - 1U);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(SOFTAP_DEFAULT_SSID);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    if (esp_wifi_start() == ESP_OK) {
        atomic_store(&s_is_ap_mode, true);
        atomic_store(&s_wifi_state, WIFI_CONN_STATE_AP_SETUP);
        (void)web_config_httpd_start();
        start_or_update_mdns();
        net_mgr_notify_mode(NET_MODE_WEB_CFG, "192.168.4.1");
        ESP_LOGI(TAG, "已啟動 SoftAP 配網: SSID=%s, IP=192.168.4.1", SOFTAP_DEFAULT_SSID);
    }
}

static void apply_wifi_transition(void)
{
    bool wifi_on = atomic_load(&s_wifi_en);
    s_sta_retry_cnt = 0;
    sync_engine_lock_live();

    if (!wifi_on) {
        s_force_ap_setup = false;
        web_config_stop();
        stop_mdns();
        (void)esp_wifi_stop();
        (void)esp_wifi_set_mode(WIFI_MODE_NULL);
        net_mgr_set_sta_connected(false, NULL);
        s_last_ip[0] = '\0';
        atomic_store(&s_is_ap_mode, false);
        atomic_store(&s_wifi_state, WIFI_CONN_STATE_OFF);
        net_mgr_notify_mode(NET_MODE_OFFLINE, "OFFLINE");
        ESP_LOGI(TAG, "區網 Wi-Fi 已關閉 (WIFI_MODE_NULL)");
        return;
    }

    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    if (s_force_ap_setup || cfg.wifi_ssid[0] == '\0') {
        s_force_ap_setup = false;
        enter_softap_mode();
        return;
    }

    (void)esp_wifi_stop();
    net_mgr_set_sta_connected(false, NULL);
    s_last_ip[0] = '\0';
    atomic_store(&s_is_ap_mode, false);
    atomic_store(&s_wifi_state, WIFI_CONN_STATE_CONNECTING);

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1U);
    strncpy((char *)sta_cfg.sta.password, cfg.wifi_pass, sizeof(sta_cfg.sta.password) - 1U);
    sta_cfg.sta.threshold.authmode = (cfg.wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK
                                                                : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
    (void)web_config_httpd_start();

    net_mode_t disp_mode = atomic_load(&s_cloud_en) ? NET_MODE_ONLINE : NET_MODE_WEB_CFG;
    net_mgr_notify_mode(disp_mode, cfg.wifi_ssid);
}

void net_mode_ctrl_init(bool force_ap_setup)
{
    load_switch_nvs();
    s_btn_tracking      = false;
    s_btn_long_handled  = false;
    s_wait_boot_release = force_ap_setup;

    if (force_ap_setup) {
        net_mode_ctrl_force_ap_setup();
    } else {
        s_force_ap_setup = false;
        atomic_store(&s_wifi_dirty, true);
    }
}

void net_mode_ctrl_force_ap_setup(void)
{
    bool prev_wifi = atomic_exchange(&s_wifi_en, true);
    s_force_ap_setup = true;
    atomic_store(&s_is_ap_mode, true);
    atomic_store(&s_wifi_state, WIFI_CONN_STATE_AP_SETUP);
    if (!prev_wifi) {
        mark_nvs_dirty();
    }
    atomic_store(&s_wifi_dirty, true);
}

void net_mode_ctrl_process_button(bool is_pressed, int64_t now_ms)
{
    if (s_wait_boot_release) {
        if (!is_pressed) {
            s_wait_boot_release = false;
        }
        return;
    }

    if (is_pressed) {
        if (!s_btn_tracking) {
            s_btn_tracking     = true;
            s_btn_long_handled = false;
            s_btn_press_ms     = now_ms;
        } else if (!s_btn_long_handled &&
                   (now_ms - s_btn_press_ms) >= BTN_LONG_PRESS_MS) {
            s_btn_long_handled = true;
            ESP_LOGI(TAG, "長按滿 1.5s，立即強制啟動 SoftAP 熱點配網");
            net_mode_ctrl_force_ap_setup();
        }
    } else if (s_btn_tracking) {
        int64_t held_ms = now_ms - s_btn_press_ms;
        bool was_long   = s_btn_long_handled;
        s_btn_tracking     = false;
        s_btn_long_handled = false;

        if (!was_long && held_ms >= BTN_SHORT_DEBOUNCE_MS) {
            bool next_wifi = !atomic_load(&s_wifi_en);
            ESP_LOGI(TAG, "短按切換 Wi-Fi -> %s", next_wifi ? "開啟" : "關閉");
            (void)net_manager_set_wifi_enabled(next_wifi);
        }
    }
}

void net_mode_ctrl_poll(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_delayed_wifi_pending &&
        (int32_t)(now - s_delayed_wifi_deadline) >= 0) {
        s_delayed_wifi_pending = false;
        (void)net_manager_set_wifi_enabled(s_delayed_wifi_target);
    }

    if (atomic_exchange(&s_wifi_dirty, false)) {
        apply_wifi_transition();
    }

    if (atomic_load(&s_nvs_dirty) &&
        (now - s_nvs_dirty_tick) >= pdMS_TO_TICKS(NVS_COMMIT_DEBOUNCE_MS) &&
        !net_mgr_is_audio_busy()) {
        atomic_store(&s_nvs_dirty, false);
        flush_switch_nvs();
    }
}

void net_mode_ctrl_on_sta_start(void)
{
    if (atomic_load(&s_wifi_en) && !atomic_load(&s_is_ap_mode)) {
        (void)esp_wifi_connect();
    }
}

void net_mode_ctrl_on_sta_disconnected(void)
{
    net_mgr_set_sta_connected(false, NULL);
    s_last_ip[0] = '\0';
    sync_engine_lock_live();

    if (!atomic_load(&s_wifi_en) || atomic_load(&s_is_ap_mode)) {
        return;
    }

    atomic_store(&s_wifi_state, WIFI_CONN_STATE_CONNECTING);
    if (s_sta_retry_cnt < STA_MAX_RETRY_COUNT) {
        s_sta_retry_cnt++;
        (void)esp_wifi_connect();
    } else {
        ESP_LOGW(TAG, "STA 連續斷線達 %u 次，轉入 SoftAP 備援配網", STA_MAX_RETRY_COUNT);
        enter_softap_mode();
    }
}

void net_mode_ctrl_on_got_ip(const char *ip_str)
{
    s_sta_retry_cnt = 0;
    atomic_store(&s_is_ap_mode, false);
    atomic_store(&s_wifi_state, WIFI_CONN_STATE_CONNECTED);

    if (ip_str != NULL) {
        strncpy(s_last_ip, ip_str, sizeof(s_last_ip) - 1U);
        s_last_ip[sizeof(s_last_ip) - 1U] = '\0';
    }
    net_mgr_set_sta_connected(true, s_last_ip);
    (void)web_config_httpd_start();
    start_or_update_mdns();

    bool cloud_on = atomic_load(&s_cloud_en);
    char disp[32];
    net_manager_format_ip_display(s_last_ip, cloud_on, disp, sizeof(disp));
    net_mgr_notify_mode(cloud_on ? NET_MODE_ONLINE : NET_MODE_WEB_CFG, disp);

    if (cloud_on) {
        net_mgr_notify_got_ip();
        net_mgr_trigger_history_sync();
    }
}

void net_mode_ctrl_on_config_saved(const sys_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    if (cfg->wifi_ssid[0] != '\0') {
        s_force_ap_setup = false;
        atomic_store(&s_wifi_en, true);
        atomic_store(&s_wifi_dirty, true);
        mark_nvs_dirty();
    }
    if (cfg->server_url[0] != '\0') {
        net_manager_set_cloud_enabled(true);
    }
}

void net_mode_ctrl_set_wifi_delayed(bool enabled, uint32_t delay_ms)
{
    s_delayed_wifi_target   = enabled;
    s_delayed_wifi_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    s_delayed_wifi_pending  = true;
}

bool net_mode_ctrl_is_ap_mode(void)
{
    return atomic_load(&s_is_ap_mode);
}

bool net_mode_ctrl_is_cloud_active(void)
{
    return atomic_load(&s_wifi_en) &&
           atomic_load(&s_cloud_en) &&
           (atomic_load(&s_wifi_state) == WIFI_CONN_STATE_CONNECTED);
}

esp_err_t net_manager_set_wifi_enabled(bool enabled)
{
    bool prev = atomic_exchange(&s_wifi_en, enabled);
    if (!enabled) {
        s_force_ap_setup = false;
        atomic_store(&s_is_ap_mode, false);
        atomic_store(&s_wifi_state, WIFI_CONN_STATE_OFF);
    } else if (!prev || atomic_load(&s_is_ap_mode) ||
               atomic_load(&s_wifi_state) == WIFI_CONN_STATE_OFF) {
        s_force_ap_setup = false;
        atomic_store(&s_is_ap_mode, false);
        atomic_store(&s_wifi_state, WIFI_CONN_STATE_CONNECTING);
    }
    if (prev != enabled) {
        mark_nvs_dirty();
    }
    atomic_store(&s_wifi_dirty, true);
    return ESP_OK;
}

bool net_manager_is_wifi_enabled(void)
{
    return atomic_load(&s_wifi_en);
}

wifi_conn_state_t net_manager_get_wifi_state(void)
{
    return (wifi_conn_state_t)atomic_load(&s_wifi_state);
}

void net_manager_set_cloud_enabled(bool enabled)
{
    bool prev = atomic_exchange(&s_cloud_en, enabled);
    if (!enabled) {
        net_manager_abort_voice_stream();
        sync_engine_lock_live();
    }
    if (prev != enabled) {
        mark_nvs_dirty();
        if (atomic_load(&s_wifi_en) &&
            atomic_load(&s_wifi_state) == WIFI_CONN_STATE_CONNECTED) {
            char disp[32];
            net_manager_format_ip_display(s_last_ip, enabled, disp, sizeof(disp));
            net_mgr_notify_mode(enabled ? NET_MODE_ONLINE : NET_MODE_WEB_CFG, disp);
            if (enabled) {
                net_mgr_notify_got_ip();
                net_mgr_trigger_history_sync();
            }
        }
    }
}

bool net_manager_is_cloud_enabled(void)
{
    return atomic_load(&s_cloud_en);
}

void net_manager_format_ip_display(const char *ip_str, bool cloud_en,
                                   char *out_buf, size_t out_len)
{
    if (out_buf == NULL || out_len == 0U) {
        return;
    }
    const char *ip = (ip_str != NULL && ip_str[0] != '\0') ? ip_str : "0.0.0.0";
    if (cloud_en) {
        snprintf(out_buf, out_len, "%s", ip);
    } else {
        snprintf(out_buf, out_len, "%s [LAN]", ip);
    }
}

esp_err_t net_mode_ctrl_set_legacy_mode(net_mode_t mode)
{
    if (mode > NET_MODE_WEB_CFG) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode == NET_MODE_OFFLINE) {
        return net_manager_set_wifi_enabled(false);
    }
    if (mode == NET_MODE_ONLINE) {
        net_manager_set_cloud_enabled(true);
        return net_manager_set_wifi_enabled(true);
    }
    net_mode_ctrl_force_ap_setup();
    return ESP_OK;
}

net_mode_t net_mode_ctrl_get_legacy_mode(void)
{
    if (!atomic_load(&s_wifi_en) ||
        atomic_load(&s_wifi_state) == WIFI_CONN_STATE_OFF) {
        return NET_MODE_OFFLINE;
    }
    if (atomic_load(&s_is_ap_mode) || !atomic_load(&s_cloud_en)) {
        return NET_MODE_WEB_CFG;
    }
    return NET_MODE_ONLINE;
}
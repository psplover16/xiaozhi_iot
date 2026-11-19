#include "net_manager_priv.h"
#include "board_bsp.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "net_manager";

#define NVS_NAMESPACE_CFG       "xiaozhi_cfg"
#define NVS_KEY_SYS_CFG         "sys_cfg"
#define STA_MAX_RETRY_COUNT     3U
#define STA_PROBE_INTERVAL_MS   30000U
#define SYNC_RETRY_INTERVAL_MS  10000U
#define BTN_DEBOUNCE_MS         50U
#define BTN_POLL_INTERVAL_MS    20U
#define LIVE_QUEUE_LEN          32U
#define LIVE_UPLOAD_BATCH_MAX   16U

static SemaphoreHandle_t    s_net_mutex         = NULL;
static QueueHandle_t        s_live_rec_queue    = NULL;
static TaskHandle_t         s_net_task          = NULL;
static net_manager_hooks_t  s_hooks             = {0};

static sys_config_t         s_cfg;
static volatile net_mode_t  s_target_mode       = NET_MODE_OFFLINE;
static volatile net_mode_t  s_active_mode       = NET_MODE_OFFLINE;
static volatile bool        s_mode_dirty        = false;
static volatile bool        s_sta_degraded      = false;
static volatile bool        s_sta_has_ip        = false;
static volatile bool        s_need_got_ip_cb    = false;
static volatile bool        s_need_history_sync = false;
static volatile uint8_t     s_sta_retry_cnt     = 0;
static TickType_t           s_last_probe_tick   = 0;
static TickType_t           s_last_sync_tick    = 0;
static bool                 s_initialized       = false;

static void load_default_config(sys_config_t *cfg)
{
    memset(cfg, 0, sizeof(sys_config_t));
    cfg->magic               = SYS_CONFIG_MAGIC;
    cfg->sample_interval_sec = 30U;
    cfg->psram_window_hours  = 1U;
    cfg->eeprom_warn_pct     = 80U;
    cfg->sd_warn_free_mb     = 100U;
    cfg->crc32 = board_calc_crc32((const uint8_t *)cfg, offsetof(sys_config_t, crc32));
}

esp_err_t net_mgr_save_config(const sys_config_t *new_cfg)
{
    if (new_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sys_config_t tmp;
    memcpy(&tmp, new_cfg, sizeof(sys_config_t));
    tmp.magic = SYS_CONFIG_MAGIC;
    tmp.crc32 = board_calc_crc32((const uint8_t *)&tmp, offsetof(sys_config_t, crc32));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY_SYS_CFG, &tmp, sizeof(sys_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK && s_net_mutex != NULL) {
        xSemaphoreTake(s_net_mutex, portMAX_DELAY);
        memcpy(&s_cfg, &tmp, sizeof(sys_config_t));
        xSemaphoreGive(s_net_mutex);
    }
    return err;
}

static void load_config_from_nvs(void)
{
    sys_config_t loaded;
    size_t len = sizeof(sys_config_t);
    bool valid = false;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, NVS_KEY_SYS_CFG, &loaded, &len) == ESP_OK &&
            len == sizeof(sys_config_t) &&
            loaded.magic == SYS_CONFIG_MAGIC &&
            board_calc_crc32((const uint8_t *)&loaded,
                             offsetof(sys_config_t, crc32)) == loaded.crc32) {
            valid = true;
        }
        nvs_close(nvs);
    }

    if (valid) {
        memcpy(&s_cfg, &loaded, sizeof(sys_config_t));
    } else {
        load_default_config(&s_cfg);
        (void)net_mgr_save_config(&s_cfg);
    }
}

void net_mgr_get_config_copy(sys_config_t *out_cfg)
{
    if (out_cfg == NULL) {
        return;
    }
    if (s_net_mutex != NULL) {
        xSemaphoreTake(s_net_mutex, portMAX_DELAY);
        memcpy(out_cfg, &s_cfg, sizeof(sys_config_t));
        xSemaphoreGive(s_net_mutex);
    } else {
        memcpy(out_cfg, &s_cfg, sizeof(sys_config_t));
    }
}

bool net_mgr_is_sta_connected(void)
{
    return s_sta_has_ip;
}

void net_mgr_query_storage_status(uint16_t *out_eeprom_cnt,
                                  bool *out_sd_mounted,
                                  uint32_t *out_sd_free_mb)
{
    if (s_hooks.get_storage_status != NULL) {
        s_hooks.get_storage_status(out_eeprom_cnt, out_sd_mounted,
                                   out_sd_free_mb, s_hooks.user_ctx);
    } else {
        *out_eeprom_cnt = 0;
        *out_sd_mounted = false;
        *out_sd_free_mb = 0;
    }
}

void net_mgr_notify_mode(net_mode_t mode, const char *info)
{
    if (s_hooks.on_mode_changed != NULL) {
        s_hooks.on_mode_changed(mode, info, s_hooks.user_ctx);
    }
}

void net_mgr_notify_play_file(const char *filepath)
{
    if (s_hooks.on_play_audio_file != NULL && filepath != NULL) {
        (void)s_hooks.on_play_audio_file(filepath, s_hooks.user_ctx);
    }
}

void net_mgr_notify_tts_ready(const uint8_t *data, size_t len,
                              const char *content_type)
{
    if (s_hooks.on_tts_audio_ready != NULL && data != NULL && len > 0U) {
        (void)s_hooks.on_tts_audio_ready(data, len, content_type, s_hooks.user_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_has_ip = false;
        sync_engine_lock_live();
        if (s_target_mode == NET_MODE_ONLINE && !s_sta_degraded) {
            if (s_sta_retry_cnt < STA_MAX_RETRY_COUNT) {
                s_sta_retry_cnt++;
                (void)esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "STA 連續失敗超過 %u 次，自動降級為離線模式",
                         STA_MAX_RETRY_COUNT);
                s_sta_degraded    = true;
                s_active_mode     = NET_MODE_OFFLINE;
                s_last_probe_tick = xTaskGetTickCount();
                (void)esp_wifi_stop();
                net_mgr_notify_mode(NET_MODE_OFFLINE, "DEGRADED");
            }
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        char ip_str[20];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "STA 取得 IP: %s", ip_str);

        s_sta_retry_cnt = 0;
        s_sta_degraded  = false;
        s_sta_has_ip    = true;
        if (s_target_mode == NET_MODE_ONLINE) {
            s_active_mode       = NET_MODE_ONLINE;
            s_need_got_ip_cb    = true;
            s_need_history_sync = true;
            sync_engine_lock_live();
            net_mgr_notify_mode(NET_MODE_ONLINE, ip_str);
        }
    }
}

static void apply_mode_transition(net_mode_t mode)
{
    web_config_stop();
    (void)esp_wifi_stop();
    s_sta_has_ip    = false;
    s_sta_degraded  = false;
    s_sta_retry_cnt = 0;
    sync_engine_lock_live();

    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    if (mode == NET_MODE_OFFLINE) {
        (void)esp_wifi_set_mode(WIFI_MODE_NULL);
        s_active_mode = NET_MODE_OFFLINE;
        net_mgr_notify_mode(NET_MODE_OFFLINE, "OFFLINE");
        return;
    }

    if (mode == NET_MODE_ONLINE) {
        if (cfg.wifi_ssid[0] == '\0') {
            s_sta_degraded    = true;
            s_active_mode     = NET_MODE_OFFLINE;
            s_last_probe_tick = xTaskGetTickCount();
            net_mgr_notify_mode(NET_MODE_OFFLINE, "NO_SSID");
            return;
        }
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1U);
        strncpy((char *)sta_cfg.sta.password, cfg.wifi_pass, sizeof(sta_cfg.sta.password) - 1U);
        sta_cfg.sta.threshold.authmode = (cfg.wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK
                                                                    : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
        s_active_mode = NET_MODE_ONLINE;
        net_mgr_notify_mode(NET_MODE_ONLINE, cfg.wifi_ssid);
        return;
    }

    if (mode == NET_MODE_WEB_CFG) {
        s_active_mode = (web_config_start(&cfg) == ESP_OK) ? NET_MODE_WEB_CFG
                                                           : NET_MODE_OFFLINE;
    }
}

static void process_record_pipeline(void)
{
    if (s_live_rec_queue == NULL) {
        return;
    }

    sensor_record_t batch[LIVE_UPLOAD_BATCH_MAX];
    size_t count = 0;
    while (count < LIVE_UPLOAD_BATCH_MAX &&
           xQueueReceive(s_live_rec_queue, &batch[count], 0) == pdTRUE) {
        count++;
    }
    if (count == 0) {
        return;
    }

    bool can_live = (s_active_mode == NET_MODE_ONLINE) &&
                    s_sta_has_ip && sync_engine_is_live_unlocked();
    if (can_live && net_mgr_http_upload_batch(batch, count) == ESP_OK) {
        return;
    }
    if (s_hooks.on_offline_record != NULL) {
        for (size_t i = 0; i < count; i++) {
            (void)s_hooks.on_offline_record(&batch[i], s_hooks.user_ctx);
        }
    }
}

static void task_net_manager(void *arg)
{
    (void)arg;
    uint32_t pressed_ms = 0;
    bool btn_handled    = false;

    while (1) {
        if (s_mode_dirty) {
            s_mode_dirty = false;
            apply_mode_transition(s_target_mode);
        }

        if (s_need_got_ip_cb) {
            s_need_got_ip_cb = false;
            if (s_hooks.on_got_ip != NULL) {
                s_hooks.on_got_ip(s_hooks.user_ctx);
            }
        }

        if (s_active_mode == NET_MODE_ONLINE && s_sta_has_ip &&
            !sync_engine_is_live_unlocked()) {
            TickType_t now = xTaskGetTickCount();
            if (s_need_history_sync ||
                (now - s_last_sync_tick) >= pdMS_TO_TICKS(SYNC_RETRY_INTERVAL_MS)) {
                s_need_history_sync = false;
                s_last_sync_tick    = now;
                (void)sync_engine_execute(net_mgr_http_upload_batch);
            }
        }

        process_record_pipeline();

        if (s_target_mode == NET_MODE_ONLINE && s_sta_degraded) {
            TickType_t now = xTaskGetTickCount();
            if ((now - s_last_probe_tick) >= pdMS_TO_TICKS(STA_PROBE_INTERVAL_MS)) {
                s_last_probe_tick = now;
                s_sta_degraded    = false;
                s_sta_retry_cnt   = 0;
                (void)esp_wifi_start();
            }
        }

        if (board_btn_is_pressed()) {
            pressed_ms += BTN_POLL_INTERVAL_MS;
            if (pressed_ms >= BTN_DEBOUNCE_MS && !btn_handled) {
                btn_handled = true;
                (void)net_manager_set_mode((net_mode_t)((s_target_mode + 1) % 3));
            }
        } else {
            pressed_ms  = 0;
            btn_handled = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS));
    }
}

esp_err_t net_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_net_mutex      = xSemaphoreCreateMutex();
    s_live_rec_queue = xQueueCreate(LIVE_QUEUE_LEN, sizeof(sensor_record_t));
    if (s_net_mutex == NULL || s_live_rec_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (esp_netif_create_default_wifi_sta() == NULL ||
        esp_netif_create_default_wifi_ap() == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    load_config_from_nvs();
    s_target_mode = (board_btn_is_pressed() || s_cfg.wifi_ssid[0] == '\0')
                        ? NET_MODE_WEB_CFG
                        : NET_MODE_ONLINE;
    s_mode_dirty = true;

    err = voice_cloud_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreatePinnedToCore(task_net_manager, "net_sync_task",
                                8192, NULL, 3, &s_net_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

void net_manager_register_hooks(const net_manager_hooks_t *hooks)
{
    if (hooks != NULL) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
}

esp_err_t net_manager_set_mode(net_mode_t mode)
{
    if (mode > NET_MODE_WEB_CFG) {
        return ESP_ERR_INVALID_ARG;
    }
    s_target_mode = mode;
    s_mode_dirty  = true;
    return ESP_OK;
}

net_mode_t net_manager_get_mode(void)
{
    return s_active_mode;
}

esp_err_t net_manager_submit_record(const sensor_record_t *rec)
{
    if (rec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_live_rec_queue != NULL &&
        xQueueSend(s_live_rec_queue, rec, 0) == pdTRUE) {
        return ESP_OK;
    }
    if (s_hooks.on_offline_record != NULL) {
        return s_hooks.on_offline_record(rec, s_hooks.user_ctx);
    }
    return ESP_ERR_NO_MEM;
}
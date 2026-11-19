#include "net_manager_priv.h"

#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "sync_engine";

static sync_source_t s_sources[SYNC_MAX_SOURCES];
static size_t        s_source_count  = 0;
static volatile bool s_live_unlocked = false;

void sync_engine_clear_sources(void)
{
    memset(s_sources, 0, sizeof(s_sources));
    s_source_count = 0;
}

esp_err_t sync_engine_register_source(const sync_source_t *source)
{
    if (source == NULL ||
        source->peek_oldest_batch == NULL ||
        source->ack_oldest_batch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_source_count >= SYNC_MAX_SOURCES) {
        return ESP_ERR_NO_MEM;
    }
    s_sources[s_source_count++] = *source;
    return ESP_OK;
}

void sync_engine_lock_live(void)
{
    s_live_unlocked = false;
}

bool sync_engine_is_live_unlocked(void)
{
    return s_live_unlocked;
}

static bool is_batch_chronological(const sensor_record_t *batch, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        if (batch[i].timestamp < batch[i - 1].timestamp) {
            return false;
        }
    }
    return true;
}

esp_err_t sync_engine_execute(sync_upload_batch_fn upload_fn)
{
    if (upload_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_live_unlocked = false;
    sensor_record_t batch[SYNC_BATCH_MAX_RECORDS];

    for (size_t idx = 0; idx < s_source_count; idx++) {
        const sync_source_t *src = &s_sources[idx];
        const char *src_name = (src->name != NULL) ? src->name : "unknown";

        while (1) {
            size_t count = 0;
            esp_err_t err = src->peek_oldest_batch(batch, SYNC_BATCH_MAX_RECORDS,
                                                   &count, src->ctx);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "讀取離線來源 [%s] 失敗: %s",
                         src_name, esp_err_to_name(err));
                return err;
            }
            if (count == 0) {
                break; /* 該優先級來源已 100% 排空，進入下一個較新來源 */
            }
            if (count > SYNC_BATCH_MAX_RECORDS) {
                count = SYNC_BATCH_MAX_RECORDS;
            }
            if (!is_batch_chronological(batch, count)) {
                ESP_LOGW(TAG, "來源 [%s] 批次內時間戳非單調遞增", src_name);
            }

            err = upload_fn(batch, count);
            if (err != ESP_OK) {
                /* 斷線保護：未收到 Server ACK 嚴禁刪除或淨空任何離線資料 */
                ESP_LOGW(TAG, "來源 [%s] 上傳未獲 ACK (%s)，保留資料並中止補傳",
                         src_name, esp_err_to_name(err));
                return err;
            }

            err = src->ack_oldest_batch(count, src->ctx);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "來源 [%s] 確認移除已傳批次失敗: %s",
                         src_name, esp_err_to_name(err));
                return err;
            }

            ESP_LOGI(TAG, "來源 [%s] 已補傳並確認 %u 筆紀錄",
                     src_name, (unsigned)count);
        }
    }

    s_live_unlocked = true;
    ESP_LOGI(TAG, "全部離線歷史紀錄已依序補傳完成，解除即時上傳鎖定");
    return ESP_OK;
}

esp_err_t net_mgr_http_upload_batch(const sensor_record_t *batch, size_t count)
{
    if (batch == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    bool is_http = (strncmp(cfg.server_url, "http://", 7) == 0) ||
                   (strncmp(cfg.server_url, "https://", 8) == 0);
    if (!is_http) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t http_cfg = {
        .url        = cfg.server_url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_post_field(client,
                                   (const char *)batch,
                                   (int)(count * sizeof(sensor_record_t)));

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
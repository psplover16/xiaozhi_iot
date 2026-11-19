#include "net_manager_priv.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "voice_cloud";

#define SPEECH_QUEUE_LEN        4U
#define VOICE_CLOUD_TASK_STACK  8192U
#define VOICE_CLOUD_TASK_PRIO   4U
#define VOICE_HTTP_TIMEOUT_MS   15000
#define VOICE_TTS_MAX_BYTES     (256U * 1024U)
#define VOICE_UPLOAD_CHUNK_SIZE 4096U

typedef struct {
    const int16_t *pcm_data;
    size_t         pcm_bytes;
    uint32_t       sample_rate_hz;
} speech_upload_req_t;

static QueueHandle_t s_speech_queue = NULL;
static TaskHandle_t  s_voice_task   = NULL;

esp_err_t voice_cloud_build_endpoint(const char *base_url, const char *subpath,
                                     char *out_url, size_t out_len)
{
    if (base_url == NULL || subpath == NULL || out_url == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    bool valid_scheme = (strncmp(base_url, "http://", 7) == 0) ||
                        (strncmp(base_url, "https://", 8) == 0);
    if (!valid_scheme) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t blen = strlen(base_url);
    while (blen > 0U && base_url[blen - 1U] == '/') {
        blen--;
    }

    const char *path = (subpath[0] == '/') ? subpath : (subpath + 0);
    int written = (subpath[0] == '/')
                      ? snprintf(out_url, out_len, "%.*s%s", (int)blen, base_url, path)
                      : snprintf(out_url, out_len, "%.*s/%s", (int)blen, base_url, path);

    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t stream_speech_and_receive_tts(const speech_upload_req_t *req)
{
    sys_config_t cfg;
    net_mgr_get_config_copy(&cfg);

    char endpoint[160];
    esp_err_t err = voice_cloud_build_endpoint(cfg.server_url, "/voice/chat",
                                               endpoint, sizeof(endpoint));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "未設定有效 HTTP(S) Server URL，無法上傳語音");
        return err;
    }

    uint8_t *tts_buf = (uint8_t *)heap_caps_malloc(VOICE_TTS_MAX_BYTES,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tts_buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 TTS 接收緩衝區失敗");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url        = endpoint,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = VOICE_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        heap_caps_free(tts_buf);
        return ESP_ERR_NO_MEM;
    }

    char rate_hdr[16];
    snprintf(rate_hdr, sizeof(rate_hdr), "%lu", (unsigned long)req->sample_rate_hz);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", rate_hdr);
    esp_http_client_set_header(client, "X-Channels", "2");
    esp_http_client_set_header(client, "X-Bits-Per-Sample", "16");

    err = esp_http_client_open(client, (int)req->pcm_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "開啟雲端語音連線失敗: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(tts_buf);
        return err;
    }

    const uint8_t *src = (const uint8_t *)req->pcm_data;
    size_t sent_total = 0;
    while (sent_total < req->pcm_bytes) {
        size_t chunk = req->pcm_bytes - sent_total;
        if (chunk > VOICE_UPLOAD_CHUNK_SIZE) {
            chunk = VOICE_UPLOAD_CHUNK_SIZE;
        }
        int w = esp_http_client_write(client, (const char *)(src + sent_total), (int)chunk);
        if (w <= 0) {
            err = ESP_FAIL;
            break;
        }
        sent_total += (size_t)w;
    }

    int status = 0;
    size_t rx_total = 0;
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            while (rx_total < VOICE_TTS_MAX_BYTES) {
                int r = esp_http_client_read(client,
                                             (char *)(tts_buf + rx_total),
                                             (int)(VOICE_TTS_MAX_BYTES - rx_total));
                if (r < 0) {
                    err = ESP_FAIL;
                    break;
                }
                if (r == 0) {
                    break;
                }
                rx_total += (size_t)r;
            }
        } else {
            ESP_LOGW(TAG, "雲端語音回應異常 HTTP %d", status);
            err = ESP_FAIL;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && rx_total > 0U) {
        ESP_LOGI(TAG, "已接收雲端 TTS 回覆音訊 (%u Bytes)，交由播報 Hook",
                 (unsigned)rx_total);
        net_mgr_notify_tts_ready(tts_buf, rx_total, "audio/mpeg");
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    heap_caps_free(tts_buf);
    return err;
}

static void task_voice_cloud(void *arg)
{
    (void)arg;
    speech_upload_req_t req;

    while (1) {
        if (xQueueReceive(s_speech_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bool online_ready = (net_manager_get_mode() == NET_MODE_ONLINE) &&
                            net_mgr_is_sta_connected();

        if (!online_ready) {
            ESP_LOGI(TAG, "目前為離線狀態，觸發本機離線提示音播報");
            net_mgr_notify_play_file(VOICE_OFFLINE_HINT_DEFAULT);
            continue;
        }

        if (stream_speech_and_receive_tts(&req) != ESP_OK) {
            ESP_LOGW(TAG, "雲端語音對話失敗，降級播放離線提示音");
            net_mgr_notify_play_file(VOICE_OFFLINE_HINT_DEFAULT);
        }
    }
}

esp_err_t voice_cloud_init(void)
{
    if (s_speech_queue == NULL) {
        s_speech_queue = xQueueCreate(SPEECH_QUEUE_LEN, sizeof(speech_upload_req_t));
        if (s_speech_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_voice_task == NULL) {
        if (xTaskCreatePinnedToCore(task_voice_cloud, "voice_cloud_task",
                                    VOICE_CLOUD_TASK_STACK, NULL,
                                    VOICE_CLOUD_TASK_PRIO, &s_voice_task, 0) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t net_manager_submit_speech(const int16_t *pcm_data, size_t pcm_bytes,
                                    uint32_t sample_rate_hz)
{
    if (pcm_data == NULL || pcm_bytes == 0U || sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_speech_queue == NULL) {
        s_speech_queue = xQueueCreate(SPEECH_QUEUE_LEN, sizeof(speech_upload_req_t));
        if (s_speech_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    speech_upload_req_t req = {
        .pcm_data       = pcm_data,
        .pcm_bytes      = pcm_bytes,
        .sample_rate_hz = sample_rate_hz,
    };

    if (xQueueSend(s_speech_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "語音上傳佇列已滿，丟棄本次語音請求");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
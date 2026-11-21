#include "net_manager_priv.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "voice_cloud";

#define SPEECH_QUEUE_LEN        4U
#define VOICE_CLOUD_TASK_STACK  8192U
#define VOICE_CLOUD_TASK_PRIO   4U
#define VOICE_HTTP_TIMEOUT_MS   VOICE_CLOUD_TIMEOUT_MS
#define VOICE_TTS_MAX_BYTES     (256U * 1024U)
#define VOICE_UPLOAD_CHUNK_SIZE 2048U
#define VOICE_READ_CHUNK_SIZE   2048U

typedef struct {
    const int16_t *pcm_data;
    size_t         pcm_bytes;
    uint32_t       sample_rate_hz;
} speech_upload_req_t;

static QueueHandle_t       s_speech_queue = NULL;
static TaskHandle_t        s_voice_task   = NULL;
static atomic_bool         s_abort_flag   = false;
static atomic_int_fast64_t s_abort_req_us = 0;

static inline bool should_abort_stream(void)
{
    return atomic_load(&s_abort_flag) || !net_manager_is_cloud_enabled();
}

void net_manager_abort_voice_stream(void)
{
    atomic_store(&s_abort_req_us, esp_timer_get_time());
    atomic_store(&s_abort_flag, true);
}

bool net_manager_is_voice_stream_aborted(void)
{
    return atomic_load(&s_abort_flag);
}

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

    int written = (subpath[0] == '/')
                      ? snprintf(out_url, out_len, "%.*s%s", (int)blen, base_url, subpath)
                      : snprintf(out_url, out_len, "%.*s/%s", (int)blen, base_url, subpath);

    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t voice_cloud_format_stream_headers(uint32_t sample_rate_hz,
                                            char *out_rate, size_t rate_len,
                                            char *out_channels, size_t ch_len,
                                            char *out_bits, size_t bits_len)
{
    if (sample_rate_hz == 0U || out_rate == NULL || out_channels == NULL || out_bits == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rate_len < 8U || ch_len < 4U || bits_len < 4U) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(out_rate, rate_len, "%lu", (unsigned long)sample_rate_hz);
    snprintf(out_channels, ch_len, "%u", (unsigned)VOICE_CLOUD_CHANNELS);
    snprintf(out_bits, bits_len, "16");
    return ESP_OK;
}

static esp_err_t stream_speech_and_receive_tts(const speech_upload_req_t *req)
{
    if (should_abort_stream()) {
        atomic_store(&s_abort_flag, false);
        return ESP_ERR_INVALID_STATE;
    }

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
    char ch_hdr[8];
    char bits_hdr[8];
    (void)voice_cloud_format_stream_headers(req->sample_rate_hz,
                                            rate_hdr, sizeof(rate_hdr),
                                            ch_hdr, sizeof(ch_hdr),
                                            bits_hdr, sizeof(bits_hdr));

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", rate_hdr);
    esp_http_client_set_header(client, "X-Channels", ch_hdr);
    esp_http_client_set_header(client, "X-Bits-Per-Sample", bits_hdr);

    err = esp_http_client_open(client, (int)req->pcm_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "開啟雲端語音連線失敗: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(tts_buf);
        return err;
    }

    bool aborted = false;
    const uint8_t *src = (const uint8_t *)req->pcm_data;
    size_t sent_total = 0U;

    while (sent_total < req->pcm_bytes) {
        if (should_abort_stream()) {
            aborted = true;
            break;
        }
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

    size_t rx_total = 0U;
    if (err == ESP_OK && !aborted) {
        if (should_abort_stream()) {
            aborted = true;
        } else {
            (void)esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status >= 200 && status < 300) {
                while (rx_total < VOICE_TTS_MAX_BYTES) {
                    if (should_abort_stream()) {
                        aborted = true;
                        break;
                    }
                    size_t to_read = VOICE_TTS_MAX_BYTES - rx_total;
                    if (to_read > VOICE_READ_CHUNK_SIZE) {
                        to_read = VOICE_READ_CHUNK_SIZE;
                    }
                    int r = esp_http_client_read(client,
                                                 (char *)(tts_buf + rx_total),
                                                 (int)to_read);
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
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (aborted || should_abort_stream()) {
        int64_t req_us = atomic_load(&s_abort_req_us);
        int64_t elapsed_ms = (req_us > 0) ? ((esp_timer_get_time() - req_us) / 1000LL) : 0LL;
        ESP_LOGI(TAG, "雲端語音串流已即時中斷 (耗時 %lld ms)", (long long)elapsed_ms);
        atomic_store(&s_abort_flag, false);
        heap_caps_free(tts_buf);
        return ESP_ERR_INVALID_STATE;
    }

    if (err == ESP_OK && rx_total > 0U) {
        ESP_LOGI(TAG, "已接收雲端 1-Ch TTS 回覆 (%u Bytes)，交由播報 Hook",
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

        if (atomic_load(&s_abort_flag)) {
            atomic_store(&s_abort_flag, false);
            continue;
        }

        if (!net_mode_ctrl_is_cloud_active()) {
            ESP_LOGI(TAG, "目前為離線或雲端關閉狀態，觸發本機離線提示音播報");
            net_mgr_notify_play_file(VOICE_OFFLINE_HINT_DEFAULT);
            continue;
        }

        esp_err_t err = stream_speech_and_receive_tts(&req);
        if (err == ESP_ERR_INVALID_STATE) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "雲端語音對話失敗 (%s)，降級播放離線提示音",
                     esp_err_to_name(err));
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

    atomic_store(&s_abort_flag, false);

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
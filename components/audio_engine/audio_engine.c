#include "audio_engine.h"
#include "audio_engine_priv.h"
#include "aec_ref.h"
#include "board_bsp.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "audio_engine";

/* 腳位相容定義：優先採用 board_config.h 巨集 */
#ifndef BOARD_PIN_I2S_MIC_BCLK
#define BOARD_PIN_I2S_MIC_BCLK  4
#endif
#ifndef BOARD_PIN_I2S_MIC_WS
#define BOARD_PIN_I2S_MIC_WS    5
#endif
#ifndef BOARD_PIN_I2S_MIC_DIN
#define BOARD_PIN_I2S_MIC_DIN   6
#endif

/* 錄音與 VAD 參數設定（16 kHz 16-bit 單聲道） */
#define AUDIO_RX_SAMPLE_RATE    16000U
#define AUDIO_CHUNK_FRAMES      512U
#define AUDIO_PSRAM_BUF_BYTES   (320U * 1024U)
#define AUDIO_PREROLL_SAMPLES   (6U * 512U) /* 192 ms @ 16 kHz 單聲道 */

#define VAD_START_HOLD_MS       150U
#define VAD_END_SILENCE_MS      800U
#define VAD_MIN_NOISE_FLOOR     60.0f
#define VAD_SNR_MULTIPLIER      2.4f
#define VAD_MIN_MARGIN_RMS      140.0f

typedef struct {
    i2s_chan_handle_t       rx_chan;
    TaskHandle_t            io_task;
    QueueHandle_t           cmd_queue;
    SemaphoreHandle_t       buf_mutex;
    SemaphoreHandle_t       vad_mutex;

    voice_cmd_callback_t    cmd_cb;
    audio_event_callback_t  event_cb;
    audio_pcm_tap_cb_t      pcm_tap_cb;

    int16_t                *psram_rec_buf;
    size_t                  rec_write_samples;
    size_t                  rec_max_samples;

    int16_t                 preroll_buf[AUDIO_PREROLL_SAMPLES];
    size_t                  preroll_head;
    size_t                  preroll_count;

    float                   noise_floor_rms;
    bool                    in_speech;
    uint32_t                speech_hold_ms;
    uint32_t                silence_hold_ms;
    volatile bool           afe_mono_active;
    bool                    initialized;
} audio_rx_ctx_t;

static audio_rx_ctx_t s_rx = {0};

static float calc_mono_rms(const int16_t *pcm, size_t count)
{
    if (count == 0U) return 0.0f;
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = pcm[i];
        sum_sq += (uint64_t)(s * s);
    }
    return sqrtf((float)sum_sq / (float)count);
}

static void append_psram_mono(const int16_t *mono_samples, size_t sample_count)
{
    if (!s_rx.psram_rec_buf || sample_count == 0U) return;
    size_t remain = s_rx.rec_max_samples - s_rx.rec_write_samples;
    size_t to_copy = (sample_count < remain) ? sample_count : remain;
    if (to_copy > 0U) {
        memcpy(&s_rx.psram_rec_buf[s_rx.rec_write_samples],
               mono_samples, to_copy * sizeof(int16_t));
        s_rx.rec_write_samples += to_copy;
    }
}

static void push_preroll_mono(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        size_t pos = (s_rx.preroll_head + s_rx.preroll_count) % AUDIO_PREROLL_SAMPLES;
        s_rx.preroll_buf[pos] = samples[i];
        if (s_rx.preroll_count < AUDIO_PREROLL_SAMPLES) {
            s_rx.preroll_count++;
        } else {
            s_rx.preroll_head = (s_rx.preroll_head + 1U) % AUDIO_PREROLL_SAMPLES;
        }
    }
}

static void flush_preroll_to_psram(void)
{
    if (s_rx.preroll_count == 0U) return;
    size_t first = AUDIO_PREROLL_SAMPLES - s_rx.preroll_head;
    if (first >= s_rx.preroll_count) {
        append_psram_mono(&s_rx.preroll_buf[s_rx.preroll_head], s_rx.preroll_count);
    } else {
        append_psram_mono(&s_rx.preroll_buf[s_rx.preroll_head], first);
        append_psram_mono(&s_rx.preroll_buf[0], s_rx.preroll_count - first);
    }
    s_rx.preroll_head = 0U;
    s_rx.preroll_count = 0U;
}

static void process_mono_vad(const int16_t *mono_pcm, size_t count, float override_rms)
{
    if (mono_pcm == NULL || count == 0U) return;

    float rms = (override_rms >= 0.0f) ? override_rms : calc_mono_rms(mono_pcm, count);
    uint32_t frame_ms = (uint32_t)((count * 1000U) / AUDIO_RX_SAMPLE_RATE);
    if (frame_ms == 0U) frame_ms = 1U;

    xSemaphoreTake(s_rx.vad_mutex, portMAX_DELAY);
    float dyn_threshold = s_rx.noise_floor_rms * VAD_SNR_MULTIPLIER + VAD_MIN_MARGIN_RMS;
    bool is_voice = (rms > dyn_threshold);

    if (!s_rx.in_speech) {
        if (!is_voice) {
            float alpha = (rms < s_rx.noise_floor_rms) ? 0.08f : 0.015f;
            s_rx.noise_floor_rms = (1.0f - alpha) * s_rx.noise_floor_rms + alpha * rms;
            if (s_rx.noise_floor_rms < VAD_MIN_NOISE_FLOOR) {
                s_rx.noise_floor_rms = VAD_MIN_NOISE_FLOOR;
            }
        }
        push_preroll_mono(mono_pcm, count);

        if (is_voice) {
            s_rx.speech_hold_ms += frame_ms;
            if (s_rx.speech_hold_ms >= VAD_START_HOLD_MS) {
                xSemaphoreTake(s_rx.buf_mutex, portMAX_DELAY);
                s_rx.rec_write_samples = 0U;
                flush_preroll_to_psram();
                xSemaphoreGive(s_rx.buf_mutex);

                s_rx.in_speech = true;
                s_rx.silence_hold_ms = 0U;
                if (s_rx.event_cb) s_rx.event_cb(AUDIO_EVENT_SPEECH_START, 0);
            }
        } else {
            s_rx.speech_hold_ms = 0U;
        }
    } else {
        xSemaphoreTake(s_rx.buf_mutex, portMAX_DELAY);
        append_psram_mono(mono_pcm, count);
        size_t cur_bytes = s_rx.rec_write_samples * sizeof(int16_t);
        bool buf_full = (s_rx.rec_write_samples >= s_rx.rec_max_samples);
        xSemaphoreGive(s_rx.buf_mutex);

        s_rx.silence_hold_ms = is_voice ? 0U : (s_rx.silence_hold_ms + frame_ms);
        if (s_rx.silence_hold_ms >= VAD_END_SILENCE_MS || buf_full) {
            s_rx.in_speech = false;
            s_rx.speech_hold_ms = 0U;
            s_rx.silence_hold_ms = 0U;
            if (s_rx.event_cb) s_rx.event_cb(AUDIO_EVENT_SPEECH_END, cur_bytes);
        }
    }
    xSemaphoreGive(s_rx.vad_mutex);
}

void audio_engine_push_afe_mono(const int16_t *mono_pcm, size_t sample_count)
{
    if (!s_rx.initialized || mono_pcm == NULL || sample_count == 0U) return;
    s_rx.afe_mono_active = true;
    process_mono_vad(mono_pcm, sample_count, -1.0f);
}

static void process_dual_mic_fallback(const int16_t *stereo_pcm, size_t frames)
{
    uint64_t sq_l = 0, sq_r = 0;
    int16_t mono_chunk[AUDIO_CHUNK_FRAMES];
    for (size_t i = 0; i < frames; i++) {
        int32_t l = stereo_pcm[i * 2U];
        int32_t r = stereo_pcm[i * 2U + 1U];
        sq_l += (uint64_t)(l * l);
        sq_r += (uint64_t)(r * r);
        mono_chunk[i] = (int16_t)((l + r) >> 1);
    }
    float rms_l = sqrtf((float)sq_l / (float)frames);
    float rms_r = sqrtf((float)sq_r / (float)frames);
    float max_ch = (rms_l > rms_r) ? rms_l : rms_r;
    float min_ch = (rms_l < rms_r) ? rms_l : rms_r;
    float combined = 0.5f * (rms_l + rms_r);
    if (max_ch > 1.0f && (min_ch / max_ch) < 0.25f) {
        combined *= 0.5f;
    }
    process_mono_vad(mono_chunk, frames, combined);
}

static void task_audio_io(void *arg)
{
    (void)arg;
    int32_t raw_i2s[AUDIO_CHUNK_FRAMES * 2U];
    int16_t stereo_pcm[AUDIO_CHUNK_FRAMES * 2U];
    voice_cmd_id_t pending_cmd = VOICE_CMD_NONE;

    while (1) {
        while (xQueueReceive(s_rx.cmd_queue, &pending_cmd, 0) == pdTRUE) {
            if (pending_cmd != VOICE_CMD_NONE && s_rx.cmd_cb != NULL) {
                s_rx.cmd_cb(pending_cmd);
            }
        }
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx.rx_chan, raw_i2s, sizeof(raw_i2s),
                                         &bytes_read, pdMS_TO_TICKS(50));
        if (ret == ESP_OK && bytes_read == sizeof(raw_i2s)) {
            for (size_t i = 0; i < AUDIO_CHUNK_FRAMES * 2U; i++) {
                stereo_pcm[i] = (int16_t)(raw_i2s[i] >> 16);
            }
            audio_pcm_tap_cb_t tap_cb = s_rx.pcm_tap_cb;
            if (tap_cb != NULL) {
                tap_cb(stereo_pcm, AUDIO_CHUNK_FRAMES);
            }
            if (!s_rx.afe_mono_active) {
                process_dual_mic_fallback(stereo_pcm, AUDIO_CHUNK_FRAMES);
            }
        }
    }
}

esp_err_t audio_engine_init(voice_cmd_callback_t cb)
{
    if (s_rx.initialized) {
        s_rx.cmd_cb = cb;
        return ESP_OK;
    }
    s_rx.cmd_cb = cb;
    s_rx.noise_floor_rms = 120.0f;

    esp_err_t err = aec_ref_init();
    if (err != ESP_OK) return err;
    err = audio_tx_ctrl_init();
    if (err != ESP_OK) return err;

    s_rx.buf_mutex = xSemaphoreCreateMutex();
    s_rx.vad_mutex = xSemaphoreCreateMutex();
    s_rx.cmd_queue = xQueueCreate(16, sizeof(voice_cmd_id_t));
    if (!s_rx.buf_mutex || !s_rx.vad_mutex || !s_rx.cmd_queue) return ESP_ERR_NO_MEM;

    s_rx.psram_rec_buf = (int16_t *)heap_caps_malloc(AUDIO_PSRAM_BUF_BYTES,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx.psram_rec_buf) return ESP_ERR_NO_MEM;
    s_rx.rec_max_samples = AUDIO_PSRAM_BUF_BYTES / sizeof(int16_t);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = AUDIO_CHUNK_FRAMES;
    err = i2s_new_channel(&chan_cfg, NULL, &s_rx.rx_chan);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RX_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BOARD_PIN_I2S_MIC_BCLK,
            .ws   = (gpio_num_t)BOARD_PIN_I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)BOARD_PIN_I2S_MIC_DIN,
            .invert_flags = {false, false, false},
        },
    };
    err = i2s_channel_init_std_mode(s_rx.rx_chan, &std_cfg);
    if (err != ESP_OK) return err;
    err = i2s_channel_enable(s_rx.rx_chan);
    if (err != ESP_OK) return err;

    s_rx.initialized = true;
    ESP_LOGI(TAG, "audio_engine 初始化完成 (16kHz 1-Ch PSRAM 緩衝已就緒)");
    return ESP_OK;
}

esp_err_t audio_engine_start_task(void)
{
    if (!s_rx.initialized) return ESP_ERR_INVALID_STATE;
    if (s_rx.io_task == NULL) {
        if (xTaskCreatePinnedToCore(task_audio_io, "task_audio_io", 4096,
                                    NULL, 6, &s_rx.io_task, 1) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return audio_tx_ctrl_start_task();
}

void audio_engine_inject_cmd(voice_cmd_id_t cmd)
{
    if (!s_rx.cmd_queue || cmd == VOICE_CMD_NONE) return;
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_rx.cmd_queue, &cmd, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xQueueSend(s_rx.cmd_queue, &cmd, 0);
    }
}

void audio_engine_register_event_cb(audio_event_callback_t cb) { s_rx.event_cb = cb; }
void audio_engine_register_pcm_tap(audio_pcm_tap_cb_t tap_cb) { s_rx.pcm_tap_cb = tap_cb; }

esp_err_t audio_engine_get_recorded_pcm(const int16_t **out_pcm, size_t *out_bytes)
{
    if (!out_pcm || !out_bytes || !s_rx.psram_rec_buf) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_rx.buf_mutex, portMAX_DELAY);
    *out_pcm = s_rx.psram_rec_buf;
    *out_bytes = s_rx.rec_write_samples * sizeof(int16_t);
    xSemaphoreGive(s_rx.buf_mutex);
    return ESP_OK;
}
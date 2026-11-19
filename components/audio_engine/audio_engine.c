#include "audio_engine.h"
#include "board_bsp.h"
#include "sd_media.h"

#include <stdio.h>
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
#ifndef BOARD_PIN_I2S_SPK_BCLK
#define BOARD_PIN_I2S_SPK_BCLK  15
#endif
#ifndef BOARD_PIN_I2S_SPK_LRC
#define BOARD_PIN_I2S_SPK_LRC   16
#endif
#ifndef BOARD_PIN_I2S_SPK_DOUT
#define BOARD_PIN_I2S_SPK_DOUT  7
#endif

/* 錄音與 VAD 參數設定 */
#define AUDIO_RX_SAMPLE_RATE    16000U
#define AUDIO_CHUNK_FRAMES      512U
#define AUDIO_CHUNK_MS          32U
#define AUDIO_PSRAM_BUF_BYTES   (320U * 1024U)
#define AUDIO_PREROLL_CHUNKS    6U

#define VAD_START_HOLD_MS       150U
#define VAD_END_SILENCE_MS      800U
#define VAD_MIN_NOISE_FLOOR     60.0f
#define VAD_SNR_MULTIPLIER      2.4f
#define VAD_MIN_MARGIN_RMS      140.0f

#define AUDIO_PATH_MAX_LEN      128U
#define TTS_DIR_PREFIX          "/sdcard/tts"

typedef struct {
    /* RX 收音與 VAD */
    i2s_chan_handle_t       rx_chan;
    TaskHandle_t            io_task;
    QueueHandle_t           cmd_queue;
    SemaphoreHandle_t       buf_mutex;

    voice_cmd_callback_t    cmd_cb;
    audio_event_callback_t  event_cb;
    audio_pcm_tap_cb_t      pcm_tap_cb;

    int16_t                *psram_rec_buf;
    size_t                  rec_write_samples;
    size_t                  rec_max_samples;

    int16_t                 preroll_ring[AUDIO_PREROLL_CHUNKS][AUDIO_CHUNK_FRAMES];
    uint8_t                 preroll_idx;
    bool                    preroll_full;

    float                   noise_floor_rms;
    bool                    in_speech;
    uint32_t                speech_hold_ms;
    uint32_t                silence_hold_ms;

    /* TX 播放與音量控制 */
    TaskHandle_t            tx_task;
    SemaphoreHandle_t       tx_mutex;
    SemaphoreHandle_t       tx_done_sem;
    char                    pending_path[AUDIO_PATH_MAX_LEN];
    volatile bool           tx_has_pending;
    volatile bool           tx_busy;
    volatile bool           tts_abort;
    volatile esp_err_t      last_play_err;

    uint8_t                 volume_pct;
    bool                    muted;
    uint16_t                gain_q15;

    bool                    initialized;
} audio_engine_ctx_t;

static audio_engine_ctx_t s_ctx = {
    .volume_pct = 80U,
    .muted      = false,
    .gain_q15   = 20971U, /* (80/100)^2 * 32768 */
};

/* ============================================================================
 * RX 雙麥收音與自適應 VAD
 * ============================================================================ */

static float calc_channel_rms(const int16_t *stereo_pcm, size_t frames, int ch_offset)
{
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t s = stereo_pcm[i * 2U + (size_t)ch_offset];
        sum_sq += (uint64_t)(s * s);
    }
    return sqrtf((float)sum_sq / (float)frames);
}

static void append_psram_mono(const int16_t *mono_samples, size_t sample_count)
{
    if (!s_ctx.psram_rec_buf || sample_count == 0) {
        return;
    }
    size_t remain = s_ctx.rec_max_samples - s_ctx.rec_write_samples;
    size_t to_copy = (sample_count < remain) ? sample_count : remain;
    if (to_copy > 0) {
        memcpy(&s_ctx.psram_rec_buf[s_ctx.rec_write_samples],
               mono_samples,
               to_copy * sizeof(int16_t));
        s_ctx.rec_write_samples += to_copy;
    }
}

static void flush_preroll_to_psram(void)
{
    uint8_t count = s_ctx.preroll_full ? AUDIO_PREROLL_CHUNKS : s_ctx.preroll_idx;
    uint8_t start = s_ctx.preroll_full ? s_ctx.preroll_idx : 0U;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t slot = (uint8_t)((start + i) % AUDIO_PREROLL_CHUNKS);
        append_psram_mono(s_ctx.preroll_ring[slot], AUDIO_CHUNK_FRAMES);
    }
    s_ctx.preroll_idx = 0;
    s_ctx.preroll_full = false;
}

static void process_dual_mic_vad(const int16_t *stereo_pcm, size_t frames)
{
    float rms_l = calc_channel_rms(stereo_pcm, frames, 0);
    float rms_r = calc_channel_rms(stereo_pcm, frames, 1);

    float max_ch = (rms_l > rms_r) ? rms_l : rms_r;
    float min_ch = (rms_l < rms_r) ? rms_l : rms_r;
    float contrast_ratio = (max_ch > 1.0f) ? (min_ch / max_ch) : 1.0f;
    float combined_rms = 0.5f * (rms_l + rms_r);

    if (contrast_ratio < 0.25f) {
        combined_rms *= 0.5f;
    }

    int16_t mono_chunk[AUDIO_CHUNK_FRAMES];
    for (size_t i = 0; i < frames; i++) {
        int32_t avg = ((int32_t)stereo_pcm[i * 2U] + (int32_t)stereo_pcm[i * 2U + 1U]) >> 1;
        mono_chunk[i] = (int16_t)avg;
    }

    float dyn_threshold = s_ctx.noise_floor_rms * VAD_SNR_MULTIPLIER + VAD_MIN_MARGIN_RMS;
    bool is_voice_frame = (combined_rms > dyn_threshold);

    if (!s_ctx.in_speech) {
        if (!is_voice_frame) {
            float alpha = (combined_rms < s_ctx.noise_floor_rms) ? 0.08f : 0.015f;
            s_ctx.noise_floor_rms = (1.0f - alpha) * s_ctx.noise_floor_rms + alpha * combined_rms;
            if (s_ctx.noise_floor_rms < VAD_MIN_NOISE_FLOOR) {
                s_ctx.noise_floor_rms = VAD_MIN_NOISE_FLOOR;
            }
        }

        memcpy(s_ctx.preroll_ring[s_ctx.preroll_idx], mono_chunk, frames * sizeof(int16_t));
        s_ctx.preroll_idx = (uint8_t)((s_ctx.preroll_idx + 1U) % AUDIO_PREROLL_CHUNKS);
        if (s_ctx.preroll_idx == 0U) {
            s_ctx.preroll_full = true;
        }

        if (is_voice_frame) {
            s_ctx.speech_hold_ms += AUDIO_CHUNK_MS;
            if (s_ctx.speech_hold_ms >= VAD_START_HOLD_MS) {
                xSemaphoreTake(s_ctx.buf_mutex, portMAX_DELAY);
                s_ctx.rec_write_samples = 0;
                flush_preroll_to_psram();
                xSemaphoreGive(s_ctx.buf_mutex);

                s_ctx.in_speech = true;
                s_ctx.silence_hold_ms = 0;
                if (s_ctx.event_cb) {
                    s_ctx.event_cb(AUDIO_EVENT_SPEECH_START, 0);
                }
            }
        } else {
            s_ctx.speech_hold_ms = 0;
        }
    } else {
        xSemaphoreTake(s_ctx.buf_mutex, portMAX_DELAY);
        append_psram_mono(mono_chunk, frames);
        size_t cur_bytes = s_ctx.rec_write_samples * sizeof(int16_t);
        bool buf_full = (s_ctx.rec_write_samples >= s_ctx.rec_max_samples);
        xSemaphoreGive(s_ctx.buf_mutex);

        if (is_voice_frame) {
            s_ctx.silence_hold_ms = 0;
        } else {
            s_ctx.silence_hold_ms += AUDIO_CHUNK_MS;
        }

        if (s_ctx.silence_hold_ms >= VAD_END_SILENCE_MS || buf_full) {
            s_ctx.in_speech = false;
            s_ctx.speech_hold_ms = 0;
            s_ctx.silence_hold_ms = 0;
            if (s_ctx.event_cb) {
                s_ctx.event_cb(AUDIO_EVENT_SPEECH_END, cur_bytes);
            }
        }
    }
}

static void task_audio_io(void *arg)
{
    (void)arg;
    int32_t raw_i2s[AUDIO_CHUNK_FRAMES * 2U];
    int16_t stereo_pcm[AUDIO_CHUNK_FRAMES * 2U];
    voice_cmd_id_t pending_cmd = VOICE_CMD_NONE;

    while (1) {
        while (xQueueReceive(s_ctx.cmd_queue, &pending_cmd, 0) == pdTRUE) {
            if (pending_cmd != VOICE_CMD_NONE && s_ctx.cmd_cb != NULL) {
                s_ctx.cmd_cb(pending_cmd);
            }
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_ctx.rx_chan,
                                         raw_i2s,
                                         sizeof(raw_i2s),
                                         &bytes_read,
                                         pdMS_TO_TICKS(50));
        if (ret == ESP_OK && bytes_read == sizeof(raw_i2s)) {
            for (size_t i = 0; i < AUDIO_CHUNK_FRAMES * 2U; i++) {
                stereo_pcm[i] = (int16_t)(raw_i2s[i] >> 16);
            }
            audio_pcm_tap_cb_t tap_cb = s_ctx.pcm_tap_cb;
            if (tap_cb != NULL) {
                tap_cb(stereo_pcm, AUDIO_CHUNK_FRAMES);
            }
            process_dual_mic_vad(stereo_pcm, AUDIO_CHUNK_FRAMES);
        }
    }
}

/* ============================================================================
 * TX 播放任務、動態取樣率重配置與軟體音量控制
 * ============================================================================ */

static void task_audio_tx(void *arg)
{
    (void)arg;
    char local_path[AUDIO_PATH_MAX_LEN];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {
            xSemaphoreTake(s_ctx.tx_mutex, portMAX_DELAY);
            if (!s_ctx.tx_has_pending) {
                xSemaphoreGive(s_ctx.tx_mutex);
                break;
            }
            strncpy(local_path, s_ctx.pending_path, sizeof(local_path) - 1U);
            local_path[sizeof(local_path) - 1U] = '\0';
            s_ctx.tx_has_pending = false;
            s_ctx.tx_busy = true;
            xSemaphoreGive(s_ctx.tx_mutex);

            esp_err_t err = sd_media_play_audio(local_path);
            s_ctx.last_play_err = err;
            s_ctx.tx_busy = false;
            xSemaphoreGive(s_ctx.tx_done_sem);
        }
    }
}

static void update_gain_q15(void)
{
    if (s_ctx.muted || s_ctx.volume_pct == 0U) {
        s_ctx.gain_q15 = 0U;
    } else {
        /* 感知對數近似曲線 (percent / 100)^2 轉 Q15 */
        uint32_t v = s_ctx.volume_pct;
        s_ctx.gain_q15 = (uint16_t)((v * v * 32768U) / 10000U);
    }
}

void audio_engine_apply_volume_pcm(int16_t *pcm, size_t sample_count)
{
    if (pcm == NULL || sample_count == 0U) {
        return;
    }
    uint16_t g = s_ctx.gain_q15;
    if (g == 32768U) {
        return;
    }
    if (g == 0U) {
        memset(pcm, 0, sample_count * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < sample_count; i++) {
        int32_t scaled = ((int32_t)pcm[i] * (int32_t)g) >> 15;
        if (scaled > 32767) {
            scaled = 32767;
        } else if (scaled < -32768) {
            scaled = -32768;
        }
        pcm[i] = (int16_t)scaled;
    }
}

esp_err_t audio_engine_set_volume(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    s_ctx.volume_pct = percent;
    update_gain_q15();
    return ESP_OK;
}

uint8_t audio_engine_adjust_volume(int8_t delta_percent)
{
    int32_t next = (int32_t)s_ctx.volume_pct + (int32_t)delta_percent;
    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    if (delta_percent > 0 && s_ctx.muted) {
        s_ctx.muted = false;
    }
    s_ctx.volume_pct = (uint8_t)next;
    update_gain_q15();
    return s_ctx.volume_pct;
}

esp_err_t audio_engine_set_mute(bool mute)
{
    s_ctx.muted = mute;
    update_gain_q15();
    return ESP_OK;
}

esp_err_t audio_engine_reconfig_tx_rate(uint32_t sample_rate)
{
    if (sample_rate == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sd_media_is_playing() || s_ctx.tx_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_handle_t tx_chan = board_get_i2s_tx_chan();
    bool temp_alloc = false;

    if (tx_chan == NULL) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
        if (err != ESP_OK) {
            return err;
        }
        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)BOARD_PIN_I2S_SPK_BCLK,
                .ws   = (gpio_num_t)BOARD_PIN_I2S_SPK_LRC,
                .dout = (gpio_num_t)BOARD_PIN_I2S_SPK_DOUT,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = {false, false, false},
            },
        };
        err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
        if (err != ESP_OK) {
            i2s_del_channel(tx_chan);
            return err;
        }
        temp_alloc = true;
    }

    esp_err_t dis_err = i2s_channel_disable(tx_chan);
    if (dis_err != ESP_OK && dis_err != ESP_ERR_INVALID_STATE) {
        if (temp_alloc) {
            i2s_del_channel(tx_chan);
        }
        return dis_err;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(tx_chan);
        (void)i2s_channel_disable(tx_chan);
    }

    if (temp_alloc) {
        i2s_del_channel(tx_chan);
    }
    return err;
}

esp_err_t audio_engine_play_file(const char *filepath)
{
    if (!s_ctx.initialized || filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.tx_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sd_media_is_playing() || s_ctx.tx_busy) {
        (void)audio_engine_stop_playback();
    }

    xSemaphoreTake(s_ctx.tx_mutex, portMAX_DELAY);
    strncpy(s_ctx.pending_path, filepath, sizeof(s_ctx.pending_path) - 1U);
    s_ctx.pending_path[sizeof(s_ctx.pending_path) - 1U] = '\0';
    s_ctx.tx_has_pending = true;
    (void)xSemaphoreTake(s_ctx.tx_done_sem, 0);
    xSemaphoreGive(s_ctx.tx_mutex);

    xTaskNotifyGive(s_ctx.tx_task);
    return ESP_OK;
}

esp_err_t audio_engine_play_file_sync(const char *filepath, uint32_t timeout_ms)
{
    esp_err_t err = audio_engine_play_file(filepath);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_ctx.tx_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        (void)audio_engine_stop_playback();
        return ESP_ERR_TIMEOUT;
    }
    return s_ctx.last_play_err;
}

esp_err_t audio_engine_stop_playback(void)
{
    s_ctx.tts_abort = true;
    sd_media_stop_audio();

    for (uint32_t elapsed = 0; elapsed < 50U; elapsed += 5U) {
        if (!sd_media_is_playing() && !s_ctx.tx_busy) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return (!sd_media_is_playing() && !s_ctx.tx_busy) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ============================================================================
 * 環境統計數字 WAV 音節拼接播報
 * ============================================================================ */

static esp_err_t play_tts_clip(const char *clip_name)
{
    if (s_ctx.tts_abort) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[AUDIO_PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/%s.wav", TTS_DIR_PREFIX, clip_name);
    return audio_engine_play_file_sync(path, 3000U);
}

static esp_err_t speak_int_0_to_100(int val)
{
    char digit_buf[8];
    if (val < 0) {
        esp_err_t e = play_tts_clip("minus");
        if (e != ESP_OK) return e;
        val = -val;
    }
    if (val >= 100) {
        esp_err_t e = play_tts_clip("1");
        if (e != ESP_OK) return e;
        return play_tts_clip("100");
    }
    if (val < 10) {
        snprintf(digit_buf, sizeof(digit_buf), "%d", val);
        return play_tts_clip(digit_buf);
    }

    int tens = val / 10;
    int ones = val % 10;
    if (tens > 1) {
        snprintf(digit_buf, sizeof(digit_buf), "%d", tens);
        esp_err_t e = play_tts_clip(digit_buf);
        if (e != ESP_OK) return e;
    }
    esp_err_t e = play_tts_clip("10");
    if (e != ESP_OK) return e;

    if (ones > 0) {
        snprintf(digit_buf, sizeof(digit_buf), "%d", ones);
        return play_tts_clip(digit_buf);
    }
    return ESP_OK;
}

esp_err_t audio_engine_speak_env_stats(const env_stats_t *stats)
{
    if (stats == NULL || stats->valid_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_media_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.tts_abort = false;

    /* 1. 播報平均溫度（精確到小數點後 1 位） */
    int temp_x10 = (int)lroundf(stats->temp_avg * 10.0f);
    int temp_int = temp_x10 / 10;
    int temp_dec = abs(temp_x10 % 10);

    esp_err_t err = play_tts_clip("temp_is");
    if (err != ESP_OK) return err;
    err = speak_int_0_to_100(temp_int);
    if (err != ESP_OK) return err;
    if (temp_dec > 0) {
        char dec_buf[4];
        snprintf(dec_buf, sizeof(dec_buf), "%d", temp_dec);
        err = play_tts_clip("point");
        if (err != ESP_OK) return err;
        err = play_tts_clip(dec_buf);
        if (err != ESP_OK) return err;
    }
    err = play_tts_clip("deg_c");
    if (err != ESP_OK) return err;

    /* 2. 播報平均濕度（整數百分比） */
    int humi_int = (int)lroundf(stats->humi_avg);
    if (humi_int < 0) humi_int = 0;
    if (humi_int > 100) humi_int = 100;

    err = play_tts_clip("humi_is");
    if (err != ESP_OK) return err;
    return speak_int_0_to_100(humi_int);
}

/* ============================================================================
 * 初始化與公開查詢 API
 * ============================================================================ */

esp_err_t audio_engine_init(voice_cmd_callback_t cb)
{
    if (s_ctx.initialized) {
        s_ctx.cmd_cb = cb;
        return ESP_OK;
    }

    s_ctx.cmd_cb = cb;
    s_ctx.noise_floor_rms = 120.0f;
    update_gain_q15();

    s_ctx.buf_mutex   = xSemaphoreCreateMutex();
    s_ctx.tx_mutex    = xSemaphoreCreateMutex();
    s_ctx.tx_done_sem = xSemaphoreCreateBinary();
    s_ctx.cmd_queue   = xQueueCreate(16, sizeof(voice_cmd_id_t));
    if (!s_ctx.buf_mutex || !s_ctx.tx_mutex || !s_ctx.tx_done_sem || !s_ctx.cmd_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_ctx.psram_rec_buf = (int16_t *)heap_caps_malloc(AUDIO_PSRAM_BUF_BYTES,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.psram_rec_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_ctx.rec_max_samples = AUDIO_PSRAM_BUF_BYTES / sizeof(int16_t);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = AUDIO_CHUNK_FRAMES;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_ctx.rx_chan);
    if (err != ESP_OK) {
        return err;
    }

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

    err = i2s_channel_init_std_mode(s_ctx.rx_chan, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = i2s_channel_enable(s_ctx.rx_chan);
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "audio_engine 初始化完成 (RX=I2S0, TX=I2S1 via sd_media)");
    return ESP_OK;
}

esp_err_t audio_engine_start_task(void)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.io_task == NULL) {
        BaseType_t ok_io = xTaskCreatePinnedToCore(task_audio_io,
                                                   "task_audio_io",
                                                   4096,
                                                   NULL,
                                                   6,
                                                   &s_ctx.io_task,
                                                   1);
        if (ok_io != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ctx.tx_task == NULL) {
        BaseType_t ok_tx = xTaskCreatePinnedToCore(task_audio_tx,
                                                   "task_audio_tx",
                                                   4096,
                                                   NULL,
                                                   5,
                                                   &s_ctx.tx_task,
                                                   1);
        if (ok_tx != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void audio_engine_inject_cmd(voice_cmd_id_t cmd)
{
    if (!s_ctx.cmd_queue || cmd == VOICE_CMD_NONE) {
        return;
    }
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_ctx.cmd_queue, &cmd, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xQueueSend(s_ctx.cmd_queue, &cmd, 0);
    }
}

void audio_engine_register_event_cb(audio_event_callback_t cb)
{
    s_ctx.event_cb = cb;
}

void audio_engine_register_pcm_tap(audio_pcm_tap_cb_t tap_cb)
{
    s_ctx.pcm_tap_cb = tap_cb;
}

esp_err_t audio_engine_get_recorded_pcm(const int16_t **out_pcm, size_t *out_bytes)
{
    if (!out_pcm || !out_bytes || !s_ctx.psram_rec_buf) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_ctx.buf_mutex, portMAX_DELAY);
    *out_pcm = s_ctx.psram_rec_buf;
    *out_bytes = s_ctx.rec_write_samples * sizeof(int16_t);
    xSemaphoreGive(s_ctx.buf_mutex);
    return ESP_OK;
}
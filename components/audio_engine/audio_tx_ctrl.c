#include "audio_engine.h"
#include "audio_engine_priv.h"
#include "aec_ref.h"
#include "board_bsp.h"
#include "sd_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"

#ifndef BOARD_PIN_I2S_SPK_BCLK
#define BOARD_PIN_I2S_SPK_BCLK  15
#endif
#ifndef BOARD_PIN_I2S_SPK_LRC
#define BOARD_PIN_I2S_SPK_LRC   16
#endif
#ifndef BOARD_PIN_I2S_SPK_DOUT
#define BOARD_PIN_I2S_SPK_DOUT  7
#endif

#define AUDIO_PATH_MAX_LEN      128U
#define TTS_DIR_PREFIX          "/sdcard/tts"
#define TX_DMA_DESC_NUM         8
#define TX_DMA_FRAME_NUM        160U  /* 8 * 160 = 1280 frames (80 ms @ 16kHz) */

typedef struct {
    TaskHandle_t        tx_task;
    SemaphoreHandle_t   tx_mutex;
    SemaphoreHandle_t   tx_done_sem;
    char                pending_path[AUDIO_PATH_MAX_LEN];
    volatile bool       tx_has_pending;
    volatile bool       tx_busy;
    volatile bool       tts_abort;
    volatile esp_err_t  last_play_err;

    uint32_t            tx_sample_rate;
    uint8_t             volume_pct;
    bool                muted;
    uint16_t            gain_q15;
    bool                initialized;
} audio_tx_ctx_t;

static audio_tx_ctx_t s_tx = {
    .tx_sample_rate = 16000U,
    .volume_pct     = 80U,
    .muted          = false,
    .gain_q15       = 20971U, /* (80/100)^2 * 32768 */
};

static void update_gain_q15(void)
{
    if (s_tx.muted || s_tx.volume_pct == 0U) {
        s_tx.gain_q15 = 0U;
    } else {
        /* 感知對數近似曲線 (percent / 100)^2 轉 Q15 */
        uint32_t v = s_tx.volume_pct;
        s_tx.gain_q15 = (uint16_t)((v * v * 32768U) / 10000U);
    }
}

static void task_audio_tx(void *arg)
{
    (void)arg;
    char local_path[AUDIO_PATH_MAX_LEN];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {
            xSemaphoreTake(s_tx.tx_mutex, portMAX_DELAY);
            if (!s_tx.tx_has_pending) {
                xSemaphoreGive(s_tx.tx_mutex);
                break;
            }
            strncpy(local_path, s_tx.pending_path, sizeof(local_path) - 1U);
            local_path[sizeof(local_path) - 1U] = '\0';
            s_tx.tx_has_pending = false;
            s_tx.tx_busy = true;
            xSemaphoreGive(s_tx.tx_mutex);

            esp_err_t err = sd_media_play_audio(local_path);
            s_tx.last_play_err = err;
            s_tx.tx_busy = false;
            aec_ref_flush();
            xSemaphoreGive(s_tx.tx_done_sem);
        }
    }
}

esp_err_t audio_tx_ctrl_init(void)
{
    if (s_tx.initialized) {
        return ESP_OK;
    }
    update_gain_q15();
    s_tx.tx_mutex    = xSemaphoreCreateMutex();
    s_tx.tx_done_sem = xSemaphoreCreateBinary();
    if (!s_tx.tx_mutex || !s_tx.tx_done_sem) {
        return ESP_ERR_NO_MEM;
    }
    s_tx.initialized = true;
    return ESP_OK;
}

esp_err_t audio_tx_ctrl_start_task(void)
{
    if (!s_tx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tx.tx_task == NULL) {
        if (xTaskCreatePinnedToCore(task_audio_tx, "task_audio_tx", 4096,
                                    NULL, 5, &s_tx.tx_task, 1) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void audio_engine_apply_volume_pcm(int16_t *pcm, size_t sample_count)
{
    if (pcm == NULL || sample_count == 0U) {
        return;
    }
    uint16_t g = s_tx.gain_q15;
    if (g == 0U) {
        memset(pcm, 0, sample_count * sizeof(int16_t));
    } else if (g != 32768U) {
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
    /* 音量縮放後、緊貼 I2S TX DMA 寫入前一刻擷取立體聲交錯參考訊號 */
    if (sample_count >= 2U) {
        aec_ref_write(pcm, sample_count / 2U, s_tx.tx_sample_rate, 2U);
    }
}

esp_err_t audio_engine_set_volume(uint8_t percent)
{
    s_tx.volume_pct = (percent > 100U) ? 100U : percent;
    update_gain_q15();
    return ESP_OK;
}

uint8_t audio_engine_adjust_volume(int8_t delta_percent)
{
    int32_t next = (int32_t)s_tx.volume_pct + (int32_t)delta_percent;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    if (delta_percent > 0 && s_tx.muted) {
        s_tx.muted = false;
    }
    s_tx.volume_pct = (uint8_t)next;
    update_gain_q15();
    return s_tx.volume_pct;
}

esp_err_t audio_engine_set_mute(bool mute)
{
    s_tx.muted = mute;
    update_gain_q15();
    return ESP_OK;
}

esp_err_t audio_engine_reconfig_tx_rate(uint32_t sample_rate)
{
    if (sample_rate == 0U) return ESP_ERR_INVALID_ARG;
    if (sd_media_is_playing() || s_tx.tx_busy) return ESP_ERR_INVALID_STATE;

    s_tx.tx_sample_rate = sample_rate;
    i2s_chan_handle_t tx_chan = board_get_i2s_tx_chan();
    bool temp_alloc = false;

    if (tx_chan == NULL) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        chan_cfg.dma_desc_num = TX_DMA_DESC_NUM;
        chan_cfg.dma_frame_num = TX_DMA_FRAME_NUM;
        chan_cfg.auto_clear = true;
        if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK) return ESP_FAIL;

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
        if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK) {
            i2s_del_channel(tx_chan);
            return ESP_FAIL;
        }
        temp_alloc = true;
    }

    esp_err_t dis_err = i2s_channel_disable(tx_chan);
    if (dis_err != ESP_OK && dis_err != ESP_ERR_INVALID_STATE) {
        if (temp_alloc) i2s_del_channel(tx_chan);
        return dis_err;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(tx_chan);
        (void)i2s_channel_disable(tx_chan);
    }
    if (temp_alloc) i2s_del_channel(tx_chan);
    return err;
}

esp_err_t audio_engine_play_file(const char *filepath)
{
    if (!s_tx.initialized || filepath == NULL) return ESP_ERR_INVALID_ARG;
    if (s_tx.tx_task == NULL) return ESP_ERR_INVALID_STATE;

    if (sd_media_is_playing() || s_tx.tx_busy) {
        (void)audio_engine_stop_playback();
    }
    xSemaphoreTake(s_tx.tx_mutex, portMAX_DELAY);
    strncpy(s_tx.pending_path, filepath, sizeof(s_tx.pending_path) - 1U);
    s_tx.pending_path[sizeof(s_tx.pending_path) - 1U] = '\0';
    s_tx.tx_has_pending = true;
    (void)xSemaphoreTake(s_tx.tx_done_sem, 0);
    xSemaphoreGive(s_tx.tx_mutex);

    xTaskNotifyGive(s_tx.tx_task);
    return ESP_OK;
}

esp_err_t audio_engine_play_file_sync(const char *filepath, uint32_t timeout_ms)
{
    esp_err_t err = audio_engine_play_file(filepath);
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_tx.tx_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        (void)audio_engine_stop_playback();
        return ESP_ERR_TIMEOUT;
    }
    return s_tx.last_play_err;
}

esp_err_t audio_engine_stop_playback(void)
{
    s_tx.tts_abort = true;
    sd_media_stop_audio();
    aec_ref_flush();

    for (uint32_t elapsed = 0; elapsed < 50U; elapsed += 5U) {
        if (!sd_media_is_playing() && !s_tx.tx_busy) {
            aec_ref_flush();
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return (!sd_media_is_playing() && !s_tx.tx_busy) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t play_tts_clip(const char *clip_name)
{
    if (s_tx.tts_abort) return ESP_ERR_INVALID_STATE;
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
    if (stats == NULL || stats->valid_count == 0U) return ESP_ERR_INVALID_ARG;
    if (!sd_media_is_mounted()) return ESP_ERR_INVALID_STATE;

    s_tx.tts_abort = false;
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

    int humi_int = (int)lroundf(stats->humi_avg);
    if (humi_int < 0) humi_int = 0;
    if (humi_int > 100) humi_int = 100;

    err = play_tts_clip("humi_is");
    if (err != ESP_OK) return err;
    return speak_int_0_to_100(humi_int);
}
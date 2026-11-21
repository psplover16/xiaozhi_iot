#include "aec_ref.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "aec_ref";

#define RESAMPLE_CHUNK_CAP 128U

static int16_t *s_ring_buf = NULL;
static size_t s_head = 0;
static size_t s_tail = 0;
static size_t s_count = 0;
static bool s_is_playing = false;

/* Q16.16 定點數重採樣與抗混疊低通濾波狀態 */
static uint32_t s_last_src_rate = 0;
static uint32_t s_phase_q16 = 0;
static int32_t s_prev_filtered = 0;
static int32_t s_lp1 = 0;
static int32_t s_lp2 = 0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void ring_push_locked(const int16_t *samples, size_t count)
{
    if (count == 0 || s_ring_buf == NULL) {
        return;
    }
    if (count >= AEC_REF_HIGH_WATER_SAMPLES) {
        samples += (count - AEC_REF_HIGH_WATER_SAMPLES);
        count = AEC_REF_HIGH_WATER_SAMPLES;
        s_head = 0;
        s_tail = 0;
        s_count = 0;
    } else if (s_count + count > AEC_REF_HIGH_WATER_SAMPLES) {
        size_t drop = (s_count + count) - AEC_REF_HIGH_WATER_SAMPLES;
        s_head = (s_head + drop) % AEC_REF_CAP_SAMPLES;
        s_count -= drop;
    }

    size_t first = AEC_REF_CAP_SAMPLES - s_tail;
    if (first >= count) {
        memcpy(&s_ring_buf[s_tail], samples, count * sizeof(int16_t));
        s_tail = (s_tail + count) % AEC_REF_CAP_SAMPLES;
    } else {
        memcpy(&s_ring_buf[s_tail], samples, first * sizeof(int16_t));
        memcpy(&s_ring_buf[0], &samples[first], (count - first) * sizeof(int16_t));
        s_tail = count - first;
    }
    s_count += count;
}

static void push_prefill_if_needed(uint32_t src_rate)
{
    bool need_prefill = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_is_playing) {
        s_is_playing = true;
        need_prefill = true;
    }
    portEXIT_CRITICAL(&s_mux);

    if (need_prefill || s_last_src_rate != src_rate) {
        s_last_src_rate = src_rate;
        s_phase_q16 = 0;
        s_prev_filtered = 0;
        s_lp1 = 0;
        s_lp2 = 0;
    }

    if (need_prefill) {
        int16_t zeros[RESAMPLE_CHUNK_CAP] = {0};
        size_t rem = AEC_REF_PREFILL_SAMPLES;
        while (rem > 0) {
            size_t n = (rem > RESAMPLE_CHUNK_CAP) ? RESAMPLE_CHUNK_CAP : rem;
            portENTER_CRITICAL(&s_mux);
            ring_push_locked(zeros, n);
            portEXIT_CRITICAL(&s_mux);
            rem -= n;
        }
    }
}

static inline int32_t extract_mono_sample(const int16_t *pcm, size_t idx, uint8_t channels)
{
    if (channels >= 2) {
        return ((int32_t)pcm[idx * 2] + (int32_t)pcm[idx * 2 + 1]) >> 1;
    }
    return (int32_t)pcm[idx];
}

esp_err_t aec_ref_init(void)
{
    if (s_ring_buf != NULL) {
        return ESP_OK;
    }
    size_t bytes = AEC_REF_CAP_SAMPLES * sizeof(int16_t);
    s_ring_buf = (int16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_ring_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes in internal SRAM", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    aec_ref_flush();
    ESP_LOGI(TAG, "AEC ref buffer initialized in internal SRAM (%u bytes)", (unsigned)bytes);
    return ESP_OK;
}

void aec_ref_deinit(void)
{
    if (s_ring_buf != NULL) {
        heap_caps_free(s_ring_buf);
        s_ring_buf = NULL;
    }
    aec_ref_flush();
}

void aec_ref_write(const int16_t *pcm, size_t frames, uint32_t src_rate, uint8_t channels)
{
    if (s_ring_buf == NULL || pcm == NULL || frames == 0 || src_rate == 0) {
        return;
    }
    push_prefill_if_needed(src_rate);

    int16_t out_chunk[RESAMPLE_CHUNK_CAP];
    size_t out_idx = 0;

    /* 16 kHz 直通路徑（僅處理立體聲轉單聲道） */
    if (src_rate == AEC_REF_SAMPLE_RATE) {
        for (size_t i = 0; i < frames; i++) {
            out_chunk[out_idx++] = (int16_t)extract_mono_sample(pcm, i, channels);
            if (out_idx == RESAMPLE_CHUNK_CAP) {
                portENTER_CRITICAL(&s_mux);
                ring_push_locked(out_chunk, out_idx);
                portEXIT_CRITICAL(&s_mux);
                out_idx = 0;
            }
        }
        if (out_idx > 0) {
            portENTER_CRITICAL(&s_mux);
            ring_push_locked(out_chunk, out_idx);
            portEXIT_CRITICAL(&s_mux);
        }
        return;
    }

    /* Q16.16 無漂移相位累加器 + 抗混疊低通濾波 */
    uint32_t step_q16 = (uint32_t)(((uint64_t)src_rate << 16) / AEC_REF_SAMPLE_RATE);
    bool two_stage_lp = (src_rate >= 32000U);

    for (size_t i = 0; i < frames; i++) {
        int32_t raw = extract_mono_sample(pcm, i, channels);
        s_lp1 = (s_lp1 + raw) >> 1;
        int32_t curr = two_stage_lp ? ((s_lp2 = (s_lp2 + s_lp1) >> 1)) : s_lp1;

        while (s_phase_q16 < 65536U) {
            int32_t frac = (int32_t)(s_phase_q16 & 0xFFFFU);
            int32_t interp = s_prev_filtered + (((curr - s_prev_filtered) * frac) >> 16);
            if (interp > 32767) {
                interp = 32767;
            } else if (interp < -32768) {
                interp = -32768;
            }
            out_chunk[out_idx++] = (int16_t)interp;
            if (out_idx == RESAMPLE_CHUNK_CAP) {
                portENTER_CRITICAL(&s_mux);
                ring_push_locked(out_chunk, out_idx);
                portEXIT_CRITICAL(&s_mux);
                out_idx = 0;
            }
            s_phase_q16 += step_q16;
        }
        s_phase_q16 -= 65536U;
        s_prev_filtered = curr;
    }

    if (out_idx > 0) {
        portENTER_CRITICAL(&s_mux);
        ring_push_locked(out_chunk, out_idx);
        portEXIT_CRITICAL(&s_mux);
    }
}

size_t aec_ref_read(int16_t *out_samples, size_t count)
{
    if (out_samples == NULL || count == 0) {
        return 0;
    }
    if (s_ring_buf == NULL) {
        memset(out_samples, 0, count * sizeof(int16_t));
        return 0;
    }

    size_t avail = 0;
    portENTER_CRITICAL(&s_mux);
    avail = (s_count < count) ? s_count : count;
    if (avail > 0) {
        size_t first = AEC_REF_CAP_SAMPLES - s_head;
        if (first >= avail) {
            memcpy(out_samples, &s_ring_buf[s_head], avail * sizeof(int16_t));
            s_head = (s_head + avail) % AEC_REF_CAP_SAMPLES;
        } else {
            memcpy(out_samples, &s_ring_buf[s_head], first * sizeof(int16_t));
            memcpy(&out_samples[first], &s_ring_buf[0], (avail - first) * sizeof(int16_t));
            s_head = avail - first;
        }
        s_count -= avail;
    }
    if (s_count == 0) {
        s_is_playing = false;
    }
    portEXIT_CRITICAL(&s_mux);

    /* 待機或緩衝不足時自動補 0 靜音樣本，確保 ESP-AFE feed() 不阻塞 */
    if (avail < count) {
        memset(&out_samples[avail], 0, (count - avail) * sizeof(int16_t));
    }
    return avail;
}

void aec_ref_flush(void)
{
    portENTER_CRITICAL(&s_mux);
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_is_playing = false;
    portEXIT_CRITICAL(&s_mux);

    s_last_src_rate = 0;
    s_phase_q16 = 0;
    s_prev_filtered = 0;
    s_lp1 = 0;
    s_lp2 = 0;
}
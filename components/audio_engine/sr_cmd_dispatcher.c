/**
 * @file sr_cmd_dispatcher.c
 * @brief Offline Speech Pinyin Syllable Splicing & Playback Dispatcher
 */

#include "sr_engine.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "sd_media.h"

static const char *TAG = "sr_dispatcher";

#define TTS_OFFLINE_DIR         "/sdcard/tts_offline"
#define TTS_MAX_CLIP_SEQUENCE   24U
#define TTS_CLIP_PLAY_TIMEOUT   3000U

static void push_clip(const char *filename, char out_paths[][64],
                      size_t max_clips, size_t *io_count)
{
    if (filename == NULL || out_paths == NULL || io_count == NULL) {
        return;
    }
    if (*io_count >= max_clips) {
        return;
    }
    snprintf(out_paths[*io_count], 64U, "%s/%s", TTS_OFFLINE_DIR, filename);
    (*io_count)++;
}

static void push_digit_clip(uint32_t digit, char out_paths[][64],
                            size_t max_clips, size_t *io_count)
{
    char fname[12];
    snprintf(fname, sizeof(fname), "%u.wav", (unsigned)(digit % 10U));
    push_clip(fname, out_paths, max_clips, io_count);
}

static void append_int_clips(uint32_t num, char out_paths[][64],
                             size_t max_clips, size_t *io_count)
{
    if (num == 0U) {
        push_clip("0.wav", out_paths, max_clips, io_count);
        return;
    }

    uint32_t wan = num / 10000U;
    uint32_t rem = num % 10000U;
    if (wan > 0U) {
        append_int_clips(wan, out_paths, max_clips, io_count);
        push_clip("wan.wav", out_paths, max_clips, io_count);
    }

    uint32_t qian = rem / 1000U;
    uint32_t bai  = (rem % 1000U) / 100U;
    uint32_t shi  = (rem % 100U) / 10U;
    uint32_t ge   = rem % 10U;
    bool need_zero = false;

    if (qian > 0U) {
        push_digit_clip(qian, out_paths, max_clips, io_count);
        push_clip("qian.wav", out_paths, max_clips, io_count);
    } else if (wan > 0U && rem > 0U) {
        need_zero = true;
    }

    if (bai > 0U) {
        if (need_zero) {
            push_clip("0.wav", out_paths, max_clips, io_count);
            need_zero = false;
        }
        push_digit_clip(bai, out_paths, max_clips, io_count);
        push_clip("bai.wav", out_paths, max_clips, io_count);
    } else if ((wan > 0U || qian > 0U) && (rem % 100U) > 0U) {
        need_zero = true;
    }

    if (shi > 0U) {
        if (need_zero) {
            push_clip("0.wav", out_paths, max_clips, io_count);
            need_zero = false;
        }
        if (!(shi == 1U && wan == 0U && qian == 0U && bai == 0U)) {
            push_digit_clip(shi, out_paths, max_clips, io_count);
        }
        push_clip("shi.wav", out_paths, max_clips, io_count);
    } else if ((wan > 0U || qian > 0U || bai > 0U) && ge > 0U) {
        need_zero = true;
    }

    if (ge > 0U) {
        if (need_zero) {
            push_clip("0.wav", out_paths, max_clips, io_count);
        }
        push_digit_clip(ge, out_paths, max_clips, io_count);
    }
}

size_t sr_dispatcher_build_number_clips(float value, int decimal_places,
                                        char out_paths[][64], size_t max_clips)
{
    if (out_paths == NULL || max_clips == 0U) {
        return 0U;
    }

    int places = decimal_places;
    if (places < 0) {
        places = 0;
    } else if (places > 4) {
        places = 4;
    }

    uint32_t scale = 1U;
    for (int i = 0; i < places; i++) {
        scale *= 10U;
    }

    bool is_neg = (value < 0.0f);
    float abs_val = is_neg ? -value : value;
    uint32_t scaled = (uint32_t)(abs_val * (float)scale + 0.5f);

    size_t count = 0U;
    if (is_neg && scaled > 0U) {
        push_clip("fu.wav", out_paths, max_clips, &count);
    }

    uint32_t int_part  = scaled / scale;
    uint32_t frac_part = scaled % scale;
    append_int_clips(int_part, out_paths, max_clips, &count);

    if (places > 0) {
        push_clip("dian.wav", out_paths, max_clips, &count);
        uint32_t div = scale / 10U;
        for (int i = 0; i < places; i++) {
            uint32_t digit = (frac_part / div) % 10U;
            push_digit_clip(digit, out_paths, max_clips, &count);
            div /= 10U;
        }
    }

    return count;
}

static esp_err_t play_clip_sequence(char clips[][64], size_t count)
{
    if (count == 0U) {
        return ESP_OK;
    }
    if (!sd_media_is_mounted()) {
        ESP_LOGW(TAG, "SD 卡未掛載，安全降級為僅顯示 OLED");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t last_err = ESP_OK;
    for (size_t i = 0U; i < count; i++) {
        if (!sd_media_is_mounted()) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = audio_engine_play_file_sync(clips[i], TTS_CLIP_PLAY_TIMEOUT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "缺失或無法播放音檔 [%s]: %s", clips[i], esp_err_to_name(err));
            last_err = err;
        }
    }
    return last_err;
}

esp_err_t sr_dispatcher_speak_temperature(float temp_c, int8_t trend)
{
    char clips[TTS_MAX_CLIP_SEQUENCE][64];
    size_t n = 0U;

    push_clip("wen_du.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    n += sr_dispatcher_build_number_clips(temp_c, 1, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
    push_clip("du.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);

    if (trend > 0) {
        push_clip("shang_sheng.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    } else if (trend < 0) {
        push_clip("xia_jiang.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    } else {
        push_clip("chi_ping.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    }

    return play_clip_sequence(clips, n);
}

esp_err_t sr_dispatcher_speak_humidity(float humi_pct)
{
    char clips[TTS_MAX_CLIP_SEQUENCE][64];
    size_t n = 0U;

    push_clip("shi_du.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    push_clip("bai_fen_zhi.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    n += sr_dispatcher_build_number_clips(humi_pct, 1, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
    return play_clip_sequence(clips, n);
}

esp_err_t sr_dispatcher_speak_lux(uint32_t lux)
{
    char clips[TTS_MAX_CLIP_SEQUENCE][64];
    size_t n = 0U;

    push_clip("guang_zhao.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    n += sr_dispatcher_build_number_clips((float)lux, 0, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
    push_clip("liu_ming.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    return play_clip_sequence(clips, n);
}

esp_err_t sr_dispatcher_speak_time(uint8_t hour, uint8_t minute)
{
    char clips[TTS_MAX_CLIP_SEQUENCE][64];
    size_t n = 0U;

    uint8_t h = (uint8_t)(hour % 24U);
    uint8_t m = (uint8_t)(minute % 60U);

    push_clip("xian_zai_shi_jian.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    n += sr_dispatcher_build_number_clips((float)h, 0, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
    push_clip("dian.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);

    if (m == 0U) {
        push_clip("zheng.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    } else {
        if (m < 10U) {
            push_clip("0.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
        }
        n += sr_dispatcher_build_number_clips((float)m, 0, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
        push_clip("fen.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    }

    return play_clip_sequence(clips, n);
}

esp_err_t sr_dispatcher_speak_storage_status(uint8_t alert_flags, uint16_t eeprom_pct, uint32_t sd_free_mb)
{
    char clips[TTS_MAX_CLIP_SEQUENCE][64];
    size_t n = 0U;

    push_clip("hei_he_zi.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    push_clip("bai_fen_zhi.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    n += sr_dispatcher_build_number_clips((float)eeprom_pct, 0, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
    if ((alert_flags & STORAGE_ALERT_EEPROM_80) != 0U) {
        push_clip("rong_liang_jing_gao.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    }

    if (!sd_media_is_mounted()) {
        push_clip("sd_ka_wei_cha_ru.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
    } else {
        push_clip("sd_ka.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
        n += sr_dispatcher_build_number_clips((float)sd_free_mb, 0, &clips[n], TTS_MAX_CLIP_SEQUENCE - n);
        push_clip("zhao_zi_jie.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
        if ((alert_flags & STORAGE_ALERT_SD_FULL) != 0U) {
            push_clip("yi_man.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
        } else if ((alert_flags & STORAGE_ALERT_SD_LOW) != 0U) {
            push_clip("kong_jian_bu_zu.wav", clips, TTS_MAX_CLIP_SEQUENCE, &n);
        }
    }

    return play_clip_sequence(clips, n);
}
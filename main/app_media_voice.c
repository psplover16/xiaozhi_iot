#include "app_bridges.h"
#include "sr_engine.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "board_bsp.h"
#include "sd_media.h"
#include "oled_ui.h"
#include "audio_engine.h"
#include "net_manager.h"

static const char *TAG = "app_voice";

#define SD_CACHE_DIR            "/sdcard/cache"
#define SD_TTS_REPLY_MP3        "/sdcard/cache/reply.mp3"
#define SD_TTS_REPLY_WAV        "/sdcard/cache/reply.wav"
#define SD_WAKE_WAV_PATH        "/sdcard/tts_offline/wake.wav"
#define SD_DEFAULT_MUSIC_MP3    "/sdcard/music/daoxiang.mp3"
#define SD_DEFAULT_MUSIC_WAV    "/sdcard/music/daoxiang.wav"
#define SD_VOICE_MUTEX_WAIT_MS  1000U
#define AUDIO_RX_SAMPLE_RATE_HZ 16000U

static bool s_is_muted = false;

void app_voice_on_audio_event(audio_event_t event, size_t pcm_bytes)
{
    if (event != AUDIO_EVENT_SPEECH_END || pcm_bytes == 0U) {
        return;
    }

    const int16_t *pcm_ptr = NULL;
    size_t actual_bytes = 0U;
    if (audio_engine_get_recorded_pcm(&pcm_ptr, &actual_bytes) == ESP_OK &&
        pcm_ptr != NULL && actual_bytes > 0U) {
        esp_err_t err = net_manager_submit_speech(pcm_ptr, actual_bytes,
                                                  AUDIO_RX_SAMPLE_RATE_HZ);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "投遞語音至 Core 0 失敗: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t app_voice_on_tts_ready(const uint8_t *audio_data, size_t audio_len,
                                 const char *content_type, void *ctx)
{
    (void)ctx;
    if (audio_data == NULL || audio_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_media_is_mounted()) {
        ESP_LOGW(TAG, "SD 卡未掛載，無法暫存雲端 TTS 回覆音訊");
        return ESP_ERR_INVALID_STATE;
    }

    bool is_wav = (content_type != NULL && strstr(content_type, "wav") != NULL);
    const char *target_path = is_wav ? SD_TTS_REPLY_WAV : SD_TTS_REPLY_MP3;

    SemaphoreHandle_t sd_mtx = board_get_sd_mutex();
    if (sd_mtx == NULL ||
        xSemaphoreTake(sd_mtx, pdMS_TO_TICKS(SD_VOICE_MUTEX_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    struct stat st;
    if (stat(SD_CACHE_DIR, &st) != 0) {
        (void)mkdir(SD_CACHE_DIR, 0775);
    }

    FILE *fp = fopen(target_path, "wb");
    if (fp == NULL) {
        xSemaphoreGive(sd_mtx);
        ESP_LOGE(TAG, "開啟暫存檔失敗: %s", target_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(audio_data, 1U, audio_len, fp);
    (void)fflush(fp);
    (void)fsync(fileno(fp));
    fclose(fp);
    xSemaphoreGive(sd_mtx);

    if (written != audio_len) {
        ESP_LOGE(TAG, "寫入 TTS 暫存檔不完整 (%u / %u)",
                 (unsigned)written, (unsigned)audio_len);
        return ESP_FAIL;
    }

    return audio_engine_play_file(target_path);
}

esp_err_t app_voice_on_play_file(const char *filepath, void *ctx)
{
    (void)ctx;
    if (filepath == NULL || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_engine_play_file(filepath);
}

static void play_default_local_music(void)
{
    if (!sd_media_is_mounted()) {
        ESP_LOGW(TAG, "SD 卡未掛載，無法播放本機音樂");
        return;
    }

    struct stat st;
    if (stat(SD_DEFAULT_MUSIC_MP3, &st) == 0) {
        (void)audio_engine_play_file(SD_DEFAULT_MUSIC_MP3);
    } else if (stat(SD_DEFAULT_MUSIC_WAV, &st) == 0) {
        (void)audio_engine_play_file(SD_DEFAULT_MUSIC_WAV);
    } else {
        ESP_LOGW(TAG, "找不到預設音樂檔 (%s 或 %s)",
                 SD_DEFAULT_MUSIC_MP3, SD_DEFAULT_MUSIC_WAV);
    }
}

void app_voice_on_command(voice_cmd_id_t cmd)
{
    switch (cmd) {
        case VOICE_CMD_WIFI_ON:
        case VOICE_CMD_STA_MODE:
            (void)net_manager_set_mode(NET_MODE_ONLINE);
            break;

        case VOICE_CMD_WIFI_OFF:
            (void)net_manager_set_mode(NET_MODE_OFFLINE);
            break;

        case VOICE_CMD_AP_MODE:
            (void)net_manager_set_mode(NET_MODE_WEB_CFG);
            break;

        case VOICE_CMD_REPORT_ENV: {
            env_stats_t stats = {0};
            if (app_storage_get_latest_snapshot(NULL, &stats)) {
                (void)audio_engine_speak_env_stats(&stats);
            } else {
                ESP_LOGW(TAG, "尚無有效環境統計快照可供語音播報");
            }
            break;
        }

        case VOICE_CMD_PLAY_MUSIC:
            play_default_local_music();
            break;

        case VOICE_CMD_STOP_MUSIC:
            (void)audio_engine_stop_playback();
            break;

        case VOICE_CMD_WAKEUP:
            if (sd_media_is_mounted()) {
                (void)audio_engine_play_file(SD_WAKE_WAV_PATH);
            }
            break;

        case VOICE_CMD_TIME_NOW: {
            time_t now = time(NULL);
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            ESP_LOGI(TAG, "目前本地時間: %04d-%02d-%02d %02d:%02d:%02d",
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            (void)sr_dispatcher_speak_time((uint8_t)tm_info.tm_hour, (uint8_t)tm_info.tm_min);
            break;
        }

        case VOICE_CMD_VOL_UP: {
            uint8_t vol = audio_engine_adjust_volume(10);
            ESP_LOGI(TAG, "音量調升至 %u%%", (unsigned)vol);
            break;
        }

        case VOICE_CMD_VOL_DOWN: {
            uint8_t vol = audio_engine_adjust_volume(-10);
            ESP_LOGI(TAG, "音量調降至 %u%%", (unsigned)vol);
            break;
        }

        case VOICE_CMD_MUTE:
            s_is_muted = !s_is_muted;
            (void)audio_engine_set_mute(s_is_muted);
            ESP_LOGI(TAG, "靜音狀態切換為: %s", s_is_muted ? "靜音" : "取消靜音");
            break;

        case VOICE_CMD_NONE:
        default:
            break;
    }
}
#include "app_bridges.h"
#include "sr_engine.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
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

static atomic_bool   s_is_muted         = false;
static atomic_bool   s_is_music_playing = false;
static atomic_bool   s_is_speaking      = false;
static portMUX_TYPE  s_err_spinlock     = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      s_err_seq          = 0U;
static char          s_last_err[64]     = {0};

static void record_async_error(const char *msg)
{
    if (msg == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_err_spinlock);
    s_err_seq++;
    strncpy(s_last_err, msg, sizeof(s_last_err) - 1U);
    s_last_err[sizeof(s_last_err) - 1U] = '\0';
    taskEXIT_CRITICAL(&s_err_spinlock);
    ESP_LOGW(TAG, "非同步控制錯誤 (#%lu): %s", (unsigned long)s_err_seq, msg);
}

bool app_voice_is_audio_busy(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_is_music_playing) || atomic_load(&s_is_speaking);
}

esp_err_t app_voice_get_sys_status(sys_status_snapshot_t *out_status, void *ctx)
{
    (void)ctx;
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(sys_status_snapshot_t));

    sensor_record_t rec = {0};
    env_stats_t stats = {0};
    if (app_storage_get_latest_snapshot(&rec, &stats)) {
        out_status->sensor_valid = true;
        out_status->temp_c = (rec.temperature != INVALID_TEMP_VALUE)
                                 ? ((float)rec.temperature / 10.0f)
                                 : stats.temp_avg;
        out_status->humi_pct = (rec.humidity != INVALID_HUMI_VALUE)
                                   ? ((float)rec.humidity / 10.0f)
                                   : stats.humi_avg;
        out_status->lux = (rec.lux > 0xFFFFU) ? 0xFFFFU : (uint16_t)rec.lux;
        out_status->sensor_ts = rec.timestamp;
    }

    out_status->volume_pct       = audio_engine_adjust_volume(0);
    out_status->is_muted         = atomic_load(&s_is_muted);
    out_status->is_music_playing = atomic_load(&s_is_music_playing);
    out_status->is_audio_busy    = app_voice_is_audio_busy(NULL);

    taskENTER_CRITICAL(&s_err_spinlock);
    out_status->err_seq = s_err_seq;
    memcpy(out_status->last_err, s_last_err, sizeof(out_status->last_err));
    taskEXIT_CRITICAL(&s_err_spinlock);

    return ESP_OK;
}

esp_err_t app_voice_exec_ctrl_cmd(ctrl_cmd_id_t cmd, void *ctx)
{
    (void)ctx;
    voice_cmd_id_t mapped = VOICE_CMD_NONE;

    switch (cmd) {
        case CTRL_CMD_VOL_UP:
            mapped = VOICE_CMD_VOL_UP;
            break;
        case CTRL_CMD_VOL_DOWN:
            mapped = VOICE_CMD_VOL_DOWN;
            break;
        case CTRL_CMD_MUTE_TOGGLE:
            mapped = VOICE_CMD_MUTE;
            break;
        case CTRL_CMD_MUSIC_TOGGLE:
            mapped = atomic_load(&s_is_music_playing)
                         ? VOICE_CMD_STOP_MUSIC
                         : VOICE_CMD_PLAY_MUSIC;
            break;
        case CTRL_CMD_SPEAK_ENV:
            mapped = VOICE_CMD_REPORT_ENV;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    audio_engine_inject_cmd(mapped);
    return ESP_OK;
}

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
        record_async_error("SD 卡未掛載，無法暫存雲端語音");
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
        record_async_error("開啟 TTS 暫存檔失敗");
        return ESP_FAIL;
    }

    size_t written = fwrite(audio_data, 1U, audio_len, fp);
    (void)fflush(fp);
    (void)fsync(fileno(fp));
    fclose(fp);
    xSemaphoreGive(sd_mtx);

    if (written != audio_len) {
        record_async_error("寫入 TTS 暫存檔不完整");
        return ESP_FAIL;
    }

    atomic_store(&s_is_music_playing, false);
    atomic_store(&s_is_speaking, true);
    esp_err_t err = audio_engine_play_file(target_path);
    atomic_store(&s_is_speaking, false);
    return err;
}

esp_err_t app_voice_on_play_file(const char *filepath, void *ctx)
{
    (void)ctx;
    if (filepath == NULL || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&s_is_music_playing, false);
    atomic_store(&s_is_speaking, true);
    esp_err_t err = audio_engine_play_file(filepath);
    atomic_store(&s_is_speaking, false);
    return err;
}

static void play_default_local_music(void)
{
    if (!sd_media_is_mounted()) {
        atomic_store(&s_is_music_playing, false);
        record_async_error("SD 卡未掛載，無法播放音樂");
        return;
    }

    const char *music_path = NULL;
    struct stat st;
    if (stat(SD_DEFAULT_MUSIC_MP3, &st) == 0) {
        music_path = SD_DEFAULT_MUSIC_MP3;
    } else if (stat(SD_DEFAULT_MUSIC_WAV, &st) == 0) {
        music_path = SD_DEFAULT_MUSIC_WAV;
    } else {
        atomic_store(&s_is_music_playing, false);
        record_async_error("SD 卡找不到預設音樂檔");
        return;
    }

    if (app_voice_is_audio_busy(NULL)) {
        (void)audio_engine_stop_playback();
    }
    if (audio_engine_play_file(music_path) == ESP_OK) {
        atomic_store(&s_is_music_playing, true);
    } else {
        atomic_store(&s_is_music_playing, false);
        record_async_error("音樂解碼器啟動失敗");
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
            if (atomic_load(&s_is_music_playing)) {
                (void)audio_engine_stop_playback();
                atomic_store(&s_is_music_playing, false);
            }
            env_stats_t stats = {0};
            if (!app_storage_get_latest_snapshot(NULL, &stats)) {
                record_async_error("尚無有效環境統計快照");
                break;
            }
            atomic_store(&s_is_speaking, true);
            esp_err_t err = audio_engine_speak_env_stats(&stats);
            atomic_store(&s_is_speaking, false);
            if (err != ESP_OK) {
                record_async_error("環境語音播報失敗（請檢查 SD 卡）");
            }
            break;
        }

        case VOICE_CMD_PLAY_MUSIC:
            play_default_local_music();
            break;

        case VOICE_CMD_STOP_MUSIC:
            (void)audio_engine_stop_playback();
            atomic_store(&s_is_music_playing, false);
            atomic_store(&s_is_speaking, false);
            break;

        case VOICE_CMD_WAKEUP:
            if (sd_media_is_mounted()) {
                atomic_store(&s_is_music_playing, false);
                atomic_store(&s_is_speaking, true);
                (void)audio_engine_play_file(SD_WAKE_WAV_PATH);
                atomic_store(&s_is_speaking, false);
            }
            break;

        case VOICE_CMD_TIME_NOW: {
            time_t now = time(NULL);
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            atomic_store(&s_is_speaking, true);
            (void)sr_dispatcher_speak_time((uint8_t)tm_info.tm_hour, (uint8_t)tm_info.tm_min);
            atomic_store(&s_is_speaking, false);
            break;
        }

        case VOICE_CMD_VOL_UP:
            (void)audio_engine_adjust_volume(10);
            break;

        case VOICE_CMD_VOL_DOWN:
            (void)audio_engine_adjust_volume(-10);
            break;

        case VOICE_CMD_MUTE: {
            bool next_mute = !atomic_load(&s_is_muted);
            atomic_store(&s_is_muted, next_mute);
            (void)audio_engine_set_mute(next_mute);
            break;
        }

        case VOICE_CMD_NONE:
        default:
            break;
    }
}
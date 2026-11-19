#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_CMD_NONE = 0,
    VOICE_CMD_WIFI_ON,
    VOICE_CMD_WIFI_OFF,
    VOICE_CMD_REPORT_ENV,
    VOICE_CMD_PLAY_MUSIC,
    VOICE_CMD_STOP_MUSIC,
    /* [Phase 8 擴充] 離線語音控制指令 */
    VOICE_CMD_WAKEUP,
    VOICE_CMD_TIME_NOW,
    VOICE_CMD_VOL_UP,
    VOICE_CMD_VOL_DOWN,
    VOICE_CMD_MUTE,
    VOICE_CMD_AP_MODE,
    VOICE_CMD_STA_MODE
} voice_cmd_id_t;

typedef void (*voice_cmd_callback_t)(voice_cmd_id_t cmd);

esp_err_t audio_engine_init(voice_cmd_callback_t cb);
esp_err_t audio_engine_start_task(void); // Pin to Core 1 (最高優先權)
void      audio_engine_inject_cmd(voice_cmd_id_t cmd); // 供測試或按鍵/UART 模擬觸發
esp_err_t audio_engine_speak_env_stats(const env_stats_t *stats); // 呼叫 sd_media 拼接播放數字 WAV

/* ============================================================================
 * [T-06-01 / T-06-02 / Phase 7-8 增量擴充介面]
 * ============================================================================ */

typedef enum {
    AUDIO_EVENT_SPEECH_START = 0,
    AUDIO_EVENT_SPEECH_END
} audio_event_t;

typedef void (*audio_event_callback_t)(audio_event_t event, size_t pcm_bytes);
typedef void (*audio_pcm_tap_cb_t)(const int16_t *stereo_pcm, size_t frame_count);

void      audio_engine_register_event_cb(audio_event_callback_t cb);
void      audio_engine_register_pcm_tap(audio_pcm_tap_cb_t tap_cb);
esp_err_t audio_engine_get_recorded_pcm(const int16_t **out_pcm, size_t *out_bytes);

/* MAX98357A 播放、動態取樣率重配置與軟體音量控制 */
esp_err_t audio_engine_play_file(const char *filepath);
esp_err_t audio_engine_play_file_sync(const char *filepath, uint32_t timeout_ms);
esp_err_t audio_engine_stop_playback(void);
esp_err_t audio_engine_reconfig_tx_rate(uint32_t sample_rate);
esp_err_t audio_engine_set_volume(uint8_t percent);
uint8_t   audio_engine_adjust_volume(int8_t delta_percent);
esp_err_t audio_engine_set_mute(bool mute);
void      audio_engine_apply_volume_pcm(int16_t *pcm, size_t sample_count);

#ifdef __cplusplus
}
#endif
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
    VOICE_CMD_STOP_MUSIC
} voice_cmd_id_t;

typedef void (*voice_cmd_callback_t)(voice_cmd_id_t cmd);

esp_err_t audio_engine_init(voice_cmd_callback_t cb);
esp_err_t audio_engine_start_task(void); // Pin to Core 1 (最高優先權)
void      audio_engine_inject_cmd(voice_cmd_id_t cmd); // 供測試或按鍵/UART 模擬觸發
esp_err_t audio_engine_speak_env_stats(const env_stats_t *stats); // 呼叫 sd_media 拼接播放數字 WAV

#ifdef __cplusplus
}
#endif

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
    AUDIO_FMT_UNKNOWN = 0,
    AUDIO_FMT_WAV     = 1,
    AUDIO_FMT_MP3     = 2
} audio_format_t;

esp_err_t sd_media_init(void); // 掛載 /sdcard（不重複初始化 SPI2_HOST）
bool      sd_media_is_mounted(void);
uint32_t  sd_media_get_free_mb(void);

// 音訊播放（支援 .wav 與 .mp3，內部動態切換 I2S 時脈並分段讓鎖 <= 4096B）
audio_format_t sd_media_detect_format(const char *filepath);
esp_err_t sd_media_play_audio(const char *filepath);
void      sd_media_stop_audio(void);
bool      sd_media_is_playing(void);

// 離線感測資料附加寫入（"ab" 模式 + fsync + CRC32 回讀驗證）
esp_err_t sd_media_append_offline_records(uint32_t date_yyyymmdd, const sensor_record_t *records, size_t count);

#ifdef __cplusplus
}
#endif

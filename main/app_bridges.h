#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "system_types.h"
#include "audio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 儲存與補傳協調器介面（實作於 app_storage_sync.c）
 * ============================================================================ */
void      app_storage_init_bridge(void);
void      app_storage_on_sensor_record(const sensor_record_t *rec);
esp_err_t app_storage_on_offline_record(const sensor_record_t *rec, void *ctx);
void      app_storage_get_status(uint16_t *out_eeprom_cnt, bool *out_sd_mounted,
                                 uint32_t *out_sd_free_mb, void *ctx);
esp_err_t app_storage_eeprom_flush_to_sd(const sensor_record_t *records, size_t count);
void      app_storage_register_sync_sources(void);
bool      app_storage_get_latest_snapshot(sensor_record_t *out_rec, env_stats_t *out_stats);

/* ============================================================================
 * 語音與命令協調器介面（實作於 app_media_voice.c）
 * ============================================================================ */
void      app_voice_on_audio_event(audio_event_t event, size_t pcm_bytes);
void      app_voice_on_command(voice_cmd_id_t cmd);
esp_err_t app_voice_on_tts_ready(const uint8_t *audio_data, size_t audio_len,
                                 const char *content_type, void *ctx);
esp_err_t app_voice_on_play_file(const char *filepath, void *ctx);

#ifdef __cplusplus
}
#endif
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_MAX_SOURCES           4U
#define SYNC_BATCH_MAX_RECORDS     50U
#define VOICE_OFFLINE_HINT_DEFAULT "/sdcard/audio/offline_hint.mp3"

/* Server 批次上傳介面（供 sync_engine 依賴反轉與 TDD Mock 測試） */
typedef esp_err_t (*sync_upload_batch_fn)(const sensor_record_t *batch, size_t count);

/* 抽象離線儲存來源介面（按註冊順序 0..N-1 由最舊到較新依序排空） */
typedef struct {
    const char *name;
    esp_err_t (*peek_oldest_batch)(sensor_record_t *out_buf, size_t max_count,
                                   size_t *out_count, void *ctx);
    esp_err_t (*ack_oldest_batch)(size_t count, void *ctx);
    void *ctx;
} sync_source_t;

/* 跨模組零耦合事件、儲存與語音播放回呼介面（由上層 main.c 注入，可為 NULL） */
typedef struct {
    void      (*on_got_ip)(void *ctx);
    void      (*on_mode_changed)(net_mode_t mode, const char *info, void *ctx);
    esp_err_t (*on_offline_record)(const sensor_record_t *rec, void *ctx);
    void      (*get_storage_status)(uint16_t *out_eeprom_cnt, bool *out_sd_mounted,
                                    uint32_t *out_sd_free_mb, void *ctx);
    /* T-07-03: 雲端 TTS 回覆音訊就緒（PSRAM 緩衝）與本地音檔播報（離線提示/快取檔） */
    esp_err_t (*on_tts_audio_ready)(const uint8_t *audio_data, size_t audio_len,
                                    const char *content_type, void *ctx);
    esp_err_t (*on_play_audio_file)(const char *filepath, void *ctx);
    void *user_ctx;
} net_manager_hooks_t;

esp_err_t  net_manager_init(void);
void       net_manager_register_hooks(const net_manager_hooks_t *hooks);
esp_err_t  net_manager_set_mode(net_mode_t mode);
net_mode_t net_manager_get_mode(void);

/* 四級容災寫入入口：在線且歷史補傳完成時直傳 Server，否則自動轉入離線儲存 Hook */
esp_err_t  net_manager_submit_record(const sensor_record_t *rec);

/* T-07-03: Core 1 語音結束非阻塞投遞入口（轉交 Core 0 雲端對話或離線提示音播報） */
esp_err_t  net_manager_submit_speech(const int16_t *pcm_data, size_t pcm_bytes,
                                     uint32_t sample_rate_hz);

/* Web 設定驗證純邏輯（供 FR-09 與 Unity 單元測試直接呼叫） */
esp_err_t  web_config_validate(const sys_config_t *cfg, char *err_msg, size_t err_len);

/* 嚴格時間順序補傳引擎 */
void       sync_engine_clear_sources(void);
esp_err_t  sync_engine_register_source(const sync_source_t *source);
void       sync_engine_lock_live(void);
bool       sync_engine_is_live_unlocked(void);
esp_err_t  sync_engine_execute(sync_upload_batch_fn upload_fn);

/* 雲端語音端點組裝純邏輯（供 Unity 單元測試直接驗證） */
esp_err_t  voice_cloud_build_endpoint(const char *base_url, const char *subpath,
                                      char *out_url, size_t out_len);

#ifdef __cplusplus
}
#endif
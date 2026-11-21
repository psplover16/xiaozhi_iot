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
#define VOICE_CLOUD_CHANNELS       1U
#define VOICE_CLOUD_TIMEOUT_MS     3000U

/* T-OPT-03: 區網 Wi-Fi 細粒度連線狀態機 */
typedef enum {
    WIFI_CONN_STATE_OFF = 0,     /* 區網 Wi-Fi 關閉（WIFI_MODE_NULL） */
    WIFI_CONN_STATE_AP_SETUP,    /* SoftAP 熱點配網模式（192.168.4.1） */
    WIFI_CONN_STATE_CONNECTING,  /* STA 正在連線至路由器 */
    WIFI_CONN_STATE_CONNECTED,   /* STA 已取得有效 IP */
} wifi_conn_state_t;

/* T-OPT-05: 網頁遠端控制指令列舉（零耦合跨核心控制） */
typedef enum {
    CTRL_CMD_VOL_UP = 0,
    CTRL_CMD_VOL_DOWN,
    CTRL_CMD_MUTE_TOGGLE,
    CTRL_CMD_MUSIC_TOGGLE,
    CTRL_CMD_SPEAK_ENV,
} ctrl_cmd_id_t;

/* T-OPT-05: 系統即時狀態快照（供 Web 儀表板無阻塞輪詢） */
typedef struct {
    bool     sensor_valid;
    float    temp_c;
    float    humi_pct;
    uint16_t lux;
    uint32_t sensor_ts;
    uint8_t  volume_pct;
    bool     is_muted;
    bool     is_music_playing;
    bool     is_audio_busy;
    uint32_t err_seq;
    char     last_err[64];
} sys_status_snapshot_t;

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

/* 跨模組零耦合事件、儲存、狀態快照與控制回呼介面（由上層 main.c 注入，可為 NULL） */
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
    /* T-OPT-03 / T-OPT-05: 音訊忙碌查詢、系統即時快照與跨核心控制橋接 */
    bool      (*is_audio_busy)(void *ctx);
    esp_err_t (*get_sys_status)(sys_status_snapshot_t *out_status, void *ctx);
    esp_err_t (*exec_ctrl_cmd)(ctrl_cmd_id_t cmd, void *ctx);
    void *user_ctx;
} net_manager_hooks_t;

esp_err_t  net_manager_init(void);
void       net_manager_register_hooks(const net_manager_hooks_t *hooks);
esp_err_t  net_manager_set_mode(net_mode_t mode);
net_mode_t net_manager_get_mode(void);

/* T-OPT-03: 區網 Wi-Fi 與雲端伺服器雙獨立開關與連線狀態查詢 */
esp_err_t         net_manager_set_wifi_enabled(bool enabled);
bool              net_manager_is_wifi_enabled(void);
wifi_conn_state_t net_manager_get_wifi_state(void);
void              net_manager_set_cloud_enabled(bool enabled);
bool              net_manager_is_cloud_enabled(void);
void              net_manager_format_ip_display(const char *ip_str, bool cloud_en,
                                                char *out_buf, size_t out_len);

/* 四級容災寫入入口：在線且歷史補傳完成時直傳 Server，否則自動轉入離線儲存 Hook */
esp_err_t  net_manager_submit_record(const sensor_record_t *rec);

/* T-07-03: Core 1 語音結束非阻塞投遞入口（轉交 Core 0 雲端對話或離線提示音播報） */
esp_err_t  net_manager_submit_speech(const int16_t *pcm_data, size_t pcm_bytes,
                                     uint32_t sample_rate_hz);

/* T-OPT-02: Barge-in 即時中斷控制（< 50 ms 中止 HTTP 串流） */
void       net_manager_abort_voice_stream(void);
bool       net_manager_is_voice_stream_aborted(void);

/* Web 設定驗證純邏輯（供 FR-09 與 Unity 單元測試直接呼叫） */
esp_err_t  web_config_validate(const sys_config_t *cfg, char *err_msg, size_t err_len);

/* 嚴格時間順序補傳引擎 */
void       sync_engine_clear_sources(void);
esp_err_t  sync_engine_register_source(const sync_source_t *source);
void       sync_engine_lock_live(void);
bool       sync_engine_is_live_unlocked(void);
esp_err_t  sync_engine_execute(sync_upload_batch_fn upload_fn);

/* 雲端語音端點與串流標頭組裝純邏輯（供 Unity 單元測試直接驗證） */
esp_err_t  voice_cloud_build_endpoint(const char *base_url, const char *subpath,
                                      char *out_url, size_t out_len);
esp_err_t  voice_cloud_format_stream_headers(uint32_t sample_rate_hz,
                                             char *out_rate, size_t rate_len,
                                             char *out_channels, size_t ch_len,
                                             char *out_bits, size_t bits_len);

#ifdef __cplusplus
}
#endif
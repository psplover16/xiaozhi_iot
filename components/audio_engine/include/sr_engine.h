#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SR_STATE_IDLE        = 0,
    SR_STATE_WAKE_LISTEN = 1, // WakeNet 低功耗喚醒詞監聽中
    SR_STATE_CMD_LISTEN  = 2  // MultiNet 離線命令詞識別中（預設 6000ms 逾時）
} sr_listen_state_t;

typedef void (*sr_state_change_cb_t)(sr_listen_state_t new_state);

// 初始化 ESP-AFE、載入 3MB model 分區之 WakeNet 與 MultiNet 並註冊預設漢語拼音指令表
esp_err_t         sr_engine_init(voice_cmd_callback_t cmd_cb, sr_state_change_cb_t state_cb);
esp_err_t         sr_engine_start_tasks(void); // 啟動 Core 1 之 task_sr_feed 與 task_sr_detect
sr_listen_state_t sr_engine_get_state(void);

// 動態新增漢語拼音離線命令詞（綁定至 voice_cmd_id_t）
esp_err_t         sr_engine_add_pinyin_cmd(voice_cmd_id_t cmd_id, const char *pinyin_str);

// 離線語音播報輔助函式（將數值轉為 /sdcard/tts_offline/*.wav 序列並依序透過 sd_media 播報）
esp_err_t         sr_dispatcher_speak_temperature(float temp_c, int8_t trend);
esp_err_t         sr_dispatcher_speak_humidity(float humi_pct);
esp_err_t         sr_dispatcher_speak_lux(uint32_t lux);
esp_err_t         sr_dispatcher_speak_time(uint8_t hour, uint8_t minute);
esp_err_t         sr_dispatcher_speak_storage_status(uint8_t alert_flags, uint16_t eeprom_pct, uint32_t sd_free_mb);

// 純邏輯輔助函式（供 Phase 8 Unity 單元測試直接驗證音節序列展開）
size_t            sr_dispatcher_build_number_clips(float value, int decimal_places, char out_paths[][64], size_t max_clips);

#ifdef __cplusplus
}
#endif
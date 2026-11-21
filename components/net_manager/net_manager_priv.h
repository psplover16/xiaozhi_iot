#pragma once

#include "net_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* net_manager.c 內部共享狀態與 Hook 轉發介面 */
void      net_mgr_get_config_copy(sys_config_t *out_cfg);
esp_err_t net_mgr_save_config(const sys_config_t *new_cfg);
bool      net_mgr_is_sta_connected(void);
void      net_mgr_set_sta_connected(bool connected, const char *ip_str);
void      net_mgr_query_storage_status(uint16_t *out_eeprom_cnt,
                                       bool *out_sd_mounted,
                                       uint32_t *out_sd_free_mb);
void      net_mgr_notify_mode(net_mode_t mode, const char *info);
void      net_mgr_notify_got_ip(void);
void      net_mgr_notify_play_file(const char *filepath);
void      net_mgr_notify_tts_ready(const uint8_t *data, size_t len,
                                   const char *content_type);
bool      net_mgr_is_audio_busy(void);
esp_err_t net_mgr_get_sys_status(sys_status_snapshot_t *out_status);
esp_err_t net_mgr_exec_ctrl_cmd(ctrl_cmd_id_t cmd);
void      net_mgr_trigger_history_sync(void);
esp_err_t net_mgr_http_upload_batch(const sensor_record_t *batch, size_t count);

/* net_mode_ctrl.c 雙開關狀態機、1.5s 單一分界點按鍵、NVS 防抖與 mDNS 介面 */
void       net_mode_ctrl_init(bool force_ap_setup);
void       net_mode_ctrl_poll(void);
void       net_mode_ctrl_process_button(bool is_pressed, int64_t now_ms);
void       net_mode_ctrl_force_ap_setup(void);
void       net_mode_ctrl_on_sta_start(void);
void       net_mode_ctrl_on_sta_disconnected(void);
void       net_mode_ctrl_on_got_ip(const char *ip_str);
void       net_mode_ctrl_on_config_saved(const sys_config_t *cfg);
void       net_mode_ctrl_set_wifi_delayed(bool enabled, uint32_t delay_ms);
bool       net_mode_ctrl_is_ap_mode(void);
bool       net_mode_ctrl_is_cloud_active(void);
esp_err_t  net_mode_ctrl_set_legacy_mode(net_mode_t mode);
net_mode_t net_mode_ctrl_get_legacy_mode(void);

/* web_config.c HTTP 伺服器生命週期介面 */
esp_err_t web_config_httpd_start(void);
esp_err_t web_config_start(const sys_config_t *cfg);
void      web_config_stop(void);

/* voice_cloud.c 雲端語音任務初始化 */
esp_err_t voice_cloud_init(void);

#ifdef __cplusplus
}
#endif
#pragma once

#include "net_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void      net_mgr_get_config_copy(sys_config_t *out_cfg);
esp_err_t net_mgr_save_config(const sys_config_t *new_cfg);
bool      net_mgr_is_sta_connected(void);
void      net_mgr_query_storage_status(uint16_t *out_eeprom_cnt,
                                       bool *out_sd_mounted,
                                       uint32_t *out_sd_free_mb);
void      net_mgr_notify_mode(net_mode_t mode, const char *info);
void      net_mgr_notify_play_file(const char *filepath);
void      net_mgr_notify_tts_ready(const uint8_t *data, size_t len,
                                   const char *content_type);
esp_err_t net_mgr_http_upload_batch(const sensor_record_t *batch, size_t count);

esp_err_t web_config_start(const sys_config_t *cfg);
void      web_config_stop(void);

esp_err_t voice_cloud_init(void);

#ifdef __cplusplus
}
#endif
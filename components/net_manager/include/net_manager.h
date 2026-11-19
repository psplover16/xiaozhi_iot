#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Server 批次上傳介面（供 sync_engine 依賴反轉與 TDD Mock 測試）
typedef esp_err_t (*sync_upload_batch_fn)(const sensor_record_t *batch, size_t count);

esp_err_t  net_manager_init(void);
esp_err_t  net_manager_set_mode(net_mode_t mode);
net_mode_t net_manager_get_mode(void);

// Web 設定驗證純邏輯（供 FR-09 與 Unity 單元測試直接呼叫）
esp_err_t  web_config_validate(const sys_config_t *cfg, char *err_msg, size_t err_len);

// 嚴格時間順序補傳引擎
esp_err_t  sync_engine_execute(sync_upload_batch_fn upload_fn);

#ifdef __cplusplus
}
#endif

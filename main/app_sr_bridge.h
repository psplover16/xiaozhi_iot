/**
 * @file app_sr_bridge.h
 * @brief Layer-3 Asynchronous Bridge between ESP-SR (Core 1) and App Voice Dispatcher (Core 0)
 */

#ifndef APP_SR_BRIDGE_H
#define APP_SR_BRIDGE_H

#include "esp_err.h"
#include "sr_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化第三層語音佇列橋接器並啟動底層 ESP-SR 雙任務
 *
 * @return esp_err_t ESP_OK 表示啟動成功
 */
esp_err_t app_sr_bridge_init(void);

/**
 * @brief 手動投遞一筆模擬語音指令至橋接佇列（供整合測試使用）
 *
 * @param cmd_id 語音指令 ID
 * @return esp_err_t ESP_OK 表示投遞成功
 */
esp_err_t app_sr_bridge_post_cmd(voice_cmd_id_t cmd_id);

#ifdef __cplusplus
}
#endif

#endif /* APP_SR_BRIDGE_H */
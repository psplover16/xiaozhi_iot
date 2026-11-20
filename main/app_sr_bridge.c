/**
 * @file app_sr_bridge.c
 * @brief Layer-3 Non-Blocking Queue Bridge for Offline Speech Recognition
 */

#include "app_sr_bridge.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "app_bridges.h"

static const char *TAG = "app_sr_bridge";

#define SR_BRIDGE_QUEUE_LEN     8U
#define SR_BRIDGE_TASK_STACK    (5U * 1024U)
#define SR_BRIDGE_TASK_PRIO     4
#define SR_BRIDGE_CORE_ID       0

static QueueHandle_t s_cmd_queue   = NULL;
static bool          s_initialized = false;

static void on_sr_state_cb(sr_listen_state_t state)
{
    const char *state_str = (state == SR_STATE_WAKE_LISTEN) ? "WAKE_LISTEN" :
                            (state == SR_STATE_CMD_LISTEN)  ? "CMD_LISTEN"  : "IDLE";
    ESP_LOGI(TAG, "離線語音狀態切換 -> %s", state_str);
}

/* Core 1 task_sr_detect 回呼：僅做零阻塞投遞，避免 SD 播報阻塞喚醒狀態機 */
static void on_voice_cmd_cb(voice_cmd_id_t cmd)
{
    if (s_cmd_queue == NULL || cmd == VOICE_CMD_NONE) {
        return;
    }
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "語音指令佇列已滿，丟棄指令 id=%d", (int)cmd);
    }
}

static void task_sr_bridge(void *arg)
{
    (void)arg;
    voice_cmd_id_t cmd = VOICE_CMD_NONE;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            app_voice_on_command(cmd);
        }
    }
}

esp_err_t app_sr_bridge_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_cmd_queue = xQueueCreate(SR_BRIDGE_QUEUE_LEN, sizeof(voice_cmd_id_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(task_sr_bridge, "task_sr_bridge", SR_BRIDGE_TASK_STACK,
                                NULL, SR_BRIDGE_TASK_PRIO, NULL, SR_BRIDGE_CORE_ID) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sr_engine_init(on_voice_cmd_cb, on_sr_state_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sr_engine_init 失敗: %s", esp_err_to_name(err));
        return err;
    }

    err = sr_engine_start_tasks();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sr_engine_start_tasks 失敗: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "第三層語音協調橋接器啟動完成 (Core 1 SR -> Core 0 Dispatcher)");
    return ESP_OK;
}

esp_err_t app_sr_bridge_post_cmd(voice_cmd_id_t cmd_id)
{
    if (!s_initialized || s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xQueueSend(s_cmd_queue, &cmd_id, pdMS_TO_TICKS(50)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
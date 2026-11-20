/**
 * @file sr_engine.c
 * @brief Offline Speech Recognition Engine (ESP-AFE Dual-Mic + WakeNet + MultiNet)
 */

#include "sr_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

static const char *TAG = "sr_engine";

#define SR_CMD_LISTEN_TIMEOUT_MS  6000U
#define SR_PCM_RINGBUF_BYTES      (32U * 1024U)
#define SR_FEED_TASK_STACK        (6U * 1024U)
#define SR_DETECT_TASK_STACK      (8U * 1024U)
#define SR_FEED_TASK_PRIO         6
#define SR_DETECT_TASK_PRIO       6
#define SR_CORE_ID                1
#define MAX_DYNAMIC_CMDS          64

typedef struct {
    voice_cmd_id_t cmd_id;
    const char    *pinyin;
} default_pinyin_cmd_t;

/* 18 條標準漢語拼音離線命令詞表 */
static const default_pinyin_cmd_t s_default_cmds[] = {
    { VOICE_CMD_WIFI_ON,    "da kai wang luo"        },
    { VOICE_CMD_WIFI_ON,    "da kai wu xian wang"    },
    { VOICE_CMD_WIFI_OFF,   "guan bi wang luo"       },
    { VOICE_CMD_WIFI_OFF,   "duan kai wang luo"      },
    { VOICE_CMD_AP_MODE,    "da kai pei wang"        },
    { VOICE_CMD_AP_MODE,    "jin ru pei wang mo shi" },
    { VOICE_CMD_STA_MODE,   "lian jie wang luo"      },
    { VOICE_CMD_REPORT_ENV, "bo bao huan jing"       },
    { VOICE_CMD_REPORT_ENV, "wen shi du duo shao"    },
    { VOICE_CMD_REPORT_ENV, "dang qian wen du"       },
    { VOICE_CMD_REPORT_ENV, "guang zhao duo shao"    },
    { VOICE_CMD_TIME_NOW,   "xian zai ji dian"       },
    { VOICE_CMD_TIME_NOW,   "bao shi"                },
    { VOICE_CMD_PLAY_MUSIC, "bo fang yin yue"        },
    { VOICE_CMD_STOP_MUSIC, "ting zhi bo fang"       },
    { VOICE_CMD_VOL_UP,     "sheng yin da yi dian"   },
    { VOICE_CMD_VOL_DOWN,   "sheng yin xiao yi dian" },
    { VOICE_CMD_MUTE,       "jing yin"               },
};

static srmodel_list_t            *s_models        = NULL;
static const esp_afe_sr_iface_t  *s_afe_handle    = NULL;
static esp_afe_sr_data_t         *s_afe_data      = NULL;
static const esp_mn_iface_t      *s_multinet      = NULL;
static model_iface_data_t        *s_mn_data       = NULL;

static RingbufHandle_t            s_pcm_rb        = NULL;
static StaticRingbuffer_t        *s_rb_struct     = NULL;
static uint8_t                   *s_rb_storage    = NULL;

static voice_cmd_callback_t       s_cmd_cb        = NULL;
static sr_state_change_cb_t       s_state_cb      = NULL;
static volatile sr_listen_state_t s_state         = SR_STATE_IDLE;
static volatile int64_t           s_cmd_start_ms  = 0;

static voice_cmd_id_t             s_id_to_cmd[MAX_DYNAMIC_CMDS + 1];
static int                        s_next_cmd_id   = 1;
static bool                       s_initialized   = false;
static bool                       s_tasks_started = false;

static void set_sr_state(sr_listen_state_t next)
{
    if (s_state != next) {
        s_state = next;
        if (s_state_cb != NULL) {
            s_state_cb(next);
        }
    }
}

static void on_audio_pcm_tap(const int16_t *stereo_pcm, size_t frame_count)
{
    if (s_pcm_rb == NULL || stereo_pcm == NULL || frame_count == 0U) {
        return;
    }
    size_t bytes = frame_count * 2U * sizeof(int16_t);
    (void)xRingbufferSend(s_pcm_rb, stereo_pcm, bytes, 0);
}

static esp_err_t add_pinyin_cmd_internal(voice_cmd_id_t cmd_id, const char *pinyin_str, bool update_now)
{
    if (s_multinet == NULL || s_mn_data == NULL || pinyin_str == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_next_cmd_id > MAX_DYNAMIC_CMDS) {
        return ESP_ERR_NO_MEM;
    }

    int assigned_id = s_next_cmd_id;
    esp_err_t err = esp_mn_commands_add(assigned_id, (char *)pinyin_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "註冊拼音指令失敗 [%s]: %s", pinyin_str, esp_err_to_name(err));
        return err;
    }

    s_id_to_cmd[assigned_id] = cmd_id;
    s_next_cmd_id++;
    if (update_now) {
        esp_mn_commands_update();
    }
    return ESP_OK;
}

esp_err_t sr_engine_add_pinyin_cmd(voice_cmd_id_t cmd_id, const char *pinyin_str)
{
    return add_pinyin_cmd_internal(cmd_id, pinyin_str, true);
}

esp_err_t sr_engine_init(voice_cmd_callback_t cmd_cb, sr_state_change_cb_t state_cb)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_cmd_cb   = cmd_cb;
    s_state_cb = state_cb;

    s_models = esp_srmodel_init("model");
    if (s_models == NULL || s_models->num <= 0) {
        ESP_LOGE(TAG, "無法從 'model' 分區載入語音模型");
        return ESP_ERR_NOT_FOUND;
    }

    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (wn_name == NULL || mn_name == NULL) {
        ESP_LOGE(TAG, "模型缺失 (WakeNet=%s, MultiNet=%s)",
                 wn_name ? wn_name : "NULL", mn_name ? mn_name : "NULL");
        return ESP_ERR_NOT_FOUND;
    }

    afe_config_t *afe_cfg = afe_config_init("MM", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    afe_cfg->wakenet_init           = true;
    afe_cfg->aec_init               = false;
    afe_cfg->se_init                = true;
    afe_cfg->vad_init               = true;
    afe_cfg->memory_alloc_mode      = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->afe_perferred_core     = SR_CORE_ID;
    afe_cfg->afe_perferred_priority = 5;

    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    s_afe_data   = (s_afe_handle != NULL) ? s_afe_handle->create_from_config(afe_cfg) : NULL;
    afe_config_free(afe_cfg);
    if (s_afe_data == NULL) {
        return ESP_FAIL;
    }

    s_multinet = esp_mn_handle_from_name(mn_name);
    s_mn_data  = (s_multinet != NULL) ? s_multinet->create(mn_name, SR_CMD_LISTEN_TIMEOUT_MS) : NULL;
    if (s_mn_data == NULL) {
        return ESP_FAIL;
    }

    esp_mn_commands_alloc(s_multinet, s_mn_data);
    esp_mn_commands_clear();
    memset(s_id_to_cmd, 0, sizeof(s_id_to_cmd));
    s_next_cmd_id = 1;

    size_t cmd_cnt = sizeof(s_default_cmds) / sizeof(s_default_cmds[0]);
    for (size_t i = 0; i < cmd_cnt; i++) {
        (void)add_pinyin_cmd_internal(s_default_cmds[i].cmd_id, s_default_cmds[i].pinyin, false);
    }
    esp_mn_commands_update();

    s_rb_struct  = heap_caps_calloc(1, sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_rb_storage = heap_caps_malloc(SR_PCM_RINGBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rb_struct == NULL || s_rb_storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_pcm_rb = xRingbufferCreateStatic(SR_PCM_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       s_rb_storage, s_rb_struct);
    audio_engine_register_pcm_tap(on_audio_pcm_tap);

    s_initialized = true;
    set_sr_state(SR_STATE_WAKE_LISTEN);
    ESP_LOGI(TAG, "ESP-SR 就緒 (WN=%s, MN=%s, 指令數=%d)", wn_name, mn_name, s_next_cmd_id - 1);
    return ESP_OK;
}

static void return_to_wake_listen(void)
{
    if (s_afe_handle != NULL && s_afe_data != NULL) {
        s_afe_handle->enable_wakenet(s_afe_data);
    }
    set_sr_state(SR_STATE_WAKE_LISTEN);
}

bool sr_engine_poll_timeout(void)
{
    if (s_state == SR_STATE_CMD_LISTEN) {
        if (((esp_timer_get_time() / 1000LL) - s_cmd_start_ms) >= (int64_t)SR_CMD_LISTEN_TIMEOUT_MS) {
            return_to_wake_listen();
            return true;
        }
    }
    return false;
}

static void task_sr_feed(void *arg)
{
    (void)arg;
    const size_t chunk_bytes = (size_t)s_afe_handle->get_feed_chunksize(s_afe_data) *
                               (size_t)s_afe_handle->get_feed_channel_num(s_afe_data) * sizeof(int16_t);
    uint8_t *feed_buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (feed_buf == NULL) {
        vTaskDelete(NULL);
        return;
    }

    size_t filled = 0U;
    while (1) {
        size_t item_size = 0U;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(
            s_pcm_rb, &item_size, pdMS_TO_TICKS(50), chunk_bytes - filled);
        if (item != NULL && item_size > 0U) {
            memcpy(feed_buf + filled, item, item_size);
            vRingbufferReturnItem(s_pcm_rb, item);
            filled += item_size;
            if (filled >= chunk_bytes) {
                s_afe_handle->feed(s_afe_data, (const int16_t *)feed_buf);
                filled = 0U;
            }
        }
    }
}

static void task_sr_detect(void *arg)
{
    (void)arg;
    while (1) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (s_state == SR_STATE_WAKE_LISTEN) {
            if (res->wakeup_state == WAKENET_DETECTED) {
                s_afe_handle->disable_wakenet(s_afe_data);
                s_multinet->clean(s_mn_data);
                s_cmd_start_ms = esp_timer_get_time() / 1000LL;
                set_sr_state(SR_STATE_CMD_LISTEN);
                if (s_cmd_cb != NULL) {
                    s_cmd_cb(VOICE_CMD_WAKEUP);
                }
            }
            continue;
        }

        if (s_state == SR_STATE_CMD_LISTEN) {
            if (sr_engine_poll_timeout()) {
                ESP_LOGI(TAG, "離線命令視窗逾時 (6000ms)，退回喚醒監聽");
                continue;
            }

            esp_mn_state_t mn_state = s_multinet->detect(s_mn_data, res->data);
            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_res = s_multinet->get_results(s_mn_data);
                if (mn_res != NULL && mn_res->num > 0) {
                    int cmd_idx = mn_res->command_id[0];
                    if (cmd_idx >= 1 && cmd_idx <= MAX_DYNAMIC_CMDS &&
                        s_id_to_cmd[cmd_idx] != VOICE_CMD_NONE && s_cmd_cb != NULL) {
                        s_cmd_cb(s_id_to_cmd[cmd_idx]);
                    }
                }
                return_to_wake_listen();
            } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                return_to_wake_listen();
            }
        }
    }
}

esp_err_t sr_engine_start_tasks(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tasks_started) {
        return ESP_OK;
    }

    size_t drain_sz = 0U;
    void *drain_ptr = NULL;
    while ((drain_ptr = xRingbufferReceiveUpTo(s_pcm_rb, &drain_sz, 0, SR_PCM_RINGBUF_BYTES)) != NULL) {
        vRingbufferReturnItem(s_pcm_rb, drain_ptr);
    }

    if (xTaskCreatePinnedToCore(task_sr_feed, "task_sr_feed", SR_FEED_TASK_STACK,
                                NULL, SR_FEED_TASK_PRIO, NULL, SR_CORE_ID) != pdPASS ||
        xTaskCreatePinnedToCore(task_sr_detect, "task_sr_detect", SR_DETECT_TASK_STACK,
                                NULL, SR_DETECT_TASK_PRIO, NULL, SR_CORE_ID) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_tasks_started = true;
    return ESP_OK;
}

sr_listen_state_t sr_engine_get_state(void)
{
    return s_state;
}

size_t sr_engine_get_registered_cmd_count(void)
{
    return (s_next_cmd_id > 1) ? (size_t)(s_next_cmd_id - 1) : 0U;
}

voice_cmd_id_t sr_engine_get_mapped_cmd(int mn_cmd_id)
{
    if (mn_cmd_id < 1 || mn_cmd_id >= s_next_cmd_id || mn_cmd_id > MAX_DYNAMIC_CMDS) {
        return VOICE_CMD_NONE;
    }
    return s_id_to_cmd[mn_cmd_id];
}

void sr_engine_set_state_for_test(sr_listen_state_t state)
{
    if (state == SR_STATE_CMD_LISTEN) {
        s_cmd_start_ms = esp_timer_get_time() / 1000LL;
    }
    set_sr_state(state);
}
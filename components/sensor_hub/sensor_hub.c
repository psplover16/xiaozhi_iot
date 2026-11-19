#include "sensor_hub.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_sntp.h"
#include "esp_log.h"

#include "board_config.h"
#include "system_types.h"
#include "board_bsp.h"
#include "env_sensors_drv.h"

static const char *TAG = "sensor_hub";

#define SENSOR_POLL_INTERVAL_MS     2000U
#define SENSOR_POLL_TASK_STACK      4096U
#define SENSOR_POLL_TASK_PRIO       5U
#define SENSOR_POLL_TASK_CORE       1
#define SNTP_SYNC_TIMEOUT_MS        10000U
#define SNTP_SYNC_POLL_STEP_MS      200U
#define VALID_EPOCH_THRESHOLD       1704067200 /* 2024-01-01 00:00:00 UTC */

static SemaphoreHandle_t    s_hub_mutex         = NULL;
static TaskHandle_t         s_poll_task_handle  = NULL;
static sensor_new_record_cb s_on_record_cb      = NULL;

static bool                 s_initialized       = false;
static bool                 s_ds3231_ready      = false;
static bool                 s_gy30_ready        = false;
static bool                 s_dht22_ready       = false;

static sensor_record_t      s_latest_record     = {0};

static void sensor_hub_collect_once(sensor_record_t *out_rec)
{
    memset(out_rec, 0, sizeof(sensor_record_t));

    /* 1. 取得系統時間並驗證 DS3231 RTC 狀態 */
    time_t now = time(NULL);
    if (s_ds3231_ready) {
        struct tm rtc_tm = {0};
        if (env_ds3231_get_time(&rtc_tm) == ESP_OK) {
            out_rec->status_flags |= REC_FLAG_RTC_VALID;
            /* 若系統時鐘尚未有效同步，從 DS3231 補同步 */
            if (now < VALID_EPOCH_THRESHOLD) {
                if (env_ds3231_sync_to_system() == ESP_OK) {
                    now = time(NULL);
                }
            }
        }
    }
    out_rec->timestamp = (now > 0) ? (uint32_t)now : 0U;

    /* 2. 讀取 DHT22 溫濕度（失敗時降級填入 INVALID 值） */
    int16_t temp_x10 = INVALID_TEMP_VALUE;
    uint16_t humi_x10 = INVALID_HUMI_VALUE;
    if (s_dht22_ready && env_dht22_read(&temp_x10, &humi_x10) == ESP_OK) {
        out_rec->temperature = temp_x10;
        out_rec->humidity = humi_x10;
        out_rec->status_flags |= REC_FLAG_DHT22_VALID;
    } else {
        out_rec->temperature = INVALID_TEMP_VALUE;
        out_rec->humidity = INVALID_HUMI_VALUE;
    }

    /* 3. 讀取 GY-30 (BH1750) 環境光照度 */
    uint16_t lux = 0;
    if (s_gy30_ready && env_gy30_read_lux(&lux) == ESP_OK) {
        out_rec->lux = lux;
        out_rec->status_flags |= REC_FLAG_GY30_VALID;
    } else {
        out_rec->lux = 0;
    }

    /* 4. 計算前段欄位 CRC16 校驗碼 */
    out_rec->crc16 = board_calc_crc16((const uint8_t *)out_rec,
                                      offsetof(sensor_record_t, crc16));
}

static void task_sensor_poll(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t poll_period = pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS);

    ESP_LOGI(TAG, "task_sensor_poll started on Core %d (period=%u ms)",
             xPortGetCoreID(), SENSOR_POLL_INTERVAL_MS);

    while (1) {
        sensor_record_t rec;
        sensor_hub_collect_once(&rec);

        if (xSemaphoreTake(s_hub_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_latest_record = rec;
            xSemaphoreGive(s_hub_mutex);
        }

        if (s_on_record_cb != NULL) {
            s_on_record_cb(&rec);
        }

        vTaskDelayUntil(&last_wake_time, poll_period);
    }
}

esp_err_t sensor_hub_init(sensor_new_record_cb on_record_cb)
{
    if (s_hub_mutex == NULL) {
        s_hub_mutex = xSemaphoreCreateMutex();
        if (s_hub_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create sensor_hub mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 若尚未設定時區，預設使用 UTC+8 (CST-8) 以維持 RTC 與日期換算一致性 */
    if (getenv("TZ") == NULL) {
        setenv("TZ", "CST-8", 1);
        tzset();
    }

    s_on_record_cb = on_record_cb;

    /* 初始化 DS3231 並嘗試將硬體時間同步至系統時鐘 */
    if (env_ds3231_init() == ESP_OK) {
        s_ds3231_ready = true;
        if (env_ds3231_sync_to_system() != ESP_OK) {
            ESP_LOGW(TAG, "DS3231 online but initial system clock sync failed");
        }
    } else {
        s_ds3231_ready = false;
        ESP_LOGW(TAG, "DS3231 unavailable; running without hardware RTC backup");
    }

    /* 初始化 GY-30 光照感測器 */
    if (env_gy30_init() == ESP_OK) {
        s_gy30_ready = true;
    } else {
        s_gy30_ready = false;
        ESP_LOGW(TAG, "GY-30 unavailable; lux readings will degrade to 0");
    }

    /* 初始化 DHT22 單匯流排溫濕度感測器 */
    if (env_dht22_init() == ESP_OK) {
        s_dht22_ready = true;
    } else {
        s_dht22_ready = false;
        ESP_LOGW(TAG, "DHT22 init failed; temp/humi will degrade to INVALID");
    }

    memset(&s_latest_record, 0, sizeof(s_latest_record));
    s_latest_record.temperature = INVALID_TEMP_VALUE;
    s_latest_record.humidity = INVALID_HUMI_VALUE;

    s_initialized = true;
    ESP_LOGI(TAG, "sensor_hub initialized (RTC=%d, GY30=%d, DHT22=%d)",
             s_ds3231_ready, s_gy30_ready, s_dht22_ready);
    return ESP_OK;
}

esp_err_t sensor_hub_start_task(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "sensor_hub_start_task called before sensor_hub_init");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_poll_task_handle != NULL) {
        ESP_LOGW(TAG, "task_sensor_poll is already running");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        task_sensor_poll,
        "task_sensor_poll",
        SENSOR_POLL_TASK_STACK,
        NULL,
        SENSOR_POLL_TASK_PRIO,
        &s_poll_task_handle,
        SENSOR_POLL_TASK_CORE
    );

    if (ret != pdPASS) {
        s_poll_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create task_sensor_poll on Core %d", SENSOR_POLL_TASK_CORE);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t sensor_hub_sync_ntp(void)
{
    ESP_LOGI(TAG, "Starting SNTP time synchronization...");

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    uint32_t elapsed_ms = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        if (elapsed_ms >= SNTP_SYNC_TIMEOUT_MS) {
            ESP_LOGW(TAG, "SNTP sync timed out after %u ms", SNTP_SYNC_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SNTP_SYNC_POLL_STEP_MS));
        elapsed_ms += SNTP_SYNC_POLL_STEP_MS;
    }

    time_t now = 0;
    struct tm local_tm = {0};
    time(&now);
    localtime_r(&now, &local_tm);

    ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
             local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);

    if (!s_ds3231_ready) {
        ESP_LOGW(TAG, "DS3231 offline; skipped hardware RTC calibration");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = env_ds3231_set_time(&local_tm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to calibrate DS3231 after SNTP sync: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DS3231 RTC calibrated successfully from SNTP");
    return ESP_OK;
}

uint32_t sensor_hub_get_date_yyyymmdd(uint32_t unix_ts)
{
    time_t ts = (time_t)unix_ts;
    struct tm tm_info = {0};
    localtime_r(&ts, &tm_info);

    uint32_t year  = (uint32_t)(tm_info.tm_year + 1900);
    uint32_t month = (uint32_t)(tm_info.tm_mon + 1);
    uint32_t day   = (uint32_t)tm_info.tm_mday;

    return (year * 10000U) + (month * 100U) + day;
}
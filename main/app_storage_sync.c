#include "app_bridges.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "board_bsp.h"
#include "psram_cache.h"
#include "eeprom_blackbox.h"
#include "sd_media.h"
#include "sensor_hub.h"
#include "oled_ui.h"
#include "net_manager.h"

static const char *TAG = "app_storage";

#define SD_OFFLINE_DIR          "/sdcard/sensor_offline"
#define SD_LOW_WARN_DEFAULT_MB  100U
#define SD_FULL_LIMIT_MB        20U
#define SD_MUTEX_WAIT_MS        1000U

static SemaphoreHandle_t s_snap_mutex      = NULL;
static sensor_record_t   s_latest_rec      = {0};
static env_stats_t       s_latest_stats    = {0};
static bool              s_has_snapshot    = false;
static uint8_t           s_alert_flags     = STORAGE_ALERT_NONE;

static char              s_sd_cur_file[64] = {0};
static size_t            s_sd_cur_offset   = 0;
static uint16_t          s_ee_read_idx     = 0;

void app_storage_init_bridge(void)
{
    if (s_snap_mutex == NULL) {
        s_snap_mutex = xSemaphoreCreateMutex();
    }
}

static void refresh_alert_flags(uint8_t eeprom_flags)
{
    uint8_t flags = eeprom_flags;
    if (sd_media_is_mounted()) {
        uint32_t free_mb = sd_media_get_free_mb();
        if (free_mb < SD_FULL_LIMIT_MB) {
            flags |= (STORAGE_ALERT_SD_LOW | STORAGE_ALERT_SD_FULL);
        } else if (free_mb < SD_LOW_WARN_DEFAULT_MB) {
            flags |= STORAGE_ALERT_SD_LOW;
        }
    }
    s_alert_flags = flags;
    oled_ui_set_alert_flags(s_alert_flags);
}

void app_storage_on_sensor_record(const sensor_record_t *rec)
{
    if (rec == NULL) {
        return;
    }

    (void)psram_cache_push(rec);

    env_stats_t stats = {0};
    (void)psram_cache_get_stats(3600U, rec->timestamp, &stats);

    if (s_snap_mutex != NULL &&
        xSemaphoreTake(s_snap_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_latest_rec   = *rec;
        s_latest_stats = stats;
        s_has_snapshot = true;
        xSemaphoreGive(s_snap_mutex);
    }

    oled_ui_update_sensor(rec, &stats);
    oled_ui_adjust_contrast_by_lux(rec->lux);

    (void)net_manager_submit_record(rec);
}

esp_err_t app_storage_on_offline_record(const sensor_record_t *rec, void *ctx)
{
    (void)ctx;
    if (rec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t ee_flags = STORAGE_ALERT_NONE;
    esp_err_t err = eeprom_bb_push_record(rec, &ee_flags);
    refresh_alert_flags(ee_flags);
    return err;
}

void app_storage_get_status(uint16_t *out_eeprom_cnt, bool *out_sd_mounted,
                            uint32_t *out_sd_free_mb, void *ctx)
{
    (void)ctx;
    if (out_eeprom_cnt != NULL) {
        *out_eeprom_cnt = eeprom_bb_get_record_count();
    }
    if (out_sd_mounted != NULL) {
        *out_sd_mounted = sd_media_is_mounted();
    }
    if (out_sd_free_mb != NULL) {
        *out_sd_free_mb = sd_media_is_mounted() ? sd_media_get_free_mb() : 0U;
    }
}

esp_err_t app_storage_eeprom_flush_to_sd(const sensor_record_t *records, size_t count)
{
    if (records == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_media_is_mounted()) {
        ESP_LOGE(TAG, "EEPROM 滿載轉存失敗：SD 卡未掛載");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t free_mb = sd_media_get_free_mb();
    if (free_mb < SD_FULL_LIMIT_MB) {
        refresh_alert_flags(STORAGE_ALERT_EEPROM_80);
        ESP_LOGE(TAG, "SD 卡剩餘空間不足 %uMB，禁止寫入以保護檔案系統", SD_FULL_LIMIT_MB);
        return ESP_ERR_NO_MEM;
    }

    uint32_t date_ymd = sensor_hub_get_date_yyyymmdd(records[0].timestamp);
    esp_err_t err = sd_media_append_offline_records(date_ymd, records, count);
    if (err == ESP_OK) {
        refresh_alert_flags(STORAGE_ALERT_NONE);
    }
    return err;
}

static bool is_valid_offline_bin(const char *name)
{
    if (name == NULL || strlen(name) != 12U) {
        return false;
    }
    for (size_t i = 0; i < 8U; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return false;
        }
    }
    return (strcmp(name + 8U, ".bin") == 0);
}

static bool find_oldest_sd_file_locked(char *out_path, size_t out_len)
{
    DIR *dir = opendir(SD_OFFLINE_DIR);
    if (dir == NULL) {
        return false;
    }

    char oldest_name[16] = {0};
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_valid_offline_bin(ent->d_name)) {
            continue;
        }
        if (oldest_name[0] == '\0' || strcmp(ent->d_name, oldest_name) < 0) {
            strncpy(oldest_name, ent->d_name, sizeof(oldest_name) - 1U);
            oldest_name[sizeof(oldest_name) - 1U] = '\0';
        }
    }
    closedir(dir);

    if (oldest_name[0] == '\0') {
        return false;
    }
    snprintf(out_path, out_len, "%s/%s", SD_OFFLINE_DIR, oldest_name);
    return true;
}

static esp_err_t sd_sync_peek_batch(sensor_record_t *out_buf, size_t max_count,
                                    size_t *out_count, void *ctx)
{
    (void)ctx;
    *out_count = 0U;

    if (!sd_media_is_mounted()) {
        return ESP_OK;
    }

    SemaphoreHandle_t sd_mtx = board_get_sd_mutex();
    if (sd_mtx == NULL ||
        xSemaphoreTake(sd_mtx, pdMS_TO_TICKS(SD_MUTEX_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    while (1) {
        if (s_sd_cur_file[0] == '\0') {
            if (!find_oldest_sd_file_locked(s_sd_cur_file, sizeof(s_sd_cur_file))) {
                break;
            }
            s_sd_cur_offset = 0U;
        }

        FILE *fp = fopen(s_sd_cur_file, "rb");
        if (fp == NULL) {
            s_sd_cur_file[0] = '\0';
            s_sd_cur_offset  = 0U;
            break;
        }

        (void)fseek(fp, (long)s_sd_cur_offset, SEEK_SET);
        size_t n = fread(out_buf, sizeof(sensor_record_t), max_count, fp);
        fclose(fp);

        if (n == 0U) {
            (void)unlink(s_sd_cur_file);
            s_sd_cur_file[0] = '\0';
            s_sd_cur_offset  = 0U;
            continue;
        }

        *out_count = n;
        break;
    }

    xSemaphoreGive(sd_mtx);
    return ESP_OK;
}

static esp_err_t sd_sync_ack_batch(size_t count, void *ctx)
{
    (void)ctx;
    if (s_sd_cur_file[0] == '\0' || count == 0U) {
        return ESP_OK;
    }

    SemaphoreHandle_t sd_mtx = board_get_sd_mutex();
    if (sd_mtx == NULL ||
        xSemaphoreTake(sd_mtx, pdMS_TO_TICKS(SD_MUTEX_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_sd_cur_offset += count * sizeof(sensor_record_t);

    struct stat st;
    if (stat(s_sd_cur_file, &st) == 0 && s_sd_cur_offset >= (size_t)st.st_size) {
        ESP_LOGI(TAG, "離線日誌 %s 已 100%% 補傳並獲 ACK，執行刪除", s_sd_cur_file);
        (void)unlink(s_sd_cur_file);
        s_sd_cur_file[0] = '\0';
        s_sd_cur_offset  = 0U;
    }

    xSemaphoreGive(sd_mtx);
    return ESP_OK;
}

static esp_err_t eeprom_sync_peek_batch(sensor_record_t *out_buf, size_t max_count,
                                        size_t *out_count, void *ctx)
{
    (void)ctx;
    *out_count = 0U;

    uint16_t total = eeprom_bb_get_record_count();
    if (total == 0U || s_ee_read_idx >= total) {
        s_ee_read_idx = 0U;
        return ESP_OK;
    }

    uint16_t rem = (uint16_t)(total - s_ee_read_idx);
    uint16_t n   = (rem < (uint16_t)max_count) ? rem : (uint16_t)max_count;

    esp_err_t err = eeprom_bb_read_records(s_ee_read_idx, out_buf, n);
    if (err == ESP_OK) {
        *out_count = n;
    }
    return err;
}

static esp_err_t eeprom_sync_ack_batch(size_t count, void *ctx)
{
    (void)ctx;
    s_ee_read_idx = (uint16_t)(s_ee_read_idx + count);

    uint16_t total = eeprom_bb_get_record_count();
    if (s_ee_read_idx >= total) {
        s_ee_read_idx = 0U;
        refresh_alert_flags(STORAGE_ALERT_NONE);
        return eeprom_bb_clear_partition_b();
    }
    return ESP_OK;
}

void app_storage_register_sync_sources(void)
{
    s_sd_cur_file[0] = '\0';
    s_sd_cur_offset  = 0U;
    s_ee_read_idx    = 0U;

    sync_engine_clear_sources();

    const sync_source_t src_sd = {
        .name              = "sd_offline_dir",
        .peek_oldest_batch = sd_sync_peek_batch,
        .ack_oldest_batch  = sd_sync_ack_batch,
        .ctx               = NULL,
    };
    const sync_source_t src_eeprom = {
        .name              = "eeprom_partition_b",
        .peek_oldest_batch = eeprom_sync_peek_batch,
        .ack_oldest_batch  = eeprom_sync_ack_batch,
        .ctx               = NULL,
    };

    (void)sync_engine_register_source(&src_sd);
    (void)sync_engine_register_source(&src_eeprom);
}

bool app_storage_get_latest_snapshot(sensor_record_t *out_rec, env_stats_t *out_stats)
{
    bool valid = false;
    if (s_snap_mutex != NULL &&
        xSemaphoreTake(s_snap_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        valid = s_has_snapshot;
        if (valid) {
            if (out_rec != NULL) {
                *out_rec = s_latest_rec;
            }
            if (out_stats != NULL) {
                *out_stats = s_latest_stats;
            }
        }
        xSemaphoreGive(s_snap_mutex);
    }
    return valid;
}
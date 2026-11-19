#include "eeprom_blackbox.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "board_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "eeprom_blackbox";

// 分區 A 主備份位址（0x0000 ~ 0x03FF 共 1024 Bytes，sys_config_t 固定 240 Bytes）
#define EEPROM_CONFIG_PRIMARY_ADDR  (EEPROM_CONFIG_ADDR)
#define EEPROM_CONFIG_BACKUP_ADDR   (EEPROM_CONFIG_ADDR + 0x0100U)
#define EEPROM_READ_CHUNK_BYTES     128U

typedef struct {
    bool                     initialized;
    eeprom_io_write_fn       wr_fn;
    eeprom_io_read_fn        rd_fn;
    eeprom_overflow_flush_cb flush_cb;
    SemaphoreHandle_t        mutex;
    sys_config_t             cfg;
} eeprom_bb_ctx_t;

static eeprom_bb_ctx_t s_ctx = {
    .initialized = false,
    .wr_fn       = NULL,
    .rd_fn       = NULL,
    .flush_cb    = NULL,
    .mutex       = NULL,
};

// -----------------------------------------------------------------------------
// 內建 2-Byte 記憶體位址 I2C 轉接層（T-02-01）
// -----------------------------------------------------------------------------

static esp_err_t default_eeprom_write(uint16_t mem_addr, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U || len > EEPROM_PAGE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx_buf[2U + EEPROM_PAGE_SIZE];
    tx_buf[0] = (uint8_t)((mem_addr >> 8) & 0xFFU);
    tx_buf[1] = (uint8_t)(mem_addr & 0xFFU);
    memcpy(&tx_buf[2], data, len);

    return board_i2c_write(I2C_ADDR_EEPROM, tx_buf, 2U + len);
}

static esp_err_t default_eeprom_read(uint16_t mem_addr, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t addr_buf[2];
    addr_buf[0] = (uint8_t)((mem_addr >> 8) & 0xFFU);
    addr_buf[1] = (uint8_t)(mem_addr & 0xFFU);

    return board_i2c_write_read(I2C_ADDR_EEPROM, addr_buf, 2U, data, len);
}

// -----------------------------------------------------------------------------
// 64-Byte 頁邊界自動切割寫入 ＋ 6ms 延遲（T-02-02 核心防呆）
// -----------------------------------------------------------------------------

static inline void eeprom_wait_write_cycle(void)
{
    TickType_t ticks = pdMS_TO_TICKS(6);
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

static esp_err_t eeprom_write_paged(uint16_t mem_addr, const uint8_t *data, size_t total_len)
{
    if (data == NULL || total_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)mem_addr + (uint32_t)total_len > EEPROM_TOTAL_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0U;
    while (offset < total_len) {
        uint16_t cur_addr      = (uint16_t)(mem_addr + offset);
        uint16_t page_offset   = cur_addr % EEPROM_PAGE_SIZE;
        size_t   space_in_page = EEPROM_PAGE_SIZE - page_offset;
        size_t   remaining     = total_len - offset;
        size_t   chunk         = (remaining < space_in_page) ? remaining : space_in_page;

        esp_err_t err = s_ctx.wr_fn(cur_addr, &data[offset], chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Page write failed at 0x%04X (len=%zu): %s",
                     cur_addr, chunk, esp_err_to_name(err));
            return err;
        }

        // 每次頁寫入後強制等待 >= 6ms（大於 AT24C256 5ms 寫入週期）
        eeprom_wait_write_cycle();
        offset += chunk;
    }

    return ESP_OK;
}

static esp_err_t eeprom_read_bytes(uint16_t mem_addr, uint8_t *data, size_t total_len)
{
    if (data == NULL || total_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)mem_addr + (uint32_t)total_len > EEPROM_TOTAL_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0U;
    while (offset < total_len) {
        uint16_t cur_addr  = (uint16_t)(mem_addr + offset);
        size_t   remaining = total_len - offset;
        size_t   chunk     = (remaining < EEPROM_READ_CHUNK_BYTES) ? remaining : EEPROM_READ_CHUNK_BYTES;

        esp_err_t err = s_ctx.rd_fn(cur_addr, &data[offset], chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Read failed at 0x%04X (len=%zu): %s",
                     cur_addr, chunk, esp_err_to_name(err));
            return err;
        }
        offset += chunk;
    }

    return ESP_OK;
}

// -----------------------------------------------------------------------------
// 分區 A（系統設定）內部輔助函式
// -----------------------------------------------------------------------------

static bool is_config_valid(const sys_config_t *cfg)
{
    if (cfg->magic != SYS_CONFIG_MAGIC) {
        return false;
    }
    if (cfg->record_count > EEPROM_MAX_RECORDS) {
        return false;
    }
    uint32_t expected_crc = board_calc_crc32((const uint8_t *)cfg, offsetof(sys_config_t, crc32));
    return (cfg->crc32 == expected_crc);
}

static void fill_default_config(sys_config_t *cfg)
{
    memset(cfg, 0, sizeof(sys_config_t));
    cfg->magic               = SYS_CONFIG_MAGIC;
    cfg->sample_interval_sec = 10U;
    cfg->psram_window_hours  = 24U;
    cfg->eeprom_warn_pct     = 80U;
    cfg->sd_warn_free_mb     = 50U;
    cfg->record_count        = 0U;
    cfg->crc32               = board_calc_crc32((const uint8_t *)cfg, offsetof(sys_config_t, crc32));
}

static esp_err_t persist_config_locked(const sys_config_t *cfg)
{
    sys_config_t copy = *cfg;
    copy.magic = SYS_CONFIG_MAGIC;
    copy.crc32 = board_calc_crc32((const uint8_t *)&copy, offsetof(sys_config_t, crc32));

    esp_err_t err = eeprom_write_paged(EEPROM_CONFIG_PRIMARY_ADDR,
                                       (const uint8_t *)&copy,
                                       sizeof(sys_config_t));
    if (err != ESP_OK) {
        return err;
    }

    // 同步寫入備份區，防止寫入瞬間斷電毀損設定與 record_count
    err = eeprom_write_paged(EEPROM_CONFIG_BACKUP_ADDR,
                             (const uint8_t *)&copy,
                             sizeof(sys_config_t));
    if (err != ESP_OK) {
        return err;
    }

    s_ctx.cfg = copy;
    return ESP_OK;
}

static esp_err_t load_config_locked(sys_config_t *out_cfg)
{
    sys_config_t primary_cfg;
    sys_config_t backup_cfg;

    bool primary_ok = (eeprom_read_bytes(EEPROM_CONFIG_PRIMARY_ADDR,
                                         (uint8_t *)&primary_cfg,
                                         sizeof(sys_config_t)) == ESP_OK) &&
                      is_config_valid(&primary_cfg);

    if (primary_ok) {
        s_ctx.cfg = primary_cfg;
        if (out_cfg != NULL) {
            *out_cfg = primary_cfg;
        }
        return ESP_OK;
    }

    bool backup_ok = (eeprom_read_bytes(EEPROM_CONFIG_BACKUP_ADDR,
                                        (uint8_t *)&backup_cfg,
                                        sizeof(sys_config_t)) == ESP_OK) &&
                     is_config_valid(&backup_cfg);

    if (backup_ok) {
        ESP_LOGW(TAG, "Primary config corrupted, restoring from backup");
        esp_err_t err = persist_config_locked(&backup_cfg);
        if (err != ESP_OK) {
            return err;
        }
        if (out_cfg != NULL) {
            *out_cfg = s_ctx.cfg;
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "No valid config found in EEPROM, initializing defaults");
    sys_config_t def_cfg;
    fill_default_config(&def_cfg);
    esp_err_t err = persist_config_locked(&def_cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (out_cfg != NULL) {
        *out_cfg = s_ctx.cfg;
    }
    return ESP_OK;
}

static esp_err_t clear_partition_b_locked(void)
{
    uint8_t erase_page[EEPROM_PAGE_SIZE];
    memset(erase_page, 0xFF, sizeof(erase_page));

    for (uint32_t addr = EEPROM_DATA_START_ADDR; addr < EEPROM_TOTAL_BYTES; addr += EEPROM_PAGE_SIZE) {
        esp_err_t err = eeprom_write_paged((uint16_t)addr, erase_page, EEPROM_PAGE_SIZE);
        if (err != ESP_OK) {
            return err;
        }
    }

    s_ctx.cfg.record_count = 0U;
    return persist_config_locked(&s_ctx.cfg);
}

// -----------------------------------------------------------------------------
// 公開 API 實作
// -----------------------------------------------------------------------------

esp_err_t eeprom_bb_init(eeprom_io_write_fn custom_wr, eeprom_io_read_fn custom_rd)
{
    if (s_ctx.mutex == NULL) {
        s_ctx.mutex = xSemaphoreCreateMutex();
        if (s_ctx.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    s_ctx.wr_fn = (custom_wr != NULL) ? custom_wr : default_eeprom_write;
    s_ctx.rd_fn = (custom_rd != NULL) ? custom_rd : default_eeprom_read;

    esp_err_t err = load_config_locked(NULL);
    if (err == ESP_OK) {
        s_ctx.initialized = true;
        ESP_LOGI(TAG, "EEPROM blackbox ready (stored records: %" PRIu16 "/%" PRIu16 ")",
                 s_ctx.cfg.record_count, (uint16_t)EEPROM_MAX_RECORDS);
    }

    xSemaphoreGive(s_ctx.mutex);
    return err;
}

void eeprom_bb_register_flush_cb(eeprom_overflow_flush_cb cb)
{
    if (s_ctx.mutex != NULL) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.flush_cb = cb;
        xSemaphoreGive(s_ctx.mutex);
    } else {
        s_ctx.flush_cb = cb;
    }
}

esp_err_t eeprom_bb_load_config(sys_config_t *out_cfg)
{
    if (out_cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized || s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    esp_err_t err = load_config_locked(out_cfg);
    xSemaphoreGive(s_ctx.mutex);
    return err;
}

esp_err_t eeprom_bb_save_config(const sys_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized || s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    // 保留目前分區 B 的實際 record_count，確保更新設定絕不影響分區 B 狀態
    sys_config_t to_save = *cfg;
    to_save.record_count = s_ctx.cfg.record_count;

    esp_err_t err = persist_config_locked(&to_save);

    xSemaphoreGive(s_ctx.mutex);
    return err;
}

esp_err_t eeprom_bb_push_record(const sensor_record_t *rec, uint8_t *out_alert_flags)
{
    if (rec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized || s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (out_alert_flags != NULL) {
        *out_alert_flags = STORAGE_ALERT_NONE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    // 100% 滿載時觸發溢位轉存 Callback；僅在回傳 ESP_OK 確認落盤後才淨空分區 B
    if (s_ctx.cfg.record_count >= EEPROM_MAX_RECORDS) {
        if (s_ctx.flush_cb == NULL) {
            xSemaphoreGive(s_ctx.mutex);
            ESP_LOGE(TAG, "EEPROM Partition B full and no flush callback registered");
            return ESP_ERR_NO_MEM;
        }

        size_t dump_bytes = (size_t)EEPROM_MAX_RECORDS * sizeof(sensor_record_t);
        sensor_record_t *dump_buf = (sensor_record_t *)heap_caps_malloc(
            dump_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (dump_buf == NULL) {
            dump_buf = (sensor_record_t *)malloc(dump_bytes);
        }
        if (dump_buf == NULL) {
            xSemaphoreGive(s_ctx.mutex);
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err = eeprom_read_bytes(EEPROM_DATA_START_ADDR, (uint8_t *)dump_buf, dump_bytes);
        if (err != ESP_OK) {
            free(dump_buf);
            xSemaphoreGive(s_ctx.mutex);
            return err;
        }

        err = s_ctx.flush_cb(dump_buf, EEPROM_MAX_RECORDS);
        free(dump_buf);

        if (err != ESP_OK) {
            xSemaphoreGive(s_ctx.mutex);
            ESP_LOGW(TAG, "Overflow flush callback failed (%s), retaining Partition B data",
                     esp_err_to_name(err));
            return err;
        }

        err = clear_partition_b_locked();
        if (err != ESP_OK) {
            xSemaphoreGive(s_ctx.mutex);
            return err;
        }
    }

    sensor_record_t rec_copy = *rec;
    if (rec_copy.crc16 == 0U) {
        rec_copy.crc16 = board_calc_crc16((const uint8_t *)&rec_copy,
                                          offsetof(sensor_record_t, crc16));
    }

    uint16_t write_addr = (uint16_t)(EEPROM_DATA_START_ADDR +
                                     (uint32_t)s_ctx.cfg.record_count * sizeof(sensor_record_t));

    esp_err_t err = eeprom_write_paged(write_addr,
                                       (const uint8_t *)&rec_copy,
                                       sizeof(sensor_record_t));
    if (err != ESP_OK) {
        xSemaphoreGive(s_ctx.mutex);
        return err;
    }

    s_ctx.cfg.record_count++;
    err = persist_config_locked(&s_ctx.cfg);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ctx.mutex);
        return err;
    }

    if (out_alert_flags != NULL) {
        uint8_t warn_pct = (s_ctx.cfg.eeprom_warn_pct > 0U && s_ctx.cfg.eeprom_warn_pct <= 100U)
                               ? s_ctx.cfg.eeprom_warn_pct
                               : 80U;
        if ((uint32_t)s_ctx.cfg.record_count * 100U >=
            (uint32_t)EEPROM_MAX_RECORDS * (uint32_t)warn_pct) {
            *out_alert_flags |= STORAGE_ALERT_EEPROM_80;
        }
    }

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

uint16_t eeprom_bb_get_record_count(void)
{
    uint16_t cnt = 0U;
    if (s_ctx.initialized && s_ctx.mutex != NULL) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        cnt = s_ctx.cfg.record_count;
        xSemaphoreGive(s_ctx.mutex);
    }
    return cnt;
}

esp_err_t eeprom_bb_read_records(uint16_t start_idx, sensor_record_t *out_buf, uint16_t count)
{
    if (out_buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.initialized || s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count == 0U) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if ((uint32_t)start_idx + (uint32_t)count > (uint32_t)s_ctx.cfg.record_count) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t read_addr = (uint16_t)(EEPROM_DATA_START_ADDR +
                                    (uint32_t)start_idx * sizeof(sensor_record_t));
    size_t read_bytes = (size_t)count * sizeof(sensor_record_t);

    esp_err_t err = eeprom_read_bytes(read_addr, (uint8_t *)out_buf, read_bytes);

    xSemaphoreGive(s_ctx.mutex);
    return err;
}

esp_err_t eeprom_bb_clear_partition_b(void)
{
    if (!s_ctx.initialized || s_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    esp_err_t err = clear_partition_b_locked();
    xSemaphoreGive(s_ctx.mutex);
    return err;
}
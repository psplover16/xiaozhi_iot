#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 底層 16-bit 記憶體定址讀寫介面（傳入 NULL 時自動使用內建 I2C 2-Byte 位址轉接器；TDD 測試時傳入 RAM Mock）
typedef esp_err_t (*eeprom_io_write_fn)(uint16_t mem_addr, const uint8_t *data, size_t len);
typedef esp_err_t (*eeprom_io_read_fn)(uint16_t mem_addr, uint8_t *data, size_t len);

// 100% 滿載溢位轉存 Callback（回傳 ESP_OK 代表已驗證落盤成功，才允許淨空分區 B）
typedef esp_err_t (*eeprom_overflow_flush_cb)(const sensor_record_t *records, size_t count);

esp_err_t eeprom_bb_init(eeprom_io_write_fn custom_wr, eeprom_io_read_fn custom_rd);
void      eeprom_bb_register_flush_cb(eeprom_overflow_flush_cb cb);

// 分區 A：系統設定讀寫（首次開機無效自動載入預設值；絕不影響分區 B）
esp_err_t eeprom_bb_load_config(sys_config_t *out_cfg);
esp_err_t eeprom_bb_save_config(const sys_config_t *cfg);

// 分區 B：感測黑盒子操作
esp_err_t eeprom_bb_push_record(const sensor_record_t *rec, uint8_t *out_alert_flags);
uint16_t  eeprom_bb_get_record_count(void);
esp_err_t eeprom_bb_read_records(uint16_t start_idx, sensor_record_t *out_buf, uint16_t count);
esp_err_t eeprom_bb_clear_partition_b(void); // 僅抹除 0x0400~0x7FFF 並歸零 record_count

#ifdef __cplusplus
}
#endif

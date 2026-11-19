#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_CONFIG_MAGIC        0x585A494FUL
#define EEPROM_TOTAL_BYTES      32768U
#define EEPROM_PAGE_SIZE        64U
#define EEPROM_CONFIG_ADDR      0x0000U
#define EEPROM_DATA_START_ADDR  0x0400U
#define EEPROM_MAX_RECORDS      1984U

typedef enum {
    NET_MODE_OFFLINE = 0,
    NET_MODE_ONLINE  = 1,
    NET_MODE_WEB_CFG = 2
} net_mode_t;

// 儲存警告旗標位元遮罩
#define STORAGE_ALERT_NONE       0x00U
#define STORAGE_ALERT_EEPROM_80  0x01U
#define STORAGE_ALERT_SD_LOW     0x02U
#define STORAGE_ALERT_SD_FULL    0x04U

// sensor_record_t status_flags 位元定義
#define REC_FLAG_DHT22_VALID     (1U << 0)
#define REC_FLAG_GY30_VALID      (1U << 1)
#define REC_FLAG_RTC_VALID       (1U << 2)
#define REC_FLAG_MODE_SHIFT      4U
#define REC_FLAG_MODE_MASK       (0x03U << 4)

#define INVALID_TEMP_VALUE       ((int16_t)0x7FFF)
#define INVALID_HUMI_VALUE       ((uint16_t)0xFFFF)

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    int16_t  temperature;
    uint16_t humidity;
    uint32_t lux;
    uint16_t status_flags;
    uint16_t crc16;
} sensor_record_t; // 固定 16 Bytes

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char     wifi_ssid[32];
    char     wifi_pass[64];
    char     server_url[128];
    uint16_t sample_interval_sec;
    uint8_t  psram_window_hours;
    uint8_t  eeprom_warn_pct;
    uint16_t sd_warn_free_mb;
    uint16_t record_count;
    uint32_t crc32;
} sys_config_t; // 固定 240 Bytes

typedef struct {
    float temp_avg, temp_min, temp_max;
    float humi_avg, humi_min, humi_max;
    uint32_t lux_avg, lux_min, lux_max;
    int8_t temp_trend; // -1:下降, 0:持平, 1:上升
    uint32_t valid_count;
} env_stats_t;

#ifdef __cplusplus
}
#endif

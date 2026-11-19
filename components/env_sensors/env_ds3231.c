#include "env_sensors_drv.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "board_bsp.h"

static const char *TAG = "env_ds3231";

#define DS3231_REG_SECONDS      0x00
#define DS3231_REG_CONTROL      0x0E
#define DS3231_REG_STATUS       0x0F
#define DS3231_STATUS_OSF_BIT   (1U << 7)

static inline uint8_t bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10U) + (val & 0x0FU));
}

static inline uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10U) << 4) | (val % 10U));
}

esp_err_t env_ds3231_init(void)
{
    if (!board_i2c_is_device_online(ENV_DS3231_I2C_ADDR)) {
        ESP_LOGW(TAG, "DS3231 (0x%02X) offline", ENV_DS3231_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t reg_addr = DS3231_REG_STATUS;
    uint8_t status = 0;
    esp_err_t ret = board_i2c_write_read(ENV_DS3231_I2C_ADDR, &reg_addr, 1, &status, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 read status failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (status & DS3231_STATUS_OSF_BIT) {
        ESP_LOGW(TAG, "DS3231 oscillator stop flag (OSF) set; time invalid until calibrated");
    }

    /* 確保振盪器啟用 (EOSC = 0) */
    uint8_t ctrl_payload[2] = { DS3231_REG_CONTROL, 0x00 };
    ret = board_i2c_write(ENV_DS3231_I2C_ADDR, ctrl_payload, sizeof(ctrl_payload));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 init control failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DS3231 initialized (0x%02X)", ENV_DS3231_I2C_ADDR);
    return ESP_OK;
}

esp_err_t env_ds3231_get_time(struct tm *out_tm)
{
    if (out_tm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t start_reg = DS3231_REG_SECONDS;
    uint8_t raw[7] = {0};
    esp_err_t ret = board_i2c_write_read(ENV_DS3231_I2C_ADDR, &start_reg, 1, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }

    memset(out_tm, 0, sizeof(struct tm));
    out_tm->tm_sec  = bcd_to_dec(raw[0] & 0x7FU);
    out_tm->tm_min  = bcd_to_dec(raw[1] & 0x7FU);

    /* 處理 12/24 小時制 */
    if (raw[2] & 0x40U) {
        uint8_t hr12 = bcd_to_dec(raw[2] & 0x1FU);
        bool is_pm = (raw[2] & 0x20U) != 0;
        if (hr12 == 12U) {
            out_tm->tm_hour = is_pm ? 12 : 0;
        } else {
            out_tm->tm_hour = is_pm ? (hr12 + 12) : hr12;
        }
    } else {
        out_tm->tm_hour = bcd_to_dec(raw[2] & 0x3FU);
    }

    out_tm->tm_wday = (int)(bcd_to_dec(raw[3] & 0x07U) - 1U);
    out_tm->tm_mday = bcd_to_dec(raw[4] & 0x3FU);
    out_tm->tm_mon  = (int)(bcd_to_dec(raw[5] & 0x1FU) - 1U);

    uint16_t year = 2000U + bcd_to_dec(raw[6]);
    if (raw[5] & 0x80U) {
        year += 100U;
    }
    out_tm->tm_year = (int)(year - 1900U);

    return ESP_OK;
}

esp_err_t env_ds3231_set_time(const struct tm *in_tm)
{
    if (in_tm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int full_year = in_tm->tm_year + 1900;
    if (full_year < 2000 || full_year > 2199) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t century_bit = (full_year >= 2100) ? 0x80U : 0x00U;
    uint8_t year_2d = (uint8_t)(full_year % 100);
    uint8_t wday = (uint8_t)((in_tm->tm_wday >= 0 && in_tm->tm_wday <= 6) ? (in_tm->tm_wday + 1) : 1);

    uint8_t payload[8] = {
        DS3231_REG_SECONDS,
        dec_to_bcd((uint8_t)in_tm->tm_sec),
        dec_to_bcd((uint8_t)in_tm->tm_min),
        dec_to_bcd((uint8_t)in_tm->tm_hour) & 0x3FU, /* 強制 24 小時制 */
        dec_to_bcd(wday),
        dec_to_bcd((uint8_t)in_tm->tm_mday),
        (uint8_t)(dec_to_bcd((uint8_t)(in_tm->tm_mon + 1)) | century_bit),
        dec_to_bcd(year_2d)
    };

    esp_err_t ret = board_i2c_write(ENV_DS3231_I2C_ADDR, payload, sizeof(payload));
    if (ret != ESP_OK) {
        return ret;
    }

    /* 清除 OSF 旗標 */
    uint8_t status_reg = DS3231_REG_STATUS;
    uint8_t status = 0;
    if (board_i2c_write_read(ENV_DS3231_I2C_ADDR, &status_reg, 1, &status, 1) == ESP_OK) {
        uint8_t clr_payload[2] = {
            DS3231_REG_STATUS,
            (uint8_t)(status & ~DS3231_STATUS_OSF_BIT)
        };
        (void)board_i2c_write(ENV_DS3231_I2C_ADDR, clr_payload, sizeof(clr_payload));
    }

    return ESP_OK;
}

esp_err_t env_ds3231_sync_to_system(void)
{
    struct tm rtc_tm;
    esp_err_t ret = env_ds3231_get_time(&rtc_tm);
    if (ret != ESP_OK) {
        return ret;
    }

    time_t epoch = mktime(&rtc_tm);
    if (epoch < 0) {
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0
    };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "System clock synced from DS3231: %04d-%02d-%02d %02d:%02d:%02d",
             rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1, rtc_tm.tm_mday,
             rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
    return ESP_OK;
}
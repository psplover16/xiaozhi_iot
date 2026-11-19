#include "env_sensors_drv.h"

#include "esp_log.h"
#include "board_bsp.h"

static const char *TAG = "env_gy30";

#define BH1750_CMD_POWER_ON         0x01
#define BH1750_CMD_RESET            0x07
#define BH1750_CMD_CONT_H_RES_MODE  0x10

esp_err_t env_gy30_init(void)
{
    if (!board_i2c_is_device_online(ENV_GY30_I2C_ADDR)) {
        ESP_LOGW(TAG, "GY-30 / BH1750 (0x%02X) offline", ENV_GY30_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t cmd_on = BH1750_CMD_POWER_ON;
    esp_err_t ret = board_i2c_write(ENV_GY30_I2C_ADDR, &cmd_on, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t cmd_mode = BH1750_CMD_CONT_H_RES_MODE;
    ret = board_i2c_write(ENV_GY30_I2C_ADDR, &cmd_mode, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "GY-30 / BH1750 initialized (0x%02X)", ENV_GY30_I2C_ADDR);
    return ESP_OK;
}

esp_err_t env_gy30_read_lux(uint16_t *out_lux)
{
    if (out_lux == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2] = {0};
    esp_err_t ret = board_i2c_read(ENV_GY30_I2C_ADDR, buf, sizeof(buf));
    if (ret != ESP_OK) {
        *out_lux = 0;
        return ret;
    }

    uint32_t raw = ((uint32_t)buf[0] << 8) | (uint32_t)buf[1];
    /* BH1750 換算公式: lux = raw / 1.2 (整數四捨五入) */
    uint32_t lux = (raw * 10U + 6U) / 12U;
    if (lux > 65535U) {
        lux = 65535U;
    }
    *out_lux = (uint16_t)lux;

    return ESP_OK;
}
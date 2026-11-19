#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 安全腳位定義（完全避開 0,3,19,20,26-37,43-46）
#define BOARD_PIN_MODE_BTN      1
#define BOARD_PIN_DHT22         4
#define BOARD_PIN_I2C_SDA       8
#define BOARD_PIN_I2C_SCL       9
#define BOARD_PIN_SD_CS         10
#define BOARD_PIN_SD_MOSI       11
#define BOARD_PIN_SD_CLK        12
#define BOARD_PIN_SD_MISO       13
#define BOARD_PIN_I2S_MIC_WS    15
#define BOARD_PIN_I2S_MIC_BCLK  16
#define BOARD_PIN_I2S_MIC_DIN   17
#define BOARD_PIN_I2S_SPK_LRC   18
#define BOARD_PIN_I2S_SPK_BCLK  21
#define BOARD_PIN_I2S_SPK_DOUT  38

// 7-bit I2C 裝置位址
#define I2C_ADDR_OLED           0x3C
#define I2C_ADDR_GY30           0x23
#define I2C_ADDR_DS3231         0x68
#define I2C_ADDR_EEPROM         0x50

esp_err_t board_init(void);
SemaphoreHandle_t board_get_i2c_mutex(void);
SemaphoreHandle_t board_get_sd_mutex(void);

// 執行緒安全 I2C 代理函式（內部自動加解 xI2cBusMutex 並匹配對應 dev_handle）
esp_err_t board_i2c_write(uint8_t dev_addr, const uint8_t *data, size_t len);
esp_err_t board_i2c_read(uint8_t dev_addr, uint8_t *data, size_t len);
esp_err_t board_i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_len);
bool      board_i2c_is_device_online(uint8_t dev_addr);

// I2S 通道 Handle 取得（由 board_init 建立，供 audio_engine 與 sd_media 共用）
i2s_chan_handle_t board_get_i2s_rx_chan(void);
i2s_chan_handle_t board_get_i2s_tx_chan(void);

// 校驗演算法
uint16_t board_calc_crc16(const uint8_t *data, size_t len);
uint32_t board_calc_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

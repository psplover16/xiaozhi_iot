/**
 * @file board_bsp.c
 * @brief Board Support Package / Hardware Configuration for Xiaozhi IoT
 *
 * Implements hardware pin mappings, I2C master bus initialization with thread-safe
 * mutex-protected read/write transactions, device handle caching, and CRC utilities.
 * Compliant with ESP-IDF v6.1.1 and C11 standard.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "board_bsp.h"

static const char *TAG = "board_bsp";

/* =========================================================================
 * 腳位定義鎖死區（依 spec.md 腳位表與 tasks.md T-00-01 規格）
 * ========================================================================= */

// I2C 匯流排腳位（共用匯流排：OLED, GY-30, DS3231, AT24C256）
#define BOARD_PIN_I2C_SDA           8
#define BOARD_PIN_I2C_SCL           9

// SPI2 匯流排與 MicroSD 卡腳位
#define BOARD_PIN_SD_MOSI           11
#define BOARD_PIN_SD_MISO           13
#define BOARD_PIN_SD_CLK            12
#define BOARD_PIN_SD_CS             10

// I2S0 音訊輸入 (INMP441 麥克風陣列)
#define BOARD_PIN_I2S_IN_BCLK       4
#define BOARD_PIN_I2S_IN_WS         5
#define BOARD_PIN_I2S_IN_DIN        6

// I2S1 音訊輸出 (MAX98357 功放)
#define BOARD_PIN_I2S_OUT_BCLK      15
#define BOARD_PIN_I2S_OUT_LRC       16
#define BOARD_PIN_I2S_OUT_DOUT      7

// 感測器與使用者介面腳位（T-00-01 規格）
#undef BOARD_PIN_DHT22
#define BOARD_PIN_DHT22             14
#undef BOARD_PIN_MODE_BTN
#define BOARD_PIN_MODE_BTN          1
#define BOARD_PIN_BACKLIGHT_PWM     2

// 依 plan.md v1.1.0 / spec.md v1.7.0 安全腳位映射別名
#define BOARD_PIN_DHT22_SAFE        4
#define BOARD_PIN_MIC_WS_SAFE       15
#define BOARD_PIN_MIC_BCLK_SAFE     16
#define BOARD_PIN_MIC_DIN_SAFE      17
#define BOARD_PIN_SPK_LRC_SAFE      18
#define BOARD_PIN_SPK_BCLK_SAFE     21
#define BOARD_PIN_SPK_DOUT_SAFE     38

/* =========================================================================
 * 7-bit I2C 裝置位址常數定義
 * ========================================================================= */
#define I2C_DEV_ADDR_OLED           0x3C
#define I2C_DEV_ADDR_GY30           0x23
#define I2C_DEV_ADDR_DS3231         0x68
#define I2C_DEV_ADDR_EEPROM         0x50

#define I2C_DEV_CACHE_SIZE          8
#define I2C_BUS_TIMEOUT_MS          100
#define I2C_BUS_FREQ_HZ             400000

/* =========================================================================
 * 全域靜態變數、快取與同步鎖
 * ========================================================================= */
static SemaphoreHandle_t s_i2c_bus_mutex = NULL;
static SemaphoreHandle_t s_spi_sd_mutex  = NULL;
static i2s_chan_handle_t s_i2s_rx_handle = NULL;
static i2s_chan_handle_t s_i2s_tx_handle = NULL;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;
static bool s_board_initialized = false;

typedef struct {
    uint8_t dev_addr;
    i2c_master_dev_handle_t handle;
} i2c_dev_slot_t;

static i2c_dev_slot_t s_i2c_dev_slots[I2C_DEV_CACHE_SIZE];
static size_t s_i2c_dev_count = 0;

/* =========================================================================
 * 內部輔助函式：動態取得或快取 I2C 裝置 Handle
 * ========================================================================= */
static esp_err_t get_or_add_i2c_device(uint8_t dev_addr, i2c_master_dev_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // 檢查快取
    for (size_t i = 0; i < s_i2c_dev_count; i++) {
        if (s_i2c_dev_slots[i].dev_addr == dev_addr) {
            *out_handle = s_i2c_dev_slots[i].handle;
            return ESP_OK;
        }
    }

    // 快取未命中，動態註冊裝置
    if (s_i2c_dev_count >= I2C_DEV_CACHE_SIZE) {
        ESP_LOGE(TAG, "I2C device cache full (max %d)", I2C_DEV_CACHE_SIZE);
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = I2C_BUS_FREQ_HZ,
    };

    i2c_master_dev_handle_t new_handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &new_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C dev 0x%02X: %s", dev_addr, esp_err_to_name(err));
        return err;
    }

    s_i2c_dev_slots[s_i2c_dev_count].dev_addr = dev_addr;
    s_i2c_dev_slots[s_i2c_dev_count].handle = new_handle;
    s_i2c_dev_count++;

    *out_handle = new_handle;
    return ESP_OK;
}

/* =========================================================================
 * 核心硬體管理與互斥鎖取得函式
 * ========================================================================= */

SemaphoreHandle_t board_get_i2c_mutex(void)
{
    if (s_i2c_bus_mutex == NULL) {
        s_i2c_bus_mutex = xSemaphoreCreateMutex();
    }
    return s_i2c_bus_mutex;
}

SemaphoreHandle_t board_get_sd_mutex(void)
{
    if (s_spi_sd_mutex == NULL) {
        s_spi_sd_mutex = xSemaphoreCreateMutex();
    }
    return s_spi_sd_mutex;
}

esp_err_t board_init(void)
{
    if (s_board_initialized) {
        return ESP_OK;
    }

    // 建立互斥鎖
    if (board_get_i2c_mutex() == NULL || board_get_sd_mutex() == NULL) {
        ESP_LOGE(TAG, "Failed to create hardware mutexes");
        return ESP_ERR_NO_MEM;
    }

    // 使用 ESP-IDF v6.1.1 新版 i2c_new_master_bus() 初始化 I2C0 (400kHz, 內部上拉)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)BOARD_PIN_I2C_SDA,
        .scl_io_num = (gpio_num_t)BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C master bus: %s", esp_err_to_name(err));
        return err;
    }

    // 預先快取 4 個核心 I2C 裝置 Handle (OLED, GY-30, DS3231, AT24C256)
    const uint8_t core_dev_addrs[] = {
        I2C_DEV_ADDR_OLED,
        I2C_DEV_ADDR_GY30,
        I2C_DEV_ADDR_DS3231,
        I2C_DEV_ADDR_EEPROM
    };
    for (size_t i = 0; i < sizeof(core_dev_addrs); i++) {
        i2c_master_dev_handle_t dev_h = NULL;
        get_or_add_i2c_device(core_dev_addrs[i], &dev_h);
    }

    // 1. 使用 spi_bus_initialize() 初始化 SPI2_HOST（DMA 自動通道），供後續 SD 卡掛載使用
    spi_bus_config_t spi_bus_cfg = {
        .mosi_io_num = (gpio_num_t)BOARD_PIN_SD_MOSI,
        .miso_io_num = (gpio_num_t)BOARD_PIN_SD_MISO,
        .sclk_io_num = (gpio_num_t)BOARD_PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SPI2_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI2 bus: %s", esp_err_to_name(err));
        return err;
    }

    // 2. 初始化 GPIO 1 為輸入模式並開啟內部上拉（GPIO_PULLUP_ONLY）
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_MODE_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&btn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure Mode Button GPIO: %s", esp_err_to_name(err));
        return err;
    }

    // 3. 初始化 LEDC Timer 與 Channel 輸出 PWM 至 GPIO 2（頻率 5 kHz、8-bit 解析度 0~255）
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = BOARD_PIN_BACKLIGHT_PWM,
        .duty           = 0,
        .hpoint         = 0,
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel: %s", esp_err_to_name(err));
        return err;
    }

    s_board_initialized = true;
    ESP_LOGI(TAG, "Board BSP initialized: I2C0, SPI2, Button(GPIO 1), LEDC PWM(GPIO 2) ready");
    return ESP_OK;
}

esp_err_t board_bsp_init(void)
{
    return board_init();
}

bool board_btn_is_pressed(void)
{
    return (gpio_get_level((gpio_num_t)BOARD_PIN_MODE_BTN) == 0);
}

esp_err_t board_set_backlight(uint8_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    return err;
}

/* =========================================================================
 * 執行緒安全 I2C 讀寫代理函式（自動加解 xI2cBusMutex，逾時 100ms）
 * ========================================================================= */

esp_err_t board_i2c_write(uint8_t dev_addr, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex = board_get_i2c_mutex();
    if (mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(I2C_BUS_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "I2C bus mutex take timeout on write to 0x%02X", dev_addr);
        return ESP_ERR_TIMEOUT;
    }

    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t err = get_or_add_i2c_device(dev_addr, &dev_handle);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev_handle, data, len, I2C_BUS_TIMEOUT_MS);
    }

    xSemaphoreGive(mutex);
    return err;
}

esp_err_t board_i2c_read(uint8_t dev_addr, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex = board_get_i2c_mutex();
    if (mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(I2C_BUS_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "I2C bus mutex take timeout on read from 0x%02X", dev_addr);
        return ESP_ERR_TIMEOUT;
    }

    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t err = get_or_add_i2c_device(dev_addr, &dev_handle);
    if (err == ESP_OK) {
        err = i2c_master_receive(dev_handle, data, len, I2C_BUS_TIMEOUT_MS);
    }

    xSemaphoreGive(mutex);
    return err;
}

esp_err_t board_i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, size_t tx_len,
                               uint8_t *rx_data, size_t rx_len)
{
    if (tx_data == NULL || tx_len == 0 || rx_data == NULL || rx_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex = board_get_i2c_mutex();
    if (mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(I2C_BUS_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "I2C bus mutex take timeout on write_read to 0x%02X", dev_addr);
        return ESP_ERR_TIMEOUT;
    }

    i2c_master_dev_handle_t dev_handle = NULL;
    esp_err_t err = get_or_add_i2c_device(dev_addr, &dev_handle);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev_handle, tx_data, tx_len, rx_data, rx_len, I2C_BUS_TIMEOUT_MS);
    }

    xSemaphoreGive(mutex);
    return err;
}

bool board_i2c_is_device_online(uint8_t dev_addr)
{
    if (s_i2c_bus_handle == NULL) {
        return false;
    }

    SemaphoreHandle_t mutex = board_get_i2c_mutex();
    if (mutex != NULL && xSemaphoreTake(mutex, pdMS_TO_TICKS(I2C_BUS_TIMEOUT_MS)) == pdTRUE) {
        esp_err_t ret = i2c_master_probe(s_i2c_bus_handle, dev_addr, I2C_BUS_TIMEOUT_MS);
        xSemaphoreGive(mutex);
        return (ret == ESP_OK);
    }
    return false;
}

i2s_chan_handle_t board_get_i2s_rx_chan(void)
{
    return s_i2s_rx_handle;
}

i2s_chan_handle_t board_get_i2s_tx_chan(void)
{
    return s_i2s_tx_handle;
}

/* =========================================================================
 * 標準校驗演算法（CRC16-MODBUS & IEEE 802.3 CRC32）
 * ========================================================================= */

uint16_t board_calc_crc16(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

uint32_t board_calc_crc32(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

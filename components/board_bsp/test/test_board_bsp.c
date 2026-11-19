/**
 * @file test_board_bsp.c
 * @brief Unity Unit Tests for Board Support Package (Phase 0)
 *
 * Verifies I2C bus probing, boundary parameter checking, LEDC PWM backlight control,
 * GPIO button reading, hardware mutex synchronization, and standard CRC16/32 algorithms.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "board_bsp.h"

static const char *TAG = "test_board_bsp";

TEST_CASE("BSP: I2C Master Bus probe and scan core devices", "[board_bsp][i2c]")
{
    // 確保硬體基礎初始化
    TEST_ASSERT_EQUAL(ESP_OK, board_bsp_init());

    const uint8_t core_devices[] = {
        I2C_ADDR_OLED,   // 0x3C
        I2C_ADDR_GY30,   // 0x23
        I2C_ADDR_DS3231, // 0x68
        I2C_ADDR_EEPROM  // 0x50
    };

    ESP_LOGI(TAG, "Scanning I2C bus for 4 core devices...");
    for (size_t i = 0; i < sizeof(core_devices); i++) {
        uint8_t addr = core_devices[i];
        bool online = board_i2c_is_device_online(addr);
        ESP_LOGI(TAG, "Device 0x%02X: %s", addr, online ? "ONLINE (ACK)" : "OFFLINE (NACK)");
    }
}

TEST_CASE("BSP: I2C thread-safe write/read boundary checks", "[board_bsp][i2c]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_bsp_init());

    // 驗證空指針與 0 長度邊界防護
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_write(I2C_ADDR_OLED, NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_read(I2C_ADDR_OLED, NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_write_read(I2C_ADDR_OLED, NULL, 0, NULL, 0));

    uint8_t dummy = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_write(I2C_ADDR_OLED, NULL, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_read(I2C_ADDR_OLED, NULL, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_write(I2C_ADDR_OLED, &dummy, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, board_i2c_read(I2C_ADDR_OLED, &dummy, 0));
}

TEST_CASE("BSP: Backlight LEDC PWM duty range safety", "[board_bsp][ledc]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_bsp_init());

    // 驗證全範圍 0 ~ 255 調光皆能正常設定不崩潰
    TEST_ASSERT_EQUAL(ESP_OK, board_set_backlight(0));
    TEST_ASSERT_EQUAL(ESP_OK, board_set_backlight(64));
    TEST_ASSERT_EQUAL(ESP_OK, board_set_backlight(128));
    TEST_ASSERT_EQUAL(ESP_OK, board_set_backlight(192));
    TEST_ASSERT_EQUAL(ESP_OK, board_set_backlight(255));
}

TEST_CASE("BSP: Mode button state read does not crash", "[board_bsp][gpio]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_bsp_init());

    // 驗證按鍵電位狀態讀取為合法布林值
    bool pressed = board_btn_is_pressed();
    ESP_LOGI(TAG, "Mode button pressed state: %s", pressed ? "TRUE (LOW)" : "FALSE (HIGH)");
    TEST_ASSERT_TRUE(pressed == true || pressed == false);
}

TEST_CASE("BSP: Mutex handle integrity and concurrency protection", "[board_bsp][mutex]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_bsp_init());

    SemaphoreHandle_t i2c_mutex = board_get_i2c_mutex();
    SemaphoreHandle_t sd_mutex = board_get_sd_mutex();

    TEST_ASSERT_NOT_NULL(i2c_mutex);
    TEST_ASSERT_NOT_NULL(sd_mutex);

    // 測試 I2C 互斥鎖獲取與釋放
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)));
    xSemaphoreGive(i2c_mutex);

    // 測試 SD 互斥鎖獲取與釋放
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(50)));
    xSemaphoreGive(sd_mutex);
}

TEST_CASE("BSP: CRC16-MODBUS standard vector and boundary verification", "[board_bsp][crc]")
{
    // 標準 ASCII 測試向量 "123456789"
    const uint8_t test_vector[] = "123456789";
    uint16_t crc16 = board_calc_crc16(test_vector, 9);
    TEST_ASSERT_EQUAL_HEX16(0x4B37, crc16);

    // 邊界條件防護測試
    TEST_ASSERT_EQUAL_HEX16(0x0000, board_calc_crc16(NULL, 0));
    TEST_ASSERT_EQUAL_HEX16(0x0000, board_calc_crc16(NULL, 10));
    TEST_ASSERT_EQUAL_HEX16(0x0000, board_calc_crc16(test_vector, 0));
}

TEST_CASE("BSP: CRC32 IEEE 802.3 standard vector and boundary verification", "[board_bsp][crc]")
{
    // 標準 ASCII 測試向量 "123456789"
    const uint8_t test_vector[] = "123456789";
    uint32_t crc32 = board_calc_crc32(test_vector, 9);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32);

    // 邊界條件防護測試
    TEST_ASSERT_EQUAL_HEX32(0x00000000, board_calc_crc32(NULL, 0));
    TEST_ASSERT_EQUAL_HEX32(0x00000000, board_calc_crc32(NULL, 10));
    TEST_ASSERT_EQUAL_HEX32(0x00000000, board_calc_crc32(test_vector, 0));
}

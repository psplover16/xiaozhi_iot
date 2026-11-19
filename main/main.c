/**
 * @file main.c
 * @brief System entry point for Xiaozhi IoT
 *
 * Implements single initialization of NVS, Netif, Event Loop, hardware self-check,
 * and boot mode branching (Unity Test Menu vs Normal Execution).
 */

#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "board_bsp.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== Xiaozhi IoT Booting Up ===");

    // 1. 系統底層服務單次初始化（spec.md 1.2 規則 10 / plan.md 4.1）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. 開機硬體記憶體自檢日誌（spec.md AC-01.1）
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "Hardware Report -> Flash: 16MB, PSRAM: %u MB (Octal)",
             (unsigned int)(psram_size / (1024 * 1024)));
    if (psram_size == 0) {
        ESP_LOGE(TAG, "CRITICAL: PSRAM size is 0 or not detected!");
    }

    // 3. 硬體抽象基礎層初始化（I2C, SPI2, 按鍵 GPIO 1, LEDC PWM GPIO 2）
    ESP_ERROR_CHECK(board_bsp_init());

    // 4. 開機測試分流判斷（T-00-04 規格 / plan.md 4.1）
    ESP_LOGI(TAG, "Checking mode button (GPIO 1) status (delay 300ms)...");
    vTaskDelay(pdMS_TO_TICKS(300));

    if (board_btn_is_pressed()) {
        ESP_LOGW(TAG, "Mode button held for >300ms: Entering Unity Test Menu...");
        unity_run_menu();
    } else {
        ESP_LOGI(TAG, "Mode button not pressed: Continuing normal startup...");
    }
}

/**
 * @file main.c
 * @brief System entry point and top-level zero-coupling orchestrator for Xiaozhi IoT
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_runner.h"

#include "board_bsp.h"
#include "psram_cache.h"
#include "eeprom_blackbox.h"
#include "sd_media.h"
#include "env_sensors.h"
#include "sensor_hub.h"
#include "oled_ui.h"
#include "audio_engine.h"
#include "net_manager.h"
#include "app_bridges.h"
#include "app_sr_bridge.h"

/* 硬體未到貨前設為 1，直接進入 Unity 測試選單；實機部署時改回 0 */
#define FORCE_UNITY_TEST_MENU    0

/* 512 KB PSRAM 環形快取容量（32,768 筆 x 16 Bytes） */
#define PSRAM_CACHE_CAPACITY_REC ((512U * 1024U) / sizeof(sensor_record_t))

static const char *TAG = "main";

static void on_net_got_ip_cb(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Mode 1 取得 IP，啟動 SNTP 校時並同步寫入 DS3231");
    (void)sensor_hub_sync_ntp();
}

static void on_net_mode_changed_cb(net_mode_t mode, const char *info, void *ctx)
{
    (void)ctx;
    oled_ui_set_net_status(mode, info);
}

static void print_boot_memory_report(void)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_internal  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t min_spiram    = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "=== Boot Memory Health Report ===");
    ESP_LOGI(TAG, "Internal SRAM Free: %u KB (Min Watermark: %u KB)",
             (unsigned)(free_internal / 1024U), (unsigned)(min_internal / 1024U));
    ESP_LOGI(TAG, "Octal PSRAM   Free: %u KB (Min Watermark: %u KB)",
             (unsigned)(free_spiram / 1024U), (unsigned)(min_spiram / 1024U));
}

void app_main(void)
{
    esp_rom_printf("\r\n[BOOT_CHECK] Reached app_main!\r\n");
    ESP_LOGI(TAG, "=== Xiaozhi IoT Booting Up ===");

    /* 1. 系統底層服務單次初始化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. 開機硬體記憶體自檢日誌 */
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "Hardware Report -> Flash: 16MB, PSRAM: %u MB (Octal)",
             (unsigned)(psram_size / (1024U * 1024U)));
    if (psram_size == 0U) {
        ESP_LOGE(TAG, "CRITICAL: PSRAM size is 0 or not detected!");
    }

    /* 3. 第 0 層硬體抽象基礎層初始化（I2C, SPI2, GPIO 1 按鍵, GPIO 2 LEDC PWM） */
    ESP_ERROR_CHECK(board_bsp_init());

    /* 4. 開機測試模式分流（長按 GPIO 1 > 300ms 進入 Unity 測試選單） */
    ESP_LOGI(TAG, "Checking mode button (GPIO 1) status (delay 300ms)...");
    vTaskDelay(pdMS_TO_TICKS(300));

    if (FORCE_UNITY_TEST_MENU || board_btn_is_pressed()) {
        esp_rom_printf("[BOOT_CHECK] Launching Unity Test Menu...\r\n");
        ESP_LOGW(TAG, "Entering Unity Test Menu...");
        unity_run_menu();
        return;
    }

    ESP_LOGI(TAG, "Mode button not pressed: Initializing application subsystems...");

    /* 5. 初始化第 1 層儲存、採集與顯示元件 */
    app_storage_init_bridge();
    ESP_ERROR_CHECK(psram_cache_init(PSRAM_CACHE_CAPACITY_REC));

    ESP_ERROR_CHECK_WITHOUT_ABORT(eeprom_bb_init(NULL, NULL));
    eeprom_bb_register_flush_cb(app_storage_eeprom_flush_to_sd);

    if (sd_media_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD card not mounted at boot: running in degraded storage mode");
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(sensor_hub_init(app_storage_on_sensor_record));
    ESP_ERROR_CHECK_WITHOUT_ABORT(oled_ui_init());

    /* 6. 初始化第 2 層雙核心業務元件並註冊零耦合 Hook */
    ESP_ERROR_CHECK_WITHOUT_ABORT(audio_engine_init(app_voice_on_command));
    audio_engine_register_event_cb(app_voice_on_audio_event);

    const net_manager_hooks_t net_hooks = {
        .on_got_ip          = on_net_got_ip_cb,
        .on_mode_changed    = on_net_mode_changed_cb,
        .on_offline_record  = app_storage_on_offline_record,
        .get_storage_status = app_storage_get_status,
        .on_tts_audio_ready = app_voice_on_tts_ready,
        .on_play_audio_file = app_voice_on_play_file,
        .is_audio_busy      = app_voice_is_audio_busy,
        .get_sys_status     = app_voice_get_sys_status,
        .exec_ctrl_cmd      = app_voice_exec_ctrl_cmd,
        .user_ctx           = NULL,
    };
    net_manager_register_hooks(&net_hooks);
    app_storage_register_sync_sources();
    ESP_ERROR_CHECK(net_manager_init());

    /* 7. 啟動 Core 1 即時任務與離線語音識別橋接器 */
    ESP_ERROR_CHECK_WITHOUT_ABORT(sensor_hub_start_task());
    ESP_ERROR_CHECK_WITHOUT_ABORT(audio_engine_start_task());
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_sr_bridge_init());

    /* 8. 印出開機可用內部 SRAM 與 Octal PSRAM 剩餘容量報告 */
    print_boot_memory_report();
    ESP_LOGI(TAG, "=== Xiaozhi IoT Startup Complete (Core 0: Net/Sync/TTS | Core 1: Audio/Sensors/UI) ===");
}
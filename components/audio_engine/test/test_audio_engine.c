#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>

#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "board_bsp.h"
#include "sd_media.h"
#include "audio_engine.h"

static const char *TAG = "test_audio_engine";

static SemaphoreHandle_t s_cmd_sem = NULL;
static volatile voice_cmd_id_t s_last_cmd = VOICE_CMD_NONE;

static void test_voice_cmd_cb(voice_cmd_id_t cmd)
{
    s_last_cmd = cmd;
    if (s_cmd_sem != NULL) {
        xSemaphoreGive(s_cmd_sem);
    }
}

static void ensure_audio_test_env(void)
{
    if (s_cmd_sem == NULL) {
        s_cmd_sem = xSemaphoreCreateBinary();
        TEST_ASSERT_NOT_NULL(s_cmd_sem);
    }
    (void)board_bsp_init();
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_init(test_voice_cmd_cb));
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_start_task());
}

TEST_CASE("PSRAM buffer allocation and Core 1 task init", "[audio_engine]")
{
    ensure_audio_test_env();

    const int16_t *pcm_ptr = NULL;
    size_t pcm_bytes = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_get_recorded_pcm(&pcm_ptr, &pcm_bytes));
    TEST_ASSERT_NOT_NULL(pcm_ptr);
    TEST_ASSERT_TRUE_MESSAGE(esp_ptr_external_ram(pcm_ptr),
                             "Voice recording buffer must be allocated in Octal PSRAM");
}

TEST_CASE("Dynamic I2S TX sample rate switching without memory leak", "[audio_engine]")
{
    ensure_audio_test_env();
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_stop_playback());

    /* 先執行一次預熱，排除首次驅動內部狀態初始化的一次性配置 */
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_reconfig_tx_rate(16000U));

    size_t free_int_before   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    const uint32_t rates[] = { 16000U, 44100U, 48000U, 22050U, 16000U };
    for (int round = 0; round < 10; round++) {
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
            esp_err_t err = audio_engine_reconfig_tx_rate(rates[i]);
            TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err, "I2S TX clock reconfig failed");
        }
    }

    size_t free_int_after   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "Heap check - Internal: %u -> %u, PSRAM: %u -> %u",
             (unsigned)free_int_before, (unsigned)free_int_after,
             (unsigned)free_psram_before, (unsigned)free_psram_after);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)free_int_before, (uint32_t)free_int_after);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)free_psram_before, (uint32_t)free_psram_after);
}

TEST_CASE("Voice command injection and Core 1 dispatch", "[audio_engine]")
{
    ensure_audio_test_env();

    (void)xSemaphoreTake(s_cmd_sem, 0);
    s_last_cmd = VOICE_CMD_NONE;

    audio_engine_inject_cmd(VOICE_CMD_REPORT_ENV);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_cmd_sem, pdMS_TO_TICKS(150)));
    TEST_ASSERT_EQUAL(VOICE_CMD_REPORT_ENV, s_last_cmd);

    audio_engine_inject_cmd(VOICE_CMD_WIFI_ON);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_cmd_sem, pdMS_TO_TICKS(150)));
    TEST_ASSERT_EQUAL(VOICE_CMD_WIFI_ON, s_last_cmd);
}

TEST_CASE("Software digital volume scaling and mute control", "[audio_engine]")
{
    ensure_audio_test_env();

    /* 1. 100% 音量保持原值 */
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_set_mute(false));
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_set_volume(100U));
    int16_t buf_full[4] = { 10000, -10000, 20000, -20000 };
    audio_engine_apply_volume_pcm(buf_full, 4);
    TEST_ASSERT_EQUAL_INT16(10000, buf_full[0]);
    TEST_ASSERT_EQUAL_INT16(-10000, buf_full[1]);

    /* 2. 50% 音量（對數近似曲線 (50/100)^2 = 0.25 倍） */
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_set_volume(50U));
    int16_t buf_half[2] = { 10000, -10000 };
    audio_engine_apply_volume_pcm(buf_half, 2);
    TEST_ASSERT_INT16_WITHIN(2, 2500, buf_half[0]);
    TEST_ASSERT_INT16_WITHIN(2, -2500, buf_half[1]);

    /* 3. 相對步進調整與邊界鉗制 */
    TEST_ASSERT_EQUAL_UINT8(30U, audio_engine_adjust_volume(-20));
    TEST_ASSERT_EQUAL_UINT8(0U, audio_engine_adjust_volume(-50));
    TEST_ASSERT_EQUAL_UINT8(100U, audio_engine_adjust_volume(120));

    /* 4. 靜音強制歸零 */
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_set_mute(true));
    int16_t buf_mute[2] = { 12345, -12345 };
    audio_engine_apply_volume_pcm(buf_mute, 2);
    TEST_ASSERT_EQUAL_INT16(0, buf_mute[0]);
    TEST_ASSERT_EQUAL_INT16(0, buf_mute[1]);

    /* 還原預設 80% 音量 */
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_set_mute(false));
    TEST_ASSERT_EQUAL(ESP_OK, audio_engine_set_volume(80U));
}

TEST_CASE("Playback barge-in stop latency within 50 ms", "[audio_engine]")
{
    ensure_audio_test_env();
    (void)sd_media_init();

    const char *candidates[] = {
        "/sdcard/test.wav",
        "/sdcard/test.mp3",
        "/sdcard/audio/offline_hint.mp3"
    };
    const char *found_file = NULL;
    struct stat st;

    if (sd_media_is_mounted()) {
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            if (stat(candidates[i], &st) == 0 && st.st_size > 0) {
                found_file = candidates[i];
                break;
            }
        }
    }

    if (found_file != NULL) {
        ESP_LOGI(TAG, "Testing barge-in stop on %s", found_file);
        TEST_ASSERT_EQUAL(ESP_OK, audio_engine_play_file(found_file));
        vTaskDelay(pdMS_TO_TICKS(30));
    } else {
        ESP_LOGW(TAG, "No SD audio file found; verifying idle stop_playback fast-return path");
    }

    int64_t t0_us = esp_timer_get_time();
    esp_err_t stop_err = audio_engine_stop_playback();
    int64_t elapsed_ms = (esp_timer_get_time() - t0_us) / 1000LL;

    ESP_LOGI(TAG, "audio_engine_stop_playback() took %" PRIi64 " ms", elapsed_ms);
    TEST_ASSERT_EQUAL(ESP_OK, stop_err);
    TEST_ASSERT_FALSE(sd_media_is_playing());
    TEST_ASSERT_LESS_OR_EQUAL_INT64(50LL, elapsed_ms);
}

static volatile uint32_t s_tap_call_count = 0;

static void test_pcm_tap_cb(const int16_t *stereo_pcm, size_t frames)
{
    (void)stereo_pcm;
    (void)frames;
    s_tap_call_count++;
}

TEST_CASE("PCM Tap registration and unregistration", "[audio_engine]")
{
    ensure_audio_test_env();

    s_tap_call_count = 0;
    audio_engine_register_pcm_tap(test_pcm_tap_cb);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 取消註冊 (傳入 NULL) 後不應發生空指標崩潰，且計數不再增加 */
    audio_engine_register_pcm_tap(NULL);
    uint32_t count_after_unreg = s_tap_call_count;
    vTaskDelay(pdMS_TO_TICKS(50));

    TEST_ASSERT_EQUAL_UINT32(count_after_unreg, s_tap_call_count);
}
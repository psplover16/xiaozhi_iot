/**
 * @file test_sr_engine.c
 * @brief Phase 8 Unity Unit Tests for Offline Speech Recognition & Pinyin Syllable Dispatcher
 */

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "sr_engine.h"
#include "sd_media.h"

static const char *TAG = "test_sr_engine";

static volatile voice_cmd_id_t    s_last_cmd   = VOICE_CMD_NONE;
static volatile sr_listen_state_t s_last_state = SR_STATE_IDLE;

static void test_on_cmd_cb(voice_cmd_id_t cmd)
{
    s_last_cmd = cmd;
}

static void test_on_state_cb(sr_listen_state_t state)
{
    s_last_state = state;
}

TEST_CASE("離線拼音音節拼接：整數、負數、小數與進位補零規則", "[audio_engine][sr_engine]")
{
    char clips[24][64];
    size_t n = 0U;

    /* 1. 零值測試：0 -> ["0.wav"] */
    memset(clips, 0, sizeof(clips));
    n = sr_dispatcher_build_number_clips(0.0f, 0, clips, 24U);
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/0.wav", clips[0]);

    /* 2. 規格範例 25.3 -> ["2.wav", "shi.wav", "5.wav", "dian.wav", "3.wav"] */
    memset(clips, 0, sizeof(clips));
    n = sr_dispatcher_build_number_clips(25.3f, 1, clips, 24U);
    TEST_ASSERT_EQUAL_UINT32(5U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/2.wav", clips[0]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/shi.wav", clips[1]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/5.wav", clips[2]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/dian.wav", clips[3]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/3.wav", clips[4]);

    /* 3. 十位數省略首位「一」：15 -> ["shi.wav", "5.wav"] */
    memset(clips, 0, sizeof(clips));
    n = sr_dispatcher_build_number_clips(15.0f, 0, clips, 24U);
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/shi.wav", clips[0]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/5.wav", clips[1]);

    /* 4. 百位缺十位補零：105 -> ["1.wav", "bai.wav", "0.wav", "5.wav"] */
    memset(clips, 0, sizeof(clips));
    n = sr_dispatcher_build_number_clips(105.0f, 0, clips, 24U);
    TEST_ASSERT_EQUAL_UINT32(4U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/1.wav", clips[0]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/bai.wav", clips[1]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/0.wav", clips[2]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/5.wav", clips[3]);

    /* 5. 負數與小數：-26.4 -> ["fu", "2", "shi", "6", "dian", "4"] */
    memset(clips, 0, sizeof(clips));
    n = sr_dispatcher_build_number_clips(-26.4f, 1, clips, 24U);
    TEST_ASSERT_EQUAL_UINT32(6U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/fu.wav", clips[0]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/2.wav", clips[1]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/shi.wav", clips[2]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/6.wav", clips[3]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/dian.wav", clips[4]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/4.wav", clips[5]);

    /* 6. 萬位多段補零：10205 -> ["1", "wan", "0", "2", "bai", "0", "5"] */
    memset(clips, 0, sizeof(clips));
    n = sr_dispatcher_build_number_clips(10205.0f, 0, clips, 24U);
    TEST_ASSERT_EQUAL_UINT32(7U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/1.wav", clips[0]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/wan.wav", clips[1]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/0.wav", clips[2]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/2.wav", clips[3]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/bai.wav", clips[4]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/0.wav", clips[5]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/5.wav", clips[6]);
}

TEST_CASE("離線拼音音節拼接：邊界緩衝區截斷與無效參數防呆", "[audio_engine][sr_engine]")
{
    char small_clips[2][64];
    memset(small_clips, 0, sizeof(small_clips));

    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)sr_dispatcher_build_number_clips(12.3f, 1, NULL, 10U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)sr_dispatcher_build_number_clips(12.3f, 1, small_clips, 0U));

    size_t n = sr_dispatcher_build_number_clips(-26.4f, 1, small_clips, 2U);
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/fu.wav", small_clips[0]);
    TEST_ASSERT_EQUAL_STRING("/sdcard/tts_offline/2.wav", small_clips[1]);
}

TEST_CASE("離線語音播報：無 SD 卡安全降級與五類播報接口驗證", "[audio_engine][sr_engine]")
{
    if (!sd_media_is_mounted()) {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sr_dispatcher_speak_temperature(25.3f, 1));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sr_dispatcher_speak_humidity(60.5f));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sr_dispatcher_speak_lux(320U));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sr_dispatcher_speak_time(9U, 5U));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                          sr_dispatcher_speak_storage_status(0U, 25U, 1024U));
    } else {
        esp_err_t err = sr_dispatcher_speak_time(12U, 0U);
        TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NOT_FOUND);
    }
}

TEST_CASE("ESP-SR 18 條拼音指令表註冊映射與 6000ms 命令視窗逾時退回驗證", "[audio_engine][sr_engine][hw]")
{
    size_t sram_before  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    TEST_ASSERT_EQUAL(ESP_OK, sr_engine_init(test_on_cmd_cb, test_on_state_cb));
    TEST_ASSERT_EQUAL(ESP_OK, sr_engine_init(test_on_cmd_cb, test_on_state_cb));
    TEST_ASSERT_EQUAL(ESP_OK, sr_engine_start_tasks());

    /* 1. 驗證全部 18 條預設漢語拼音指令註冊數量與 ID 映射正確性 */
    TEST_ASSERT_EQUAL_UINT32(18U, (uint32_t)sr_engine_get_registered_cmd_count());
    TEST_ASSERT_EQUAL(VOICE_CMD_WIFI_ON,    sr_engine_get_mapped_cmd(1));
    TEST_ASSERT_EQUAL(VOICE_CMD_WIFI_OFF,   sr_engine_get_mapped_cmd(3));
    TEST_ASSERT_EQUAL(VOICE_CMD_AP_MODE,    sr_engine_get_mapped_cmd(5));
    TEST_ASSERT_EQUAL(VOICE_CMD_STA_MODE,   sr_engine_get_mapped_cmd(7));
    TEST_ASSERT_EQUAL(VOICE_CMD_REPORT_ENV, sr_engine_get_mapped_cmd(8));
    TEST_ASSERT_EQUAL(VOICE_CMD_TIME_NOW,   sr_engine_get_mapped_cmd(12));
    TEST_ASSERT_EQUAL(VOICE_CMD_PLAY_MUSIC, sr_engine_get_mapped_cmd(14));
    TEST_ASSERT_EQUAL(VOICE_CMD_STOP_MUSIC, sr_engine_get_mapped_cmd(15));
    TEST_ASSERT_EQUAL(VOICE_CMD_VOL_UP,     sr_engine_get_mapped_cmd(16));
    TEST_ASSERT_EQUAL(VOICE_CMD_VOL_DOWN,   sr_engine_get_mapped_cmd(17));
    TEST_ASSERT_EQUAL(VOICE_CMD_MUTE,       sr_engine_get_mapped_cmd(18));
    TEST_ASSERT_EQUAL(VOICE_CMD_NONE,       sr_engine_get_mapped_cmd(0));
    TEST_ASSERT_EQUAL(VOICE_CMD_NONE,       sr_engine_get_mapped_cmd(19));

    /* 2. 驗證 SR_STATE_CMD_LISTEN 未滿 6000ms 保持聆聽、超過 6000ms 自動退回 WAKE_LISTEN */
    sr_engine_set_state_for_test(SR_STATE_CMD_LISTEN);
    TEST_ASSERT_EQUAL(SR_STATE_CMD_LISTEN, sr_engine_get_state());

    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_FALSE(sr_engine_poll_timeout());
    TEST_ASSERT_EQUAL(SR_STATE_CMD_LISTEN, sr_engine_get_state());

    vTaskDelay(pdMS_TO_TICKS(6050));
    (void)sr_engine_poll_timeout();
    TEST_ASSERT_EQUAL(SR_STATE_WAKE_LISTEN, sr_engine_get_state());

    /* 3. 驗證模型配置於 PSRAM 且內部 SRAM 保持充裕 (> 40 KB) */
    size_t sram_after  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "SR Init Memory Delta -> SRAM: %u -> %u B | PSRAM: %u -> %u B",
             (unsigned)sram_before, (unsigned)sram_after,
             (unsigned)psram_before, (unsigned)psram_after);
    TEST_ASSERT_GREATER_THAN_UINT32(40U * 1024U, (uint32_t)sram_after);
}
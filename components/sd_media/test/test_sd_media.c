#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "sd_media.h"
#include "system_types.h"

TEST_CASE("sd_media: audio format detection & argument validation", "[sd_media]")
{
    // 1. 副檔名格式偵測（含大小寫與無效路徑邊界）
    TEST_ASSERT_EQUAL(AUDIO_FMT_WAV, sd_media_detect_format("/sdcard/alert.wav"));
    TEST_ASSERT_EQUAL(AUDIO_FMT_WAV, sd_media_detect_format("/sdcard/ALERT.WAV"));
    TEST_ASSERT_EQUAL(AUDIO_FMT_MP3, sd_media_detect_format("/sdcard/music/bgm.mp3"));
    TEST_ASSERT_EQUAL(AUDIO_FMT_MP3, sd_media_detect_format("/sdcard/music/BGM.Mp3"));
    TEST_ASSERT_EQUAL(AUDIO_FMT_UNKNOWN, sd_media_detect_format("/sdcard/notes.txt"));
    TEST_ASSERT_EQUAL(AUDIO_FMT_UNKNOWN, sd_media_detect_format("/sdcard/no_extension"));
    TEST_ASSERT_EQUAL(AUDIO_FMT_UNKNOWN, sd_media_detect_format(NULL));

    // 2. 空指標與無效參數防呆
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_media_play_audio(NULL));

    sensor_record_t dummy_rec = {
        .timestamp    = 1700000000U,
        .temperature  = 250,
        .humidity     = 600U,
        .lux          = 300U,
        .status_flags = REC_FLAG_DHT22_VALID,
        .crc16        = 0U,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_media_append_offline_records(20261119U, NULL, 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sd_media_append_offline_records(20261119U, &dummy_rec, 0U));

    // 3. 播放狀態初始檢查
    sd_media_stop_audio();
    TEST_ASSERT_FALSE(sd_media_is_playing());
}

TEST_CASE("sd_media: degraded mode guard & conditional SD log append CRC32", "[sd_media]")
{
    if (!sd_media_is_mounted()) {
        (void)sd_media_init();
    }

    sensor_record_t batch1[3] = {
        { .timestamp = 1700000010U, .temperature = 245, .humidity = 580U, .lux = 210U, .status_flags = REC_FLAG_DHT22_VALID, .crc16 = 0U },
        { .timestamp = 1700000020U, .temperature = 246, .humidity = 582U, .lux = 215U, .status_flags = REC_FLAG_DHT22_VALID, .crc16 = 0U },
        { .timestamp = 1700000030U, .temperature = 247, .humidity = 585U, .lux = 220U, .status_flags = REC_FLAG_DHT22_VALID, .crc16 = 0U },
    };

    if (!sd_media_is_mounted()) {
        // 未插卡降級防護：不當機、安全回傳預設值與 ESP_ERR_INVALID_STATE
        TEST_ASSERT_EQUAL_UINT32(0U, sd_media_get_free_mb());
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sd_media_play_audio("/sdcard/test.wav"));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sd_media_append_offline_records(20261119U, batch1, 3U));
    } else {
        // 實體插卡環境驗證：不支援格式攔截 ＋ 兩次 "ab" 附加寫入與 CRC32 回讀驗證
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, sd_media_play_audio("/sdcard/unsupported.ogg"));

        const uint32_t test_date = 20991231U;
        TEST_ASSERT_EQUAL(ESP_OK, sd_media_append_offline_records(test_date, batch1, 3U));

        sensor_record_t batch2[2] = {
            { .timestamp = 1700000040U, .temperature = 248, .humidity = 590U, .lux = 230U, .status_flags = REC_FLAG_GY30_VALID, .crc16 = 0U },
            { .timestamp = 1700000050U, .temperature = 249, .humidity = 595U, .lux = 240U, .status_flags = REC_FLAG_GY30_VALID, .crc16 = 0U },
        };
        TEST_ASSERT_EQUAL(ESP_OK, sd_media_append_offline_records(test_date, batch2, 2U));

        // 清理測試日誌檔，避免殘留垃圾檔案
        unlink("/sdcard/logs/20991231.bin");
    }
}
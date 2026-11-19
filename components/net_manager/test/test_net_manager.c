#include "unity.h"
#include "net_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

typedef struct {
    sensor_record_t items[8];
    size_t          total;
    size_t          head;
} mock_ring_src_t;

static uint32_t s_uploaded_ts[16];
static size_t   s_uploaded_cnt = 0;
static size_t   s_fail_after   = 999;
static char     s_played_path[96];

static esp_err_t mock_peek_batch(sensor_record_t *out_buf, size_t max_count,
                                 size_t *out_count, void *ctx)
{
    mock_ring_src_t *src = (mock_ring_src_t *)ctx;
    size_t avail = src->total - src->head;
    size_t n = (avail < max_count) ? avail : max_count;
    for (size_t i = 0; i < n; i++) {
        out_buf[i] = src->items[src->head + i];
    }
    *out_count = n;
    return ESP_OK;
}

static esp_err_t mock_ack_batch(size_t count, void *ctx)
{
    mock_ring_src_t *src = (mock_ring_src_t *)ctx;
    src->head += count;
    return ESP_OK;
}

static esp_err_t mock_upload_fn(const sensor_record_t *batch, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (s_uploaded_cnt >= s_fail_after) {
            return ESP_FAIL;
        }
        s_uploaded_ts[s_uploaded_cnt++] = batch[i].timestamp;
    }
    return ESP_OK;
}

static esp_err_t mock_play_file_hook(const char *filepath, void *ctx)
{
    (void)ctx;
    strncpy(s_played_path, filepath, sizeof(s_played_path) - 1U);
    s_played_path[sizeof(s_played_path) - 1U] = '\0';
    return ESP_OK;
}

TEST_CASE("web_config_validate accepts valid config and rejects out-of-range fields", "[net_manager]")
{
    sys_config_t cfg;
    char err_msg[96];

    memset(&cfg, 0, sizeof(cfg));
    cfg.magic               = SYS_CONFIG_MAGIC;
    strcpy(cfg.wifi_ssid, "MyHomeWiFi");
    strcpy(cfg.wifi_pass, "12345678");
    strcpy(cfg.server_url, "https://example.com/api");
    cfg.sample_interval_sec = 30;
    cfg.psram_window_hours  = 1;
    cfg.eeprom_warn_pct     = 80;
    cfg.sd_warn_free_mb     = 100;

    TEST_ASSERT_EQUAL(ESP_OK, web_config_validate(&cfg, err_msg, sizeof(err_msg)));

    cfg.sample_interval_sec = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, web_config_validate(&cfg, err_msg, sizeof(err_msg)));
    cfg.sample_interval_sec = 30;

    cfg.sd_warn_free_mb = 10;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, web_config_validate(&cfg, err_msg, sizeof(err_msg)));
}

TEST_CASE("sync_engine drains oldest sources first and preserves unACKed records on disconnect", "[net_manager]")
{
    mock_ring_src_t older_src = { .total = 3, .head = 0 };
    mock_ring_src_t newer_src = { .total = 2, .head = 0 };

    older_src.items[0].timestamp = 1000;
    older_src.items[1].timestamp = 1010;
    older_src.items[2].timestamp = 1020;
    newer_src.items[0].timestamp = 2000;
    newer_src.items[1].timestamp = 2010;

    sync_engine_clear_sources();
    sync_source_t s0 = { .name = "older_tier", .peek_oldest_batch = mock_peek_batch, .ack_oldest_batch = mock_ack_batch, .ctx = &older_src };
    sync_source_t s1 = { .name = "newer_tier", .peek_oldest_batch = mock_peek_batch, .ack_oldest_batch = mock_ack_batch, .ctx = &newer_src };
    TEST_ASSERT_EQUAL(ESP_OK, sync_engine_register_source(&s0));
    TEST_ASSERT_EQUAL(ESP_OK, sync_engine_register_source(&s1));

    s_uploaded_cnt = 0;
    s_fail_after   = 3;
    TEST_ASSERT_EQUAL(ESP_FAIL, sync_engine_execute(mock_upload_fn));
    TEST_ASSERT_FALSE(sync_engine_is_live_unlocked());
    TEST_ASSERT_EQUAL(3, older_src.head);
    TEST_ASSERT_EQUAL(0, newer_src.head);

    s_fail_after = 999;
    TEST_ASSERT_EQUAL(ESP_OK, sync_engine_execute(mock_upload_fn));
    TEST_ASSERT_TRUE(sync_engine_is_live_unlocked());
    TEST_ASSERT_EQUAL(5, s_uploaded_cnt);
    TEST_ASSERT_EQUAL(1000, s_uploaded_ts[0]);
    TEST_ASSERT_EQUAL(2010, s_uploaded_ts[4]);

    sync_engine_clear_sources();
}

TEST_CASE("voice_cloud builds endpoint URL and routes offline speech to local hint audio", "[net_manager]")
{
    char url[128];
    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_build_endpoint("https://api.example.com/", "/voice/chat", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://api.example.com/voice/chat", url);

    TEST_ASSERT_EQUAL(ESP_OK, voice_cloud_build_endpoint("http://192.168.1.10:8080", "voice/chat", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:8080/voice/chat", url);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, voice_cloud_build_endpoint("ws://invalid.local", "/voice/chat", url, sizeof(url)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, net_manager_submit_speech(NULL, 100, 16000));

    net_manager_hooks_t hooks = {
        .on_play_audio_file = mock_play_file_hook,
    };
    net_manager_register_hooks(&hooks);
    memset(s_played_path, 0, sizeof(s_played_path));

    int16_t dummy_pcm[32] = { 100, -100, 200, -200 };
    TEST_ASSERT_EQUAL(ESP_OK, net_manager_init());
    TEST_ASSERT_EQUAL(ESP_OK, net_manager_set_mode(NET_MODE_OFFLINE));
    vTaskDelay(pdMS_TO_TICKS(60));

    TEST_ASSERT_EQUAL(ESP_OK, net_manager_submit_speech(dummy_pcm, sizeof(dummy_pcm), 16000));
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL_STRING(VOICE_OFFLINE_HINT_DEFAULT, s_played_path);
}
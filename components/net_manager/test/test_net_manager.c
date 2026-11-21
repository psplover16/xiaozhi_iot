#include "unity.h"
#include "net_manager.h"
#include "../net_manager_priv.h"
#include <string.h>

static void reset_net_ctrl_state(void)
{
    net_manager_register_hooks(NULL);
    net_mode_ctrl_init(false);
    (void)net_manager_set_wifi_enabled(true);
    net_manager_set_cloud_enabled(true);
}

TEST_CASE("T-OPT-04 button debounce (<50ms) and short press toggle (50~1499ms)", "[net_manager]")
{
    reset_net_ctrl_state();
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());

    /* 1. 彈跳雜訊 (< 50ms) 應直接忽略 */
    net_mode_ctrl_process_button(true, 1000);
    net_mode_ctrl_process_button(false, 1049);
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());

    /* 2. 50ms 邊界短按放開 -> 關閉 Wi-Fi */
    net_mode_ctrl_process_button(true, 2000);
    net_mode_ctrl_process_button(false, 2050);
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_OFF, net_manager_get_wifi_state());
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());

    /* 3. 按住 1499ms 尚未放開 -> 不可誤觸熱點或提前切換 */
    net_mode_ctrl_process_button(true, 3000);
    net_mode_ctrl_process_button(true, 4499);
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());

    /* 4. 1499ms 放開 -> 仍判定為短按，開啟 Wi-Fi 進入 STA 連線狀態 */
    net_mode_ctrl_process_button(false, 4499);
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_CONNECTING, net_manager_get_wifi_state());
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());
}

TEST_CASE("T-OPT-04 button 1500ms immediate SoftAP trigger and release suppression", "[net_manager]")
{
    reset_net_ctrl_state();
    (void)net_manager_set_wifi_enabled(false);
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());

    /* 1. 按下並持續按住至 1499ms，尚未到達 1.5s 單一分界點 */
    net_mode_ctrl_process_button(true, 5000);
    net_mode_ctrl_process_button(true, 6499);
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());

    /* 2. 滿 1500ms 當下（尚未鬆手）立即強制啟動 SoftAP 配網 */
    net_mode_ctrl_process_button(true, 6500);
    TEST_ASSERT_TRUE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_AP_SETUP, net_manager_get_wifi_state());
    TEST_ASSERT_EQUAL_INT(NET_MODE_WEB_CFG, net_manager_get_mode());

    /* 3. 繼續按住到 2200ms 才放開，放開瞬間絕不可誤觸短按把 Wi-Fi 關掉 */
    net_mode_ctrl_process_button(true, 7000);
    net_mode_ctrl_process_button(false, 7200);
    TEST_ASSERT_TRUE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_AP_SETUP, net_manager_get_wifi_state());

    /* 4. 進入熱點後再次短按 -> 正常退出熱點並關閉 Wi-Fi */
    net_mode_ctrl_process_button(true, 8000);
    net_mode_ctrl_process_button(false, 8100);
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_OFF, net_manager_get_wifi_state());

    /* 5. 再次短按 -> 恢復 STA 連線狀態，不殘留強制熱點旗標 */
    net_mode_ctrl_process_button(true, 9000);
    net_mode_ctrl_process_button(false, 9100);
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_CONNECTING, net_manager_get_wifi_state());
}

TEST_CASE("T-OPT-04 boot-time SoftAP force setup and release guard", "[net_manager]")
{
    net_manager_register_hooks(NULL);
    net_mode_ctrl_init(true);

    /* 開機偵測按鍵壓下 -> 直接進入 SoftAP 配網 */
    TEST_ASSERT_TRUE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_AP_SETUP, net_manager_get_wifi_state());

    /* 鬆手防護：開機按鍵放開時不可誤觸短按關閉 Wi-Fi */
    net_mode_ctrl_process_button(true, 100);
    net_mode_ctrl_process_button(false, 300);
    TEST_ASSERT_TRUE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());

    /* 鬆手之後的下一次正常短按 -> 可正常關閉 Wi-Fi */
    net_mode_ctrl_process_button(true, 500);
    net_mode_ctrl_process_button(false, 600);
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());
}

TEST_CASE("T-OPT-03 & T-OPT-04 dual-switch and legacy voice mode alignment", "[net_manager]")
{
    reset_net_ctrl_state();

    /* 離線語音「關閉網路」 -> NET_MODE_OFFLINE */
    TEST_ASSERT_EQUAL_INT(ESP_OK, net_manager_set_mode(NET_MODE_OFFLINE));
    TEST_ASSERT_FALSE(net_manager_is_wifi_enabled());
    TEST_ASSERT_EQUAL_INT(NET_MODE_OFFLINE, net_manager_get_mode());

    /* 離線語音「開啟熱點」 -> NET_MODE_WEB_CFG (強制進入 SoftAP) */
    TEST_ASSERT_EQUAL_INT(ESP_OK, net_manager_set_mode(NET_MODE_WEB_CFG));
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_TRUE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_AP_SETUP, net_manager_get_wifi_state());
    TEST_ASSERT_EQUAL_INT(NET_MODE_WEB_CFG, net_manager_get_mode());

    /* 離線語音「開啟網路」 -> NET_MODE_ONLINE (退出熱點並開啟雲端同步) */
    TEST_ASSERT_EQUAL_INT(ESP_OK, net_manager_set_mode(NET_MODE_ONLINE));
    TEST_ASSERT_TRUE(net_manager_is_wifi_enabled());
    TEST_ASSERT_TRUE(net_manager_is_cloud_enabled());
    TEST_ASSERT_FALSE(net_mode_ctrl_is_ap_mode());
    TEST_ASSERT_EQUAL_INT(WIFI_CONN_STATE_CONNECTING, net_manager_get_wifi_state());

    /* OLED IP 顯示格式與雲端獨立開關驗證 */
    char ip_disp[32] = {0};
    net_manager_format_ip_display("192.168.1.88", true, ip_disp, sizeof(ip_disp));
    TEST_ASSERT_EQUAL_STRING("192.168.1.88", ip_disp);

    net_manager_set_cloud_enabled(false);
    TEST_ASSERT_FALSE(net_mode_ctrl_is_cloud_active());
    net_manager_format_ip_display("192.168.1.88", false, ip_disp, sizeof(ip_disp));
    TEST_ASSERT_EQUAL_STRING("192.168.1.88 [LAN]", ip_disp);
}

static sensor_record_t s_mock_hist[3];
static size_t          s_mock_idx = 0;
static uint32_t        s_uploaded_ts[3];
static size_t          s_uploaded_cnt = 0;

static esp_err_t mock_peek_oldest(sensor_record_t *out_buf, size_t max_count,
                                  size_t *out_count, void *ctx)
{
    (void)ctx;
    size_t rem = (s_mock_idx < 3U) ? (3U - s_mock_idx) : 0U;
    size_t n   = (rem < max_count) ? rem : max_count;
    for (size_t i = 0; i < n; i++) {
        out_buf[i] = s_mock_hist[s_mock_idx + i];
    }
    *out_count = n;
    return ESP_OK;
}

static esp_err_t mock_ack_oldest(size_t count, void *ctx)
{
    (void)ctx;
    s_mock_idx += count;
    return ESP_OK;
}

static esp_err_t mock_upload_batch(const sensor_record_t *batch, size_t count)
{
    for (size_t i = 0; i < count && s_uploaded_cnt < 3U; i++) {
        s_uploaded_ts[s_uploaded_cnt++] = batch[i].timestamp;
    }
    return ESP_OK;
}

TEST_CASE("sync_engine strict FIFO backfill and live lock", "[net_manager]")
{
    memset(s_mock_hist, 0, sizeof(s_mock_hist));
    memset(s_uploaded_ts, 0, sizeof(s_uploaded_ts));
    s_mock_idx     = 0;
    s_uploaded_cnt = 0;

    s_mock_hist[0].timestamp = 1700000100U;
    s_mock_hist[1].timestamp = 1700000200U;
    s_mock_hist[2].timestamp = 1700000300U;

    sync_engine_clear_sources();
    const sync_source_t src = {
        .name              = "mock_fifo_src",
        .peek_oldest_batch = mock_peek_oldest,
        .ack_oldest_batch  = mock_ack_oldest,
        .ctx               = NULL,
    };
    TEST_ASSERT_EQUAL_INT(ESP_OK, sync_engine_register_source(&src));

    sync_engine_lock_live();
    TEST_ASSERT_FALSE(sync_engine_is_live_unlocked());

    TEST_ASSERT_EQUAL_INT(ESP_OK, sync_engine_execute(mock_upload_batch));
    TEST_ASSERT_TRUE(sync_engine_is_live_unlocked());
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)s_uploaded_cnt);
    TEST_ASSERT_EQUAL_UINT32(1700000100U, s_uploaded_ts[0]);
    TEST_ASSERT_EQUAL_UINT32(1700000200U, s_uploaded_ts[1]);
    TEST_ASSERT_EQUAL_UINT32(1700000300U, s_uploaded_ts[2]);

    sync_engine_clear_sources();
}
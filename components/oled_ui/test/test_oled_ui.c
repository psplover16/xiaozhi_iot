#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#include "system_types.h"
#include "oled_ui.h"
#include "oled_ssd1306.h"

static uint32_t count_set_bits_in_range(const uint8_t *buf, size_t start, size_t len)
{
    uint32_t count = 0;
    for (size_t i = start; i < (start + len); i++) {
        uint8_t v = buf[i];
        while (v != 0U) {
            count += (v & 0x01U);
            v >>= 1;
        }
    }
    return count;
}

TEST_CASE("oled_ui renders three-tier layout and top alert banner at 10FPS", "[oled_ui][phase5]")
{
    esp_err_t ret = oled_ui_init();
    TEST_ASSERT_TRUE(ret == ESP_OK || ret == ESP_ERR_NOT_FOUND);

    sensor_record_t rec = {
        .timestamp = 1795017600U,
        .temperature = 256,   /* 25.6 C */
        .humidity = 605,      /* 60.5 % */
        .lux = 350,
        .status_flags = REC_FLAG_DHT22_VALID | REC_FLAG_GY30_VALID | REC_FLAG_RTC_VALID,
        .crc16 = 0
    };

    env_stats_t stats = {
        .temp_avg = 25.2f,
        .temp_min = 24.0f,
        .temp_max = 26.5f,
        .humi_avg = 60.0f,
        .humi_min = 55.0f,
        .humi_max = 65.0f,
        .lux_avg = 320,
        .lux_min = 100,
        .lux_max = 500,
        .temp_trend = 1,
        .valid_count = 42
    };

    oled_ui_set_net_status(NET_MODE_ONLINE, "192.168.1.50");
    oled_ui_set_alert_flags(STORAGE_ALERT_NONE);
    oled_ui_update_sensor(&rec, &stats);

    /* 等待超過 1 個 10FPS 幀週期 (100ms) */
    vTaskDelay(pdMS_TO_TICKS(150));

    const uint8_t *fb = oled_ssd1306_get_framebuffer();
    TEST_ASSERT_NOT_NULL(fb);

    uint32_t normal_top_bits = count_set_bits_in_range(fb, 0, OLED_WIDTH * 2U);
    uint32_t mid_bits = count_set_bits_in_range(fb, OLED_WIDTH * 2U, OLED_WIDTH * 4U);
    uint32_t bot_bits = count_set_bits_in_range(fb, OLED_WIDTH * 6U, OLED_WIDTH * 2U);

    TEST_ASSERT_GREATER_THAN_UINT32(0U, normal_top_bits);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, mid_bits);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, bot_bits);

    /* 觸發置頂反白警告旗標，驗證 Page 0~1 點亮像素顯著增加 */
    oled_ui_set_alert_flags(STORAGE_ALERT_SD_FULL);
    vTaskDelay(pdMS_TO_TICKS(150));

    uint32_t alert_top_bits = count_set_bits_in_range(fb, 0, OLED_WIDTH * 2U);
    TEST_ASSERT_GREATER_THAN_UINT32(normal_top_bits + 500U, alert_top_bits);

    /* 恢復正常顯示 */
    oled_ui_set_alert_flags(STORAGE_ALERT_NONE);
    vTaskDelay(pdMS_TO_TICKS(150));
}

TEST_CASE("oled_ui EMA ambient light adaptive dimming runs safely", "[oled_ui][phase5]")
{
    esp_err_t ret = oled_ui_init();
    TEST_ASSERT_TRUE(ret == ESP_OK || ret == ESP_ERR_NOT_FOUND);

    /* 模擬強光階躍輸入並等待數個 10FPS 週期讓 EMA 收斂 */
    oled_ui_adjust_contrast_by_lux(1200U);
    vTaskDelay(pdMS_TO_TICKS(450));

    /* 模擬暗室階躍輸入並等待平滑衰減 */
    oled_ui_adjust_contrast_by_lux(0U);
    vTaskDelay(pdMS_TO_TICKS(450));
}
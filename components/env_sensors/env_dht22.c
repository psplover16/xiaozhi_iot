#include "env_sensors_drv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "system_types.h"

static const char *TAG = "env_dht22";

#define DHT22_START_LOW_US          1200U     /* 主機拉低起始脈衝 (>= 1ms) */
#define DHT22_START_RELEASE_US      30U       /* 主機釋放等待響應 (20~40us) */
#define DHT22_PHASE_TIMEOUT_US      100LL     /* 單一電位階段超時上限 */
#define DHT22_CRIT_MAX_BUDGET_US    4800LL    /* 臨界區總時間硬上限 (< 5ms) */
#define DHT22_BIT_ONE_THRESHOLD_US  40LL      /* 高電位 > 40us 判定為 '1' (0:~26us, 1:~70us) */
#define DHT22_MIN_INTERVAL_US       2000000LL /* DHT22 最小連續採樣間隔 2 秒 */

static portMUX_TYPE s_dht22_mux = portMUX_INITIALIZER_UNLOCKED;
static bool         s_dht22_initialized = false;
static bool         s_dht22_has_cache   = false;
static int16_t      s_dht22_cached_temp = INVALID_TEMP_VALUE;
static uint16_t     s_dht22_cached_humi = INVALID_HUMI_VALUE;
static int64_t      s_dht22_last_ok_us  = 0;

/**
 * @brief 在臨界區內等待 DHT22 腳位達到目標電位，並同步檢查階段超時與 4.8ms 總預算
 * @return 經過的微秒數 (>= 0)；若超時則回傳 -1
 */
static inline int64_t dht22_wait_level(int expected_level, int64_t phase_timeout_us, int64_t crit_start_us)
{
    int64_t phase_start = esp_timer_get_time();
    while (gpio_get_level(ENV_DHT22_GPIO_NUM) != expected_level) {
        int64_t now = esp_timer_get_time();
        if ((now - phase_start) > phase_timeout_us) {
            return -1;
        }
        if ((now - crit_start_us) > DHT22_CRIT_MAX_BUDGET_US) {
            return -1;
        }
    }
    return esp_timer_get_time() - phase_start;
}

esp_err_t env_dht22_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ENV_DHT22_GPIO_NUM),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DHT22 GPIO %d config failed: %s", ENV_DHT22_GPIO_NUM, esp_err_to_name(ret));
        return ret;
    }

    /* 開漏模式輸出 1 即釋放匯流排至高電位閒置狀態 */
    gpio_set_level(ENV_DHT22_GPIO_NUM, 1);
    s_dht22_initialized = true;
    s_dht22_has_cache = false;
    s_dht22_cached_temp = INVALID_TEMP_VALUE;
    s_dht22_cached_humi = INVALID_HUMI_VALUE;
    s_dht22_last_ok_us = 0;

    ESP_LOGI(TAG, "DHT22 initialized on GPIO %d (Open-Drain + Pull-up)", ENV_DHT22_GPIO_NUM);
    return ESP_OK;
}

esp_err_t env_dht22_read(int16_t *out_temp_x10, uint16_t *out_humi_x10)
{
    if (out_temp_x10 == NULL || out_humi_x10 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_temp_x10 = INVALID_TEMP_VALUE;
    *out_humi_x10 = INVALID_HUMI_VALUE;

    if (!s_dht22_initialized) {
        esp_err_t init_ret = env_dht22_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    /* 遵守 DHT22 硬體 2 秒採樣週期限制，若間隔不足且有有效快取則直接回傳快取值 */
    int64_t now_us = esp_timer_get_time();
    if (s_dht22_has_cache && (now_us - s_dht22_last_ok_us) < DHT22_MIN_INTERVAL_US) {
        *out_temp_x10 = s_dht22_cached_temp;
        *out_humi_x10 = s_dht22_cached_humi;
        return ESP_OK;
    }

    /* 1. Host 起始訊號（置於臨界區外，確保臨界區總時長嚴格 < 5ms） */
    gpio_set_level(ENV_DHT22_GPIO_NUM, 0);
    esp_rom_delay_us(DHT22_START_LOW_US);

    uint8_t data[5] = {0};
    bool ok = true;
    int fail_stage = 0;
    int fail_bit = -1;

    /* 2. 進入微秒級臨界區進行響應握手與 40-bit 採樣（硬上限 4.8ms） */
    portENTER_CRITICAL(&s_dht22_mux);
    int64_t crit_start_us = esp_timer_get_time();

    /* 釋放匯流排為高電位並等待 20~40us */
    gpio_set_level(ENV_DHT22_GPIO_NUM, 1);
    esp_rom_delay_us(DHT22_START_RELEASE_US);

    /* 等待 DHT22 拉低響應 (80us) */
    if (dht22_wait_level(0, DHT22_PHASE_TIMEOUT_US, crit_start_us) < 0) {
        ok = false;
        fail_stage = 1;
    }

    /* 等待 DHT22 拉高準備傳送 (80us) */
    if (ok && dht22_wait_level(1, DHT22_PHASE_TIMEOUT_US, crit_start_us) < 0) {
        ok = false;
        fail_stage = 2;
    }

    /* 等待 DHT22 結束高電位進入第一個 bit 的 50us 低電位前導 */
    if (ok && dht22_wait_level(0, DHT22_PHASE_TIMEOUT_US, crit_start_us) < 0) {
        ok = false;
        fail_stage = 3;
    }

    /* 連續讀取 40 bits (5 Bytes, MSB First) */
    if (ok) {
        for (int i = 0; i < 40; i++) {
            /* 等待 50us 低電位結束，進入資料高電位 */
            if (dht22_wait_level(1, DHT22_PHASE_TIMEOUT_US, crit_start_us) < 0) {
                ok = false;
                fail_stage = 4;
                fail_bit = i;
                break;
            }

            /* 量測高電位持續時間以區分 '0' (~26us) 與 '1' (~70us) */
            int64_t high_dur_us = dht22_wait_level(0, DHT22_PHASE_TIMEOUT_US, crit_start_us);
            if (high_dur_us < 0) {
                ok = false;
                fail_stage = 5;
                fail_bit = i;
                break;
            }

            data[i / 8] <<= 1;
            if (high_dur_us > DHT22_BIT_ONE_THRESHOLD_US) {
                data[i / 8] |= 1U;
            }
        }
    }

    int64_t crit_elapsed_us = esp_timer_get_time() - crit_start_us;
    portEXIT_CRITICAL(&s_dht22_mux);

    /* 確保匯流排維持在高電位閒置狀態 */
    gpio_set_level(ENV_DHT22_GPIO_NUM, 1);

    if (!ok) {
        ESP_LOGW(TAG, "DHT22 read timeout at stage=%d bit=%d (crit=%lld us)",
                 fail_stage, fail_bit, (long long)crit_elapsed_us);
        return ESP_ERR_TIMEOUT;
    }

    /* 3. 驗證 8-bit Checksum */
    uint8_t checksum = (uint8_t)((data[0] + data[1] + data[2] + data[3]) & 0xFFU);
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "DHT22 checksum mismatch: calc=0x%02X recv=0x%02X [%02X %02X %02X %02X]",
                 checksum, data[4], data[0], data[1], data[2], data[3]);
        return ESP_ERR_INVALID_CRC;
    }

    /* 4. 解析濕度 (0.1% RH) 與溫度 (0.1 °C, 最高位為負號旗標) */
    uint16_t raw_humi = ((uint16_t)data[0] << 8) | (uint16_t)data[1];
    int16_t  raw_temp = (int16_t)(((uint16_t)(data[2] & 0x7FU) << 8) | (uint16_t)data[3]);
    if (data[2] & 0x80U) {
        raw_temp = (int16_t)(-raw_temp);
    }

    /* 物理合理範圍檢查 (-40.0°C ~ +80.0°C, 0.0% ~ 100.0% RH) */
    if (raw_humi > 1000U || raw_temp < -400 || raw_temp > 800) {
        ESP_LOGW(TAG, "DHT22 out-of-range: temp=%d (0.1C), humi=%u (0.1%%)", raw_temp, raw_humi);
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_dht22_cached_temp = raw_temp;
    s_dht22_cached_humi = raw_humi;
    s_dht22_has_cache   = true;
    s_dht22_last_ok_us  = esp_timer_get_time();

    *out_temp_x10 = raw_temp;
    *out_humi_x10 = raw_humi;

    return ESP_OK;
}
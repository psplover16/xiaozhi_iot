#include "oled_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "board_config.h"
#include "system_types.h"
#include "board_bsp.h"
#include "oled_ssd1306.h"

static const char *TAG = "oled_ui";

#define OLED_UI_FPS                 10U
#define OLED_UI_FRAME_PERIOD_MS     (1000U / OLED_UI_FPS) /* 100ms (10FPS) */
#define OLED_UI_TASK_STACK          3072U
#define OLED_UI_TASK_PRIO           4U
#define OLED_UI_TASK_CORE           1

#define LUX_EMA_ALPHA               0.25f
#define BACKLIGHT_PWM_MIN           25U
#define BACKLIGHT_PWM_MID           200U
#define BACKLIGHT_PWM_MAX           255U
#define OLED_CONTRAST_MIN           0x10U
#define OLED_CONTRAST_MAX           0xFFU

typedef struct {
    bool            initialized;
    bool            dirty;
    bool            has_record;
    bool            has_stats;
    bool            ema_initialized;
    sensor_record_t record;
    env_stats_t     stats;
    uint8_t         alert_flags;
    net_mode_t      net_mode;
    char            net_label[24];
    uint32_t        raw_lux;
    float           ema_lux;
    uint8_t         last_contrast;
    uint8_t         last_pwm;
} oled_ui_state_t;

static SemaphoreHandle_t s_ui_mutex       = NULL;
static TaskHandle_t      s_ui_task_handle = NULL;
static oled_ui_state_t   s_ui             = {0};

static const char *net_mode_short_tag(net_mode_t mode)
{
    switch (mode) {
        case NET_MODE_ONLINE:
            return "[ONLINE]";
        case NET_MODE_WEB_CFG:
            return "[WEBCFG]";
        case NET_MODE_OFFLINE:
        default:
            return "[OFFLNE]";
    }
}

static char trend_symbol(int8_t trend)
{
    if (trend > 0) {
        return '^';
    }
    if (trend < 0) {
        return 'v';
    }
    return '=';
}

static uint8_t compute_pwm_from_ema_lux(float ema_lux)
{
    if (ema_lux <= 10.0f) {
        return BACKLIGHT_PWM_MIN;
    }
    if (ema_lux >= 1000.0f) {
        return BACKLIGHT_PWM_MAX;
    }
    if (ema_lux <= 500.0f) {
        float ratio = (ema_lux - 10.0f) / 490.0f;
        return (uint8_t)(BACKLIGHT_PWM_MIN + (ratio * (float)(BACKLIGHT_PWM_MID - BACKLIGHT_PWM_MIN)));
    }
    float ratio = (ema_lux - 500.0f) / 500.0f;
    return (uint8_t)(BACKLIGHT_PWM_MID + (ratio * (float)(BACKLIGHT_PWM_MAX - BACKLIGHT_PWM_MID)));
}

static uint8_t compute_contrast_from_ema_lux(float ema_lux)
{
    if (ema_lux <= 10.0f) {
        return OLED_CONTRAST_MIN;
    }
    if (ema_lux >= 1000.0f) {
        return OLED_CONTRAST_MAX;
    }
    float ratio = (ema_lux - 10.0f) / 990.0f;
    return (uint8_t)(OLED_CONTRAST_MIN + (ratio * (float)(OLED_CONTRAST_MAX - OLED_CONTRAST_MIN)));
}

static void render_top_section(void)
{
    char line0[24];
    char line1[24];

    if (s_ui.alert_flags != STORAGE_ALERT_NONE) {
        /* Page 0~1 (y=0..15): 反白置頂顯示儲存警告旗標 */
        oled_fb_fill_rect(0, 0, OLED_WIDTH, 15, true);

        if (s_ui.alert_flags & STORAGE_ALERT_SD_FULL) {
            snprintf(line0, sizeof(line0), "!ALERT: SD CARD FULL!");
        } else if (s_ui.alert_flags & STORAGE_ALERT_SD_LOW) {
            snprintf(line0, sizeof(line0), "!WARN: SD SPACE LOW !");
        } else {
            snprintf(line0, sizeof(line0), "!WARN: EEPROM > 80%% !");
        }

        snprintf(line1, sizeof(line1), "FLG:0x%02X %s",
                 s_ui.alert_flags, net_mode_short_tag(s_ui.net_mode));

        oled_fb_draw_str(1, 0, line0, true);
        oled_fb_draw_str(1, 8, line1, true);
    } else {
        char time_buf[12] = "--:--:--";
        if (s_ui.has_record && s_ui.record.timestamp > 0U) {
            time_t ts = (time_t)s_ui.record.timestamp;
            struct tm tm_now = {0};
            localtime_r(&ts, &tm_now);
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        }

        snprintf(line0, sizeof(line0), "%-8s %s",
                 net_mode_short_tag(s_ui.net_mode), time_buf);

        if (s_ui.net_label[0] != '\0') {
            snprintf(line1, sizeof(line1), "NET: %.16s", s_ui.net_label);
        } else {
            snprintf(line1, sizeof(line1), "NET: STANDALONE");
        }

        oled_fb_draw_str(0, 0, line0, false);
        oled_fb_draw_str(0, 8, line1, false);
    }

    oled_fb_draw_hline(0, 15, OLED_WIDTH, true);
}

static void render_middle_section(void)
{
    char line_temp[24];
    char line_humi[24];
    char line_lux[24];

    bool dht_valid = s_ui.has_record &&
                     ((s_ui.record.status_flags & REC_FLAG_DHT22_VALID) != 0U) &&
                     (s_ui.record.temperature != INVALID_TEMP_VALUE) &&
                     (s_ui.record.humidity != INVALID_HUMI_VALUE);

    if (dht_valid) {
        int temp_val = (int)s_ui.record.temperature;
        char sign = (temp_val < 0) ? '-' : ' ';
        int abs_temp = (temp_val < 0) ? -temp_val : temp_val;
        char tr = s_ui.has_stats ? trend_symbol(s_ui.stats.temp_trend) : '=';

        snprintf(line_temp, sizeof(line_temp), "TEMP:%c%2d.%1d C  [%c]",
                 sign, abs_temp / 10, abs_temp % 10, tr);

        unsigned int humi_val = (unsigned int)s_ui.record.humidity;
        snprintf(line_humi, sizeof(line_humi), "HUMI: %3u.%1u %%",
                 humi_val / 10U, humi_val % 10U);
    } else {
        snprintf(line_temp, sizeof(line_temp), "TEMP: --.- C  [=]");
        snprintf(line_humi, sizeof(line_humi), "HUMI: --.- %%");
    }

    bool gy30_valid = s_ui.has_record &&
                      ((s_ui.record.status_flags & REC_FLAG_GY30_VALID) != 0U);
    if (gy30_valid) {
        snprintf(line_lux, sizeof(line_lux), "LUX : %5lu LX",
                 (unsigned long)s_ui.record.lux);
    } else {
        snprintf(line_lux, sizeof(line_lux), "LUX : ----- LX");
    }

    oled_fb_draw_str(0, 18, line_temp, false);
    oled_fb_draw_str(0, 28, line_humi, false);
    oled_fb_draw_str(0, 38, line_lux, false);

    oled_fb_draw_hline(0, 47, OLED_WIDTH, true);
}

static void render_bottom_section(void)
{
    char line_stat0[24];
    char line_stat1[24];

    if (s_ui.has_stats && s_ui.stats.valid_count > 0U) {
        snprintf(line_stat0, sizeof(line_stat0), "T:%4.1f-%4.1f A:%4.1f",
                 (double)s_ui.stats.temp_min,
                 (double)s_ui.stats.temp_max,
                 (double)s_ui.stats.temp_avg);

        snprintf(line_stat1, sizeof(line_stat1), "H:%2.0f-%2.0f%% N:%lu",
                 (double)s_ui.stats.humi_min,
                 (double)s_ui.stats.humi_max,
                 (unsigned long)s_ui.stats.valid_count);
    } else {
        snprintf(line_stat0, sizeof(line_stat0), "STATS: COLLECTING...");
        snprintf(line_stat1, sizeof(line_stat1), "WINDOW SAMPLES: 0");
    }

    oled_fb_draw_str(0, 49, line_stat0, false);
    oled_fb_draw_str(0, 57, line_stat1, false);
}

static void oled_ui_compose_locked(void)
{
    oled_fb_clear();
    render_top_section();
    render_middle_section();
    render_bottom_section();
    s_ui.dirty = false;
}

static void task_oled_ui(void *arg)
{
    (void)arg;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t frame_period = pdMS_TO_TICKS(OLED_UI_FRAME_PERIOD_MS);
    uint32_t frame_counter = 0U;

    ESP_LOGI(TAG, "task_oled_ui started on Core %d (%u FPS)",
             xPortGetCoreID(), OLED_UI_FPS);

    while (1) {
        bool need_flush = false;
        bool update_pwm = false;
        bool update_contrast = false;
        uint8_t next_pwm = 0;
        uint8_t next_contrast = 0;

        if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            /* 1. EMA 環境光平滑濾波與背光/對比度計算 */
            if (!s_ui.ema_initialized) {
                s_ui.ema_lux = (float)s_ui.raw_lux;
                s_ui.ema_initialized = true;
            } else {
                s_ui.ema_lux = (LUX_EMA_ALPHA * (float)s_ui.raw_lux) +
                               ((1.0f - LUX_EMA_ALPHA) * s_ui.ema_lux);
            }

            next_pwm = compute_pwm_from_ema_lux(s_ui.ema_lux);
            if (next_pwm != s_ui.last_pwm) {
                s_ui.last_pwm = next_pwm;
                update_pwm = true;
            }

            next_contrast = compute_contrast_from_ema_lux(s_ui.ema_lux);
            int diff = (int)next_contrast - (int)s_ui.last_contrast;
            if (diff < 0) {
                diff = -diff;
            }
            if (diff >= 2) {
                s_ui.last_contrast = next_contrast;
                update_contrast = true;
            }

            /* 2. Dirty 狀態或每秒心跳重繪 Framebuffer */
            frame_counter++;
            if (s_ui.dirty || (frame_counter % OLED_UI_FPS) == 0U) {
                oled_ui_compose_locked();
                need_flush = true;
            }

            xSemaphoreGive(s_ui_mutex);
        }

        /* 3. 在 Mutex 之外執行硬體 PWM 與 I2C 刷新 */
        if (update_pwm) {
            (void)board_set_backlight(next_pwm);
        }

        if (oled_ssd1306_is_online()) {
            if (update_contrast) {
                (void)oled_ssd1306_set_contrast(next_contrast);
            }
            if (need_flush) {
                (void)oled_ssd1306_flush();
            }
        }

        vTaskDelayUntil(&last_wake_time, frame_period);
    }
}

esp_err_t oled_ui_init(void)
{
    if (s_ui_mutex == NULL) {
        s_ui_mutex = xSemaphoreCreateMutex();
        if (s_ui_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create oled_ui mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t hw_ret = oled_ssd1306_init();

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(&s_ui, 0, sizeof(s_ui));
        s_ui.initialized = true;
        s_ui.dirty = true;
        s_ui.alert_flags = STORAGE_ALERT_NONE;
        s_ui.net_mode = NET_MODE_OFFLINE;
        s_ui.raw_lux = 100U;
        s_ui.ema_lux = 100.0f;
        s_ui.ema_initialized = true;
        s_ui.last_contrast = 0x8F;
        s_ui.last_pwm = compute_pwm_from_ema_lux(100.0f);

        oled_ui_compose_locked();
        xSemaphoreGive(s_ui_mutex);
    }

    (void)board_set_backlight(s_ui.last_pwm);

    if (oled_ssd1306_is_online()) {
        (void)oled_ssd1306_flush();
    } else {
        ESP_LOGW(TAG, "OLED display offline; UI framebuffer running in degraded mode");
    }

    if (s_ui_task_handle == NULL) {
        BaseType_t task_ret = xTaskCreatePinnedToCore(
            task_oled_ui,
            "task_oled_ui",
            OLED_UI_TASK_STACK,
            NULL,
            OLED_UI_TASK_PRIO,
            &s_ui_task_handle,
            OLED_UI_TASK_CORE
        );
        if (task_ret != pdPASS) {
            s_ui_task_handle = NULL;
            ESP_LOGE(TAG, "Failed to create task_oled_ui on Core %d", OLED_UI_TASK_CORE);
            return ESP_ERR_NO_MEM;
        }
    }

    return hw_ret;
}

void oled_ui_update_sensor(const sensor_record_t *rec, const env_stats_t *stats)
{
    if (s_ui_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (rec != NULL) {
            s_ui.record = *rec;
            s_ui.has_record = true;
            if ((rec->status_flags & REC_FLAG_GY30_VALID) != 0U) {
                s_ui.raw_lux = rec->lux;
            }
        }
        if (stats != NULL) {
            s_ui.stats = *stats;
            s_ui.has_stats = true;
        }
        s_ui.dirty = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

void oled_ui_set_alert_flags(uint8_t alert_flags)
{
    if (s_ui_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_ui.alert_flags != alert_flags) {
            s_ui.alert_flags = alert_flags;
            s_ui.dirty = true;
        }
        xSemaphoreGive(s_ui_mutex);
    }
}

void oled_ui_set_net_status(net_mode_t mode, const char *ip_or_ssid)
{
    if (s_ui_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_ui.net_mode = mode;
        if (ip_or_ssid != NULL) {
            snprintf(s_ui.net_label, sizeof(s_ui.net_label), "%s", ip_or_ssid);
        } else {
            s_ui.net_label[0] = '\0';
        }
        s_ui.dirty = true;
        xSemaphoreGive(s_ui_mutex);
    }
}

void oled_ui_adjust_contrast_by_lux(uint32_t lux)
{
    if (s_ui_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_ui.raw_lux = lux;
        xSemaphoreGive(s_ui_mutex);
    }
}
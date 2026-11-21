#ifndef AEC_REF_H
#define AEC_REF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 固定輸出給 ESP-AFE 的參考訊號取樣率 (16 kHz) */
#define AEC_REF_SAMPLE_RATE         16000U

/** 參考緩衝區總容量：160 ms (2560 samples = 5120 Bytes，置於內部 SRAM) */
#define AEC_REF_CAP_SAMPLES         2560U

/** 高水位保護門檻：130 ms (超過則丟棄最舊樣本，防止參考超前 AFE 濾波窗口) */
#define AEC_REF_HIGH_WATER_SAMPLES  2080U

/** 播放啟動預填靜音：30 ms (補償 I2S TX DMA 硬體路徑延遲) */
#define AEC_REF_PREFILL_SAMPLES     480U

/**
 * @brief 初始化 AEC 參考訊號環形緩衝區（配置於內部 SRAM MALLOC_CAP_INTERNAL）
 * @return esp_err_t ESP_OK 成功，ESP_ERR_NO_MEM 內部 SRAM 不足
 */
esp_err_t aec_ref_init(void);

/**
 * @brief 釋放 AEC 參考訊號緩衝區資源
 */
void aec_ref_deinit(void);

/**
 * @brief 寫入播放音訊作為回音消除參考訊號（自動降採樣至 16 kHz 單聲道）
 *
 * 必須在「套用軟體音量倍率之後、緊貼 i2s_channel_write() 前一刻」呼叫。
 *
 * @param pcm       音量調整後的 16-bit PCM 指標
 * @param frames    音訊幀數 (Frames，非 Byte 數)
 * @param src_rate  原始取樣率 (如 16000, 24000, 44100, 48000)
 * @param channels  聲道數 (1 = 單聲道, 2 = 立體聲交錯)
 */
void aec_ref_write(const int16_t *pcm, size_t frames, uint32_t src_rate, uint8_t channels);

/**
 * @brief 讀取指定樣本數的 16 kHz 單聲道參考訊號（不足時自動補 0 靜音）
 *
 * 供 Core 1 的 ESP-AFE feed() 迴圈呼叫，確保待機無播放時不阻塞。
 *
 * @param out_samples 輸出緩衝區 (長度至少為 count)
 * @param count       欲讀取的樣本數
 * @return size_t     實際讀到的非靜音播放參考樣本數 (0 代表目前處於待機靜音)
 */
size_t aec_ref_read(int16_t *out_samples, size_t count);

/**
 * @brief 立即清空參考緩衝區與重採樣相位（停止播放或遭喚醒詞 Barge-in 打斷時呼叫）
 */
void aec_ref_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* AEC_REF_H */
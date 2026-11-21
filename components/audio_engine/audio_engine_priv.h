#ifndef AUDIO_ENGINE_PRIV_H
#define AUDIO_ENGINE_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 TX 播放控制子模組（互斥鎖、完成信號量與預設增益） */
esp_err_t audio_tx_ctrl_init(void);

/** 啟動釘選於 Core 1 的 TX 播放任務 */
esp_err_t audio_tx_ctrl_start_task(void);

/** 接收 ESP-AFE 降噪、回音消除與 AGC 增益後的 16-bit 單聲道 PCM 進行 VAD 與 PSRAM 錄音 */
void audio_engine_push_afe_mono(const int16_t *mono_pcm, size_t sample_count);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENGINE_PRIV_H */
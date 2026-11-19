#include "sd_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"

static const char *TAG = "sd_media";

#define SD_MOUNT_POINT          "/sdcard"
#define SD_LOG_DIR              "/sdcard/logs"
#define SD_LOG_BATCH_MAGIC      0xA55AU
#define SD_AUDIO_CHUNK_BYTES    4096U
#define SD_MUTEX_TIMEOUT_MS     1000U

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t record_count;
    uint32_t date_tag;
    uint32_t crc32;
} sd_log_batch_hdr_t;

typedef struct {
    bool              mounted;
    sdmmc_card_t     *card;
    i2s_chan_handle_t local_tx_chan;
    volatile bool     playing;
    volatile bool     stop_requested;
} sd_media_ctx_t;

static sd_media_ctx_t s_ctx = {
    .mounted        = false,
    .card           = NULL,
    .local_tx_chan  = NULL,
    .playing        = false,
    .stop_requested = false,
};

// -----------------------------------------------------------------------------
// 內部輔助：SD SPI 互斥鎖與目錄檢查
// -----------------------------------------------------------------------------

static inline bool sd_lock(void)
{
    SemaphoreHandle_t mtx = board_get_sd_mutex();
    if (mtx == NULL) {
        return true;
    }
    return (xSemaphoreTake(mtx, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) == pdTRUE);
}

static inline void sd_unlock(void)
{
    SemaphoreHandle_t mtx = board_get_sd_mutex();
    if (mtx != NULL) {
        xSemaphoreGive(mtx);
    }
}

static esp_err_t ensure_dir_exists(const char *dir_path)
{
    struct stat st;
    if (stat(dir_path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(dir_path, 0775) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// 內部輔助：I2S TX 通道取得與動態取樣率切換（支援 16kHz / 22.05kHz / 44.1kHz / 48kHz）
// -----------------------------------------------------------------------------

static esp_err_t acquire_and_config_i2s_tx(uint32_t sample_rate, i2s_chan_handle_t *out_chan)
{
    if (sample_rate == 0U || out_chan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_handle_t tx_chan = board_get_i2s_tx_chan();
    if (tx_chan == NULL) {
        if (s_ctx.local_tx_chan == NULL) {
            i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
            chan_cfg.auto_clear = true;
            esp_err_t err = i2s_new_channel(&chan_cfg, &s_ctx.local_tx_chan, NULL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to allocate I2S TX channel: %s", esp_err_to_name(err));
                return err;
            }

            i2s_std_config_t std_cfg = {
                .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
                .gpio_cfg = {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = (gpio_num_t)BOARD_PIN_I2S_SPK_BCLK,
                    .ws   = (gpio_num_t)BOARD_PIN_I2S_SPK_LRC,
                    .dout = (gpio_num_t)BOARD_PIN_I2S_SPK_DOUT,
                    .din  = I2S_GPIO_UNUSED,
                    .invert_flags = {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv   = false,
                    },
                },
            };

            err = i2s_channel_init_std_mode(s_ctx.local_tx_chan, &std_cfg);
            if (err != ESP_OK) {
                i2s_del_channel(s_ctx.local_tx_chan);
                s_ctx.local_tx_chan = NULL;
                ESP_LOGE(TAG, "Failed to init I2S TX std mode: %s", esp_err_to_name(err));
                return err;
            }
        }
        tx_chan = s_ctx.local_tx_chan;
    }

    // 依規範依序執行 disable -> reconfig_std_clock -> enable，確保動態切換不變調
    esp_err_t dis_err = i2s_channel_disable(tx_chan);
    if (dis_err != ESP_OK && dis_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s_channel_disable returned %s", esp_err_to_name(dis_err));
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconfig I2S clock to %" PRIu32 " Hz: %s",
                 sample_rate, esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(err));
        return err;
    }

    *out_chan = tx_chan;
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// 內部輔助：WAV 標頭解析與分段讓鎖播放（<= 4096B）
// -----------------------------------------------------------------------------

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_bytes;
} wav_info_t;

static esp_err_t parse_wav_header_locked(FILE *fp, wav_info_t *out_info)
{
    uint8_t riff_hdr[12];
    if (fread(riff_hdr, 1, sizeof(riff_hdr), fp) != sizeof(riff_hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(riff_hdr, "RIFF", 4) != 0 || memcmp(&riff_hdr[8], "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool fmt_found  = false;
    bool data_found = false;
    memset(out_info, 0, sizeof(wav_info_t));

    while (!feof(fp)) {
        uint8_t chunk_hdr[8];
        if (fread(chunk_hdr, 1, sizeof(chunk_hdr), fp) != sizeof(chunk_hdr)) {
            break;
        }
        uint32_t chunk_size = (uint32_t)chunk_hdr[4] |
                              ((uint32_t)chunk_hdr[5] << 8) |
                              ((uint32_t)chunk_hdr[6] << 16) |
                              ((uint32_t)chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t fmt_buf[16];
            if (fread(fmt_buf, 1, sizeof(fmt_buf), fp) != sizeof(fmt_buf)) {
                return ESP_ERR_INVALID_SIZE;
            }
            out_info->audio_format    = (uint16_t)fmt_buf[0] | ((uint16_t)fmt_buf[1] << 8);
            out_info->num_channels    = (uint16_t)fmt_buf[2] | ((uint16_t)fmt_buf[3] << 8);
            out_info->sample_rate     = (uint32_t)fmt_buf[4] |
                                        ((uint32_t)fmt_buf[5] << 8) |
                                        ((uint32_t)fmt_buf[6] << 16) |
                                        ((uint32_t)fmt_buf[7] << 24);
            out_info->bits_per_sample = (uint16_t)fmt_buf[14] | ((uint16_t)fmt_buf[15] << 8);

            uint32_t remain = chunk_size - 16U;
            if ((chunk_size & 1U) != 0U) {
                remain++;
            }
            if (remain > 0U && fseek(fp, (long)remain, SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
            fmt_found = true;
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            out_info->data_bytes = chunk_size;
            data_found = true;
            break;
        } else {
            uint32_t skip = chunk_size + (chunk_size & 1U);
            if (fseek(fp, (long)skip, SEEK_CUR) != 0) {
                break;
            }
        }
    }

    if (!fmt_found || !data_found) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out_info->audio_format != 1U || out_info->bits_per_sample != 16U ||
        (out_info->num_channels != 1U && out_info->num_channels != 2U) ||
        out_info->sample_rate == 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static esp_err_t play_wav_file(const char *filepath)
{
    if (!sd_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *fp = fopen(filepath, "rb");
    if (fp == NULL) {
        sd_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    wav_info_t info;
    esp_err_t err = parse_wav_header_locked(fp, &info);
    sd_unlock();

    if (err != ESP_OK) {
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return err;
    }

    i2s_chan_handle_t tx_chan = NULL;
    err = acquire_and_config_i2s_tx(info.sample_rate, &tx_chan);
    if (err != ESP_OK) {
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return err;
    }

    uint8_t *io_buf = (uint8_t *)heap_caps_malloc(SD_AUDIO_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (io_buf == NULL) {
        io_buf = (uint8_t *)malloc(SD_AUDIO_CHUNK_BYTES);
    }
    if (io_buf == NULL) {
        (void)i2s_channel_disable(tx_chan);
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return ESP_ERR_NO_MEM;
    }

    uint32_t remaining = info.data_bytes;
    size_t max_read = (info.num_channels == 1U) ? (SD_AUDIO_CHUNK_BYTES / 2U) : SD_AUDIO_CHUNK_BYTES;

    while (remaining > 0U && !s_ctx.stop_requested) {
        size_t to_read = (remaining < max_read) ? (size_t)remaining : max_read;

        if (!sd_lock()) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        size_t nread = fread(io_buf, 1, to_read, fp);
        sd_unlock();

        if (nread == 0U) {
            break;
        }
        remaining -= (uint32_t)nread;

        size_t tx_bytes = nread;
        if (info.num_channels == 1U) {
            // 原地由後往前將單聲道 16-bit PCM 擴展為立體聲（L/R 相同）
            size_t mono_samples = nread / sizeof(int16_t);
            int16_t *pcm = (int16_t *)io_buf;
            for (size_t i = mono_samples; i > 0U; i--) {
                int16_t s = pcm[i - 1U];
                pcm[(i - 1U) * 2U]      = s;
                pcm[(i - 1U) * 2U + 1U] = s;
            }
            tx_bytes = mono_samples * 2U * sizeof(int16_t);
        }

        size_t bytes_written = 0U;
        err = i2s_channel_write(tx_chan, io_buf, tx_bytes, &bytes_written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            break;
        }
    }

    (void)i2s_channel_disable(tx_chan);
    free(io_buf);

    if (sd_lock()) {
        fclose(fp);
        sd_unlock();
    }

    return err;
}

// -----------------------------------------------------------------------------
// 內部輔助：dr_mp3 執行緒安全 SD 讀取回呼與分段讓鎖播放（<= 4096B）
// -----------------------------------------------------------------------------

static size_t mp3_sd_on_read(void *pUserData, void *pBufferOut, size_t bytesToRead)
{
    FILE *fp = (FILE *)pUserData;
    if (fp == NULL || pBufferOut == NULL || bytesToRead == 0U) {
        return 0U;
    }
    if (!sd_lock()) {
        return 0U;
    }
    size_t n = fread(pBufferOut, 1, bytesToRead, fp);
    sd_unlock();
    return n;
}

static drmp3_bool32 mp3_sd_on_seek(void *pUserData, int offset, drmp3_seek_origin origin)
{
    FILE *fp = (FILE *)pUserData;
    if (fp == NULL) {
        return DRMP3_FALSE;
    }
    int whence = SEEK_SET;
    if (origin == DRMP3_SEEK_CUR) {
        whence = SEEK_CUR;
    } else if (origin == DRMP3_SEEK_END) {
        whence = SEEK_END;
    }
    if (!sd_lock()) {
        return DRMP3_FALSE;
    }
    int res = fseek(fp, (long)offset, whence);
    sd_unlock();
    return (res == 0) ? DRMP3_TRUE : DRMP3_FALSE;
}

static drmp3_bool32 mp3_sd_on_tell(void *pUserData, drmp3_int64 *pCursor)
{
    FILE *fp = (FILE *)pUserData;
    if (fp == NULL || pCursor == NULL) {
        return DRMP3_FALSE;
    }
    if (!sd_lock()) {
        return DRMP3_FALSE;
    }
    long pos = ftell(fp);
    sd_unlock();
    if (pos < 0L) {
        return DRMP3_FALSE;
    }
    *pCursor = (drmp3_int64)pos;
    return DRMP3_TRUE;
}

static esp_err_t play_mp3_file(const char *filepath)
{
    if (!sd_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    FILE *fp = fopen(filepath, "rb");
    sd_unlock();

    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    drmp3 *mp3 = (drmp3 *)heap_caps_malloc(sizeof(drmp3), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mp3 == NULL) {
        mp3 = (drmp3 *)malloc(sizeof(drmp3));
    }
    if (mp3 == NULL) {
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return ESP_ERR_NO_MEM;
    }

    if (!drmp3_init(mp3, mp3_sd_on_read, mp3_sd_on_seek, mp3_sd_on_tell, NULL, fp, NULL)) {
        free(mp3);
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t sample_rate = mp3->sampleRate;
    uint32_t channels    = mp3->channels;
    if (sample_rate == 0U || (channels != 1U && channels != 2U)) {
        drmp3_uninit(mp3);
        free(mp3);
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    i2s_chan_handle_t tx_chan = NULL;
    esp_err_t err = acquire_and_config_i2s_tx(sample_rate, &tx_chan);
    if (err != ESP_OK) {
        drmp3_uninit(mp3);
        free(mp3);
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return err;
    }

    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(SD_AUDIO_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm_buf == NULL) {
        pcm_buf = (int16_t *)malloc(SD_AUDIO_CHUNK_BYTES);
    }
    if (pcm_buf == NULL) {
        (void)i2s_channel_disable(tx_chan);
        drmp3_uninit(mp3);
        free(mp3);
        if (sd_lock()) {
            fclose(fp);
            sd_unlock();
        }
        return ESP_ERR_NO_MEM;
    }

    // 每次最多讀取 1024 幀立體聲（1024 * 2 * 2B = 4096B），確保單次持有 SD 鎖資料量 <= 4096B
    const drmp3_uint64 frames_per_chunk = SD_AUDIO_CHUNK_BYTES / (2U * sizeof(int16_t));

    while (!s_ctx.stop_requested) {
        drmp3_uint64 frames_read = drmp3_read_pcm_frames_s16(mp3, frames_per_chunk, pcm_buf);
        if (frames_read == 0ULL) {
            break;
        }

        if (channels == 1U) {
            for (drmp3_uint64 i = frames_read; i > 0ULL; i--) {
                int16_t s = pcm_buf[i - 1ULL];
                pcm_buf[(i - 1ULL) * 2ULL]       = s;
                pcm_buf[(i - 1ULL) * 2ULL + 1ULL] = s;
            }
        }

        size_t tx_bytes = (size_t)frames_read * 2U * sizeof(int16_t);
        size_t bytes_written = 0U;
        err = i2s_channel_write(tx_chan, pcm_buf, tx_bytes, &bytes_written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            break;
        }
    }

    (void)i2s_channel_disable(tx_chan);
    free(pcm_buf);
    drmp3_uninit(mp3);
    free(mp3);

    if (sd_lock()) {
        fclose(fp);
        sd_unlock();
    }

    return err;
}

// -----------------------------------------------------------------------------
// 公開 API 實作
// -----------------------------------------------------------------------------

esp_err_t sd_media_init(void)
{
    if (s_ctx.mounted) {
        return ESP_OK;
    }

    if (!sd_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)BOARD_PIN_SD_CS;
    slot_config.host_id = SPI2_HOST;

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_ctx.card);
    if (err != ESP_OK) {
        s_ctx.mounted = false;
        s_ctx.card    = NULL;
        sd_unlock();
        // 未插卡或掛載失敗僅記錄警告，嚴禁觸發重啟（允許降級運作）
        ESP_LOGW(TAG, "MicroSD mount failed (%s), running in degraded storage mode",
                 esp_err_to_name(err));
        return err;
    }

    s_ctx.mounted = true;
    (void)ensure_dir_exists(SD_LOG_DIR);
    sd_unlock();

    ESP_LOGI(TAG, "MicroSD mounted at %s (free: %" PRIu32 " MB)",
             SD_MOUNT_POINT, sd_media_get_free_mb());
    return ESP_OK;
}

bool sd_media_is_mounted(void)
{
    return s_ctx.mounted;
}

uint32_t sd_media_get_free_mb(void)
{
    if (!s_ctx.mounted) {
        return 0U;
    }
    if (!sd_lock()) {
        return 0U;
    }

    uint64_t total_bytes = 0ULL;
    uint64_t free_bytes  = 0ULL;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes);

    sd_unlock();

    if (err != ESP_OK) {
        return 0U;
    }
    return (uint32_t)(free_bytes / (1024ULL * 1024ULL));
}

audio_format_t sd_media_detect_format(const char *filepath)
{
    if (filepath == NULL) {
        return AUDIO_FMT_UNKNOWN;
    }
    const char *ext = strrchr(filepath, '.');
    if (ext == NULL) {
        return AUDIO_FMT_UNKNOWN;
    }
    if (strcasecmp(ext, ".wav") == 0) {
        return AUDIO_FMT_WAV;
    }
    if (strcasecmp(ext, ".mp3") == 0) {
        return AUDIO_FMT_MP3;
    }
    return AUDIO_FMT_UNKNOWN;
}

esp_err_t sd_media_play_audio(const char *filepath)
{
    if (filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.playing) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_format_t fmt = sd_media_detect_format(filepath);
    if (fmt == AUDIO_FMT_UNKNOWN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_ctx.stop_requested = false;
    s_ctx.playing        = true;

    esp_err_t err = (fmt == AUDIO_FMT_WAV) ? play_wav_file(filepath) : play_mp3_file(filepath);

    s_ctx.playing = false;
    return err;
}

void sd_media_stop_audio(void)
{
    s_ctx.stop_requested = true;
}

bool sd_media_is_playing(void)
{
    return s_ctx.playing;
}

esp_err_t sd_media_append_offline_records(uint32_t date_yyyymmdd, const sensor_record_t *records, size_t count)
{
    if (records == NULL || count == 0U || count > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!sd_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ensure_dir_exists(SD_LOG_DIR);
    if (err != ESP_OK) {
        sd_unlock();
        return err;
    }

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "%s/%08" PRIu32 ".bin", SD_LOG_DIR, date_yyyymmdd);

    size_t payload_bytes = count * sizeof(sensor_record_t);
    sd_log_batch_hdr_t hdr = {
        .magic        = SD_LOG_BATCH_MAGIC,
        .record_count = (uint16_t)count,
        .date_tag     = date_yyyymmdd,
        .crc32        = board_calc_crc32((const uint8_t *)records, payload_bytes),
    };

    // 1. 以 "ab" 附加模式寫入，並強制 fflush + fsync 確保實體落盤
    FILE *fp = fopen(filepath, "ab");
    if (fp == NULL) {
        sd_unlock();
        return ESP_FAIL;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        sd_unlock();
        return ESP_FAIL;
    }
    long write_offset = ftell(fp);
    if (write_offset < 0L) {
        fclose(fp);
        sd_unlock();
        return ESP_FAIL;
    }

    size_t w_hdr = fwrite(&hdr, 1, sizeof(hdr), fp);
    size_t w_pay = fwrite(records, 1, payload_bytes, fp);
    fflush(fp);
    int sync_res = fsync(fileno(fp));
    fclose(fp);

    if (w_hdr != sizeof(hdr) || w_pay != payload_bytes || sync_res != 0) {
        sd_unlock();
        ESP_LOGE(TAG, "Failed to flush/fsync offline records to %s", filepath);
        return ESP_FAIL;
    }

    // 2. CRC32 回讀驗證：重新開啟並讀取剛寫入的區塊比對校驗碼
    FILE *vfp = fopen(filepath, "rb");
    if (vfp == NULL) {
        sd_unlock();
        return ESP_FAIL;
    }

    if (fseek(vfp, write_offset, SEEK_SET) != 0) {
        fclose(vfp);
        sd_unlock();
        return ESP_FAIL;
    }

    sd_log_batch_hdr_t rb_hdr;
    if (fread(&rb_hdr, 1, sizeof(rb_hdr), vfp) != sizeof(rb_hdr)) {
        fclose(vfp);
        sd_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *rb_payload = (uint8_t *)heap_caps_malloc(payload_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rb_payload == NULL) {
        rb_payload = (uint8_t *)malloc(payload_bytes);
    }
    if (rb_payload == NULL) {
        fclose(vfp);
        sd_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t r_pay = fread(rb_payload, 1, payload_bytes, vfp);
    fclose(vfp);
    sd_unlock();

    if (r_pay != payload_bytes) {
        free(rb_payload);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t actual_crc = board_calc_crc32(rb_payload, payload_bytes);
    free(rb_payload);

    if (rb_hdr.magic != SD_LOG_BATCH_MAGIC ||
        rb_hdr.crc32 != hdr.crc32 ||
        actual_crc != hdr.crc32) {
        ESP_LOGE(TAG, "CRC32 read-back verification failed on %s (expected 0x%08" PRIX32 ", got 0x%08" PRIX32 ")",
                 filepath, hdr.crc32, actual_crc);
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}
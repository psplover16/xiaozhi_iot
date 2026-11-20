# Technical Implementation Plan: 小智 IoT 語音機器人 (`xiaozhi_iot`)

* **文件版本**：v1.2.0（對應 `spec.md` v1.9.1 / ESP-IDF `v6.1.1` / C11 — 原架構完整保留 ＋ Phase 8 離線聲控增量擴充版）
* **設計目標**：為 VS Code AI 輔助開發提供**零歧義的檔案樹、逐一寫死的 `CMakeLists.txt` 依賴、鎖死的 C 語言標頭檔介面（`.h`）與硬體時序規格**，徹底杜絕弱模型跨檔案實作時的 API 幻覺、型別不符與編譯報錯。
* **開發紀律**：實作任何程式碼前，必須嚴格遵守本文件之目錄結構與函式簽章；每個模組皆須先建立 `test/` 下的 Unity 單元測試（Red），再撰寫 `.c` 實作（Green）。

---

## 1. 專案目錄結構與無環依賴藍圖（Project File Tree & DAG Dependency）

### 1.1 專案目錄與檔案拆分（單檔限制 `<= 300` 行）
為防止單一檔案過長導致弱模型輸出省略（`// ... 略`），所有複雜模組均拆分為職責單一的小型 `.c` 檔（完整保留原架構樹，僅於 `audio_engine/` 下新增 Phase 8 離線語音檔案）：

```text
xiaozhi_iot/
├── CMakeLists.txt                          # 根目錄建置檔（設定專案名稱與 TEST_COMPONENTS）
├── partitions.csv                          # 自訂 16MB Flash 分區表（配置 4MB factory App 分區 + Phase 8 3MB model 分區）
├── sdkconfig.defaults                      # 鎖定 16MB Flash、Octal PSRAM、240MHz、Custom Partition
├── spec.md                                 # 規格權威文件（v1.9.1）
├── plan.md                                 # 本技術實作計畫書（v1.2.0）
├── main/
│   ├── CMakeLists.txt
│   └── main.c                              # 系統總啟動（單次初始化 NVS/Netif/EventLoop、測試/正常開機分流）
└── components/
    ├── board_config/                       # [第 0 層：硬體抽象基礎層]
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   ├── system_types.h              # 全系統共用結構（sensor_record_t, sys_config_t, 警告旗標）
    │   │   └── board_config.h              # 安全 GPIO 巨集、I2C 代理讀寫、SPI/I2S Handle、CRC16/32
    │   ├── board_config.c                  # I2C Master 4裝置註冊與加鎖代理、SPI2/I2S 初始化、開機自檢
    │   ├── board_crc.c                     # 標準 CRC16-MODBUS 與 CRC32 演算法
    │   └── test/
    │       ├── CMakeLists.txt
    │       └── test_board_config.c         # CRC16/CRC32 標準測試向量 Unity 測試
    ├── psram_cache/                        # [第 1 層：獨立資料層] 8MB Octal PSRAM 環形統計快取
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   └── psram_cache.h
    │   ├── psram_cache.c                   # MALLOC_CAP_SPIRAM 環形陣列、極值/均值/趨勢計算與摘要格式化
    │   └── test/
    │       ├── CMakeLists.txt
    │       └── test_psram_cache.c          # 環形覆寫邊界、無效值過濾、趨勢判定 Unity 測試
    ├── sd_media/                           # [第 1 層：獨立資料與媒體層] SD 卡 FAT32、WAV/MP3、離線轉存
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   ├── sd_media.h
    │   │   └── dr_mp3.h                    # 外部純 C 單標頭檔 MP3 解碼庫（手動放入，禁止 AI 修改）
    │   ├── sd_media.c                      # 定義 DR_MP3_IMPLEMENTATION、FAT32 掛載、WAV/MP3 統一 PCM 播音
    │   ├── sd_offline_store.c              # /sensor_offline/YYYYMMDD.bin "ab" 附加、fsync+CRC32 驗證、空間監控
    │   └── test/
    │       ├── CMakeLists.txt
    │       └── test_sd_media.c             # 副檔名路由、44B WAV 檔頭解析、單聲道轉雙聲道擴展測試
    ├── eeprom_blackbox/                    # [第 1 層：獨立容災層] AT24C256 雙分區管理與 64B 跨頁安全寫入
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   └── eeprom_blackbox.h
    │   ├── eeprom_blackbox.c               # I2C 2B位址轉接、64B 分頁切分 + 6ms 延遲、80% 預警、100% 轉存
    │   └── test/
    │       ├── CMakeLists.txt
    │       └── test_eeprom_blackbox.c      # 32KB 記憶體 Mock、跨頁寫入、80% 警告、轉存失敗保留資料測試
    ├── sensor_hub/                         # [第 1 層：獨立採集層] 拆分獨立驅動檔避免單檔肥大
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   └── sensor_hub.h
    │   ├── dht22_driver.c                  # esp_rom_delay_us + 20ms vTaskDelay + 4.5ms 臨界區讀取
    │   ├── gy30_driver.c                   # BH1750 (0x23) 連續高解析度光照度讀取
    │   ├── ds3231_driver.c                 # DS3231 (0x68) BCD 時間讀寫與系統時間同步
    │   ├── sensor_hub.c                    # Core 1 定時採集任務協調器與 esp_sntp.h 校時回呼
    │   └── test/
    │       ├── CMakeLists.txt
    │       └── test_sensor_hub.c           # DHT22 校驗和解析、DS3231 BCD 轉換純邏輯測試
    ├── oled_ui/                            # [第 1 層：獨立顯示層] 0.96" SSD1306 純 C 幀緩衝與自動調光
    │   ├── CMakeLists.txt
    │   ├── include/
    │   │   └── oled_ui.h
    │   ├── oled_font8x8.h                  # 內建 8x8 ASCII 點陣字型表（const 存於 Flash）
    │   ├── oled_ui.c                       # 1024B 顯存渲染、Page 0~1 置頂警告橫幅、GY-30 對比度映射
    │   └── test/
    │       ├── CMakeLists.txt
    │       └── test_oled_ui.c              # 顯存座標繪製、警告橫幅優先權覆蓋測試
    ├── audio_engine/                       # [第 2 層：語音互動層] 雙 INMP441 收音與離線命令分發
    │   ├── CMakeLists.txt
    │   ├── idf_component.yml               # [Phase 8 新增] 宣告 espressif/esp-sr 元件依賴
    │   ├── include/
    │   │   ├── audio_engine.h
    │   │   └── sr_engine.h                 # [Phase 8 新增] WakeNet/MultiNet 狀態機與離線播報介面
    │   ├── audio_engine.c                  # I2S0 Rx 雙聲道收音至 PSRAM、命令回呼分發、呼叫 sd_media 播報
    │   ├── sr_engine.c                     # [Phase 8 新增] ESP-AFE 雙麥降噪餵入、WakeNet/MultiNet 識別
    │   ├── sr_cmd_dispatcher.c             # [Phase 8 新增] 離線指令動作派發與 SD 卡音節拼接播報
    │   └── test/
    │       ├── CMakeLists.txt
    │       ├── test_audio_engine.c         # 離線指令路由與模擬觸發測試
    │       └── test_sr_engine.c            # [Phase 8 新增] 拼音指令註冊、6 秒逾時與音節序列測試
    └── net_manager/                        # [第 2 層：網路與同步層] 狀態機、補傳引擎、本地設定網頁
        ├── CMakeLists.txt
        ├── include/
        │   └── net_manager.h
        ├── net_manager.c                   # Mode 0/1/2 互斥狀態機與 GPIO 1 按鍵防彈跳切換
        ├── sync_engine.c                   # 嚴格時間順序補傳（SD YYYYMMDD.bin -> EEPROM -> Live）與 ACK 刪除
        ├── web_config.c                    # Mode 2 HTTP Server (stack=8192)、SoftAP 降級、參數邊界檢查
        └── test/
            ├── CMakeLists.txt
            └── test_net_manager.c          # 補傳排序與無 ACK 斷線保護測試、URL 解碼與參數越界攔截測試
```

### 1.2 有向無環依賴圖（Strict DAG — 杜絕標頭檔循環引用）
```text
[第 0 層]  board_config (含 system_types.h)
              │
              ├──► [第 1 層] psram_cache
              ├──► [第 1 層] sensor_hub
              ├──► [第 1 層] oled_ui
              ├──► [第 1 層] sd_media ──────────┬──► [第 2 層] audio_engine (含 Phase 8 sr_engine)
              └──► [第 1 層] eeprom_blackbox ───┴──► [第 2 層] net_manager
                                                                  │
[第 3 層]  main/main.c ◄── (總組裝所有元件並註冊跨模組 Callback) ──┘
```
* **鐵律**：依賴箭頭僅允許由下層指向高層，同層之間嚴禁互相 `#include`。Phase 8 新增的 `sr_engine` 與 `sr_cmd_dispatcher` 歸屬於第 2 層 `audio_engine` 元件內部，跨模組控制（如調整 OLED 亮度、切換 `net_manager` 模式、查詢感測器數值）一律透過 `voice_cmd_callback_t` 交由 `main/main.c` 註冊的協調器分派，嚴禁破壞單向依賴樹。

---

## 2. 建置系統與逐元件 `CMakeLists.txt` 契約（ESP-IDF v6.1.1）

### 2.1 `partitions.csv`（配置 4MB 主程式分區 + Phase 8 3MB `model` 分區）
```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x400000,
model,    data, spiffs,  0x410000, 0x300000,
coredump, data, coredump,0x710000, 0x10000,
```
*(註：既有的 `nvs`、`phy_init`、`factory` 分區偏移與大小完全不變；在 Phase 8 `T-08-01` 時補入 `model`（`3MB`）與 `coredump` 分區，專門存放 WakeNet 與 MultiNet 中文模型權重。)*

### 2.2 根目錄 `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.16)

set(TEST_COMPONENTS
    "board_config"
    "psram_cache"
    "sd_media"
    "eeprom_blackbox"
    "sensor_hub"
    "oled_ui"
    "audio_engine"
    "net_manager"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi_iot)
```

### 2.3 逐元件 `CMakeLists.txt` 精確依賴對照表（直接複製使用）
AI 建立或更新各元件時，必須嚴格使用下表指定的 `SRCS` 與 `REQUIRES`，禁止自行刪減或亂猜元件名稱：

| 檔案路徑 | `SRCS` / `SRC_DIRS` | `REQUIRES` 精確清單（ESP-IDF v6.1.1） |
| :--- | :--- | :--- |
| `main/CMakeLists.txt` | `SRCS "main.c"` | `unity board_config psram_cache sd_media eeprom_blackbox sensor_hub oled_ui audio_engine net_manager nvs_flash esp_netif esp_event` |
| `components/board_config/CMakeLists.txt` | `SRCS "board_config.c" "board_crc.c"` | `esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_gpio esp_psram` |
| `components/psram_cache/CMakeLists.txt` | `SRCS "psram_cache.c"` | `board_config esp_psram` |
| `components/sd_media/CMakeLists.txt` | `SRCS "sd_media.c" "sd_offline_store.c"` | `board_config esp_driver_i2s esp_psram fatfs sdmmc` |
| `components/eeprom_blackbox/CMakeLists.txt` | `SRCS "eeprom_blackbox.c"` | `board_config` |
| `components/sensor_hub/CMakeLists.txt` | `SRCS "dht22_driver.c" "gy30_driver.c" "ds3231_driver.c" "sensor_hub.c"` | `board_config esp_driver_gpio esp_rom esp_timer esp_netif` |
| `components/oled_ui/CMakeLists.txt` | `SRCS "oled_ui.c"` | `board_config` |
| `components/audio_engine/CMakeLists.txt`（Phase 6 基線） | `SRCS "audio_engine.c"` | `board_config sd_media esp_driver_i2s esp_psram` |
| `components/audio_engine/CMakeLists.txt`（Phase 8 擴充） | `SRCS "audio_engine.c" "sr_engine.c" "sr_cmd_dispatcher.c"` | `board_config sd_media esp_driver_i2s esp_psram espressif__esp-sr` |
| `components/net_manager/CMakeLists.txt` | `SRCS "net_manager.c" "sync_engine.c" "web_config.c"` | `board_config eeprom_blackbox sd_media esp_driver_gpio esp_wifi esp_netif esp_event esp_http_server esp_psram` |
| 各元件 `test/CMakeLists.txt` | `SRC_DIRS "."` | `unity <該元件名稱> board_config` |

> **[工作區既有進度防衝突提醒]**：若工作區已完成之 Phase 0 ～ Phase 5 實際目錄命名為 `board_bsp`（而非 `board_config`）或已拆分 `env_sensors`，撰寫 Phase 6 ～ Phase 8 的 `CMakeLists.txt` 時，請直接引用工作區實際已存在的第 0／1 層元件名稱，**嚴禁修改已通過測試的 Phase 0 ～ Phase 5 原始碼**。

---

## 3. 核心模組標頭檔介面契約（Locked C Header API Contracts）

AI 撰寫各元件實作時，**必須嚴格遵照以下函式名稱、參數型別與回傳值**，嚴禁私自更改介面簽章。

### 3.1 `board_config`（基礎型別與硬體代理層）
* **`components/board_config/include/system_types.h`**：
  ```c
  #pragma once
  #include <stdint.h>
  #include <stdbool.h>

  #define SYS_CONFIG_MAGIC        0x585A494FUL
  #define EEPROM_TOTAL_BYTES      32768U
  #define EEPROM_PAGE_SIZE        64U
  #define EEPROM_CONFIG_ADDR      0x0000U
  #define EEPROM_DATA_START_ADDR  0x0400U
  #define EEPROM_MAX_RECORDS      1984U

  typedef enum {
      NET_MODE_OFFLINE = 0,
      NET_MODE_ONLINE  = 1,
      NET_MODE_WEB_CFG = 2
  } net_mode_t;

  // 儲存警告旗標位元遮罩
  #define STORAGE_ALERT_NONE       0x00U
  #define STORAGE_ALERT_EEPROM_80  0x01U
  #define STORAGE_ALERT_SD_LOW     0x02U
  #define STORAGE_ALERT_SD_FULL    0x04U

  // sensor_record_t status_flags 位元定義
  #define REC_FLAG_DHT22_VALID     (1U << 0)
  #define REC_FLAG_GY30_VALID      (1U << 1)
  #define REC_FLAG_RTC_VALID       (1U << 2)
  #define REC_FLAG_MODE_SHIFT      4U
  #define REC_FLAG_MODE_MASK       (0x03U << 4)

  #define INVALID_TEMP_VALUE       ((int16_t)0x7FFF)
  #define INVALID_HUMI_VALUE       ((uint16_t)0xFFFF)

  typedef struct __attribute__((packed)) {
      uint32_t timestamp;
      int16_t  temperature;
      uint16_t humidity;
      uint32_t lux;
      uint16_t status_flags;
      uint16_t crc16;
  } sensor_record_t; // 固定 16 Bytes

  typedef struct __attribute__((packed)) {
      uint32_t magic;
      char     wifi_ssid[32];
      char     wifi_pass[64];
      char     server_url[128];
      uint16_t sample_interval_sec;
      uint8_t  psram_window_hours;
      uint8_t  eeprom_warn_pct;
      uint16_t sd_warn_free_mb;
      uint16_t record_count;
      uint32_t crc32;
  } sys_config_t; // 固定 240 Bytes

  typedef struct {
      float temp_avg, temp_min, temp_max;
      float humi_avg, humi_min, humi_max;
      uint32_t lux_avg, lux_min, lux_max;
      int8_t temp_trend; // -1:下降, 0:持平, 1:上升
      uint32_t valid_count;
  } env_stats_t;
  ```

* **`components/board_config/include/board_config.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "driver/i2s_std.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
  #include "system_types.h"

  // 安全腳位定義（完全避開 0,3,19,20,26-37,43-46）
  #define BOARD_PIN_MODE_BTN      1
  #define BOARD_PIN_DHT22         4
  #define BOARD_PIN_I2C_SDA       8
  #define BOARD_PIN_I2C_SCL       9
  #define BOARD_PIN_SD_CS         10
  #define BOARD_PIN_SD_MOSI       11
  #define BOARD_PIN_SD_CLK        12
  #define BOARD_PIN_SD_MISO       13
  #define BOARD_PIN_I2S_MIC_WS    15
  #define BOARD_PIN_I2S_MIC_BCLK  16
  #define BOARD_PIN_I2S_MIC_DIN   17
  #define BOARD_PIN_I2S_SPK_LRC   18
  #define BOARD_PIN_I2S_SPK_BCLK  21
  #define BOARD_PIN_I2S_SPK_DOUT  38

  // 7-bit I2C 裝置位址
  #define I2C_ADDR_OLED           0x3C
  #define I2C_ADDR_GY30           0x23
  #define I2C_ADDR_DS3231         0x68
  #define I2C_ADDR_EEPROM         0x50

  esp_err_t board_init(void);
  SemaphoreHandle_t board_get_i2c_mutex(void);
  SemaphoreHandle_t board_get_sd_mutex(void);

  // 執行緒安全 I2C 代理函式（內部自動加解 xI2cBusMutex 並匹配對應 dev_handle）
  esp_err_t board_i2c_write(uint8_t dev_addr, const uint8_t *data, size_t len);
  esp_err_t board_i2c_read(uint8_t dev_addr, uint8_t *data, size_t len);
  esp_err_t board_i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, size_t tx_len, uint8_t *rx_data, size_t rx_len);
  bool      board_i2c_is_device_online(uint8_t dev_addr);

  // I2S 通道 Handle 取得（由 board_init 建立，供 audio_engine 與 sd_media 共用）
  i2s_chan_handle_t board_get_i2s_rx_chan(void);
  i2s_chan_handle_t board_get_i2s_tx_chan(void);

  // 校驗演算法
  uint16_t board_calc_crc16(const uint8_t *data, size_t len);
  uint32_t board_calc_crc32(const uint8_t *data, size_t len);
  ```

### 3.2 `psram_cache`（PSRAM 統計快取）
* **`components/psram_cache/include/psram_cache.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "system_types.h"

  esp_err_t psram_cache_init(uint32_t max_capacity_records); // 配置於 MALLOC_CAP_SPIRAM
  esp_err_t psram_cache_push(const sensor_record_t *rec);
  esp_err_t psram_cache_get_stats(uint32_t window_seconds, uint32_t now_ts, env_stats_t *out_stats);
  size_t    psram_cache_format_summary(const env_stats_t *stats, char *out_buf, size_t buf_len);
  ```

### 3.3 `sd_media`（SD 卡儲存與 WAV/MP3 播放）
* **`components/sd_media/include/sd_media.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "system_types.h"

  typedef enum {
      AUDIO_FMT_UNKNOWN = 0,
      AUDIO_FMT_WAV     = 1,
      AUDIO_FMT_MP3     = 2
  } audio_format_t;

  esp_err_t sd_media_init(void); // 掛載 /sdcard（不重複初始化 SPI2_HOST）
  bool      sd_media_is_mounted(void);
  uint32_t  sd_media_get_free_mb(void);

  // 音訊播放（支援 .wav 與 .mp3，內部動態切換 I2S 時脈並分段讓鎖 <= 4096B）
  audio_format_t sd_media_detect_format(const char *filepath);
  esp_err_t sd_media_play_audio(const char *filepath);
  void      sd_media_stop_audio(void);
  bool      sd_media_is_playing(void);

  // 離線感測資料附加寫入（"ab" 模式 + fsync + CRC32 回讀驗證）
  esp_err_t sd_media_append_offline_records(uint32_t date_yyyymmdd, const sensor_record_t *records, size_t count);
  ```

### 3.4 `eeprom_blackbox`（AT24C256 雙分區黑盒子）
* **`components/eeprom_blackbox/include/eeprom_blackbox.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "system_types.h"

  // 底層 16-bit 記憶體定址讀寫介面（傳入 NULL 時自動使用內建 I2C 2-Byte 位址轉接器；TDD 測試時傳入 RAM Mock）
  typedef esp_err_t (*eeprom_io_write_fn)(uint16_t mem_addr, const uint8_t *data, size_t len);
  typedef esp_err_t (*eeprom_io_read_fn)(uint16_t mem_addr, uint8_t *data, size_t len);

  // 100% 滿載溢位轉存 Callback（回傳 ESP_OK 代表已驗證落盤成功，才允許淨空分區 B）
  typedef esp_err_t (*eeprom_overflow_flush_cb)(const sensor_record_t *records, size_t count);

  esp_err_t eeprom_bb_init(eeprom_io_write_fn custom_wr, eeprom_io_read_fn custom_rd);
  void      eeprom_bb_register_flush_cb(eeprom_overflow_flush_cb cb);

  // 分區 A：系統設定讀寫（首次開機無效自動載入預設值；絕不影響分區 B）
  esp_err_t eeprom_bb_load_config(sys_config_t *out_cfg);
  esp_err_t eeprom_bb_save_config(const sys_config_t *cfg);

  // 分區 B：感測黑盒子操作
  esp_err_t eeprom_bb_push_record(const sensor_record_t *rec, uint8_t *out_alert_flags);
  uint16_t  eeprom_bb_get_record_count(void);
  esp_err_t eeprom_bb_read_records(uint16_t start_idx, sensor_record_t *out_buf, uint16_t count);
  esp_err_t eeprom_bb_clear_partition_b(void); // 僅抹除 0x0400~0x7FFF 並歸零 record_count
  ```

### 3.5 `sensor_hub`、`oled_ui`、`audio_engine`、`net_manager`
* **`components/sensor_hub/include/sensor_hub.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "system_types.h"

  typedef void (*sensor_new_record_cb)(const sensor_record_t *rec);

  esp_err_t sensor_hub_init(sensor_new_record_cb on_record_cb);
  esp_err_t sensor_hub_start_task(void); // Pin to Core 1
  esp_err_t sensor_hub_sync_ntp(void);   // Mode 1 聯網後透過 esp_sntp.h 校準 DS3231
  uint32_t  sensor_hub_get_date_yyyymmdd(uint32_t unix_ts);
  ```
* **`components/oled_ui/include/oled_ui.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "system_types.h"

  esp_err_t oled_ui_init(void);
  void      oled_ui_update_sensor(const sensor_record_t *rec, const env_stats_t *stats);
  void      oled_ui_set_alert_flags(uint8_t alert_flags); // 置頂顯示於 Page 0~1
  void      oled_ui_set_net_status(net_mode_t mode, const char *ip_or_ssid);
  void      oled_ui_adjust_contrast_by_lux(uint32_t lux);
  ```
* **`components/audio_engine/include/audio_engine.h`（保留既有介面 ＋ Phase 8 PCM 幀分發與離線命令擴充）**：
  ```c
  #pragma once
  #include <stdint.h>
  #include <stddef.h>
  #include <stdbool.h>
  #include "esp_err.h"
  #include "system_types.h"

  typedef enum {
      VOICE_CMD_NONE = 0,
      VOICE_CMD_WIFI_ON,
      VOICE_CMD_WIFI_OFF,
      VOICE_CMD_REPORT_ENV,
      VOICE_CMD_PLAY_MUSIC,
      VOICE_CMD_STOP_MUSIC,
      // [Phase 8 擴充離線命令詞 ID]
      VOICE_CMD_VOL_UP,
      VOICE_CMD_VOL_DOWN,
      VOICE_CMD_MUTE_TOGGLE,
      VOICE_CMD_OLED_ON,
      VOICE_CMD_OLED_OFF,
      VOICE_CMD_OLED_BRIGHT_UP,
      VOICE_CMD_OLED_AUTO_BRIGHT,
      VOICE_CMD_NET_WEB_CFG,
      VOICE_CMD_REPORT_TEMP,
      VOICE_CMD_REPORT_HUMI,
      VOICE_CMD_REPORT_LUX,
      VOICE_CMD_REPORT_TIME,
      VOICE_CMD_REPORT_STORAGE
  } voice_cmd_id_t;

  typedef void (*voice_cmd_callback_t)(voice_cmd_id_t cmd);

  // [Phase 8 前向相容介面] 非阻塞 16kHz / 16-bit 雙聲道交錯 PCM 幀分發回呼（供 ESP-AFE 訂閱）
  typedef void (*audio_pcm_frame_cb_t)(const int16_t *stereo_pcm, size_t frame_samples, void *user_ctx);

  esp_err_t audio_engine_init(voice_cmd_callback_t cb);
  esp_err_t audio_engine_start_task(void); // Pin to Core 1 (最高優先權)
  void      audio_engine_inject_cmd(voice_cmd_id_t cmd); // 供測試或按鍵/UART 模擬觸發
  esp_err_t audio_engine_speak_env_stats(const env_stats_t *stats); // 呼叫 sd_media 拼接播放數字 WAV

  // [Phase 8 前向相容介面] 註冊/取消 16kHz 雙聲道 PCM 幀訂閱回呼（sr_engine 掛載點）
  esp_err_t audio_engine_register_pcm_tap(audio_pcm_frame_cb_t tap_cb, void *user_ctx);
  ```
* **`components/net_manager/include/net_manager.h`**：
  ```c
  #pragma once
  #include "esp_err.h"
  #include "system_types.h"

  // Server 批次上傳介面（供 sync_engine 依賴反轉與 TDD Mock 測試）
  typedef esp_err_t (*sync_upload_batch_fn)(const sensor_record_t *batch, size_t count);

  esp_err_t  net_manager_init(void);
  esp_err_t  net_manager_set_mode(net_mode_t mode);
  net_mode_t net_manager_get_mode(void);

  // Web 設定驗證純邏輯（供 FR-09 與 Unity 單元測試直接呼叫）
  esp_err_t  web_config_validate(const sys_config_t *cfg, char *err_msg, size_t err_len);

  // 嚴格時間順序補傳引擎
  esp_err_t  sync_engine_execute(sync_upload_batch_fn upload_fn);
  ```

### 3.6 `sr_engine`（Phase 8 新增：`ESP-SR` 離線語音喚醒與命令詞識別）
* **`components/audio_engine/include/sr_engine.h`**：
  ```c
  #pragma once
  #include <stdint.h>
  #include <stddef.h>
  #include <stdbool.h>
  #include "esp_err.h"
  #include "audio_engine.h"

  typedef enum {
      SR_STATE_IDLE        = 0,
      SR_STATE_WAKE_LISTEN = 1, // WakeNet 低功耗喚醒詞監聽中
      SR_STATE_CMD_LISTEN  = 2  // MultiNet 離線命令詞識別中（預設 6000ms 逾時）
  } sr_listen_state_t;

  typedef void (*sr_state_change_cb_t)(sr_listen_state_t new_state);

  // 初始化 ESP-AFE、載入 3MB model 分區之 WakeNet 與 MultiNet 並註冊預設漢語拼音指令表
  esp_err_t         sr_engine_init(voice_cmd_callback_t cmd_cb, sr_state_change_cb_t state_cb);
  esp_err_t         sr_engine_start_tasks(void); // 啟動 Core 1 之 task_sr_feed 與 task_sr_detect
  sr_listen_state_t sr_engine_get_state(void);

  // 動態新增漢語拼音離線命令詞（綁定至 voice_cmd_id_t）
  esp_err_t         sr_engine_add_pinyin_cmd(voice_cmd_id_t cmd_id, const char *pinyin_str);

  // 離線語音播報輔助函式（將數值轉為 /sdcard/tts_offline/*.wav 序列並依序透過 sd_media_play_audio 播報）
  esp_err_t         sr_dispatcher_speak_temperature(float temp_c, int8_t trend);
  esp_err_t         sr_dispatcher_speak_humidity(float humi_pct);
  esp_err_t         sr_dispatcher_speak_lux(uint32_t lux);
  esp_err_t         sr_dispatcher_speak_time(uint8_t hour, uint8_t minute);
  esp_err_t         sr_dispatcher_speak_storage_status(uint8_t alert_flags, uint16_t eeprom_pct, uint32_t sd_free_mb);

  // 純邏輯輔助函式（供 Phase 8 Unity 單元測試直接驗證音節序列展開）
  size_t            sr_dispatcher_build_number_clips(float value, int decimal_places, char out_paths[][64], size_t max_clips);
  ```

---

## 4. FreeRTOS 雙核心任務與開機流程契約（Task & Boot Architecture）

### 4.1 `main.c` 開機與 Unity 測試模式分流規則
為避免開機後永遠卡在測試選單無法正常運作，`app_main()` 必須依序執行：
1. 執行單次系統服務初始化：`nvs_flash_init()`、`esp_netif_init()`、`esp_event_loop_create_default()`。
2. 呼叫 `board_init()`（初始化 GPIO、Mutex、I2C/SPI2/I2S 匯流排並執行開機自檢）。
3. **測試模式判定**：讀取 `BOARD_PIN_MODE_BTN`（GPIO 1，開機預設內部上拉）；**若開機瞬間按住模式鍵不放（電位為 `0` 持續超過 `300 ms`）**，則印出提示並呼叫 `unity_run_menu()` 進入序列埠 TDD 測試選單（不啟動背景業務任務，避免硬體資源衝突）；**若未按住按鍵（電位為 `1`）**，則正常初始化 7 個業務元件、綁定 Callback 並啟動雙核心 FreeRTOS 任務；*(於 Phase 8 `T-08-03` 啟用後)* 再接續呼叫 `sr_engine_init()` 與 `sr_engine_start_tasks()`。

### 4.2 雙核心任務配置表（禁止任務未阻塞空轉）
| 任務名稱 | 綁定核心 | 優先權 | Stack Size | 職責與阻塞／讓出 CPU 機制 |
| :--- | :--- | :--- | :--- | :--- |
| `audio_rx_task` | **Core 1** | `6`（最高） | `6144 B` | 讀取 I2S0 雙麥音訊流至 PSRAM 緩衝（透過 `i2s_channel_read` DMA 阻塞等待，並分發至 PCM Tap）。 |
| `audio_tx_task` | **Core 1** | `5` | `8192 B` | WAV/MP3 解碼與 I2S1 播放（無播音時透過 `ulTaskNotifyTake` 休眠；播音時每 4096B 釋放 SD Mutex）。 |
| `sensor_task` | **Core 1** | `4` | `6144 B` | 依 `sample_interval_sec` 定時採集 DHT22/GY-30/DS3231，寫入 PSRAM 與 EEPROM（`vTaskDelay` 休眠）。 |
| `oled_ui_task` | **Core 1** | `2` | `4096 B` | 每 `1000 ms` 刷新畫面與調光（透過 `board_i2c_write` 寫入 `0x3C`）。 |
| `net_sync_task` | **Core 0** | `3` | `8192 B` | 管理 Wi-Fi STA/AP 事件、Mode 1 歷史補傳（SD $\rightarrow$ EEPROM $\rightarrow$ Live）與即時上報。 |
| `httpd`（系統內建） | **Core 0** | `3` | `8192 B` | Mode 2 本地設定網頁伺服器（`httpd_config_t.core_id = 0`, `stack_size = 8192`）。 |
| `task_sr_feed` *(Phase 8)* | **Core 1** | `5` | `4096 B` | 從 PSRAM RingBuffer 取出 16kHz 雙聲道 PCM 幀餵入 `ESP-AFE`（雙麥波束成形與降噪）。 |
| `task_sr_detect` *(Phase 8)* | **Core 1** | `4` | `6144 B` | 執行 WakeNet / MultiNet 推論（模型權重位於 PSRAM），管理 6000ms 逾時狀態機並觸發指令回呼。 |

---

## 5. 關鍵演算法與 ESP-IDF v6.1.1 時序實作規格（Critical Implementation Details）

1. **`AT24C256` 2-Byte 位址轉接與 64-Byte 跨頁切分演算法（`eeprom_blackbox.c`）**：
   * 當 `eeprom_bb_init(NULL, NULL)` 被呼叫時，預設綁定內部靜態函式 `default_eeprom_write(mem_addr, data, len)` 與 `default_eeprom_read(mem_addr, data, len)`。
   * **跨頁切分**：計算當前頁剩餘空間 `page_rem = 64 - (addr % 64)`，單次寫入長度 `chunk = (len < page_rem) ? len : page_rem`。在區域陣列 `uint8_t pkt[66]` 中填入 `pkt[0] = (uint8_t)(addr >> 8)`、`pkt[1] = (uint8_t)(addr & 0xFF)` 與 `memcpy(&pkt[2], data, chunk)`，呼叫 `board_i2c_write(I2C_ADDR_EEPROM, pkt, chunk + 2)` 後，**強制執行 `vTaskDelay(pdMS_TO_TICKS(6))`** 等待內部燒錄完成。
   * **讀取轉接**：設定 `uint8_t addr_buf[2] = { (uint8_t)(mem_addr >> 8), (uint8_t)(mem_addr & 0xFF) }`，呼叫 `board_i2c_write_read(I2C_ADDR_EEPROM, addr_buf, 2, data, len)`。
2. **WAV/MP3 統一 16-bit 立體聲 PCM 管線（`sd_media.c`）**：
   * I2S1 初始化為 `16000 Hz`（或 `44100 Hz`）、`I2S_DATA_BIT_WIDTH_16BIT`、`I2S_SLOT_MODE_STEREO`。
   * 若檔案來源為單聲道（`channels == 1`），讀取 `N` 個 `int16_t` 樣本後，在緩衝區反向展開為 `2N` 個樣本（`out[2*i] = out[2*i+1] = in[i]`）再呼叫 `i2s_channel_write()`。
   * 取樣率變更時，若 `file_sample_rate != current_i2s_rate`，依序呼叫 `i2s_channel_disable(tx)` $\rightarrow$ `i2s_channel_reconfig_std_clock(tx, &clk_cfg)` $\rightarrow$ `i2s_channel_enable(tx)`。
   * 僅在 `sd_media.c` 頂部定義一次 `#define DR_MP3_IMPLEMENTATION` 再 `#include "dr_mp3.h"`。
3. **DHT22 單線精準時序與 SNTP 校時標頭檔（`sensor_hub`）**：
   * `dht22_driver.c` 必須使用 **`#include "esp_rom_sys.h"`** 的 `esp_rom_delay_us()` 計算脈衝寬度（嚴禁引用舊版 `ets_sys.h`）。主機拉低 `BOARD_PIN_DHT22` 時使用 `vTaskDelay(pdMS_TO_TICKS(20))` 讓出 CPU，僅針對後續約 `4.5 ms` 的 40-bit 資料脈衝讀取窗口進入 `portENTER_CRITICAL()`。
   * `sensor_hub.c` 聯網校時必須使用 **`#include "esp_sntp.h"`**（`esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL)`、`esp_sntp_setservername(0, "pool.ntp.org")`、`esp_sntp_init()`）。
4. **嚴格時間順序補傳（`sync_engine.c`）**：
   * 掃描 `/sdcard/sensor_offline/` 目錄下所有符合 `????????.bin` 的檔名，使用 `qsort()` 依字串字典序升冪排序（即日期由舊到新）。
   * 逐檔開啟並每次讀取最多 `50` 筆（`800 Bytes`），呼叫 `upload_fn(batch, count)`；僅當整份檔案所有批次皆回傳 `ESP_OK` 時才呼叫 `unlink(filepath)`。一旦任一批次失敗，立即中止補傳並保留檔案。
   * 確認目錄無殘留 `.bin` 檔後，從 `eeprom_bb_read_records()` 讀取剩餘筆數並分批上傳，全數取得 `ESP_OK` 後呼叫 `eeprom_bb_clear_partition_b()`。
5. **Mode 2 本地網頁設定伺服器（`web_config.c`）**：
   * 若 `sys_config_t.wifi_ssid` 非空，先啟動 STA 嘗試連線（逾時 `10 秒`）；若為空或逾時失敗，自動切換為 `WIFI_MODE_AP`（SSID：`Xiaozhi_Setup`、密碼：`12345678`、IP：`192.168.4.1`）。
   * 啟動 `httpd_start()` 時將 `config.core_id = 0` 與 `config.stack_size = 8192`；切離 Mode 2 時必須先呼叫 `httpd_stop()` 再停止 Wi-Fi 射頻。
6. **`[Phase 8 新增]` `ESP-SR` 雙階段離線語音狀態機、漢語拼音動態註冊與 SD 卡音節拼接播報（`sr_engine.c` & `sr_cmd_dispatcher.c`）**：
   * **模型載入與 PSRAM 隔離**：透過 `esp_srmodel_init("model")` 從 `3MB` `model` 分區載入中文模型列表，初始化 `ESP-AFE`（`AFE_TYPE_SR`，`pcm_config.total_ch_num = 2, pcm_config.mic_num = 2`）、WakeNet 與 MultiNet，所有模型權重與推論緩衝配置於 8MB Octal PSRAM。
   * **雙階段狀態機與 6 秒逾時**：
     * 開機預設處於 `SR_STATE_WAKE_LISTEN`，`task_sr_detect` 僅執行 WakeNet 監聽喚醒詞；偵測到 `WAKENET_DETECTED` 後，切換為 `SR_STATE_CMD_LISTEN`、紀錄 `cmd_listen_start_tick = xTaskGetTickCount()`、通知 `oled_ui` 顯示「聆聽中」狀態並播放 `/sdcard/tts_offline/wake.wav`。
     * 在 `SR_STATE_CMD_LISTEN` 下執行 `multinet->detect()`：若偵測到有效命令詞（`ESP_MN_STATE_DETECTED`），取出最高信心度 `phrase_id` 對應為 `voice_cmd_id_t` 並呼叫 `cmd_cb(cmd_id)` 後重設回 `SR_STATE_WAKE_LISTEN`；若 `(xTaskGetTickCount() - cmd_listen_start_tick) >= pdMS_TO_TICKS(6000)`，自動退回 `SR_STATE_WAKE_LISTEN` 並恢復 OLED 待機顯示。
   * **漢語拼音離線指令註冊（必須與 `spec.md` 第 6 章完全一致）**：
     ```c
     esp_mn_commands_clear();
     esp_mn_commands_add(VOICE_CMD_VOL_UP,          "da sheng yi dian");
     esp_mn_commands_add(VOICE_CMD_VOL_UP,          "tiao gao yin liang");
     esp_mn_commands_add(VOICE_CMD_VOL_DOWN,        "xiao sheng yi dian");
     esp_mn_commands_add(VOICE_CMD_VOL_DOWN,        "tiao di yin liang");
     esp_mn_commands_add(VOICE_CMD_MUTE_TOGGLE,     "jing yin");
     esp_mn_commands_add(VOICE_CMD_MUTE_TOGGLE,     "qu xiao jing yin");
     esp_mn_commands_add(VOICE_CMD_PLAY_MUSIC,      "bo fang yin yue");
     esp_mn_commands_add(VOICE_CMD_STOP_MUSIC,      "ting zhi bo fang");
     esp_mn_commands_add(VOICE_CMD_OLED_ON,         "da kai ping mu");
     esp_mn_commands_add(VOICE_CMD_OLED_OFF,        "guan bi ping mu");
     esp_mn_commands_add(VOICE_CMD_OLED_BRIGHT_UP,  "tiao liang ping mu");
     esp_mn_commands_add(VOICE_CMD_OLED_AUTO_BRIGHT,"zi dong liang du");
     esp_mn_commands_add(VOICE_CMD_NET_WEB_CFG,     "kai qi wang ye pei wang");
     esp_mn_commands_add(VOICE_CMD_NET_OFFLINE,     "qie huan li xian mo shi");
     esp_mn_commands_add(VOICE_CMD_NET_ONLINE,      "chong xin lian xian wang luo");
     esp_mn_commands_add(VOICE_CMD_REPORT_TEMP,     "xian zai ji du");
     esp_mn_commands_add(VOICE_CMD_REPORT_TEMP,     "wen du duo shao");
     esp_mn_commands_add(VOICE_CMD_REPORT_HUMI,     "xian zai shi du duo shao");
     esp_mn_commands_add(VOICE_CMD_REPORT_LUX,      "xian zai guang xian zen me yang");
     esp_mn_commands_add(VOICE_CMD_REPORT_ENV_ALL,  "bo bao huan jing zhuang kuang");
     esp_mn_commands_add(VOICE_CMD_REPORT_TIME,     "xian zai ji dian ji fen");
     esp_mn_commands_add(VOICE_CMD_REPORT_STORAGE,  "jian cha cun chu zhuang tai");
     esp_mn_commands_update();
     ```
   * **SD 卡音節拼接離線語音播報（`sr_dispatcher_build_number_clips`）**：
     * 將浮點數或整數拆解為 `/sdcard/tts_offline/` 內的基礎音節檔名陣列（例如 `25.3°C` 展開為 `["/sdcard/tts_offline/2.wav", "/sdcard/tts_offline/shi.wav", "/sdcard/tts_offline/5.wav", "/sdcard/tts_offline/dian.wav", "/sdcard/tts_offline/3.wav", "/sdcard/tts_offline/du.wav"]`），並依序呼叫 `sd_media_play_audio()` 播報，完全無需聯網。

---

## 6. 漸進式 TDD 開發階段規劃（Phased TDD Architecture Roadmap）

整體系統依架構依賴關係分為 9 個漸進階段推進（Phase 0 ～ Phase 7 為原規劃主線，Phase 8 為無新增硬體之離線語音擴充），確保每一層地基穩固後再往上疊加：

* **Phase 0（基礎骨架層）**：分區表、`system_types.h`、`board_config`（I2C 代理、SPI2/I2S 初始化、CRC16/32、開機按鍵測試分流與硬體自檢）。
* **Phase 1（記憶體快取層）**：`psram_cache`（Octal PSRAM 環形統計快取）。
* **Phase 2（第一線容災層）**：`eeprom_blackbox`（AT24C256 I2C 2-Byte 位址轉接、64B 分頁寫入、80% 警告、100% Callback 轉存）。
* **Phase 3（第二線儲存與媒體層）**：`sd_media`（FAT32 掛載、WAV/MP3 統一 PCM 播放、離線感測附加寫入與 CRC32 校驗）。
* **Phase 4（環境感測層）**：`sensor_hub`（DHT22、GY-30、DS3231 驅動與定時採集任務）。
* **Phase 5（人機顯示層）**：`oled_ui`（1024B 幀緩衝、環境光調光、置頂警告橫幅）。
* **Phase 6（語音互動層）**：`audio_engine`（雙麥克風收音、非阻塞 16kHz PCM 幀分發介面與離線命令分發）。
* **Phase 7（網路同步與設定層）**：`net_manager`（三態互斥切換、嚴格時間順序補傳、Mode 2 本地設定網頁伺服器與 `main.c` 全系統整合）。
* **Phase 8（無新增硬體之離線語音擴充層 — 須於 Phase 7 完成後執行）**：`sr_engine` + `sr_cmd_dispatcher`（引入 `espressif/esp-sr` 與 `3MB` `model` 分區、實作 `ESP-AFE` + WakeNet + MultiNet 雙階段離線語音識別、SD 卡音節拼接語音播報與 Phase 8 Unity 單元測試）。
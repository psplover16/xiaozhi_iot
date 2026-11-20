# Xiaozhi IoT 施工任務清單 (`tasks.md`)

> **給 AI 開發助手的強制執行規則（嚴格遵守，禁止跳步）**：
> 1. **單次單任務**：每次只允許實作使用者指定的「單一 Task ID」，禁止偷跑後續任務。
> 2. **遵守合約**：所有元件目錄、`CMakeLists.txt` 的 `REQUIRES` 依賴與 `.h` 標頭檔簽章，必須 100% 符合 `plan.md`（v1.2.0）與 `spec.md`（v1.9.1）（註：第 0／1 層元件名稱以工作區已完成之 `board_bsp`、`env_sensors` 為準）。
> 3. **純 ESP-IDF v6.1.1（C11）**：嚴禁出現任何 Arduino API（`Wire`、`Serial`、`millis()` 等）或已棄用的舊版 driver 標頭檔（如 `driver/i2c.h`、`driver/i2s.h`）。
> 4. **先測後進**：每個 Phase 的最後一個任務皆為 Unity 單元測試，必須確認編譯通過且測試覆蓋核心邊界條件後，才算完成該階段。

---

## 前置準備階段（手動建立專案骨架與外部依賴）

- [x] **T-PRE-01：建立專案目錄結構與建置基礎檔**
  - **目標檔案**：`CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv`、`main/CMakeLists.txt`、`main/main.c`
  - **實作規格**：
    1. 依照 `plan.md` 第 2.1 節建立完整目錄樹（含 `components/` 下 8 個元件資料夾及其 `include/` 與 `test/` 子目錄）。
    2. 根目錄 `CMakeLists.txt` 設定 `project(xiaozhi_iot)` 與 C11 標準。
    3. `partitions.csv` 設定自訂分割區表：`nvs` (24KB)、`phy_init` (4KB)、`factory` app (4MB / `0x400000`)。
    4. `sdkconfig.defaults` 強制開啟：`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`、`CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_MODE_OCT=y`、`CONFIG_SPIRAM_SPEED_80M=y`、`CONFIG_PARTITION_TABLE_CUSTOM=y`、`CONFIG_FREERTOS_HZ=1000`。
    5. `main/main.c` 先寫最小可編譯 `app_main()`（僅印出開機 Log），執行 `idf.py build` 確認基礎環境無誤。

- [x] **T-PRE-02：手動放入單檔 MP3 解碼庫 `dr_mp3.h`（人工操作）**
  - **目標檔案**：`components/sd_media/include/dr_mp3.h`
  - **實作規格**：
    1. 從官方開源庫 `mackron/dr_libs` 下載最新單檔標頭檔 `dr_mp3.h`，手動複製至 `components/sd_media/include/dr_mp3.h`。
    2. 確認檔案存在且非空（此檔在 Phase 3 才會被 `#include` 編譯）。

- [x] **T-PRE-03：建立全部 8 個元件的鎖定標頭檔（`.h`）**
  - **目標檔案**：
    - `components/board_bsp/include/board_bsp.h`
    - `components/psram_cache/include/psram_cache.h`
    - `components/eeprom_blackbox/include/eeprom_blackbox.h`
    - `components/sd_media/include/sd_media.h`
    - `components/env_sensors/include/env_sensors.h`
    - `components/oled_ui/include/oled_ui.h`
    - `components/audio_engine/include/audio_engine.h`
    - `components/net_manager/include/net_manager.h`
  - **實作規格**：
    1. 將 `plan.md` 第 3.1 至 3.8 節定義的所有 `.h` 標頭檔內容一字不差寫入對應檔案（含 `#pragma once`、`extern "C"` 與完整結構體/函式簽章）。
    2. 嚴禁自行修改函式名稱、參數型別或回傳值。

---

## Phase 0：硬體抽象基礎層（`board_bsp`）

- [x] **T-00-01：建立 `board_bsp` 建置檔與腳位常數定義**
  - **目標檔案**：`components/board_bsp/CMakeLists.txt`、`components/board_bsp/board_bsp.c`（骨架）
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 第 2.3 節設定：`REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_ledc`。
    2. 在 `board_bsp.c` 中以 `#define` 鎖死 `spec.md` 腳位表（I2C SDA=GPIO 8 / SCL=GPIO 9、SPI MOSI=GPIO 11 / MISO=GPIO 13 / CLK=GPIO 12 / SD_CS=GPIO 10、I2S_IN BCLK=GPIO 4 / WS=GPIO 5 / DIN=GPIO 6、I2S_OUT BCLK=GPIO 15 / LRC=GPIO 16 / DOUT=GPIO 7、DHT22=GPIO 14、Mode_BTN=GPIO 1、Backlight_PWM=GPIO 2）。

- [x] **T-00-02：實作 I2C Master 匯流排與執行緒安全 Mutex 讀寫**
  - **目標檔案**：`components/board_bsp/board_bsp.c`
  - **實作規格**：
    1. 使用 ESP-IDF v6.1.1 新版 API `i2c_new_master_bus()` 初始化 I2C0（`400 kHz`、內部上拉致能），並建立一把全域 `SemaphoreHandle_t` 互斥鎖保護整條 I2C 匯流排。
    2. 實作 `board_i2c_write()` 與 `board_i2c_read()`：動態取得或快取對應 `dev_addr` 的 `i2c_master_dev_handle_t`，每次傳輸前後必須 `xSemaphoreTake` / `xSemaphoreGive`（逾時 100ms），確保多核心同時存取 OLED、RTC、GY-30 與 EEPROM 時絕不發生匯流排碰撞。

- [x] **T-00-03：實作 SPI 匯流排、GPIO 按鍵與 LEDC 背光 PWM**
  - **目標檔案**：`components/board_bsp/board_bsp.c`
  - **實作規格**：
    1. 使用 `spi_bus_initialize()` 初始化 `SPI2_HOST`（DMA 自動通道），供後續 SD 卡掛載使用。
    2. 初始化 `GPIO 1` 為輸入模式並開啟內部上拉（`GPIO_PULLUP_ONLY`），實作 `board_btn_is_pressed()`（低電位為按下）。
    3. 初始化 LEDC Timer 與 Channel 輸出 PWM 至 `GPIO 2`（頻率 `5 kHz`、8-bit 解析度 `0~255`），實作 `board_set_backlight(uint8_t duty)`。

- [x] **T-00-04：撰寫 `board_bsp` 單元測試與開機測試分流**
  - **目標檔案**：`components/board_bsp/test/test_board_bsp.c`、`main/main.c`
  - **實作規格**：
    1. 在 `test_board_bsp.c` 撰寫 Unity 測試：掃描 I2C 匯流排確認至少能偵測到 `0x3C` (OLED)、`0x68` (DS3231)、`0x23` (GY-30)、`0x50` (AT24C256) 之 ACK 回應，並測試 `board_set_backlight()` 正常不崩潰。
    2. 在 `main/main.c` 實作開機檢查：呼叫 `board_bsp_init()` 後延遲 `300 ms`，若 `board_btn_is_pressed()` 仍為 `true`，則呼叫 `unity_run_menu()` 進入測試選單；否則繼續正常啟動流程。執行 `idf.py build` 驗證通過。

---

## Phase 1：PSRAM 環形快取層（`psram_cache`）

- [x] **T-01-01：實作 512KB PSRAM 環形緩衝區與執行緒安全寫入**
  - **目標檔案**：`components/psram_cache/CMakeLists.txt`、`components/psram_cache/psram_cache.c`
  - **實作規格**：
    1. `CMakeLists.txt` 設定 `REQUIRES freertos esp_psram`。
    2. 使用 `heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 分配 512KB 環形緩衝區（若分配失敗回傳 `ESP_ERR_NO_MEM`）。
    3. 實作 `psram_cache_push()`：以 `SemaphoreHandle_t` 保護頭尾指標，將 `log_record_t` 寫入環形佇列；當緩衝區剩餘空間不足時，覆寫最舊資料並遞增溢出計數器，絕不阻塞呼叫端。

- [x] **T-01-02：實作批次讀取、確認提交與 `psram_cache` 單元測試**
  - **目標檔案**：`components/psram_cache/psram_cache.c`、`components/psram_cache/test/test_psram_cache.c`
  - **實作規格**：
    1. 實作 `psram_cache_peek_batch()`（只讀取不移動讀指標，支援跨環形陣列尾端折返）與 `psram_cache_commit_pop()`（確認下層寫入成功後才移動讀指標釋放空間）。
    2. 撰寫 `test_psram_cache.c` Unity 測試：驗證指標分配在 PSRAM 位址空間（`esp_ptr_external_ram()` 為真）、環形折返讀寫一致性、以及寫滿覆寫時的邊界安全性。

---

## Phase 2：AT24C256 離線黑盒子（`eeprom_blackbox`）

- [x] **T-02-01：建立 `eeprom_blackbox` 建置檔與 2-Byte 位址 I2C 轉接層**
  - **目標檔案**：`components/eeprom_blackbox/CMakeLists.txt`、`components/eeprom_blackbox/eeprom_blackbox.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp freertos`。
    2. 實作內部預設 I/O 轉接函式 `default_eeprom_write(uint16_t mem_addr, const uint8_t *data, size_t len)` 與 `default_eeprom_read(...)`：將 16-bit 記憶體偏移量拆為高/低 2 位元組（`[mem_addr >> 8, mem_addr & 0xFF]`），寫入時拼裝 `[addr_hi, addr_lo, data...]` 後呼叫 `board_i2c_write(0x50, ...)`；讀取時先寫入 2-Byte 位址再呼叫 `board_i2c_read(0x50, ...)`。
    3. 支援 `eeprom_blackbox_set_io_hooks()` 注入自訂 Mock 函式，以便無硬體環境下執行單元測試。

- [x] **T-02-02：實作 64-Byte 頁邊界自動切割寫入與磨損均衡指標**
  - **目標檔案**：`components/eeprom_blackbox/eeprom_blackbox.c`
  - **實作規格**：
    1. **嚴格遵守硬體時序紅線**：任何跨越 `64-Byte` 頁邊界（`mem_addr % 64`）的寫入，必須自動拆分為多筆不跨頁的小寫入，且每筆 I2C 頁寫入完成後**強制呼叫 `vTaskDelay(pdMS_TO_TICKS(6))`**（大於 5ms 寫入週期），絕不允許跨頁回捲覆寫。
    2. 將 `0x0000 ~ 0x003F`（前 64 Bytes）規劃為雙備份 Metadata 區塊（記錄 `write_head`、`read_tail`、`seq_num` 與 CRC16），每次寫入採 Ping-Pong 交替更新，實現磨損均衡並防止斷電損毀指標。
    3. `0x0040 ~ 0x7FFF`（32,704 Bytes）作為環形日誌儲存區，實作 `eeprom_blackbox_append()`、`eeprom_blackbox_read_oldest()` 與 `eeprom_blackbox_pop_oldest()`。

- [x] **T-02-03：撰寫 `eeprom_blackbox` 跨頁與掉電一致性單元測試**
  - **目標檔案**：`components/eeprom_blackbox/test/test_eeprom_blackbox.c`
  - **實作規格**：
    1. 使用 Mock 32KB 記憶體陣列並加入「頁回捲模擬器」（若單次寫入跨過 `64` 倍數邊界則故意模擬 AT24C256 頁首覆寫錯亂），驗證 `eeprom_blackbox_append()` 在任意偏移位址寫入跨頁資料時皆能 100% 完整讀回。
    2. 驗證寫滿 32KB 環形折返覆寫最舊資料、以及重新呼叫 `eeprom_blackbox_init()` 後能正確從 Metadata 恢復斷電前的讀寫指標。

---

## Phase 3：SD 卡儲存與 WAV/MP3 播放（`sd_media`）

- [x] **T-03-01：建立 `sd_media` 建置檔與 FATFS 掛載管理**
  - **目標檔案**：`components/sd_media/CMakeLists.txt`、`components/sd_media/sd_media.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp fatfs sdmmc`。
    2. 使用 `esp_vfs_fat_sdspi_mount()` 掛載 SD 卡至 `/sdcard`（使用 `SPI2_HOST`、`SD_CS = GPIO 10`、`max_files = 5`、`allocation_unit_size = 16 * 1024`）。
    3. 若開機時未插卡或掛載失敗，標記 `is_mounted = false` 並回傳錯誤碼，**嚴禁觸發 `ESP_ERROR_CHECK` 導致系統重啟**；實作 `sd_media_is_mounted()` 供降級策略查詢。

- [x] **T-03-02：實作按日歸檔日誌寫入（`YYYYMMDD.bin`）與 CRC32 校驗**
  - **目標檔案**：`components/sd_media/sd_media.c`
  - **實作規格**：
    1. 實作 `sd_media_append_logs()`：依據 `log_record_t` 的 Unix 時間戳轉換為 UTC+8 日期字串，開啟 `/sdcard/logs/YYYYMMDD.bin`（若目錄不存在則自動 `mkdir`）。
    2. **防掉電資料遺失強制規範**：每筆寫入封裝為 `[Magic: 0xA55A (2B)][Length (2B)][Payload (N B)][CRC32 (4B)]`，開啟檔案一律使用附加模式 `"ab"`，寫入後**強制呼叫 `fflush(fp)` 與 `fsync(fileno(fp))`** 再 `fclose(fp)`。
    3. 實作 `sd_media_read_unsynced()`：掃描目錄下最舊的未同步 `.bin` 檔，逐筆驗證 Magic 與 CRC32（遇損毀封包自動跳過並記錄警告），回傳有效紀錄供補傳模組使用。

- [x] **T-03-03：整合 `dr_mp3.h` 與 WAV 解析器實現統一 16-bit PCM 串流解碼**
  - **目標檔案**：`components/sd_media/sd_media.c`
  - **實作規格**：
    1. 在 `sd_media.c` 頂部定義 `#define DR_MP3_IMPLEMENTATION` 與 `#define DR_MP3_NO_SIMD` 後 `#include "dr_mp3.h"`（使用 PSRAM 分配解碼器實例，避免佔用內部 SRAM）。
    2. 實作 `sd_media_open_audio()`、`sd_media_read_pcm_frames()` 與 `sd_media_close_audio()`：
       - 若副檔名為 `.wav`：解析 RIFF/fmt/data 標頭，取得取樣率與聲道數，直接讀取 16-bit PCM 樣本。
       - 若副檔名為 `.mp3`：透過 `drmp3_init_file()` 初始化串流解碼器，每次呼叫 `drmp3_read_pcm_frames_s16()` 輸出標準 16-bit signed PCM 樣本，並支援單聲道自動複製為雙聲道交錯格式（L/R 相同），統一輸出規格給 I2S 播放。

- [x] **T-03-04：撰寫 `sd_media` 日誌寫入與音訊解碼單元測試**
  - **目標檔案**：`components/sd_media/test/test_sd_media.c`
  - **實作規格**：
    1. 測試在無 SD 卡情況下呼叫 `sd_media_append_logs()` 能安全回傳 `ESP_ERR_INVALID_STATE` 且不崩潰。
    2. 若測試環境有插 SD 卡（或掛載 SPIFFS/FatFS 測試分區），驗證寫入 10 筆日誌後讀回之 CRC32 100% 吻合，並測試開啟測試用 `.wav` / `.mp3` 檔能正確取得取樣率與非零 PCM 幀資料。

---

## Phase 4: 環境感測與時間基準 (`env_sensors` & `sensor_hub`)

- [x] **T-04-01**: 建立 `components/env_sensors/` 基礎架構與 I2C 感測器驅動
  - 檔案：`components/env_sensors/CMakeLists.txt`、`components/env_sensors/env_sensors_drv.h`、`components/env_sensors/env_ds3231.c`、`components/env_sensors/env_gy30.c`
  - 內容：實作 DS3231 (`0x68`) BCD 時間讀寫與 `settimeofday()` 系統時間同步；實作 GY-30/BH1750 (`0x23`) 連續高解析度光照度（Lux）讀取。

- [x] **T-04-02**: 實作 DHT22 微秒級單匯流排溫濕度獨立驅動
  - 檔案：`components/env_sensors/env_dht22.c`（同步更新 `env_sensors_drv.h` 與 `CMakeLists.txt`）
  - 內容：使用 `GPIO 14` 開漏模式、`esp_rom_sys.h` 與 `portMUX_TYPE` 臨界區（硬上限 `< 5ms`）讀取 40-bit 溫濕度與 Checksum 校驗，失敗時回傳 `INVALID_TEMP_VALUE` / `INVALID_HUMI_VALUE`。

- [x] **T-04-03**: 實作 `sensor_hub` 整合層、Core 1 採集任務與 Phase 4 Unity 單元測試
  - 檔案：`components/sensor_hub/CMakeLists.txt`、`components/sensor_hub/sensor_hub.c`（以及 Phase 4 Unity 測試檔）
  - 內容：建立 `sensor_hub` 元件並依賴 `env_sensors`、`psram_cache`、`board_bsp`；實作 `sensor_hub_init`、`sensor_hub_start_task`（Pin to Core 1，每 2 秒輪詢三感測器、組裝 16B `sensor_record_t` 含 CRC16 並寫入 PSRAM 快取與觸發 Callback）、`sensor_hub_sync_ntp`（`esp_sntp.h` 校準 DS3231）、`sensor_hub_get_date_yyyymmdd`，並完成 Unity 單元測試。



---

## Phase 5：OLED 顯示與自適應調光（`oled_ui`）

- [x] **T-05-01：實作 SSD1306 128x64 Framebuffer 繪圖引擎與狀態列 UI**
  - **目標檔案**：`components/oled_ui/CMakeLists.txt`、`components/oled_ui/oled_ui.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp freertos`。
    2. 在內部 SRAM 配置 `1024 Bytes`（`128 * 64 / 8`）顯示緩衝區，透過 `board_i2c_write(0x3C, ...)` 發送 SSD1306 初始化序列與整頁刷新指令（水平尋址模式），嚴禁在 I2C 傳輸期間持有畫面鎖（應先複製本地快照再透過 I2C 送出）。
    3. 實作內建 `6x8` 與 `8x16` ASCII 點陣字型與基礎圖元繪製，規劃三段式版面：
       - **頂部狀態列（Y=0~15）**：左側顯示網路連線狀態圖示（Wi-Fi 在線 / 離線降級 / AP 配網）、右側顯示待同步日誌筆數。
       - **中央主視覺區（Y=16~47）**：大字體顯示 `HH:MM:SS` 時間或語音狀態表情/波形動畫（待機 / 聆聽中 / 思考中 / 播放中）。
       - **底部資訊列（Y=48~63）**：顯示即時溫度（`°C`）、濕度（`%`）與照度（`lx`）。

- [x] **T-05-02：實作環境光自適應背光調光與 `oled_ui` 單元測試**
  - **目標檔案**：`components/oled_ui/oled_ui.c`、`components/oled_ui/test/test_oled_ui.c`
  - **實作規格**：
    1. 建立 UI 刷新任務 `task_oled_refresh`（**釘選於 Core 1**，優先級 2，刷新率 `10 FPS` / 100ms 週期）。
    2. 實作 `oled_ui_update_auto_brightness(float lux)` 平滑調光演算法（指數移動平均 EMA 濾波防閃爍）：環境光 `< 20 lux` 時背光 PWM 降至 `25`（夜間護眼模式）並同步降低 SSD1306 對比度暫存器（`0x81`）；`20 ~ 300 lux` 線性映射至 `50 ~ 180`；`> 300 lux` 設為 `255`。
    3. 撰寫 `test_oled_ui.c` Unity 測試：驗證狀態切換 API 執行緒安全性，以及不同照度輸入下 PWM 輸出值符合單調遞增且不發生劇烈抖動。

---

## Phase 6：雙麥語音引擎與動態取樣率播放（`audio_engine`）

- [x] **T-06-01：實作雙 INMP441 立體聲 I2S RX 錄音、VAD 與 PCM 幀分發掛載點**
  - **目標檔案**：`components/audio_engine/CMakeLists.txt`、`components/audio_engine/audio_engine.c`
  - **實作規格**：
    1. `CMakeLists.txt` 設定：`REQUIRES board_bsp sd_media esp_driver_i2s esp_psram freertos`。
    2. 使用 ESP-IDF v6.1.1 新版 API `i2s_new_channel()` + `i2s_channel_init_std_mode()` 初始化 `I2S_NUM_0` 為 RX 標準模式（`16 kHz`、`32-bit` 槽寬、`I2S_SLOT_MODE_STEREO`，`BCLK=GPIO 4`、`WS=GPIO 5`、`DIN=GPIO 6`，左聲道 L/R 接 GND、右聲道 L/R 接 3.3V），讀入後右移 16 位元轉為 16-bit 雙聲道交錯 PCM。
    3. 實作雙麥能量計算與自適應噪聲底線語音活動偵測（VAD）：持續追蹤背景 RMS 能量，當前景人聲能量超過動態門檻且持續 `150 ms` 觸發 `AUDIO_EVENT_SPEECH_START`；靜音超過 `800 ms` 觸發 `AUDIO_EVENT_SPEECH_END`，將錄音 PCM 存入 PSRAM 語音緩衝區（上限 10 秒 / 320KB）。
    4. **[Phase 8 前向相容介面]**：實作非阻塞 `audio_engine_register_pcm_tap()`，每當 `I2S_NUM_0` 讀入一幀 `16 kHz / 16-bit` 雙聲道交錯 PCM 時，同步呼叫已註冊的 `audio_pcm_frame_cb_t`（供 Phase 8 `ESP-AFE` 訂閱，嚴禁阻塞 I2S RX 讀取迴圈），並實作 `audio_engine_inject_cmd()` 供指令分發與測試。

- [x] **T-06-02：實作 MAX98357A I2S TX 播放、動態取樣率重配置與音量／靜音控制**
  - **目標檔案**：`components/audio_engine/audio_engine.c`
  - **實作規格**：
    1. 使用 `i2s_new_channel()` 初始化 `I2S_NUM_1` 為 TX 標準模式（`16-bit` 立體聲槽寬，`BCLK=GPIO 15`、`LRC=GPIO 16`、`DOUT=GPIO 7`）。
    2. **動態取樣率切換強制規範**：實作 `audio_engine_play_file(const char *filepath)`，先呼叫 `sd_media_open_audio()` 取得音訊檔真實取樣率（如 `16000 Hz`、`22050 Hz`、`44100 Hz` 或 `48000 Hz`），接著依序執行 **`i2s_channel_disable(tx_chan)` → `i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg)` → `i2s_channel_enable(tx_chan)`**，防止播放不同取樣率的 MP3/WAV 時變調或破音；並提供同步等待播放完畢機制，支援 Phase 8 多段短 WAV 音節連續無縫拼接播報。
    3. 實作軟體數位音量縮放（`audio_engine_set_volume(uint8_t pct)`，`0~100%` 步進 `10%`）、靜音切換（`audio_engine_set_mute(bool mute)`）與 `audio_engine_stop_playback()` 隨時打斷播放（Barge-in 支援）。

- [x] **T-06-03：撰寫 `audio_engine` 音訊管線、PCM Tap 與 I2S 時脈切換單元測試**
  - **目標檔案**：`components/audio_engine/test/test_audio_engine.c`
  - **實作規格**：
    1. 建立 `task_audio_io`（**釘選於 Core 1**，最高優先級 6），確保音訊讀寫與重採樣絕不受 Core 0 網路傳輸阻塞。
    2. 撰寫 `test_audio_engine.c` Unity 測試：驗證連續切換 `16000 Hz` 與 `44100 Hz` 取樣率時 `i2s_channel_reconfig_std_clock()` 回傳 `ESP_OK` 無記憶體洩漏、測試播放過程中呼叫 `audio_engine_stop_playback()` 能在 `50 ms` 內乾淨停止，並驗證 `audio_engine_inject_cmd()` 與 PCM Tap 註冊/取消機制正常運作。

---

## Phase 7：三態網路、按時間順序補傳與全系統總組裝（`net_manager` + `main`）

- [x] **T-07-01：實作三態網路狀態機（AP 配網 / STA 在線 / 離線降級）與 NVS 設定存取**
  - **目標檔案**：`components/net_manager/CMakeLists.txt`、`components/net_manager/net_manager.c`
  - **實作規格**：
    1. `CMakeLists.txt` 設定：`REQUIRES board_bsp psram_cache eeprom_blackbox sd_media env_sensors sensor_hub esp_wifi esp_netif esp_event esp_http_server esp_http_client nvs_flash lwip`。
    2. 從 NVS 讀取儲存的 Wi-Fi SSID/密碼與伺服器 URL；若 NVS 為空、開機按住模式鍵或收到外部模式切換請求（`net_manager_set_mode(NET_MODE_WEB_CFG)`），啟動 SoftAP（SSID: `Xiaozhi-Setup`）與內建輕量 HTTP 設定網頁（可設定 Wi-Fi、Server URL 與時區，並執行參數邊界檢查）。
    3. 若有 Wi-Fi 設定則啟動 STA 連線並開啟 `esp_sntp.h`（**嚴禁使用舊版 `sntp.h`**）校時，校時成功後自動寫回 DS3231 RTC；若連續重連失敗超過 3 次或切換至 `NET_MODE_OFFLINE`，自動進入離線降級模式（背景每 30 秒低功耗探測重連，絕不阻塞本機語音/感測/OLED）。

- [x] **T-07-02：實作四級離線容災寫入管線與「按時間順序」斷網補傳引擎**
  - **目標檔案**：`components/net_manager/net_manager.c`
  - **實作規格**：
    1. **寫入管線**：背景任務定期從已完成的 `psram_cache_peek_batch()` 取出紀錄；若當前為 `NET_MODE_ONLINE` 則優先 HTTP POST 批次上傳伺服器（收到 HTTP 200 後呼叫 `psram_cache_commit_pop()`）；若離線或上傳失敗，則依序降級寫入 `sd_media_append_logs()`（SD 卡存在時）或 `eeprom_blackbox_append()`（SD 卡拔除或損壞時）。
    2. **嚴格遵守「按時間順序補傳」紅線**：當偵測到網路從離線恢復為 `NET_MODE_ONLINE` 時，補傳引擎**必須嚴格依照資料產生的時間先後順序（Oldest First）**執行排空：
       - **第一步**：先將 `eeprom_blackbox` 內殘留的最舊離線紀錄透過 `eeprom_blackbox_read_oldest()` 讀出並上傳至伺服器，確認 HTTP 200 後才呼叫 `eeprom_blackbox_pop_oldest()`（先清空最老的，再傳較新的）。
       - **第二步**：接著呼叫 `sd_media_read_unsynced()` 掃描 SD 卡 `/sdcard/logs/` 下檔名日期最舊的未同步 `YYYYMMDD.bin` 檔案，逐批上傳並確認伺服器 ACK 後標記為已同步。
       - **第三步**：最後才恢復 `psram_cache` 即時資料流上傳，確保伺服器端收到的時間序列 100% 連續且不亂序。

- [x] **T-07-03：實作雲端語音對話上傳與 TTS 串流播放對接**
  - **目標檔案**：`components/net_manager/net_manager.c`
  - **實作規格**：
    1. 當 Core 1 語音引擎觸發 `AUDIO_EVENT_SPEECH_END`（且非 Phase 8 已命中之本機離線指令）時，透過 FreeRTOS Queue 通知 Core 0 網路任務（**釘選於 Core 0**，優先級 4）。
    2. 在線模式下透過 `esp_http_client` 將 PSRAM 錄音緩衝區上傳至後端語音介面，接收回傳的回覆音訊（儲存至 PSRAM 暫存檔或 `/sdcard/cache/reply.mp3`）後，通知 `audio_engine_play_file()` 播報；離線模式下則直接播放本機 SD 卡預設離線提示音（如 `/sdcard/audio/offline_hint.mp3`）。

- [x] **T-07-04：全系統雙核心總組裝（`main.c`）與整合驗證**
  - **目標檔案**：`main/CMakeLists.txt`、`main/main.c`
  - **實作規格**：
    1. `main/CMakeLists.txt` 列入專案全部元件（`board_bsp psram_cache eeprom_blackbox sd_media env_sensors sensor_hub oled_ui audio_engine net_manager`）與 `unity`、`nvs_flash`、`esp_netif`、`esp_event`。
    2. 在 `main/main.c` 依序完成：NVS 初始化 → `board_bsp_init()` → 300ms 開機按鍵檢查（長按進 `unity_run_menu()`）→ `psram_cache_init()` → `eeprom_blackbox_init()` → `sd_media_init()` → `env_sensors` / `sensor_hub_init()` → `oled_ui_init()` → `audio_engine_init()` → `net_manager_init()`，並註冊中央命令回呼（負責在 Core 1 安全派發音量、OLED 開關／亮度、網路三態切換與感測數值查詢）。
    3. 確認所有背景任務嚴格落實雙核心隔離（**Core 0**：Wi-Fi / HTTP / SNTP / 離線補傳；**Core 1**：I2S 雙麥錄音與播放 / DHT22 與 I2C 感測 / OLED 刷新），執行完整 `idf.py build` 零警告通過，並印出開機可用內部 SRAM 與 PSRAM 剩餘容量報告。

---

## Phase 8：無新增硬體之離線語音喚醒與命令識別（`ESP-SR` 擴充）

- [ ] **T-08-01：配置 `3MB` `model` 分區與 `espressif/esp-sr` 元件依賴**
  - **目標檔案**：`partitions.csv`、`components/audio_engine/idf_component.yml`、`components/audio_engine/include/sr_engine.h`、`components/audio_engine/CMakeLists.txt`
  - **實作規格**：
    1. 修改 `partitions.csv`：保留既有的 `nvs`、`phy_init`、`factory`（`0x400000`）分區偏移與大小完全不變，於 `0x410000` 處新增 `model, data, spiffs, 0x410000, 0x300000,`（`3MB`）與 `coredump, data, coredump, 0x710000, 0x10000,`。
    2. 建立 `components/audio_engine/idf_component.yml`，宣告 `dependencies: { espressif/esp-sr: "^1.9.0" }`（或相容 ESP-IDF v6.1.1 之最新穩定版），並建立符合 `plan.md` 第 3.6 節簽章之 `sr_engine.h`。
    3. 更新 `components/audio_engine/CMakeLists.txt`，將 `espressif__esp-sr` 加入 `REQUIRES`（先行建立空骨架實作或分階段編譯），執行 `idf.py reconfigure` 與 `idf.py build` 確認官方 `esp-sr` 模型正確打包進 `model` 分區且 PSRAM 配置無誤。

- [ ] **T-08-02：實作 `ESP-AFE` 雙麥降噪與 WakeNet / MultiNet 雙階段狀態機（`sr_engine.c`）**
  - **目標檔案**：`components/audio_engine/sr_engine.c`、`components/audio_engine/CMakeLists.txt`
  - **實作規格**：
    1. 實作 `sr_engine_init()`：透過 `esp_srmodel_init("model")` 載入模型，初始化 `ESP-AFE`（`AFE_TYPE_SR`，`total_ch_num = 2, mic_num = 2`，利用雙 INMP441 進行波束成形與降噪）、`WakeNet`（預設喚醒詞）與 `MultiNet`（中文命令詞模型），所有模型權重與環形緩衝配置於 8MB Octal PSRAM（`MALLOC_CAP_SPIRAM`）。
    2. 透過 `audio_engine_register_pcm_tap()` 訂閱 `16 kHz` 雙聲道交錯 PCM 幀寫入 PSRAM RingBuffer，建立兩個釘選於 **Core 1** 之任務：`task_sr_feed`（優先級 5，餵入 `ESP-AFE`）與 `task_sr_detect`（優先級 4，執行 WakeNet / MultiNet 推論）。
    3. 實作雙階段狀態機與 `6000 ms` 逾時保護：
       - `SR_STATE_WAKE_LISTEN` 命中喚醒詞後切換為 `SR_STATE_CMD_LISTEN`、通知 `oled_ui` 顯示「聆聽中」並播放 `/sdcard/tts_offline/wake.wav`。
       - 在 `SR_STATE_CMD_LISTEN` 下使用 `esp_mn_commands_clear()` / `esp_mn_commands_add()` 註冊全部 18 條漢語拼音指令（音量、播放、OLED 開關與調光、三態網路切換、溫濕度／光照／時間／儲存狀態查詢），識別成功即觸發 `cmd_cb(cmd_id)` 並重置回 `SR_STATE_WAKE_LISTEN`；若超過 `6000 ms` 無有效命令則自動退回待機。

- [ ] **T-08-03：實作離線指令動作派發、SD 卡音節拼接播報與 Phase 8 Unity 單元測試**
  - **目標檔案**：`components/audio_engine/sr_cmd_dispatcher.c`、`components/audio_engine/test/test_sr_engine.c`、`main/main.c`
  - **實作規格**：
    1. 實作 `sr_cmd_dispatcher.c`：提供 `sr_dispatcher_build_number_clips()` 與 `sr_dispatcher_speak_temperature()` / `humidity()` / `lux()` / `time()` / `storage_status()`，將即時數值拆解為 `/sdcard/tts_offline/*.wav` 基礎音節路徑陣列（例如 `25.3°C` 展開為 `2.wav`、`shi.wav`、`5.wav`、`dian.wav`、`3.wav`、`du.wav`），並依序呼叫 `audio_engine_play_file()` 連續播報（若 SD 卡未掛載或缺失音檔則安全降級為 OLED 顯示，絕不崩潰）。
    2. 在 `main/main.c` 的正常開機流程（`net_manager_init()` 之後）掛載 `sr_engine_init()` 與 `sr_engine_start_tasks()`，並將中央命令回呼接通 18 條離線指令。
    3. 撰寫 `components/audio_engine/test/test_sr_engine.c` Unity 測試：驗證拼音指令表註冊數量與 ID 映射正確性、模擬 `SR_STATE_CMD_LISTEN` 超過 `6000 ms` 自動退回 `SR_STATE_WAKE_LISTEN`、驗證 `sr_dispatcher_build_number_clips()` 數值拆解音節陣列 100% 吻合預期，並確認無 SD 卡時安全降級不崩潰。
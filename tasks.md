# Xiaozhi IoT 施工任務清單 (`tasks.md`)

> **給 AI 開發助手的強制執行規則（嚴格遵守，禁止跳步）**：
> 1. **單次單任務**：每次只允許實作使用者指定的「單一 Task ID」，禁止偷跑後續任務。
> 2. **遵守合約**：所有元件目錄、`CMakeLists.txt` 的 `REQUIRES` 依賴與 `.h` 標頭檔簽章，必須 100% 符合 `plan.md`（v1.1.0）與 `spec.md`（v1.7.0）。
> 3. **純 ESP-IDF v6.1.1（C11）**：嚴禁出現任何 Arduino API（`Wire`、`Serial`、`millis()` 等）或已棄用的舊版 driver 標頭檔（如 `driver/i2c.h`、`driver/i2s.h`）。
> 4. **先測後進**：每個 Phase 的最後一個任務皆為 Unity 單元測試，必須確認編譯通過且測試覆蓋核心邊界條件後，才算完成該階段。

---

## 前置準備階段（手動建立專案骨架與外部依賴）

- [ ] **T-PRE-01：建立專案目錄結構與建置基礎檔**
  - **目標檔案**：`CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv`、`main/CMakeLists.txt`、`main/main.c`
  - **實作規格**：
    1. 依照 `plan.md` 第 2.1 節建立完整目錄樹（含 `components/` 下 8 個元件資料夾及其 `include/` 與 `test/` 子目錄）。
    2. 根目錄 `CMakeLists.txt` 設定 `project(xiaozhi_iot)` 與 C11 標準。
    3. `partitions.csv` 設定自訂分割區表：`nvs` (24KB)、`phy_init` (4KB)、`factory` app (4MB / `0x400000`)。
    4. `sdkconfig.defaults` 強制開啟：`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`、`CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_MODE_OCT=y`、`CONFIG_SPIRAM_SPEED_80M=y`、`CONFIG_PARTITION_TABLE_CUSTOM=y`、`CONFIG_FREERTOS_HZ=1000`。
    5. `main/main.c` 先寫最小可編譯 `app_main()`（僅印出開機 Log），執行 `idf.py build` 確認基礎環境無誤。

- [ ] **T-PRE-02：手動放入單檔 MP3 解碼庫 `dr_mp3.h`（人工操作）**
  - **目標檔案**：`components/sd_media/include/dr_mp3.h`
  - **實作規格**：
    1. 從官方開源庫 `mackron/dr_libs` 下載最新單檔標頭檔 `dr_mp3.h`，手動複製至 `components/sd_media/include/dr_mp3.h`。
    2. 確認檔案存在且非空（此檔在 Phase 3 才會被 `#include` 編譯）。

- [ ] **T-PRE-03：建立全部 8 個元件的鎖定標頭檔（`.h`）**
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

- [ ] **T-00-01：建立 `board_bsp` 建置檔與腳位常數定義**
  - **目標檔案**：`components/board_bsp/CMakeLists.txt`、`components/board_bsp/board_bsp.c`（骨架）
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 第 2.3 節設定：`REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_ledc`。
    2. 在 `board_bsp.c` 中以 `#define` 鎖死 `spec.md` 腳位表（I2C SDA=GPIO 8 / SCL=GPIO 9、SPI MOSI=GPIO 11 / MISO=GPIO 13 / CLK=GPIO 12 / SD_CS=GPIO 10、I2S_IN BCLK=GPIO 4 / WS=GPIO 5 / DIN=GPIO 6、I2S_OUT BCLK=GPIO 15 / LRC=GPIO 16 / DOUT=GPIO 7、DHT22=GPIO 14、Mode_BTN=GPIO 1、Backlight_PWM=GPIO 2）。

- [ ] **T-00-02：實作 I2C Master 匯流排與執行緒安全 Mutex 讀寫**
  - **目標檔案**：`components/board_bsp/board_bsp.c`
  - **實作規格**：
    1. 使用 ESP-IDF v6.1.1 新版 API `i2c_new_master_bus()` 初始化 I2C0（`400 kHz`、內部上拉致能），並建立一把全域 `SemaphoreHandle_t` 互斥鎖保護整條 I2C 匯流排。
    2. 實作 `board_i2c_write()` 與 `board_i2c_read()`：動態取得或快取對應 `dev_addr` 的 `i2c_master_dev_handle_t`，每次傳輸前後必須 `xSemaphoreTake` / `xSemaphoreGive`（逾時 100ms），確保多核心同時存取 OLED、RTC、GY-30 與 EEPROM 時絕不發生匯流排碰撞。

- [ ] **T-00-03：實作 SPI 匯流排、GPIO 按鍵與 LEDC 背光 PWM**
  - **目標檔案**：`components/board_bsp/board_bsp.c`
  - **實作規格**：
    1. 使用 `spi_bus_initialize()` 初始化 `SPI2_HOST`（DMA 自動通道），供後續 SD 卡掛載使用。
    2. 初始化 `GPIO 1` 為輸入模式並開啟內部上拉（`GPIO_PULLUP_ONLY`），實作 `board_btn_is_pressed()`（低電位為按下）。
    3. 初始化 LEDC Timer 與 Channel 輸出 PWM 至 `GPIO 2`（頻率 `5 kHz`、8-bit 解析度 `0~255`），實作 `board_set_backlight(uint8_t duty)`。

- [ ] **T-00-04：撰寫 `board_bsp` 單元測試與開機測試分流**
  - **目標檔案**：`components/board_bsp/test/test_board_bsp.c`、`main/main.c`
  - **實作規格**：
    1. 在 `test_board_bsp.c` 撰寫 Unity 測試：掃描 I2C 匯流排確認至少能偵測到 `0x3C` (OLED)、`0x68` (DS3231)、`0x23` (GY-30)、`0x50` (AT24C256) 之 ACK 回應，並測試 `board_set_backlight()` 正常不崩潰。
    2. 在 `main/main.c` 實作開機檢查：呼叫 `board_bsp_init()` 後延遲 `300 ms`，若 `board_btn_is_pressed()` 仍為 `true`，則呼叫 `unity_run_menu()` 進入測試選單；否則繼續正常啟動流程。執行 `idf.py build` 驗證通過。

---

## Phase 1：PSRAM 環形快取層（`psram_cache`）

- [ ] **T-01-01：實作 512KB PSRAM 環形緩衝區與執行緒安全寫入**
  - **目標檔案**：`components/psram_cache/CMakeLists.txt`、`components/psram_cache/psram_cache.c`
  - **實作規格**：
    1. `CMakeLists.txt` 設定 `REQUIRES freertos esp_psram`。
    2. 使用 `heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 分配 512KB 環形緩衝區（若分配失敗回傳 `ESP_ERR_NO_MEM`）。
    3. 實作 `psram_cache_push()`：以 `SemaphoreHandle_t` 保護頭尾指標，將 `log_record_t` 寫入環形佇列；當緩衝區剩餘空間不足時，覆寫最舊資料並遞增溢出計數器，絕不阻塞呼叫端。

- [ ] **T-01-02：實作批次讀取、確認提交與 `psram_cache` 單元測試**
  - **目標檔案**：`components/psram_cache/psram_cache.c`、`components/psram_cache/test/test_psram_cache.c`
  - **實作規格**：
    1. 實作 `psram_cache_peek_batch()`（只讀取不移動讀指標，支援跨環形陣列尾端折返）與 `psram_cache_commit_pop()`（確認下層寫入成功後才移動讀指標釋放空間）。
    2. 撰寫 `test_psram_cache.c` Unity 測試：驗證指標分配在 PSRAM 位址空間（`esp_ptr_external_ram()` 為真）、環形折返讀寫一致性、以及寫滿覆寫時的邊界安全性。

---

## Phase 2：AT24C256 離線黑盒子（`eeprom_blackbox`）

- [ ] **T-02-01：建立 `eeprom_blackbox` 建置檔與 2-Byte 位址 I2C 轉接層**
  - **目標檔案**：`components/eeprom_blackbox/CMakeLists.txt`、`components/eeprom_blackbox/eeprom_blackbox.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp freertos`。
    2. 實作內部預設 I/O 轉接函式 `default_eeprom_write(uint16_t mem_addr, const uint8_t *data, size_t len)` 與 `default_eeprom_read(...)`：將 16-bit 記憶體偏移量拆為高/低 2 位元組（`[mem_addr >> 8, mem_addr & 0xFF]`），寫入時拼裝 `[addr_hi, addr_lo, data...]` 後呼叫 `board_i2c_write(0x50, ...)`；讀取時先寫入 2-Byte 位址再呼叫 `board_i2c_read(0x50, ...)`。
    3. 支援 `eeprom_blackbox_set_io_hooks()` 注入自訂 Mock 函式，以便無硬體環境下執行單元測試。

- [ ] **T-02-02：實作 64-Byte 頁邊界自動切割寫入與磨損均衡指標**
  - **目標檔案**：`components/eeprom_blackbox/eeprom_blackbox.c`
  - **實作規格**：
    1. **嚴格遵守硬體時序紅線**：任何跨越 `64-Byte` 頁邊界（`mem_addr % 64`）的寫入，必須自動拆分為多筆不跨頁的小寫入，且每筆 I2C 頁寫入完成後**強制呼叫 `vTaskDelay(pdMS_TO_TICKS(6))`**（大於 5ms 寫入週期），絕不允許跨頁回捲覆寫。
    2. 將 `0x0000 ~ 0x003F`（前 64 Bytes）規劃為雙備份 Metadata 區塊（記錄 `write_head`、`read_tail`、`seq_num` 與 CRC16），每次寫入採 Ping-Pong 交替更新，實現磨損均衡並防止斷電損毀指標。
    3. `0x0040 ~ 0x7FFF`（32,704 Bytes）作為環形日誌儲存區，實作 `eeprom_blackbox_append()`、`eeprom_blackbox_read_oldest()` 與 `eeprom_blackbox_pop_oldest()`。

- [ ] **T-02-03：撰寫 `eeprom_blackbox` 跨頁與掉電一致性單元測試**
  - **目標檔案**：`components/eeprom_blackbox/test/test_eeprom_blackbox.c`
  - **實作規格**：
    1. 使用 Mock 32KB 記憶體陣列並加入「頁回捲模擬器」（若單次寫入跨過 `64` 倍數邊界則故意模擬 AT24C256 頁首覆寫錯亂），驗證 `eeprom_blackbox_append()` 在任意偏移位址寫入跨頁資料時皆能 100% 完整讀回。
    2. 驗證寫滿 32KB 環形折返覆寫最舊資料、以及重新呼叫 `eeprom_blackbox_init()` 後能正確從 Metadata 恢復斷電前的讀寫指標。

---

## Phase 3：SD 卡儲存與 WAV/MP3 播放（`sd_media`）

- [ ] **T-03-01：建立 `sd_media` 建置檔與 FATFS 掛載管理**
  - **目標檔案**：`components/sd_media/CMakeLists.txt`、`components/sd_media/sd_media.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp fatfs sdmmc`。
    2. 使用 `esp_vfs_fat_sdspi_mount()` 掛載 SD 卡至 `/sdcard`（使用 `SPI2_HOST`、`SD_CS = GPIO 10`、`max_files = 5`、`allocation_unit_size = 16 * 1024`）。
    3. 若開機時未插卡或掛載失敗，標記 `is_mounted = false` 並回傳錯誤碼，**嚴禁觸發 `ESP_ERROR_CHECK` 導致系統重啟**；實作 `sd_media_is_mounted()` 供降級策略查詢。

- [ ] **T-03-02：實作按日歸檔日誌寫入（`YYYYMMDD.bin`）與 CRC32 校驗**
  - **目標檔案**：`components/sd_media/sd_media.c`
  - **實作規格**：
    1. 實作 `sd_media_append_logs()`：依據 `log_record_t` 的 Unix 時間戳轉換為 UTC+8 日期字串，開啟 `/sdcard/logs/YYYYMMDD.bin`（若目錄不存在則自動 `mkdir`）。
    2. **防掉電資料遺失強制規範**：每筆寫入封裝為 `[Magic: 0xA55A (2B)][Length (2B)][Payload (N B)][CRC32 (4B)]`，開啟檔案一律使用附加模式 `"ab"`，寫入後**強制呼叫 `fflush(fp)` 與 `fsync(fileno(fp))`** 再 `fclose(fp)`。
    3. 實作 `sd_media_read_unsynced()`：掃描目錄下最舊的未同步 `.bin` 檔，逐筆驗證 Magic 與 CRC32（遇損毀封包自動跳過並記錄警告），回傳有效紀錄供補傳模組使用。

- [ ] **T-03-03：整合 `dr_mp3.h` 與 WAV 解析器實現統一 16-bit PCM 串流解碼**
  - **目標檔案**：`components/sd_media/sd_media.c`
  - **實作規格**：
    1. 在 `sd_media.c` 頂部定義 `#define DR_MP3_IMPLEMENTATION` 與 `#define DR_MP3_NO_SIMD` 後 `#include "dr_mp3.h"`（使用 PSRAM 分配解碼器實例，避免佔用內部 SRAM）。
    2. 實作 `sd_media_open_audio()`、`sd_media_read_pcm_frames()` 與 `sd_media_close_audio()`：
       - 若副檔名為 `.wav`：解析 RIFF/fmt/data 標頭，取得取樣率與聲道數，直接讀取 16-bit PCM 樣本。
       - 若副檔名為 `.mp3`：透過 `drmp3_init_file()` 初始化串流解碼器，每次呼叫 `drmp3_read_pcm_frames_s16()` 輸出標準 16-bit signed PCM 樣本，並支援單聲道自動複製為雙聲道交錯格式（L/R 相同），統一輸出規格給 I2S 播放。

- [ ] **T-03-04：撰寫 `sd_media` 日誌寫入與音訊解碼單元測試**
  - **目標檔案**：`components/sd_media/test/test_sd_media.c`
  - **實作規格**：
    1. 測試在無 SD 卡情況下呼叫 `sd_media_append_logs()` 能安全回傳 `ESP_ERR_INVALID_STATE` 且不崩潰。
    2. 若測試環境有插 SD 卡（或掛載 SPIFFS/FatFS 測試分區），驗證寫入 10 筆日誌後讀回之 CRC32 100% 吻合，並測試開啟測試用 `.wav` / `.mp3` 檔能正確取得取樣率與非零 PCM 幀資料。

---

## Phase 4：環境感測與精準校時（`env_sensors`）

- [ ] **T-04-01：實作 DS3231 RTC 讀寫與 GY-30（BH1750）光照感測**
  - **目標檔案**：`components/env_sensors/CMakeLists.txt`、`components/env_sensors/env_sensors.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp esp_timer freertos`。
    2. 實作 DS3231（I2C `0x68`）BCD 碼與 `struct tm` 雙向轉換，提供 `env_sensors_get_rtc_time()` 與 `env_sensors_set_rtc_time()`，系統開機時同步更新 ESP32 內部系統時鐘（`settimeofday()`）。
    3. 實作 GY-30（I2C `0x23`）連續高解析度模式（指令 `0x10`）讀取，轉換為 `float lux` 照度值。

- [ ] **T-04-02：實作 DHT22 微秒級單匯流排溫濕度驅動**
  - **目標檔案**：`components/env_sensors/env_sensors.c`
  - **實作規格**：
    1. **嚴格使用 ESP-IDF v6.1.1 標頭檔 `#include "esp_rom_sys.h"`**（呼叫 `esp_rom_delay_us()`），禁止引用舊版 `ets_sys.h`。
    2. 實作 DHT22（`GPIO 14`）40-bit 時序讀取：發送起始低電位 `1.2 ms` 後切換為上拉輸入，讀取前**使用 `portMUX_TYPE` 進入臨界區（`taskENTER_CRITICAL`）**防止 FreeRTOS 上下文切換干擾微秒脈寬量測，讀取完畢立即退出臨界區（總臨界時間小於 5ms）。
    3. 驗證第 5 位元組校驗和（`(byte0 + byte1 + byte2 + byte3) & 0xFF == byte4`），若讀取失敗則保留上一次有效值並回傳錯誤碼，且限制最小讀取間隔為 `2000 ms`。

- [ ] **T-04-03：實作感測器背景採集任務與 `env_sensors` 單元測試**
  - **目標檔案**：`components/env_sensors/env_sensors.c`、`components/env_sensors/test/test_env_sensors.c`
  - **實作規格**：
    1. 建立背景採集任務 `task_sensor_poll`（**釘選於 Core 1**，優先級 3），每 `2 秒` 更新一次溫濕度與光照快取（受 Mutex 保護），供 `env_sensors_get_latest()` 零阻塞讀取；同時將溫濕度異常或定時紀錄推入 `psram_cache`。
    2. 撰寫 `test_env_sensors.c` Unity 測試：驗證 DS3231 時間設定與讀回一致性、以及溫濕度數值落在物理合理範圍（`-40~80°C`、`0~100% RH`）。

---

## Phase 2：AT24C256 離線黑盒子（`eeprom_blackbox`）

- [ ] **T-02-01：建立 `eeprom_blackbox` 建置檔與 2-Byte 位址 I2C 轉接層**
  - **目標檔案**：`components/eeprom_blackbox/CMakeLists.txt`、`components/eeprom_blackbox/eeprom_blackbox.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp freertos`。
    2. 實作內部預設 I/O 轉接函式 `default_eeprom_write(uint16_t mem_addr, const uint8_t *data, size_t len)` 與 `default_eeprom_read(...)`：將 16-bit 記憶體偏移量拆為高/低 2 位元組（`[mem_addr >> 8, mem_addr & 0xFF]`），寫入時拼裝 `[addr_hi, addr_lo, data...]` 後呼叫 `board_i2c_write(0x50, ...)`；讀取時先寫入 2-Byte 位址再呼叫 `board_i2c_read(0x50, ...)`。
    3. 支援 `eeprom_blackbox_set_io_hooks()` 注入自訂 Mock 函式，以便無硬體環境下執行單元測試。

- [ ] **T-02-02：實作 64-Byte 頁邊界自動切割寫入與磨損均衡指標**
  - **目標檔案**：`components/eeprom_blackbox/eeprom_blackbox.c`
  - **實作規格**：
    1. **嚴格遵守硬體時序紅線**：任何跨越 `64-Byte` 頁邊界（`mem_addr % 64`）的寫入，必須自動拆分為多筆不跨頁的小寫入，且每筆 I2C 頁寫入完成後**強制呼叫 `vTaskDelay(pdMS_TO_TICKS(6))`**（大於 5ms 寫入週期），絕不允許跨頁回捲覆寫。
    2. 將 `0x0000 ~ 0x003F`（前 64 Bytes）規劃為雙備份 Metadata 區塊（記錄 `write_head`、`read_tail`、`seq_num` 與 CRC16），每次寫入採 Ping-Pong 交替更新，實現磨損均衡並防止斷電損毀指標。
    3. `0x0040 ~ 0x7FFF`（32,704 Bytes）作為環形日誌儲存區，實作 `eeprom_blackbox_append()`、`eeprom_blackbox_read_oldest()` 與 `eeprom_blackbox_pop_oldest()`。

- [ ] **T-02-03：撰寫 `eeprom_blackbox` 跨頁與掉電一致性單元測試**
  - **目標檔案**：`components/eeprom_blackbox/test/test_eeprom_blackbox.c`
  - **實作規格**：
    1. 使用 Mock 32KB 記憶體陣列並加入「頁回捲模擬器」（若單次寫入跨過 `64` 倍數邊界則故意模擬 AT24C256 頁首覆寫錯亂），驗證 `eeprom_blackbox_append()` 在任意偏移位址寫入跨頁資料時皆能 100% 完整讀回。
    2. 驗證寫滿 32KB 環形折返覆寫最舊資料、以及重新呼叫 `eeprom_blackbox_init()` 後能正確從 Metadata 恢復斷電前的讀寫指標。

---

## Phase 3：SD 卡儲存與 WAV/MP3 播放（`sd_media`）

- [ ] **T-03-01：建立 `sd_media` 建置檔與 FATFS 掛載管理**
  - **目標檔案**：`components/sd_media/CMakeLists.txt`、`components/sd_media/sd_media.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp fatfs sdmmc`。
    2. 使用 `esp_vfs_fat_sdspi_mount()` 掛載 SD 卡至 `/sdcard`（使用 `SPI2_HOST`、`SD_CS = GPIO 10`、`max_files = 5`、`allocation_unit_size = 16 * 1024`）。
    3. 若開機時未插卡或掛載失敗，標記 `is_mounted = false` 並回傳錯誤碼，**嚴禁觸發 `ESP_ERROR_CHECK` 導致系統重啟**；實作 `sd_media_is_mounted()` 供降級策略查詢。

- [ ] **T-03-02：實作按日歸檔日誌寫入（`YYYYMMDD.bin`）與 CRC32 校驗**
  - **目標檔案**：`components/sd_media/sd_media.c`
  - **實作規格**：
    1. 實作 `sd_media_append_logs()`：依據 `log_record_t` 的 Unix 時間戳轉換為 UTC+8 日期字串，開啟 `/sdcard/logs/YYYYMMDD.bin`（若目錄不存在則自動 `mkdir`）。
    2. **防掉電資料遺失強制規範**：每筆寫入封裝為 `[Magic: 0xA55A (2B)][Length (2B)][Payload (N B)][CRC32 (4B)]`，開啟檔案一律使用附加模式 `"ab"`，寫入後**強制呼叫 `fflush(fp)` 與 `fsync(fileno(fp))`** 再 `fclose(fp)`。
    3. 實作 `sd_media_read_unsynced()`：掃描目錄下最舊的未同步 `.bin` 檔，逐筆驗證 Magic 與 CRC32（遇損毀封包自動跳過並記錄警告），回傳有效紀錄供補傳模組使用。

- [ ] **T-03-03：整合 `dr_mp3.h` 與 WAV 解析器實現統一 16-bit PCM 串流解碼**
  - **目標檔案**：`components/sd_media/sd_media.c`
  - **實作規格**：
    1. 在 `sd_media.c` 頂部定義 `#define DR_MP3_IMPLEMENTATION` 與 `#define DR_MP3_NO_SIMD` 後 `#include "dr_mp3.h"`（使用 PSRAM 分配解碼器實例，避免佔用內部 SRAM）。
    2. 實作 `sd_media_open_audio()`、`sd_media_read_pcm_frames()` 與 `sd_media_close_audio()`：
       - 若副檔名為 `.wav`：解析 RIFF/fmt/data 標頭，取得取樣率與聲道數，直接讀取 16-bit PCM 樣本。
       - 若副檔名為 `.mp3`：透過 `drmp3_init_file()` 初始化串流解碼器，每次呼叫 `drmp3_read_pcm_frames_s16()` 輸出標準 16-bit signed PCM 樣本，並支援單聲道自動複製為雙聲道交錯格式（L/R 相同），統一輸出規格給 I2S 播放。

- [ ] **T-03-04：撰寫 `sd_media` 日誌寫入與音訊解碼單元測試**
  - **目標檔案**：`components/sd_media/test/test_sd_media.c`
  - **實作規格**：
    1. 測試在無 SD 卡情況下呼叫 `sd_media_append_logs()` 能安全回傳 `ESP_ERR_INVALID_STATE` 且不崩潰。
    2. 若測試環境有插 SD 卡（或掛載 SPIFFS/FatFS 測試分區），驗證寫入 10 筆日誌後讀回之 CRC32 100% 吻合，並測試開啟測試用 `.wav` / `.mp3` 檔能正確取得取樣率與非零 PCM 幀資料。

---

## Phase 4：環境感測與精準校時（`env_sensors`）

- [ ] **T-04-01：實作 DS3231 RTC 讀寫與 GY-30（BH1750）光照感測**
  - **目標檔案**：`components/env_sensors/CMakeLists.txt`、`components/env_sensors/env_sensors.c`
  - **實作規格**：
    1. `CMakeLists.txt` 依 `plan.md` 設定：`REQUIRES board_bsp esp_timer freertos`。
    2. 實作 DS3231（I2C `0x68`）BCD 碼與 `struct tm` 雙向轉換，提供 `env_sensors_get_rtc_time()` 與 `env_sensors_set_rtc_time()`，系統開機時同步更新 ESP32 內部系統時鐘（`settimeofday()`）。
    3. 實作 GY-30（I2C `0x23`）連續高解析度模式（指令 `0x10`）讀取，轉換為 `float lux` 照度值。

- [ ] **T-04-02：實作 DHT22 微秒級單匯流排溫濕度驅動**
  - **目標檔案**：`components/env_sensors/env_sensors.c`
  - **實作規格**：
    1. **嚴格使用 ESP-IDF v6.1.1 標頭檔 `#include "esp_rom_sys.h"`**（呼叫 `esp_rom_delay_us()`），禁止引用舊版 `ets_sys.h`。
    2. 實作 DHT22（`GPIO 14`）40-bit 時序讀取：發送起始低電位 `1.2 ms` 後切換為上拉輸入，讀取前**使用 `portMUX_TYPE` 進入臨界區（`taskENTER_CRITICAL`）**防止 FreeRTOS 上下文切換干擾微秒脈寬量測，讀取完畢立即退出臨界區（總臨界時間小於 5ms）。
    3. 驗證第 5 位元組校驗和（`(byte0 + byte1 + byte2 + byte3) & 0xFF == byte4`），若讀取失敗則保留上一次有效值並回傳錯誤碼，且限制最小讀取間隔為 `2000 ms`。

- [ ] **T-04-03：實作感測器背景採集任務與 `env_sensors` 單元測試**
  - **目標檔案**：`components/env_sensors/env_sensors.c`、`components/env_sensors/test/test_env_sensors.c`
  - **實作規格**：
    1. 建立背景採集任務 `task_sensor_poll`（**釘選於 Core 1**，優先級 3），每 `2 秒` 更新一次溫濕度與光照快取（受 Mutex 保護），供 `env_sensors_get_latest()` 零阻塞讀取；同時將溫濕度異常或定時紀錄推入 `psram_cache`。
    2. 撰寫 `test_env_sensors.c` Unity 測試：驗證 DS3231 時間設定與讀回一致性、以及溫濕度數值落在物理合理範圍（`-40~80°C`、`0~100% RH`）。
# 小智 AIoT 專案：架構重構與全功能升級實作任務清單 (`tasks.md`)

> **文件說明與定位**：  
> 本文件為依據最新系統規格書（`架構1124.md`）、全專案代碼審查報告（`SDD_DIFF_REPORT.md`）及架構決策確認書（`aa.md`）所編制之**「鉅細靡遺任務式執行藍圖」**。  
> 包含所有**需求變更、模組升級、架構解耦、效能優化與 Bug 修復**。每個任務均細緻拆分，詳列**目標檔案、函式簽章、具體做法、邊界處理與驗收標準**。後續開發嚴格以本文件為唯一執行基準，完成一項打勾一項（`- [x]`），禁止跨階段偷跑。

---

## 執行前必讀最高紅線（Definition of Done 通用準則）

1. **單檔行數硬性上限**：新增或修改之 `.c`、`.h` 檔案**嚴格限制 $\le 300$ 行**；超出行數必須合理拆分子模組，嚴禁省略實作細節或留下 `TODO` 空殼。
2. **純 ESP-IDF v6.1.1（C11）現代驅動**：全面採用新版驅動 API（`driver/i2c_master.h`、`driver/i2s_std.h`、`esp_timer.h`、`esp_sntp.h`），嚴禁引入 Arduino 庫或舊版 Legacy 棄用 API。
3. **模組解耦與核心隔離**：
   * 音訊引擎（`audio_engine`）與語音辨識釘選 **Core 1**。
   * 網路通訊（`net_manager`）、Web 服務與感測採樣中樞釘選 **Core 0**。
   * 模組間嚴禁循環 `#include`，跨模組狀態共享一律透過 `system_types.h` 快照與 Hook 回調函式傳遞。
4. **記憶體配置分工紅線**：
   * 512KB 環形滑動統計緩衝、音訊預錄緩衝（500ms）與錄音串流緩衝**必須強制配置於 Octal PSRAM（`MALLOC_CAP_SPIRAM`）**。
   * DMA 傳輸描述符與中斷臨界區緩衝必須配置於內部 SRAM（`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`）。
5. **編譯驗收標準**：每個任務完成後必須執行 `idf.py build` 確認零警告/零錯誤編譯通過，並通過 Unity 單元測試覆蓋驗證。

---

## 階段導航與執行里程碑（Milestones）

```
Phase 0：核心資料結構、系統配置與硬體抽象層標準化 (T-00-01 ~ T-00-06)
Phase 1：感測器驅動升級與非同步解耦採樣中樞 (T-01-01 ~ T-01-07)
Phase 2：黑盒子儲存、開機二分搜尋與 SD 卡雙模歸檔 (T-02-01 ~ T-02-07)
Phase 3：音訊引擎升級、PSRAM 預錄緩衝與獨立 WAV 錄音 (T-03-01 ~ T-03-07)
Phase 4：OLED 128x64 全新三段視覺與純指令調光 (T-04-01 ~ T-04-07)
Phase 5：網路安全配網、APSTA 雙模與 17 項設定儀表板 (T-05-01 ~ T-05-06)
Phase 6：斷網最舊優先補傳排空狀態機與整機聯調 (T-06-01 ~ T-06-05)
```

---

## Phase 0：核心資料結構、系統配置與硬體抽象層標準化

- [ ] **T-00-01：定義 32-Byte 頁對齊二進位日誌結構體 (`sensor_record_t`)**
  - **依賴項目**：無（全專案最優先基礎）
  - **目標檔案**：修改 `components/board_config/include/system_types.h`（第 1 部分，$\le 100$ 行）
  - **函式與資料簽章**：
    ```c
    typedef struct __attribute__((packed)) {
        uint32_t timestamp;        // 4B: UNIX 秒級時間戳
        uint16_t seq_id;           // 2B: 單調遞增流水號 (0~65535, 二分搜尋與斷電防呆)
        int16_t  temperature;      // 2B: 溫度 (實際值 x 10, 如 265 代表 26.5°C)
        uint16_t humidity;         // 2B: 相對濕度 (實際值 x 10, 如 550 代表 55.0%)
        uint32_t lux;              // 4B: 光照強度 (lux)
        uint16_t voc_index;        // 2B: SGP41 VOC 指標 (0~500)
        uint16_t nox_index;        // 2B: SGP41 NOx 指標 (0~500)
        uint16_t peak_voc;         // 2B: 該 60 秒視窗內之最高 VOC 峰值
        uint16_t status_flags;     // 2B: 狀態旗標 (感測器在線、網路在線、校時標記)
        uint8_t  reserved[6];      // 6B: 保留欄位 (供未來擴充或對齊填充)
        uint32_t crc32;            // 4B: 整筆資料的 CRC32 校驗碼
    } sensor_record_t;
    ```
  - **具體做法**：
    1. 在 `system_types.h` 定義上述緊湊結構體，並在下方加入靜態編譯斷言：
       `_Static_assert(sizeof(sensor_record_t) == 32, "sensor_record_t must be exactly 32 bytes");`
    2. 定義狀態旗標遮罩：`REC_FLAG_SHT41_VALID (1<<0)`、`REC_FLAG_SGP41_VALID (1<<1)`、`REC_FLAG_GY30_VALID (1<<2)`、`REC_FLAG_RTC_SYNCED (1<<3)`、`REC_FLAG_NET_ONLINE (1<<4)`。
  - **邊界處理**：感測器讀取無效時填入預設非法標記值：無效溫度 `((int16_t)0x7FFF)`、無效濕度 `((uint16_t)0xFFFF)`、無效 VOC/NOx `((uint16_t)0xFFFF)`。
  - **驗收標準**：`idf.py build` 編譯通過，`sizeof(sensor_record_t)` 精確等於 32 Bytes。

- [ ] **T-00-02：定義執行緒安全感測器即時快取結構體 (`sensor_live_cache_t`)**
  - **依賴項目**：`T-00-01`
  - **目標檔案**：修改 `components/board_config/include/system_types.h`（第 2 部分）
  - **函式與資料簽章**：
    ```c
    typedef struct {
        float    current_temp;       // 當前溫度 (°C)
        float    current_humi;       // 當前濕度 (% RH)
        uint32_t current_lux;        // 當前光照 (lux)
        uint16_t current_voc;        // 當前 VOC 指標 (0~500)
        uint16_t current_nox;        // 當前 NOx 指標 (0~500)
        uint16_t window_peak_voc;    // 本歸檔週期內最高 VOC
        uint16_t window_peak_nox;    // 本歸檔週期內最高 NOx
        uint32_t last_update_sht41;  // SHT41 最後更新時間戳
        uint32_t last_update_sgp41;  // SGP41 最後更新時間戳
        uint32_t last_update_gy30;   // GY-30 最後更新時間戳
        uint16_t sensor_status_mask; // 感測器在線遮罩
        SemaphoreHandle_t cache_mutex; // 讀寫保護互斥鎖
    } sensor_live_cache_t;
    ```
  - **具體做法**：
    1. 在 `system_types.h` 宣告此全域共享快取型別，專供 OLED 每秒刷新與 Web 即時讀取。
    2. 宣告快照安全複製輔助函式簽章：`void sensor_cache_copy_snapshot(const sensor_live_cache_t *src, sensor_live_cache_t *dst);`。
  - **邊界處理**：互斥鎖獲取逾時時間設為 50ms；若獲取逾時則回退使用前一次快照，保證 OLED 不掉幀。
  - **驗收標準**：結構體宣告完整，無編譯錯誤。

- [ ] **T-00-03：擴展 17 項系統配置結構體 (`sys_config_t`) 與預設值定義**
  - **依賴項目**：`T-00-01`
  - **目標檔案**：修改 `components/board_config/include/system_types.h`（第 3 部分）
  - **函式與資料簽章**：
    ```c
    typedef enum {
        SD_SYNC_ACTION_DELETE = 0, // 策略 A：同步後刪除
        SD_SYNC_ACTION_KEEP   = 1  // 策略 B：同步後保留備份 (預設)
    } sd_sync_action_t;

    typedef struct __attribute__((packed)) {
        uint32_t magic;                  // 0x585A494F ("XZIO")
        char     wifi_ssid[32];          // Wi-Fi SSID
        char     wifi_pass[64];          // Wi-Fi 密碼
        char     server_url[128];        // 小智伺服器 URL
        char     ntp_server[64];         // NTP 伺服器 (預設 "time.stdtime.gov.tw")
        int8_t   timezone_offset;        // 時區 (預設 +8)
        uint16_t sht41_interval_sec;     // SHT41 週期 (預設 10s)
        uint16_t sgp41_interval_sec;     // SGP41 週期 (預設 1s)
        uint16_t gy30_interval_sec;      // GY-30 週期 (預設 30s)
        uint16_t log_interval_sec;       // 日誌歸檔週期 (預設 60s)
        uint8_t  eeprom_warn_pct;        // EEPROM 滿載門檻 (預設 90)
        uint8_t  sd_sync_action;         // SD 留存策略 (預設 SD_SYNC_ACTION_KEEP)
        uint8_t  system_volume;          // 音量 (0~100%, 預設 70%)
        uint16_t record_max_minutes;     // 錄音單次上限 (預設 10 分鐘)
        uint8_t  auto_dimming_en;        // 自動調光開關 (預設 1)
        uint8_t  cloud_enabled;          // 雲端連線開關 (預設 1)
        uint8_t  reserved[14];           // 保留對齊
        uint32_t crc32;                  // 配置校驗碼
    } sys_config_t;
    ```
  - **具體做法**：
    1. 定義常數巨集 `SYS_CONFIG_DEFAULT_NTP "time.stdtime.gov.tw"`。
    2. 提供預設配置生成函式巨集或宣告：`void sys_config_load_defaults(sys_config_t *cfg);`。
  - **驗收標準**：`sizeof(sys_config_t)` 固定且校驗碼位於結構體尾部。

- [ ] **T-00-04：硬體安全腳位宏全面更新 (`board_config.h`)**
  - **依賴項目**：`T-00-01`
  - **目標檔案**：修改 `components/board_config/include/board_config.h`（$\le 100$ 行）
  - **具體做法**：
    1. 更新 I2S 麥克風腳位：`BOARD_PIN_I2S_MIC_BCLK (4)`、`BOARD_PIN_I2S_MIC_WS (5)`、`BOARD_PIN_I2S_MIC_DIN (6)`。
    2. 更新 I2S 揚聲器腳位：`BOARD_PIN_I2S_SPK_BCLK (15)`、`BOARD_PIN_I2S_SPK_LRC (16)`、`BOARD_PIN_I2S_SPK_DOUT (7)`（徹底刪除 GPIO 38 定義）。
    3. 更新實體按鍵：`BOARD_PIN_MODE_BTN (1)`。
    4. 更新 I2C 匯流排：`BOARD_PIN_I2C_SDA (8)`、`BOARD_PIN_I2C_SCL (9)`。
    5. 更新 SD 卡 SPI：`BOARD_PIN_SD_CS (10)`、`BOARD_PIN_SD_MOSI (11)`、`BOARD_PIN_SD_CLK (12)`、`BOARD_PIN_SD_MISO (13)`。
    6. 新增 I2C 7-bit 位址：`I2C_ADDR_SHT41 (0x44)`、`I2C_ADDR_SGP41 (0x59)`。
  - **驗收標準**：引腳定義無任何重複或衝突，完全避開 ESP32-S3 Strapping 引腳（0, 3, 45, 46）與 Octal PSRAM 引腳（33~37）。

- [ ] **T-00-05：拔除無效背光 PWM 電路與清理 BSP 介面 (`board_bsp.c`)**
  - **依賴項目**：`T-00-04`
  - **目標檔案**：
    - 修改 `components/board_bsp/board_bsp.c`（$\le 200$ 行）
    - 修改 `components/board_bsp/include/board_bsp.h`（$\le 50$ 行）
  - **具體做法**：
    1. 從 `board_bsp.h` 移除 `esp_err_t board_set_backlight(uint8_t duty);` 介面宣告。
    2. 從 `board_bsp.c` 徹底刪除 LEDC 定時器與通道配置代碼（移除原 GPIO 2 相關邏輯）。
    3. 保留 `GPIO 1` 按鍵輸入初始化（`gpio_config_t`，啟用 `GPIO_PULLUP_ONLY`，中斷或電平讀取）。
  - **驗收標準**：編譯通過，專案不再引用任何 LEDC 驅動模組。

- [ ] **T-00-06：Phase 0 單元測試：記憶體對齊、CRC32 與腳位完整性驗證**
  - **依賴項目**：`T-00-01` ~ `T-00-05`
  - **目標檔案**：新增/修改 `components/board_bsp/test/test_board_bsp.c`（$\le 150$ 行）
  - **具體做法**：
    1. 實作 `TEST_CASE("sensor_record_t 32B alignment and CRC32 test")`：宣告實例，填入測試數據，計算 CRC32，竄改任一 Byte 斷言校驗失敗。
    2. 實作 `TEST_CASE("sys_config_t defaults test")`：檢查載入預設值後各欄位符合規格（如 NTP 伺服器名稱、90% 警戒門檻）。
    3. 實作 `TEST_CASE("GPIO 1 button read test")`：驗證 `board_btn_is_pressed()` 正常讀取。
  - **驗收標準**：`idf.py build` 零錯誤，開機測試選單可見此 3 個測試項目且 100% 通過。

---

## Phase 1：感測器驅動升級與非同步解耦採樣中樞

- [ ] **T-01-01：廢棄舊版 DHT22 驅動並清理元件相依**
  - **依賴項目**：`T-00-04`
  - **目標檔案**：
    - 刪除或清空 `components/env_sensors/dht22.c` 及 `dht22.h`
    - 修改 `components/env_sensors/CMakeLists.txt`
  - **具體做法**：
    1. 移除 `esp_rom_delay_us` 單匯流排 bit-banging 讀取代碼。
    2. 從 `CMakeLists.txt` 的 `SRCS` 清單中移除 `dht22.c`。
  - **驗收標準**：編譯無找不到 `dht22.h` 的殘留報錯。

- [ ] **T-01-02：實作 Sensirion SHT41 I2C 溫濕度驅動與 CRC8 校驗 (`sht41.c`)**
  - **依賴項目**：`T-01-01`
  - **目標檔案**：
    - 新增 `components/env_sensors/include/sht41.h`（$\le 70$ 行）
    - 新增 `components/env_sensors/sht41.c`（$\le 220$ 行）
    - 修改 `components/env_sensors/CMakeLists.txt`
  - **函式簽章**：
    ```c
    esp_err_t sht41_init(void);
    esp_err_t sht41_read(float *out_temp, float *out_humi);
    ```
  - **具體做法**：
    1. 發送高精度量測指令 `0xFD`，延遲 10ms。
    2. 透過 `board_i2c_read(0x44, rx_buf, 6)` 讀取 6 Bytes。
    3. 實作 Sensirion CRC8 校驗（多項式 $0x31$，初始值 $0xFF$）：分別校驗溫度與濕度 Word。
    4. 依公式計算：$T = -45.0f + 175.0f \times \frac{raw\_t}{65535.0f}$，$RH = -6.0f + 125.0f \times \frac{raw\_rh}{65535.0f}$。
  - **邊界處理**：將濕度限制在 `0.0f ~ 100.0f` 範圍內；若 I2C 無應答或 CRC 失敗，回傳 `ESP_ERR_INVALID_CRC` 並將輸出設為無效標記。
  - **驗收標準**：連續讀取 50 次成功率 100%，常溫常濕下數值在合理區間。

- [ ] **T-01-03：實作 Sensirion SGP41 VOC/NOx 原始訊號驅動 (`sgp41.c`)**
  - **依賴項目**：`T-01-02`
  - **目標檔案**：
    - 新增 `components/env_sensors/include/sgp41.h`（$\le 80$ 行）
    - 新增 `components/env_sensors/sgp41.c`（$\le 240$ 行）
  - **函式簽章**：
    ```c
    esp_err_t sgp41_init(void);
    esp_err_t sgp41_measure_raw(float temp_c, float humi_rh, uint16_t *out_sraw_voc, uint16_t *out_sraw_nox);
    ```
  - **具體做法**：
    1. 將溫度與相對濕度轉為 SGP41 補償格式：
       - `ticks_humi = (uint16_t)(humi_rh * 65535.0f / 100.0f)`
       - `ticks_temp = (uint16_t)((temp_c + 45.0f) * 65535.0f / 175.0f)`
    2. 組裝指令 `0x2612` 與 2 組 Word+CRC8 參數發送至 I2C 位址 `0x59`。
    3. 等待 50ms 測量轉換，讀取 6 Bytes 返回數據並驗證 CRC8，取出原始 `sraw_voc` 與 `sraw_nox`。
  - **驗收標準**：能穩定傳入當前溫濕度並正確讀回 16-bit 原始訊號。

- [ ] **T-01-04：整合 Sensirion Gas Index 演算法轉換 VOC 與 NOx 指標 (`sgp41_algo.c`)**
  - **依賴項目**：`T-01-03`
  - **目標檔案**：新增 `components/env_sensors/sgp41_algo.c`（$\le 280$ 行）
  - **函式簽章**：
    ```c
    void      sgp41_algo_init(void);
    esp_err_t sgp41_get_indices(float temp_c, float humi_rh, int32_t *out_voc_idx, int32_t *out_nox_idx);
    ```
  - **具體做法**：
    1. 整合輕量定點數 Gas Index Algorithm 狀態機（或封裝 Sensirion 官方 C 模組）。
    2. 每 1 秒以取樣頻率呼叫一次，將原始 SRAW 訊號正規化並輸出：
       - `voc_index`：0 ~ 500（100 代表平均清淨空氣）。
       - `nox_index`：0 ~ 500（1 代表未受污染空氣）。
  - **邊界處理**：開機前 45 秒處於 Conditioning 預熱期，演算法平滑過渡，回傳初始基線值。
  - **驗收標準**：演算法記憶體開銷 $< 2\text{KB}$ RAM，輸出值穩定隨空氣品質變動。

- [ ] **T-01-05：重構 `sensor_hub` 非同步採樣調度中樞 (`sensor_hub.c`)**
  - **依賴項目**：`T-00-02`, `T-01-02`, `T-01-04`
  - **目標檔案**：修改 `components/sensor_hub/sensor_hub.c`（$\le 280$ 行）
  - **函式簽章**：
    ```c
    esp_err_t sensor_hub_init(void);
    esp_err_t sensor_hub_get_live_cache(sensor_live_cache_t *out_cache);
    ```
  - **具體做法**：
    1. 建立獨立 FreeRTOS 任務 `sensor_hub_task`（優先級 5，釘選 Core 0）。
    2. 內部維護非同步計數器：
       - 每 1 秒：讀取 SGP41，更新 `current_voc`、`current_nox`，並若數值大於 `window_peak_voc` / `window_peak_nox` 則即時更新峰值。
       - 每 10 秒：讀取 SHT41，更新 `current_temp`、`current_humi`，並將數值暫存供下一個 1 秒 SGP41 補償使用。
       - 每 30 秒：讀取 GY-30，更新 `current_lux`。
    3. 所有寫入操作受 `cache_mutex` 保護。
  - **驗收標準**：三個感測器互不干擾；互斥鎖鎖定時間每次 $< 1\text{ms}$。

- [ ] **T-01-06：實作 `sensor_hub` 歸檔快照打包器 (`sensor_hub.c`)**
  - **依賴項目**：`T-01-05`
  - **目標檔案**：修改 `components/sensor_hub/sensor_hub.c`（擴充打包函式）
  - **函式簽章**：
    ```c
    esp_err_t sensor_hub_build_record_snapshot(sensor_record_t *out_rec);
    ```
  - **具體做法**：
    1. 獲取 `cache_mutex`，讀取當前各項數據快照。
    2. 填入目前 UNIX 時間戳，遞增內部流水號 `s_seq_id++`。
    3. 填入溫濕光（數值放大 10 倍轉整數）、當前 VOC/NOx 以及視窗峰值 `peak_voc`。
    4. **原子化清空視窗峰值**：將 `window_peak_voc` 與 `window_peak_nox` 重置為當前值，開啟下一個 60 秒歸檔週期。
    5. 調用 `board_calc_crc32()` 計算前 28 Bytes，填入末尾 `crc32` 欄位。
  - **驗收標準**：產出的 `sensor_record_t` 大小為 32 Bytes，CRC32 校驗無誤，峰值重置正確。

- [ ] **T-01-07：Phase 1 感測中樞與溫濕度補償單元測試**
  - **依賴項目**：`T-01-01` ~ `T-01-06`
  - **目標檔案**：新增 `components/sensor_hub/test/test_sensor_hub.c`（$\le 180$ 行）
  - **具體做法**：
    1. 實作 Mock 感測器注入，測試當溫濕度變動時，SGP41 補償計算輸出是否同步改變。
    2. 測試 60 秒週期內若出現瞬間高濃度 VOC，歸檔打包時 `peak_voc` 是否正確捕捉到該最高值。
  - **驗收標準**：單元測試 100% 通過。

---

## Phase 2：黑盒子儲存、開機二分搜尋與 SD 卡雙模歸檔

- [ ] **T-02-01：重構 EEPROM 32KB 全量環形空間與單頁 32B 寫入 (`eeprom_blackbox.c`)**
  - **依賴項目**：`T-00-01`, `T-01-06`
  - **目標檔案**：
    - 修改 `components/eeprom_blackbox/include/eeprom_blackbox.h`（$\le 80$ 行）
    - 修改 `components/eeprom_blackbox/eeprom_blackbox.c`（$\le 250$ 行）
  - **函式簽章**：
    ```c
    esp_err_t eeprom_bb_init(void);
    esp_err_t eeprom_bb_push_record(const sensor_record_t *rec);
    uint16_t  eeprom_bb_get_count(void);
    void      eeprom_bb_reset_pointer(void);
    ```
  - **具體做法**：
    1. 徹底廢止舊版 0x0400 偏移量，起始位址改為 `0x0000`，最大筆數 `EEPROM_MAX_RECORDS = 1024`。
    2. 單筆寫入位址 `addr = write_idx * 32`。因 $32$ 被 $64$（頁大小）整除，每頁精確存放 2 筆，永不觸發晶片跨頁回捲（Roll-over）。
    3. 調用 `board_i2c_write` 寫入 32 Bytes 後延遲 5ms（等待晶片內部硬體寫入週期 $t_{WR}$）。
    4. 遞增 `write_idx = (write_idx + 1) % 1024`。
  - **驗收標準**：單次寫入耗時約 5~6ms，I2C 匯流排鎖定時間最短。

- [ ] **T-02-02：實作 EEPROM 水位追蹤與 90% 門檻告警機制 (`eeprom_blackbox.c`)**
  - **依賴項目**：`T-02-01`
  - **目標檔案**：修改 `components/eeprom_blackbox/eeprom_blackbox.c`
  - **函式簽章**：
    ```c
    bool eeprom_bb_is_warn_threshold_reached(void);
    ```
  - **具體做法**：
    1. 維護有效資料計數 `total_records_count`。
    2. 當 `total_records_count >= (EEPROM_MAX_RECORDS * sys_cfg.eeprom_warn_pct / 100)`（預設 921 筆）時，觸發警告事件回調，通知 OLED 與 Web 顯示 `!90%` 警報。
    3. 當筆數達 1024（100% 滿載）時，觸發強制轉存 SD 卡回調（Flush Callback）。
  - **驗收標準**：筆數達到 921 筆時精確觸發告警，未達門檻不誤報。

- [ ] **T-02-03：實作開機二分搜尋寫入端點演算法 (`eeprom_search.c`)**
  - **依賴項目**：`T-02-01`
  - **目標檔案**：
    - 新增 `components/eeprom_blackbox/eeprom_search.c`（$\le 200$ 行）
    - 修改 `components/eeprom_blackbox/CMakeLists.txt`
  - **函式簽章**：
    ```c
    esp_err_t eeprom_find_write_head(uint16_t *out_write_idx, uint16_t *out_total_count);
    ```
  - **具體做法**：
    1. 實作單筆讀取與校驗：讀取 32 Bytes，檢查是否全為 0xFF（空白頁），並比對 CRC32。
    2. 讀取 Index 0 與 Index 1023：
       - 若 Index 0 無效：晶片全空，`write_idx = 0, count = 0`。
       - 若 Index 0 有效且 Index 1023 無效：尚未寫滿一輪，二分搜尋找出空白端點。
       - 若 Index 0 與 Index 1023 皆有效：已寫滿一輪以上，二分搜尋找出流水號斷層點（`seq_id[mid] > seq_id[mid+1]`），折返點即為最新寫入端點。
    3. 最大迭代次數限制為 $\le 10$ 次，開機耗時 $< 10\text{ms}$。
  - **驗收標準**：全晶片零固定頁磨損，開機瞬間正確恢復寫入索引與歷史總筆數。

- [ ] **T-02-04：實作 EEPROM 斷電損壞記錄修復與平滑容錯**
  - **依賴項目**：`T-02-03`
  - **目標檔案**：修改 `components/eeprom_blackbox/eeprom_search.c`
  - **具體做法**：
    1. 若在二分搜尋時遇到 CRC32 損壞記錄（因上一次剛好在 5ms 寫入中途斷電）：
    2. 自動向前搜尋 1~2 筆尋找最後合法記錄，將寫入指針定位在損壞位置進行覆蓋修復，防止整張表失效。
  - **驗收標準**：人工模擬寫入半截斷電壞軌，重開機自動修復並繼續正常寫入。

- [ ] **T-02-05：實作 SD 卡按日附加歸檔機制 (`sd_media_logger.c`)**
  - **依賴項目**：`T-00-01`
  - **目標檔案**：
    - 新增 `components/sd_media/include/sd_media_logger.h`（$\le 80$ 行）
    - 新增 `components/sd_media/sd_media_logger.c`（$\le 220$ 行）
    - 修改 `components/sd_media/CMakeLists.txt`
  - **函式簽章**：
    ```c
    esp_err_t sd_media_append_records(uint32_t date_yyyymmdd, const sensor_record_t *records, size_t count);
    ```
  - **具體做法**：
    1. 依傳入日期格式化檔名：`/sdcard/logs/%08lu.bin`。
    2. 若目錄 `/sdcard/logs` 不存在，自動呼叫 `f_mkdir()` 建立。
    3. 以 `"ab"`（附加二進位）模式開啟檔案，寫入 `count * 32` Bytes，調用 `fsync()` 落盤。
  - **邊界處理**：若未插 SD 卡或卡已寫滿，立即回傳 `ESP_ERR_NOT_FOUND`，保護資料不遺失並禁止清空 EEPROM。
  - **驗收標準**：多次啟動寫入同一天日誌，檔案尺寸平滑遞增，無舊資料被覆蓋。

- [ ] **T-02-06：實作 SD 卡日誌同步後雙模處置與容量滾動淘汰 (`sd_media_logger.c`)**
  - **依賴項目**：`T-02-05`, `T-00-03`
  - **目標檔案**：修改 `components/sd_media/sd_media_logger.c`
  - **函式簽章**：
    ```c
    esp_err_t sd_media_handle_sync_complete(uint32_t date_yyyymmdd);
    esp_err_t sd_media_purge_old_archives(uint32_t min_free_mb);
    ```
  - **具體做法**：
    1. 實作同步完成處置：
       - 若 `sys_cfg.sd_sync_action == SD_SYNC_ACTION_DELETE`：呼叫 `f_unlink()` 徹底刪除檔案。
       - 若 `sys_cfg.sd_sync_action == SD_SYNC_ACTION_KEEP`（預設）：呼叫 `f_rename()` 將檔案改名為 `/sdcard/logs/%08lu.synced`。
    2. 實作容量守護輪替：當 SD 卡可用空間 $< 100\text{MB}$ 時，掃描 `/sdcard/logs` 目錄，優先刪除日期最舊的 `.synced` 檔案。
  - **驗收標準**：雙模切換行為精確；空間不足時能安全釋放容量。

- [ ] **T-02-07：Phase 2 儲存黑盒子與 SD 卡雙模單元測試**
  - **依賴項目**：`T-02-01` ~ `T-02-06`
  - **目標檔案**：新增 `components/eeprom_blackbox/test/test_eeprom_blackbox.c`（$\le 200$ 行）
  - **具體做法**：
    1. 測試寫入 100 筆資料，驗證二分搜尋快速回傳正確筆數。
    2. 模擬整批導出寫入 SD 卡，並驗證重命名為 `.synced` 或刪除之動作。
  - **驗收標準**：單元測試 100% 通過。

---

## Phase 3：音訊引擎升級、PSRAM 預錄緩衝與獨立 WAV 錄音

- [ ] **T-03-01：標準化 I2S 雙通道配置與 Core 1 排程 (`audio_engine.c`)**
  - **依賴項目**：`T-00-04`
  - **目標檔案**：
    - 修改 `components/audio_engine/include/audio_engine.h`（$\le 100$ 行）
    - 修改 `components/audio_engine/audio_engine.c`（$\le 280$ 行）
  - **函式簽章**：
    ```c
    esp_err_t audio_engine_init(void);
    i2s_chan_handle_t audio_engine_get_rx_chan(void);
    i2s_chan_handle_t audio_engine_get_tx_chan(void);
    ```
  - **具體做法**：
    1. 在 `board_init()` 建立獨立的 I2S RX Channel（`i2s_channel_init_std_mode`，`BCLK=4, WS=5, DIN=6`，立體聲，16kHz 16-bit）。
    2. 建立獨立的 I2S TX Channel（`BCLK=15, LRC=16, DOUT=7`，標準 Philips 模式）。
    3. 音訊處理主任務釘選於 **Core 1**。
  - **驗收標準**：I2S RX 與 TX 獨立時鐘運作，互不阻塞。

- [ ] **T-03-02：實作軟體數位防削波（Soft-Clipping）與 NVS 音量持久化 (`audio_volume.c`)**
  - **依賴項目**：`T-03-01`
  - **目標檔案**：
    - 新增 `components/audio_engine/audio_volume.c`（$\le 180$ 行）
    - 修改 `components/audio_engine/include/audio_engine.h`
  - **函式簽章**：
    ```c
    void      audio_volume_set(uint8_t volume_pct);
    uint8_t   audio_volume_get(void);
    void      audio_volume_apply_soft_clip(int16_t *samples, size_t count);
    ```
  - **具體做法**：
    1. 實作 0% ~ 100% 軟體增益衰減曲線。在音量接近 100% 時，採用雙曲正切或平滑飽和函數限制波峰，防止爆音削波。
    2. 音量調整時即時生效，啟動 1 秒防抖定時器寫入 NVS `"audio"` 分區，開機自動讀取還原。
  - **驗收標準**：滿幅正弦波在 100% 音量下播放無破音；重啟音量保持。

- [ ] **T-03-03：實作 50ms 語音即時打斷機制（Barge-in）與 DMA FIFO 清空**
  - **依賴項目**：`T-03-01`
  - **目標檔案**：修改 `components/audio_engine/audio_engine.c`
  - **函式簽章**：
    ```c
    void audio_engine_trigger_barge_in(void);
    ```
  - **具體做法**：
    1. 當檢測到喚醒詞或短按實體按鍵時，設定原子打斷旗標 `s_barge_in_flag = true`。
    2. 調用 `i2s_channel_disable(tx_chan)` 並立刻清空 DMA FIFO 緩衝，重置播放狀態。
    3. 整個打斷流程耗時嚴格控制在 $< 50\text{ms}$ 內。
  - **驗收標準**：小智長篇播音時，按下按鍵或喊喚醒詞可在 50ms 內瞬間安靜。

- [ ] **T-03-04：實作 PSRAM 500ms 循環滾動預錄緩衝區（Pre-roll RingBuffer）**
  - **依賴項目**：`T-03-01`
  - **目標檔案**：
    - 新增 `components/audio_engine/audio_preroll.c`（$\le 180$ 行）
    - 修改 `components/audio_engine/CMakeLists.txt`
  - **函式簽章**：
    ```c
    esp_err_t audio_preroll_init(uint32_t duration_ms);
    void      audio_preroll_push(const int16_t *samples, size_t count);
    size_t    audio_preroll_extract(int16_t *out_buf, size_t max_count);
    ```
  - **具體做法**：
    1. 在 Octal PSRAM 分配 500ms（16kHz Mono 16-bit = 16,000 Bytes）環形緩衝區。
    2. 待機時持續將麥克風單聲道音訊推入緩衝區，舊資料自動覆蓋。
    3. 喚醒觸發時，一次性將 500ms 音訊導出並拼接在語音辨識流最前段。
  - **驗收標準**：開口第一個字（如「你好」的「你」）100% 完整捕捉不吞字。

- [ ] **T-03-05：實作小智多輪對話 5 秒動態等待視窗與超時狀態機 (`voice_chat.c`)**
  - **依賴項目**：`T-03-04`, `T-03-03`
  - **目標檔案**：
    - 新增/修改 `components/audio_engine/voice_chat.c`（$\le 260$ 行）
  - **函式簽章**：
    ```c
    void voice_chat_on_tts_finished(void);
    void voice_chat_tick_100ms(void);
    ```
  - **具體做法**：
    1. 揚聲器播音結束時，將狀態切換為 `CHAT_STATE_LISTENING_WINDOW`，啟動 5 秒倒數。
    2. 進入該視窗時，將 VAD 能量閾值動態提高 20%（防止環境雜音誤觸發）。
    3. 若 5 秒內有語音輸入，直接進入辨識（無需再次喊喚醒詞）；若 5 秒倒數歸零，靜默切回 `IDLE` 待機。
  - **驗收標準**：5 秒內可流暢追問；無人講話時安靜退回待機，無噪音自激。

- [ ] **T-03-06：實作本地獨立錄音模組（DMA 分塊直寫 SD 卡 WAV）(`local_recorder.c`)**
  - **依賴項目**：`T-02-05`, `T-03-01`
  - **目標檔案**：
    - 新增 `components/audio_engine/include/local_recorder.h`（$\le 80$ 行）
    - 新增 `components/audio_engine/local_recorder.c`（$\le 250$ 行）
  - **函式簽章**：
    ```c
    esp_err_t local_recorder_start(void);
    esp_err_t local_recorder_stop(void);
    bool      local_recorder_is_running(void);
    ```
  - **具體做法**：
    1. 支援離線指令「開始錄音」啟動。
    2. 自動建立 `/sdcard/rec/YYYYMMDD_HHMMSS.wav`，先寫入 44B 虛擬 WAV Header。
    3. 以 4,096 Bytes 為一塊，透過 PSRAM 中繼緩衝串流寫入 SD 卡，Core 1 負載 $< 3\%$。
    4. 支援離線指令「停止錄音」，回寫真實檔案長度並安全關閉檔案。
  - **驗收標準**：連續錄音 10 分鐘，檔案正常播放無跳幀破音；OLED 顯示閃爍 `REC` 圖示。

- [ ] **T-03-07：Phase 3 音訊管線、錄音與打斷單元測試**
  - **依賴項目**：`T-03-01` ~ `T-03-06`
  - **目標檔案**：新增 `components/audio_engine/test/test_audio_engine.c`（$\le 200$ 行）
  - **具體做法**：
    1. 驗證 Barge-in 打斷後 50ms 內 DMA 暫停。
    2. 驗證預錄緩衝寫滿 500ms 後覆寫與提取長度精確無誤。
    3. 驗證獨立錄音生成之 WAV Header 結構相容標準播放器。
  - **驗收標準**：單元測試 100% 通過。

---

## Phase 4：OLED 128x64 全新三段視覺與純指令調光

- [ ] **T-04-01：自建緊湊字模庫：16x24 數字與冒號 (`ssd1306_fonts.c`)**
  - **依賴項目**：無（可平行開發）
  - **目標檔案**：
    - 修改 `components/oled_ui/ssd1306_fonts.c`（第 1 部分，$\le 150$ 行）
    - 修改 `components/oled_ui/include/ssd1306_fonts.h`
  - **函式簽章**：
    ```c
    extern const uint8_t font_16x24_digits[11][48]; // 0~9 與 ':'
    ```
  - **具體做法**：
    1. 製作緊湊 16x24 點陣數字字模（每個字符佔用 $16 \times 24 / 8 = 48\text{ Bytes}$）。
    2. 提供字元繪製介面：`void ssd1306_draw_char_16x24(uint8_t x, uint8_t y, char c);`。
  - **驗收標準**：中央時鐘字體飽滿清晰，無鋸齒毛邊，佔用 Flash 僅約 528 Bytes。

- [ ] **T-04-02：設計與實作小智五態動態圖示（雙訊框 16x16 點陣）**
  - **依賴項目**：`T-04-01`
  - **目標檔案**：修改 `components/oled_ui/ssd1306_fonts.c`（第 2 部分）
  - **函式簽章**：
    ```c
    typedef enum {
        XIAOZHI_ICON_IDLE = 0,
        XIAOZHI_ICON_LISTENING,
        XIAOZHI_ICON_THINKING,
        XIAOZHI_ICON_SPEAKING,
        XIAOZHI_ICON_RECORDING
    } xiaozhi_icon_t;

    void ssd1306_draw_xiaozhi_icon(uint8_t x, uint8_t y, xiaozhi_icon_t icon, uint8_t frame);
    ```
  - **具體做法**：
    1. 為五態各繪製 2 幀 16x16 點陣圖（待機微標、聆聽波形、思考光點、說話聲波、錄音實心圓）。
    2. 提供以每 500ms 切換 `frame = 0` / `1` 的動畫介面。
  - **驗收標準**：動畫切換平滑，生動反應目前小智語音狀態。

- [ ] **T-04-03：實作純軟體 I2C `0x81` 對比度三級平滑自動調光 (`oled_dimmer.c`)**
  - **依賴項目**：`T-00-04`
  - **目標檔案**：
    - 新增 `components/oled_ui/oled_dimmer.c`（$\le 160$ 行）
    - 修改 `components/oled_ui/include/oled_ui.h`
  - **函式簽章**：
    ```c
    void oled_dimmer_update_lux(uint32_t lux);
    ```
  - **具體做法**：
    1. 接收 GY-30 光照度（lux），依三級區間計算目標對比度：
       - `< 20 lux`（暗室）：對比度 `0x10`。
       - `20 ~ 300 lux`（室內）：對比度 `0x7F`。
       - `> 300 lux`（強光）：對比度 `0xFF`。
    2. 引入 3 秒遲滯濾波器（Hysteresis），避免周遭微小光影變化導致頻繁下發 I2C 指令。
    3. 發送 SSD1306 原生指令 `0x81, contrast_val`。
  - **驗收標準**：光線變化時螢幕亮度平滑調節，無可見頻閃或畫面撕裂。

- [ ] **T-04-04：實作 OLED 頂部狀態列渲染（雙軌連線與 !90% 警告）**
  - **依賴項目**：`T-04-01`, `T-02-02`
  - **目標檔案**：修改 `components/oled_ui/oled_ui.c`（第 1 區塊）
  - **具體做法**：
    1. 繪製高度 0~12 區域：
       - 左側：`ONLINE` / `OFFLINE`；網頁模式開啟時追加顯示 `[WEB]`。
       - 右側：顯示未同步筆數（如 `Sync:128`）；當 EEPROM 水位 $\ge 90\%$ 時，閃爍顯示 `!90%` 警告標記。
       - 圖示：Wi-Fi 訊號圖標、SD 卡在位圖標。
  - **驗收標準**：頂部資訊緊湊不擁擠，狀態即時反映。

- [ ] **T-04-05：實作 OLED 中央主視覺區渲染（大時鐘與五態表情）**
  - **依賴項目**：`T-04-01`, `T-04-02`
  - **目標檔案**：修改 `components/oled_ui/oled_ui.c`（第 2 區塊）
  - **具體做法**：
    1. 繪製高度 14~44 區域：
       - 調用 `font_16x24_digits` 渲染高辨識度時鐘 `HH:MM:SS`。
       - 右側調用 `ssd1306_draw_xiaozhi_icon` 繪製當下小智表情微動畫。
       - 獨立錄音期間顯示 `REC 02:45` 閃爍圓點與錄音時長。
  - **驗收標準**：中央時鐘每秒整點跳動，表情動畫流暢。

- [ ] **T-04-06：實作 OLED 底部雙頁定時自動輪播渲染器**
  - **依賴項目**：`T-01-05`
  - **目標檔案**：修改 `components/oled_ui/oled_ui.c`（第 3 區塊）
  - **具體做法**：
    1. 繪製高度 46~63 區域：
    2. 實作軟體定時器：每 3~5 秒自動切換顯示頁面（Page 0 / Page 1）：
       - **Page 0**：顯示環境基礎指標 `26.5°C | 55% | 320Lx`。
       - **Page 1**：顯示空氣品質指標 `VOC:120 | NOx:15 [良]`。
    3. 全面採用顯存局部 Direct Blit，10 FPS 更新率下 CPU 佔用 $< 2\%$。
  - **驗收標準**：底層數據雙頁每 3~5 秒自動交替輪播，空氣品質評級對齊 VOC 指數。

- [ ] **T-04-07：Phase 4 OLED 顯存合成與圖形渲染單元測試**
  - **依賴項目**：`T-04-01` ~ `T-04-06`
  - **目標檔案**：新增 `components/oled_ui/test/test_oled_ui.c`（$\le 160$ 行）
  - **具體做法**：
    1. 離線顯存緩衝區測試：模擬各狀態組合下渲染顯存，斷言顯存無陣列越界。
    2. 驗證雙頁輪播計時器在 3 秒整精確切換頁碼。
  - **驗收標準**：單元測試 100% 通過。

---

## Phase 5：網路安全配網、APSTA 雙模與 17 項設定儀表板

- [ ] **T-05-01：實作免密碼開放熱點 `Xiaozhi-Setup` 與 APSTA 雙模共存 (`net_mode_ctrl.c`)**
  - **依賴項目**：`T-00-03`
  - **目標檔案**：
    - 修改 `components/net_manager/net_mode_ctrl.c`（$\le 260$ 行）
    - 修改 `components/net_manager/include/net_manager.h`
  - **函式簽章**：
    ```c
    esp_err_t net_mode_enter_ap_setup(void);
    esp_err_t net_mode_exit_ap_setup(void);
    bool      net_mode_is_ap_setup_active(void);
    ```
  - **具體做法**：
    1. 開啟 SoftAP 名稱固定為 `Xiaozhi-Setup`，設定為開放無密碼（`WIFI_AUTH_OPEN`）。
    2. 配置 Wi-Fi 模式為 `WIFI_MODE_APSTA`：若 STA 原本已連線，開啟熱點期間不中斷 STA 對外網路與雲端串流。
  - **驗收標準**：手機搜尋無密碼直接連入 `192.168.4.1`，STA 網路同時保持暢通。

- [ ] **T-05-02：實作 SoftAP 安全防護機制（5 分鐘超時自動休眠）(`net_mode_ctrl.c`)**
  - **依賴項目**：`T-05-01`
  - **目標檔案**：修改 `components/net_manager/net_mode_ctrl.c`
  - **具體做法**：
    1. 啟動 5 分鐘軟體計時器 `ap_timeout_timer`。
    2. 每次收到 HTTP 請求時重置計時器；若 5 分鐘內無連線或無 HTTP 操作，自動呼叫 `net_mode_exit_ap_setup()` 關閉熱點退回純 Station 模式。
    3. 網頁端點擊「儲存配置」並套用成功後，立即自動關閉熱點。
  - **驗收標準**：連入後發呆 5 分鐘熱點自動消失；點擊儲存後熱點立即關閉。

- [ ] **T-05-03：擴展 Web 表單支援 17 項系統配置參數與 NVS 持久化 (`web_config.c`)**
  - **依賴項目**：`T-00-03`, `T-05-01`
  - **目標檔案**：修改 `components/net_manager/web_config.c`（$\le 280$ 行）
  - **具體做法**：
    1. 實作 `/save` POST 路由解析 17 項表單欄位（Wi-Fi、Token、URL、NTP、三感測器間隔、歸檔週期、90% 警戒、SD 留存策略、音量等）。
    2. 校驗參數合法性（如採樣間隔不可小於 1s、音量在 0~100%）。
    3. 更新 RAM 配置並寫入 NVS Flash，更新 CRC32。
  - **驗收標準**：所有 17 項參數重開機後完整還原，表單儲存無記憶體洩漏。

- [ ] **T-05-04：實作 Web 端敏感資訊安全遮罩（Wi-Fi 密碼與 Token 遮罩）**
  - **依賴項目**：`T-05-03`
  - **目標檔案**：修改 `components/net_manager/web_config.c`
  - **具體做法**：
    1. 前端請求 `/config` 取得當前設定時，後端強制將 `wifi_pass` 與 `token` 輸出為 `********`。
    2. 提交表單時，若偵測到使用者未更改該欄位（值依然為 `********`），保留原有密碼不覆寫。
  - **驗收標準**：他人連入熱點無法透過原始碼窺探真實 Wi-Fi 密碼與小智 Token。

- [ ] **T-05-05：升級 Web Dashboard 即時數據 API (`web_dashboard.c`)**
  - **依賴項目**：`T-01-05`, `T-05-01`
  - **目標檔案**：
    - 新增 `components/net_manager/web_dashboard.c`（$\le 240$ 行）
    - 修改 `components/net_manager/CMakeLists.txt`
  - **函式簽章**：
    ```c
    esp_err_t register_web_dashboard_routes(httpd_handle_t server);
    ```
  - **具體做法**：
    1. 註冊 `/api/status` GET 路由，輸出 JSON：
       `{"temp":26.5,"humi":55.0,"lux":320,"voc":120,"nox":15,"ip":"192.168.1.100","rssi":-62,"eeprom_pct":15,"sd_free_mb":14200,"sync_pending":0}`。
    2. 內嵌輕量化 HTML/CSS/JS 儀表板，手機瀏覽器每 2 秒輪詢更新。
  - **驗收標準**：手機即時查看儀表板，SGP41 VOC/NOx 與環境指標即時跳動顯示。

- [ ] **T-05-06：Phase 5 網路狀態機、安全超時與 Web API 單元測試**
  - **依賴項目**：`T-05-01` ~ `T-05-05`
  - **目標檔案**：新增 `components/net_manager/test/test_net_manager.c`（$\le 180$ 行）
  - **具體做法**：
    1. 測試 5 分鐘超時軟體計時器觸發關閉回調。
    2. 測試密碼遮罩提交時不被誤覆蓋為星號。
  - **驗收標準**：單元測試 100% 通過。

---

## Phase 6：斷網最舊優先補傳排空狀態機與整機聯調

- [ ] **T-06-01：實作最舊優先（FIFO）三階段雲端排空補傳狀態機 (`sync_pipeline.c`)**
  - **依賴項目**：`T-02-01`, `T-02-06`, `T-05-01`
  - **目標檔案**：
    - 新增 `components/net_manager/sync_pipeline.c`（$\le 280$ 行）
    - 修改 `components/net_manager/CMakeLists.txt`
  - **函式簽章**：
    ```c
    void sync_pipeline_trigger(void);
    bool sync_pipeline_is_busy(void);
    ```
  - **具體做法**：
    1. 當 Wi-Fi 連線恢復且雲端連線開啟時啟動排空任務，**暫時鎖定 PSRAM 即時上傳**。
    2. **第 1 階段（SD 卡排空）**：掃描 SD 卡中日期最舊的 `.bin`，按時間順序逐批上傳雲端；伺服器確認成功後，依設定執行策略 B（改名為 `.synced`）或策略 A（刪除）。
    3. **第 2 階段（EEPROM 排空）**：SD 卡日誌上傳完畢後，讀取 EEPROM 尚未轉存的離線紀錄，逐批上傳；確認接收成功後重置 EEPROM 指針歸零。
    4. **第 3 階段（PSRAM 恢復）**：歷史積壓資料全部確認歸零後，解除鎖定，恢復 PSRAM 最新數據即時直傳。
  - **驗收標準**：斷網期間產生的記錄在恢復連網後依嚴格時間先後順序上傳，雲端時間軸不倒流，補傳完畢 EEPROM 成功歸零。

- [ ] **T-06-02：整合實體按鍵 (GPIO 1) 四重手勢控制器 (`button_controller.c`)**
  - **依賴項目**：`T-00-04`, `T-03-03`, `T-05-01`
  - **目標檔案**：
    - 新增 `main/button_controller.c`（$\le 220$ 行）
    - 新增 `main/include/button_controller.h`
  - **具體做法**：
    1. 實作 50ms 軟體消抖，區分四重手勢：
       - **開機按住 $> 300\text{ms}$**：分流進入 Unity 單元測試選單（`unity_run_menu()`）。
       - **運行時短按（$< 1.5\text{s}$）**：若揚聲器正在播放，優先執行語音打斷（Barge-in）；待機狀態下切換 Wi-Fi 連線模式。
       - **運行時長按（$3\text{s} \sim 10\text{s}$）**：開啟或關閉 `Xiaozhi-Setup` 網頁模式。
       - **運行時超長按（$> 10\text{s}$）**：執行原廠復原（Factory Reset），抹除 NVS 設定並自動重啟。
  - **驗收標準**：按鍵手勢判定精準，長按觸發後釋放不誤觸短按。

- [ ] **T-06-03：整合 NTP 網路校時與 DS3231 RTC 走時回寫校準 (`time_sync.c`)**
  - **依賴項目**：`T-00-03`
  - **目標檔案**：
    - 新增 `components/board_bsp/time_sync.c`（$\le 160$ 行）
    - 修改 `components/board_bsp/include/board_bsp.h`
  - **具體做法**：
    1. 連上 Wi-Fi 後自動啟動 SNTP，同步伺服器預設為 `time.stdtime.gov.tw`（備援 `pool.ntp.org`）。
    2. 校時成功後獲取當前 UNIX 時間戳，將最新時間回寫修正 I2C DS3231 RTC 晶片。
    3. 設定系統時區為 `UTC+8`。
  - **驗收標準**：連網後 5 秒內完成校時並回寫 DS3231；拔掉電源後 DS3231 持續由鈕扣電池精確走時。

- [ ] **T-06-04：系統全模組組裝與主任務入口 (`main/main.c`)**
  - **依賴項目**：Phase 0 ～ Phase 6 所有模組
  - **目標檔案**：修改 `main/main.c`（$\le 250$ 行）
  - **具體做法**：
    1. `app_main()` 依序初始化硬體與服務：
       `board_init()` -> `sensor_hub_start()` -> `audio_engine_start()` -> `oled_ui_start()` -> `net_manager_start()`。
    2. 開機瞬間檢查 GPIO 1 電平：若按住 $> 300\text{ms}$ 則直接引導進入 Unity 測試菜單，否則進入正常業務迴圈。
  - **驗收標準**：編譯零錯誤，系統平穩啟動，無 Watchdog 觸發。

- [ ] **T-06-05：全系統整合壓力驗收與長效穩定性測試**
  - **依賴項目**：`T-06-04`
  - **目標檔案**：專案全系統驗收
  - **具體驗收步驟**：
    1. **拔插電源測試**：系統連續運行中隨機拔除 USB 電源 5 次，重新上電後驗證開機二分搜尋能瞬間定位端點，歷史日誌無損，RTC 走時精準。
    2. **無 SD 卡對話測試**：拔除 SD 卡，向小智提問，驗證雲端 AI 語音回覆在 PSRAM 中正常流轉播放，喇叭播報完全正常。
    3. **長效運行測試**：連續運行 2 小時，驗證 SGP41 1s 採樣、OLED 10 FPS 渲染、Wi-Fi 傳輸並存時，SRAM 最小剩餘 $> 2\text{KB}$，PSRAM 無洩漏。
  - **驗收標準**：全項通過，正式具備交付與量產基準。

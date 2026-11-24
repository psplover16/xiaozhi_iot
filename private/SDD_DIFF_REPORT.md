# Xiaozhi IoT 程式碼與新版 SDD 規格比對差異報告 (`SDD_DIFF_REPORT.md`)

* **比對基準文件**：`架構1124.md` (v1.7.0+ 2026-11-24 新版 SDD)
* **執行規範**：嚴格遵守 `SDD_AUDIT_TASKS.md` 之 4 大最高守則（全自動連續執行、不改動原始碼、逐任務即時寫入、精確定位檔名行號並誠實揭露疑惑）。
* **產出時間**：2026-11-24

---

## Task 1 比對報告：硬體模組、I2C 位址與 GPIO 腳位配置

### 1. 比對現況總覽
| 模組 / 項目 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **主控模組** | ESP32-S3-N16R8（雙核 240MHz、16MB Flash、8MB Octal PSRAM 80MHz） | `sdkconfig.defaults` 設定 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` | **完全符合** |
| **溫濕度感測器** | 廢棄 DHT22（釋出 GPIO 14），改用 I2C0 匯流排 SHT41 | 專案仍保留完整的 DHT22 單匯流排驅動，且完全無 SHT41 實作 | **嚴重不符（待重構）** |
| **空氣品質感測器** | 新增 I2C0 SGP41（偵測 VOC / NOx 指標） | 專案中無任何 SGP41 相關驅動或資料結構 | **完全缺失（待新增）** |
| **螢幕背光調光** | 刪除 GPIO 2 LEDC PWM，改由 SSD1306 內部對比度指令（0x81）調光 | 專案在 `board_bsp.c` 與 `oled_ui.c` 仍大量使用 GPIO 2 LEDC PWM | **嚴重不符（待刪除）** |
| **I2C0 其他裝置** | GY-30 (`0x23`)、DS3231 (`0x68`)、AT24C256 (`0x50`)、SSD1306 (`0x3C`) | `board_bsp.c`、`board_config.h` 已註冊此 4 裝置位址，GY-30 亦支援連續高解析度指令 `0x10` | **符合** |
| **SPI2 SD 卡** | MOSI=11, MISO=13, CLK=12, CS=10，掛載 FATFS `/sdcard` | `board_config.h`, `board_bsp.c`, `sd_media.c` 腳位與掛載路徑一致 | **完全符合** |
| **I2S 音訊與按鍵腳位** | MIC (INMP441): BCLK=4, WS=5, DIN=6<br>SPK (MAX98357A): BCLK=15, LRC=16, DOUT=7<br>Mode 按鍵: GPIO 1 (上拉低電位) | `board_config.h` 與 `audio_engine.c` / `audio_tx_ctrl.c` 腳位定義分裂衝突 | **嚴重衝突（待校準）** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) DHT22 殘留與 SHT41 缺失
* **殘留位置 1**：[`components/board_config/include/board_config.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/board_config.h#L19)
  - 第 19 行：`#define BOARD_PIN_DHT22 4`（佔用 GPIO 4，與 I2S MIC BCLK 產生嚴重衝突）。
* **殘留位置 2**：[`components/env_sensors/env_sensors_drv.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/env_sensors/env_sensors_drv.h#L15-L29)
  - 第 15 行：`#define ENV_DHT22_GPIO_NUM GPIO_NUM_14`。
  - 第 28-29 行：宣告 `env_dht22_init()` 與 `env_dht22_read()`。
* **殘留位置 3**：[`components/env_sensors/env_dht22.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/env_sensors/env_dht22.c#L1-L203)
  - 全檔完整實作了基於 GPIO 14 的 DHT22 單線脈衝微秒讀取與快取機制。
* **殘留位置 4**：[`components/sensor_hub/sensor_hub.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/sensor_hub/sensor_hub.c#L37-L164)
  - 第 37, 61-67, 150-164 行：定時採集任務綁定呼叫 `env_dht22_read()`，並在 `out_rec->status_flags` 設定 `REC_FLAG_DHT22_VALID`。
* **殘留位置 5**：[`components/board_config/include/system_types.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/system_types.h#L32)
  - 第 32 行：`#define REC_FLAG_DHT22_VALID (1U << 0)`。
* **缺失**：專案完全沒有 `SHT41` 相關驅動程式碼（I2C 指令如高精量測 `0xFD`、讀取與 CRC8 校驗等），亦無 SHT41 溫濕度向 SGP41 補償輸入的管道。

#### (2) SGP41 空氣品質感測器完全缺失
* **缺失**：專案全庫搜尋無任何 `SGP41`、`VOC`、`NOx` 關鍵字。未在 I2C0 註冊 SGP41 裝置 Handle，未實作 Sensirion Gas Index 演算法或量測原始資料（raw tick）讀取驅動。

#### (3) 廢棄之 GPIO 2 LEDC PWM 背光調光殘留
* **殘留位置 1**：[`components/board_config/include/board_config.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/board_config.h#L18)
  - 第 18 行：`#define BOARD_PIN_BACKLIGHT_PWM 2`。
  - 第 46 行：宣告 `esp_err_t board_set_backlight(uint8_t duty);`。
* **殘留位置 2**：[`components/board_bsp/board_bsp.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_bsp/board_bsp.c#L20-L242)
  - 第 20 行：`#include "driver/ledc.h"`。
  - 第 191-218 行：初始化 LEDC Timer 0 (5kHz, 8-bit) 與 Channel 0 輸出 PWM 至 `BOARD_PIN_BACKLIGHT_PWM`。
  - 第 235-242 行：實作 `board_set_backlight(uint8_t duty)` 透過 `ledc_set_duty` / `ledc_update_duty` 調光。
* **殘留位置 3**：[`components/oled_ui/oled_ui.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/oled_ui/oled_ui.c#L26-L329)
  - 第 26-28 行：`#define BACKLIGHT_PWM_MIN 25U`, `BACKLIGHT_PWM_MID 200U`, `BACKLIGHT_PWM_MAX 255U`。
  - 第 77-91 行：實作 `compute_pwm_from_ema_lux()` 依據 GY-30 光照度計算 PWM 占空比。
  - 第 284-286, 329 行：呼叫 `board_set_backlight(next_pwm)`。
* **修正需求**：SSD1306 係自發光 OLED，無背光板，應刪除上述所有 LEDC PWM 邏輯，改為依據照度直接呼叫 SSD1306 對比度暫存器指令 `0x81`。

#### (4) I2S 音訊腳位定義嚴重分裂衝突
* **衝突點 1（board_config.h 舊定義）**：[`components/board_config/include/board_config.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/board_config.h#L26-L31)
  ```c
  #define BOARD_PIN_I2S_MIC_WS        15  // 與新版 SDD 不符
  #define BOARD_PIN_I2S_MIC_BCLK      16  // 與新版 SDD 不符
  #define BOARD_PIN_I2S_MIC_DIN       17  // 與新版 SDD 不符
  #define BOARD_PIN_I2S_SPK_LRC       18  // 與新版 SDD 不符
  #define BOARD_PIN_I2S_SPK_BCLK      21  // 與新版 SDD 不符
  #define BOARD_PIN_I2S_SPK_DOUT      38  // 與新版 SDD 不符
  ```
* **衝突點 2（audio_engine 內部覆寫）**：[`components/audio_engine/audio_engine.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/audio_engine/audio_engine.c#L19-L26)
  ```c
  #ifndef BOARD_PIN_I2S_MIC_BCLK
  #define BOARD_PIN_I2S_MIC_BCLK  4   // 符合新版 SDD
  #endif
  #ifndef BOARD_PIN_I2S_MIC_WS
  #define BOARD_PIN_I2S_MIC_WS    5   // 符合新版 SDD
  #endif
  #ifndef BOARD_PIN_I2S_MIC_DIN
  #define BOARD_PIN_I2S_MIC_DIN   6   // 符合新版 SDD
  #endif
  ```
* **衝突點 3（audio_tx_ctrl 內部覆寫）**：[`components/audio_engine/audio_tx_ctrl.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/audio_engine/audio_tx_ctrl.c#L16-L23)
  ```c
  #ifndef BOARD_PIN_I2S_SPK_BCLK
  #define BOARD_PIN_I2S_SPK_BCLK  15  // 符合新版 SDD
  #endif
  #ifndef BOARD_PIN_I2S_SPK_LRC
  #define BOARD_PIN_I2S_SPK_LRC   16  // 符合新版 SDD
  #endif
  #ifndef BOARD_PIN_I2S_SPK_DOUT
  #define BOARD_PIN_I2S_SPK_DOUT  7   // 符合新版 SDD
  #endif
  ```
* **衝突點 4（sd_media.c 使用 board_config.h 舊定義）**：[`components/sd_media/sd_media.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/sd_media/sd_media.c#L121-L123)
  - `sd_media.c` 播音初始化直接使用 `BOARD_PIN_I2S_SPK_BCLK` (21), `BOARD_PIN_I2S_SPK_LRC` (18), `BOARD_PIN_I2S_SPK_DOUT` (38)，導致 `sd_media` 輸出的腳位與 `audio_engine` 的 TX 腳位（15, 16, 7）不一致！

---

### 3. 疑惑與待確認項目
1. **DHT22 原佔用腳位疑慮**：`board_config.h` 中寫 `BOARD_PIN_DHT22 4`，但 GPIO 4 在新版 SDD 是 `I2S_MIC_BCLK`；而 `env_sensors_drv.h` 中寫 `ENV_DHT22_GPIO_NUM GPIO_NUM_14`。目前移除 DHT22 後，GPIO 4 應專屬歸還給 MIC BCLK，GPIO 14 是否作為通用備用 GPIO 或保留浮空？（新版 SDD 明確指出釋出 GPIO 14）。
2. **I2S 腳位統一性**：必須統一將 `board_config.h` 之 MIC 腳位改為 `4, 5, 6`，SPK 腳位改為 `15, 16, 7`，並同步清理 `audio_engine.c`、`audio_tx_ctrl.c` 與 `sd_media.c` 中的私有宏定義。



---

## Task 2 比對報告：資料儲存分工、延壽層級與補傳順序

### 1. 比對現況總覽
| 項目 / 規格 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **系統設定檔存放** | 專存於 ESP32 內部 NVS，嚴禁寫入高頻感測數據 | `eeprom_blackbox.c` 仍將 0x0000~0x03FF 規劃為設定區，每次寫入感測紀錄皆回寫 EEPROM 更新指標；`net_manager.c` 又另存一份 NVS | **嚴重不符（雙重衝突且傷 EEPROM）** |
| **AT24C256 容量釋放** | 徹底移除 EEPROM 設定區，整顆 32KB（約 2,040 筆）全開給感測日誌 | `system_types.h` 仍保留 1KB 設定區，感測區鎖定為 1,984 筆（0x0400 起始） | **未達標（容量未全開）** |
| **儲存優先層級** | 第 1 層 PSRAM ＞ 第 2 層 雲端 ＞ 第 3 層 AT24C256 ＞ 第 4 層 SD 卡 | 流程符合（PSRAM → 雲端 → 離線進 EEPROM → 滿載倒入 SD 卡） | **流程符合，但目錄與參數衝突** |
| **AT24C256 滿載警告** | 預設 90%（網頁可調），OLED 與網頁同時警告，滿載整批倒入 SD 卡（/sdcard/logs/YYYYMMDD.bin）追加不覆蓋 | 程式碼硬寫死為 80% 警告；轉存路徑在 `app_storage_sync.c` 與 `sd_media.c` 命名分裂 | **部分不符** |
| **恢復聯網自動補傳** | ① 先清空 SD 卡 .bin → ② 再清空 AT24C256 → ③ 恢復 PSRAM 即時上傳；雲端關閉時暫停 | `sync_engine.c` 採用抽象註冊機制，順序正確；但受限於 SD 卡路徑分歧與缺漏雲端獨立開關檢查 | **核心邏輯符合，邊界連動待修復** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) 設定檔存放位置衝突與 EEPROM 異常磨損
* **衝突位置 1**：[`components/eeprom_blackbox/eeprom_blackbox.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/eeprom_blackbox/eeprom_blackbox.c#L16-L18)
  - 第 16-18 行：`#define EEPROM_CONFIG_PRIMARY_ADDR (EEPROM_CONFIG_ADDR)` (0x0000)、`#define EEPROM_CONFIG_BACKUP_ADDR (EEPROM_CONFIG_ADDR + 0x0100U)`。
  - 第 410-415 行：在 `eeprom_bb_push_record()` 函式中，**每寫入一筆感測紀錄，就呼叫 `persist_config_locked(&s_ctx.cfg)`**，強制對 EEPROM 的 0x0000 設定頁執行一次寫入以記錄 `record_count++`。這會導致 EEPROM 設定頁在幾天內達到數十萬次寫入極限而損壞！
* **衝突位置 2**：[`components/net_manager/net_manager.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/net_manager.c#L50-L99)
  - `net_manager.c` 內部自行透過 `nvs_set_blob(nvs, NVS_KEY_SYS_CFG, ...)` 與 `nvs_get_blob(...)` 管理 NVS 設定檔。
* **新版 SDD 要求**：
  - `sys_config_t` 必須 100% 移至 ESP32 內部 NVS。
  - 徹底廢除 `eeprom_blackbox.c` 中的 `EEPROM_CONFIG_ADDR`、`eeprom_bb_load_config()`、`eeprom_bb_save_config()` 與每次寫入更新設定區的行為。
  - AT24C256 應改為純日誌黑盒子，其讀寫頭指針應透過頁對齊或環形平均磨損（Ping-Pong / 環形指針）方式管理。

#### (2) AT24C256 儲存容量未全開
* **現有位置**：[`components/board_config/include/system_types.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/system_types.h#L11-L15)
  - 第 13 行：`#define EEPROM_CONFIG_ADDR 0x0000U`
  - 第 14 行：`#define EEPROM_DATA_START_ADDR 0x0400U`（佔用前 1,024 Bytes 給設定檔）
  - 第 15 行：`#define EEPROM_MAX_RECORDS 1984U`（31,744 Bytes ÷ 16 Bytes = 1,984 筆）
* **新版 SDD 要求**：整顆 32KB（32,768 Bytes）全數開放給感測紀錄，容量提升至約 2,040 筆。

#### (3) SD 卡日誌目錄路徑分裂（重大 Bug）
* **路徑定義 1**：[`main/app_storage_sync.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/main/app_storage_sync.c#L23)
  - 第 23 行：`#define SD_OFFLINE_DIR "/sdcard/sensor_offline"`
  - `find_oldest_sd_file_locked()` 與補傳讀取函式全部在 `/sdcard/sensor_offline` 尋找 `.bin` 檔案。
* **路徑定義 2**：[`components/sd_media/sd_media.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/sd_media/sd_media.c#L32)
  - 第 32 行：`#define SD_LOG_DIR "/sdcard/logs"`
  - `sd_media_append_offline_records()` 實際寫入的目標目錄是 `/sdcard/logs/YYYYMMDD.bin`。
* **致命後果**：EEPROM 滿載轉存至 SD 卡時寫入 `/sdcard/logs`，但上傳同步引擎 `app_storage_sync.c` 卻監聽 `/sdcard/sensor_offline`，導致 SD 卡上的離線日誌永遠無法被補傳引擎發現與上傳！
* **新版 SDD 要求**：統一全系統目錄為 `/sdcard/logs/YYYYMMDD.bin`。

#### (4) 滿載警告門檻硬編碼為 80%
* **現有位置**：
  - [`components/board_config/include/system_types.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/system_types.h#L25)：`#define STORAGE_ALERT_EEPROM_80 0x01U`
  - [`components/eeprom_blackbox/eeprom_blackbox.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/eeprom_blackbox/eeprom_blackbox.c#L420-L424)：預設門檻寫死為 80%。
* **新版 SDD 要求**：新版 SDD 將門檻標準調整為預設 **90%**，並允許在網頁設定模式中自由調整。

---

### 3. 疑惑與待確認項目
1. **AT24C256 讀寫指標防斷電丟失機制**：在廢棄 Partition A 設定區後，AT24C256 目前儲存之有效筆數（或頭尾指標）應如何保存？
   - *方案 A*：在 AT24C256 前 64-Byte（0x0000~0x003F）規劃雙備份 Ping-Pong Metadata 頁（僅記錄指標與 CRC，不存完整設定檔），每筆或每批次交替寫入。
   - *方案 B*：開機時由驅動程式自動掃描 32KB 空間中的封包 timestamp 與 CRC16，自動找出最舊與最新邊界（零額外寫入磨損，但開機需耗時數十毫秒讀取）。待後續重構時拍板。
2. **SD 卡空間警告時轉存政策**：`app_storage_eeprom_flush_to_sd()`（第 124-128 行）在 SD 卡剩餘空間 `< 20MB` 時直接拒絕轉存並回傳 `ESP_ERR_NO_MEM`，此時 EEPROM 滿載不淨空，會持續發出警報並停止寫入新數據。此防禦邏輯完全符合新版 SDD 底線保護要求，建議保留。



---

## Task 3 比對報告：小智 AI 語音互動、獨立錄音功能與音訊防破音

### 1. 比對現況總覽
| 項目 / 規格 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **免按鍵喚醒與多輪對話** | 支援離線喚醒（如「你好小智」），一問一答後短暫保持聆聽連續追問 | `sr_engine.c` 支援喚醒詞與離線指令，但缺乏多輪連續對話狀態機（單次一問一答即回到 IDLE） | **部分符合（缺連續追問）** |
| **預錄防漏字緩衝 (Pre-roll)** | 待機在 PSRAM 維持 300~500ms 滾動循環，語音觸發前置銜接 | `audio_engine.c` 雖有 preroll，但大小僅 192ms（3,072 點且放在內部 SRAM） | **未達標（容量偏小且未放 PSRAM）** |
| **雲端 TTS 語音暫存** | 僅在 PSRAM 暫存（用完即清，絕不存檔），上限 5~60 秒（預設 15 秒） | `app_media_voice.c` 硬性寫入 SD 卡 `/sdcard/tts_cache/reply.wav`，無卡直接報錯中止 | **嚴重不符（誤寫 SD 卡）** |
| **隨時打斷插話 (Barge-in)** | 說話、喊停或短按實體按鍵，50ms 內中斷播報 | 語音指令與測試案例支援 50ms 內打斷；但實體按鍵短按僅綁定 Wi-Fi 開關，未優先打斷播音 | **部分符合（按鍵未連動）** |
| **獨立錄音功能 (/sdcard/rec)** | 語音觸發「開始錄音/停止錄音」，以時間命名存於 SD 卡，支援 1~60 分鐘 | 專案中無任何獨立錄音功能、無對應命令詞、無 `/sdcard/rec/` 檔案管線 | **完全缺失（待新增）** |
| **喇叭 5 種播放情境** | ① 雲端 TTS ② 離線提示音 ③ 離線指令回饋 ④ SD 本地音樂 ⑤ SGP41 警示音 | 缺 SGP41 警示音、缺錄音提示音、缺網頁開關回饋音 | **部分缺失** |
| **音量調整與防削波防破音** | 0~100% 存 NVS；軟體數位防削波（Soft-Clipping）避免破音 | 音量僅在 RAM 中縮放且未存 NVS；音量縮放採傳統硬削波（Hard-Clipping），無平滑過載壓縮 | **嚴重不符（缺軟防削波與 NVS）** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) 雲端 TTS 錯誤落盤 SD 卡（重大架構偏離）
* **現有位置**：[`main/app_media_voice.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/main/app_media_voice.c#L148-L184)
  - 第 148-151 行：若 `!sd_media_is_mounted()`，直接報錯「SD 卡未掛載，無法暫存雲端語音」並中斷對話。
  - 第 167-177 行：強制呼叫 `fopen(target_path, "wb")` 將雲端 TTS 二進位音訊寫入 SD 卡 `/sdcard/tts_cache/reply.wav`。
* **新版 SDD 要求**：
  - 雲端 AI 對話語音必須**完全在 PSRAM 中串流快取**，解碼播放完畢立即清空釋放，**嚴禁寫入 SD 卡**！
  - 當設備無插 SD 卡時，依然必須能夠正常進行雲端 AI 對話。

#### (2) 預錄防漏字緩衝（Pre-roll Buffer）時長不足且未放 PSRAM
* **現有位置**：[`components/audio_engine/audio_engine.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/audio_engine/audio_engine.c#L33-L58)
  - 第 33 行：`#define AUDIO_PREROLL_SAMPLES (6U * 512U) /* 192 ms @ 16 kHz 單聲道 */`
  - 第 56 行：`int16_t preroll_buf[AUDIO_PREROLL_SAMPLES];`（配置於內部 SRAM 靜態結構）
* **新版 SDD 要求**：
  - 滾動緩衝區應維持在 **300 ~ 500 ms**（以 16kHz 單聲道計，約需 4,800 ~ 8,000 samples）。
  - 為節省內部 SRAM 空間，緩衝區應配置於 Octal PSRAM。

#### (3) 獨立錄音功能完全缺失
* **缺失項目**：
  1. [`components/audio_engine/include/audio_engine.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/audio_engine/include/audio_engine.h#L14-L28)：缺少 `VOICE_CMD_START_RECORD`（開始錄音）與 `VOICE_CMD_STOP_RECORD`（停止錄音）命令列舉。
  2. [`components/audio_engine/sr_engine.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/audio_engine/sr_engine.c#L45-L65)：未註冊「kai shi lu yin」、「ting zhi lu yin」拼音離線命令。
  3. 未實作將麥克風 PCM 串流即時寫入 `/sdcard/rec/YYYYMMDD_HHMMSS.wav` 的錄音任務與 SD 卡空間守護保護機制。

#### (4) 實體按鍵短按未連動 Barge-in 插話打斷
* **現有位置**：[`components/net_manager/net_mode_ctrl.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/net_mode_ctrl.c#L236-L240)
  - 短按（held_ms >= 50ms 且 < 1.5s）直接執行 `net_manager_set_wifi_enabled(!s_wifi_en)`。
* **新版 SDD 要求**：
  - 運行中「短按 1 下」：**喇叭播放中優先作為立即停止播放鍵（Barge-in）**；待機無音訊播放時才切換 Wi-Fi 連線狀態。

#### (5) 音量控制未持久化且音訊縮放為硬削波（Hard-Clipping）
* **現有位置**：[`components/audio_engine/audio_tx_ctrl.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/audio_engine/audio_tx_ctrl.c#L133-L167)
  - 第 134-140 行：
    ```c
    int32_t scaled = ((int32_t)pcm[i] * (int32_t)g) >> 15;
    if (scaled > 32767) scaled = 32767;
    else if (scaled < -32768) scaled = -32768;
    pcm[i] = (int16_t)scaled;
    ```
    此為直接飽和截斷（Hard-clipping），在音量放大時會造成刺耳破音諧波失真。
  - 第 149-167 行：`audio_engine_set_volume()` 僅改變變數 `s_tx.volume_pct`，未儲存至 NVS，重新開機後自訂音量遺失。
* **新版 SDD 要求**：
  - 應實作軟體數位防削波（Soft-Clipping，如平滑非線性壓縮函數，保留動態並消除方波諧波）。
  - 音量設定值需與 NVS 系統設定參數綁定，並支援斷電記憶。

---

### 3. 疑惑與待確認項目
1. **多輪對話聆聽逾時機制**：新版 SDD 提出一問一答後「短暫保持聆聽支援連續追問」，需確認追問等待視窗秒數（如 5 秒？），若 5 秒內無人聲是否自動播報結束音或靜默返回待機狀態？
2. **錄音格式編碼負載**：獨立錄音規格支援 WAV 或 MP3。WAV 為裸 PCM 打包 RIFF 檔頭，CPU 負載極低；若在 ESP32-S3 上即時將 16kHz PCM 軟體壓縮編碼為 MP3 寫入 SD 卡，需額外評估 Core 1 CPU 佔用率與相應之純 C MP3 編碼庫（如 shine 或 LAME 微型版）。



---

## Task 4 比對報告：環境監測、SGP41 警告、背景統計與 NTP 對時

### 1. 比對現況總覽
| 項目 / 規格 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **感測器採樣週期** | SHT41、SGP41、GY-30 三者各自具備獨立採樣間隔秒數（網頁模式獨立設定） | 程式碼僅有一個全域 `sample_interval_sec`（所有感測器在同一迴圈每 1 秒採集一次） | **嚴重不符（缺獨立週期）** |
| **SGP41 數值儲存** | 每筆紀錄需包含溫濕度補償後之 VOC 指標、NOx 指標、狀態標記與校驗碼 | `sensor_record_t` 僅固定 16 Bytes（溫、濕、光、標記、CRC16），完全無空氣品質欄位 | **嚴重不符（結構缺欄位）** |
| **SGP41 查詢與雙重超標警告** | 支援語音查詢空氣品質；VOC 或 NOx 超標時同時觸發 OLED 警告圖示 ＋ 喇叭警示音 | 無空氣品質語音查詢指令，無超標門檻設定，無警示圖示與警示提示音 | **完全缺失（待新增）** |
| **背景趨勢統計** | PSRAM 歷史視窗內計算溫、濕、光、VOC、NOx 之極值/均值，以及「溫度」與「SGP41」之升降平趨勢 | `env_stats_t` 與 `psram_cache.c` 僅統計溫濕度與光照，且趨勢僅有 `temp_trend`，缺 VOC/NOx 統計與趨勢 | **部分缺失** |
| **UTC+8 與 NTP 對時** | 全系統 UTC+8，連網後透過網頁設定之 NTP 伺服器對時（預設 `time.stdtime.gov.tw`）並回寫 DS3231 | 時區設為 UTC+8（CST-8）符合，但 NTP 伺服器寫死為 `pool.ntp.org` / `time.google.com`，不可自訂 | **部分不符（伺服器寫死）** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) 感測器無獨立採樣週期（共用單一週期）
* **現有位置 1**：[`components/board_config/include/system_types.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/system_types.h#L53)
  - 第 53 行：`sys_config_t` 中僅宣告 `uint16_t sample_interval_sec;`（單一採樣間隔）。
* **現有位置 2**：[`components/sensor_hub/sensor_hub.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/sensor_hub/sensor_hub.c#L90-L110)
  - 第 90-110 行：`task_sensor_poll` 任務中，所有感測器綁死在同一 `SENSOR_POLL_INTERVAL_MS`（1000ms）頻率下執行 `sensor_hub_collect_once()`。
* **新版 SDD 要求**：
  - SHT41（溫濕度）、SGP41（空氣品質）、GY-30（照度）三者必須各自在 `sys_config_t` 擁有獨立的間隔秒數設定。
  - `sensor_hub.c` 應依各自設定之週期獨立排程讀取。

#### (2) `sensor_record_t` 與 `env_stats_t` 缺乏 SGP41 VOC/NOx 欄位
* **現有位置 1**：[`components/board_config/include/system_types.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/system_types.h#L39-L46)
  ```c
  typedef struct __attribute__((packed)) {
      uint32_t timestamp;     // 4B
      int16_t  temperature;   // 2B
      uint16_t humidity;      // 2B
      uint32_t lux;           // 4B
      uint16_t status_flags;  // 2B
      uint16_t crc16;         // 2B
  } sensor_record_t;          // 總長度 16 Bytes
  ```
  完全沒有 SGP41 的 `voc_index`（1~500）與 `nox_index`（1~500）欄位。
* **現有位置 2**：[`components/board_config/include/system_types.h`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/board_config/include/system_types.h#L61-L67)
  ```c
  typedef struct {
      float temp_avg, temp_min, temp_max;
      float humi_avg, humi_min, humi_max;
      uint32_t lux_avg, lux_min, lux_max;
      int8_t temp_trend; // -1:下降, 0:持平, 1:上升
      uint32_t valid_count;
  } env_stats_t;
  ```
  缺少 `voc_avg, voc_min, voc_max, voc_trend` 以及 `nox_avg, nox_min, nox_max, nox_trend`。
* **現有位置 3**：[`components/psram_cache/psram_cache.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/psram_cache/psram_cache.c#L130-L240)
  - `psram_cache_get_stats()` 僅遍歷計算溫濕度與照度，未納入 VOC/NOx 數值累加與升降趨勢判定。

#### (3) NTP 校時伺服器寫死且未提供台灣國家時間伺服器
* **現有位置**：[`components/sensor_hub/sensor_hub.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/sensor_hub/sensor_hub.c#L207-L210)
  ```c
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_init();
  ```
* **新版 SDD 要求**：
  - NTP 伺服器網址必須開放由網頁模式設定並存入 NVS。
  - 出廠預設值與介面預填必須為台灣國家時間與頻率標準實驗室伺服器：`time.stdtime.gov.tw`（第一順位），備援為 `tock.stdtime.gov.tw` 與 `pool.ntp.org`。

---

### 3. 疑惑與待確認項目
1. **`sensor_record_t` 擴充後之二進位長度與 EEPROM 容量影響**：
   - 目前 `sensor_record_t` 固定為 16 Bytes，若加入 `uint16_t voc_index`（2B）與 `uint16_t nox_index`（2B），結構長度將擴展為 **20 Bytes**。
   - AT24C256 總容量為 32,768 Bytes：
     - 若為 16 Bytes：最多存 2,048 筆（整頁 64B 對齊：每頁恰好 4 筆）。
     - 若為 20 Bytes：每頁 64 Bytes 無法整除（64 ÷ 20 = 3 筆，餘 4 Bytes）。跨頁寫入將產生更頻繁的 64-Byte 分割切片，或需將封包補齊為 24 Bytes 或 32 Bytes 對齊以利硬體寫入與頁平均磨損。此資料結構變更需工程確認。
2. **三感測器獨立週期如何打包進統一結構**：
   - 若 SHT41 設為 10 秒、SGP41 設為 5 秒、GY-30 設為 30 秒，何時產生一筆新 `sensor_record_t`？
   - *方案*：以最快採樣者（或固定排程週期）產生封包，其他感測器沿用最新快取讀數，並透過 `status_flags` 標記該次採樣更新之模組。待實作階段拍板。



---

## Task 5 比對報告：OLED 螢幕顯示配置與對比度自動調光

### 1. 比對現況總覽
| 項目 / 規格 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **螢幕基本規格** | 128x64 單色自發光 OLED，刷新率 10 FPS | `OLED_WIDTH=128`, `OLED_HEIGHT=64`, `OLED_UI_FPS=10`，100ms 週期任務 | **完全符合** |
| **頂部狀態列** | 左側：Wi-Fi 在線/離線 ＋ `[網頁模式]` 標記；右側：未同步筆數 ＋ 警告圖示（滿載/SD低空間/SGP超標） | 目前頂部僅顯示單一模式文字標籤與時間，有警告時以大反白佔滿 Page 0~1，無未同步筆數計數與精緻圖示 | **嚴重不符（版面架構不同）** |
| **中央主視覺區** | 待機顯示大字體時間（`HH:MM:SS`）；語音時顯示小智狀態（聆聽/思考/說話）；錄音時顯示錄音時間 | 目前中央僅以 8x8 小字排版 TEMP、HUMI、LUX 三行文字，無大時鐘字型，無小智與錄音狀態機渲染 | **嚴重不符（缺少主視覺）** |
| **底部雙頁輪播** | 每 3~5 秒自動輪播：第 1 頁（溫、濕、光）；第 2 頁（SGP41 VOC、NOx） | 目前底部固定顯示歷史統計文字（T min-max, H min-max），無雙頁切換邏輯，完全無 SGP41 數值 | **完全缺失（待改寫）** |
| **環境光內部調光** | 無外部 PWM，依 GY-30 平滑漸變直接控制 SSD1306 內部對比度（<20 lux 低、20~300 lux 線性、>300 lux 高） | 程式碼中仍殘留外部 LEDC PWM 背光設定；對比度映射範圍為 10~1000 lux，門檻與 SDD 不符 | **部分不符（殘留 PWM 且門檻不符）** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) 版面配置（Layout）與視覺層級完全不符合新版 SDD
* **現有位置**：[`components/oled_ui/oled_ui.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/oled_ui/oled_ui.c#L105-L228)
  - 第 105-151 行 `render_top_section()`：
    - 當有警告時，整面 Page 0~1（16 像素高）被反白文字填滿，覆蓋掉所有狀態指示。
    - 無警告時，第 0 行顯示 `MODE TIME`，第 1 行顯示 `NET: SSID`。
    - **缺漏**：未獨立反映雙軌狀態（在線/離線 ＋ `[網頁模式]` 標記），右側缺少「未同步日誌總筆數」數值，缺少 SGP41 超標警告圖示。
  - 第 153-195 行 `render_middle_section()`：
    - 硬性排版顯示 3 行 8x8 ASCII 文字（TEMP、HUMI、LUX）。
    - **缺漏**：缺少大字體時間數字渲染、缺少小智四態互動圖示／文字（待機中 / 聆聽中 / 思考中 / 說話中）、缺少獨立錄音狀態指示。
  - 第 197-220 行 `render_bottom_section()`：
    - 固定顯示歷史統計數值 `STATS: T:xx-xx A:xx, H:xx-xx%`。
    - **缺漏**：新版 SDD 要求底部為**雙頁自動輪播列（每 3~5 秒自動輪替）**：
      - *第 1 頁*：室內溫度（°C）、相對濕度（% RH）、光照度（lux）。
      - *第 2 頁*：SGP41 空氣品質指標（VOC 指標、NOx 指標）。
    - 目前完全沒有輪播計時器與頁面狀態切換邏輯。

#### (2) 字型庫限制（缺少大時鐘字型）
* **現有位置**：[`components/oled_ui/oled_ssd1306.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/oled_ui/oled_ssd1306.c#L24-L35)
  - 專案內建點陣字型僅包含標準 8x8 ASCII 字型表（`s_font_8x8`）。
  - 若要在中央主視覺區呈現美觀清晰的大字體時間（如 16x16 或 16x24 數字點陣），目前程式庫缺少對應的大字型字模表與多尺寸字串繪製函式。

#### (3) 自動調光仍綁定外部 PWM 且對比度門檻不符
* **現有位置**：[`components/oled_ui/oled_ui.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/oled_ui/oled_ui.c#L93-L103,L284-L286)
  - 第 284-286 行：每次照度更新時，依然執行 `board_set_backlight(next_pwm)` 發送 GPIO 2 LEDC PWM 訊號。
  - 第 93-103 行：對比度換算門檻設定為 `10.0f ~ 1000.0f lux`。
* **新版 SDD 要求**：
  - 徹底移除 `board_set_backlight()` 呼叫。
  - 調光門檻改為符合人眼體感與 SDD 規範之區間：
    - `< 20 lux`：夜間護眼最低對比度。
    - `20 ~ 300 lux`：線性平滑漸變對比度。
    - `> 300 lux`：最高對比度。

---

### 3. 疑惑與待確認項目
1. **中文字型與互動圖示渲染方式**：SSD1306 為 128x64 點陣，新版 SDD 規劃之「待機中/聆聽中/思考中/說話中」與「未同步筆數圖示」，建議以點陣 Bitmap（如 16x16 圖示）或緊湊英文代碼（`IDLE` / `LISTEN` / `THINK` / `SPEAK` / `REC`）呈現，以避免在 16MB Flash 中打包龐大中文字庫。待 UI 規格拍板。
2. **輪播週期秒數決定**：新版 SDD 規定每 3~5 秒輪播底部資訊列，建議直接定為 4 秒（即在 10 FPS 下計數 40 幀切換一次），切換時是否需要平滑捲動（Scroll）特效或直接切頁？



---

## Task 6 比對報告：雙軌獨立狀態、網頁模式（監控＋17 項設定）與按鍵手勢

### 1. 比對現況總覽
| 項目 / 規格 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **雙軌獨立架構** | 狀態一：Wi-Fi 在線/離線；狀態二：網頁模式 ON/OFF；支援連上家用 Wi-Fi 時開啟網頁模式（AP_STA） | `net_mode_ctrl.c` 已有獨立旗標雛形，但 SoftAP 採用純 `WIFI_MODE_AP`，連網與配網切換仍有互斥停用現象 | **部分符合（底層 Wi-Fi 模式待轉 AP_STA）** |
| **熱點名稱與安全性** | 免密碼開放熱點 `Xiaozhi-Setup`，IP 為 `192.168.4.1` 或 `http://xiaozhi.local` | 程式碼中為有密碼熱點 `Xiaozhi_Setup`，密碼 `12345678`（下劃線 vs 連字號，且非免密碼） | **不符（熱點名稱與密碼需修正）** |
| **即時監控儀表板** | 顯示 SHT41（溫濕）、GY-30（光）、SGP41（VOC/NOx）、Wi-Fi/IP/SSID、EEPROM 已用%/筆數/警告、SD 掛載/容量/MB/警告 | `index.html` 僅顯示溫濕光、Wi-Fi、SSID 與簡單字串；缺 SGP41、缺局域網 IP、缺 EEPROM 筆數與 SD 卡容量 MB | **嚴重缺失（資訊不全）** |
| **17 項可調參數** | 涵蓋網路、伺服器開關、NTP、三感測器獨立間隔、視窗時數、儲存警戒、SGP警戒、音量、語音暫存、錄音3項參數 | `index.html` 與 `web_config.c` 僅實作 7 個舊欄位，其餘 10 項參數完全缺失 | **嚴重缺失（缺 10 項設定）** |
| **按鍵手勢（GPIO 1）** | ① 開機 >0.3s 自檢 ② 運行中短按：播音時優先打斷；待機切換 Wi-Fi ③ 長按 3s：開關網頁模式 ④ 長按 10s：原廠復原 | 目前僅有開機分流與 1.5s 長按啟動 AP、短按切換 Wi-Fi，無 3s 開關網頁模式、無 10s 原廠復原、無短按打斷播音 | **嚴重不符（手勢定義脫節）** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) 網頁模式 17 項參數缺漏對照（缺少 10 項）
* **現有位置**：[`components/net_manager/index.html`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/index.html#L97-L130) 及 [`components/net_manager/web_config.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/web_config.c#L143-L170)
* **比對清單**：
  1. `wifi_ssid`：**已有實作**（長度上限 31）。
  2. `wifi_pass`：**已有實作**（長度上限 63）。
  3. `server_url`：**已有實作**（長度上限 127）。
  4. `雲端伺服器連線開關`：在 `index.html` 作為非同步 toggle 開關，未正式整合進表單設定欄位持久化。
  5. `NTP 伺服器網址`：**完全缺失**（後端寫死，前端無 input 欄位，未預填 `time.stdtime.gov.tw`）。
  6. `SHT41 採樣間隔（秒）`：**缺失**（僅有全域 `sample_interval_sec`）。
  7. `SGP41 採樣間隔（秒）`：**完全缺失**。
  8. `GY-30 採樣間隔（秒）`：**完全缺失**。
  9. `psram_window_hours`：**已有實作**（1~24 小時）。
  10. `eeprom_warn_pct`：**已有實作**（但預設值為 80，新版需改為 90%）。
  11. `sd_warn_free_mb`：**已有實作**（50~1000 MB）。
  12. `SGP41 空氣品質警戒門檻（VOC/NOx）`：**完全缺失**。
  13. `喇叭音量大小（0%~100%）`：**缺失**（僅在頂部遙控按鈕觸發微調，表單無設定欄位且未存 NVS）。
  14. `小智對話語音暫存上限（5~60 秒）`：**完全缺失**。
  15. `獨立錄音存放路徑`：**完全缺失**（預設 `/sdcard/rec/`）。
  16. `獨立錄音格式（WAV / MP3）`：**完全缺失**。
  17. `獨立錄音單次上限（1~60 分鐘）`：**完全缺失**。

#### (2) 即時監控儀表板（Dashboard）數據缺漏
* **現有位置**：[`components/net_manager/index.html`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/index.html#L20-L41) 及 [`components/net_manager/web_api.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/web_api.c#L130-L160)
  - `GET /api/status` 回傳之 JSON 缺少：
    - `sgp41_voc`、`sgp41_nox` 即時指標。
    - 設備區域網路 IP 位址（如 `192.168.1.x`），導致使用者在熱點模式下看不到連上路由器後的真實 LAN IP。
    - AT24C256 目前儲存之精確筆數（僅有文字提示）。
    - SD 卡總容量與剩餘空間（單位 MB）數值顯示。

#### (3) 熱點名稱、密碼與連網模式不符
* **現有位置**：[`components/net_manager/net_mode_ctrl.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/net_mode_ctrl.c#L22-L23,L120)
  ```c
  #define SOFTAP_DEFAULT_SSID     "Xiaozhi_Setup"   // 與新版 SDD 不符（底線 vs 連字號）
  #define SOFTAP_DEFAULT_PASS     "12345678"        // 與新版 SDD 不符（新版為免密碼開放熱點）
  ...
  esp_wifi_set_mode(WIFI_MODE_AP);                  // 進入熱點時完全關閉 STA，未採 AP_STA
  ```
* **新版 SDD 要求**：
  - 熱點名稱為 `Xiaozhi-Setup`。
  - 熱點安全性為開放無密碼（免密碼熱點），手機連上自動跳轉彈窗。
  - 當已連線路由器時，若開啟網頁模式，應切換為 `WIFI_MODE_APSTA` 雙模共存，使家用 Wi-Fi 與熱點同時在線。

#### (4) 按鍵手勢時間閾值與行為偏離
* **現有位置**：[`components/net_manager/net_mode_ctrl.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/components/net_manager/net_mode_ctrl.c#L20-L21,L224-L241)
  - 第 21 行：`#define BTN_LONG_PRESS_MS 1500LL`（寫死 1.5 秒）。
  - 短按：直接切換 Wi-Fi 開關，無法打斷音訊播放。
  - 無 3 秒長按邏輯（開關網頁模式）。
  - 無 10 秒長按邏輯（清空 NVS 恢復出廠預設值）。

---

### 3. 疑惑與待確認項目
1. **免密碼熱點連線安全性**：新版 SDD 將熱點設定為無密碼（Open），在家庭環境中方便手機配網；但連上熱點後可直接修改 Wi-Fi 密碼與 Server URL。是否需限制僅在未連上路由器或出廠狀態下開放免密碼熱點，或完全依照 SDD 規範全時開放免密碼？（目前以遵守 SDD 為準）。
2. **網頁模式關閉後的行為**：網頁按下「儲存設定」後自動關閉網頁模式，此時若原本是處於純 SoftAP 狀態，關閉網頁模式是否意味著關閉 SoftAP 射頻轉為嘗試連線路由器 STA？若是 AP_STA 狀態，則僅關閉 AP 射頻並停止 HTTP 伺服器，保留 STA 連線。待邏輯確認。



---

## Task 7 比對報告：硬體異常容錯與單點故障隔離

### 1. 比對現況總覽
| 硬體異常情境 | 新版 SDD 規格標準 | 專案現有程式碼實作狀態 | 符合程度 |
| :--- | :--- | :--- | :---: |
| **開機與周邊單點故障** | 嚴禁對外部周邊讀寫或可熱插拔模組使用觸發整機重啟的 `ESP_ERROR_CHECK` | `main.c` 針對外部元件初始化採用 `ESP_ERROR_CHECK_WITHOUT_ABORT`，不整機重啟 | **符合** |
| **SD 卡未插或運作中拔除** | 離線資料寫入 EEPROM；90% 警告並暫停轉存（不刪資料）；略過提示音；無卡時獨立錄音提示失敗 | 轉存失敗保留 EEPROM 資料符合；但**雲端 TTS 因硬性寫入 SD 卡而在無卡時報錯癱瘓** | **嚴重偏離（TTS 連帶受害）** |
| **SHT41 讀取失敗** | 沿用上次有效值並標記異常旗標，不中斷整體採樣排程 | 舊驅動填入 `INVALID_TEMP_VALUE`，未沿用前次有效值；尚未實作 SHT41 | **待改進** |
| **SGP41 未連接或異常** | 標記為無效並略過超標警報，不影響溫濕度與光照採集 | 專案無 SGP41 實作 | **缺失** |
| **GY-30 未連接或異常** | 照度預設為 `0`，螢幕維持預設安全對比度 | `sensor_hub.c` 填 0，`oled_ui.c` 自動降至最低/安全對比度 | **完全符合** |
| **DS3231 異常或斷電** | 正常開機，連上 Wi-Fi 後透過 NTP 自動校正回寫 | `sensor_hub.c` 降級使用系統時間 `time(NULL)`，聯網後回寫 DS3231 | **完全符合** |
| **AT24C256 異常或離線** | 略過 EEPROM 緩衝層，離線數據直接**降級寫入 SD 卡保存** | `app_storage_on_offline_record()` 失敗時直接放棄，**未降級寫入 SD 卡** | **嚴重不符（離線數據丟失）** |

---

### 2. 詳細差異與缺漏清單（含精確檔案路徑與行號）

#### (1) AT24C256 故障時缺少降級寫入 SD 卡容錯機制
* **現有位置**：[`main/app_storage_sync.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/main/app_storage_sync.c#L85-L96)
  ```c
  esp_err_t app_storage_on_offline_record(const sensor_record_t *rec, void *ctx)
  {
      uint8_t ee_flags = STORAGE_ALERT_NONE;
      esp_err_t err = eeprom_bb_push_record(rec, &ee_flags);
      refresh_alert_flags(ee_flags);
      return err;
  }
  ```
  當 `eeprom_bb_push_record()` 回傳失敗（如 I2C 總線無 ACK、晶片損壞或接觸不良）時，函式直接返回錯誤，離線數據直接被拋棄！
* **新版 SDD 要求**：
  若 AT24C256 未連接或寫入失敗，系統必須**自動略過 EEPROM 緩衝層**，將離線紀錄直接呼叫 `sd_media_append_offline_records()` 附加寫入 SD 卡 `/sdcard/logs/YYYYMMDD.bin` 保存，確保極限環境下離線數據零丟失。

#### (2) SD 卡缺失對前景 AI 業務的致命連鎖反應
* **現有位置**：[`main/app_media_voice.c`](file:///c:/Users/Gary/Documents/xiaozhi_iot/main/app_media_voice.c#L148-L151)
  - 由於雲端 TTS 錯誤地寫入 SD 卡，導致「SD 卡未插」這項次要儲存故障，連帶造成「前景雲端語音對話」完全癱瘓。
* **新版 SDD 要求**：
  - 各模組職責必須徹底解耦隔離（Single Point of Failure Isolation）。SD 卡拔除時，小智雲端對話依然必須在 PSRAM 正常運作。

---

## 總結：修改優先順序與待拍板疑惑清單

本比對清單依據新版規格書（`架構1124.md`）與全專案原始碼實測分析，歸納出**四大重構類別**與**必須修改優先順序總表**：

### 一、 修改優先順序總表（P0 ～ P3）

| 優先級 | 項目編號 | 改造模組與目標 | 核心變更內容與風險說明 |
| :---: | :---: | :--- | :--- |
| **P0** | **MOD-01** | **I2S 腳位與衝突清理**<br>(`board_config.h`, `board_bsp.c`, `sd_media.c`) | 1. 統一將 MIC 腳位改為 `BCLK=4, WS=5, DIN=6`，SPK 腳位改為 `BCLK=15, LRC=16, DOUT=7`。<br>2. 刪除 `board_config.h` 之 GPIO 4 DHT22 與舊音訊腳位。<br>3. 徹底刪除 GPIO 2 LEDC PWM 背光驅動代碼。 |
| **P0** | **MOD-02** | **設定檔專用 NVS 儲存**<br>(`eeprom_blackbox.c`, `system_types.h`) | 1. 徹底廢止 EEPROM 分區 A 設定區（0x0000~0x03FF）及 `record_count` 頻繁寫入，保護 EEPROM 壽命。<br>2. 所有系統設定集中移至 ESP32 NVS（`nvs_flash`）。<br>3. AT24C256 整顆 32KB 全數釋放為感測黑盒子。 |
| **P0** | **MOD-03** | **SD 卡日誌路徑統一與容災修復**<br>(`app_storage_sync.c`, `sd_media.c`) | 1. 修正 `app_storage_sync.c` 路徑為 `/sdcard/logs/YYYYMMDD.bin`，修復補傳引擎找不到日誌的嚴重 Bug。<br>2. 實作 AT24C256 故障時自動降級直接寫入 SD 卡保護機制。 |
| **P1** | **MOD-04** | **溫濕度感測器替換為 SHT41**<br>(`components/env_sensors`, `sensor_hub.c`) | 1. 廢棄 `env_dht22.c`，釋出 GPIO 14。<br>2. 新增 I2C0 `SHT41` 驅動（讀取、CRC8 驗證），失敗時沿用前值容錯。<br>3. 溫濕度讀數同步送入 SGP41 進行環境溫濕度補償。 |
| **P1** | **MOD-05** | **新增 SGP41 空氣品質驅動與雙重超標警告**<br>(`env_sensors`, `sensor_hub.c`, `oled_ui.c`) | 1. 新增 I2C0 `SGP41` 驅動（VOC/NOx 演算法讀取）。<br>2. 擴充 `sensor_record_t` 與 `env_stats_t` 納入 VOC/NOx 數據與趨勢。<br>3. 超標時同時觸發 OLED 警告圖示與喇叭警示提示音。 |
| **P1** | **MOD-06** | **雲端 TTS 快取改為 PSRAM 串流播放**<br>(`app_media_voice.c`, `sd_media.c`) | 1. 雲端對話語音移出 SD 卡，改在 PSRAM 暫存，用完即清空。<br>2. 拔除 SD 卡時，小智雲端對話依然可順暢運作。 |
| **P2** | **MOD-07** | **按鍵手勢（GPIO 1）完整時間狀態機**<br>(`net_mode_ctrl.c`) | 1. 實作「短按優先停止播音（Barge-in），待機切換 Wi-Fi」。<br>2. 實作「長按 3 秒手動開關網頁模式」。<br>3. 實作「長按 10 秒恢復原廠預設值（還原 NVS 並開啟熱點）」。 |
| **P2** | **MOD-08** | **網頁模式 17 項參數與雙模 AP_STA**<br>(`web_config.c`, `web_api.c`, `index.html`) | 1. 熱點改為免密碼 `Xiaozhi-Setup`。<br>2. 支援已連家用 Wi-Fi 下啟動 `WIFI_MODE_APSTA` 雙軌並存。<br>3. 補齊 17 項可調參數（NTP、三獨立週期、視窗時數、門檻、錄音參數等）與 Dashboard 即時監控（含 SGP41 與 LAN IP）。 |
| **P2** | **MOD-09** | **音量 NVS 持久化與數位軟防削波 (Soft-Clipping)**<br>(`audio_tx_ctrl.c`) | 1. 0%~100% 音量儲存至 NVS，支援斷電記憶。<br>2. 音量放大縮放時加入非線性軟防削波曲線，消除破音諧波。 |
| **P3** | **MOD-10** | **新增獨立錄音功能與長時 Pre-roll**<br>(`audio_engine.c`, `app_media_voice.c`) | 1. 擴充離線指令「開始錄音 / 停止錄音」。<br>2. 建立 PCM 串流直寫 `/sdcard/rec/YYYYMMDD_HHMMSS.wav` 錄音任務。<br>3. Pre-roll 緩衝提升至 300~500ms 並配置於 PSRAM。 |
| **P3** | **MOD-11** | **OLED 版面視覺重構與環境光平滑對比度調光**<br>(`oled_ui.c`) | 1. 頂部狀態列整合雙軌狀態、未同步筆數與警報圖示。<br>2. 中央改為大字體時鐘與小智/錄音狀態顯示。<br>3. 底部實現 3~5 秒自動輪播（第 1 頁溫濕光、第 2 頁 VOC/NOx）。<br>4. 對比度映射重設為 <20 lux、20~300 lux、>300 lux。 |

---

### 二、 待拍板架構疑惑與決策清單（請工程負責人確認）

1. **`sensor_record_t` 二進位長度與 EEPROM 頁對齊決策**：
   - *現況*：16 Bytes（剛好整除 AT24C256 64-Byte 頁，每頁 4 筆）。
   - *擴充需求*：納入 `voc_index` (uint16_t, 2B) 與 `nox_index` (uint16_t, 2B) 後為 **20 Bytes**。
   - *方案選擇*：
     - **方案 A（保持緊湊 20 Bytes）**：每頁 64 Bytes 容納 3 筆餘 4 Bytes，跨頁時由驅動層自動切分（每次切分為 4B + 16B 兩段寫入，每次間隔 6ms 燒錄延遲）。
     - **方案 B（填充至 32 Bytes 對齊）**：預留 12 Bytes 擴充欄位（未來可放 CO2、PM2.5 或原始 tick），每頁剛好 2 筆，寫入時完全無跨頁拆片問題，總筆數降為 1,024 筆。
2. **多感測器獨立採樣週期如何封裝為單一歷史日誌**：
   - *情境*：若 SHT41 設為 10 秒、SGP41 設為 5 秒、GY-30 設為 30 秒。
   - *建議做法*：以「最頻繁採樣之間隔」（如本例的 5 秒）作為封包產出心跳，其餘感測器沿用 RAM 最新有效讀數，並透過 `status_flags` 標記該筆紀錄當下更新之感測項目。
3. **免密碼熱點連線資安考量**：
   - 新版 SDD 明確指定 `Xiaozhi-Setup` 為**免密碼開放熱點**。在配網完畢連上家用 Wi-Fi 後，網頁模式在「儲存設定」後即自動關閉 SoftAP，此舉已大幅降低未授權入侵風險。確認直接遵照此規範執行。
4. **獨立錄音音訊編碼格式**：
   - 規格書標註支援 WAV / MP3。因 ESP32-S3 軟體即時壓縮 MP3 較耗 CPU，建議預設採用 **16kHz 16-bit PCM WAV** 直接寫入 SD 卡，MP3 格式可作為次階段擴充選項。


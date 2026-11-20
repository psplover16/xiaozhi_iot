# Project Specification: 小智 IoT 語音機器人 (`xiaozhi_iot`)

* **文件版本**：v1.9.1（ESP-IDF v6.1.1 + Weak-AI Compiler & Timing Hardened SDD/TDD Baseline + Phase 8 離線聲控完整擴充）
* **當前專案狀態**：已完成前置骨架與 Phase 0 ～ Phase 5（`board_bsp`、`psram_cache`、`eeprom_blackbox`、`sd_media`、`env_sensors`、`sensor_hub`、`oled_ui` 及對應 Unity 單元測試）；後續將依序推進 **Phase 6（`audio_engine`）**、**Phase 7（`net_manager` / `main`）** 與 **Phase 8（`ESP-SR` 離線語音喚醒與命令詞擴充）**。
* **開發方法論**：SDD（規格驅動開發）＋ TDD（測試驅動開發，採用 ESP-IDF 內建 Unity 框架）。
* **閱讀對象**：開發者與 VS Code AI 程式輔助模型（**必須逐字遵守本文件所有規範、腳位、元件名稱與資料結構，嚴禁擅自變更架構、簡化條件或省略防呆機制**）。

---

## 0. 當前專案實作進度鎖定表（AI 必讀狀態邊界）

| 階段 | 對應元件目錄 | 當前狀態 | 對弱 AI 模型的強制操作邊界 |
| :--- | :--- | :--- | :--- |
| **Pre & Phase 0** | `board_bsp` | **已完成** | **鎖定唯讀**：嚴禁修改腳位定義與匯流排初始化；直接 `#include "board_bsp.h"` 呼叫既有 API。 |
| **Phase 1** | `psram_cache` | **已完成** | **鎖定唯讀**：直接 `#include "psram_cache.h"` 呼叫既有 API。 |
| **Phase 2** | `eeprom_blackbox` | **已完成** | **鎖定唯讀**：直接 `#include "eeprom_blackbox.h"` 呼叫既有 API。 |
| **Phase 3** | `sd_media` | **已完成** | **鎖定唯讀**：直接 `#include "sd_media.h"` 呼叫既有 API。 |
| **Phase 4** | `env_sensors` + `sensor_hub` | **已完成** | **鎖定唯讀**：直接 `#include "sensor_hub.h"` 呼叫既有 API。 |
| **Phase 5** | `oled_ui` | **已完成** | **鎖定唯讀**：直接 `#include "oled_ui.h"` 呼叫既有 API。 |
| **Phase 6** | `audio_engine`（基礎收放音與 VAD） | **待實作** | 依 `tasks.md` `T-06-01` ～ `T-06-03` 單任務推進，並預留 Phase 8 PCM 幀分發介面。 |
| **Phase 7** | `net_manager` + `main/main.c` | **待實作** | 依 `tasks.md` `T-07-01` ～ `T-07-04` 單任務推進，完成雙核心總組裝。 |
| **Phase 8** | `audio_engine`（`ESP-SR` 離線聲控） | **待實作** | **必須在 Phase 7 完成後**，依 `tasks.md` `T-08-01` ～ `T-08-03` 執行，不新增任何硬體。 |

---

## 1. 開發基線與嚴格禁止事項（Baseline & Hard Prohibitions）

### 1.1 既定專案環境基線（不可破壞）
1. **開發框架**：純 **ESP-IDF `v6.1.1`** 原生 C 語言架構（C11 標準）。
2. **目標晶片**：`esp32s3`（實體型號為 **`ESP32-S3-N16-R8`**，雙核 240MHz、16MB Flash、8MB Octal PSRAM）。
3. **已鎖定之 `sdkconfig` 與分區設定**：
   * `Flash size` = `16 MB`（`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`）
   * `SPI RAM` = Enabled，且模式為 **`Octal Mode PSRAM`**（`CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_MODE_OCT=y`、`CONFIG_SPIRAM_SPEED_80M=y`）
   * `CPU frequency` = `240 MHz`、`CONFIG_FREERTOS_HZ=1000`
   * **自訂分區表（`partitions.csv`）**：必須啟用 `CONFIG_PARTITION_TABLE_CUSTOM=y`，利用 16MB Flash 配置至少 **`4 MB`（`0x400000`）給 `factory` 主程式分區**，防止加入 Wi-Fi 與網頁伺服器後發生 `app partition is too small` 連結錯誤；並於 Phase 8 在 `0x410000` 配置 **`3 MB`（`0x300000`）給 `model` 離線語音模型分區**（存放 WakeNet 與 MultiNet 權重）。

### 1.2 給 AI 模型的 12 條絕對紅線（違反即視為錯誤實作）
1. **嚴禁混用 Arduino 與已移除之 ESP-IDF v4/v5.0 舊版驅動**：
   * 禁止出現 `#include <Arduino.h>`、`Serial`、`Wire`、`SPI`、`pinMode`、`digitalWrite`、`analogWrite`、`delay`、`millis()`。
   * **針對 ESP-IDF v6.1.1 特別限制**：舊版 `driver/i2c.h`（`i2c_driver_install`）、`driver/i2s.h`（`i2s_driver_install`）、`ets_sys.h` 與 `sntp.h` 在 v6.x 已移除，**絕對禁止使用**。必須一律使用 v6.x 新版驅動 API：`driver/i2c_master.h`、`driver/i2s_std.h`、`driver/spi_master.h`、`driver/gpio.h`、`driver/ledc.h`、`esp_rom_sys.h`、`esp_sntp.h` 與 `esp_log.h`。
2. **單向模組依賴樹、真實元件命名與共用型別集中管理**：
   * 本專案真實存在的 9 個元件目錄名稱為：`board_bsp`、`psram_cache`、`eeprom_blackbox`、`sd_media`、`env_sensors`、`sensor_hub`、`oled_ui`、`audio_engine`、`net_manager`。**絕對禁止**捏造不存在的元件名稱（如 `board_config`、`display_driver`、`wifi_manager`）。
   * 所有跨模組共用資料結構（`sensor_record_t`、`sys_config_t`、儲存警告旗標、網路模式列舉）集中定義於基礎層標頭檔。
   * 依賴方向嚴格單向：上層業務元件皆可依賴 `board_bsp`，**`board_bsp` 嚴禁依賴或 `#include` 任何上層元件**，同層元件之間嚴禁互相 `#include`，杜絕標頭檔循環依賴。
3. **匯流排底層初始化與 I2C 代理嚴格隔離**：
   * `driver/i2c_master.h` 的 Bus/Device Handle 僅限封裝於 `board_bsp` 內部，上層模組（`oled_ui`、`env_sensors`、`sensor_hub`、`eeprom_blackbox`）一律透過 `board_bsp` 提供的執行緒安全 I2C 讀寫代理函式存取（7-bit 位址 + 原始位元組緩衝區）。
   * `SPI2_HOST` 匯流排與 `LEDC` 背光 PWM 僅在 `board_bsp` 執行一次 `spi_bus_initialize()` 與 PWM 初始化，`sd_media` 僅呼叫 `esp_vfs_fat_sdspi_mount()` 掛載 FAT32，**嚴禁重複初始化 SPI 匯流排**。
4. **嚴禁使用內部保留腳位與硬寫死腳位數字**：
   * **GPIO 26 ～ 32**（內部 SPI Flash）、**GPIO 33 ～ 37**（內部 Octal PSRAM）、**GPIO 19、20**（原生 USB）、**GPIO 43、44**（UART0 Console 日誌與 Unity 測試通訊）與 **GPIO 0、3、45、46**（Strapping 啟動腳位）**絕對禁止作為一般腳位使用**。
   * 所有 `.c` 原始碼檔**嚴禁出現硬寫死的腳位數字**，一律使用第 2 節鎖定並定義於基礎層標頭檔的腳位巨集。
5. **大記憶體配置強制使用 PSRAM**：凡超過 `2 KB` 的緩衝區（音訊 DMA/錄音緩衝、`drmp3` 解碼器實例結構、PSRAM 統計環形陣列、`ESP-AFE` 與 WakeNet/MultiNet 推論緩衝、HTTP 網頁與表單解析緩衝、SD 批量讀寫緩衝），**必須**使用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` 分配，嚴禁宣告為大型全域靜態陣列擠爆 512KB 內部 SRAM。
6. **共享匯流排與 SD 檔案操作強制加鎖**：存取 I2C 裝置必須受 `xI2cBusMutex` 保護；存取 `/sdcard` 檔案系統（包含音訊讀取、離線語音播報與離線紀錄寫入）必須取得 `xSpiSdMutex`，且音訊播放每次讀取區塊不得超過 `4096 Bytes`，區塊間須釋放鎖以確保寫入不被阻塞。
7. **嚴禁覆寫 SD 卡離線感測檔**：寫入 `/sdcard/logs/` 或 `/sdcard/sensor_offline/` 時一律使用 `"ab"`（二進位檔尾附加）模式，絕對禁止使用 `"wb"` 覆寫舊檔。
8. **嚴禁淨空 EEPROM 系統設定／Metadata 區**：清理 `AT24C256` 離線感測資料時，只允許抹除資料區段，絕對禁止動到前 `64 Bytes`（`0x0000 ~ 0x003F`）Metadata 區或系統設定區。
9. **`dr_mp3.h` 單標頭檔實作巨集唯一性規則**：
   * `components/sd_media/include/dr_mp3.h` 為單標頭檔純 C 庫，**僅允許在 `components/sd_media/sd_media.c` 頂部（`#include "dr_mp3.h"` 之前）定義一次 `#define DR_MP3_IMPLEMENTATION`**，嚴禁在其他任何 `.h` 或 `.c` 檔中重複定義。直接透過 ESP-IDF VFS 路徑呼叫 `drmp3_init_file()`，不需撰寫自訂檔案讀取回呼。
10. **系統底層服務單次初始化與任務安全堆疊**：
    * `nvs_flash_init()`、`esp_netif_init()` 與 `esp_event_loop_create_default()` 僅在 `main/main.c` 開機時執行一次，切換網路模式（0/1/2）時**嚴禁重複呼叫**。
    * 凡執行 FatFS 讀寫、MP3 解碼、I2S 音訊、`ESP-SR` 模型推論或 HTTP Server 的 FreeRTOS 任務，堆疊大小（Stack Size）**不得低於 `6144 Bytes`**（HTTP Server 設為 `8192 Bytes`），且迴圈內必須包含阻塞等待或 `vTaskDelay()`，防止看門狗（Watchdog）重啟。
11. **ESP-IDF v6.1.1 明確元件依賴聲明**：
    * 各元件 `CMakeLists.txt` 必須明確宣告 v6.x 細粒度依賴名稱（依實際使用勾選）：`esp_driver_i2c`、`esp_driver_i2s`、`esp_driver_spi`、`esp_driver_gpio`、`esp_driver_ledc`、`esp_psram`、`fatfs`、`sdmmc`、`esp_http_server`、`esp_http_client`、`esp_wifi`、`esp_netif`、`esp_event`、`nvs_flash`、`lwip`、`unity`（Phase 8 另加 `espressif__esp-sr`）。
12. **強制執行 TDD 先測後寫（Red $\rightarrow$ Green）與程式碼完整性**：
    * 每個模組須建立 `test/` 子目錄並使用 `#include "unity.h"` 與 `TEST_CASE("測試描述", "[模組標籤]")` 巨集撰寫單元測試，**絕對禁止在任何 `test/` 檔案中定義 `app_main()`**。
    * 單一 `.c` 原始碼檔案建議控制在 **300 行以內**；AI 生成程式碼時**嚴禁使用 `// ... 其餘略` 或 `// TODO` 偷懶省略任何實作細節**。

---

## 2. 硬體規格、唯一鎖定腳位配置與實體電氣邊界（Hardware Specifications）

實體硬體採用洞洞板（Perfboard）搭配裸銅線手工佈線，以下為 `board_bsp`（`T-00-01` ～ `T-00-03`）已鎖定並實機驗證通過之**唯一合法 GPIO 腳位配置**，軟體驅動必須嚴格配合：

| 類別 | 元件型號 | 通訊介面 / 7-bit 位址 | 唯一鎖定實體 GPIO（定義於 `board_bsp`） | 硬體規格與軟體對應約束 |
| :--- | :--- | :--- | :--- | :--- |
| **主控 MCU** | `ESP32-S3-N16-R8` | Xtensa LX7 雙核心 | 避開 `0,3,19,20,26-37,43-46` | Core 0 專責網路與同步；Core 1 專責音訊、離線語音識別、感測、儲存與螢幕。 |
| **語音輸入** | 2x `INMP441` 數位麥克風 | `I2S_NUM_0`（Rx，雙聲道） | `BCLK=4`, `WS=5`, `DIN=6` | 線性雙麥陣列，中心間距 **65 mm**，硬體 L/R 分離（L 接 GND、R 接 3.3V），支援 3～5 公尺遠場拾音與 `ESP-AFE` 雙麥降噪。 |
| **語音輸出** | `MAX98357A` + 3W 8Ω 喇叭 | `I2S_NUM_1`（Tx，雙聲道） | `BCLK=15`, `LRC=16`, `DOUT=7` | 喇叭與主洞洞板機械懸空隔離；支援 **16-bit PCM WAV** 與 **MP3（`dr_mp3.h`）** 統一 PCM 輸出及動態取樣率重配置。 |
| **顯示螢幕** | `0.96" OLED`（SSD1306） | `I2C0`（位址 **`0x3C`**） | `SDA=8`, `SCL=9`（共用 I2C） | 背面電阻焊於 `0x78`（7-bit `0x3C`）；自建 1024B 幀緩衝透過 `board_bsp` 代理刷屏。 |
| **螢幕背光** | OLED / 狀態背光控制 | `LEDC PWM`（5 kHz, 8-bit） | `BACKLIGHT_PWM=2` | 依 `GY-30` 量測照度透過 EMA 濾波自動調節 PWM（`25` ～ `255`）。 |
| **光照感測** | `GY-30`（BH1750） | `I2C0`（位址 **`0x23`**） | `SDA=8`, `SCL=9`（共用 I2C） | 量測環境照度（單位：lux），供 OLED 自動調光及寫入感測紀錄。 |
| **即時時鐘** | `DS3231`（ChronoDot V2.0） | `I2C0`（位址 **`0x68`**） | `SDA=8`, `SCL=9`（共用 I2C） | 內建鈕扣電池維持精準 Unix 時間戳；聯網時透過 NTP 自動校準。 |
| **離線快閃** | `AT24C256` EEPROM | `I2C0`（位址 **`0x50`**） | `SDA=8`, `SCL=9`（共用 I2C） | 32,768 Bytes（**64-Byte Page**）；每寫完一頁須等待 **`6 ms`** 內部燒錄週期。 |
| **溫濕度** | `DHT22`（AM2302） | 1-Wire 單線數位開漏 | `DATA=14` | `18 ms` 啟動訊號用 `vTaskDelay(20ms)`，**僅約 `4.5 ms` 資料接收窗口**進入臨界區；採樣間隔 $\ge 2 \sim 5$ 秒。 |
| **長期儲存** | `MicroSD` 讀卡機模組 | `SPI2_HOST`（FAT32 格式） | `CS=10`, `MOSI=11`, `CLK=12`, `MISO=13` | 掛載於 `/sdcard`（檔名統一英數字）；存放本地 WAV/MP3、離線語音片段與感測日誌檔。 |
| **模式按鍵** | 實體微動開關 | GPIO 輸入（內建上拉） | `MODE_BTN=1` | 低電位為按下；開機長按 $> 300\text{ ms}$ 進入 Unity 測試；平時短按循環切換網路模式（$0 \rightarrow 1 \rightarrow 2$），含軟體防彈跳（Debounce 50ms）。 |

---

## 3. 核心系統架構與資料契約（Architecture & Data Contracts）

### 3.1 高獨立性模組化元件架構（`components/`）
所有業務邏輯拆分為 9 個低耦合獨立 ESP-IDF Component（透過 Callback 與 FreeRTOS Queue 解耦跨模組事件，使各模組可單獨編譯、單獨測試與跨專案復用），每個模組內建專屬 `test/` 目錄：

1. `board_bsp`（已完成）：安全 GPIO 定義、共用型別、I2C Master 代理讀寫（內建 4 裝置 Handle 與 Mutex）、`SPI2_HOST` 匯流排、GPIO 1 按鍵與 GPIO 2 LEDC PWM 初始化、開機硬體自檢。
2. `psram_cache`（已完成）：8MB Octal PSRAM 512KB 感測數據環形快取、批次窺視/確認提交與本地毫秒級統計運算。
3. `sd_media`（已完成）：SD 卡掛載、WAV 與 MP3（`dr_mp3.h`）統一 PCM 音訊解碼播放、`/sdcard/logs/` 與 `/sdcard/sensor_offline/` 附加寫入與 CRC32 校驗、剩餘空間監控。
4. `eeprom_blackbox`（已完成）：AT24C256 雙備份 Metadata 與資料分區管理（含 64-Byte 跨頁切分與 6ms 燒錄延遲）、80% 一級預警、100% 滿載透過轉存 Callback 寫入外部儲存並於驗證通過後原子淨空。
5. `env_sensors` + `sensor_hub`（已完成）：拆分為 `env_dht22.c`、`env_gy30.c`、`env_ds3231.c` 與主協調器 `sensor_hub.c`，負責定時採集與 `esp_sntp.h` 校時。
6. `oled_ui`（已完成）：OLED 1024-Byte 幀緩衝三段式渲染、GY-30 EMA 自動調光、EEPROM/SD 儲存臨界警告置頂橫幅。
7. `audio_engine`（Phase 6 基礎管線 + Phase 8 離線語音擴充）：雙 INMP441 立體聲收音與自適應能量 VAD、MAX98357A 動態取樣率播放、非阻塞 16kHz PCM 幀分發介面，以及 Phase 8 擴充之 `ESP-SR` 喚醒／離線命令詞識別（`sr_engine.c`）與 SD 卡音節拼接語音播報（`sr_cmd_dispatcher.c`）。
8. `net_manager`（Phase 7）：拆分為 `net_manager.c`（三態互斥狀態機）、`sync_engine.c`（嚴格時間順序補傳）與 `web_config.c`（模式 2 本地設定網頁伺服器）。

### 3.2 FreeRTOS 雙核心任務隔離契約
* **Core 0（通訊核心）**：Wi-Fi 驅動、LWIP、`net_manager` 狀態機、模式 2 Local Config Web Server、聯網歷史數據補傳任務、雲端語音對話上傳。
* **Core 1（即時核心）**：`audio_engine` 收音與播音任務（最高優先權）、Phase 8 `ESP-SR` 聲學前端餵入與離線語音推論任務、`sensor_hub` 採集任務、`psram_cache` 運算、`eeprom_blackbox` 寫入、`oled_ui` 刷新任務。

### 3.3 統一 16-Byte 感測器數據封包（`sensor_record_t`）
全系統（PSRAM、AT24C256、SD 卡、Server 補傳）統一使用固定 **16 Bytes** 緊湊型二進位結構，嚴禁增減欄位或改變順序：

```c
typedef struct __attribute__((packed)) {
    uint32_t timestamp;     // 4 Bytes: DS3231 提供之 Unix Timestamp（精準至秒）
    int16_t  temperature;   // 2 Bytes: 攝氏溫度 × 10（例：253 代表 25.3°C；無效值為 0x7FFF）
    uint16_t humidity;      // 2 Bytes: 相對濕度 × 10（例：615 代表 61.5%；無效值為 0xFFFF）
    uint32_t lux;           // 4 Bytes: GY-30 環境光照度（單位：lux）
    uint16_t status_flags;  // 2 Bytes: Bit0=DHT22有效, Bit1=GY30有效, Bit2=RTC有效, Bit4..5=網路模式(0/1/2)
    uint16_t crc16;         // 2 Bytes: 前 14 Bytes 之 CRC16-MODBUS 校驗碼
} sensor_record_t;          // 總長度固定 = 16 Bytes
```

### 3.4 `AT24C256`（32,768 Bytes）記憶體分區與設定契約（`sys_config_t`）
* **分區 A：系統設定／Metadata 區（`0x0000` ～ `0x03FF`，共 1,024 Bytes；其中 `0x0000` ～ `0x003F` 為 `eeprom_blackbox` 雙備份 Ping-Pong Metadata）**
  * 儲存固定結構 `sys_config_t`（保留其餘空間供日後擴充）。首次開機若 `magic != 0x585A494F` 或 `crc32` 不符，**必須自動載入出廠預設值並寫入分區 A / NVS**（同時將 `record_count` 歸零）：
    ```c
    #define SYS_CONFIG_MAGIC 0x585A494FUL // "XZIO"

    typedef struct __attribute__((packed)) {
        uint32_t magic;                // 4 Bytes: 固定 0x585A494F
        char     wifi_ssid[32];        // 32 Bytes: 預設 ""
        char     wifi_pass[64];        // 64 Bytes: 預設 ""
        char     server_url[128];      // 128 Bytes: 預設 ""
        uint16_t sample_interval_sec;  // 2 Bytes: 預設 30（合法範圍 5 ~ 3600）
        uint8_t  psram_window_hours;   // 1 Byte:  預設 1（合法範圍 1 ~ 24）
        uint8_t  eeprom_warn_pct;      // 1 Byte:  預設 80（合法範圍 50 ~ 95）
        uint16_t sd_warn_free_mb;      // 2 Bytes: 預設 100（合法範圍 50 ~ 1000）
        uint16_t record_count;         // 2 Bytes: 分區 B 目前已儲存之有效封包筆數（0 ~ 1984）
        uint32_t crc32;                // 4 Bytes: 以上所有欄位（前 236 Bytes）之 CRC32 校驗碼
    } sys_config_t;                    // 總長度固定 = 240 Bytes
    ```
* **分區 B：感測器黑盒子區（`0x0400` ～ `0x7FFF`，共 31,744 Bytes；或由 `eeprom_blackbox` 依既有指標管理之資料區）**
  * 專供存放離線 `sensor_record_t` 封包，最大容量為 **`1,984 筆`**（$1,984 \times 16 = 31,744\text{ Bytes}$）。

### 3.5 `partitions.csv` 與統一離線語音命令列舉（`voice_cmd_id_t`）
* **`partitions.csv`（16MB Flash 自訂分區表）**：
  ```csv
  # Name,   Type, SubType, Offset,   Size,     Flags
  nvs,      data, nvs,     0x9000,   0x6000,
  phy_init, data, phy,     0xf000,   0x1000,
  factory,  app,  factory, 0x10000,  0x400000,
  model,    data, spiffs,  0x410000, 0x300000,
  coredump, data, coredump,0x710000, 0x10000,
  ```
* **統一離線語音命令 ID 列舉（定義於 `components/audio_engine/include/audio_engine.h`）**：
  ```c
  typedef enum {
      VOICE_CMD_NONE = 0,
      VOICE_CMD_VOL_UP,           // 調高音量 / 大聲一點
      VOICE_CMD_VOL_DOWN,         // 調低音量 / 小聲一點
      VOICE_CMD_MUTE_TOGGLE,      // 靜音 / 取消靜音
      VOICE_CMD_PLAY_MUSIC,       // 播放音樂
      VOICE_CMD_STOP_MUSIC,       // 停止播放
      VOICE_CMD_OLED_ON,          // 開啟螢幕
      VOICE_CMD_OLED_OFF,         // 關閉螢幕
      VOICE_CMD_OLED_BRIGHT_UP,   // 調亮螢幕
      VOICE_CMD_OLED_AUTO_BRIGHT, // 自動亮度
      VOICE_CMD_NET_WEB_CFG,      // 開啟網頁配網
      VOICE_CMD_NET_OFFLINE,      // 切換離線模式
      VOICE_CMD_NET_ONLINE,       // 重新連線網路
      VOICE_CMD_REPORT_TEMP,      // 查詢目前溫度
      VOICE_CMD_REPORT_HUMI,      // 查詢目前濕度
      VOICE_CMD_REPORT_LUX,       // 查詢目前照度
      VOICE_CMD_REPORT_ENV_ALL,   // 播報完整環境狀況
      VOICE_CMD_REPORT_TIME,      // 查詢目前時間
      VOICE_CMD_REPORT_STORAGE    // 檢查儲存空間與健康狀態
  } voice_cmd_id_t;
  ```

---

## 4. 功能需求與驗收標準（Functional Requirements & Acceptance Criteria）

### FR-01：開機硬體自檢與匯流排仲裁（`board_bsp`）— `[已完成]`
* **需求說明**：開機時初始化 GPIO、Mutex、I2C Master 代理層、`SPI2_HOST` 匯流排與 GPIO 2 LEDC PWM，並輸出硬體偵測報告。
* **驗收標準**：
  * **AC-01.1**：開機日誌必須印出 `Flash: 16MB` 與 `PSRAM: 8MB (Octal)`；若 `esp_psram_get_size()` 為 0 或非預期容量，必須印出 `ESP_LOGE` 嚴重錯誤。
  * **AC-01.2**：開機執行 I2C 探測並列出在線裝置位址（`0x3C`, `0x23`, `0x68`, `0x50`）；若有感測器未接上，僅記錄警告旗標，不可造成系統當機。
  * **AC-01.3（TDD 單元測試）**：掃描 I2C 匯流排 ACK 回應並測試 `board_set_backlight()` 正常不崩潰。

### FR-02：8MB Octal PSRAM 即時統計快取（`psram_cache`）— `[已完成]`
* **需求說明**：無論處於何種網路模式，每次感測採集皆寫入 PSRAM 環形陣列（`512 KB`），保留最近 $N$ 小時（預設 **1 小時**）數據並執行本地統計。
* **驗收標準**：
  * **AC-02.1**：環形陣列必須配置於 `MALLOC_CAP_SPIRAM`，寫滿自動覆寫最舊資料，絕不阻塞呼叫端。
  * **AC-02.2**：本地端可於 $< 10\text{ ms}$ 內算出區間溫濕度與光照的「平均值、最大值、最小值、首尾升降趨勢」，並支援 `peek_batch()` 與 `commit_pop()` 交易式讀取。
  * **AC-02.3**：在模式 1 聯網詢問「今天／現在環境狀態」時，僅將地端算好的簡短統計摘要文字送交雲端 LLM 潤飾發音，**嚴禁**上傳整包原始數據陣列（以節省 90% 以上 Token 費用）。
  * **AC-02.4（TDD 單元測試）**：透過 Unity 測試餵入模擬溫濕度序列（包含環形覆寫邊界與異常無效值過濾），驗證指標位於 `esp_ptr_external_ram()` 空間、平均值、極值與趨勢判定 100% 準確。

### FR-03：四層漏斗式離線容災儲存與雙重容量預警（`eeprom_blackbox` + `sd_media`）— `[已完成]`
* **需求說明**：當系統處於非聯網同步狀態（如模式 0 關閉 Wi-Fi）時，感測數據依序經過 `PSRAM` $\rightarrow$ `AT24C256` $\rightarrow$ `SD 卡` 進行多級容災儲存，並提供雙重容量預警。
* **驗收標準**：
  * **AC-03.1（EEPROM 64-Byte 跨頁安全寫入與 80% 一級預警）**：
    * 寫入 `AT24C256` 時，底層分頁寫入函式**必須依 64-Byte 實體頁邊界自動切分寫入長度**（防止跨頁位址回繞覆蓋開頭資料），且每寫完一頁後**必須延遲 `6 ms`（`vTaskDelay(pdMS_TO_TICKS(6))`）**等待 EEPROM 內部燒錄完成。
    * 當分區 B 已儲存筆數達到設定閥值（預設 **80%**，即 $\ge 1,587$ 筆）時，立即觸發 `STORAGE_ALERT_EEPROM_80` 警告旗標，透過 OLED 顯示警告橫幅與提示音提醒使用者開啟 Wi-Fi。
  * **AC-03.2（滿載溢位轉存 SD 卡與防覆寫）**：若使用者仍未開啟 Wi-Fi 且 `AT24C256` 分區 B 達到 **100%**（`1,984 筆`）無法再負荷，系統自動將分區 B 內全部 1,984 筆資料讀出，依 `DS3231` 當日日期寫入 SD 卡 **`/sdcard/sensor_offline/YYYYMMDD.bin`**（或既定 `/sdcard/logs/YYYYMMDD.bin`）。寫入模式**必須為 `"ab"`（檔尾附加）**，同日多次滿載須接續往下新增，跨日自動另開新檔，**嚴禁覆寫任何既有資料**。
  * **AC-03.3（原子級先驗證後淨空）**：寫入 SD 卡後，必須執行 `fflush()` 與 `fsync()` 強制落盤，並重新從 SD 卡回讀該批次資料計算 `CRC32` 與比對位元組長度。**唯有 100% 驗證完全一致後**，才可將 `AT24C256` 分區 B（僅限感測器資料區，嚴禁動到系統設定／Metadata 區）完全淨空並將寫入指標歸零；若 SD 卡未插入或驗證失敗，必須保留 `AT24C256` 資料並在 OLED 顯示嚴重錯誤。
  * **AC-03.4（SD 卡二級臨界警告與底線保護）**：系統定期檢查 SD 卡剩餘空間，當剩餘空間低於設定臨界值（預設 **`< 100 MB`** 或使用率 $> 90\%$）時，立即觸發 `STORAGE_ALERT_SD_LOW` 警告，透過 OLED 閃爍警告橫幅與本地語音提示使用者立即開啟 Wi-Fi 同步；若剩餘空間低於安全底線（**`< 20 MB`**），觸發 `STORAGE_ALERT_SD_FULL` 並停止寫入新感測紀錄，以保護 FAT32 檔案系統與既有音訊檔不損毀。
  * **AC-03.5（TDD 單元測試）**：使用記憶體 Mock 模擬 32KB EEPROM 與頁回捲模擬器，驗證 64-Byte 跨頁寫入無錯位、80% 警告觸發、100% 轉存失敗時不淨空資料，以及掉電後 Metadata 指標恢復。

### FR-04：環境感測與 RTC 精準校時（`env_sensors` + `sensor_hub`）— `[已完成]`
* **需求說明**：依設定週期讀取 `DHT22`（`GPIO 14`）與 `GY-30`（`0x23`），搭配 `DS3231`（`0x68`）時間戳封裝為 `sensor_record_t`。
* **驗收標準**：
  * **AC-04.1**：`DHT22` 使用 `esp_rom_sys.h` 與 `< 5 ms` 臨界區讀取；讀取失敗時自動重試最多 2 次，若仍失敗則在 `status_flags` 清除 DHT22 有效位元並填入 `INVALID_TEMP_VALUE` / `INVALID_HUMI_VALUE`，不阻塞其他模組。
  * **AC-04.2**：斷電重啟後 1 秒內從 `DS3231` 恢復系統時間；進入模式 1 連上 NTP 後透過 `esp_sntp.h` 自動校準 `DS3231`。

### FR-05：SD 卡本地音樂（WAV/MP3）與離線語音提示（`sd_media`）— `[已完成]`
* **需求說明**：支援讀取並串流播放 `/sdcard/music/` 內的標準 16-bit PCM WAV（`.wav`）與 MP3（`.mp3`）音樂（如 `daoxiang.mp3` 或 `daoxiang.wav`），以及 `/sdcard/tts_offline/` 內的離線短語音片段（數字、單位與儲存警告提示音）。
* **驗收標準**：
  * **AC-05.1（雙格式統一 PCM 管線與零依賴解碼）**：依副檔名自動分流，前端輸出統一轉為交錯式 `16-bit PCM` 幀送入同一條 `I2S_NUM_1` 輸出管線：
    * **`.wav` 格式**：原生解析標準 44-Byte RIFF/WAVE 檔頭，直接讀取 16-bit PCM 資料區塊。
    * **`.mp3` 格式**：強制使用專案內建之單一標頭檔 **`components/sd_media/include/dr_mp3.h`**（嚴禁 AI 自行手寫 MP3 解碼演算法或修改外部套件依賴），透過 `heap_caps_malloc(sizeof(drmp3), MALLOC_CAP_SPIRAM)` 在 PSRAM 配置解碼器實例，呼叫 `drmp3_init_file()` 與 `drmp3_read_pcm_frames_s16()` 轉為 16-bit PCM（自動跳過 ID3v2 標籤與封面圖）。
  * **AC-05.2（動態取樣率切換、單聲道轉雙聲道與檔名規範）**：
    * `I2S_NUM_1` 固定維持**雙聲道（`I2S_SLOT_MODE_STEREO`）16-bit** 模式；若讀取到的 WAV 或 MP3 來源為單聲道（`channels == 1`），必須在送入 I2S 前將每個樣本複製為左右聲道（`[S, S]`），防止播放速度變為 2 倍快轉。
    * 開檔讀取到檔案實際取樣率（例如離線語音 `16000 Hz`、音樂 `44100 Hz`）後，依 ESP-IDF v6.1.1 原生 API 依序執行 `i2s_channel_disable()` $\rightarrow$ `i2s_channel_reconfig_std_clock()` $\rightarrow$ `i2s_channel_enable()` 動態切換 `I2S_NUM_1` 時脈。
    * SD 卡音訊路徑與檔名統一使用英數字命名（如 `/sdcard/music/daoxiang.mp3`），避免 FatFS 編碼問題導致開檔失敗。
  * **AC-05.3（匯流排分段讓鎖與 TDD 單元測試）**：在模式 0 完全斷網下可正常播放 WAV/MP3 與離線語音；無論播放何種格式，單次讀取 SD 卡緩衝區上限為 **`4096 Bytes`**，每讀取一塊並送入 I2S DMA 後**必須暫時釋放 `xSpiSdMutex`**，確保離線感測寫入或 EEPROM 滿載轉存能隨時插隊，不可發生聲音中斷或寫入失敗。

### FR-06：OLED 自動調光與優先警告顯示（`oled_ui`）— `[已完成]`
* **需求說明**：驅動 `0.96" OLED`（SSD1306）輪播即時溫濕度、PSRAM 1 小時統計摘要與網路狀態，並依環境光自動調整對比度與 `GPIO 2` 背光 PWM。
* **驗收標準**：
  * **AC-06.1（純 C 幀緩衝與代理傳輸）**：`oled_ui` 自行維護 `1024-Byte`（$128 \times 64 / 8$）單色顯存與內建點陣字型，透過 `board_bsp` 提供的 I2C 代理函式發送頁指令至 `0x3C`（**嚴禁**使用 `esp_lcd` 元件直接接管 I2C Bus Handle），並依據 `GY-30` 量測到的 lux 值透過 EMA 濾波自動調整 `GPIO 2` PWM（`25` ～ `255`）與 SSD1306 對比度（指令 `0x81`）。
  * **AC-06.2（優先警告橫幅）**：當 `STORAGE_ALERT_EEPROM_80`、`STORAGE_ALERT_SD_LOW` 或 `STORAGE_ALERT_SD_FULL` 任一旗標為真時，畫面最上方（Page 0～1）**必須優先置頂顯示對應警告橫幅**，直到開啟 Wi-Fi 完成同步後解除。

### FR-07：雙麥克風遠場語音引擎（`audio_engine` — Phase 6 待實作）
* **需求說明**：透過 65mm 間距雙 `INMP441` 陣列進行遠場收音與回音／能量偵測，支援語音喚醒／離線命令分發與本地語音回覆。
* **驗收標準**：
  * **AC-07.1（雙 INMP441 立體聲收音與自適應 VAD）**：可穩定從雙通道 `I2S_NUM_0`（`BCLK=4, WS=5, DIN=6`）擷取 `16000 Hz`、`32-bit` 槽寬音訊流並右移 16 位元轉為 `16-bit` 交錯雙聲道 PCM 至 PSRAM 緩衝區（上限 `10 秒` / `320 KB`）；當前景人聲 RMS 能量超過動態噪聲底線且持續 `150 ms` 觸發 `AUDIO_EVENT_SPEECH_START`，靜音超過 `800 ms` 觸發 `AUDIO_EVENT_SPEECH_END`。
  * **AC-07.2（非阻塞 PCM 幀分發與離線指令觸發介面）**：提供 `audio_engine_register_pcm_tap()` 非阻塞分發 `16kHz / 16-bit` 雙聲道交錯 PCM 幀給 Phase 8 `ESP-SR` 訂閱；同時提供 `audio_engine_inject_cmd(voice_cmd_id_t cmd)` 支援實體按鍵／序列埠／Unity 測試模擬觸發離線指令，並可在模式 0（離線）下直接執行對應系統動作與拼接播放 `/sdcard/tts_offline/` 內的 WAV/MP3 語音片段。
  * **AC-07.3（MAX98357A 動態取樣率播放與 Barge-in 打斷）**：初始化 `I2S_NUM_1`（`BCLK=15, LRC=16, DOUT=7`），播放音訊前動態執行 `i2s_channel_disable()` $\rightarrow$ `i2s_channel_reconfig_std_clock()` $\rightarrow$ `i2s_channel_enable()` 切換取樣率，支援 `0 ~ 100%` 軟體數位音量縮放與 `50 ms` 內打斷停止（`audio_engine_stop_playback()`）。

### FR-08：三態互斥網路狀態機與時間順序補傳引擎（`net_manager` + `sync_engine` — Phase 7 待實作）
* **需求說明**：管理三種互斥網路運行模式，並在聯網後依照「資料產生時間由舊到新」的嚴格順序補傳所有離線數據。
* **驗收標準**：
  * **AC-08.1（三態互斥切換）**：同一時間僅允許一種網路模式運作，支援實體按鍵（`GPIO 1`）或離線語音指令循環切換：
    * **模式 0（Offline）**：徹底關閉 Wi-Fi 射頻（零射頻功耗與干擾），本地功能全速運作。
    * **模式 1（Online）**：連線路由器與外部 Server，開啟 `esp_sntp.h` 校時（回寫 `DS3231`）、雲端自然語音對話、中央氣象署（CWA）預報查詢（經由 Server 轉發）及跨日歷史查詢；若連續重連失敗超過 `3 次`，自動降級至離線模式並每 `30 秒` 低功耗背景探測重連，絕不阻塞 Core 1。
    * **模式 2（Local Config Web）**：進入本地網頁設定模式，具體網頁伺服器與熱點行為依 **`FR-09`** 執行。
  * **AC-08.2（嚴格時間順序補傳，Chronological Sync）**：切換至模式 1 並連上 Server 後，補傳引擎（釘選於 Core 0）必須依資料產生時間由舊到新依序執行以下步驟：
    1. **第一階段（最舊歷史資料，SD 卡 / EEPROM 依時間序）**：優先排空較早產生之離線紀錄（掃描 SD 卡 `/sdcard/sensor_offline/` 與 `/sdcard/logs/`，依檔名日期 `YYYYMMDD.bin` 由小到大排序，以及 `eeprom_blackbox` 內殘留的最舊離線紀錄）；逐檔分批（每包 `50 筆`）傳送至 Server，收到 Server 針對該批次／該檔案全部筆數的 `ACK` 後立即刪除該 `.bin` 檔或呼叫 `eeprom_blackbox_pop_oldest()`，再傳下一批。
    2. **第二階段（較新殘留）**：確認所有舊離線紀錄皆已收到 `ACK` 清空後，解除補傳鎖定。
    3. **第三階段（恢復即時）**：恢復 `psram_cache` 即時數據上報，並對接 Core 1 傳來的 `AUDIO_EVENT_SPEECH_END` 上傳 PSRAM 錄音播放雲端 TTS 回覆（離線模式下播放 `/sdcard/audio/offline_hint.mp3`）。
  * **AC-08.3（斷線保護）**：補傳過程中若網路中斷，未收到 Server `ACK` 的資料**嚴禁刪除 SD 卡檔案或淨空 EEPROM**，待下次連網後繼續補傳。
  * **AC-08.4（TDD 單元測試）**：使用 Mock 檔案清單與模擬 Server ACK 回應，驗證補傳排序嚴格符合時間由舊到新，且模擬斷線（無 ACK）時資料 100% 保留不被刪除。

### FR-09：本地網頁設定介面與參數持久化（Mode 2 Local Config Web — Phase 7 待實作）
* **需求說明**：當使用者切換至模式 2 時，系統於 Core 0 啟動輕量級 HTTP 設定伺服器（`esp_http_server`），提供手機或電腦瀏覽器連線修改系統參數，並安全寫入設定持久化區（`AT24C256` 分區 A / NVS）。
* **驗收標準**：
  * **AC-09.1（雙重連線與 SoftAP 自動降級）**：
    * 若設定檔已有 Wi-Fi 帳密，優先嘗試 STA 連線路由器（逾時上限 **10 秒**）；連線成功後於區域網路 IP（Port `80`）啟動網頁伺服器，並在 OLED 顯示該 LAN IP。
    * 若無設定 Wi-Fi 或 10 秒內連線失敗，**必須自動降級啟動 ESP32 SoftAP 熱點**（預設熱點名稱 SSID：`Xiaozhi_Setup`、密碼：`12345678`、固定閘道 IP：`192.168.4.1`），並在 OLED 顯示熱點名稱與 IP。
  * **AC-09.2（網頁端點、堆疊安全與 PSRAM 緩衝配置）**：
    * `httpd_config_t` 必須將 `core_id` 設為 **`0`**、`stack_size` 設為 **`8192`**；切離模式 2 時必須先呼叫 `httpd_stop()` 再停止 Wi-Fi 射頻。
    * **`GET /`**：回傳內嵌輕量 HTML 設定表單，自動帶入目前 `sys_config_t` 數值（Wi-Fi 密碼欄位留空並提示「留空則不修改原密碼」，避免誤清空原密碼）。
    * **`POST /save`**：限制 `req->content_len <= 1024`，完整接收並補上結尾 `\0` 後解析表單資料（`application/x-www-form-urlencoded`），通過驗證後寫入設定持久化區，回傳成功頁面並重新套用新參數。
    * **`GET /status`**：回傳 JSON 格式之即時感測讀數、PSRAM/EEPROM/SD 卡使用率及目前警告狀態。
    * 所有超過 `2 KB` 的網頁 HTML 模板緩衝區，**必須配置於 `MALLOC_CAP_SPIRAM`**。
  * **AC-09.3（參數合法性數值邊界檢查）**：儲存前伺服器端必須執行嚴格邊界驗證，任一欄位不合法時回傳 HTTP `400 Bad Request` 錯誤提示且**不可寫入設定區**：
    * `wifi_ssid`：長度 `0 ~ 31` 字元。
    * `wifi_pass`：長度 `0`（不變更／開放網路）或 `8 ~ 63` 字元。
    * `server_url`：長度 `0 ~ 127` 字元（非空時必須以 `http://`、`https://`、`ws://` 或 `wss://` 開頭）。
    * `sample_interval_sec`：**`5 ~ 3600` 秒**（預設 `30`；硬體限制：嚴禁低於 5 秒，以保護 DHT22 單線採樣穩定度）。
    * `psram_window_hours`：**`1 ~ 24` 小時**（預設 `1`）。
    * `eeprom_warn_pct`：**`50 ~ 95` %**（預設 `80`）。
    * `sd_warn_free_mb`：**`50 ~ 1000` MB**（預設 `100`；必須高於 20MB 強制停止寫入底線）。
  * **AC-09.4（分區隔離保護與 TDD 單元測試）**：
    * 寫入新設定時，必須同步更新設定區內的 `crc32`，且**嚴禁動到 EEPROM 離線感測數據與寫入指標**。
    * 透過 Unity 單元測試驗證 URL 解碼函式與參數邊界檢查函式（測試正常值通過、越界值如採樣週期設為 `1` 秒或 SD 警告設為 `10` MB 必須被攔截拒絕），無需依賴實體 Wi-Fi 即可完成驗證。

---

## 5. 系統核心資料流與狀態轉換圖（Reference Diagrams）

### 5.1 環境監測、四層漏斗式離線儲存與模式 1 三階段補傳資料流（原版基準圖）

```text
[感測器定時採集: DHT22 + GY-30 + DS3231]
       │
       ├──► (1) 【8MB Octal PSRAM】永遠寫入 -> 近 1 小時統計摘要 -> OLED 顯示 & 節省 LLM Token
       │
       └──► (若未處於模式 1 聯網即時狀態)
                 ▼
            (2) 【AT24C256 分區 B】寫入 (上限 1,984 筆)
                 ├──► 達 80% ──► 觸發【一級預警】：OLED 橫幅 + 提示音
                 └──► 達 100% ──► 附加寫入 SD 卡 (/sdcard/sensor_offline/YYYYMMDD.bin)
                                   ──► fsync() + CRC32 回讀驗證通過
                                   ──► 【精準淨空 AT24C256 分區 B】
                                   ──► 若 SD 剩餘 < 100MB ──► 觸發【二級嚴重預警】

[使用者切換至模式 1（開啟 Wi-Fi）並連上 Server]
       │
       ▼
  [階段 1] 掃描 SD 卡 /sensor_offline/，依檔名日期由舊到新分批上傳 ──► 收到 ACK 刪除檔案
       │ (SD 卡離線目錄完全清空後)
       ▼
  [階段 2] 讀取 AT24C256 分區 B 剩餘資料，由舊到新分批上傳 ──► 收到 ACK 淨空分區 B
       │
       ▼
  [階段 3] 恢復正常即時感測上報與雲端語音互動
```

### 5.2 雙麥克風語音互動與 Phase 8 離線聲控分流資料流（新增語音管線圖）

```text
[雙 INMP441 16kHz 雙聲道收音 (Core 1, I2S_NUM_0: BCLK=4, WS=5, DIN=6)]
       │
       ├──► [Phase 6] 自適應能量 VAD ──► 錄音存入 PSRAM (320KB) ──► [Phase 7] 聯網上傳雲端對話
       │
       └──► [Phase 8] Non-blocking PCM Frame Tap ──► PSRAM RingBuffer
                 └──► ESP-AFE (雙麥波束成形 + 降噪)
                        ├──► [狀態 1: SR_STATE_WAKE_LISTEN] WakeNet 監聽喚醒詞
                        └──► [狀態 2: SR_STATE_CMD_LISTEN]  MultiNet 離線命令詞 (6000ms 逾時退回狀態 1)
                               ├──► 硬體/模式控制: 音量 / 音樂 / 螢幕亮度 / 切換配網或離線模式
                               └──► 狀態語音查詢: 讀取感測與儲存數據 ──► 拼接播放 /sdcard/tts_offline/*.wav
```

---

## 6. 追加功能：無新增硬體之本地離線語音喚醒與命令詞擴充（Phase 8 / `FR-10` & `NFR-01`）

### FR-10：本地離線語音喚醒與命令詞識別引擎（Offline WakeNet & MultiNet）
* **執行前提**：必須在 Phase 6 與 Phase 7 全部完成並通過整機穩定度驗證後再依序執行；**不新增任何外部晶片與 GPIO 腳位**。
* **驗收標準**：
  * **AC-10.1（硬體零新增與雙麥聲學前端 `ESP-AFE`）**：
    * 100% 複用既有雙 `INMP441` 麥克風陣列（`GPIO 4/5/6`）、`MAX98357A` 功放（`GPIO 15/16/7`）與 SD 卡儲存空間。
    * 透過 `components/audio_engine/idf_component.yml` 引入樂鑫官方 **`espressif/esp-sr`** 元件。
    * 透過 `audio_engine_register_pcm_tap()` 訂閱雙 `INMP441` 的 `16kHz / 16-bit` 雙聲道交錯 PCM 幀，寫入配置於 PSRAM 的環形緩衝區，由 `task_sr_feed`（釘選於 Core 1，優先級 5，Stack `4096 B`）餵入 **`ESP-AFE`**（`AFE_TYPE_SR`，`pcm_config.total_ch_num = 2, pcm_config.mic_num = 2`）執行雙麥波束成形（Beamforming）與環境降噪（NS）。
  * **AC-10.2（雙階段離線識別狀態機與 6 秒逾時退回）**：
    * 透過 `esp_srmodel_init("model")` 從 `3MB` `model` 分區載入中文 WakeNet 與 MultiNet 模型，推論任務 `task_sr_detect` 釘選於 Core 1（優先級 4，Stack `6144 B`）。
    * **階段一（`SR_STATE_WAKE_LISTEN`）**：開機預設狀態，僅啟動輕量級 **WakeNet** 持續監聽喚醒詞（如「你好小智」）；偵測成功後播放 `/sdcard/tts_offline/wake.wav` 提示音，並透過 `oled_ui` 在頂部狀態列顯示「聆聽中」狀態。
    * **階段二（`SR_STATE_CMD_LISTEN`）**：喚醒後切換至 **MultiNet** 離線命令詞識別，啟動 **`6,000 ms` 逾時計時器**；識別到有效指令後派發對應 `voice_cmd_id_t` 並重設回 `SR_STATE_WAKE_LISTEN`；若超過 `6,000 ms` 無有效指令，自動退回 `SR_STATE_WAKE_LISTEN` 並恢復 OLED 待機顯示（端到端識別延遲 $< 500\text{ ms}$）。
  * **AC-10.3（鎖定漢語拼音離線命令詞表 — 禁止弱 AI 自行更改拼音）**：
    * 必須使用 `esp_mn_commands_add(cmd_id, pinyin)` 註冊以下基礎指令表：

| `voice_cmd_id_t` | 漢語拼音註冊字串（`esp_mn_commands_add`） | 對應系統執行動作 |
| :--- | :--- | :--- |
| `VOICE_CMD_VOL_UP` | `"da sheng yi dian"`, `"tiao gao yin liang"` | 調高音量（`+15%`） |
| `VOICE_CMD_VOL_DOWN` | `"xiao sheng yi dian"`, `"tiao di yin liang"` | 調低音量（`-15%`） |
| `VOICE_CMD_MUTE_TOGGLE` | `"jing yin"`, `"qu xiao jing yin"` | 切換靜音 / 取消靜音 |
| `VOICE_CMD_PLAY_MUSIC` | `"bo fang yin yue"` | 播放 `/sdcard/music/` 音樂 |
| `VOICE_CMD_STOP_MUSIC` | `"ting zhi bo fang"` | 呼叫 `audio_engine_stop_playback()` |
| `VOICE_CMD_OLED_ON` | `"da kai ping mu"` | 開啟 OLED 顯示與背光 |
| `VOICE_CMD_OLED_OFF` | `"guan bi ping mu"` | 關閉 OLED 顯示與背光 |
| `VOICE_CMD_OLED_BRIGHT_UP` | `"tiao liang ping mu"` | 提高螢幕背光亮度 |
| `VOICE_CMD_OLED_AUTO_BRIGHT` | `"zi dong liang du"` | 恢復 `GY-30` 自動調光 |
| `VOICE_CMD_NET_WEB_CFG` | `"kai qi wang ye pei wang"` | `net_manager_set_mode(NET_MODE_WEB_CFG)` |
| `VOICE_CMD_NET_OFFLINE` | `"qie huan li xian mo shi"` | `net_manager_set_mode(NET_MODE_OFFLINE)` |
| `VOICE_CMD_NET_ONLINE` | `"chong xin lian xian wang luo"` | `net_manager_set_mode(NET_MODE_ONLINE)` |
| `VOICE_CMD_REPORT_TEMP` | `"xian zai ji du"`, `"wen du duo shao"` | 離線語音播報目前溫度 |
| `VOICE_CMD_REPORT_HUMI` | `"xian zai shi du duo shao"` | 離線語音播報目前濕度 |
| `VOICE_CMD_REPORT_LUX` | `"xian zai guang xian zen me yang"` | 離線語音播報目前照度 |
| `VOICE_CMD_REPORT_ENV_ALL` | `"bo bao huan jing zhuang kuang"` | 依序播報溫度、濕度與照度 |
| `VOICE_CMD_REPORT_TIME` | `"xian zai ji dian ji fen"` | 讀取 DS3231 並播報目前時分 |
| `VOICE_CMD_REPORT_STORAGE` | `"jian cha cun chu zhuang tai"` | 播報 EEPROM 與 SD 卡健康狀態 |

  * **AC-10.4（SD 卡音節拼接離線語音播報規範）**：
    * 於 SD 卡 `/sdcard/tts_offline/` 目錄存放常用提示短語與基礎音節 WAV 檔，檔名鎖定如下：
      * 數字音節：`0.wav` ～ `9.wav`、`shi.wav`（十）、`bai.wav`（百）、`dian.wav`（點）、`fu.wav`（負）
      * 單位與提示音：`du.wav`（度）、`baifenzhi.wav`（百分之）、`lux.wav`（勒克斯）、`dian_time.wav`（點-時間）、`fen.wav`（分）、`wake.wav`（喚醒提示音）、`ok.wav`（執行確認音）、`warn_storage.wav`（儲存警告提示音）
    * 查詢環境數值或系統狀態時（例如溫度 `25.3°C`），由 `sr_cmd_dispatcher` 展開為 `["2.wav", "shi.wav", "5.wav", "dian.wav", "3.wav", "du.wav"]` 並依序透過 `audio_engine_play_file()` 播報，完全不依賴網路連線。
  * **AC-10.5（TDD 單元測試）**：
    * 建立 `components/audio_engine/test/test_sr_engine.c`，驗證漢語拼音指令表動態註冊、喚醒後 `6,000 ms` 逾時自動退回 `SR_STATE_WAKE_LISTEN` 狀態機邏輯，以及溫度／濕度／照度數值展開為 `/sdcard/tts_offline/*.wav` 音節檔名序列的 100% 準確性。

### NFR-01：離線語音模型分區與雙記憶體隔離規範
* **Flash 模型分區隔離**：必須在 `partitions.csv` 劃分獨立 **`3 MB`（`0x300000`）** 的 `model` 資料分區（偏移 `0x410000`），專門存放 WakeNet 與 MultiNet 中文模型權重，嚴禁擠佔 `4 MB`（`0x400000`）的 `factory` 主程式分區。
* **PSRAM 與內部 SRAM 記憶體隔離**：`ESP-AFE` 演算法與 WakeNet / MultiNet 模型權重推論緩衝（約 `1.5 MB ~ 2.0 MB`）必須配置於 **8MB Octal PSRAM**（`CONFIG_SPIRAM`），不可佔用內部 SRAM；推論任務（`task_sr_feed`、`task_sr_detect`）固定釘選於 **Core 1**，避免與 Core 0 的 Wi-Fi 與 HTTP 任務競爭資源。
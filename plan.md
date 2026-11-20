# Xiaozhi IoT 技術架構與模組拆分設計書（`plan.md`）

## 1. 系統架構與零耦合拓撲（System Architecture）

本設計嚴格維持 `net_manager`、`audio_engine` 與 `sensor_hub` 三大元件之間的**零編譯期相依**，並透過 FreeRTOS 雙核心釘選（Core Pinning）隔離即時音訊處理與網路 I/O。

```text
+-----------------------------------------------------------------------------------+
|                                  main (橋接層)                                    |
|  app_bridges.c: 實作 net_manager_hooks_t，持有 FreeRTOS Cmd Queue 與狀態快照指標   |
+-------------------+-----------------------------------+---------------------------+
                    | (Hook 指標註冊)                   | (直接呼叫 API)
                    v                                   v
+---------------------------------------+   +---------------------------------------+
|     Core 0: net_manager (網路與網頁)   |   |     Core 1: audio_engine (高優先權)    |
|  - net_manager.c / net_mode_ctrl.c    |   |  - audio_engine.c / aec_ref.c         |
|  - web_config.c / web_api.c           |   |  - I2S RX (雙 INMP441) + TX (MAX98357) |
|  - voice_cloud.c (可中斷 HTTP 上傳)    |   |  - ESP-AFE (2-Mic + 1-Ref AEC + AGC)  |
|  - mDNS (xiaozhi.local) + HTTPD       |   |  - 內部 SRAM aec_ref RingBuffer       |
+---------------------------------------+   +---------------------------------------+
                    |                                   ^
                    +------> Core 0: sensor_hub <-------+ (透過 main 取得溫濕光數據)
```

---

## 2. 模組拆分與單檔行數預算規劃（$\le 300$ 行硬性約束）

為防止既有檔案（`net_manager.c` 296 行、`web_config.c` 272 行）超出 300 行紅線，所有新增功能一律拆入獨立子模組：

| 所屬元件 | 檔案路徑 | 狀態 | 職責說明 | 預估行數 |
| :--- | :--- | :--- | :--- | :---: |
| `audio_engine` | `components/audio_engine/include/aec_ref.h` | **新增** | AEC 參考緩衝區與 Q16.16 重採樣介面宣告 | ~60 行 |
| `audio_engine` | `components/audio_engine/aec_ref.c` | **新增** | 內部 SRAM RingBuffer、Q16.16 抗混疊重採樣、Pre-fill 與 Flush 實作 | ~220 行 |
| `audio_engine` | `components/audio_engine/audio_engine.c` | 修改 | 釘選 Core 1、整合 `aec_ref` 音量後取樣、AFE 2-Mic+1-Ref 與 AGC | ~260 行 |
| `net_manager` | `components/net_manager/include/net_manager.h` | 修改 | 擴充 `sys_status_snapshot_t`、`ctrl_cmd_id_t` 與雙開關 API | ~160 行 |
| `net_manager` | `components/net_manager/net_manager_priv.h` | 修改 | 內部私有狀態、NVS 延遲落盤旗標與 `wifi_state` 共享宣告 | ~95 行 |
| `net_manager` | `components/net_manager/net_mode_ctrl.c` | **新增** | 雙獨立開關狀態機、NVS 防抖避播音落盤、mDNS 與 1.5s 實體按鍵計時 | ~240 行 |
| `net_manager` | `components/net_manager/net_manager.c` | 修改 | 瘦身重構，將模式切換與按鍵處理委派給 `net_mode_ctrl.c` | ~230 行 |
| `net_manager` | `components/net_manager/web_api.h` | **新增** | `GET /status` 與 `POST /api/control` 路由註冊宣告 | ~40 行 |
| `net_manager` | `components/net_manager/web_api.c` | **新增** | JSON 狀態序列化、前置條件校驗與控制指令派發 | ~220 行 |
| `net_manager` | `components/net_manager/web_config.c` | 修改 | 調校 `httpd` Socket 並發參數、註冊靜態資源快取與 `web_api` | ~220 行 |
| `net_manager` | `components/net_manager/voice_cloud.c` | 修改 | 單聲道 `X-Channels: 1` 上傳、原子 `abort_flag` 與 $\le 3\text{s}$ 逾時 | ~230 行 |
| `net_manager` | `components/net_manager/index.html` | 修改 | 精簡 HTML 骨架（環境看板、雙開關、快捷鍵、Toast 容器） | ~150 行 |
| `net_manager` | `components/net_manager/style.css` | **新增** | 零外部依賴響應式深色／淺色卡片樣式與狀態徽章 | ~160 行 |
| `net_manager` | `components/net_manager/app.js` | **新增** | `AbortController` 輪詢、防連發鎖定、防蓋台序號與 Toast 邏輯 | ~210 行 |
| `main` | `main/app_bridges.c` | 修改 | 實作 `get_sys_status`、`exec_ctrl_cmd` 佇列派工與音訊搶占邏輯 | ~210 行 |

---

## 3. 核心資料結構與跨模組介面設計（`net_manager.h`）

為避免每新增一個遙控按鈕就膨脹 Hook 簽章，跨模組通訊統一濃縮為**狀態快照結構體**與**指令列舉**：

```c
/* 網路真實連線階段列舉 */
typedef enum {
    WIFI_STATE_OFF = 0,      /* Wi-Fi 晶片已關閉 (WIFI_MODE_NULL) */
    WIFI_STATE_AP_SETUP,     /* 強制配網熱點模式 (Xiaozhi_Setup) */
    WIFI_STATE_CONNECTING,   /* 正在連線至家裡路由器 */
    WIFI_STATE_CONNECTED     /* 已取得區網 IP (STA Connected) */
} wifi_conn_state_t;

/* 遠端遙控指令列舉 */
typedef enum {
    CTRL_CMD_VOL_UP = 0,     /* 音量增加 (+10%，上限 85~90%) */
    CTRL_CMD_VOL_DOWN,       /* 音量減少 (-10%) */
    CTRL_CMD_MUTE_TOGGLE,    /* 靜音 / 恢復切換 */
    CTRL_CMD_MUSIC_TOGGLE,   /* 播放 / 停止 SD 卡音樂 */
    CTRL_CMD_SPEAK_ENV       /* 搶占音樂並離線語音播報環境數值 */
} ctrl_cmd_id_t;

/* 系統即時狀態快照 (由 main 層填入，供 web_api.c 序列化) */
typedef struct {
    float temperature;       /* 攝氏溫度 (°C) */
    float humidity;          /* 相對濕度 (%) */
    float lux;               /* 環境光照度 (Lux) */
    uint8_t volume;          /* 目前音量 (0 ~ 90) */
    bool is_muted;           /* 是否處於靜音 */
    bool is_music_playing;   /* 是否正在播放 SD 卡音樂 */
    uint32_t err_seq;        /* 非同步背景錯誤遞增序號 (初始為 0) */
    char last_err[64];       /* 最近一次背景執行失敗原因 */
} sys_status_snapshot_t;
```

在 `net_manager_hooks_t` 中僅新增以下高內聚成員：
```c
bool (*get_sys_status)(sys_status_snapshot_t *out);
esp_err_t (*exec_ctrl_cmd)(ctrl_cmd_id_t cmd, int32_t param);
bool (*is_audio_busy)(void); /* 供 NVS 延遲落盤檢查音訊是否閒置 */
```

---

## 4. 音訊 AEC 管線與時脈對齊設計（`aec_ref.c`）

### 4.1 參考訊號資料流與記憶體配置
```text
[AI 回覆 / SD 音樂 / 離線語音]
        │
        ▼  (套用軟體音量倍率，硬上限 85%~90% 防喇叭削波)
[音量後 PCM 緩衝區] ──► i2s_channel_write() ──► I2S TX DMA (60~80 ms) ──► MAX98357A
        │
        ▼  (緊貼 DMA 寫入前一刻擷取)
[Q16.16 定點數抗混疊重採樣] (44.1k / 24k -> 16kHz 單聲道)
        │
        ▼
[內部 SRAM aec_ref RingBuffer] (MALLOC_CAP_INTERNAL, 容量 160 ms = 5,120 Bytes)
        │  - 播放啟動時預填 30 ms 靜音補償硬體路徑延遲
        │  - 水位 > 130 ms 時丟棄最舊樣本防超前失鎖
        │  - 停止播放或 Barge-in 打斷時立即 Flush 清空
        ▼
[ESP-AFE Engine (Core 1)] ◄── I2S RX (雙 INMP441, 16kHz)
  - 2-Mic + 1-Ref (aec_filter_length = 2)
  - 待機緩衝不足時自動補 0 (靜音樣本)
  - 啟用 agc_init = true -> 輸出 16-bit 單聲道 PCM 存入 PSRAM
```

### 4.2 Q16.16 無漂移重採樣演算法
- 計算固定步進值：`step_q16 = ((uint32_t)src_rate << 16) / 16000U`。
- 例如 `44100 Hz` 對應 `step_q16 = 180633`（即 $2.75625 \times 65536$），每次輸出一個 16 kHz 樣本時累加 `phase_q16 += step_q16`，利用整數部分 `idx = phase_q16 >> 16` 與小數部分 `frac = phase_q16 & 0xFFFF` 執行一階低通線性內插，並維持 3-tap 抗混疊前置濾波，確保長時間播放零累積時脈漂移。

---

## 5. 網路雙開關狀態機、NVS 落盤與 HTTPD 設計

### 5.1 NVS 獨立鍵值設計（不破壞舊有 `sys_config_t`）
- 保留既有 NVS Namespace `"net_cfg"` 下的 `"config"` Blob 結構不變，確保重燒韌體後既有 `ssid`、`password`、`server_url` 完整保留。
- 新增兩個獨立 `u8` 鍵值：
  - `"wifi_en"`：預設 `1`（開啟區網 Wi-Fi）。
  - `"cloud_en"`：預設 `1`（開啟雲端伺服器）。
- **防爆音落盤機制**：網頁切換開關時立即更新 RAM 變數並設 `s_nvs_dirty = true` 記錄時間戳；主迴圈檢查當 `esp_timer_get_time() - dirty_ts >= 1,000,000 us` **且** `hooks.is_audio_busy() == false` 時，才執行 `nvs_set_u8()` 與 `nvs_commit()`。

### 5.2 實體按鍵計時狀態機（1.5 秒單一分界點）
- **開機檢測**：啟動初始化時若檢測到按鍵處於按下狀態，直接進入 `WIFI_STATE_AP_SETUP`（`Xiaozhi_Setup`）。
- **運行期檢測**：
  - 按下瞬間記錄 `press_start_ms`，重置 `long_press_handled = false`。
  - 持續按住且 `!long_press_handled` 時，一旦 `now - press_start_ms >= 1500`：**當下立即觸發**強制開啟 `Xiaozhi_Setup` SoftAP，將 `wifi_en = true`、`wifi_state = WIFI_STATE_AP_SETUP`，並設 `long_press_handled = true`。
  - 放開按鍵時：若 `!long_press_handled` 且 `now - press_start_ms >= 50`（防彈跳），執行短按動作——切換 `wifi_en = !wifi_en`。

### 5.3 HTTPD 並發調校與 API 契約（`web_api.c`）
- `httpd_config_t` 參數調整：`config.max_open_sockets = 7`、`config.lru_purge_enable = true`、`config.max_uri_handlers = 12`。
- `POST /api/control` 請求與回應格式：
  - **Request JSON**：`{"cmd": "vol_up" | "vol_down" | "mute" | "music_toggle" | "speak_env" | "set_wifi" | "set_cloud", "val": 0 | 1}`
  - **Response JSON**：
    ```json
    {
      "ok": true,
      "msg": "OK",
      "state": {
        "temp": 25.4, "humi": 58.0, "lux": 320,
        "vol": 60, "muted": false, "music": true,
        "wifi_en": true, "cloud_en": false, "wifi_state": "CONNECTED",
        "err_seq": 0, "last_err": ""
      }
    }
    ```
  - 若 `cmd == "set_cloud"` 且 `val == 1`，後端先檢查 `strlen(cfg.server_url) > 0`，若為空則回傳 `{"ok": false, "msg": "請先於下方設定伺服器網址", "state": {...}}`。

---

## 6. 前端狀態機與防競態設計（`index.html` / `style.css` / `app.js`）

1. **零外部依賴打包**：在 `components/net_manager/CMakeLists.txt` 的 `EMBED_TXTFILES` 加入 `index.html`、`style.css`、`app.js`，所有圖示採用內嵌 `<svg>`，字型採用 `system-ui, -apple-system, sans-serif`。
2. **請求世代編號（Generation Counter）防蓋台**：
   - 前端維護遞增變數 `reqGen`。每次發起 `GET /status` 時記下當前 `myGen = reqGen`，若回應抵達時 `myGen !== reqGen`（代表期間使用者按了控制按鈕），直接丟棄該筆舊輪詢資料。
   - 發送 `POST /api/control` 時遞增 `reqGen`、立即 `abort()` 進行中的輪詢 `AbortController`、鎖定所有控制按鈕，收到回應後直接用回傳的 `res.state` 更新 UI 並解鎖按鈕。
3. **非同步錯誤 Toast**：前端記錄 `lastSeenErrSeq`，每次刷新狀態發現 `state.err_seq > lastSeenErrSeq` 時，更新序號並在畫面頂部彈出 3 秒提示條顯示 `state.last_err`。
# Xiaozhi IoT 系統優化與功能擴充規格書（`spec.md`）

## 1. 專案背景與目標
本規格書定義 Xiaozhi IoT 韌體（ESP-IDF 平台）的系統重構與功能擴充需求，旨在解決全場景音訊自激回音、降低雲端語音上傳頻寬、解耦區網與雲端連線狀態，並將內建網頁升級為具備即時環境監控與遠端遙控能力的控制面板。

---

## 2. 核心架構與硬體邊界約束（Non-Negotiable Constraints）

| 約束編號 | 約束項目 | 強制規範內容 |
| :--- | :--- | :--- |
| **CON-01** | **單檔行數硬性上限** | 所有 `.c`、`.h`、`.html`、`.css`、`.js` 原始碼檔案**嚴格限制 $\le 300$ 行**。超出時必須依職責拆分子模組檔案，嚴禁以省略實作細節換取行數。 |
| **CON-02** | **模組零耦合** | `net_manager`、`audio_engine`、`sensor_hub` 之間禁止直接 `#include` 彼此標頭檔，所有跨模組查詢與控制一律透過 `net_manager_hooks_t` 回呼函式指標在 `main` 層橋接。 |
| **CON-03** | **雙核心任務隔離** | `audio_engine` 與 `ESP-AFE` 任務必須釘選於 **Core 1**（高優先權）；Wi-Fi、`httpd` 與 `sensor_hub` 任務釘選於 **Core 0**，防止網路中斷或 HTTP 請求干擾音訊時序。 |
| **CON-04** | **記憶體與 MSPI 匯流排隔離** | 大型緩衝（雲端錄音緩衝、MP3 解碼緩衝、HTTP 接收緩衝）配置於外部 PSRAM；高頻讀寫的 `aec_ref` 參考緩衝區（約 5.1 KB）必須配置於**內部 SRAM（`MALLOC_CAP_INTERNAL`）**，避免與內部 Flash 共用 MSPI 快取產生阻塞。 |

---

## 3. 功能需求規格（Functional Requirements）

### FR-01：全場景聲學回音消除（AEC）與參考訊號管線
- **FR-01.1 全來源參考擷取**：凡送往 MAX98357A I2S 輸出的所有音源（包含**雲端 AI 語音回覆、SD 卡本地音樂播放、離線拼音語音播報**），皆須同步寫入 `aec_ref` 參考 RingBuffer。
- **FR-01.2 音量後取樣與防削波上限**：參考訊號擷取點必須位於**軟體音量倍率調整（Volume Scaling）之後**、緊貼 `i2s_channel_write()` 寫入 DMA 前一刻；軟體最大音量上限限制於 **85%～90%** 不失真區間，避免小喇叭硬體削波產生線性 AEC 無法消除的諧波失真。
- **FR-01.3 DMA 緩衝與相位時間窗對齊**：
  - I2S TX DMA 總緩衝區設定為 **60～80 ms**。
  - `aec_ref` RingBuffer 容量設定為 **150～160 ms**（配合 `ESP-AFE` `aec_filter_length = 2`，對應 128 ms 消除窗口）。
  - 每次播放啟動時預填約 **30 ms** 靜音樣本作為硬體路徑延遲補償；當緩衝水位超過 **130 ms** 時平滑丟棄最舊樣本，防止參考訊號超前實際播放導致濾波器失鎖。
- **FR-01.4 Q16.16 無漂移抗混疊重採樣**：非 16 kHz 音源（如 44.1 kHz / 24 kHz 音樂）寫入參考緩衝區前，必須透過**Q16.16 定點數相位累加器搭配低通內插濾波**轉換為 **16 kHz / 16-bit 單聲道**，確保長時間播放零累積樣本漂移且無頻譜混疊雜訊。
- **FR-01.5 打斷播放（Barge-in）即時清空**：當播放任務停止或遭喚醒詞打斷時，必須**立即清空（Flush）`aec_ref` RingBuffer**，避免殘留未播完的參考資料失真後續人聲辨識。
- **FR-01.6 待機靜音補零**：喇叭無播放（RingBuffer 資料不足）時自動補 `0` 湊齊幀長餵給 `ESP-AFE`，確保 `feed()` 持續運行並維持待機喚醒靈敏度（採用 **2-Mic + 1-Ref** 模式：`[Mic_L, Mic_R, Ref_Spk]`）。

### FR-02：雙麥收音單聲道雲端上傳與可中斷串流
- **FR-02.1 單聲道降噪與 AGC 輸出**：硬體端保留雙 INMP441 收音與 `ESP-AFE` 雙麥波束成型／降噪，並明確啟用 **`agc_init = true`（自動增益控制）**，輸出 **16-bit 單聲道（1-Channel）** PCM 存入 PSRAM。
- **FR-02.2 傳輸標頭調整**：`voice_cloud` 上傳音訊時，HTTP Request Header 改為 `X-Channels: 1`，頻寬消耗減半。
- **FR-02.3 HTTP 串流即時可中斷性與快速失敗**：
  - `voice_cloud` 於每次讀寫 HTTP Chunk 迴圈中檢查原子旗標 `abort_flag`；一旦觸發打斷播放（Barge-in）或網頁關閉 `cloud_enabled`，必須在 **$< 50\text{ ms}$ 內主動關閉 HTTP 連線**並釋放資源。
  - HTTP 連線與讀取 `timeout_ms` 設定為 **$\le 3000\text{ ms}$**，外網異常時快速失敗並轉播本機離線提示音。

### FR-03：區網 Wi-Fi 與雲端伺服器雙獨立開關與 mDNS
- **FR-03.1 無痛擴充 NVS（保留既有帳密）**：不更動既有 `sys_config_t` 二進位結構大小，改用獨立 NVS Key（`"wifi_en"` 與 `"cloud_en"`，透過 `nvs_get_u8` / `nvs_set_u8` 讀寫，未設定時預設為 `1`），確保升級重燒韌體保留既有 Wi-Fi 帳密與伺服器網址。
- **FR-03.2 防抖且避開播音的 Flash 落盤**：開關切換時先更新 RAM 狀態並立即回應前端，延遲 `1 秒`（防抖）且**確認 `!audio_is_busy()`（音訊閒置）時才執行 `nvs_commit()`**，杜絕 SPI Flash 寫入凍結（30～100 ms）導致音訊爆音。
- **FR-03.3 三種組合狀態與細粒度連線狀態**：
  - 設定開關：`wifi_enabled`（布林）、`cloud_enabled`（布林）。
  - 即時狀態列舉 `wifi_state`：`OFF`、`AP_SETUP`、`CONNECTING`、`CONNECTED`。
  - **組合 1（Wi-Fi 開 ＋ 雲端開）**：連上路由器、網頁遙控可用、AI 聊天與感測數據正常上傳；OLED 顯示 `192.168.x.x`。
  - **組合 2（Wi-Fi 開 ＋ 雲端關）**：連上路由器、**網頁遙控保持可用**，停止連線 `server_url`（聊天播離線提示音、感測數據改存本機）；OLED 顯示 `192.168.x.x [LAN]`。
  - **組合 3（Wi-Fi 關）**：進入 `WIFI_MODE_NULL` 前先優雅停止 `httpd` 與 `mdns`，避免 Socket 洩漏。
- **FR-03.4 狀態轉換連動與安全關閉**：
  - 透過 `/save` 表單儲存有效 Wi-Fi 帳密後，自動將 `wifi_en`（及有填 `server_url` 時的 `cloud_en`）設為 `1` 並發起連線。
  - 僅在 `wifi_en == true && !is_ap_mode` 時允許 STA 斷線自動重連。
  - 偵測到 `cloud_enabled` 由 `false` 轉為 `true` 且 STA 已連線時，主動觸發本機暫存數據補傳與雲端健康檢查。
  - 收到關閉 Wi-Fi 指令時，先回傳 HTTP `200 OK` 給前端，主迴圈延遲 `150～200 ms` 後再執行網路關閉流程。
- **FR-03.5 mDNS 固定域名**：在 `IP_EVENT_STA_GOT_IP` 時啟動／更新 `mdns`，綁定主機名稱 `xiaozhi`，支援透過 **`http://xiaozhi.local`** 存取網頁。

### FR-04：實體按鍵短按／長按行為重構
- **FR-04.1 單一時間分界點（1.5 秒）**：移除三模式循環切換與時間死區，改為：
  - **短按放開（$< 1.5\text{ 秒}$）**：切換 `wifi_enabled`（開 $\leftrightarrow$ 關），作為網頁斷線後的實體復原鍵。
  - **長按（按滿 $\ge 1.5\text{ 秒}$ 當下即觸發，或開機時按住）**：強制啟動 `Xiaozhi_Setup` SoftAP 配網熱點（`192.168.4.1`），同步將 `wifi_enabled` 強制設為 `true`、狀態設為 `AP_SETUP`，並設定 `long_press_handled` 旗標防止放開時誤觸短按。
- **FR-04.2 離線語音指令對齊**：既有離線語音指令（`VOICE_CMD_WIFI_ON` / `VOICE_CMD_WIFI_OFF` / `VOICE_CMD_AP_MODE`）直接操作對應開關狀態機。

### FR-05：高內聚跨模組橋接介面與音訊搶占
- **FR-05.1 精簡 Hook 介面**：`net_manager_hooks_t` 僅新增兩個高內聚介面：
  - `bool (*get_sys_status)(sys_status_snapshot_t *out)`：一次取得溫度（`°C`）、濕度（`%`）、光照度（`Lux`）、音量百分比、音樂播放狀態，以及非同步錯誤遞增序號 `err_seq` 與錯誤訊息 `last_err`。
  - `esp_err_t (*exec_ctrl_cmd)(ctrl_cmd_id_t cmd, int32_t param)`：透過統一指令列舉派工（`VOL_UP`、`VOL_DOWN`、`MUTE_TOGGLE`、`MUSIC_TOGGLE`、`SPEAK_ENV`）。
- **FR-05.2 跨核心執行緒安全與音訊搶占**：Core 0 `httpd` 呼叫控制 Hook 時僅寫入原子旗標或 FreeRTOS Queue，由 Core 1 `audio_engine` 安全執行；觸發 `SPEAK_ENV` 或新語音指令前，若正在播放 SD 卡音樂，必須先安全停止音樂並釋放解碼器；背景執行失敗時遞增 `err_seq` 並更新 `last_err`。

### FR-06：網頁控制 API 與高並發 HTTP 調校
- **FR-06.1 HTTPD 並發設定**：啟用 `lru_purge_enable = true`，`max_open_sockets = 7`，靜態資源加上 `Cache-Control` 標頭。
- **FR-06.2 `GET /status`**：回傳包含環境數值、音量、音樂狀態、`wifi_enabled`、`cloud_enabled`、`wifi_state`、`err_seq`、`last_err` 的 JSON。
- **FR-06.3 `POST /api/control`**：執行前校驗前置條件（如 `server_url` 為空時拒絕開啟雲端），統一回傳 `{"ok": true/false, "msg": "...", "state": {...}}`，成功時直接附帶最新狀態快照。

### FR-07：前端即時監控與遙控面板
- **FR-07.1 零外部依賴**：嚴禁引用外部 CDN 字型或圖示（全採用系統預設字型與 Inline SVG），確保無外網環境下 0.1 秒內完整渲染。
- **FR-07.2 智能輪詢與省電熔斷**：每 2～3 秒發送 `GET /status` 局部更新 DOM；每次請求綁定 `AbortController`（1.5 秒超時中斷）並監聽 `document.visibilitychange`（鎖屏或切背景暫停輪詢，切回前景立即刷新）。
- **FR-07.3 獨立開關與遙控防護**：
  - 關閉「區網 Wi-Fi」前彈出確認視窗；後端回傳 `"ok": false` 時開關自動回彈並提示原因。
  - 送出 `POST /api/control` 期間暫時鎖定按鈕並作廢進行中的 `GET /status` 輪詢，收到回應後直接以 `state` 刷新畫面；偵測到 `err_seq` 遞增時彈出 3 秒 Toast 顯示 `last_err`。

---

## 4. 驗收標準（Acceptance Criteria）
1. **靜態與編譯檢查**：全專案所有 `.c`、`.h`、`.html`、`.css`、`.js` 檔案行數均 $\le 300$ 行；`idf.py build` 零錯誤、零警告；Unity 單元測試全數通過。
2. **音訊與 AEC 驗證**：連續播放 44.1 kHz 音樂 3 分鐘以上無自激喚醒，2 公尺內可隨時喊喚醒詞打斷並準確辨識指令；AI 回覆期間關閉雲端開關可於 50 ms 內中斷連線。
3. **網路與按鍵驗證**：重燒韌體保留原有 Wi-Fi 帳密；可透過 `http://xiaozhi.local` 存取遙控面板；播音期間切換開關無爆音；實體鍵短按可復原 Wi-Fi，按滿 1.5 秒立即開啟 `Xiaozhi_Setup` 熱點。
4. **前端互動驗證**：純區網模式下遙控器正常運作，重新開啟雲端時自動補傳離線數據；無 SD 卡或無音樂檔點擊播放時畫面彈出明確 Toast 提示。
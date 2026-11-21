# Xiaozhi IoT 系統優化與功能擴充任務清單（`tasks.md`）

## 執行前必讀紅線（Definition of Done 通用準則）
- **單檔行數硬性上限**：每個新增或修改的 `.c`、`.h`、`.html`、`.css`、`.js` 檔案**嚴格 $\le 300$ 行**，超出必須依 `plan.md` 拆分子檔，嚴禁省略實作細節。
- **模組零耦合**：`net_manager`、`audio_engine`、`sensor_hub` 之間禁止互相 `#include`，一律透過 `net_manager_hooks_t` 在 `main/app_bridges.c` 橋接。
- **核心與記憶體隔離**：音訊與 `ESP-AFE` 任務釘選 **Core 1**，網路與感測任務釘選 **Core 0**；`aec_ref` 熱緩衝區放**內部 SRAM（`MALLOC_CAP_INTERNAL`）**，大型錄音與解碼緩衝放 **PSRAM**。

---

## Phase 1：音訊防回音與單聲道串流優化（`audio_engine` / `voice_cloud`）

- [x] **T-OPT-01：實作全場景聲學回音消除（AEC）與 Q16.16 無漂移重採樣**
  - **依賴項目**：無（可最優先啟動）
  - **目標檔案**：
    - 新增 `components/audio_engine/include/aec_ref.h`（預估 ~60 行）
    - 新增 `components/audio_engine/aec_ref.c`（預估 ~220 行）
    - 修改 `components/audio_engine/audio_engine.c`（預估 ~260 行）
  - **執行步驟**：
    1. 在 `aec_ref.c` 使用 `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` 建立 **150～160 ms**（約 5,120 Bytes）的 16 kHz / 16-bit 單聲道 RingBuffer。
    2. 實作 Q16.16 定點數相位累加器（`step_q16 = (src_rate << 16) / 16000U`）搭配低通內插抗混疊濾波，將 44.1 kHz / 24 kHz 音源無漂移降採樣為 16 kHz 單聲道。
    3. 實作播放啟動預填 **30 ms** 靜音補償、緩衝水位 **$> 130\text{ ms}$** 平滑丟棄最舊樣本，以及 `aec_ref_flush()` 即時清空函式。
    4. 修改 `audio_engine.c`：將主音訊任務釘選於 **Core 1**；軟體最大音量上限限制於 **85%～90%**；將 I2S TX DMA 總緩衝調大至 **60～80 ms**；在**套用軟體音量之後、緊貼 `i2s_channel_write()` 前一刻**將雲端 AI 回覆、SD 卡音樂與離線拼音播報寫入 `aec_ref`。
    5. 啟用 `ESP-AFE` **2-Mic + 1-Ref** 模式（`aec_filter_length = 2`），喇叭無播放時自動補 `0`（靜音樣本）湊齊幀長餵入 `feed()`；停止播放或偵測喚醒詞打斷（Barge-in）時立即呼叫 `aec_ref_flush()`。
  - **驗收標準**：
    - 單檔行數皆 $\le 300$ 行。
    - 待機喚醒靈敏度正常；連續播放 44.1 kHz 音樂 3 分鐘以上無自激喚醒，2 公尺內喊喚醒詞可即時打斷且後續指令辨識無失真。

- [x] **T-OPT-02：雲端語音上傳改為單聲道（含 AGC 與 HTTP 串流即時中斷）**
  - **依賴項目**：`T-OPT-01`
  - **目標檔案**：
    - 修改 `components/audio_engine/audio_engine.c`
    - 修改 `components/net_manager/voice_cloud.c`（預估 ~230 行）
    - 更新對應 Unity 單元測試檔
  - **執行步驟**：
    1. 在 `ESP-AFE` 初始化設定中明確啟用 **`agc_init = true`**，將雙 INMP441 降噪融合後的 **16-bit 單聲道（1-Channel）** PCM 存入 PSRAM 上傳緩衝。
    2. 修改 `voice_cloud.c` HTTP Request Header 為 `X-Channels: 1`，並更新 Unity 單元測試斷言。
    3. 在 `voice_cloud.c` 每次讀寫 HTTP Chunk 迴圈中加入原子旗標 `abort_flag` 檢查，當觸發 Barge-in 打斷或網頁關閉 `cloud_enabled` 時，於 **$< 50\text{ ms}$ 內執行 `esp_http_client_close()`** 並釋放資源。
    4. 將 HTTP 連線與讀取 `timeout_ms` 設為 **$\le 3000\text{ ms}$**，斷外網時快速失敗並轉播本機離線提示音。
  - **驗收標準**：
    - 上傳資料量減半，遠（2 公尺）近（30 公分）距離語音辨識準確率穩定。
    - AI 回覆播報期間觸發打斷或關閉雲端開關，可在 50 ms 內中斷 HTTP 連線；Unity 單元測試全數通過。

---

## Phase 2：網路雙獨立開關、mDNS 與實體按鍵重構（`net_manager`）

- [x] **T-OPT-03：實作「區網 Wi-Fi」與「雲端伺服器」雙獨立開關狀態機與 mDNS**
  - **依賴項目**：無（可與 Phase 1 平行開發）
  - **目標檔案**：
    - 修改 `components/net_manager/include/net_manager.h`（預估 ~160 行）
    - 修改 `components/net_manager/net_manager_priv.h`（預估 ~95 行）
    - 新增 `components/net_manager/net_mode_ctrl.c`（預估 ~240 行）
    - 修改 `components/net_manager/net_manager.c`（瘦身至 ~230 行）
  - **執行步驟**：
    1. 在 `net_manager.h` 定義 `wifi_conn_state_t`（`OFF` / `AP_SETUP` / `CONNECTING` / `CONNECTED`）並宣告雙開關控制 API；保持既有 `sys_config_t` Blob 結構不變。
    2. 在 `net_mode_ctrl.c` 實作獨立 NVS `u8` 鍵值 `"wifi_en"` 與 `"cloud_en"` 讀寫（預設值皆為 `1`）；切換開關時先更新 RAM 並設 `s_nvs_dirty = true`，主迴圈延遲 **1 秒防抖且確認 `!hooks.is_audio_busy()`** 時才執行 `nvs_commit()`。
    3. 實作三種組合狀態與生命週期管理：
       - **Wi-Fi 開 ＋ 雲端開**：連上路由器、保持 `web_config_start()` 運作、允許雲端連線，OLED 顯示 `192.168.x.x`。
       - **Wi-Fi 開 ＋ 雲端關**：連上路由器、保持 `web_config_start()` 運作、停止 `server_url` 連線，OLED 顯示 `192.168.x.x [LAN]`。
       - **Wi-Fi 關**：進入 `WIFI_MODE_NULL` 前先優雅停止 `httpd` 與 `mdns`。
    4. 處理狀態連動：`/save` 表單儲存有效 Wi-Fi 帳密後自動開啟 `wifi_en`（及有填網址時的 `cloud_en`）；僅在 `wifi_en && !is_ap_mode` 時允許 STA 斷線自動重連；`cloud_en` 由 `false` 轉 `true` 且 STA 已連線時主動觸發本機暫存數據補傳與健康檢查；網頁關閉 Wi-Fi 時延遲 **150～200 ms** 再執行關閉以確保 `200 OK` 送達前端。
    5. 在 `IP_EVENT_STA_GOT_IP` 時啟動／更新 `mdns` 註冊主機名稱 `xiaozhi`（支援 `http://xiaozhi.local`）。
  - **驗收標準**：
    - `net_manager.c` 與 `net_mode_ctrl.c` 行數皆 $\le 300$ 行。
    - 重燒韌體保留原有 Wi-Fi 帳密；播音期間切換開關無爆音；反覆開關 Wi-Fi 無 Socket 記憶體洩漏；手機可透過 `http://xiaozhi.local` 連線。

- [x] **T-OPT-04：重構實體按鍵為「1.5 秒單一分界點（短按開關 Wi-Fi／長按開熱點）」**
  - **依賴項目**：`T-OPT-03`
  - **目標檔案**：
    - 修改 `components/net_manager/net_mode_ctrl.c`
    - 修改 `components/net_manager/net_manager.c`
  - **執行步驟**：
    1. 移除原有三模式循環切換與 1.5s～2.0s 時間死區。
    2. 開機初始化若偵測按鍵壓下，直接進入 `WIFI_STATE_AP_SETUP`（`Xiaozhi_Setup`）。
    3. 運行期按住滿 **$\ge 1500\text{ ms}$ 當下立即觸發**強制開啟 `Xiaozhi_Setup` SoftAP（`192.168.4.1`），強制設 `wifi_en = true`、`wifi_state = WIFI_STATE_AP_SETUP`，並設 `long_press_handled = true` 防止放開時誤觸短按。
    4. 放開按鍵時若 `!long_press_handled` 且按下時間 $\ge 50\text{ ms}$（防彈跳），切換 `wifi_en = !wifi_en`。
    5. 對齊 `net_manager_set_mode()` 與離線語音指令（`VOICE_CMD_WIFI_ON` / `VOICE_CMD_WIFI_OFF` / `VOICE_CMD_AP_MODE`）。
  - **驗收標準**：
    - 網頁關閉 Wi-Fi 後，短按實體鍵可立即恢復連網；按滿 1.5 秒當下即強制開出 `Xiaozhi_Setup` 熱點且放開不重複觸發；相關 Unity 單元測試通過。

---

## Phase 3：跨模組橋接、Web API 與前端面板升級（`main` / `web_config` / `web_api`）

- [x] **T-OPT-05：實作 `net_manager_hooks_t` 高內聚快照與執行緒安全控制橋接**
  - **依賴項目**：`T-OPT-01`、`T-OPT-03`
  - **目標檔案**：
    - 修改 `components/net_manager/include/net_manager.h`
    - 修改 `main/app_bridges.c`（預估 ~210 行）
  - **執行步驟**：
    1. 在 `net_manager.h` 定義 `sys_status_snapshot_t`、`ctrl_cmd_id_t`（`VOL_UP`、`VOL_DOWN`、`MUTE_TOGGLE`、`MUSIC_TOGGLE`、`SPEAK_ENV`），並在 `net_manager_hooks_t` 加入 `get_sys_status`、`exec_ctrl_cmd`、`is_audio_busy`。
    2. 在 `main/app_bridges.c` 實作 `get_sys_status`：彙整 `sensor_hub` 溫濕光照、`audio_engine` 音量與音樂狀態，以及非同步錯誤計數器 `err_seq` 與 `last_err`。
    3. 在 `main/app_bridges.c` 實作 `exec_ctrl_cmd`：Core 0 `httpd` 呼叫時僅將指令推入 FreeRTOS Queue 或設原子旗標，交由 Core 1 `audio_engine` 任務安全執行；執行 `SPEAK_ENV` 前若正在播放 SD 卡音樂，先安全停止音樂並釋放解碼器；若執行失敗（如 SD 卡未就緒或無音樂檔），遞增 `err_seq` 並寫入 `last_err`。
  - **驗收標準**：
    - 編譯零循環依賴，所有修改檔案 $\le 300$ 行。
    - 播音樂時從網頁連點「語音播報環境」或「停止音樂」絕不發生雙核心競爭當機或破音。

- [x] **T-OPT-06：拆分並實作 `web_api.c` 控制路由與 HTTPD 並發調校**
  - **依賴項目**：`T-OPT-03`、`T-OPT-05`
  - **目標檔案**：
    - 新增 `components/net_manager/web_api.h`（預估 ~40 行）
    - 新增 `components/net_manager/web_api.c`（預估 ~220 行）
    - 修改 `components/net_manager/web_config.c`（瘦身至 ~220 行）
    - 修改 `components/net_manager/CMakeLists.txt`
  - **執行步驟**：
    1. 在 `web_config.c` 將 `httpd_config_t` 設為 `max_open_sockets = 7`、`lru_purge_enable = true`、`max_uri_handlers = 12`，並為靜態資源加上 `Cache-Control` 標頭。
    2. 在 `web_api.c` 實作 `GET /status`，回傳包含感測數值、音量、音樂狀態、`wifi_en`、`cloud_en`、`wifi_state`、`err_seq`、`last_err` 的完整 JSON。
    3. 在 `web_api.c` 實作 `POST /api/control`，解析 `cmd` 與 `val`；若欲開啟雲端（`set_cloud = 1`）但 `server_url` 為空，拒絕操作並回傳 `{"ok": false, "msg": "請先於下方設定伺服器網址", "state": {...}}`；成功時執行指令並直接附帶最新 `state` JSON 回傳。
  - **驗收標準**：
    - `web_config.c` 與 `web_api.c` 各自 $\le 300$ 行。
    - `/status` 與 `/api/control` 回應時間 $< 100\text{ ms}$，多檔並發載入不發生 Socket 耗盡。

- [ ] **T-OPT-07：重構前端為「即時監控＋遙控面板」（零外部依賴、防連發與超時熔斷）**
  - **依賴項目**：`T-OPT-06`
  - **目標檔案**：
    - 修改 `components/net_manager/index.html`（預估 ~150 行）
    - 新增 `components/net_manager/style.css`（預估 ~160 行）
    - 新增 `components/net_manager/app.js`（預估 ~210 行）
    - 修改 `components/net_manager/CMakeLists.txt`（將三檔加入 `EMBED_TXTFILES`）
  - **執行步驟**：
    1. **零外部依賴**：移除任何外部 CDN 字型或圖示，全數採用系統預設字型與 Inline SVG，確保離線熱點環境 0.1 秒內完整渲染。
    2. **智能輪詢與省電熔斷**：在 `app.js` 每 2.5 秒發起 `GET /status`，每次綁定 `AbortController`（**1.5 秒超時中斷**），並監聽 `document.visibilitychange`（手機鎖屏／切背景暫停輪詢，切回前景立即刷新）。
    3. **獨立雙開關與錯誤回彈**：建立「區網 Wi-Fi」與「雲端伺服器」開關；關閉 Wi-Fi 前跳出 `confirm()` 確認視窗；後端回傳 `"ok": false` 時自動將開關回彈並顯示錯誤提示。
    4. **快捷遙控、防蓋台與 Toast 提示**：建立 `音量 +`、`音量 -`、`靜音`、`播放／停止音樂`、`語音播報環境` 按鈕；利用請求世代計數器 `reqGen` 作廢進行中的輪詢並鎖定按鈕直到 `POST /api/control` 回應；偵測到 `err_seq > lastSeenErrSeq` 時彈出 3 秒 Toast 顯示 `last_err`。
  - **驗收標準**：
    - `index.html`、`style.css`、`app.js` 各自 $\le 300$ 行。
    - 手機連上 `Xiaozhi_Setup` 熱點或 `http://xiaozhi.local` 版面秒開正常，快速連點按鈕數值不回跳，無 SD 卡時有明確 Toast 提示。

---

## Phase 4：全專案自動化檢查與實機整合驗收

- [ ] **T-OPT-08：執行行數稽核、編譯測試與七大情境實機驗收**
  - **依賴項目**：`T-OPT-01` ～ `T-OPT-07` 全部完成
  - **執行步驟**：
    1. 執行行數檢查指令確認專案內所有 `.c`、`.h`、`.html`、`.css`、`.js` 檔案皆 $\le 300$ 行。
    2. 執行 `idf.py build` 與 Unity 單元測試，確認零錯誤、零警告。
    3. 完成以下 **7 項實機整合測試**：
       - [ ] **情境 1（無痛升級與 mDNS）**：重燒韌體確認原有 Wi-Fi 帳密與伺服器網址保留，手機可透過 `http://xiaozhi.local` 開啟遙控面板。
       - [ ] **情境 2（無漂移 AEC 與打斷）**：連續播放 44.1 kHz 音樂 3 分鐘以上無誤喚醒，2 公尺內喊喚醒詞可即時打斷並準確辨識後續指令。
       - [ ] **情境 3（串流即時中斷）**：AI 回覆長語音期間於網頁關閉「雲端伺服器」或喊打斷，驗證 50 ms 內中斷 HTTP 連線。
       - [ ] **情境 4（防 Flash 凍結爆音）**：播放音樂期間反覆切換網頁開關，確認延遲避播音 `nvs_commit` 期間音訊零爆音。
       - [ ] **情境 5（音訊搶占與 Toast 提示）**：播音樂時點擊「語音播報環境」驗證平滑停止音樂並播報；拔除 SD 卡點擊播放驗證網頁彈出錯誤 Toast。
       - [ ] **情境 6（純區網模式與自動補傳）**：關閉雲端開關後確認區網網頁遙控正常且感測數據存本機；重新開啟雲端後確認自動補傳離線數據。
       - [ ] **情境 7（實體按鍵 1.5s 邏輯）**：網頁關閉 Wi-Fi 後短按實體鍵（$< 1.5\text{s}$）立即恢復連網；按滿 $1.5\text{s}$ 當下強制開出 `Xiaozhi_Setup` 熱點且放開不重複觸發。
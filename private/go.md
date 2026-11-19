一、 標準單任務施工範本（日常推進用）

請嚴格閱讀 spec.md、plan.md 與 tasks.md，現在只實作 [T-00-03]。

強制要求：

嚴格遵守 plan.md 的 CMakeLists.txt 依賴與 .h 標頭檔簽章，禁止修改任何 .h 介面。

僅使用純 ESP-IDF v6.1.1（C11） 新版驅動 API（如 i2c_master.h、i2s_std.h、esp_rom_sys.h、esp_sntp.h），禁止使用 Arduino 或舊版棄用 API，且不得留下 TODO 空殼。

完成後執行 idf.py build 確認零錯誤編譯通過，並貼出最後 5 行編譯結果。

將 tasks.md 中對應的任務改為 - [x] 後立即停止，禁止偷跑下一個任務。




二、 每個 Phase 結尾的「單元測試驗收範本」（遇到各階段最後一題用）

請嚴格閱讀 spec.md、plan.md 與 tasks.md，現在只實作 [T-00-04]。

強制要求：

在對應元件的 test/ 目錄下撰寫完整的 Unity 單元測試（使用 TEST_CASE 巨集），完整覆蓋該階段的邊界與硬體紅線條件。

確認 main/CMakeLists.txt 的 WHOLE_ARCHIVE 機制能正確連結該測試元件，確保開機按住模式鍵（GPIO 1）超過 300ms 進入 unity_run_menu() 時看得到此測試項目。

執行 idf.py build 確認零錯誤編譯通過，貼出編譯結果與所有新增的 TEST_CASE 名稱列表。

將 tasks.md 中對應的任務改為 - [x] 後立即停止。




三、 遇錯修正範本（萬一編譯報錯時用）
執行 [填入 Task ID] 時發生以下編譯錯誤，請依據 spec.md 與 plan.md（ESP-IDF v6.1.1）定位根本原因並修正（禁止修改 .h 標頭檔簽章與隨意降級功能），修正後重新執行 idf.py build 驗證通過再回報：
[貼上終端機的 error 訊息]




我剛切換模型接手開發，請先閱讀 spec.md、plan.md、tasks.md 以及已完成的 components/board_bsp/ 對齊程式碼風格，然後嚴格依照規則只實作 T-01-01，完成後執行 idf.py build 驗證並將 tasks.md 對應項目打勾後停止。


開啟終端機
ESP-IDF: Open ESP-IDF Terminal
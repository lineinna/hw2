# HW2 實作規劃

> 本文件是依據 `System Programming HW2.pdf`、`tests/public/points.toml`、
> 公開測試案例與目前的 `sp.c/sp.h` 模板整理出的開發清單。  


## 1. 評分導向的總體策略

| 大步驟 | 對應評分群組 | 分數 | 目標 |
|---|---:|---:|---|
| A. 單一 SP main 的本地 key-value store | Group 1 | 7 | `store/delete/fetch/kill` 在 root 上正確工作 |
| B. 建立子程序與 ancestor lookup | Group 2 | 21 | `create`、FIFO、父子程序通訊、向上查找與向下回覆 |
| C. Consultant 與 update | Group 3 | 29 | root miss 執行 consultant、失敗重試、切換 consultant |
| D. 完整 subtree kill、並行、長資料與資源衛生 | Group 4 | 43 | 非同步協定、完整清理、資源限制與壓力情境 |

公開測試只占每組的 30%，通過公開測試不代表私有測試一定通過。實作時應先完成
前一組的穩定基礎，再擴充下一組；每組都要保留前面所有行為。

## 2. 先建立整體設計（所有功能的共同基礎）

### A-1. 確認協定與執行模型

- 區分兩種程序狀態：
  - **SP main**：由 judge 啟動，從標準輸入讀命令。
  - **node**：由父 node `fork()` 後 `exec()` 同一個 `sp`，從自己的 command FIFO
    讀命令並寫 reply FIFO。
- 先定義命令 parser，能正確辨識：
  - `create <command-fifo> <reply-fifo>`
  - `store <key> <value>`
  - `delete <key>`
  - `fetch <hash> <key>`
  - `kill`
  - `update <consultant-path>`
- parser 必須支援動態長度輸入；不要假設 key/value 可以放進固定大小 buffer。
- STDOUT 是 judge 的 protocol channel；除規格要求的 fetch reply 外，不要輸出診斷文字。
  - 診斷資訊應寫到 STDERR，並在提交前移除不必要輸出。

### A-2. 設計本地資料結構

- 使用動態配置的 key-value table。
- `store` 對同 key 做 replace。
- `delete` 對不存在的 key 不報錯、不影響其他 key。
- 每個 node 都有獨立 table；不能用 shared memory、檔案或跨程序 cache 保存 value。
- 所有 allocation、reallocation、字串複製與釋放都要檢查錯誤。

### A-3. 設計父子程序通訊資料結構

- 為每個 node 保存：
  - 子程序 PID。
  - 與每個 child 的父子 protocol pipe/file descriptor。
  - child 的 command/reply FIFO fd 與路徑（若由該 node 建立）。
  - child 的存活與 kill 狀態。
- 父子 protocol 必須能同時承載多個 fetch request，不能只保存一個「目前查詢」。
- 每一筆 fetch request 要有可辨識的 hash 與回傳方向資訊；hash 在 concurrent fetch
  中不重複，適合作為 request correlation id。
- 建立 fd ownership 表：每次 `pipe/fork/open` 後，明確列出 parent/child 各自要保留與關閉
  的 fd，避免 EOF 永遠不出現。

## 3. 大步驟 A：完成 Group 1（7 分）

### A-1. 啟動 SP main 與輸入迴圈

- 驗證 `argc`；錯誤用法印到 STDERR 並回傳非零。
- 初始化 root 的 local table、事件迴圈與清理狀態。
- 逐行讀 STDIN，完整讀取後再 parse 與執行。
- 對空行、過短命令、allocation/read 失敗提供明確錯誤處理，不可 silent fallback。

### A-2. 實作 `store` 與 `delete`

- `store main k v` 將 pair 放入 root table。
- 重複 store 必須覆蓋舊 value。
- `delete` 移除指定 key，並釋放 key/value 記憶體。
- 這些命令不需要回覆 judge，但不能阻塞後續命令。

### A-3. 實作 root-only `fetch`

- 先查 receiving node 的 local table。
- 命中時輸出 `<hash> <value>`，只輸出一行有效 reply。
- Group 1 沒有 miss，但 reply 格式、hash 配對與 flush 行為要先固定。

### A-4. 實作 root `kill`

- 結束 root event loop。
- Group 1 先確保沒有 child/consultant 時能正常釋放 table、fd 與記憶體。
- root 由 judge kill 時必須以 status 0 結束。

### A-5. Group 1 驗收

- 執行 `make test`，先確認 `single_node/case-0` 到 `case-4`。
- 特別驗證：
  - `case-0`：store/fetch。
  - `case-1`：store replace。
  - 其他案例：delete、長時間命令順序與重複執行。
- 只有 Group 1 全部穩定後才進入 fork/FIFO；否則後續問題難以定位。

## 4. 大步驟 B：完成 Group 2（21 分）

### B-1. 實作 `create`

- 在收到 `create` 的 node 中 `fork()`，且每次 create 恰好建立一個 direct child。
- child 以 `exec()` 啟動同一個 `sp`，不要建立另一份不同的 node 程式。
- 透過啟動參數或環境變數讓 child 知道：
  - 它是 node 而不是 SP main。
  - command FIFO 路徑。
  - reply FIFO 路徑。
  - 必要的父子 protocol fd。
- child 必須先對兩個 FIFO 呼叫 `mkfifo()`，再開啟 FIFO；路徑是規格給定的絕對路徑。
- FIFO 可在成功開啟兩端後立即 unlink，也可在 kill 時 unlink，但不能留下路徑。
- FIFO 與 protocol fd 使用 `O_CLOEXEC` 或明確設定 `FD_CLOEXEC`，避免後續 create/consultant
  繼承不應持有的 fd。
- 父程序在 child 啟動失敗時要處理錯誤並回收 PID/fd，不可留下半初始化 node。

### B-2. 完成 node 的輸入輸出

- node 從自己的 command FIFO 讀命令，回覆寫到自己的 reply FIFO。
- parent/child protocol 與 judge FIFO 分開管理，不能把內部訊息寫到 judge reply channel。
- 正確關閉每個 fork 後不使用的 pipe end；父程序要能觀察 child EOF。
- 每個 node 都必須能持續接收命令，不能因一個 fetch 阻塞而停止讀其他命令。

### B-3. 實作向上查找

- local hit：直接產生 fetch result。
- local miss：
  - 非 root node 將 request 傳給 parent。
  - parent 先查自己的 local table，再決定回覆或繼續往上。
- nearest ancestor wins：離 request node 最近的 ancestor 命中時不能繼續問更上層。
- 不得 cache ancestor 或 consultant 回傳的 value；每次 miss 都要重新走完整查找。

### B-4. 實作向下回覆

- request 往上傳時保留原始 hash 與來源 child/route。
- value 回來時依 route 逐層往下傳，最後只有最初接收 fetch 的 node 寫
  `<hash> <value>` 到 judge reply FIFO。
- 中間 node 不可在自己的 reply FIFO 額外輸出 reply。
- 多個 request 可同時在同一條 parent-child path 上流動，不能用單一全域 buffer 覆蓋狀態。

### B-5. Group 2 驗收

- 執行 `tree_lookups/case-0` 到 `case-4`。
- 先驗證單層，再驗證多層：
  - child 找到 parent 的 value。
  - 三層樹找到最近 ancestor。
  - 同一路徑多個 fetch 同時進行仍能以 hash 正確配對。
- 每個測試結束後確認 process tree 沒有非預期 child 或未關閉 fd。

## 5. 大步驟 C：完成 Group 3（29 分）

### C-1. 實作 consultant runner

- SP main 以 `fork()` + `exec()` 啟動目前 consultant，且 consultant 是 SP main 的 direct child。
- 以兩條 pipe 連接 consultant STDIN/STDOUT；不要使用 `system()` 或 `popen()`。
- 寫入 `<key>\n` 後讀一整行回覆。
- 解析：
  - `Ok: <value>`：取得 value。
  - `Err: <reason>` 或未完成一整行/提早退出：視為本次失敗。
- 每次 consultant run 結束都要 `waitpid()`、關閉 fd、釋放暫存資料。
- consultant 失敗時，用目前 consultant 對同一 key 重新 fork/exec；成功前不得回覆 fetch。
- 多個 root miss 可以同時執行 consultant，因此 consultant state 不能只有單一全域欄位。

### C-2. 將 root miss 接到 consultant

- fetch 在整棵 ancestor chain 都 miss 後，才由 SP main 執行 consultant。
- consultant 的 value 只用來回答目前 request，不放進任何 node table 或 cache。
- 回覆仍必須沿原 request route 回到最初接收 fetch 的 node。
- 每一個 miss 都必須獨立執行 consultant；同 key 也不可重用上一輪結果。

### C-3. 實作 `update`

- 只接受 SP main 收到的 update；child 收到時依設計拒絕或回報 invalid command，不得改變 root consultant。
- 用 `stat()`/等價方式驗證 path 是 regular file 且具 execute permission。
- 有效 path 從「下一個 consultant run」開始生效。
- 無效 path 忽略，保留目前 consultant。
- 已經執行中的 consultant 不受 update 影響，讓它正常結束。

### C-4. Group 3 驗收

- 執行 `consultant/case-0` 到 `case-4`。
- 確認：
  - root miss 會執行一次 consultant。
  - child miss 會一路到 root 後才執行 consultant。
  - `Err`/不完整回覆會重試，而且每次重試都有新的 child process 並被 reap。
  - 有效 update 只影響後續 miss；無效 update 不會破壞現有 consultant。
  - 同時 miss 不會互相覆蓋 request 或 consultant 的 pipe。

## 6. 大步驟 D：完成 Group 4（43 分）

### D-1. 將 fetch 改為真正非同步

- 主事件迴圈必須同時處理：
  - judge/stdin 或 node command FIFO。
  - parent/child protocol pipe。
  - consultant pipe。
  - 已結束 child 的 reaping。
- 可使用 `poll()`/`select()`/`epoll()` 或等價設計，但不能以單一 blocking fetch
  阻塞整個 node。
- 所有 stream 都要處理 partial read、partial write、EOF、EINTR 與錯誤。
- 2 MB 級 value 要能完整傳遞；不要依賴一次 `read()`/`write()` 完成整行。
- 協定訊息需有 framing（例如長度前綴或明確 newline parser），避免多筆訊息黏在一起或被切開。

### D-2. 實作 subtree `kill`

- kill receiving node 時，先要求所有 descendants 結束，再由 child parent 逐層 reaping。
- 可用關閉 parent-to-child pipe 讓 child 看到 EOF，也可使用自訂 shutdown message；
  但每個 process 都要有明確的終止狀態。
- child 的終止順序：
  1. 通知/關閉所有 children。
  2. 等待並 `waitpid()` 所有 children。
  3. 關閉自身 fd。
  4. unlink 自己建立的 FIFO。
  5. 正常退出。
- parent 看到 child 結束要立即 `waitpid()`，不能等下一個 judge command 才 reap。
- root kill 還要等所有 consultant runs 結束並 reaped，然後 root 以 status 0 結束。
- kill 期間不能留下 process、zombie、FIFO path 或持有 pipe write end 的 stray descriptor。

### D-3. 資源與錯誤衛生

- fork/exec/pipe/open/mkfifo/read/write/poll/waitpid/stat/malloc/realloc/unlink
  全部檢查回傳值。
- 失敗時用 `perror()` 或 `strerror(errno)` 提供原因；不可用成功形狀的 fallback 掩蓋錯誤。
- 每次 fork 後立即關閉不屬於該 process 的 fd。
- 以 CLOEXEC 防止新 node/consultant 繼承不相關 fd。
- 動態資料在 request 完成、consultant 結束、node kill 與 process cleanup 時釋放。
- 避免不必要的 process、buffer 複製與 busy loop，以符合 process/fd/memory/CPU limits。

### D-4. Group 4 驗收

- 執行 `kill_and_resources/case-0` 到 `case-4`：
  - leaf kill 後 parent 仍可服務。
  - kill subtree 會移除整個子樹。
  - kill 後重新使用相同 FIFO path 的 node 必須是空 table。
  - 2,000,000 字元 value 能從 ancestor 傳到深層 node。
  - 32-node tree 能完成 fetch 並完整清理。
- 反覆執行測試，觀察偶發 race、timeout、zombie 與 leftover FIFO。
- 檢查 judge 設定的 process、open fd、memory、CPU soft/hard limits。

## 7. 整合測試與提交前清單

### 7-1. 測試流程

1. `make clean`
2. `make`
3. `make test`
4. 逐組排查失敗，不要在 Group 1 未穩定時直接改 Group 4。
5. 對重複執行的案例多跑幾次，確認不是只通過單一 timing。
6. 用 STDERR 留存必要診斷，確認 STDOUT 沒有 debug noise。

### 7-2. Makefile 與檔案

- `make` 必須編譯所有 `.c/.h` source。
- `make clean` 必須移除所有 build 產物。
- 若拆成多個 source/header，要同步更新 `SUBMISSION_FILES` 與 build rule。
- 不提交 binary、object file、judge、FIFO、core dump、測試產物或其他 generated files。

### 7-3. Submission 驗證

- 設定正確 student ID 後執行 `make submission STUDENT_ID=<student-id>`。
- 檢查 zip 內檔案位於頂層，不要多包一層資料夾。
- zip 至少包含 `Makefile`、`sp.c`、`sp.h` 及所有必要的額外 source/header。
- 在乾淨暫存目錄解壓後重新執行 `make` 與 `make clean`。

## 8. 建議的實作里程碑

- [ ] M1：parser、dynamic local table、root `store/delete/fetch`。
- [ ] M2：root kill、錯誤處理與 Group 1 全部通過。
- [ ] M3：create、exec、FIFO 建立/開啟/清理。
- [ ] M4：單一 ancestor lookup 與 route-based reply。
- [ ] M5：多個 pending fetch 與 Group 2 全部通過。
- [ ] M6：consultant fork/exec、Ok/Err parsing、retry。
- [ ] M7：update validation 與 Group 3 全部通過。
- [ ] M8：partial I/O、長資料、非同步事件處理。
- [ ] M9：subtree kill、reaping、FIFO/fd cleanup。
- [ ] M10：Group 4 壓力測試、resource limits 與 submission dry run。

## 9. 最容易失分的風險

- 把 fetch 寫成同步 blocking call，造成同一 node 無法處理第二個 fetch。
- 用固定大小 buffer，長 key/value 或 partial read/write 時截斷資料。
- 將 consultant result cache 起來，違反每次 miss 都要重新查詢。
- consultant failure 沒有重新啟動，或沒有 reap 失敗的 consultant。
- `fork()` 後遺漏關閉某個 pipe write end，導致 EOF 不出現、kill timeout。
- child 不是建立它的 node 的 direct child，或 create 多/少建立 process。
- kill 只終止目標 node，沒有遞迴處理 descendants、FIFO 與 zombie。
- 將 debug 訊息寫到 STDOUT，破壞 judge protocol。
- 只跑一次測試；規格中的部分測試會重跑以捕捉 timing-dependent bug。

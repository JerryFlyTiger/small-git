# small_git 設計文件

## 專案定位

紮實可用的 git 替代品(不是純教學玩具)。以純 C 撰寫,目標是解決以下四類實際痛點,同時維持與
git 物件格式的完全相容,讓 `.git` 目錄可被真正的 `git`/`libgit2`/GitHub 等既有生態系工具正常讀取。

優先解決的痛點(依開發順序排列,但彼此有交疊):

1. **CLI/UX 混亂難懂** — checkout 多重角色、staging area 難理解、錯誤訊息不友善
2. **危險操作與救援機制** — force push / reset --hard 造成資料遺失、reflog 不易發現
3. **大型/二進位檔案處理** — 無內建 dedup、大檔案讓 repo 肥大
4. **巨型 repo 效能** — status/checkout/clone 在大型 monorepo 上變慢

## 核心工程原則

- **相容優先於創新**:物件模型(blob/tree/commit/tag)、pack 格式、index 格式一律與 git 相容。
  新功能以「相容模式為預設、進階功能為選配」的方式加入,絕不破壞既有 `.git` 目錄的可讀性。
- **資料安全 > 效能 > 便利**:任何可能造成資料遺失的操作,預設要有救援路徑。
- **允許使用基礎系統庫**:zlib(壓縮)、OpenSSL EVP(SHA-1/SHA-256)。不重造壓縮/雜湊演算法輪子,
  把工程心力放在架構與 UX 差異化上。
- **可驗證的相容性**:每個里程碑都要有「small_git 寫、真正的 git 讀」以及反過來的 round-trip 測試。

## 高層架構

```
small_git/
  src/
    object/    blob / tree / commit / tag 的序列化與解析
    storage/   loose object 讀寫、packfile 讀寫、refs、index
    index/     staging area(.git/index)讀寫與比對
    workdir/   working tree 掃描、status、checkout、diff
    cli/       command dispatch、porcelain 指令實作
    safety/    自動快照、undo/recovery、危險操作守門
    util/       hash(SHA-1 wrapper)、zutil(zlib wrapper)、path、mmap 工具
  include/sg/  對外(模組間)共用標頭
  tests/       單元測試 + interop 測試(呼叫系統 git 驗證)
  docs/        設計文件、路線圖
```

不做 library/CLI 分離的過度設計;先以單一 `sg` 執行檔為主,內部模組化,未來若有需要再拆 libsg。

## 各痛點的對應設計

### 1. CLI/UX 混亂難懂
- 從第一天就把 `checkout` 拆成 `sg switch`(切換分支)與 `sg restore`(還原檔案),不重蹈 git 早期
  的多重角色設計。
- `sg status` 預設輸出更明確的下一步建議(例如衝突時列出具體指令而非只列檔名)。
- 所有可能有歧義的指令,錯誤訊息要包含「你可能想要的指令」。

### 2. 危險操作與救援機制
- 破壞性操作(reset --hard、branch -D、force push 等價操作)執行前,自動在
  `refs/small-git/undo/<timestamp>` 建立快照 ref,不需使用者手動操作。
- 提供 `sg undo`:列出最近的自動快照並可一鍵還原,比 reflog 更好發現、更好用。
- force-push 類操作預設要求二次確認,並在可能覆蓋遠端非快進歷史時列出將遺失的 commit。

### 3. 大型/二進位檔案處理
- 早期(相容模式):大檔案仍以標準 git blob 儲存,只求正確可用。
- 進階(選配):以內容切割(content-defined chunking, rolling hash)將大 blob 拆成 chunk,
  透過小型 pointer blob 參照,checkout 時透明還原,概念類似內建、不需外部 server 的 Git LFS。
  可選擇輸出與 Git LFS pointer 相容的格式,與既有 LFS repo 互通。

### 4. 巨型 repo 效能
- object 存取採用 mmap,避免大檔案整檔讀入記憶體。
- 支援 git 的 commit-graph 檔案格式,加速 log/merge-base 等歷史走訪。
- 後期支援 partial clone / sparse checkout(需先完成 smart protocol v2 client)。

## 分階段路線圖

| Phase | 內容 | 交付驗證方式 |
|---|---|---|
| 0 | 專案骨架:build system、目錄結構、測試框架、CI | `make && make test` 可執行 |
| 1 | 物件模型:SHA-1/zlib wrapper、loose object 讀寫、`sg init`/`hash-object`/`cat-file` | small_git 寫的 blob 能被系統 `git cat-file -p` 讀出;反之亦然 |
| 2 | Index + 基礎 porcelain:`add`/`status`/`commit`/`log`/`diff`、refs、`switch`/`restore` | 在 small_git 建立的 repo,`git log`/`git status` 可正常運作 |
| 3 | UX 差異化層:友善錯誤訊息、guided status、危險操作二次確認 | 手動 UX walkthrough |
| 4 | 救援機制:自動快照 ref、`sg undo`、merge/rebase 與衝突 UX | 模擬誤操作後可 100% 復原 |
| 5 | Packfile 與網路互通:pack reader/writer、delta 壓縮、smart HTTP v2 client(clone/fetch/push) | 可 clone 真實 GitHub repo、可 push 上去 |
| 6 | 大檔案 chunking(選配功能) | 大檔案 repo 體積與 checkout 時間對比 |
| 7 | 巨型 repo 效能:commit-graph、multi-pack-index、平行化 | 大型 repo 上 status/log 效能 benchmark |
| 8 | 文件、打包、跨平台收尾 | README、man page、release build |

## 開放技術決策(隨開發過程確認)

- 雜湊演算法預設 SHA-1(相容 git 舊格式),SHA-256 repo 支援排入 Phase 5 之後視需求評估。
- Build system 採 Makefile(維持「small」精神),不引入 CMake,除非未來需要更複雜的跨平台矩陣。
- 測試框架:輕量自製 assert-based test runner,避免引入大型測試框架依賴。

## Phase 6:大檔案內容分塊(CDC)—— 已知限制

分塊功能**預設關閉**(`.git/config` 的 `[sg] chunking`)。未啟用時,repo 與 git 的相容性完全不變。

啟用後的取捨與已知限制:

- **真正的 `git` 讀不到內容**。tree 裡放的是指標 blob,`git checkout` 會拿到指標文字而非檔案內容。這與 Git LFS 在未安裝 LFS 的環境下的表現相同,是啟用者接受的代價。`sg` 之間(clone/fetch/push)則完全正常。
- **`refs/sg/chunks` 不可刪除**。所有 chunk 靠這個 keep-alive ref 在 git 的物件圖中保持可達,否則 `git gc` 會清掉它們。刪掉它之後,`sg` 會對每個分塊檔案硬失敗(不會靜默寫出指標文字),但資料可能已經無法復原。
- **並發保護僅限單機**。`keep_alive_add` 用 `.git/sg-chunks.lock` 檔案鎖序列化同一個 repo 內的並發 `sg add`。跨機器共享檔案系統的情境未經測試。
- **push 的原子性依賴伺服器**。`sg push` 在遠端宣告 `atomic` 能力時會請求它,讓分支 ref 與 `refs/sg/chunks` 綁成單一事務。伺服器不支援時,理論上仍可能出現「分支更新成功、keep-alive ref 被拒」的部分套用;此時 `sg push` 會明確報錯(不會靜默成功),但在使用者重試前,遠端若剛好執行 `git gc` 仍可能清掉新 chunk。

## Phase 7a:物件存取層 —— 改動、量測與已知限制

### 原本的問題

`pack_read_depth()` 每查詢**一個物件**就 `opendir` 一次 `objects/pack/`,並把整個 `.idx` 與整個 `.pack` 讀進 malloc 的緩衝區,用完整包丟掉。單次查詢的成本因此正比於**整個 repo 的 pack 體積**,一次歷史走訪就是 O(物件數 × pack 大小)。

另有一個硬性缺陷:`idx_find()` 遇到 offset 表項目 MSB 被設定(idx v2 用來表示「這是 large-offset 表的索引」)就直接放棄,代表**超過 2GB 的 pack 完全無法讀取**。

### 改動

1. `.idx` / `.pack` 改為 `mmap` 唯讀映射,不再整檔複製進堆積。
2. 新增 process 生命週期的 pack registry(以 `git_dir` 為 key),每個 pack 只開啟與解析一次。
3. `idx_find()` 改用 fanout 表把二分搜尋範圍先收斂到 `id[0]` 的桶。
4. 讀取端支援 idx v2 的 64-bit large-offset 表(寫入端維持拒絕產生 >2GB 的 pack)。

### 量測

基準 repo:811 commits、48MB pack,warm page cache,同機比較。

| | 改動前 | 改動後 | 系統 `git` |
|---|---|---|---|
| `sg log`(走完 811 個 commit) | 2.64s | 0.007s | 0.005s |
| `sg status` | 0.27s | 0.03s | 0.03s |

`sg log` 的輸出在改動前後**逐位元組相同**(三個不同大小的基準 repo 各驗證一次)。

### 兩個在審查與實測中發現、已修正的缺陷

- **重入導致的 use-after-free**。`read_entry_at()` 解 REF_DELTA 時會遞迴回 `pack_read_depth()`;若那個巢狀查詢 miss 且此時 pack 目錄的 mtime 變了(例如同時有 `git gc` 在跑),它會觸發重掃,而重掃會釋放外層 `pack_read_from_dir()` 迴圈**正在走訪**的那個鏈結節點。已用手工合成的「REF_DELTA 但 base 不存在」物件搭配並行 toucher 在 ASan 下穩定重現(第 127 次迭代),修法是在讀取進行中把被替換的清單先寄放,等最外層讀取返回才真正回收;修正後同樣條件跑 1200 次無重現。
- **秒級 mtime 造成的過期窗口**。以目錄 mtime 判斷是否需要重掃,而 `st_mtime` 只有秒級解析度:另一個行程若在我們掃描的**同一秒內**寫入新 pack,mtime 比較會相等,`sg` 就會把真實存在的物件回報為不存在——這是相對舊「每次查詢都重掃」實作的退化。修法沿用 git 對 racily-clean 的處理思路:掃描當下若記錄到的 mtime 就是當前這一秒,該次掃描結果視為不可信,miss 時一律重掃。已用合成的大 delta(把讀取窗口撐到約 100ms)搭配同秒內發布 base pack 的測試驗證:修正後 5/5 命中,關掉該防護的對照組 4 次有效測試中 3 次漏掉。

### 已知限制

- **pack registry 沒有上限**。一個 process 會把它查詢過的每個 `git_dir` 的所有 pack 都保持映射到結束為止。對短命的 CLI 指令沒有影響;若未來要做長時間常駐的行程,需要加上 LRU 與淘汰。
- **尚未做 delta base cache**。深 delta 鏈上的每一層都會重新解壓縮它的 base,深度 N 的鏈成本是 O(N²)。git 用 `core.deltaBaseCacheLimit`(預設 96MiB)處理這件事。目前的量測顯示這不是主要瓶頸,但在 `--aggressive` 打包過的 repo 上做大量 checkout 時會浮現。
- **commit-graph 與 multi-pack-index 尚未實作**。原路線圖把它們列在 Phase 7;實測顯示物件存取層修好之後,`sg log` 已與 `git log` 同級,commit-graph 的邊際效益因此大幅下降,留待有實際需求(例如 `--graph`、大量 merge-base 查詢)時再評估。
- **寫入端仍不產生 >2GB 的 pack**。讀取已支援 large-offset,寫入遇到需要它的情況會明確報錯而非產生壞檔。

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

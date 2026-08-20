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

| Phase | 內容 | 狀態 | 交付驗證方式 |
|---|---|---|---|
| 0 | 專案骨架:build system、目錄結構、測試框架、CI | 完成 | `make && make test` 可執行;CI 於 Phase 8 補上 |
| 1 | 物件模型:SHA-1/zlib wrapper、loose object 讀寫、`sg init`/`hash-object`/`cat-file` | 完成 | small_git 寫的 blob 能被系統 `git cat-file -p` 讀出;反之亦然 |
| 2 | Index + 基礎 porcelain:`add`/`status`/`commit`/`log`/`diff`、refs、`switch`/`restore` | 完成 | 在 small_git 建立的 repo,`git log`/`git status` 可正常運作 |
| 3 | UX 差異化層:友善錯誤訊息、guided status、危險操作二次確認 | 完成 | 手動 UX walkthrough |
| 4 | 救援機制:自動快照 ref、`sg undo`、merge/rebase 與衝突 UX | 完成 | 模擬誤操作後可 100% 復原 |
| 5 | Packfile 與網路互通:pack reader/writer、smart HTTP client(clone/fetch/push) | 完成 | 可 clone 真實 repo、可 push 上去 |
| 6 | 大檔案 chunking(選配功能) | 完成(預設關閉) | 大檔案 repo 體積與 checkout 時間對比 |
| 7 | 巨型 repo 效能 | 部分完成 | 見下方 Phase 7a;**實際做的與原規劃不同**,說明如下 |
| 8 | 文件、打包、跨平台收尾 | 完成 | README、man page、`make release`/`install`;CI 設定為在 Linux(gcc/clang)與 macOS 上建置並跑完整測試 |
| 9 | 可用性補完:`.gitignore`、`sg add` 遞迴、`sg branch` | 完成 | 見下方 Phase 9;以真 git 為 oracle 的 600 次隨機模糊測試零分歧 |
| 12 | revision 解析、`sg tag`、`sg reset`、訊息正規化、loose 解壓上限 | 完成 | 見下方 Phase 12;interop 從 382 增至 642 項,全部以真 git 為 oracle |
| 13 | `sg push` 推送 tag(`--tags`、名稱同時查 heads/tags) | 完成 | 見下方 Phase 13;interop 從 642 增至 680 項,以真 git 為 oracle |

Phase 7 的內容與原規劃不同,原因記錄在此以免日後誤解:原本列的是 commit-graph、multi-pack-index、平行化。實測後發現真正的瓶頸完全不在那裡——是物件查找每次都重讀整個 pack(見下方 Phase 7a)。修好之後 `sg log` 已與 `git log` 同級,commit-graph 的邊際效益因此大幅下降,故未實作,留待有實際需求時再評估。原規劃的三項都尚未完成。

另外,Phase 5 原本列有「delta 壓縮」,但寫入端至今仍是每個物件各自 zlib 壓縮、不做 delta;讀取端則完整支援 OFS_DELTA/REF_DELTA。表格已移除這個未兌現的描述。


## Phase 9:可用性補完(`.gitignore`、`sg add` 遞迴、`sg branch`)

前八個 Phase 做完後,`sg` 在真實專案上其實還不能用:`status` 會把 `node_modules`、build 產物
全列出來,`add` 只吃單一檔案路徑,而且**沒有任何列出分支的指令**。Phase 9 補這三項。

### 做法與驗證

- **`.gitignore` 引擎**(`src/workdir/ignore.c`):完整 `gitignore(5)` 語意——逐目錄規則堆疊
  (深層覆蓋淺層)、`.git/info/exclude`(較低優先)、否定、錨定、目錄限定、`**` 三種形式、
  字元類、跳脫與尾隨空白處理。**21 項語意規則全部先以真 git 實測確認才寫進規格**,而非憑記憶。
- **`sg add` 遞迴**:`sg add .` 走訪整棵樹,略過 `.git`(任何深度)、剪除被忽略的目錄、
  已追蹤檔案不受忽略影響、明確指名被忽略檔案需 `-f`、暫存刪除,並維持既有的全有全無索引寫入。
- **`sg branch`**:列出(合併 loose 與 packed-refs、loose 優先)、建立、刪除。刪除**必須同時
  清掉 loose 檔與 packed-refs 行**——只刪 loose 會讓陳舊的 packed 項目把分支復活到舊 commit。

### 驗證方式

除了 377 項 interop 檢查與 ASan/UBSan 全綠之外,關鍵的一致性驗證是**以真 git 為 oracle 的隨機
模糊測試**:自動產生 600 組隨機 pattern 集合與目錄樹,比對 `sg status` 的未追蹤集合與
`git status --porcelain -uall` 是否完全相等——**600 次零分歧**。這比逐案手寫測試更能涵蓋
matcher 的組合邊界。

### 過程中發現並修掉的缺陷

**`sg add .` 會靜默漏檔**。新的走訪程式把 `lstat` 失敗一律當成「檔案在走訪途中被刪掉」而跳過,
但 `lstat` 也會因 `ENAMETOOLONG`(sg 組絕對路徑,深樹會超過平台 `PATH_MAX`)、`EACCES`、
`ELOOP` 失敗。結果是 `sg add .` 回傳 0、什麼都沒暫存,而 `git add .` 正常暫存——commit 會安靜
地少掉內容。現在只有 `ENOENT` 視為良性,其餘明確報錯;`sg status` 則印警告說明清單可能不完整。

值得記錄的是**第一版回歸測試是空的**:它用「巢狀到 mkdir 失敗」建樹,而失敗點落在
`opendir`(那條路徑本來就有正確報錯),根本沒走到出問題的 `lstat`——把修正還原後測試照樣通過。
改成用 `chdir` + 相對路徑建出「目錄開得起來、但裡面項目的絕對路徑超長」的確定性邊界後,
還原修正的對照組才真的 FAIL。**測試必須先證明它會失敗,才能相信它的通過。**

### 已知限制

- 不支援 `core.excludesFile` 與全域 ignore 檔。
- 無法走訪絕對路徑超過平台 `PATH_MAX` 的目錄樹(git 用 `openat()` 相對走訪則不受限)。
  這是既有限制,非本 Phase 引入;現在至少會明確報錯而非靜默略過。
- 字元類中含 `/`(如 `a[x/]b`)與 POSIX 具名類別(`[[:alpha:]]`)未實作。

## 支援平台

- **macOS**:已在本機實際建置並跑過完整測試套件(release 與 ASan/UBSan 版本各一次)。
- **Linux**:CI 設定為以 gcc 與 clang 建置並跑完整測試套件。開發機沒有 Linux 環境,因此 Linux 的建置結果**以 CI 為準**,而非本機驗證過的結論。
- **Windows 不支援**。程式碼直接使用 POSIX API(`mmap`、`opendir`、`fcntl` 檔案鎖、POSIX 路徑分隔),要支援 Windows 需要一層相容層,目前不在範圍內。
- 兩個平台需要不同的 feature-test macro:`-std=c11` 是嚴格 ISO C,glibc 會把 `strdup`、`strtok_r`、`mkstemp`、`getcwd` 以及 `struct stat` 的 `st_mtim` 都藏在 `_POSIX_C_SOURCE` 後面;而 Darwin 預設就看得到這些,反而是**定義了** `_POSIX_C_SOURCE` 會把 `st_mtimespec` 藏起來。因此 Makefile 依 `uname -s` 分別定義 `-D_POSIX_C_SOURCE=200809L` 與 `-D_DARWIN_C_SOURCE`,兩者不可互換。

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

## Phase 10:二進位解析器的 fuzzing —— 攻擊面、發現與已知限制

`sg` 會解析兩類**不可信的二進位輸入**:從網路收到的 packfile 與 `.idx`,以及
磁碟上的 index v2。過去每次為這個專案手工合成惡意 pack 都會挖到真 bug
(Phase 5a 的 REF_DELTA 環造成 stack overflow、Phase 7a 的 missing-base
REF_DELTA 造成 use-after-free),所以把它系統化成常設的 fuzz harness。

新增 `tests/test_fuzz_pack.c` 與 `tests/test_fuzz_index.c`,照既有測試慣例
(無框架、`CHECK` 巨集、丟進 `tests/` 就被 `Makefile:48` 自動收集)。

### 為什麼不能照抄 `tests/fuzz_ignore.py` 的形狀

`fuzz_ignore.py` 是**差分測試**:拿真 `git` 當 oracle,比對語意分歧。二進位
解析器要抓的是**記憶體安全問題**,而這裡沒有天然的 oracle——把同一份損毀資料
餵給真 git,git 只會說「index 損毀」,不會告訴你 `sg` 有沒有越界。所以這兩支
改成 in-process 的 C harness,鑑別力來自 sanitizer 而不是比對。CI 的接法也
因此不同:`fuzz-ignore` job 是純 `make`,而這兩支真正有牙齒的地方是
`sanitizers` job(見下方「哪些修法真的有回歸保護」)。

### 兩個決定成敗的設計限制

1. **破壞位元組後必須重算 trailer SHA-1**。`sg_index_read` 在解析任何 entry
   **之前**就先驗全檔 checksum,pack 也一樣。純隨機 bit-flip 會 100% 卡在
   checksum,永遠打不到後面的解析邏輯——那會是一支「看起來在 fuzz、其實什麼
   都沒測到」的測試。
2. **每輪要用全新的 `mkdtemp` git_dir**。pack registry 以 `git_dir` 為 key、
   靠目錄 mtime 判斷是否重掃,而 `st_mtime` 只有秒級解析度;同一個 git_dir 在
   一秒內反覆換 pack 內容會讀到 stale cache,測試就會假綠。

### 找到並修掉的缺陷

| # | 位置 | 問題 |
|---|---|---|
| 1 | `index.c` 三處錯誤路徑 | 整個 index 檔的 `buf` 沒釋放 |
| 2 | `index.c` 的 `padded_len` 檢查 | `free_entries(entries, i)` 上界不含當前 entry,剛 malloc 的 path 漏掉 |
| 3 | `index.c` 的 `nentries` | 攻擊者可控的 u32 直接當 malloc 大小,無上限 |
| 4 | `pack_inflate` | `strm.avail_out = (uInt)expected_len` 把 64-bit size varint 截成 32-bit |
| 5 | `read_entry_at` 等五處 | 宣告的解壓大小未與可用壓縮位元組數對照就 malloc |
| 6 | `sg_pack_delta_apply` | target size 是**已解壓 delta stream** 裡的獨立 varint,不受 5 的檢查管轄 |

缺陷 1、2 之所以長期存在,是因為 CI 的 ASan 為了容忍兩個 process 生命週期的
快取而全域關掉了 leak detection——等於洩漏偵測有個大洞。這個洞已在 Phase 11
補起來(見下)。

缺陷 4 是唯一的記憶體**內容**安全問題(其餘是資源耗盡):宣告大小 `2^32+5`
時 `avail_out` 被截成 5,壓縮流只要恰好解出 5 bytes,zlib 就回報成功,而呼叫端
用的是未截斷的 `size`,於是回傳一個「宣告 4 GB、實際只有 5 個 byte 有效、
其餘全是未初始化記憶體」的物件。修法是把 zlib 的餵料改成分塊,並加一道
「本輪 `total_in`/`total_out` 都沒動就判定卡住」的獨立防線——這個迴圈在處理
不可信輸入,卡死就是 DoS,不該把安全性外包給對 zlib 隱性契約的信任。

### 壓縮比界限的實測

缺陷 5、6 的修法都是「宣告大小 vs. 可用位元組數」的合理性檢查。pack 這邊用
DEFLATE 的理論最大壓縮比。理論硬上限是 **1032:1**(258 bytes ÷ 2 bits),但
實測真 zlib 對 8 MB 全零檔達到 **1026.8:1**(`git verify-pack -v` 量到
size=8000000 / packed=7791),距離上限只剩 **0.45%**——而 pack 裡**最後一個**
物件的可用位元組數幾乎等於它自己的壓縮大小,正是最緊的情況。1032 在數學上
確實不會誤殺,但這種邊際沒有工程餘裕,因此常數放寬到 `1032 * 4`。這不損害
檢查的目的:它要擋的是差了好幾個數量級的荒謬值(例如宣告 4 GB 卻只有 33
bytes 可用)。

`sg_pack_delta_apply` 不能用壓縮比,因為 target size 不是壓縮流的解壓結果,
而是 copy/insert 指令重建出來的。界限改由指令流推導:一個 copy opcode byte
最多帶出 65536 bytes,所以 target size 不可能超過「剩餘 delta 位元組數 ×
65536」。

### 哪些修法真的有回歸保護(mutation 驗證實測)

逐一破壞每處修法、看對應測試會不會紅。**這一步由主對話執行,不交給寫測試的
人自己驗**:

| 破壞的地方 | 普通建置 | ASan 建置 |
|---|---|---|
| 拿掉錯誤路徑的 `free(buf)` | **紅** | — |
| `free_entries(i + 1)` 改回 `(i)` | **紅** | — |
| 拿掉 `nentries` 上限 | 綠 | 綠 |
| `decompressed_size_is_plausible` 恆真 | 綠 | **紅**(19 TB 配置請求) |
| 拿掉 delta target 界限 | 綠 | **紅**(545 TB 配置請求) |
| 還原 `pack_inflate` 的 chunking | **紅** | — |
| 同時拿掉 `pack_inflate` 的兩個 guard | **紅**(看門狗) | — |

最後一列原本是**卡死**而不是紅燈——`fork()` + `alarm()` 只保護了單一呼叫點,
同一個二進位裡其他呼叫點照樣會無限期停住,而 GitHub Actions 的 job 預設
timeout 是 6 小時。因此兩支 fuzzer 的 `main()` 都裝了整體看門狗
(`SG_FUZZ_TIMEOUT`,預設 600 秒),把任何位置的卡死轉成帶診斷訊息的非 0 退出。

⚠ **做 mutation 驗證時每次都要 `make clean`**。第一次跑這張表時出現「還原修法
後測試仍然紅」的詭異結果,查下來是增量重建沒有正確反映還原(object 檔不記錄
自己是從哪個版本編來的,見 `Makefile:76-80`),clean 重建後兩個方向都正常。
差點把自己的建置殘留誤判成測試不穩定。

重點結論:**「拒絕荒謬大小」這類加固,回傳值與未加固版本完全相同**(都是
-1,只是機制從顯式拒絕變成 malloc 失敗),透過公開 API 根本觀察不到差別。
它們的鑑別力**只存在於 ASan 底下**——而且抓到它們的是隨機 fuzz 輪,不是手工
構造的確定性測試。這也是為什麼 `sanitizers` job 裡加了一段長 campaign:那是
這批加固唯一真正的守門員。

### 已知限制

- **`nentries` 上限沒有任何測試能分辨**。`0xFFFFFFFF × sizeof(sg_index_entry)`
  約 344 GB,在 ASan 的 1 TB 配置上限之內,不會觸發 allocation-size-too-big;
  普通建置下 malloc 失敗也一樣回 -1。這個檢查是 defense-in-depth,對應的測試
  是 smoke test 而非鑑別性測試,已在測試註解裡明講,不假裝它守得住東西。
- **macOS 完全不支援 LeakSanitizer**(指定 `detect_leaks=1` 會直接 abort,不是
  「預設不啟用」)。洩漏偵測因此靠 `test_index_fail_path_leak` 的 RSS 探針,
  而 ASan 的配置器 quarantine 會扣住已釋放的 chunk 撐大 RSS 造成偽陽性,所以
  該探針在 ASan 建置下自我停用。結果是洩漏與記憶體安全兩種偵測分屬不同的 CI
  job,不能只看其中一個。**更根本的問題是 CI 全域關掉了 `detect_leaks`,所以
  除了 index 那一條失敗路徑之外,這個專案至今沒有任何自動化的洩漏偵測**——
  缺陷 1、2 能存活這麼久就是這個原因。當初關掉它的理由是「pack registry 與
  chunk keepalive 是刻意不釋放的 process 生命週期快取,會被誤報」,但那個前提
  **很可能不成立**:兩者都掛在全域指標下(`pack.c:498`、`chunk.c:744`),而
  LeakSanitizer 的可達性分析把全域變數當 root,仍然可達的記憶體本來就不會被
  判定成 leak。也就是說在 Linux CI 上直接開 `detect_leaks=1` 有相當機會直接
  通過,連抑制檔都不必寫。這是一次 CI 就能驗證的事,本機(macOS)測不到。
- **只有 pack/idx/index 三個解析器被覆蓋**。`sg_decompress`
  (`src/util/zutil.c`,loose object 的解壓路徑)是「每次滿了就 `cap*2`」的
  無界成長,沒有輸出長度上限,是同類的 zip-bomb 攻擊面,但不在這次範圍內。

## Phase 11:把 CI 的 LeakSanitizer 開回來

Phase 10 留下的第一順位 follow-up。改動只有一行語意:`.github/workflows/ci.yml`
的 ASan job 從三處 `ASAN_OPTIONS=detect_leaks=0` 改成 `detect_leaks=1`。

### 原本關掉的理由是錯的

當初的說法是「`pack.c:498` 的 mmap pack registry 與 `chunk.c:744` 的 chunk
keepalive cache 是刻意不釋放的 process 生命週期快取,開了會被誤報」。這個前提
不成立:兩者都是**檔案層級的全域變數**,而 LeakSanitizer 的可達性分析把全域
當 root,仍然可達(still-reachable)的記憶體不會被判定成 leak。實測證實了這點
——`detect_leaks=1` 在 Linux CI 上直接通過,三個矩陣格加 ASan job 全綠,
**不需要任何 suppression 檔**。

代價是實在的:缺陷 1、2 兩個 `index.c` 的洩漏能活到 Phase 10,靠的是
`test_fuzz_index` 量 peak RSS 才抓到——比 LSan 鈍得多的工具。

### 綠燈不算證據:植入洩漏的三次實測

「開了之後 CI 還是綠」有兩種解釋:沒有洩漏,或 LSan 根本沒在跑。分不出來就等於
沒有這道閘門。所以在合併前種了故意的洩漏送 CI,結果值得記下來:

| 植入形態 | CI 結果 | 解讀 |
|---|---|---|
| 1 塊 1234 B,指標存進 `sg_cli_run` 的區域變數後設 NULL | **綠**(382/382 通過) | 假陰性 |
| 100 塊 1234 B,同一位置迴圈 | 紅,`detected memory leaks`,interop 200/382 | LSan 活著 |
| 1 塊 1234 B,配置在一個會返回的 helper 內 | 紅,`Direct leak of 1234 byte(s) in 1 object(s)` | 單一洩漏抓得到 |

第一列的假陰性不是 LSan 的缺陷,是**植入位置**的問題:那個指標被溢出到
`sg_cli_run` 自己的堆疊框架,而該框架在整個指令執行期間都是活的祖先框架,
保守掃描把它當成 root,於是那塊記憶體一直「可達」。真實的洩漏來自**會返回的
函式**,框架隨即被後續更深的呼叫覆蓋——第三列證明那種形態單一一塊就會被抓到。

教訓一般化:**驗證一個保守式洩漏掃描器時,不要把植入放在會存活到 process
結束的框架裡**,否則證明出來的是工具沒壞,而不是閘門有效。

### macOS 本機怎麼先驗一輪

Apple 的 ASan 沒有實作 LSan,`detect_leaks=1` 會直接 abort,所以本機測不到。
替代品是系統內建的 `leaks(1)`:

```bash
MallocStackLogging=1 leaks --atExit -- build/tests/test_foo
MallocStackLogging=1 leaks --atExit -- build/sg status
```

它會印 `N leaks for M total leaked bytes`。用它掃過 22 個測試二進位與 20 種
CLI 呼叫,全部 0 leak——這是推 CI 前的信心來源。同樣的紀律適用:先用一支故意
洩漏的小程式確認 `leaks` 會紅(它會印 "Process is not debuggable" 警告,但
偵測照常運作),再相信那些 0。

### 已知限制

- 覆蓋範圍等於 ASan job 跑到的路徑:`make sanitize` 的單元測試 + `interop.sh`
  + 兩支解析器 fuzzer。沒被這些跑到的程式碼一樣沒有洩漏偵測。
- 保守掃描的本質:仍然可達的記憶體不算 leak。刻意留存的快取因此免疫,但
  「本該釋放卻剛好還被某個全域或活框架指到」的真洩漏也會被漏掉。
- 這是 Linux 專屬的閘門。macOS 那格 CI 不跑 ASan,本機也無法複製。


## Phase 12:`sg tag`、`sg reset`,與兩個位元相容缺口

新增 `sg_rev_parse_commit`(`include/sg/revparse.h`)——`HEAD`/分支/tag/40-hex
加上 `~N`/`^N` 後綴的小型 rev 語法子集,以及 `sg tag`:輕量標籤只是
`refs/tags/<name>` 直接指向 commit;`-a`/`-m` annotated 標籤額外寫入一個
`SG_OBJ_TAG` 物件,ref 改指向該物件。`-m` 沒有 `-a` 時比照真 git 隱含
annotated(已用真 git 實測確認)。與 `sg branch` 共用
`sg_ref_list_under`/`sg_ref_delete_under`(loose+packed 合併/清除)及
`sg_ref_name_valid_for_create`。

### 已知限制:`sg push` 不推送 tag(**已於 Phase 13 解除,見下方**)

`sg push` 當時寫死只推送 `refs/heads/`——不論本地是否有 `sg tag` 或真 git
建立的 tag,`sg push` 都不會把它們傳到遠端,也不會報錯或警告,是靜默的範圍
限制。因為 `sg` 與真 git 位元相容,使用者可以用真 git 的 `git push --tags`
對同一個 repo 補推 tag 繞過。這一段保留下來是因為 Phase 13 的實作範圍正是
由它界定的;現況以 Phase 13 為準。

### `sg reset`:三種模式,三道不同的安全閘門

`--soft` 只動分支指標,`--mixed`(預設)另外重寫 index,`--hard` 再加上工作
目錄。目標 revision 走上面的 `sg_rev_parse_commit`。

**不把三種模式都塞進 `sg_safe_apply_tree`**,雖然那是既有的破壞性操作骨架。
真 git 對 `--soft`/`--mixed` 不要求確認——它們根本不覆寫檔案——全部走同一條
路會比它模仿的工具還礙事。折衷是:`--hard` 完整的確認加快照;`--mixed` 自動
建快照但不打斷;`--soft` 兩者皆無。

`--mixed` 需要一條「重寫 index、不碰工作目錄」的路徑,而
`sg_apply_tree_to_workdir` 表達不了——它靠 `stat()` 剛寫出的檔案來填 index
條目的 stat 欄位。不寫檔就沒有那個來源,而**沿用舊條目的 stat 比看起來更糟**:
index 會宣稱檔案是最新的,同時記錄一個不同的 blob,而真 git 把 stat 當捷徑
信任,於是對一個內容已經不同的檔案回報乾淨。`sg_index_reset_to_tree` 因此
只在 sha 未變時沿用 stat 欄位,其餘一律歸零。

### 兩個位元相容缺口

**訊息沒有正規化。** `sg commit -m "x"` 把字串原封不動寫進物件,真 git 先套用
預設的 `--cleanup=whitespace`。同一個邏輯 commit 在兩邊算出不同的物件 id:
git 的訊息段結尾有換行,sg 沒有。git 讀得懂 sg 的物件、`fsck` 也乾淨,所以
沒有壞掉——但「同一個 commit 在兩個工具下雜湊相同」正是這個專案的主張,而
382 項 interop 檢查從來沒有逐位元組比對過訊息段。

規則是實測出來的,不是回想的:每行剝掉行尾空白、連續空行併成一個、去掉開頭
與結尾的空行、結尾正好一個換行,行首空白保留。只有尾隨換行那條是顯而易見的
——只補那一條會用一個分歧換來四個。**`\v` 與 `\f` 不算空白**(真 git 保留
它們),`\r` 算。第一版把這兩個字元也剝了,審查時比對真 git 才發現。

`sg_message_cleanup` 放在序列化函式旁邊,但**刻意不從序列化函式裡呼叫**:
`cmd_rebase.c` 轉發既有 commit 的訊息,而真 git 用 `--cleanup=verbatim` 可以
產生沒有尾隨換行的訊息,rebase 必須逐位元組保真。套用的是持有訊息的呼叫端
——commit、tag、merge、snapshot、chunk。

順帶記一個真 git 本身的不對稱,反直覺到值得寫下來免得日後被「順手統一」:
**`git commit -m ""` 中止,`git tag -a -m ""` 接受**並建出訊息段為空的 tag。

**loose object 解壓無上限。** `sg_decompress` 每次滿了就把緩衝區加倍,沒有
天花板,所以一個解壓後有數 GB 的 loose object 檔會被完整解出來才輪到別人看它
一眼。Phase 10 把這條記成剩下的 zip-bomb 面。

上限不是魔術數字:`sg_loose_read` 先只解壓開頭 64 個位元組,從中解析出
`"<type> <size>\0"` 標頭,再把 `header_len + declared_size` 當硬天花板做真正的
解壓——git 自己的形狀。zutil 保持泛用(`sg_decompress_bounded` 把天花板當
參數,不認識物件),兩階段邏輯放在有資格認識格式的 `loose.c`。

同一輪修掉一個與 Phase 10 缺陷 4 同族的截斷:`avail_in` 讓 `size_t` 穿過
32-bit 的 `uInt`,所以超過 4 GB 的 loose 檔會餵給 zlib 錯誤的長度。Phase 10
修了 pack 那條,漏了這條。

### 審查抓到的:一個假的 merge parent

`sg reset` 沒有清掉 `.git/MERGE_HEAD`。在衝突的合併中用裸的 `sg reset` 放棄
它、暫存修正、正常 commit——產出的 commit 有**兩個 parent**,第二個指向剛被
放棄的 merge target,全程沒有任何警告。只有 `--hard` 會清理,因為它走
`sg_safe_apply_tree`;soft 與 mixed 兩條路徑從來沒呼叫過 `sg_merge_head_remove`。

矩陣是實測三種模式 × 兩種進行中狀態量出來的,不是猜的:真 git 在合併中或
rebase 暫停中**直接拒絕** `--soft`,`--mixed` 清掉 `MERGE_HEAD`,而**三種模式
都不動 rebase 狀態**。sg 現在與此一致,唯一例外是 `--hard` 仍會經
`sg_safe_apply_tree` 清掉 rebase 狀態——那條路徑與 `switch`、`undo` 共用,
要改屬於另一次改動,已記在 `docs/sg.1` 的 BUGS 段落。

614 項檢查裡沒有一項看得到這件事:**從來沒有任何測試在合併進行中呼叫過
`sg reset`**。新的檢查斷言使用者會注意到的終態——後續 commit 只有一個 parent。

### 這一階段關於測試的三個教訓

**優先序在同一個地方錯了兩次,方向相反。** 先是 tag 與 branch 誰優先(gitrevisions
是 tag 勝),接著是完整 40-hex 的位置(它比任何 ref 都優先——名字剛好是別的
commit 的 hex 的 branch 會被字面值蓋掉)。兩次都是憑對 gitrevisions 的記憶下
判斷,兩次都錯。現在註解裡寫的是「哪個順序是實測來的」。

**壞掉的 mutation 會給出看起來更漂亮的紅。** 驗證 hex 優先序時第一次改壞了
`resolve_base` 的區塊順序,兩個測試都紅了——比正確的 mutation 多一個。重做成
乾淨的 mutation 後只有目標測試紅,那才是有效證據。

**保守式洩漏掃描與 stat 快取都會讓「沒紅」變得沒有意義。** 證明 CI 的
LeakSanitizer 還活著時,種在會存活到 process 結束的框架裡的單一洩漏不會被報
(見 Phase 11);而 `--mixed` 的 stat 守門,在檔案 mtime 與 index 同一秒時被
git 的 racily-clean 規則遮住——所有其他檢查都在同一秒內寫檔與寫 index,所以
整套測試都看不見它。把 mtime 用 `touch -t` 推到過去才顯形。

### 已知限制

- **沒有縮寫 sha。** `sg_rev_parse_commit` 只吃完整 40 字 hex;前綴匹配要同時
  掃 loose 與每一個 pack,是另一個量級,標頭註解有寫明這是刻意的。
- **`sg reset` 不吃 pathspec。** `sg restore --staged` 已經是
  `git reset --mixed <path>` 的同義詞,硬做會變成第三份重疊邏輯。
- **detached HEAD 一律拒絕。** sg 從來不寫裸 sha 進 HEAD,全專案沒有這個
  primitive;`cmd_push.c` 與 `cmd_rebase.c` 已是「偵測到就拒絕」的既有慣例。
- **`sg reset --soft --hard` 報用法錯誤**,真 git 則接受並讓最後一個旗標生效。
  刻意收斂:模糊的呼叫寧可拒絕。寫進 man page 免得被當成 bug 修掉。
- **4 GB 截斷的修法沒有測試覆蓋。** 要真的配置超過 1 GB 才觀測得到差異,與
  專案「測試不配置大量記憶體」的既有約束衝突,只有程式碼審查層級的信心。
- **`loose.c` 的溢位保護與「無進度即判定卡死」防線都沒有能鑑別的測試。**
  前者需要 `declared_size` 接近 `SIZE_MAX` 的案例,後者需要一段病態的 deflate
  串流,兩者都不容易自然構造。如實記錄,不假裝有覆蓋。

## Phase 13:`sg push` 推送 tag

`sg push` 從只認 `refs/heads/` 擴充到能推 tag。三種模式:不給名稱維持原本的
「推當前分支」;給了名稱時**同時查 `refs/heads/<name>` 與 `refs/tags/<name>`**,
兩邊都命中就報 `src refspec '<name>' matches more than one` 並拒絕;`--tags`
推送 `refs/tags/` 底下全部。

### 要改的比預期少

踏勘的結論推翻了 Phase 12 對工作量的估計。當時記的是「需要擴充 ref 列舉範圍
**與 pack 打包對象**(annotated tag 物件本身也要進 pack)」——後半句是錯的:

- `cmd_push.c` 的 `walk_add_object` 早就是型別無關的,`SG_OBJ_TAG` 分支一直
  存在且會走 `tag.object` 那條邊。餵一個 annotated tag 物件 id 進去,tag 物件
  本身加它指向的 commit 與整棵樹都會被正確收進 send set。
- `sg_pack_build_buf` 對 `SG_OBJ_TAG` 沒有任何排除或特判,與 blob/tree/commit
  走同一條路。
- 網路層(`sg_transport_push`、pkt-line 組包、report-status 解析)對 ref 命名
  空間完全無知。`sg_ref_name_is_safe` 只用來過濾**遠端**廣播,不擋本地要送出
  的 ref 名稱。

於是整個里程碑的產品程式碼改動都落在 `src/cli/cmd_push.c` 一支函式裡。這是
「先派人測繪再開規格」直接省下工作量的一次:照 Phase 12 的估計去做,會有一半
的力氣花在改根本不需要改的 pack 與網路層上。

### tag 的更新規則不是 fast-forward

真 git 對 tag **不算祖先關係**:遠端已有同名 tag 而 id 不同,一律拒絕
(`! [rejected] lw -> lw (already exists)`),`--force` 才覆寫。這是實測出來的,
不是回想的。

這件事直接決定了實作:tag **不走** `check_fast_forward`,tag 的 id 也不流進
`sg_merge_base`。踏勘時發現「把 tag 物件 id 餵進 `sg_merge_base` 會被判成
unrelated,於是變成 non-ff,於是要求 `--force`」——結果**碰巧**與真 git 一致。
碰巧一致的路徑不能留:`sg_merge_base` 走的是 commit parent 鏈,它對非 commit
輸入的行為沒有任何人保證過,而正確答案根本不需要它。tag 的規則寫成獨立分支:
遠端沒有就新增、id 相同就跳過、id 不同看 `--force`。

另外兩件也是實測而非記憶:遠端的 `refs/tags/<name>` 對 annotated tag 指向
**tag 物件 id,不 peel**(所以本地 id 要用 `sg_ref_read_path` 讀原始值,不能
用會 peel 的 `sg_rev_parse_commit`);push tag **不建立**
`refs/remotes/<remote>/<tag>`,那個命名空間只屬於分支。

### 部分成功

`--tags` 推多個 tag 時,其中一個因為已存在而被拒,其餘照樣送出,最終退出碼
為 1——與真 git 一致。這使得原本的 `updates[2]` stack 陣列(1 分支 + 1 chunks
ref)不夠用,改成動態配置,並且 report 解析從「比對單一 ref_name」推廣成逐一
比對所有送出的 update。`sg_push_ref_update.ref_name` 是 borrowed 指標,所以
持有字串的 entry 陣列必須活到 `sg_transport_push` 回來為止。

### 審查抓到的:零個 tag 時連遠端都不碰

第一版在 `--tags` 模式取得 tag 清單後,若一個 tag 都沒有就直接印
`Everything up-to-date.` 退出 0。問題是這發生在 `sg_transport_ls_refs_push`
**之前**,於是連帶跳過了整個 `refs/sg/chunks` keepalive 傳播區塊——而其他每
一條 push 路徑都會走到那段。實測可觀測:零 tag 的 repo 對連不上的遠端下
`sg push origin --tags` 得到 `Everything up-to-date.` 與退出碼 0、零連線嘗試,
同一個遠端下 `sg push origin` 則正確報連線失敗。

一個用了分塊儲存但當下沒有任何 tag 的 repo,會在 `sg push --tags` 時靜默跳過
chunk 可達性的同步。修法是移除那個提早返回:零 tag 只是讓候選集合保持空的,
照常做 ls-refs、照常評估 chunks,最後由既有的統一判斷
`entry_count == 0 && !send_chunks_update` 決定要不要印
`Everything up-to-date.`。

教訓的形狀值得記下來:**「沒有東西要做」與「不必問對方」是兩回事**。前者只有
在問過遠端之後才成立,而這個專案有一個真 git 沒有的、掛在遠端狀態上的不變量
(chunks keepalive),任何從純本地狀態就提早返回的捷徑都會把它漏掉。

### 測試紀律:否定式斷言需要定向 mutation

新增 38 項 interop 檢查(642 → 680),全部以真 git 為 oracle。驗證分兩層,
兩層都由主對話執行:

**全面還原**——把 `cmd_push.c` 還原成改動前、保留新測試,26 項第一批檢查裡
**21 項變紅**。剩下 5 項不紅,但它們並非都沒有鑑別力:其中 3 項是**否定式
斷言**(「沒有建立 remote-tracking ref」「遠端沒有多出這個 tag」),在什麼都
沒推成功的情況下自然成立。全面還原對這類斷言結構性地無效。

**定向 mutation**——那 3 項要靠針對性的改動才看得出來,實跑確認:拿掉
`if (!entries[i].is_tag)` 守衛 → 667/668,只有「不建立 remote-tracking ref」
變紅;把 ambiguity 判斷改成 `if (0)` → 665/668,三項具名檢查變紅。修復輪的
三處也各自實跑:重新植回提早返回 → 678/680;`rc = had_rejection ? 1 : 0;`
改成 `rc = 0;` → 679/680;usage 守衛的 `return 1` 改成 `return 0` → 679/680。
每一條都只有目標檢查變紅,沒有連帶災情——乾淨的 mutation 才是有效證據
(Phase 12 已經吃過一次「改壞的 mutation 給出更漂亮的紅」的虧)。

實作者最初回報「5 項空洞檢查」,冷讀的審查者只數到 2 項。兩邊都不完全對:
正確的區分是「全面還原時不紅」(5 項)與「任何定向 mutation 都測不到」(2 項),
這兩個數字量的不是同一件事。

### 已知限制

- **沒有 refspec 語法。** 不支援 `src:dst`、`+force` 前綴、`--delete`。名稱
  一律同時解讀為分支或 tag,同名就拒絕而不是猜。
- **`--tags` 與明確名稱不能併用**,是用法錯誤。真 git 兩者可以並存,這裡刻意
  收斂:模糊的呼叫寧可拒絕(與 Phase 12 的 `sg reset --soft --hard` 同一個
  取捨)。
- ~~**「只有 chunks 落後」的防線沒有能鑑別的測試。**~~ **2026-08-12 補上**
  (interop phase6a case 8,5 項檢查,909 → 914)。缺口本身如實記在這裡:把
  `entry_count == 0 && !send_chunks_update` 改成 `if (entry_count == 0)`,
  當時 680 項檢查照樣全綠,因為現有的 chunks push 檢查都伴隨著真實的新 commit
  一起送出。

  能鑑別的場景來自一個實測而非推理的事實:`sg add` 在**寫 blob 時**就把
  chunk 併進 keepalive 樹(`chunk.c` 的 `keep_alive_add`),不是等到 commit。
  所以推過一次之後再 `sg add` 一個大檔**而不 commit**,分支停在原地、
  `refs/sg/chunks` 已經往前——正是這道守衛唯一能被觀測到的形狀。

  同一個 mutation 現在殺掉 5 項裡的 3 項(914 → 911),且沒有連帶災情。
  另外 2 項不紅是對的,而且各有用途:一項是**fixture 前提**(斷言 `sg add`
  確實動了 keepalive ref 而沒動分支——哪天 `sg add` 改了行為,其餘檢查會
  因為錯誤的理由變綠);另一項是「第二次 push 退出 0」,在 mutation 下**照樣
  退出 0**,正好示範了只看退出碼就是假覆蓋。真正有鑑別力的斷言是**遠端自己的
  `refs/sg/chunks` 有沒有前進到本地的 keepalive commit**,以及新檔案的每個
  chunk 有沒有實際落到遠端。
- **annotated tag 的 `object_type` 在本地建立時恆為 commit**
  (`cmd_tag.c` 走 `sg_rev_parse_commit`,一定 peel 到 commit),所以 `sg` 自己
  建不出指向 tree 或 blob 的 annotated tag。push 端不依賴這個假設(它推的是
  ref 裡的原始 id,不看 `object_type`),從真 git clone 來的這類 tag 能正常
  推送,但這條路徑沒有測試覆蓋——要造出這種 tag 需要真 git 參與。

## Phase 14:paused rebase 不再被 `reset --hard` 與 `switch` 終結

Phase 12 留下的已知 divergence:`sg reset --hard` 會清掉進行中的 rebase 序列
器狀態,真 git 不會。當時記在 `docs/sg.1` 的 BUGS 段落沒有修,因為根源在
`sg_safe_apply_tree`——四個指令共用的那層安全包裝——不屬於 `sg reset` 的範圍。

### 先量,再改

開工第一件事是把真 git 2.55.0 的行為量出來,不是回想。六格結果:

| 情境 | 真 git |
|---|---|
| `reset --hard`(含帶 commit 參數)在 paused rebase 期間 | 允許,**保留** `.git/rebase-merge`;之後 `--abort`/`--continue` 都還能用 |
| `reset --hard` 在衝突 merge 期間 | 允許,**清掉** `MERGE_HEAD` |
| `switch` / `switch --force` / `switch --discard-changes` | **拒絕**,exit 128,狀態原封不動 |
| `switch -c <new>` | 一樣拒絕,而且**新分支不會被建立** |
| `checkout -f` 在 rebase 期間 | 允許切換,**保留** rebase 狀態 |
| `reset`(mixed)在 paused rebase 期間 | 保留(sg 原本就已做對) |

歸納成一條規則:**`MERGE_HEAD` 會被任何重設工作目錄的操作清掉;rebase 序列器
狀態只有 rebase 自己的子指令能結束。**

這條規則直接改變了里程碑的範圍。原本以為是「`reset --hard` 那一行不要清」,
但 `sg_safe_apply_tree` 一旦不清了,`sg switch` 若照舊放行,就會留下一份指向
另一個分支的殘留 rebase 狀態——**比修法前更糟**。所以 switch 必須同時改成拒
絕。這不是範圍蔓延,是同一條規則的另一半。

### 三個呼叫端,三種答案

- `cmd_reset.c`(`--hard`):保留狀態。真 git 如此。
- `cmd_switch.c`:直接拒絕,`--force` 繞不過(真 git 的 `--force` 也繞不過),
  且閘門必須在 `-c` 建立分支之前。
- `cmd_undo.c`:**維持清除**,但改由呼叫端自己做。`sg undo` 沒有真 git 對應
  物,沒有 oracle 可抄;把工作目錄倒回快照之後留著序列器狀態,`rebase
  --continue` 會在一棵被抽掉的樹上續作,語意不通。
- `cmd_merge.c`(fast-forward):不需要改,`cmd_merge.c:511-518` 早就在上游擋
  掉 rebase 進行中。

於是 `sg_safe_apply_tree` 的職責收斂成一句可以寫進標頭的話:清 `MERGE_HEAD`,
不碰 rebase 狀態,需要舊行為的呼叫端自己在回傳後處理。

### mutation 實測:9 條檢查裡只有 1 條有鑑別力

新增的 interop 檢查裡有 9 條跟 switch 有關。把 `cmd_switch.c` 的閘門停用後
重跑,**只有 1 條變紅**(`sg switch --force ... is still rejected`)。

原因是既有機制搶先一步:`sg_safe_apply_tree` 的 dirty 判斷(`apply.c:312`)
本來就含 `sg_rebase_state_exists`,而 interop 在非 tty 下跑,
`sg_confirm_dangerous` 會自動拒絕。於是沒有 `--force` 的 switch **在閘門存在
與否兩種情況下退出碼都是 1**——只是理由完全不同。退出碼相同讓 8 條檢查看起來
綠得很有信心,實際上什麼都沒守住。

修法是對這些案例額外斷言 stderr 的**訊息內容**(閘門的專屬字串),而不只是退
出碼。教訓的形狀:**當被測行為與既有的保護機制在可觀測結果上重疊時,斷言必須
往下探到「理由」那一層,否則覆蓋率是假的。**這與 Phase 10 記過的「加固前後回
傳值完全一樣,只有 sanitizer 看得出差別」是同一類問題的不同外衣。

### 審查抓到的:確認提示對 `sg undo` 說謊

`sg_safe_apply_tree` 的確認提示是四個呼叫端共用的。第一版寫「但 rebase 本身
會保留,要結束它請用 `sg rebase --abort`」——對 `reset --hard` 為真,對
`sg undo` 為假:使用者在同一個提示裡被承諾保留,按下 y 之後它被刪掉。

`switch` 和 `merge` 都在自己的閘門就被擋掉,永遠走不到這個提示,所以 `undo`
是唯一會看到這句謊話的呼叫端。修法是把共用訊息降級成中性的真話(只講「會覆蓋
衝突解決內容」),由 `cmd_undo.c` 在呼叫**之前**自己印出「會放棄 rebase」的告
知——順序上會出現在確認提示之前,使用者決定 y/N 時就看得到。

這條原本測不到:interop 裡每一個 `sg undo` 都帶 `--force`,而 `force=1` 的
`sg_confirm_dangerous` 會提早返回,**根本不印 message**。非 tty 且無 force
那條分支才會把訊息印到 stderr(`confirm.c:18-21`),補測試要走那條路徑。

### `reset --hard <另一個 commit>` 之後 continue

原本所有案例都只用裸 `reset --hard`(等於原地不動),沒測過帶參數的版本。實測
兩邊一致:剩下的 commit 會疊在 **reset 的目標** 之上,不是原本的 onto。sg 的
`rebase --continue` 從 `sg_ref_resolve_head` 取父節點,真 git 的 sequencer 也
是,所以碰巧一致——但這次是量過才寫下來的。

順帶記一個沒有處理的既有 divergence:sg 的 rebase 進行中 **HEAD 仍指在分支
上**,真 git 是 detached。上面那組案例的最終 graph 兩邊相同,所以沒有動它。

### 正規化過頭的比對,與否定式斷言

第二輪冷讀抓到 `reset --hard <另一個 commit>` 那組的 graph 比對**正規化過頭**:
把 `%P` 的 40-hex 全部換成 `X` 之後,線性歷史的每一行都塌成
「`<subject> X`」,實際上只驗到「每個 commit 有幾個 parent」——而這組案例真正
要驗的性質(重放出來的 commit 疊在 **reset 目標** 而不是原本的 onto)恰好被抹
平掉了。跨 repo 比對必須正規化 sha(兩邊的 commit id 必然不同),但正規化的
範圍要剛好停在「無法比較的部分」。補的是直接斷言 parent 身分:在各自的 repo
裡驗 `feature^` 等於各自的 base sha,再加一條「不是 master 的後代」,真 git
那側也照驗一次而不是預設它對。

否定式斷言(「提示不得承諾 rebase 會保留」)照 Phase 12 的教訓需要自己的
mutation:把新句子留著、只把舊承諾**附加**回去,肯定式那條照樣綠,只有否定式
那條變紅。這證明它不是多餘的。

最終 `tests/interop.sh` 726 項檢查(Phase 13 結束時是 680),八條定向 mutation
每一條都至少讓一條檢查變紅。

## Phase 15:`sg stash`

### 架構決策:為什麼實作真 reflog

`stash@{N}` 完全建立在 reflog 上——這是實測結論,不是設計偏好。刪掉
`.git/logs/refs/stash` 只留 `refs/stash`,真 git 的 `stash list` 變空、`pop`
直接失敗且不 fallback。走「從 snapshot 長出一個 stash 堆疊,不碰 reflog」的
路會產生真 git 完全看不見的 stash——比不實作更糟,因為它讓使用者以為變更被
保存了。這條路直接違反專案第一原則:`tests/interop.sh` 拿真 `git` 當 oracle,
一個 git 讀不到的 stash 沒有任何東西能測它。

決定性證據:手工偽造一個 sg 風格的 stash(身分 `small_git <sg@localhost>`、
固定 `+0000` 時區、全零 old-oid、手寫 reflog 行),**真 git 全盤接受**——
`list`/`show`/`pop` 都正常,產出正確的工作目錄與 index。於是 reflog 不是一個
子系統,是「一行一筆的文字檔 + 一條硬性不變量」,详见下一節。

`sg_reflog_*`(`include/sg/reflog.h`)的 `ref_path` 刻意是**參數**而不是寫死
的 `"refs/stash"`——格式層與「每次 ref 更新都寫一筆」的呼叫端覆蓋是兩件事,
Phase 15 只做前者、只呼叫一次(`"refs/stash"`),日後的 HEAD reflog 直接重用
同一個檔案層,把預算花在呼叫端覆蓋上。

### 兩條硬性不變量(實測)

- **`refs/stash` 必須等於 reflog 最後一行的 new-oid**,否則真 git 報
  `log for ref refs/stash unexpectedly ended`,後續**所有** `stash@{N}` 一併
  失效——不是只有出問題的那一筆。
- **stash commit 必須有 ≥2 個 parent**。手工偽造的 1-parent 版本 `list` 看得到
  (list 只讀 reflog,不驗證 commit 結構),但 `show`/`apply`/`pop` 全部以
  `not a stash-like commit` 死掉。所以「index commit(parent 2)是必需品」不是
  Phase 16 的加分項,`sg_stash_push` 無條件寫它。

### 位元相容的界線(兩個方向要求不同)

真 git 讀 sg 的 stash 只要求**結構有效**,不要求身分一致(上面的偽造測試已
證明:`+0000`、`small_git`、全零 old-oid 全部被接受,git 從不驗證 commit 的
author/committer 身分,只驗上面兩條不變量)。

sg 讀真 git 的 stash 要求**寬容解析**:任意名字(含空格、`<`、`>`)、任意時區
(`+0800`)、3-parent(`git stash -u`)、以及 `git gc` 之後活在 packed-refs 裡
的 `refs/stash`。`sg_commit_parse`(`src/object/commit.c`)本就會成長 parent
陣列,3 parent 免費解析;`sg_ref_read_path` 本就會 fallback 到 packed-refs。

因為 sg 的時區是固定字面量 `"+0000"`、時間戳來自 `time(NULL)`、不理會
`GIT_AUTHOR_DATE`,**sg 的 stash commit id 永遠不可能等於真 git 對同樣內容
產生的 id**——這不是 Phase 15 引入的新讓步,`sg` 寫的每一個 commit 本來就是
這樣,只是 stash 是第一個「兩邊互相讀對方產出物」的功能,所以第一次必須把它
寫進規格,免得日後有人設計出注定失敗的 oid 相等檢查。**因此任何 interop 檢查
都不准比對兩邊的 commit id。** 最強的 oracle 是 `sg stash list` 與
`git stash list` 逐位元組相同——這個字串來自 sg 自己寫的 reflog message,
是唯一一處兩邊「應該」相同的東西。

### 「Dropped」訊息的規則被記錯又改正的經過

最初抽樣兩個案例,記成「pop 帶 `refs/` 前綴、drop 不帶」。那是抽樣假象——
碰巧那次 pop 沒帶參數而 drop 帶了。測完全部六種組合(pop/drop × {無參數,
裸 `0`,`stash@{0}`})之後,真正的規則是**兩個子指令共用同一條**:git 只在
參數已經是 `stash@{N}` 形式時原樣回印,無參數與裸 `0` 都解析成完整的
`refs/stash@{N}`。

六種組合現在都有 interop 檢查,每條 sg 斷言都配一條 git oracle 斷言,這樣真
git 改變行為時是 oracle 那一半先變紅,而不是檢查悄悄凍結一個過時的認知。
過程中也踩到兩個與規則本身無關但值得記的坑:oracle 必須固定 `LC_ALL=C`
(git 會翻譯這句訊息,`zh_TW` 環境印出全形括號的中文,CI 預設 `C`——不釘住
會「CI 過、本機紅」);pop 的輸出含雙引號(`use "git add"` 狀態區塊),塞進
`sh -c` 字串會被截斷成一半案例誤判,drop 的單行輸出沒有這問題——六案例正好
一半一半誤判,像是個真的 product bug。

### 與真 git 一致的危險行為

**rebase 暫停中執行 `stash push` 會讓正在 rebase 的 commit 消失**——實測 sg
與真 git 2.55.0 **行為完全相同**:push exit 0;`rebase --continue` 印
「Successfully rebased」exit 0;那個正在重放的 commit 從 log 消失;工作只留
在 stash 裡。原因是 push 把 index 與工作目錄重設回 HEAD,`--continue` 於是
判定「這個變更已經存在於 upstream」而跳過它。sg 不改這個行為(改了反而是與
git 分歧),但**多印一行 stderr 告知**使用者,並用兩邊各跑一次的 interop
檢查釘住「行為等價」(exit code、log 是否消失、stash 是否保住工作)。

### 刻意的偏離(列表)

- **pop/apply 要求工作目錄乾淨**,真 git 會嘗試三方合併已存在的骯髒變更。
  比 git 嚴格,與 `sg merge` 既有先例一致(`merge_require_clean`)——乾淨的
  工作目錄讓 apply 可以直接把 `sg_merge_trees` 的 ours 設成 HEAD 樹,不需要
  額外一層合併邏輯。
- **rebase 進行中 pop/apply 拒絕**(真 git 允許),比照 `cmd_switch.c`/
  `cmd_merge.c` 的既有閘門先例,而不是新發明一條規則。
- **sg 沒有 exit 128**,真 git 在「reflog 條目不存在」之類的情況用 128,
  sg 一律用自己風格的訊息 + exit 1。
- `sg stash apply`/`pop` 成功時印 `sg status` 的內容(真 git 印同等的 status
  區塊,但 sg 的用詞是中文、提示裡是 `sg` 指令而非 `git`)。

### 明確不做(Phase 16+)

| 項目 | 為什麼這個切點乾淨 |
|---|---|
| `-u`(儲存未追蹤檔案) | 需要把 `collect_untracked`(`cmd_status.c`,目前 `static`、回傳裸 `char **`)升格成公開 API、回傳 `sg_flat_entry`,加第三個 parent,加 overwrite 拒絕。純加法,commit builder 本就吃 parents 陣列,reflog 層完全不用動;但**這次已經加了強制守衛**:Phase 15 遇到 3-parent stash 一律拒絕,絕不悄悄只還原追蹤的那半而丟掉未追蹤檔案。 |
| `show` | 唯一需要全新輸出格式器的子指令:git 預設 `--stat`,而 `sg diff` 沒有任何 flag、沒有 tree-vs-tree 模式。沒有其他東西依賴它,`git stash show` 對 sg 建的 stash 現在就能正常用。 |
| `--keep-index` / `--index` | `--keep-index` 便宜但會讓 flag 矩陣變複雜;`--index` 不便宜——git 對 index 樹另外做合併,有自己的失敗模式。兩者都是純加法,不影響已寫的部分。 |
| `--patch`、`--staged`、pathspec、`stash branch/create/store` | 各自獨立的介面,不影響已寫的部分。 |
| 髒工作目錄下 apply | 見上面「刻意的偏離」,是本階段最不情願的取捨,但它是讓 apply 能直接重用 `sg_merge_trees` 而不必另寫一層的前提。 |
| 通用 reflog(`HEAD@{N}`、`sg reflog`、branch 更新寫 reflog) | 檔案層(`src/storage/reflog.c`)這次就落地了,expensive 的一半——每個 ref 更新點都要接上呼叫——留給獨立的一個 phase。 |

### mutation 實測結果

| mutation | 變紅的檢查數 |
|---|---|
| push 的 unmerged 守衛 | 原本 **0 條紅** → 補斷言後 1 條 |
| drop 不重指 `refs/stash` | 1 條 |
| push 寫 ref 但跳過 reflog append | 30 條 |
| `-u` 三 parent 守衛 | 原本 **0 條紅** → 補斷言後 1 條 |
| 衝突標籤換成分支名 | 2 條 |
| push 改用 `sg_safe_apply_tree` | 9 條 |
| pop 的 index 修正 | 11 條 |
| clear 用 unlink 取代 `sg_ref_delete_under` | 1 條 |
| list 順序反轉 | 2 條 |
| `sg stash apply` 也去 drop | 5 條(**補之前 59 條全部沒抓到**) |
| reflog message normalization 關掉 | 6 條 |
| reflog rewrite 重新蓋 ident | 3 條 |
| reflog count-0 寫空檔而非刪檔 | 1 條 |
| reflog rewrite 不重新串接 old-id | 2 條(預先設計時就記成「應該測不到」,實測是唯二能測到的——見下方基礎設施小節的更正) |
| `read_rc == -1` 降級成警告 | 3 條 |
| `print_dropped` 一律加前綴 | 2 條 |

**最有價值的兩條要特別寫一段。** push 的 unmerged 守衛與 `-u` 三 parent 守衛
原本都是**0 條紅**,原因與 Phase 14 記過的教訓是同一個形狀:守衛存在於 CLI
與 library 兩層,**任一層單獨關掉都由另一層接手**,退出碼相同,只是理由不
同。修法也一樣——斷言要探到 stderr 的專屬訊息那一層,不能只看退出碼。

查 unmerged 守衛時還發現 push 路徑**根本沒有 CLI 層守衛**:library 拒絕
之後,CLI 印的是既有的 catch-all 訊息「無法建立 stash(未初始化的 HEAD,
或 index 有未解決的衝突?)」——一個問號,而使用者當下正卡在合併衝突中,
明明知道答案卻只給猜測。修法是把檢查搬到「已經知道答案」的地方,直接給出
陳述句而非問句,interop 斷言釘住那句具體訊息。這個斷言本身也踩過一次坑:
第一版 grep 的是「unresolved conflict」,而 catch-all 訊息裡剛好也含這個
片段,所以拿掉守衛之後這條檢查照樣綠——收斂成比對守衛自己專屬的措辭才有
鑑別力。

### 已知無覆蓋(誠實記錄,比照 Phase 10/12 的既有寫法)

- **reflog 的 82-byte 最小行長守衛**:被後續的 hex 與格式驗證整個包住。試過
  兩種構造(含正好 81 bytes、兩個 oid 都是合法 hex、緊接著截斷),普通 build
  與 `make sanitize` **都不會紅**。與 `index.c` 的 `nentries` cap 同類缺口。
- **`chunk_enabled` 的初始值**:`sg_repo_read_chunk_config`
  (`src/storage/repo.c`)進門就無條件 `*enabled_out = 0`,呼叫端傳什麼都會
  被覆寫,是個 dead store,與 stash 無關但踏勘時順手發現,記在這裡免得散佚。
- **`sg_write_file_mkdirs` 失敗分支**:已補上單元測試(用同名目錄擋住寫入
  路徑觸發 `EISDIR`),這條不再是缺口,但觸發手法值得記下來:目標路徑先建成
  一個同名目錄,寫入時 `open()` 回 `EISDIR`,不必真的耗盡磁碟或權限。

### 測試基礎設施本身的教訓

跑 mutation 的過程中,**mutation 腳本自己出過三次錯**,每次都會偽造出「守衛
有覆蓋」或「守衛沒覆蓋」的假結論:

1. 字串替換版的 mutation 先命中了 `free(entries[i].ident)` 那一行的
   `entries[i].ident`,把它換成別的表達式後變成 free 一個字串常數而直接
   abort——腳本以為這是「守衛消失」的效果,其實只是把程式碼改壞了。
2. `sg_ref_delete_under` 在原始碼裡有兩處呼叫,腳本打到 drop 那一處而不是
   `sg_stash_clear` 那一處,於是「clear 用 unlink 取代」這條 mutation 一開始
   量到的其實是 drop 的行為。
3. `make && interop.sh` 串接執行,編譯失敗時 interop 沒有真的跑,而舊的
   log 檔沒被清掉,腳本讀到的是上一輪殘留的結果,誤判成「這輪也綠」。

結論:**mutation 腳本與被測程式碼一樣需要驗證**——至少要確認編譯成功、確認
改動的那一行真的命中預期的那一處(不是同名字串在別處的另一次出現)、每輪都
清掉舊產出再重新產生新的。

interop 檢查本身也出過兩個同類問題,細節寫在程式碼註解裡:`LC_ALL` 未固定
會讓某條檢查「在 CI 過、在本機紅」(見上面「Dropped」訊息那節);把含雙引號
的輸出塞進 `sh -c` 字串會炸開命令列,只讓一半案例被正確判斷。

最終 `tests/interop.sh` 826 項檢查(Phase 14 結束時是 726),單元測試新增
`tests/test_reflog.c`、`tests/test_stash.c`、`tests/test_objstore.c` 三支,
`make test` 31 支二進位全過,`make sanitize` 乾淨。

## Phase 16:進行中的 merge 不再被 `switch` 終結

Phase 15 交接時記著的追蹤項第一條:`sg switch --force` 在衝突 merge 期間會
成功切走並清掉 `MERGE_HEAD`,真 git 拒絕。這是 Phase 14 修過的那個 bug 的
同一個形狀,只是換一個子系統——rebase 換成 merge。

### 先量,再改

照 Phase 12 留下的規矩,開工第一件事是量真 git 2.55.0,不是回想:

| 情境 | 真 git |
|---|---|
| `git switch <other>` 在衝突 merge 期間 | **拒絕**,exit 128 |
| `git switch --force <other>` | **一樣拒絕**,`--force` 蓋不過 |
| `git switch -c <new>` | 一樣拒絕,而且**新分支不會被建立** |
| `git switch <目前已經在的分支>` | 一樣拒絕 |
| 衝突已解決並 `git add`(index 乾淨、`MERGE_HEAD` 還在) | 一樣拒絕 |
| `MERGE_HEAD` 內容壞掉 / 空檔 / 是一個目錄 | 一樣拒絕 |
| `git checkout -f <other>` | **允許**,並清掉 `MERGE_HEAD` |

歸納:**閘門看的是 `MERGE_HEAD` 存不存在,不是 index 還有沒有衝突**,而且
`switch` 與 `checkout -f` 在這件事上是分裂的。sg 沒有 `checkout`,所以要對
齊的是 `switch` 那一半。

### 這條閘門為什麼原本「看起來」是對的

修法前 sg 並不是完全不擋——沒有 `--force` 的 `sg switch` 在衝突 merge 期間
照樣退出 1。但理由是 `sg_safe_apply_tree` 的髒工作目錄確認,而 interop 在非
tty 下跑,`sg_confirm_dangerous` 自動拒絕。**`--force` 正好是用來繞過這一層
的**,於是唯一真正需要擋的那條路徑反而是唯一沒被擋住的。

這與 Phase 14 記過的教訓完全同型,而且這次是**在 mutation 裡再次現形**:把
新閘門整個拿掉重跑,「plain switch 被拒絕」「拒絕後 `MERGE_HEAD` 還在」
「HEAD 沒動」這幾條**照樣是綠的**,只有斷言 stderr 專屬字串的那幾條變紅。
退出碼層級的斷言在這裡是假覆蓋,第二次驗證了同一件事。

### `exists` 不是 `read`

`sg_merge_head_read` 把「沒有 merge」與「merge 狀態損壞」壓進同一個 -1,拿
它當閘門會讓損壞的 merge 狀態直接放行。所以新增了
`sg_merge_head_exists`(`include/sg/merge.h`),與 `sg_rebase_state_exists`
對稱,只問路徑上有沒有東西。

實作用 `lstat` 且**不加** `S_ISREG` 過濾,因為真 git 用的是 `file_exists()`
——實測連「`MERGE_HEAD` 是一個目錄」都照樣拒絕。第一版寫成
`stat() == 0 && S_ISREG(...)`,是量了目錄那一格才改掉的。

這個決定有專屬的鑑別測試,不然它與 `read` 版在其他所有案例上結果完全相同:
interop 的 `phase16 corrupt` 四條,以及 `tests/test_merge_head.c` 直接並排
比對兩支 API 在「壞掉/空/截斷/目錄」四種狀態下的分歧。定向 mutation 驗過:
把 `exists` 改成委派給 `read`,單元測試紅 4 條、interop 紅 4 條,其餘全綠。

### 兩個閘門的順序:量到「測不到」,就記下來

`cmd_switch.c` 現在有兩道閘門(rebase 在前、merge 在後)。想寫一條檢查釘住
順序,結果把兩者對調之後**整份 875 項全綠**。

原因量出來了:paused rebase **不會**寫 `MERGE_HEAD`(sg 只留
`.git/sg-rebase/`,真 git 留 `rebase-merge/` 加 `REBASE_HEAD`,兩邊都不寫
`MERGE_HEAD`)。兩個條件從任何可達路徑都不可能同時成立,只有手工偽造兩份狀
態檔才到得了,而那個狀態真 git 也沒有,沒有 oracle。

所以順序是**真的不可觀測**。照本專案既有的作法,把它記進「已知測不到的守衛」
而不是假裝有覆蓋,並改成釘住那條真正撐住結論的事實:paused rebase 期間
`MERGE_HEAD` 不存在(sg 與真 git 各驗一次)。

### fixture 汙染:過度反應的 mutation 會為錯的理由變紅

「閘門不可過度觸發」那組檢查(merge 結束後 switch 要恢復正常)第一版用
`sg switch` 建 fixture 的分支結構。把閘門改成永遠觸發之後那組確實變紅了——
但**是 fixture 自己沒建起來**,precondition 就先掛了,根本沒走到被測行為。
這正是「結果相同、理由不同」的另一種版本,只是這次假的是紅燈而不是綠燈。

修法是那組改用真 git 建分支結構,只留 merge 與最後那次 switch 給 sg。再跑
一次同樣的 mutation,precondition 全綠、只有四條「應該恢復正常」變紅。

### 沒有動的東西

`reset --hard` 照舊清 `MERGE_HEAD`(Phase 14 定下的規則沒變),新閘門只在
`cmd_switch.c`,沒有下沉到 `sg_safe_apply_tree`——後者是四個呼叫端共用的,
下沉會連 `reset --hard` 一起擋掉。`phase16 reset` 那組就是釘這件事的。

### 冷讀抓到的:新閘門把既有的不一致變成了死路

reviewer 冷讀時指出:`sg_merge_head_exists` 只用在 `switch` 一個呼叫端,其餘
八處判斷「merge 是否進行中」的地方仍然用 `sg_merge_head_read(...) == 0`——
而那正是新標頭註解自己點名的缺陷。獨立重現後確認成立,而且比回報的更嚴重。

`MERGE_HEAD` 損壞時(空檔、非法內容、目錄),量到的分歧:

| 指令 | 真 git 2.55.0 | 修法前的 sg |
|---|---|---|
| `merge --abort` | rc 0,**清掉** | rc 1,**留著** |
| `reset --hard` | rc 0,**清掉** | rc 0,**留著** |
| `reset --mixed` | rc 0,**清掉** | rc 0,**留著** |
| `reset --soft` | **拒絕** | **放行** |
| `stash push` | rc 0,**清掉** | rc 0,**留著** |
| `status` | 報告「有尚未合併的路徑」 | **不報告** |
| `commit` | **大聲拒絕**「損壞的 MERGE_HEAD 檔案」 | rc 0,**靜默產出單 parent commit** |

最後一列最嚴重:merge 語意從 commit graph 裡悄悄消失,而 sg 回報成功。這正是
本專案存在的理由要擋的那類分歧,而且**單看退出碼永遠測不到**——所以那組測試
除了斷言 exit != 0,還斷言「沒有產生任何新 commit」。

前六列合起來構成一個死路:新閘門讓 `switch` 永久拒絕,而沒有任何指令清得掉那
個檔案,連 `switch` 自己印的錯誤訊息建議的兩條路(`sg commit`、
`sg merge --abort`)都走不通,唯一出路是手動 `rm .git/MERGE_HEAD`。**這個死路
是新閘門造成的**:修法前 `switch --force` 至少還走得掉(雖然行為是錯的)。

修法是把「merge 是否進行中」的判斷全部收斂到 `sg_merge_head_exists`。收斂後
`sg_merge_head_read` 在 `src/` 裡只剩**一個**呼叫端——`cmd_commit.c`,唯一真
正需要那個「值」(第二個 parent)的地方,而它現在把兩個問題分開問:先問存不
存在,再問讀不讀得出來,讀不出來就照真 git 拒絕。

`cmd_rebase.c` 那道是唯一沒有 oracle 的:實測 `git rebase` 在乾淨工作樹下**根
本不看 `MERGE_HEAD`**(合法或損壞都一樣,直接跑完並清掉它),sg 是刻意拒絕。
既然刻意拒絕,就必須連損壞狀態一起涵蓋,否則 rebase 與 switch 會對「有沒有
merge 在進行」給出不同答案。這一點寫在註解裡。

### 這一輪的 mutation

- 把**除了 switch 以外**全部八處還原成 `read`:18 條變紅,涵蓋每一列;而
  「合法 merge 仍然產出雙 parent commit」保持綠,證明拒絕是針對損壞而不是針
  對 merge 本身。
- 只還原 `cmd_commit.c`:精準 3 條紅,全在 commit 那組——逐處歸屬成立。
- 加回 `S_ISREG`:第一版只有單元測試會紅,interop 全綠(reviewer 指出這個覆蓋
  缺口)。補上「`MERGE_HEAD` 是目錄」的 CLI + oracle 案例後,同一條 mutation
  現在讓 interop 也紅 2 條。

教訓的形狀:**收斂一個判斷式的時候,沒被收斂的呼叫端不是「維持現狀」,而是
與新行為產生互動**。這裡的互動剛好是最壞的一種——新守衛把舊的寬鬆行為變成了
無法脫身的狀態。範圍看起來只有一個檔案的修法,實際邊界是「所有問同一個問題的
地方」。

最終 `tests/interop.sh` 909 項檢查(Phase 15 結束時是 826),單元測試新增
`tests/test_merge_head.c`,`make test` 32 支二進位全過,`make sanitize` 乾淨。
八條定向 mutation(拿掉閘門、`exists`→`read`、永遠觸發、對調順序、
`exists` 委派 `read`、加回 `S_ISREG`、還原全部非 switch 呼叫端、只還原
`cmd_commit.c`)每一條的紅燈範圍都逐條核對過。

## Phase 17:通用 reflog(`HEAD@{N}`、`<ref>@{N}`、`sg reflog`)

Phase 15 交接時把「通用 reflog」列成明確留到獨立 phase 的項目:檔案層
(`src/storage/reflog.c`)那時已經落地,`stash@{N}` 也已經在用它,缺的是「每個
ref 更新點都要接上呼叫」——本階段做的正是這一半。

### 開工踏勘先推翻了「掛在 refs.c 就好」的預設

原本設想是在 `refs.c` 的寫入函式裡直接呼叫 `sg_reflog_append`。踏勘先發現這
個假設不成立:**ref 寫入根本沒有收斂點**。`cmd_push.c`/`cmd_fetch.c`/
`cmd_clone.c` 各自手刻一份逐字複製的 `write_ref_file`,HEAD 的 symbolic 寫入
在 `cmd_switch.c`/`cmd_clone.c`/`repo.c` 各自手刻,而 `refs.c` 本身**沒有** HEAD
寫入函式。把 reflog 掛在 `refs.c` 只會靜默漏掉 fetch/push/clone 的 ref 更新與
所有 HEAD 移動——不是報錯,是那幾行 reflog 悄悄不存在。

決定收斂的理由不是「重複太多讓人不舒服」,而是它是 **ref 後端層的不變式**:
「old 值該怎麼讀」「哪些 namespace 該記」「HEAD 何時連動記一行」這幾條規則,
散到十幾個呼叫端各自實作,漏一處不會有任何錯誤訊息,結果只是 reflog 少幾
行——這與 Phase 16 那個 bug 是同一個形狀(一條該在單一閘門實現的規則被分散到
多個呼叫端,沒被收斂的那個就是漏洞)。於是先新增
`sg_ref_update`/`sg_ref_set_head`(`include/sg/refs.h`)作為唯一寫入點,再把六
個手刻的 ref 寫入呼叫端(push/fetch/clone 的 `write_ref_file`,以及三處 HEAD
symbolic 寫入)全部改用它。old 值由寫入點自己讀,呼叫端不必再自己記账。收斂
反而讓後續加 reflog 訊息的 diff 變小:`do_fast_forward`(`cmd_merge.c`)連函式
簽名都不用改,只多填一個字串參數。順手還修掉 `cmd_push.c` 一個既有的語意
bug——它原本把 remote-tracking ref 的 old 值記成**遠端廣播的舊值**,但本地
remote-tracking 檔案該記的是**本地現值**,兩者在別人也推過同一個遠端之後會
不同;`sg_ref_update` 統一從本地檔案讀 old,這個分歧連帶消失。

### 三條實測規則(真 git 2.55.0)

規則不是回想出來的,是逐條量出來的:

- **不對稱**:具體 ref(如 `refs/heads/master`)的 log 只在 `old != new` 時
  追加一行;`logs/HEAD` 不論 `old == new` 與否都追加。實測
  `git reset --hard HEAD`(目標就是自己)→ `logs/HEAD` +1 行、分支自己的
  log +0 行。
- **HEAD 連動**:更新 HEAD 目前所指的那個分支時,`logs/HEAD` 會拿到 old/new/
  message **逐位元組相同**的一行。用 `git update-ref` 繞過所有 porcelain 也
  照樣發生,證明這是 ref 後端層的不變式,不是某個子指令的特殊行為。反向不
  成立:更新一個 HEAD 沒有指到的分支,`logs/HEAD` 不動。
- **政策**:只有 `HEAD`、`refs/heads/*`、`refs/remotes/*`、`refs/stash` 記
  reflog,tag、`refs/sg/chunks`、`refs/small-git/undo/*` 都不記——與真 git
  一致(非 bare repo 下 `core.logAllRefUpdates` 預設涵蓋的範圍正是這幾個
  namespace)。`sg_ref_update` 對政策外的 ref_path 傳非 NULL 訊息一律回 -1、
  完全不寫任何東西(連 ref 本身都不動),所以「哪些地方不記 log」是一份靠一
  次 grep 就能稽核的封閉清單,不必到處記憶哪個呼叫端該傳 NULL。

### `<ref>@{N}`

`<ref>@{N}` 名的是該筆 reflog 條目的 **`new_id`**,不是 `old_id`——這個方向
容易搞反:@{0} 的意思是「最近一次更新把 ref 移動**到**的值」,不是「移動前的
值」;最舊那一筆的 `old_id` 是全零,取錯方向會在邊界上悄悄回全零而不是第一個
commit。`HEAD@{N}` 與 `<branch>@{N}` 各讀 `logs/HEAD` 與
`logs/refs/heads/<branch>` 兩份不同的檔案。

索引與日期選擇器的分界不是語法層面的,是**數值大小**——真 git 對這件事沒有寫
死的語法規則,是啟發式:實測 `@{10000000}`(1e7)被當成索引,`@{100000000}`
(1e8)真 git 直接當 Unix 時間戳走 `@{<date>}` 路徑,印「日誌只能回到 <日期>」
的警告。sg 不支援日期選擇器,所以刻意用**純數字白名單**定義「這是一個索
引」,其餘內容(含 `@{u}`、`@{now}`、`@{-1}`)一律乾淨拒絕,而不是去模仿一個
以 1e8 為界、連真 git 自己文件都沒寫明的啟發式。結果是 1e8 以上 sg 拒絕、
git 接受(當日期解析)——這是刻意記下來的分歧,不是缺陷。

裸 `@{N}`(前面沒有 ref 名)一律拒絕。真 git 的裸 `@{N}` 指的是**當前分支**
而不是 HEAD——實測兩者在有过 `reset` 之類操作、HEAD 與分支自己的 log 分岔
之後值不同。把它猜成 HEAD 會是一個**看起來能跑但答案錯誤**的實作,比直接拒
絕更危險,所以選擇拒絕而不是猜。

### 冷讀抓到的真 bug:`~`/`^` 後綴迴圈從不驗證 `op`

`sg_rev_parse_commit` 的後綴迴圈長年只判斷 `if (op == '~') ... else /* '^' */
...`,從沒檢查 `op` 究竟是不是 `'~'` 或 `'^'`。這在只有兩種停止字元(`~`/`^`
/`\0`)時是安全的——**一個由呼叫端保證、卻從未寫進程式碼的不變式**:base 掃描
只在遇到這三種字元時停下,所以進到後綴迴圈的第一個字元必然是 `~` 或 `^`。
`@{N}` 合法地讓 base 掃描多了第四種停止條件(`@{`),前提就被打破了:實測
`sg tag t 'master@{0}x'` 回 0 並把標籤指到 `master@{0}` 的 parent(`x` 被當成
「非 `~` 即 `^`」的 `^` 分支吃掉了,又因為後面沒有數字,`parse_suffix_number`
對空字串回傳隱含的 1),真 git 對同一輸入直接報 `ambiguous argument`。
`sg_rev_parse_commit` 是 `sg reset`(破壞性操作)與 `sg tag` 解析使用者
revision 字串的唯一入口,打錯一個字元被靜默解到附近但錯誤的 commit,後果不
是「拒絕」而是「解到別的地方」。修法是在迴圈本身加一個
`if (op != '~' && op != '^') return -1;`,而不是在 `@{N}` 那段收尾處補丁——
讓不變式變回迴圈自己的局部性質,不依賴呼叫端多年前的假設繼續成立。

### 刻意的 divergence

1. **三方合併寫 `merge <arg>: Merge made by the 'sg-3way' strategy.`**,真 git
   是 `'ort'`(或歷史上的 `'recursive'`)。sg 沒有實作 ort 策略,照抄策略名
   會讓在 sg repo 上跑 `git reflog` 的人被誤導成「這是用 ort 合併的」。保留
   git 的文法外殼(它自己的策略名本來就會隨版本變,`'recursive'`→`'ort'`就
   是先例),只把策略名換成誠實的自己的名字。
2. ~~**detached HEAD 的 `switch` 不寫 reflog 行**~~——**Phase 18 已解決**,
   底下這段診斷的兩個選項都選了第一個:改共用函式,並逐一盤過 21 個呼叫端。
   原文保留,因為它對「為什麼當時不該單獨補這個角落」的判斷仍然成立。
   真 git 在這個情境下寫
   `checkout: moving from <40-hex> to <target>`(實測)。試過照做,行不通:
   `sg_ref_resolve_head`(`include/sg/refs.h`)對 detached HEAD 直接回
   -1——它只認 symbolic HEAD,所以 fallback 邏輯從未被觸發過。要做對,要嘛
   改這支被到處依賴、用來判斷「是不是 detached」的共用函式(波及所有靠它的
   指令),要嘛在 `cmd_switch.c` 自己解析 `.git/HEAD` 的原始內容繞過它。而
   sg 在 detached HEAD 這個狀態下本來就整個站不住腳(`sg status` 印
   `On branch ?`、`No commits yet`,沒有一致的使用者體驗),留給 Phase 18 一
   併處理,不在這裡單獨補一個沒有配套的角落。
3. **`sg clone` 不建立 `refs/remotes/<remote>/HEAD`,也不寫它的 log**。真 git
   clone 之後兩者都有。實作過程中曾一度只寫 log(不建 ref 本身),已經移
   除——`sg reflog origin/HEAD` 走 `sg_rev_parse_ref_path` 解析 ref 名,而
   該函式要求 ref 本身存在;ref 不存在,那個 log 檔就永遠讀不到,寫了也是
   死資料,而且等於宣稱一段這個 repo 從未真正擁有的歷史。
4. **`sg clone` 不寫 `refs/remotes/<remote>/<branch>` 的 log**——這條反而
   **與 git 一致**(git 也要等到第一次 `fetch` 才第一次寫那份 log),但因為
   直覺上「clone 應該把一切都寫好」而顯得反直覺,特地記下來,免得日後有人
   把它當 bug「修好」。
5. ~~**rebase 的 reflog 形狀留給 Phase 18**~~——**Phase 18 已完成**,做法正是
   底下這段所描述的:改成 detached 模型,而不是補一組形狀對不上的假訊息。
   原文保留,它把「為什麼加訊息字串解決不了」講清楚了。
   真 git 的 rebase 全程在 detached
   HEAD 上重放每個 commit,只在最後一次性把分支搬過去,所以分支自己的 log
   只有一行、`logs/HEAD` 有 `rebase (start)`/`(pick)`/`(finish)` 一串。sg 的
   rebase 從不 detach,每 pick 一個 commit 就直接搬一次分支 ref——結構上就
   無法逐字複製那組訊息序列。Phase 17 的做法是讓 `cmd_rebase.c` 的每個
   `sg_ref_update_branch` 呼叫維持傳 `NULL`(等同 `sg_ref_update` 的
   `reflog_msg == NULL` 分支),也就是完全不寫 reflog,而不是寫一組形狀對不
   上真 git 的假訊息。
6. **`sg fetch` 的訊息只嵌 remote 名稱**,真 git 嵌入完整的 argv(例如
   `fetch -q origin: ...`)。sg 的旗標集合與 git 不同,逐字複製 argv 沒有意
   義;只嵌 remote 名讓 `sg fetch origin` 與 `git fetch origin` 的訊息逐位元
   組相同(已量測),這是能對齊的最大公約數。

### 測試紀律這一輪學到的

- **`git reflog` 是免費的 oracle**:把磁碟格式做對之後,可以用同一串操作分
  別在 sg 與真 git 上跑一遍,逐位元組比對兩邊 `git reflog`/`sg reflog` 的
  message 欄——不必自己猜測措辭,答案就在真 git 的輸出裡。
- **假覆蓋,實跑才現形**:把 `@{N}` 的「純數字」白名單暫時放寬成「非空即
  可」,`@{u}`/`@{now}`/`@{-1}` 這幾條看起來在驗「必須是語法正確的索引」的
  測試**仍然變紅,但理由不對**——非數字字元參與了 char 算術,產生一個巨大
  的數值,是被無關的越界檢查擋下來的,不是被「必須是數字」這條規則擋下。補
  了一個刻意構造的 `wideranger@{A}`(先寫 20 筆 log,讓 `'A'-'0'==17` 落在
  合法索引範圍內)才是真正有鑑別力的案例——它必須能命中「這是合法的
  reflog 深度」卻因為「不是數字」被拒絕,而不是被別的守衛頂替。
- **假紅燈**:`master@{1}5` 這條測試在 2-commit 的 fixture 下確實變紅,但原
  因是 root commit 沒有 parent,`@{1}5` 被誤解析成 `@{1}^5` 之後找 5 層
  parent 本來就會失敗——與「後綴不合法該被拒絕」這條規則毫無關係,是巧合地
  紅在同一個退出碼上。把 fixture 延長到 3 個 commit,讓「合法解析但層數不
  夠」與「非法字元」兩種失敗理由分得開,才確認是後者在生效。**先證明測試會
  紅還不夠,還要確認紅得有道理。**
- **冗餘的防禦性檢查會把真正的驗證點藏起來**:`revparse.c` 裡有兩段與既有程
  式碼邏輯完全重複的檢查,單獨刪掉任何一段都是**零測試變紅**。它們不是多一
  層安全網,底下的防線(`parse_suffix_number`、`sg_reflog_at`)本來就已經擋
  住同樣的輸入;真正該打 mutation 的地方是下一層,打在這兩段冗餘檢查上永遠
  測不出東西。兩段都已經刪除,不留著製造「這裡好像有守衛」的錯覺。
- **否定式斷言各自需要自己的定向 mutation**:讓 `sg clone` 也對
  `origin/<branch>` 傳訊息(即偷偷開始寫這個 log)→ 精確 1 條檢查變紅;拿掉
  `sg_ref_delete_under` 對 log 檔 `unlink` 的 `ENOENT` 容忍 → 3 條變紅(其中
  一條是既有的 prefix 碰撞測試順帶抓到的)。`sg clone` 那條 mutation 刻意只
  對 remote-tracking ref 傳訊息、不對 tag 傳——如果無差別地對所有 ref 都傳
  訊息,tag 會觸發政策閘門(政策外的 ref_path 傳非 NULL 訊息直接 -1),讓整
  個 clone 失敗,那種紅燈不是「否定式斷言生效」,是「clone 整個掛了」,不算
  證據。
- **`logs/HEAD` 的行數斷言**:最初用 `tail -1` 比對最後一行內容,但構造一個
  「錯誤地寫了兩行相同內容」的 mutation 時,`tail -1` 照樣通過——兩行的最後
  一行內容確實相同。只有額外斷言**行數**(`wc -l`)才分得出「該有一行」與
  「多寫了一行」。

### 第三輪冷讀:模型改動把 `--abort` 的一個不變式弄壞了

段二交出去冷讀,抓到一個**高嚴重度、而且它自己重現過**的問題,是這次改動直接
造成的。

`finish_rebase` 是**兩次寫入**:先推進分支,再把 HEAD 接回去。我把 `--abort`
的分支還原拿掉了,理由是「分支從頭到尾沒被動過」——那個理由只在整場 rebase
是原子的時候成立。若在那兩次寫入之間被中斷(crash、SIGKILL、`sg_ref_set_head`
的暫時性 I/O 失敗),分支已經停在 rebase 後的 tip,而 `.git/sg-rebase/` 還在。
此時 `sg rebase --abort` 會:

- 把工作目錄還原成 **rebase 前**的樹,
- 把 HEAD 接回分支(而分支**已經**在 rebase 後的 tip),
- 刪掉序列器狀態、**退出 0**,並印「'topic' is back at <orig>」。

三者互相矛盾,而且是**假成功**。沒有 commit 永久遺失(新 tip 仍由分支可達),
但工具說了謊。舊版的 `--abort` 無條件把分支重設回 `orig_head`,對任何中間狀態
都是穩健的;新版依賴一個這條路徑自己就能打破的不變式。

修法剛好不必取捨:`--abort` 恢復成無條件還原分支,而且**不影響 reflog 形狀**
——`sg_ref_update_branch` 傳 NULL 訊息,永遠不寫 log 行;正常情況下值也沒變。
寫入順序是「先分支(HEAD 仍 detached,不會鏡射)、再接回 HEAD」。

這個情境**不能靠還原某一行來做 mutation 驗證**(它是缺少的防禦,不是壞掉的既
有邏輯),但可以**製造那個中斷狀態**來測,interop 就是這樣寫的。把修法拿掉會
讓那兩條精準變紅。

同一輪還有兩件事:

- **`<oid>` 正規化把 `onto` 的正確性蓋掉了**。分支的 finish 行是唯一嵌入完整
  40-hex 的 rebase 訊息,而雙軌比對前會把任何 40-hex 換成 `<oid>`——所以把
  `state->onto` 換成新 tip 或 `orig_head`,**整組 phase18f 都不會紅**。冷讀特別
  要求主對話實跑這條 mutation 驗證它的預測,實跑確認它說對了。已補一條不經正
  規化的直接斷言(外加一條前提檢查,確保 onto 與結果 tip 真的不同,否則那條斷
  言自己就沒有鑑別力)。
- **fast-forward 路徑的中斷窗口沒有任何標記**。冷讀說它「重跑就自癒」,實測
  **是錯的**:重跑 `sg rebase <upstream>` 會因為 HEAD 是 detached 而被拒絕、
  `--abort` 說「沒有進行中的 rebase」,只剩一個要確認的 `sg switch` 能脫身。
  一般路徑在動任何東西之前就寫序列器狀態,ff 沒有;現在也寫了(純粹為了讓那個
  窗口可救),收尾再刪掉。

### oracle 的環境本身要被宣告

phase18f 的 `(continue)` 那組本機全綠、**每一台 CI runner 都紅**(Linux 與 macOS
皆然)。為此加的診斷輸出直接給出答案,而且推翻了原本的假設(「git 在 2.54 與
2.55 之間改了訊息」):git 那一軌**一行 rebase 紀錄都沒寫**——不是訊息不同,是
rebase 根本沒完成。`git rebase --continue` 要開編輯器,沒有編輯器就失敗。

本機會過,是因為這台機器的 shell 匯出了 `GIT_EDITOR=true`。

這件事的教訓不是「要設 GIT_EDITOR」,而是:**「拿真 git 當 oracle」這句話,只有
在 oracle 的執行環境也被測試套件自己決定時才成立**。那次測量是真的,但它是透過
一個測試從未宣告、剛好存在於開發者 shell 裡的設定取得的。interop.sh 現在自己
`export GIT_EDITOR=true`,並在註解裡寫明理由。

順帶一提,驗證這個修法時第一次的「乾淨環境」模擬做過頭了——連 git config 一起
關掉,於是 git 找不到 committer 身分,整個套件在第一步就結束,輸出是空的。**空
輸出讀起來跟通過一模一樣**,只有實際看行數才會發現。

### 收尾補驗:自己寫的 reset 斷言也是假覆蓋

merge 之後回頭補跑「否定式斷言」的定向 mutation(記憶裡的規則:「沒有做 X」這
類斷言不能靠全面還原來驗),連帶發現 **Phase 18 自己新加的 reset 斷言是綠得沒有
鑑別力的**。

把 `move_head_to` 的 detached 分支關掉(讓它在 detached 時照樣去寫
`refs/heads/%s`,而 `current_branch` 是 NULL,於是產生一個叫
`refs/heads/(null)` 的分支、完全不碰 HEAD),原本只有 **phase14 的四條**變紅——
phase12 與 18d 那幾條我為了這個修法新寫的斷言,一條都沒紅。

原因是 fixture 退化:`p12r_base` 在 c1 上打 tag `v1`,而案例在 `HEAD~1`(= c1
= 就是 v1)detach 之後 `reset --mixed v1` ——**那是個 no-op**。「HEAD 移到目標
了」在 HEAD 根本沒動的建置上照樣成立。這是本次里程碑第三次踩到同一個形態(前兩
次是標籤與情境錯開一格、以及「tag 移走」案靠無關理由退回 id)。

改成在 c2 detach、reset 到 v1(真的移動),並補兩件事:一條**前提斷言**確認目標
與起點不同(否則那條斷言自己就沒有鑑別力),以及一條比對**整份分支清單**而不只
是 master——寫錯地方不一定會打到既有分支,`refs/heads/(null)` 就不會。同一個
mutation 現在讓 7 條紅,其中那條新守衛直接把 `refs/heads/(null)` 印出來。

順帶把踩過的 `tests/mutate.sh` 用法陷阱寫進腳本註解:perl 分隔符撞 C 語法
(`s{}{}` 撞大括號、`s!!!` 撞 `!=`),以及**字面量出現不只一次時一定要加 `/g`**。

### 無法驗證(如實記錄)

- 兩處 malloc 失敗分支(`cmd_commit.c`、`cmd_switch.c` 建構 reflog 訊息字串
  時的 OOM 路徑)——本專案沒有 malloc 失敗注入機制,只能靠讀碼確認對應的
  `free` 都在正確的路徑上執行。
- Phase 14/16 的 switch 閘門在 **reflog 這個維度**不可觀測:那些測試寫在
  reflog 存在之前,只讀退出碼與工作目錄/index 狀態,「被拒絕的 switch 沒有
  寫下任何 reflog 行」目前只靠「閘門在任何副作用之前」這個呼叫順序保證,沒
  有專屬斷言。
- `sg_ref_delete_under` 刪 log 檔那段路徑的長度截斷分支——需要一個長度剛好
  卡在 `SG_PATH_MAX` 邊界、讓 `.../logs/...` 比 ref 路徑本身多出的 5 個位元
  組正好溢出的分支名,構造成本高,沒有補。
- `sg push` 在「本地 remote-tracking ref 已經等於新值」這個情境下,規則 1
  (`old == new` 不追加)的抑制分支——沒有專屬案例區分「因為抑制而沒寫」與
  「單純沒被呼叫到」。
- `sg fetch` 的快進判斷在「new 的祖先鏈中間有物件缺失」這個邊角情況下的行
  為:目前的實作會保守地把它標成 forced-update(訊息說謊的方向是安全的,不
  會把非快進的更新誤標成快進),但沒有構造出這個情境的案例。

### 另外記一句

`sg_ref_update` 有一個已知限制,寫在自己的標頭註解裡:`sg_ref_read_path` 把
「ref 不存在」與「ref 檔損壞」壓成同一個 -1,所以一個損壞的既有 ref 被更新
時,它的 reflog 條目會把 `old_id` 記成全零,看起來像是這個 ref 剛被建立,而
不是「曾經存在但讀不出來」。不為這個情境引入第三態,維持與專案既有的
「-1 統一表示失敗」慣例一致。

最終 `tests/interop.sh` 998 項檢查(Phase 16 結束時是 909;熱身踏勘先補到
914,批 A 919、批 B 944、批 C 978、批 D 收尾到 998),`make test` 34 支二進
位全過(新增 `tests/test_ref_update.c` 與 `tests/test_reflog_messages.c`;
`tests/test_reflog.c` 是 Phase 15 就有的檔案層測試),
`make sanitize` 乾淨,子指令從 23 個增加到 24 個(新增 `sg reflog`)。

## Phase 18:detached HEAD,與 rebase 的 reflog 形狀

Phase 17 把兩件事留在這裡(見上面「刻意的 divergence」第 2、5 條)。它們看起
來是兩個題目,實際上是同一個:真 git 的 rebase 全程在 detached HEAD 上重放,
所以不先把 detached HEAD 變成 sg 真正理解的狀態,rebase 的 reflog 形狀在結構
上就對不齊。

### 根因:一個 -1 同時表示兩件事

`sg_ref_resolve_head` 在 `sg_ref_current_branch` 回 NULL 時直接回 -1,把
「HEAD 是 detached」與「HEAD 是 unborn(repo 剛建、還沒有 commit)」壓成同一
個失敗。**21 個呼叫端全部把它讀成後者**——標頭註解也是這樣寫的。

這不是一個「等 Phase 18 才會發生」的問題。真 git 隨時可以把任何 sg repo 變成
detached(`git checkout --detach`),而在那個狀態下 sg 不是拒絕,是**安靜地答
錯**:

- `sg log` 印「fatal: your current branch does not have any commits yet」,
  下面壓著一整段完好的歷史。
- `sg status` 把 HEAD 的樹算成空的,於是乾淨的工作目錄被報成「每個追蹤中的
  檔案都是新增」。
- `sg_safe_apply_tree` 共用同一段計算,所以 `sg switch` 在覆寫工作目錄前,是
  拿空樹去判斷它即將蓋掉什麼。
- `sg branch <name>` 回「目前的分支還沒有任何 commit」而拒絕建分支。
- `sg stash push`/`pop` 直接失敗;而 `sg_snapshot_create` 會把快照記成 **root
  commit**——安全網自己把 parent 連結弄丟了。

修法是讓 detached 的 HEAD 真的解出它的裸 sha,-1 只剩「unborn」。21 個呼叫端
逐一盤過,**沒有任何一處把這個失敗當成 detached 守衛在用**,所以沒有人因此失
去守衛;三個真的要拒絕 detached 的指令(reset、rebase、push)判斷的是
`sg_ref_current_branch`,不受影響。

`sg_ref_head_is_detached` 刻意是三態(1/0/-1):**「損壞的 HEAD」不是
「detached」**。這個區分有實際後果——detached 這個答案正是呼叫端用來決定「我
可以把裸 sha 寫進 HEAD」的依據,把損壞當成 detached,等於把損壞洗成一個看起來
正常的狀態。

### 為什麼 detach 不能借用 `sg_ref_update(git_dir, "HEAD", ...)`

Phase 17 收斂 ref 寫入時,`sg_ref_update` 順帶就具備了寫裸 sha 到 HEAD 的能力
(`ref_path_reflog_allowed` 早就放行 `"HEAD"`,`write_ref_path_raw` 對 ref_path
一視同仁)。踏勘因此建議直接用它,**那是錯的,而且錯得很安靜**:它取 old_id 走
`sg_ref_read_path`,而 HEAD 還是 `ref: refs/heads/<b>` 的時候那個 hex 解析必然
失敗,fallback 是全零——於是「從 commit A 分離到 B」被記成「憑空建出 B」。真
git 在那一格寫的是離開的那個 commit(實測)。

所以有了 `sg_ref_set_head_detached`,它的 old_id 走**修好之後的**
`sg_ref_resolve_head`,symbolic 與裸 sha 兩種形狀都解得出來。定向 mutation 把
它換回 `sg_ref_read_path`,只有「從分支 detach」那條斷言變紅,「從 detached 再
detach」照樣綠——因為後者 HEAD 已經是裸 sha 了。兩條測試分屬不同來源形狀,缺
一條就會留下這個死角。

### `at` 還是 `from`:標籤不在 HEAD 裡,在 reflog 裡

`HEAD detached at <x>` / `from <x>` 的 `<x>` **不是**現在的 HEAD,是當初的分離
點,而分離點要回頭掃 `logs/HEAD` 最後一筆 `checkout: moving from ... to <y>`
才知道。實測(git 2.55.0)出來的完整規則:

- `<y>` 這個 token 現在**仍然解析到同一個 commit** 時就用它,否則退回縮寫 id。
  所以移走的 tag、或 `HEAD~1` 這種本來就不是 ref 的運算式,都會退成 id。
- 剝掉 `refs/tags/` 與 `refs/remotes/` 前綴,**但不剝 `refs/heads/`**——git 真
  的會印 `HEAD detached at refs/heads/other`。
- 字面 token `"HEAD"` 不算(`sg switch --detach HEAD` 印的是 id)。用 HEAD 來
  說明 HEAD 在哪,本來也沒有資訊。
- `at` 與 `from` 的分界是**值的比較**,不是「有沒有發生過什麼」:`reset --hard`
  回到分離點會變回 `at`。
- `logs/HEAD` 裡完全沒有 checkout 條目時,git 放棄這套措辭,改印
  `Not currently on any branch.` / `* (no branch)`。

### finish 的順序,讓兩條既有規則自己產出 git 的形狀

rebase 改成 detached 模型之後,收尾的兩步順序是有意義的,而且**不需要任何特例**:

1. **先**更新分支。此刻 HEAD 還是 detached,`sg_ref_update` 的 rule 2
   (`is_current_branch`)因此是 0,那一行**不會**被鏡射進 `logs/HEAD`——分支
   於是只拿到它該有的那一行 `rebase (finish): <ref> onto <onto>`。
2. **再**把 HEAD 接回分支。old == new(兩邊都是最終 tip),而 `logs/HEAD` 從不
   做 no-op 抑制(rule 1 的不對稱性),所以 `returning to` 那行照樣寫得出來。

兩條在 Phase 17 就量好的規則,擺對順序就剛好是 git 的輸出。把兩步對調會讓五條
檢查變紅,其中包含「finish 那行 old == new」。

`--abort` 因此也變簡單了:分支全程沒被動過,只要把 HEAD 接回去。真 git 的分支
log 在 abort 後同樣一行未增,理由一模一樣——值沒變,rule 1 抑制掉了。

### 範圍是被逼著改的:`sg reset` 必須支援 detached HEAD

開工時談定的範圍是「merge / reset / 從 detached 起手的 rebase 維持拒絕」。那個
決定的前提是「detached 只可能由使用者或真 git 造成」。段二把 rebase 改成
detached 模型之後,前提消失了:**暫停中的 rebase 現在自己就是 detached**,而
`reset` 拒絕 detached 會直接拿掉「rebase 暫停中可以 reset」——那是 Phase 14 建
立並對真 git 量過的能力,7 條 interop 因此變紅。真 git 允許它,原因完全相同:
它自己的 rebase 也是 detached。

所以 `reset` 開了,`merge` 與「從 detached 起手的 rebase」維持拒絕。兩條原本釘
著「reset 在 detached 下必須拒絕」的既有測試**反過來改寫**——它們現在是錯的。
順手把 reset 三種 mode 裡逐字重複三次的 ref 寫入收斂成 `move_head_to`。

這件事本身是個教訓:**範圍決定會被後續的實作推翻**,而推翻它的是既有測試變紅,
不是有人想起來。

### 冷讀抓到的:三個「沒有斷言」而不是「斷言寫錯」

段一交出去冷讀時,18a–18d 已經驗了 HEAD 檔案、兩份 reflog、ref、`status` 與
`branch` 的措辭——**卻沒有任何一條讀 `switch` 自己的 stdout**。三個與真 git 的
分歧就活在那個縫裡:

1. `Previous HEAD position was ...` 綁在「切到分支」上,所以 detach→detach 完全
   不印。它其實屬於「**離開** detached HEAD」。修的時候我又把規則弄錯了一次
   ——真 git 只在 **commit 真的改變**時才印,同 commit 的三種組合都不印——新測
   試在一分鐘內抓到。
2. `sg switch --detach HEAD` 印出 `HEAD detached at HEAD`。
3. 描述用的緩衝區只有 512 bytes,更長的 ref 名讓整個函式失敗,而 `status` 把那
   個失敗渲染成 `Not currently on any branch.`——對一個明明 detached 的 HEAD 說
   「不在任何分支上」,那不是資訊變少,是資訊變錯。

三個都是**缺少斷言**,不是斷言寫錯。這是本專案第一次明確遇到這一類:測試覆蓋
的維度(檔案內容、退出碼、reflog)全都對,只是少了一整個維度(stdout)。

### 假覆蓋:這一輪抓到四個

1. **標籤與情境錯開一格**:顯示那組案例用 `sg commit` 移動 HEAD,但當時
   `sg commit` 在 detached 下還被拒絕,commit 根本沒發生,於是後面每個案例都往
   前挪了一格,「at 應該變 from」被斷言成反的,**而且全部通過**。之所以看得出
   來,是因為那個 helper 把比較到的字串印進 check 標籤裡。此後改用 git 移動
   HEAD:這一節測的是措辭,不該因為 sg 的寫入能力退步而靜靜變成 no-op。
2. **靠無關的理由通過**:「tag 被移走所以退回 id」那個案例,前一案剛好把 HEAD
   留在目標 commit 上,而 git 對「沒有移動的 checkout」不寫 reflog——於是分離點
   仍是更早那筆的 `HEAD~1`,退回 id 是因為 `HEAD~1` 本來就不是 ref。答案對,理
   由完全無關,把被測的守衛刪掉照樣通過。
3. **`cmp` 兩個空檔會成功**:雙軌比對前先斷言行數,否則一個什麼都沒做的
   fixture 會報告「完全相符」。
4. **走不到被測的那行**:`push` 的 detached/corrupt 訊息分流排在遠端 ref 廣播
   之後,沒有活的 remote 就永遠走不到。第一版斷言在「remote 未設定」就先失敗
   而通過。已拿掉並記進下面的無法驗證清單。

另外,**重複的字串會讓 mutation 說謊**:`"rebase (start): checkout %s"` 有兩
份,一次沒加 `/g` 的 mutation 只改到第一份,於是「plain/continue/skip 都沒紅、
只有 fast-forward 紅」看起來像是覆蓋不足,實際上是驗證工具只改了一半。收斂成
一個常數。

### 工具:`tests/mutate.sh` 的名字會進 mktemp 樣板

mutation 名字裡有 `/` 會讓 `mkdtemp` 失敗(「No such file or directory」)。腳
本是大聲失敗的(退出 1),但錯誤訊息看起來像環境問題而不像「你的名字取壞
了」,實測浪費了兩輪才看懂。名字現在會先過濾成安全字元。

### 刻意維持的 divergence

- ~~**`sg merge` 與「從 detached HEAD 起手的 `sg rebase`」仍然拒絕**~~——
  **Phase 19 已放行**,見下一節。當時的理由仍然成立:那是範圍決定而不是做不到,
  所以用 interop 釘住而不是任其漂移。釘住的代價與收益都在 Phase 19 兌現了:移
  動這個邊界就是刪掉那兩條被釘住的檢查,而那個刪除動作本身連帶弄壞了第三條,
  正好證明它們原本擋住的漂移是真的。
- **`sg status` 沒有 git 那句 `interactive rebase in progress`**。實測發現真
  git 即使沒加 `-i` 也印 interactive(2.26 起 merge backend 沿用 interactive
  機制),sg 沒有 `-i`,照抄會是誤導。

### 無法驗證(如實記錄)

- `sg_ref_detach_description` 緩衝區放不下時退回縮寫 id 的那個分支:label 長度
  上限來自 `ref_path[SG_PATH_MAX]`(4096,而 `sg_rev_parse_ref_path` 對放不下
  的名字本來就回 -1),加上 `"HEAD detached from "` 共 4116 bytes,兩個呼叫端的
  緩衝區都是 4160 → **就現有呼叫端而言永遠走不到**。兩輪冷讀各自獨立算過同一
  條式子。不是結構性死碼(未來若有第三個呼叫端傳入較小的緩衝區就會觸發),所
  以守衛保留,但如實記成沒有覆蓋。
- `cmd_push.c` 的 detached / corrupt HEAD 訊息分流:它的 HEAD 檢查排在遠端 ref
  廣播之後,要一個活的 remote 才走得到。改了但沒有斷言。
- **「`--abort` 不動分支的 reflog」這條檢查是被規則保證的,不是被程式碼選擇保
  證的**:abort 把分支還原成它本來就有的值,rule 1 的 no-op 抑制會擋掉任何寫
  入,所以改 abort 自己的程式碼**沒有任何 mutation 能讓它變紅**(試過一個)。留
  著當形狀的迴歸守衛,但它不能當成「abort 選擇不寫」的證據。
- `finish_rebase` 兩次寫入之間、以及 fast-forward 路徑 detach 之後的**中斷
  窗口本身**無法在測試裡真的觸發(沒有故障注入機制)。做法是**製造**中斷後的
  磁碟狀態再驗 `--abort` 救得回來,那覆蓋的是復原邏輯,不是窗口的存在。
- phase18e 的 branch→detach 案例對「commit 有沒有改變」這條新規則是無效覆蓋:
  HEAD 從分支出發時 `have_prev_commit` 恆為 0,那個 `memcmp` 根本走不到。它是
  重構的迴歸防護,不是新邏輯的覆蓋——標籤已經照實寫。

最終 `tests/interop.sh` 1098 項檢查(Phase 17 結束時是 998),`make test` 35 支
二進位全過(新增 `tests/test_head_detach.c`),`make sanitize` 乾淨,子指令維持
24 個(`sg switch` 新增 `--detach`)。

---

## Phase 19:merge 與 rebase 也接受 detached HEAD

Phase 18 把 detached HEAD 變成一等狀態,但刻意在三個指令前停住:merge、reset、
rebase。reset 在 Phase 18 進行中就被自己的測試逼著放行了(暫停中的 rebase 變成
detached,7 條既有 interop 因此紅)。剩下兩個是這一節。

真 git 兩者都支援,所以這裡沒有設計自由度——目標行為完全由實測決定。

### 拿真 git 當 oracle:先量,再寫

開工第一件事是把兩個指令在 detached 下的全部行為量一遍(git 2.55.0),存成一張
對照表:HEAD 檔案內容、`logs/HEAD` 追加了什麼、分支 reflog 追加了什麼、stdout
逐字、以及 commit 物件的形狀。merge 四個場景(true merge / fast-forward /
up-to-date / 衝突後 commit),rebase 五個(plain / 衝突後 continue / abort /
up-to-date / fast-forward),每個都再跑一次**分支起手**的版本當對照組。

有對照組這件事是關鍵。單看 detached 的輸出無從判斷「這一行是本來就有的、還是
detached 才有的」;並排之後,差異塌縮成很小的一張表:

| 面向 | 分支起手 | detached 起手 |
|---|---|---|
| rebase 成功訊息 | `...updated refs/heads/topic.` | `...updated detached HEAD.` |
| rebase up-to-date | `Current branch topic is up to date.` | `HEAD is up to date.` |
| `logs/HEAD` 收尾行 | `rebase (finish): returning to refs/heads/topic` | **沒有** |
| 分支 reflog 收尾行 | `rebase (finish): refs/heads/topic onto <onto>` | **沒有** |
| `--abort` 的 log | `returning to refs/heads/topic` | `returning to <orig-head 40-hex>` |
| 狀態檔 `head-name` | `refs/heads/topic` | 字面 `detached HEAD` |
| `rebase (start)` / `(pick)` / `(continue)` | 逐字相同 | 逐字相同 |

**踩到的第一個坑與程式無關**:git 預設用系統 locale 輸出,量到的是
「更新 d42fa44..466da28」而不是 `Updating ...`。訊息字串一定要先鎖 `LC_ALL=C`
再量,否則抄進測試的期望值在 CI 上必紅。這是 Phase 18 那條「oracle 的環境要自
己宣告」的同一個教訓換一個面貌出現。

### 結構性的觀察:這不是特例,是退化

把上表讀成「detached 要多寫七個特例」是錯的讀法。正確的讀法是:

Phase 18 的 `finish_rebase` 是兩步——先更新分支(HEAD 仍 detached,rule 2 不鏡
射)、再接回 HEAD(old==new,但 `logs/HEAD` 不做 no-op 抑制)。**沒有分支的時候,
這兩步各自沒有對象**:沒有分支要搬,也沒有 HEAD 要接回去(HEAD 從頭到尾就是
detached,而且已經停在正確的 commit 上)。所以 `finish_rebase` 在
`branch == NULL` 時整支 `return 0`,一個位元組都不寫——而真 git 量到的正是「什麼
都沒寫」。

merge 同理:分支路徑是「更新分支 ref,rule 2 順便鏡射一行進 `logs/HEAD`」;
detached 路徑是「直接寫 HEAD,它自己就記一行」。兩條路徑都只有一行 log,位置也
都對,沒有任何一行是為了對齊 git 而特別寫的。

這是 Phase 17 那兩條不對稱規則第二次當成**工具**而不是限制來用(第一次是 Phase
18 的 `finish_rebase` 順序)。規則選得好的時候,新狀態不需要新規則。

### sentinel:磁碟上要說「這裡沒有分支」

序列器狀態的 `orig_branch` 原本一定是分支名,`sg_rebase_state_read` 讀不到就判
損壞。detached 起手需要表達「沒有分支」,而這個表達必須與「損壞」分得開。

做法:記憶體用 `NULL`,磁碟寫字面字串 `detached HEAD`——與真 git 的
`.git/rebase-merge/head-name` 同一個 sentinel。它撞不到真的分支名,因為
`sg_ref_name_valid_for_create` 與真 git 的 check-ref-format 都拒絕含空白的 ref
名,所以叫這個名字的分支根本建不出來。

**刻意不採用的做法是「檔案不存在 = detached」。** 走到可以被 resume 的狀態時,
rebase 一定寫過那個檔案,所以檔案不見了代表有東西把它弄掉了。把缺席讀成一個合
法狀態,等於把資料遺失洗成正常運作——與 `sg_ref_head_is_detached` 堅持把損壞和
detached 分開是同一條原則。缺席仍然回 -1,而且有一條專門的單元測試釘住。

### 兩個 bug,都是測試抓的,都不是移植錯誤

**衝突合併在 detached 上 segfault。** `current_branch` 被原封不動傳進
`sg_merge_trees` 當 `ours_label`,而那個 label 會被格式化進 `<<<<<<< %s` 衝突標
記。NULL 進去就是崩潰,而且崩在使用者最容易走到的路徑上。修法是把「NULL → 
`HEAD`」算成一個 `ours_label` 區域變數,衝突標記、merge 訊息本文、摘要行三處共
用。副作用是 detached 時標記變成 `<<<<<<< HEAD`——正好就是真 git 一律用的字
(sg 在分支上用分支名,那是既有分歧,phase4b 釘著,沒動)。

**損壞 HEAD 被怪到工作目錄頭上。** merge 的「工作目錄要乾淨」閘門排在 HEAD 閘門
之前,而比對工作目錄與 HEAD 本來就得先讀 HEAD;HEAD 壞掉時比對結果變成「每個檔
案都是新增」,於是使用者被告知工作目錄髒——唯一沒問題的那部分被指控了。把 HEAD
診斷移到最前面。**這是先前就存在的**,不是這次改出來的:在 Phase 19 之前 merge
對損壞 HEAD 同樣印工作目錄髒,只是每個 detached HEAD 都在更早被拒絕,沒有測試
走得到這個組合。

### 第三個 bug:整套綠燈之下的 `(null)`

冷讀在 `cmd_rebase.c` 的 fast-forward 捷徑找到最後一個沒守衛的 `current_branch`:

```
Fast-forwarded (null) to master.
```

那條路徑**有**測試走到(phase19g 的 fast-forward 案例精確打中它),而且退出碼是
0、ref 與 reflog 全部正確——因為測試把 stdout 丟進了 `/dev/null`。這個平台的
`printf("%s", NULL)` 只印 `(null)` 不崩潰,所以三格 CI 都會靜默通過。

缺的不是一條檢查,是**一整個維度**。18a–18d 曾經因為沒有任何一條讀 `switch` 的
stdout 而漏掉三個分歧;這次一模一樣的形狀又出現一次,只是換到 rebase。現在這一
批新增的每一句 detached 專屬訊息都有 stdout 斷言:merge 摘要行、rebase 成功
行、fast-forward 行、abort 行。

一個推論值得記下來:**「覆蓋率高」與「維度齊全」是兩回事**。Phase 19 對 HEAD 檔
案、reflog、ref、commit 形狀、退出碼的斷言都很密,密到看起來不可能有洞——洞就
在唯一沒人看的那一欄。

### mutation 驗證:紅了還要紅得有道理

七條定向 mutation,六條一次命中預測。第七條(`finish_rebase` 的 `if (0)`)**被
抓到了,但理由是錯的**:NULL 分支往下走會在 `snprintf("refs/heads/%s")` 直接
segfault,rebase 根本沒機會寫出多餘的 finish 行。那條專門驗「不寫 finish 行」的
檢查因此**始終是綠的**——mutation 表面上成功,實際上什麼都沒驗到。

換成一條不會崩的 mutation(讓 `finish_rebase` 對 NULL 分支改寫一行假的
`rebase (finish)` 進 `logs/HEAD`)之後,才真的紅在該紅的地方,失敗訊息是
`no 'rebase (finish)' line is written when there is no branch (found 1)`。

同樣的紀律也用在剛修好的 `(null)` bug 上:把 bug 種回去,恰好一條檢查變紅,而
且失敗訊息逐字印出 `Fast-forwarded (null) to master.`。

另外 sentinel 那條 mutation 值得單獨記:sentinel 收斂成單一常數之後,改掉它會讓
**讀和寫同時改變**,round-trip 測試照樣綠——只有直接讀磁碟內容的那條斷言抓得到。
這是 Phase 18「重複的字串會讓 mutation 說謊」的反面:常數只有一份的時候,能抓到
它的必須是一條驗格式而不是驗自洽的斷言,那條斷言就是為此加的。

### 順手收斂

`cmd_reset.c` 的 `move_head_to` 與 `cmd_commit.c` 的 inline 版本是同一段邏輯的
兩份複本,merge 需要第三份。抽成 `sg_ref_move_head`(`include/sg/refs.h`)。
分支與 detached 的二選一容易用同一種方式錯兩次,值得只有一個地方。

### 刻意維持的 divergence

- **conflict marker 的 ours 標籤在分支上仍是分支名**,真 git 一律用 `HEAD`。
  既有分歧,phase4b 釘著,Phase 19 沒有動它——只有 detached 時因為沒有分支可
  用,才落到與 git 相同的 `HEAD`。
- **`Fast-forwarded HEAD to <upstream>.` 等訊息是 sg 自己的措辭**,不是 git 的。
  sg 在分支上本來就用自己的句子,detached 只是沿用同一套措辭,沒有理由只在這裡
  改抄 git。

  冷讀時被問過一次:五句 detached 訊息裡,為什麼只有
  `Successfully rebased and updated detached HEAD onto '%s'.` 講了 "detached",
  其餘四句都只說 `HEAD`?看起來像不一致,實際上規則是**逐句對照 git**:
  `HEAD is up to date.` 與 `...updated detached HEAD` 都是 git 的原句照抄(git
  自己就是一句說 detached、一句不說);另外三句 git 根本沒有對應輸出,是 sg 自
  己的句子,那裡用 `HEAD` 就夠——沒有分支名這件事本身已經傳達了 detached。
  統一成同一個詞會讓前兩句離開 oracle,那個代價比表面的一致性大。

### 無法驗證(如實記錄)

- **detached 起手的 rebase 在「寫完 state、還沒 detach HEAD」之間被中斷**時的可
  恢復性沒有測試覆蓋。shell 沒辦法精準砍在那個點,而 Phase 18 用的替代手法(製
  造中斷後的磁碟狀態再驗 `--abort`)在這裡沒有對應物,因為 detached 路徑的
  `finish_rebase` 什麼都不寫、根本沒有「兩次寫入之間」那個窗口可以製造。邏輯上
  自洽(冷讀逐行追過),但只有人工推理背書。
- **`sg_ref_branch_name_is_safe` 不拒絕空白字元**,所以手動把
  `.git/sg-rebase/orig-branch` 改成 `Detached HEAD`(大小寫不同)會被當成合法分
  支名,而不是判成損壞。只有直接竄改磁碟才碰得到,不經任何 `sg` 指令可達;那支
  函式的用途本來就是路徑安全而非完整驗證。不是 Phase 19 的迴歸,但 sentinel 機
  制新增了一個依賴它不誤判的地方,記著。
- **`sg merge --abort` 在 detached 下**現在有 interop 覆蓋(phase19c),但它從來
  就沒有 detached 閘門——先前它在理論上可達、實際上沒有任何測試走過。

最終 `tests/interop.sh` 1165 項檢查(Phase 18 結束時是 1098),`make test` 35 支
二進位全過,`make sanitize` 乾淨,子指令維持 24 個。

---

## Phase 20:`sg stash` 的 `-u`/`-a`/`--keep-index`/`--index`,以及放寬 apply/pop 的乾淨閘門

Phase 15 把 `sg stash` 縮到核心子集,README 當時列出的未實作清單裡有四樣:
`-u`(未追蹤檔案)、`--index`、`--keep-index`、pathspec。這一階段做了前三個,
`sg_stash_push` 也從一串位置參數改成吃 `sg_stash_push_opts`
(`include/sg/stash.h`)。**刻意不做**:`stash show`、pathspec、任何 `sg diff`
改動——`show` 需要 tree-vs-tree diff 與 `--stat`,那是下一批的範圍,這裡不碰。

### `--keep-index`:换掉 reset 的目標 tree,不是換一條新路徑

真 git 的 `--keep-index` 把工作目錄重設到**目前的 index**而不是 HEAD,index 本
身原封不動。量法是五狀態 fixture(unstaged-modify / staged-modify / staged-new
/ staged-delete / worktree-delete),`staged-delete` 那一格是唯一與其他四格反方
向移動的,所以兩個方向都各釘一條斷言。

實作是**串接兩次 `sg_apply_tree_to_workdir`**(先重設到 HEAD,再套用 index 的
tree),不是直接把它指向 index tree 的單次呼叫。單次呼叫看起來等價、描述的也是
同一個終態,但 `sg_apply_tree_to_workdir` 判斷「要不要刪掉工作目錄上的某個檔
案」是看**呼叫當下讀到的 index**列不列它,而一個被 staged 為刪除的路徑,兩邊的
index(呼叫當下的、以及目標 index tree)都不會列它,單次呼叫因此讓它在磁碟上原
封不動地留著。串接讓第二次呼叫的基準是 HEAD 的 tree(它會列出這個路徑),第一
次就把它刪乾淨。這也是真 git 自己實作這個旗標的方式。

### `-u`/`-a`:第三個 parent,而且是無條件寫的

未追蹤檔案的列舉從 `cmd_status.c` 的 `collect_untracked` 搬進
`workdir/status.c`,改名 `sg_status_list_untracked`,加一個 `include_ignored`
開關(status 與 `-u` 傳 0,`-a` 傳 1),回傳值改成排序過的——`sg_tree_build` 的
扁平清單合約要求排序輸入,第一版直接餵 `readdir` 順序進去,錯了。建對應 tree 的
`sg_tree_build_from_untracked` 加進 `tree_build.h`,與另兩支建 tree 的函式並列。

拿真 git 2.55.0 量出來的規則:

- stash commit 長出第三個 parent:一個**沒有自己 parent 的 root commit**,tree
  只放未追蹤檔案(`-a` 連 ignored 的也放)。**stash 自己的 tree(第一個
  parent)與不帶 `-u` 時逐位元組相同**——這件事單看 `-u` 的輸出看不出來,是並排
  跑一次不帶旗標的對照組,兩邊的 tree id 相減出來塌縮成的結論。
- 只要有東西要 stash,第三個 parent 就**無條件建立**,即使未追蹤清單是空的
  (寫一個空 tree)。「清單空就退回兩個 parent」的最佳化會讓寫出來的物件與真
  git 不一致——真 git 就是寫了那個空 tree。
- `-m` 不影響第二、三個 parent 的 subject,只動第一個。
- `-u`/`-a` 下,「沒東西可存」的判斷也要看未追蹤清單(用第三個 parent 會用的同
  一套過濾規則過濾過):工作目錄除了一個未追蹤檔案以外全乾淨,在純 `push` 下算
  nothing-to-save,在 `-u`/`-a` 下**不算**。
- 拿掉已收進第三個 parent 的檔案之後,`-u` 刪掉留下的空目錄,但**跳過被
  ignore 的空目錄**;`-a` 兩者都刪(因為 `-a` 本來就把 ignored 檔案一起收
  走,留下的空目錄不會再有 ignore 規則想保護的東西)。這個修剪步驟
  **開 ignore engine 的時間點在拿檔案之前,不是之後**:`.gitignore` 自己通
  常是未追蹤的,`-u` 會把它一起掃走,若修剪階段晚一步才開 ignore engine,
  看到的就是空規則、於是連本該保留的 ignored 空目錄都刪了。單元 fixture 讓
  `.gitignore` 保持未追蹤,才蓋得到這條;interop fixture 先 commit 了它,只
  蓋得到守衛本身存在,蓋不到這個時序死角。

### apply/pop:從「工作目錄必須全乾淨」收窄成「被動到的路徑不能髒」

舊的乾淨閘門是整個工作目錄與 index 都得跟 HEAD 一致。新規則
(`sg_stash_apply_check_dirty`)只看這次 merge(base = stash 第一個 parent的
tree、ours = HEAD tree、theirs = stash 的 tree)**實際會動到的路徑**:

- 該路徑在工作目錄裡的內容與 HEAD 不同 → 擋(含「內容碰巧與 stash 要寫的完全
  相同」這格——判斷式看的是「是否偏離 HEAD」,不是「是否偏離 stash」);
- 該路徑在 index 裡已經偏離 HEAD → 擋;
- 該路徑在工作目錄裡**被刪掉了** → **不擋**(沒有東西會被覆寫);
- 沒被這次 merge 碰到的路徑,不論多髒都不看。

這條規則本身依賴 `sg_merge_result_apply` 的一項改動(見下一節)才能兌現「沒碰到
的路徑維持原樣」的承諾。

### 兩處刻意與真 git 分歧

1. **`-u` 的 pop 碰撞**:真 git 遇到未追蹤半邊撞车時,已追蹤半邊照樣套用,未
   追蹤半邊逐檔失敗,把 entry 留在堆疊上——留下一個「entry 還在,但它要還原
   的檔案已經在磁碟上跟自己打架」的無出口狀態(再 pop 一次照樣撞)。sg 把未
   追蹤半邊的碰撞併進既有的事前檢查(已經同時涵蓋已追蹤與 `-u`/`-a` 未追蹤兩
   邊),整個 apply 全有全無地拒絕——安全,代價是不比照真 git 的部分套用。
2. **dirty apply 撞到 staged 改動**:真 git 的 `ours` 是 index,所以能把
   stash 三方合併進一個已經 staged 的改動(可能合出衝突);sg 的 `ours` 是
   HEAD,若在這裡放行,會直接把已 staged 的內容輾掉,所以 sg 拒絕——嚴格安全
   的一側,代價是不比照真 git 這一格的寬鬆行為。

### `sg_merge_result_apply` 的改動:本階段影響面最大的一處

它原本對 `sg_merge_trees` 的每個 clean 條目無條件寫回工作目錄。只要「工作目錄
必須乾淨」還是硬性前提,這個無條件寫回是不可見的——寫回去的內容與磁碟上原本的
逐位元組相同。放行髒工作目錄的那一刻,它就從無害變成破壞性:會把使用者留在磁
碟上、跟這次 merge 無關的髒內容原地輾掉。

現在的規則是:跳過「結果與 ours(HEAD)相同」的條目,不重讀物件、不重寫磁碟。
「是否碰到 ours」導出成獨立函式 `sg_merge_entry_touches_ours`
(`include/sg/merge.h`),讓 stash 的 index 收尾規則(下段)與 merge/rebase 的
工作目錄規則吃同一份定義——兩份獨立實作的漂移方向正好是「閘門放行了某條路
徑,收尾邏輯卻把它輾掉」,只有一份定義才不會分岔。

**`add_resolved_entry` 仍然無條件執行**,不受這條跳過規則影響:`cmd_merge.c`
與 `cmd_rebase.c` 都是拿這支函式建出來的 in-memory index 去建 commit 的
tree,少一個路徑就等於 commit 少一個檔案——這是 index 半邊,`sg_merge_entry_touches_ours`
管的是工作目錄半邊要不要動筆,兩者不是同一件事。

順帶兩個效果:`sg merge`/`sg rebase` 不再每次重寫整棵工作樹(含重組 chunk 過的
大檔案),變成只碰真的有變動的路徑;而且「兩側都刪除同一路徑」的情況不再無聲
`unlink` 掉一個剛好同名的未追蹤檔案(舊版是無條件對每個「刪除」結果呼叫刪
除,不管磁碟上那個路徑到底是什麼)。

Index 半邊需要**另外陳述同一條規則**:一個 stage-0 條目,如果它的路徑 HEAD 從
來沒有、這次 merge 也沒碰到,merge 結果裡沒有對應的 entry 可以把它接住,舊版
會讓它悄悄從 index 掉出去(變成 unstaged)。stash 的 apply/pop 走的是自己一段
專門對「沒被碰到的路徑」做第二輪掃描(對 pre-apply 的 index)來補回這個保證,
而不是指望 `sg_merge_result_apply` 自己處理——它輸入是 `sg_merge_trees` 的結
果,壓根不知道呼叫端之前的 index 長什麼樣。

### 這一輪的教訓

- **oracle 只給終態,不給機制**。`--keep-index` 量到的五種檔案狀態完美對應
  「把工作目錄重設到 index tree」,規格因此寫成「只要換一個參數」。實作時去
  查 `sg_apply_tree_to_workdir` 的刪除迴圈才發現它是拿**呼叫當下讀到的
  index**減去目標 tree——staged-delete 的路徑兩邊都不在,迴圈根本不會考慮
  它,檔案會原地留著,正好跟量到的那一格反過來。正解是前面寫的串接兩次呼叫,
  也是真 git 自己的實作方式。
- **一個測試可以「被 mutation 抓到卻什麼都沒驗到」**。把
  `remove_untracked_files` 改成無條件失敗,有兩個測試變紅——但紅的原因是
  `-u push failed`,也就是**setup 本身壞了**,不是在驗 `-2` 這個回傳值的語
  意。新測試因此改成斷言「回傳值恰好是 `-2`」,而不是「非 0」。
- **一個死角需要「守衛唯一存在的理由」那種 fixture 才照得出來**。
  `prune_empty_untracked_dirs` 的 ignore 守衛拿掉之後,`make test` 與 interop
  全過——因為兩邊 fixture 裡都沒有一個「本身被 ignore、而且是空的」目錄。補上
  之後,那個 fixture 還順帶照出一個真 bug:`.gitignore` 本身常常是未追蹤的,
  `-u` 會連它一起掃走,而修剪階段的 ignore engine 若在那之後才開,就看不到任
  何 ignore 規則了(細節見上面 `-u`/`-a` 那節)。
- **`make test` 在第一個失敗的二進位就停**,看 mutation 結果要注意某支測試可
  能根本沒跑到——本階段實際踩到:`test_merge_result_apply` 失敗導致
  `test_stash` 沒有執行,乍看之下像是 stash 沒有回歸,其實是它從沒被問過。
- **改動 merge/rebase/stash 共用的落地路徑時,`make test` 綠不算數**。實測:
  把 `add_resolved_entry` 也跟著跳過(誤把 index 半邊的規則等同工作目錄半邊)
  之後,`make test` 一條都沒紅,但 10 條 interop 檢查變紅(多數是 rebase 的
  phase4c / 17 / 18f / 19a)——本專案這條「interop 才抓得到 merge/rebase 回
  歸」的紀律,這次踩實了。

### 本階段刻意不修、留給後續的兩件事

1. **`sg_tree_build_from_workdir` 無法表示工作目錄裡的刪除**。對「已追蹤但工
   作目錄裡已經不見了」的檔案,它會退回用 index 裡的 blob
   (`include/sg/tree_build.h` 的標頭註解寫明這是刻意的,為了讓「每個條目都
   解析得出來」這個合約成立)。後果:`sg stash push` 在只有一個已追蹤檔案被
   刪除、其餘乾淨時會印「No local changes to save」、不建 stash;真 git 會
   把刪除本身記進 stash 的 tree,pop 之後檔案維持刪除狀態(已實測驗證這個
   分歧)。它有兩個呼叫端——`stash push` 與 `snapshot`——而對 snapshot 來
   說那個 fallback 正是對的(安全網要能還原一切,包括「回到刪除之前」)。修
   法必須讓兩個呼叫端拿到不同行為,這是設計決定,不是補丁,留給下一批。
2. **`restore_untracked_flat` 用裸 `snprintf` 組路徑,不檢查截斷**。它的路徑
   來自 stash 的第三個 parent 的 tree,那棵 tree 可能是真 git 寫的,不受
   `sg_status_list_untracked` 內部 `path_join` 的截斷保護。但既有的
   `sg_apply_tree_to_workdir` 對已追蹤那半邊用的**是完全相同的裸
   `snprintf`**,所以這是專案既有的一致做法(雖然不安全),只補這一支會變成
   「一類缺口只補了一半」。要修就整批一起修,不要在這裡先開一個特例。

最終 `tests/interop.sh` 1247 項檢查(Phase 19 結束時是 1165),`make test` 37
支二進位全過(新增 `tests/test_status_untracked.c`、
`tests/test_merge_result_apply.c`),`make sanitize` 乾淨,子指令維持 24 個。

## Phase 21:工作目錄的刪除可以被表示,以及路徑截斷的整批修補

接續 Phase 20 結尾「刻意不修、留給後續的兩件事」。那一段是當時決策的紀錄,
不改動;這裡記兩件事各自被怎麼處理了,以及修的過程中才發現的第三件。

### 症狀比當初記的嚴重一級

Phase 20 記的是「只刪一個已追蹤檔案時印 `No local changes to save`、不建
stash」。實測後發現那只是兩個症狀裡較輕的一個:

| 情境 | 真 git | Phase 21 之前的 sg |
|---|---|---|
| 只刪一個已追蹤檔案 | 建 stash,tree 裡沒有該檔 | 印 `No local changes to save`,不建 stash |
| 刪除 + 另一檔案被修改 | 建 stash,tree 裡沒有該檔 | 建了 stash,但 tree 裡**還有該檔**;`pop` 之後檔案**回來了** |

第二列才是真正的資料遺漏:它不是「少存了東西」,而是 stash 一輪之後**使用者的
刪除被靜默還原**,退出碼 0、沒有任何警告。當初只記了第一列,是因為那是從
`stash push` 單獨觀察得到的;第二列要 push+pop 走完整輪才看得見。

### 兩個呼叫端的分岔:必填 enum,沒有預設值

`sg_tree_build_from_workdir` 現在吃一個 `sg_workdir_missing`:
`SG_WORKDIR_MISSING_KEEP_INDEX_BLOB`(給 `sg_snapshot_create`)與
`SG_WORKDIR_MISSING_RECORD_DELETION`(給 `sg_stash_push` 建 `worktree_tree`)。
**注意同一次 `sg stash push` 裡兩種語意都會用到**——它自己也是
`sg_snapshot_create` 的呼叫端,那個安全網要的是前者。

形狀上考慮過 options struct(Phase 20 的 `sg_stash_push_opts` 是前例),但那必然
帶一套「NULL 表示預設」的慣例——**用一個把預設值制度化的形狀,去修一個預設值
造成的 bug,方向是反的**。這裡只有一個軸、兩個呼叫端,必填 enum 讓呼叫端自己
說明自己要哪一種。`= 0` 給保守那個值,讓任何意外的零初始化落在「不刪東西」那側。

### 「存在但讀不到」改成硬失敗:`sg_snapshot_create` 的合約跟著變

原本 `sg_read_file` 失敗就退回 index blob,不分「檔案被刪掉」與「權限被拒 /
變成目錄 / I/O 錯誤」。新語意下這兩者必須分開:對真的刪除,省略該路徑是對的;
對 I/O 錯誤,省略等於把檔案從 stash 裡弄丟。

兩個 policy 現在都硬失敗。對 `KEEP_INDEX_BLOB` 也一樣,因為舊行為是**靜默寫入
過期 blob 再讓破壞性操作繼續**——一個聲稱能救回、內容卻是舊的快照,比沒有快照
更糟。所以 `sg_snapshot_create` 的合約從「每個條目都解析得出來」改成
「解析得出來,否則拒絕快照」,9 個上游呼叫端會把它轉成「操作被拒絕」。

分類器的順序是關鍵,**`lstat` 一定在 `sg_read_file` 失敗之後,不可以前置探測**:

- 常見的競態是「探測後、讀之前被刪掉」。前置探測會把它判成「存在但讀不到」→
  良性競態下整個 stash/snapshot 爆掉。後置分類把它判成「不在」,那在分類當下
  是真的;只有罕見的反方向(讀失敗後檔案才出現)才硬失敗。兩者都消不掉競態,
  但要讓罕見的那個方向去承擔硬失敗。
- 零額外 syscall,成功路徑完全不動。
- **`sg_read_file` 自己的 errno 不可用**(`workdir.c`:失敗前會經過 `free()`/
  `fclose()`,而且 malloc 失敗與 EIO 走同一條 `return -1`)。後置 `lstat` 用的是
  它自己的 errno,所以不必動 `sg_read_file` 的簽名。
- 用 `lstat` 不用 `stat`,與 `stash.c` 髒閘門的存在性判定用同一個測法——push 端
  與 gate 端對「這個檔案不在了」若定義不同,會出現 push 記了刪除、gate 卻認為
  它還在。

`tests/test_snapshot.c` 那支驗 fallback 的測試**一個字都沒改就維持全綠**,因為
它的 `b.txt` 是從未建立(ENOENT)→ 分類為「不在」→ 退回 index blob。這條性質
本身就是驗證:需要改動那支測試才能過,就代表分類寫錯了。

### 修的過程才發現的第三件:空父目錄從來沒被清過

`apply.c` 與 `merge.c` 是「sg 刪掉一個已追蹤檔案」的**唯二**執行點,兩處都只
`remove()` 檔案,`src/` 裡完全沒有 `rmdir` 服務這條路徑。

**這個缺口是本階段才第一次可達的**:修法前 `worktree_tree` 必定包含 index 的
每個路徑,所以 stash apply 的 `deleted` 條目只可能來自「base 有、ours 和 theirs
都沒有」→ `sg_merge_entry_touches_ours` 回 `ours_present == 0` → `merge.c` 直接
跳過 `remove()`。也就是說**Phase 21 之前的 `sg stash pop` 從來不會刪掉任何檔案**,
新語意一上線它第一次會刪,於是第一次撞到這個缺口。這是「放寬前提會讓舊規則
靜默失效」的又一個實例——舊規則不報錯,只是不再涵蓋新出現的情況。

新增 `sg_prune_empty_parents`(`include/sg/workdir.h`),純 `rmdir` best-effort,
往上走祖先迴圈清到 repo_root 為止(root 不清)。實測(真 git 2.55.0)三個邊界:

- 目錄**被 ignore 規則涵蓋**、因刪除而變空 → **清掉**。
- 目錄裡還留著任何東西(含 ignored 檔案)→ 留著。
- 巢狀 `a/b/c/t.txt` 被刪 → `a`、`b`、`c` 一路清掉。

⚠ **第一條與 `prune_empty_untracked_dirs` 的規則相反**:那一支是 ignore-aware 的,
會**放過**「空但被 ignore」的目錄(interop 的 `build/` 檢查在守它)。兩支 prune
對 ignore 的行為刻意相反,標頭註解有寫明,**不要「統一」它們**。

### 第二件事:整批修,不是只補一支

Phase 20 記的是 `restore_untracked_flat` 與 `sg_apply_tree_to_workdir` 兩處,並
下了「要修就整批一起修」的指令。實際踏勘後是 **16 個站點**,分佈到 `merge.c`
(merge/rebase/stash 共用的落地函式)、`stash.c`、`tree_build.c`、`cmd_add.c`、
`cmd_restore.c`、`status.c`、`cmd_diff.c`。

修法是抽一支 `sg_path_join` 放進 `include/sg/workdir.h`,而不是手寫 16 次檢查
——後者等於在修重複的過程中製造第 17、18 份重複。它同時吃掉 `status.c` 與
`cmd_add.c` 兩份逐字複本的 `path_join`;`SG_PATH_MAX` 從 14 個 `.c` 各自
`#define` 收進標頭,連同 `SG_TREE_BUILD_PATH_MAX` 與 `SG_REVPARSE_PATH_MAX`
這兩個同值的獨立常數名,以及 36 處裸字面量 `4096`。

**截斷的意義是逐類決定的,不是統一的**:

| 類別 | 處理 |
|---|---|
| 寫入/刪除 | 絕不跳過(`rc = -1` 續跑 / 報錯讓該條目失敗 / `return -1`) |
| 閘門 | 往保守倒:髒檢查標 dirty、碰撞預檢標 collision。**閘門的失敗方向不可以是「放行」** |
| 回報 | 回 -1 讓 CLI 印,不可以靜默從 `sg status`/`sg diff` 掉一個檔案 |
| 清理(best-effort) | 維持靜默跳過 |

`tree_build.c` 是唯一必須**在 `sg_read_file` 之前**失敗的站點:它的迴圈把讀不到
當成「檔案不在」,截斷後的路徑若剛好不存在,一個存在的檔案就會被記成刪除。
這一站把兩件事綁在一起,所以截斷必須排在語意分岔之前落地。

`would_lose_content`(`cmd_restore.c`)因此長出第三態:原本只回 yes/no,而截斷
之下**兩個答案都是謊**——回 0 會讓 `sg restore` 在沒有警告的情況下覆寫內容。

刻意不改的:那些用 `git_dir` + 定長 hex 組路徑的 buffer(`loose.c`、`rebase.c`、
`cmd_clone.c` 等)只換掉 `#define`,沒有改成走 helper——風險輪廓不同,溢出需要
`git_dir` 本身極長。`prune_empty_untracked_dirs` 保留自己的 inline 檢查,因為它
的慣例是靜默跳過而 helper 的呼叫端會回報;它原本的註解宣稱與 `status.c` 的
`path_join` 一致,那是**錯的**(`collect_untracked` 跳過時會印 warning),已改成
講真正的理由:沒清掉的空目錄對 git 與 sg 都不可見。

### 驗證:六條定向 mutation,都紅得有道理

`sg_tree_build_from_workdir` 原本零測試覆蓋,`sg_path_join` 也沒有直接測試。
新增 `tests/test_path_join.c` 與 `tests/test_tree_build_workdir.c`,每一條斷言
都先證明會紅:

| mutation | 變紅的具名檢查 |
|---|---|
| RECORD_DELETION 塌陷成 KEEP | 「兩個 policy 產生相同 tree」「刪除沒有被表示」「空 tree」「空子樹殘留」 |
| 反向塌陷 | **`test_snapshot` 既有護欄**:`b.txt` 從快照樹消失 |
| 拿掉「讀不到」硬失敗 | 兩個 policy 各自的硬失敗斷言 |
| prune 只清一層 | 「`a/b` 該被剪掉」「`a` 該被剪掉」,而 `a/b/c` 沒紅 |
| 拿掉截斷檢查 | 四條截斷斷言 + `out_size 0` |
| 截斷檢查 off-by-one | **只有邊界那一條** |

最後一條最有價值:它只打紅一條斷言,證明邊界測試不是粗測試的冗餘複本。反向
塌陷那條則證明 `test_snapshot` 的護欄真的在守舊語意。

`sg_path_join` 的測試用**小 `out_size`** 而不是真實深目錄,這不是偷懶:macOS 的
kernel PATH_MAX 是 1024,遠低於 `SG_PATH_MAX`,真實路徑長到能截斷 4096 緩衝區
之前就會被 kernel 擋下,那個分支根本不會執行。用真實目錄寫的測試在本機會綠,
但綠的理由與待測程式碼無關。

mutation 樣式要帶右括號:`(size_t)n >= out_size` 也會前綴命中同檔案裡的
`out_size - pos`,而只改一半的 mutation 讀起來就像「已驗過」。

### interop:把當初被迫繞開的檢查放回去

Phase 20 為了避開這個 bug,額外維護了一份窄的 `p20_index_fixture`(不含
`wt_del.txt`/`staged_del.txt`)。修好之後兩份 fixture 合併,`--index` 那組檢查
現在跑在**當初必須躲開的案例**上——這是最強的回歸證據形式。

合併之後兩條 `--index` 檢查變紅,而且**紅得是真的**:sg 會把 `staged_del.txt`
從磁碟上刪掉,真 git 留著它當未追蹤檔案。用 **`1afef8b`(本階段之前的 commit)
建出的 binary 當對照組**實測,行為完全一樣——所以這是**既有分歧,不是本階段
造成的**,只是先前被窄 fixture 遮住。成因是 Phase 20 已記錄且刻意保留的
「sg 的 ours 是 HEAD 而不是 index」:`git rm --cached` 後檔案仍在磁碟上,而 sg 的
apply 只看 HEAD,於是認定 ours 有、theirs 沒有 → 刪掉。

處理方式是把該路徑排除在逐位元組比對之外,並**用兩條檢查把分歧本身釘住**
(真 git 留著 / sg 刪掉),不是靜默過濾——行為哪天變了仍然會有檢查變紅。
**沒有調整預期值到變綠**,那是這個專案最糟的失敗模式。

新增的 phase21 檢查有一條特別要記:**斷言 stdout 不是 `No local changes to save`**。
這個 bug 在「只有刪除」那個情境下的全部症狀就是一行 stdout——檔案、ref、reflog
都沒有差異。Phase 19 已經為「只驗檔案與 reflog 會漏掉整個輸出維度」付過一次
代價。

### 新可達的分歧,主動記下來

stash 了一個未 staged 的刪除 → 之後把同一路徑的刪除 staged → 再 pop:
`sg_stash_apply_check_dirty` 的 index 那一列會拒絕(`idxpos < 0 && hf != NULL`),
真 git 不會。這是同一條「ours 是 HEAD 不是 index」分歧的新組合,先前因為
stash 根本記不了刪除而不可達。interop 有一條檢查釘住 sg 自己的拒絕行為,
刻意不與真 git 比對——寫下來,而不是留給它以「interop 突然變紅」的形式被發現。

最終 `tests/interop.sh` 1276 項檢查(Phase 20 結束時是 1247),`make test` 39 支
二進位全過(新增 `tests/test_path_join.c`、`tests/test_tree_build_workdir.c`),
`make sanitize` 乾淨,子指令維持 24 個。

### 收尾冷讀抓到的:prune 會走到 repo_root 之外

尾段那批(語意分岔、prune、測試、interop)在第一輪 reviewer 交差之後才產生,
沒有任何人冷讀過,所以又交了一次**只給尾段**的審查。它抓到一個實質問題:
`sg_prune_empty_parents` 對 `relpath` 沒有任何約束,而它的標頭註解卻承諾
「up to but never including repo_root」。

**實測(不是推論)**:

- `relpath = "../sibling/f.txt"` → 第一輪就把 `cur` 剝成 `"../sibling"`,
  `sg_path_join` 產生 `repo_root/../sibling`,OS 解析成 repo 的**手足目錄**,
  `rmdir` 真的把它刪掉了。
- `relpath = "/f.txt"` → 剝完 `cur` 變成空字串,`sg_path_join` 對空 `rel` 的語意
  是「直接回傳 base」,於是 `absdir` 就是 `repo_root`,**`rmdir(repo_root)` 被呼叫**。
  真實 repo 裡 `.git` 讓它非空所以會失敗,但探針用空目錄實測時 repo_root 真的
  被刪掉了——擋住它的是 `.git` 碰巧存在,不是程式碼。

修法是加一支 `relpath_is_confined`:拒絕絕對路徑與含 `..` 元件的路徑。
**為什麼不能假設呼叫端給的是乾淨路徑**:這些 `relpath` 來自 index 條目或 merge
結果,最終來自 tree 物件,而 `src/object/tree.c` 解析 entry 名稱時**不做任何驗證**。
所以「不會跑到 repo_root 之上」必須在這裡強制,不能靠上游。

守衛刻意只擋這兩類:`...dots` 這種以點開頭的**普通目錄名**必須照樣被剪掉,
測試裡有一條專門釘住這件事——mutation 把守衛改成無差別拒絕時,那條會連同其他
四條正常 prune 檢查一起變紅,這是「守衛沒有過度嚴格」的證據。

**一個更大、本階段不修的缺口**:同樣未經驗證的路徑也被緊鄰的 `remove(abspath)`
使用(`apply.c`、`merge.c`),那在 Phase 21 之前就存在。也就是說「tree 物件裡的
entry 名稱可以是任意字串」這件事,影響的不只 prune。整批的路徑封閉性(拒絕
`..`、絕對路徑、`.git` 前綴)應該在**解析 tree / 寫 index 的那一層**做,而不是
在每個消費端各補一次。記在這裡,不在本階段處理。

長度邊界另有一條測試,用**剛好 `SG_PATH_MAX`** 長的 relpath(短一個位元組就塞得下,
所以那才是邊界)。把 `>=` 改成 `>` 的 mutation 會被抓到,但要注意**抓到它的不是
具名斷言,是二進位直接 trap(退出碼 133)**——`strcpy` 溢出一個位元組被平台的
stack 保護攔下。`mutate.sh` 的「退出碼非 0 就算被抓到」正好涵蓋這種情況,
但它證明的是「溢出真的發生」,不是「那句斷言在守」。

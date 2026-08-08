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
快取而全域關掉了 leak detection——等於洩漏偵測有個大洞。

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

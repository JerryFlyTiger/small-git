# small_git (`sg`)

用純 C11 實作、與 **git 物件格式完全相容**的版本控制工具。同一個 `.git` 目錄可以交替使用
`git` 與 `sg`——`sg` 建立的 commit 能被 `git log` 讀出、通過 `git fsck --strict`,反之亦然。

目標不是做一個教學玩具,而是針對 git 四類常見痛點提出具體做法。設計與各階段的決策紀錄見
[docs/DESIGN.md](docs/DESIGN.md)。

## 四個痛點與 `sg` 的實際做法

**CLI/UX 混亂。** `switch`(切分支)與 `restore`(還原檔案)職責分離,不再全部擠在
`checkout` 底下。指令打錯會給出建議(`sg stat` → 提示 `status`)。`status` 會直接說明下一步
可以做什麼,而不是丟一串術語。支援 `.gitignore`(逐目錄規則、否定、`**`、字元類等完整
`gitignore(5)` 語意),`sg add .` 遞迴整個目錄樹,`sg branch` 列出/建立/刪除分支。

**危險操作沒有救援。** 會造成資料遺失的操作(`switch`、`restore`、`merge`、`rebase`、
`stash push`)在執行前自動建立快照,存在 `refs/small-git/undo/`,用 `sg undo` 列出、
`sg undo <編號>` 還原。
`--force` 只跳過「確認提問」,不跳過快照——所以按 `--force` 一路做下去仍然救得回來。

**大型/二進位檔案。** 內建內容定義分塊(CDC),超過門檻的檔案切成 chunk 去重,概念類似不需要
外部伺服器的 Git LFS。**預設關閉**;啟用後純 `git` 只會看到指標文字而非檔案內容(見下方限制)。

**巨型 repo 效能。** 物件查找改用 mmap + process 層級 pack registry。在 811 commits、48MB
pack 的 repo 上,`sg log` 從 2.64 秒降到 0.008 秒,與 `git log` 同級;更重要的是成本不再隨
pack 體積成長(448KB 與 48MB 的 repo 現在一樣快)。

## 安裝

需要 `zlib`、`openssl`、`libcurl`(透過 pkg-config 偵測)與 C11 編譯器。

```sh
make release                  # 最佳化建置(-O2),產生 build/sg
sudo make install             # 安裝到 /usr/local(含 man page)
```

可用 `PREFIX` 與 `DESTDIR` 調整位置:

```sh
make install PREFIX=$HOME/.local
make install DESTDIR=/tmp/pkg PREFIX=/usr    # 打包用
make uninstall PREFIX=$HOME/.local
```

安裝後 `man sg` 有完整的指令說明。

支援 **macOS** 與 **Linux**;不支援 Windows(程式碼直接使用 POSIX API)。

## 快速開始

```sh
sg init demo && cd demo

echo "hello" > a.txt
sg add a.txt
sg commit -m "first commit"
sg log
sg status

echo "changed" >> a.txt
sg diff                       # 看尚未暫存的變更
sg add a.txt && sg commit -m "second"

sg switch -c feature          # 建立並切換到新分支
```

救援機制實際上長這樣:

```sh
echo "important work" >> a.txt
sg restore a.txt --force      # 手滑,把還沒 commit 的內容蓋掉了

sg undo                       # 1) 2026-08-04 23:17:52  restore a.txt
sg undo 1                     # 內容回來了
```

`.git` 目錄從頭到尾都是標準 git 格式,所以隨時可以直接用 `git log`、`git fsck` 檢查。

## 指令一覽

| 指令 | 說明 |
|---|---|
| `init` | 建立新的 repository |
| `add` | 將檔案或整個目錄加入暫存區(`-f` 強制加入被忽略的檔案) |
| `commit` | 建立一個 commit |
| `log` | 顯示 commit 歷史 |
| `status` | 顯示工作目錄狀態 |
| `diff` | 顯示尚未暫存的變更 |
| `switch` | 切換分支(`-c` 建立新分支,`--detach` 直接指向某個 commit) |
| `branch` | 列出、建立或刪除分支(`-d` 刪除) |
| `restore` | 還原檔案或取消暫存(`--staged`) |
| `undo` | 列出或還原自動快照 |
| `merge` | 合併另一個分支(`--abort` 中止) |
| `merge-base` | 找出兩個 commit 的最近共同祖先 |
| `rebase` | 重新套用到另一個分支之上(`--continue`/`--skip`/`--abort`) |
| `stash` | 暫存工作進度並回到乾淨狀態(`push`/`list`/`apply`/`pop`/`drop`/`clear`) |
| `clone` | 從遠端複製 repository(smart HTTP) |
| `fetch` | 從遠端取得新的 commit 與 ref |
| `push` | 將本地分支或 tag 推送到遠端(`--tags` 推送全部 tag) |
| `repack` | 將 loose object 打包成 packfile |
| `hash-object` | 計算(並可選擇寫入)物件的雜湊 |
| `cat-file` | 檢視 object 內容/型別/大小 |
| `chunk-info` | 顯示分塊儲存的診斷資訊 |
| `reflog` | 顯示某個 ref 的更新歷史(`show`/`<ref>`/`-n <count>`) |

`sg --version` 顯示版本,`sg --help` 列出所有指令。

## 與 git 的相容性

- 物件格式(blob/tree/commit)、index v2、packfile、packed-refs 都是標準格式,位元層級相容。
- commit、branch、switch、reset、merge、fetch、push、clone、stash 都會寫一筆與真 git 相容的
  reflog,`git reflog` 可以直接讀 `sg` 產生的歷史,`sg reflog` 也讀得懂 `git` 產生的。
  `<ref>@{N}` 可在任何吃 revision 的指令中使用(例如 `sg reset master@{2}`)。
  `sg rebase` 的 reflog 形狀也與真 git 一致:全程在 detached HEAD 上重放,
  `logs/HEAD` 拿到 `rebase (start)` / 每個 commit 一行 / `rebase (finish)`,而分支
  自己的 log 不論重放幾個 commit 都只增加一行(Phase 18)。
- **detached HEAD 是一等狀態**:`sg switch --detach <rev>` 進入,`sg switch <branch>`
  離開;其間 `commit`/`reset`/`branch`/`stash`/`log`/`status`/`merge`/`rebase` 都正常
  運作,狀態描述(`HEAD detached at/from <id>`)與真 git 逐字相同。detached 時
  `merge` 只移動 `HEAD`、不碰任何分支;從 detached 起手的 `rebase` 同樣不碰分支,
  連 `rebase (finish)` 那行 reflog 都不寫——因為沒有分支要搬回去(Phase 19)。
- `sg` 建立的 repo 可以直接用 `git` 操作,通過 `git fsck --strict`;`git` 建立的 repo 也可以
  直接用 `sg` 操作,包含 `git gc` 之後把 ref 收進 `packed-refs`、把物件打包成 pack 的狀態。
- 網路端實作 smart HTTP,可與真實的 git 伺服器互通(clone/fetch/push)。
- 測試套件 `tests/interop.sh` 有 1320 項檢查,大部分是拿 `sg` 的產出去餵真正的 `git` 二進位檔
  (含一個本機 `git http-backend` 伺服器)來驗證,而不是自己跟自己比對。

**唯一的例外是啟用分塊之後**——那時 tree 裡放的是指標 blob,`git checkout` 會拿到指標文字。
這與未安裝 LFS 的環境下開啟 Git LFS repo 的情況相同。

## 已知限制

誠實列出,不是待辦清單:

- **不支援 symlink 與 submodule**。
- **不讀 `core.excludesFile` / 全域 ignore 檔**;只支援逐目錄 `.gitignore` 與
  `.git/info/exclude`。
- **無法走訪超過平台 `PATH_MAX` 的目錄樹**(sg 組絕對路徑,git 用 `openat()` 相對走訪)。
  遇到時 `sg add` 明確報錯、`sg status` 印警告說明清單可能不完整——不會靜默略過。
- **不讀 `~/.gitconfig`**;commit 身分只能透過 `GIT_AUTHOR_NAME` / `GIT_AUTHOR_EMAIL` 環境變數
  設定(預設 `small_git <sg@localhost>`)。
- **寫入端不做 delta 壓縮**,每個物件各自 zlib 壓縮;讀取端則完整支援 OFS_DELTA/REF_DELTA。
- **寫入端不產生超過 2GB 的 pack**(會明確報錯而非產生壞檔);讀取端支援。
- **啟用分塊後 `refs/sg/chunks` 不可刪除**,所有 chunk 靠它在物件圖中保持可達,刪掉之後
  `git gc` 會清掉資料。`sg` 偵測到這個狀態會硬失敗,不會靜默寫出指標文字。
- **`sg stash` 支援** `push`/`list`/`apply`/`pop`/`drop`/`clear`,`push` 加
  `-u`/`--include-untracked`、`-a`/`--all`(連 ignore 的也收)、`--keep-index`
  (重設工作目錄到 index 而非 HEAD),`apply`/`pop` 加 `--index`(乾淨合併後把
  index 整個換回 push 時的樣子)。`-u`/`-a` 把未追蹤檔案存進 stash commit 的第
  三個 parent(一個只含未追蹤檔案的 root commit),已追蹤那半邊的 tree 與不帶
  旗標時逐位元組相同。`apply`/`pop` 不再要求整個工作目錄乾淨,只擋這次合併真
  的會動到的路徑上的髒改動——工作目錄裡已刪除的路徑不算擋路。**不支援**:
  `show` 與 pathspec。刻意與真 git 分歧的兩處:`-u` 撞到既有檔案時 sg 全有全無
  地拒絕(真 git 會部分套用,留下一個沒有乾淨出口的 entry);dirty apply 撞到
  **已 staged** 的改動時 sg 一律拒絕(真 git 的 ours 是 index,能合併,sg 的
  ours 是 HEAD,放行會輾掉 staged 內容)。格式本身完全相容——`sg` 建的 stash
  真 `git` 讀得到、反之亦然,`stash@{n}` 走的是與真 git 逐位元組相同的 reflog。
- **未實作 commit-graph 與 multi-pack-index**。原本規劃在 Phase 7,但物件存取層修好之後
  `sg log` 已與 `git log` 同級,邊際效益不高,留待有實際需求再評估。

## 開發

```sh
make                # 一般建置(含 -g)
make test           # 單元測試
bash tests/interop.sh   # 與真正的 git 互通測試(需先 make)
make sanitize       # ASan/UBSan 建置並跑測試

python3 tests/fuzz_ignore.py        # .gitignore 一致性模糊測試(以真 git 為 oracle)
python3 tests/fuzz_ignore.py 1000   # 跑久一點
```

`tests/fuzz_ignore.py` 隨機產生 pattern 集合與目錄樹,要求 `sg status` 的未追蹤集合與
`git status --porcelain -uall` **完全相等**。它不在 `make test` 裡(需要 python3 與真 git、
耗時較長),但動到 `src/workdir/ignore.c` 或兩個走訪之後應該手動跑一次。

切換建置模式(一般 / `release` / `sanitize`)之間要先 `make clean`——object 檔帶著當初編譯的
旗標,而 make 只在來源較新時重新連結,不清乾淨會沿用上一個模式的 object。

CI 在 Linux(gcc 與 clang)與 macOS 上建置、跑完整測試套件,並額外跑一輪 ASan/UBSan。

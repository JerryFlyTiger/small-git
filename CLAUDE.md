# Small_Git

C11 實作的簡化版 git,可執行檔 `sg`。目標是**與真 git 的磁碟格式位元相容**——
物件、index v2、packfile、pkt-line 協定都要能被真 git 直接讀懂,這條由
`tests/interop.sh`(1432 項檢查,拿真 `git` 當 oracle)守住。

在此之上有兩個真 git 沒有的東西:`src/safety/`(破壞性操作前自動快照)與
`src/storage/chunk.c`(大檔案的 content-defined chunking)。

設計決策記在 `docs/DESIGN.md`(**需要時查特定段落,不要整份讀**)。

## 建置與驗證

```bash
make                              # build/sg,含 -g
make test                         # 50 個單元測試二進位,任一失敗即整體失敗
bash tests/interop.sh             # 與真 git 的互通測試(需先 make)
make sanitize                     # clean + ASan/UBSan 重建 + 跑單元測試
python3 tests/fuzz_ignore.py      # .gitignore 一致性 fuzzer(預設 200 輪)
python3 tests/fuzz_diff.py        # patch 輸出一致性 fuzzer(預設 200 輪)
```

**前四道一次跑完:`bash tests/gates.sh`**(`--sanitize` 連第四道一起跑,
`--rebuild` 先 clean)。它印一張摘要表,每一行都附原始 log 的路徑——追不回原始
輸出的摘要只是一個新的說謊地點。重點不是少打字,是**每次都用同一套抽取規則讀
結果**:即興 grep 抽錯數字就等於誤讀閘門,而那是本專案最糟的失敗模式。看摘要時
要認得四件事(腳本註解裡有完整的 WHY):

- 印 `0 個 TU 重編` 那一行**不會給你 warning 數**:make 這次什麼都沒編,數出來
  的 0 是「沒量到」而不是「量到 0」。要真的量,用 `--rebuild`。
- `make test` 那行的 `N/50 個跑到`,N 少於 50 就是**中途中止**(Makefile 在第一
  個失敗的二進位就停),不是「其他都過了」。
- 退出碼非 0 但零 FAIL 行照樣判 FAIL:崩潰、逾時、ASan abort 都長這樣。
- interop 抓不到 `interop: N/M passed` 那一行會直接判 FAIL,不會靜靜跳過;而行
  內的 `K skipped` 只要大於 0 就標 `warn`——interop.sh 有 63 個 `skip` 呼叫,
  少一個 `python3` 或 `git` 就整組 smart-HTTP 互通跳過而它自己照樣退出 0。
  **`M` 自己也要看**:實測(Phase 17 當時,總數還是 998)關掉 `HTTP_AVAILABLE` 後
  `K` 只有 32,`M` 卻從 998 掉到 886——`skip()` 只加 `SKIP` 不加 `TOTAL`,沒被 `skip()` 明講的那 80 項連數字
  都不留。只看 `N == M` 會把「少跑了一百多項」讀成滿分。

`warn` 一律不影響退出碼(沒有 `-Werror`,讓 warning 直接判失敗會比下面的完成標準
更嚴),但四道閘門的 warning 都會數進摘要——包含 `tests/*.c` 自己的,它們是一步
編譯+連結,不會出現在 `make` 那道的 log 裡。第一點那條「沒量到 ≠ 量到 0」的但書
同樣套用在 `make test`:沒重編任何測試二進位時它會明說,不會給你一個空的 0。

`tests/test_fuzz_pack.c` 與 `tests/test_fuzz_index.c` 是二進位解析器的 fuzzer,
已含在 `make test` 裡(預設輪數只要幾秒)。`SG_FUZZ_ITERS` 調輪數、
`SG_FUZZ_SEED_BASE` 位移種子(第 i 輪用種子 base+i,失敗訊息會印出來,照著跑
就能精確重現)、`SG_FUZZ_TIMEOUT` 調看門狗(預設 600 秒,把卡死轉成乾淨失敗)。
`SG_FUZZ_BIG=1` 額外跑一個會配置約 4 GB 的截斷回歸案例,預設關閉。

**這兩支的鑑別力主要來自 sanitizer,不是斷言**——「拒絕荒謬大小」那類加固,
加固前後的回傳值完全一樣(都是 -1,只是機制從顯式拒絕變成 malloc 失敗),
只有在 ASan 底下才看得出差別。所以動到 `src/storage/pack.c` 或
`src/index/index.c` 的解析路徑時,`make test` 綠**不算數**,要跑 `make sanitize`。
細節與 mutation 驗證實測見 `docs/DESIGN.md` 的 Phase 10 段落。

**完成標準**:`make` + `make test` + `bash tests/interop.sh` 全綠。動到
`src/workdir/ignore.c` 或任一目錄走訪邏輯時,建議額外手動跑
`python3 tests/fuzz_ignore.py`(2026-08-07 起也已接進 CI 的 `fuzz-ignore` job,
但本機先跑一次能更快抓到問題)。動到記憶體管理或 pack/chunk 時加跑 `make sanitize`。

**「往共用結構加欄位」也算動到記憶體管理**,要跑 `make sanitize`。2026-08-23
(Phase 29)實測:`sg_diff_entry` 多兩個欄位之後,`tests/test_diff_out.c` 有兩處是
`malloc` 之後逐一指派欄位(沒有先 memset),新欄位因此是 malloc 垃圾,而 `print_patch`
會解參考它。**`make test` 與 interop 雙雙全綠**,只有 ASan 紅(`SEGV ... in
sg_quote_path_prefixed`,位址 `0xbebebebe`)。加欄位時要同時搜尋所有**不是**經由
配置函式建出來的實例。

**沒有 formatter 也沒有 linter**——無 `.clang-format`、無 `clang-tidy`、Makefile
無 `fmt`/`lint` 目標。全域規則裡的 `cargo fmt`/`clippy` 在這裡沒有對應物,
不要去找。另外 `CFLAGS` 只有 `-Wall -Wextra -Wpedantic`,**沒有 `-Werror`**,
所以綠燈不等於零警告——編譯輸出裡的 warning 要自己看(`Makefile:2`)。

切換建置模式之間一定要 `make clean`:object 檔不記錄自己是用哪組旗標編的
(`Makefile:76-80` 有完整說明)。`release`/`sanitize` 自帶 clean,回到普通
`make` 則要手動清。

**改任何 `include/sg/*.h` 之後也一定要 `make clean`。Makefile 沒有標頭相依追蹤**
——沒有 `-MMD`、沒有 `.d` 檔、沒有 `-include`,所以 `make` 只會重編你動過的 `.c`,
其他 TU 繼續用舊的 `.o`。改的若是結構定義(例如往 `sg_diff_entry` 加欄位),
不同 `.o` 就會對同一個結構有不同的佈局,症狀是**隨機位置的 segfault**,而且
看起來完全像是你新寫的邏輯有 bug。2026-08-23(Phase 29)實測:加一個欄位之後
`make test` 在 `cmd_stash.c` 的 `strcmp` 崩掉,`make clean && make` 之後 49/49 全過,
程式碼一行沒改。

依賴 zlib / openssl / libcurl,全走 pkg-config(`Makefile:31-40`)。只支援
macOS 與 Linux(直接用 POSIX API)。macOS 上 brew 的 openssl@3 不在預設路徑,
需設 `PKG_CONFIG_PATH`。

CI(`.github/workflows/ci.yml`)跑 ubuntu×{gcc,clang} + macos×clang 三格矩陣、
一個 ASan/UBSan job、一個 `fuzz-ignore` job,每個 branch 的 push 都跑。
**本機(macOS)測不到的:gcc、ASan/UBSan 下的 interop.sh、install/uninstall 到
staging 的驗證。本機綠燈不是充分證據。**

## 模組佈局

依賴由下而上流動。`src/<mod>/*.c` 對應 `include/sg/*.h`。

| 目錄 | 職責 | 依賴 |
|---|---|---|
| `object/` | 物件的序列化/解析,純記憶體,不碰 fs | hash |
| `index/` | index v2 二進位讀寫與有序條目操作,不讀物件 | hash |
| `util/` | zlib、SHA-1、levenshtein、LCS 表、wildmatch | — |
| `storage/` | 物件與 ref 落磁碟:loose、pack、chunk、refs、reflog、repo、revparse | object, workdir |
| `net/` | smart-HTTP:libcurl 封裝、pkt-line、transport | — |
| `workdir/` | 工作目錄:路徑/檔案 I/O、ignore、status、diff(變更清單)、apply、merge、tree_build | 幾乎全部 |
| `safety/` | snapshot(可救回的備份 ref)、rebase 序列器狀態、stash | storage, workdir |
| `cli/` | 24 個 `sg_cmd_*` + 派發器 + diff 的六種輸出格式(`diff_out.c`),唯一的組裝點 | 全部 |

- **讀物件一律走 `sg_object_read`**(`include/sg/objstore.h:16`):先 loose 再 pack。
  除了 `loose.c`/`pack.c` 自己以外不要直接呼叫底層。
- **所有 ref 與 HEAD 的寫入一律走 `sg_ref_update` / `sg_ref_set_head` /
  `sg_ref_set_head_detached`**(`include/sg/refs.h`),不要再手刻 `fopen` 寫
  ref 檔、也不要再複製一份 `write_ref_file`。第三支是 Phase 18 加的,把 HEAD
  寫成裸 40-hex(detached)。**不要改用
  `sg_ref_update(git_dir, "HEAD", ...)` 走捷徑**——它寫得出同樣的檔案,但取
  old_id 走 `sg_ref_read_path`,HEAD 還是 symref 時 hex 解析必然失敗、靜默記
  成全零,於是「從 A 分離到 B」被寫成「憑空建出 B」。

  reflog 的兩條不對稱規則(具體 ref 的 log 只在 `old != new` 時追加;
  `logs/HEAD` 永遠追加,且與它所指分支的那一行逐位元組相同)與「哪些 namespace
  才記 log」的政策閘門都收在這三支函式裡,繞過去就會靜默漏寫——不會報錯,只是
  那幾行 reflog 悄悄不存在(Phase 17)。**那兩條規則不只是要遵守的限制,也是可
  以拿來用的工具**:Phase 18 的 rebase 收尾就是靠「先更新分支(HEAD 仍
  detached → 不鏡射)、再接回 HEAD(old==new 但 HEAD 不做 no-op 抑制)」這個
  順序,不寫任何特例就長出真 git 的 reflog 形狀。

  「把 HEAD 所指的東西移到某個 commit」這個組合(detached 就寫 HEAD、否則寫
  `refs/heads/<branch>`)已經抽成 **`sg_ref_move_head`**(Phase 19),
  `commit`/`reset`/`merge` 三處共用。呼叫端傳 `branch == NULL` 表示 detached,
  **損壞的 HEAD 要由呼叫端先擋掉**——NULL 自己分不出這兩者,函式若擅自猜測就
  等於把 Phase 18 費力建立的區分洗掉。
- **`util/` 裡沒有路徑*解析***(那些在 `workdir.h`),但從 Phase 23 起有一支純位元組
  轉換 `util/quote.c`,Phase 28 起再加一支 `util/wildmatch.c`。後者是 git 的
  wildmatch 在「`/` 不特別」模式下的實作,**gitignore 與 pathspec 共用同一份**:
  `src/workdir/ignore.c` 在它上面疊 segment 層讓 `*` 停在 `/`、讓 `**` 跨目錄,
  那一層就是 gitignore 比 pathspec 多出來的東西(在真 git 裡也只差一個 WM_PATHNAME
  旗標)。**不要為了 pathspec 再寫第二個 glob 比對器。**
路徑解析、`mkdir -p`、讀寫檔
  在 `include/sg/workdir.h`(`sg_resolve_repo_path`、`sg_mkdir_parents`、
  `sg_read_file`、`sg_write_file_mkdirs`、`sg_hash_file_blob`)。找路徑工具要去
  `workdir.h`,不要去 `util/`,也不要自己再寫一份。
- **組 `base/rel` 一律走 `sg_path_join`**(`include/sg/workdir.h`,Phase 21),
  緩衝區大小用同一支標頭的 `SG_PATH_MAX`。**不要再寫裸
  `snprintf(buf, sizeof buf, "%s/%s", ...)`**:截斷後的路徑通常仍指向樹上某個
  *真實但錯誤*的位置,於是後續的 `lstat`/`unlink`/寫檔會對錯的檔案**成功**,
  而不是乾脆失敗。截斷的意義**逐類決定**——寫入/刪除絕不跳過;閘門往保守倒
  (標 dirty、標 collision,**失敗方向不可以是「放行」**);回報類回 -1 讓 CLI 印,
  不可以靜默從 `sg status`/`sg diff` 掉一個檔案。刻意的例外只有兩處:
  `prune_empty_untracked_dirs` 保留自己的 inline 檢查(它的慣例是靜默跳過),
  以及 `git_dir` + 定長 hex 那類 buffer(風險輪廓不同,只用 `SG_PATH_MAX`)。
- **印路徑給使用者看一律過 `sg_quote_path` / `_prefixed` / `_delimited`**
  (`include/sg/quote.h`,Phase 23)。含控制字元的檔名不引用的話,**真正的 ESC 位元組
  會進終端機**,可以清螢幕或改寫後續輸出的顏色。三支的分工看**版面**不看來源:
  獨佔一行的縮排清單用 `sg_quote_path`(不需要時不加引號);`diff` 的 `a/`/`b/` 用
  `_prefixed`(**引號必須包住前綴**,所以前綴折進函式裡);句子內嵌用 `_delimited`
  (無條件加引號,`format` 要改成裸 `%s`,否則會印出 `'"a\tb"'`)。
  ⚠ 回傳的是**借用指標**,指向 4 個輪替的靜態緩衝區,**不要存起來跨敘述用、不要 free**。
  ⚠ **≥0x80 原樣輸出**(等同 `core.quotepath=false`),所以 **interop 比對要在 git 側
  加 `-c core.quotepath=false`**;控制字元組不必加(兩邊都會引用)。
  ⚠ **明文禁止引用**:commit/tag 訊息與 author 字串(會破壞 `cat-file -p` 的位元組保真)、
  ref/branch/tag 名稱(真 git 也不引用,**沒有 oracle**)、`Cloning into` 這類 stdout
  資訊訊息(真 git 也不引用,已實測)。
- **「哪些路徑變了」一律走 `sg_diff_*`**(`include/sg/diff.h`,Phase 25):四個建構器
  對應 tree↔tree、tree↔index、index↔工作目錄、tree↔工作目錄,輸出一份**依 path 排序**
  的 `sg_diff_list`。**不要再手刻走訪 index 的迴圈**——改寫前 `sg diff` 只可能比較
  index 與工作目錄,原因就是「找出變更」與「印出來」是同一個迴圈。`old_tree` 傳 `NULL`
  表示空 tree,unborn HEAD 因此不必為了 diff 而寫一個空 tree 物件。
  ⚠ **衝突路徑在三種比較下是三個不同答案**(`--cached` 給一列 `U`;`<rev>` 給一般的
  `M`,因為 index 只決定成員資格、內容仍取自工作目錄;index-vs-工作目錄給 `U` 加上
  stage 2 vs 工作目錄共兩列)。三者都是實測真 git 2.55.0 得到的,不要憑直覺統一。
  ⚠ **blob 讀不到時不可以讓整個呼叫失敗**:建構器要把該路徑當成有變放進清單,
  讓渲染層帶著路徑印出 actionable 訊息。整份清單陣亡的話,連「是哪個檔案壞了」
  都沒有人知道了。
- **印 diff 一律走 `sg_diff_print`**(`include/sg/diff_out.h`,Phase 25),六種格式
  (patch/`--stat`/`--numstat`/`--shortstat`/`--name-only`/`--name-status`)。
  `sg diff` 與 `sg stash show` 共用這一份,不要再寫第二份格式化。
  patch body 從 Phase 26 起**與真 git 逐位元組相同**(`index` 行、`new file mode`/
  `deleted file mode`/`/dev/null`、context 3 的多 hunk 切分、函式名後綴、
  `\ No newline at end of file`),interop 因此對**六種格式全部**做全輸出 `cmp`。
  ⚠ 剩約 **2–3% 的位置歧異**,成因是**底層對齊演算法**(sg 用 LCS 回溯、git 預設 Myers),
  **不是**壓縮或縮排啟發式——那一層已與 `xdiff/xdiffi.c` 逐條核對過(14 個常數、
  `measure_split`、`xdl_change_compact` 的 `else if` 優先權與三個滑動下界)。
  實測 11 個殘留案例中有 6 個與 `git diff --histogram` 逐位元組相同。
  **不要去找一個不存在的評分 bug**;細節見 `docs/DESIGN.md` Phase 26。
  動到 `diff_out.c` / `diff_lcs.c` / `workdir/diff.c` 時,`make test` 綠**不算數**,
  要跑 `python3 tests/fuzz_diff.py 200 --max-failures 0` 並比對殘留數字。
- **pathspec 一律走 `sg_pathspec_*`**(`include/sg/pathspec.h`,Phase 28),比對規則
  是**三條、有順序**:字面量精確、字面量的目錄前綴、含萬用字元才走 `sg_wildmatch`。
  ⚠ **前兩條與第三條不相加**:含萬用字元的 spec **沒有**目錄前綴規則。實測真 git
  2.55.0:`o[tx]her` 對 `other/d.c` 印不出東西、`su?` 與 `s*b` 對 `sub/` 底下也是空的;
  `sub*` 之所以會中,是因為 **`*` 會跨 `/`**(pathspec 用的是 WM_PATHNAME 關掉的
  wildmatch),不是因為遞迴進目錄。憑直覺把兩者統一會讓 `sg diff` 靜默多印或少印檔案。
  ⚠ **尾綴 `/` 有意義,不是雜訊**:`sub/` 列出 sub 的內容,`a.txt/` **什麼都不匹配**
  (它問的是「這個名字底下的東西」)。`sg_resolve_repo_path_allow_root` 會把它正規化掉,
  所以 `sg_pathspec_add` 記得再接回去——這一對是唯一分得出「有沒有接回去」的測試。
  ⚠ **magic(`:(icase)`、`:!`、`:/`)一律拒絕,不可以當成字面路徑**:靜默匹配不到、
  或匹配到一個真的叫 `:!sub` 的檔案,都是在回答使用者沒問的問題。
  過濾發生在**清單建好之後**(`sg_diff_list_filter`),不在四個建構器裡面——四份各自
  的 pathspec 判斷正是 Phase 27 花一個里程碑消滅的形狀。代價是被過濾掉的檔案仍然被
  雜湊過一次,那是速度帳單不是錯答案。
- **裸引數(不加 `--`)的消歧規則是實測來的,不要簡化**(Phase 28):既是版本又是既有
  檔案 → 直接拒絕;**第一個路徑之後的每個引數都必須存在**(`sg diff a.txt HEAD` 會
  指名 HEAD 失敗,即使它是完美的版本);兩者皆非 → 「有歧義的參數」。⚠ **含萬用字元
  的引數不做存在性檢查**——`git diff '*.zzz'` 匹配不到任何東西仍然退出 0,而
  `git diff nosuch` 是硬錯誤。判斷「這看起來像 pathspec 嗎」用
  `sg_pathspec_looks_like_spec`,字元集只有那一份,與比對器放在一起。
- **改名一律走 `sg_diff_detect_renames`**(`include/sg/diff.h`,Phase 29),它是**建好清單
  之後的一個 pass**,不在四個建構器裡面(理由同 `sg_diff_list_filter`)。
  ⚠ **必須排在 `sg_diff_list_filter` 之後**。實測真 git:`git diff --cached --name-status
  -- b1.txt`(只指名改名的新那半)印 `A`,不是 `R100`——git 先用 pathspec 過濾、再偵測,
  半個配對就配不成了。順序寫反沒有任何徵兆,只會在這種情況給出錯的答案。
  ⚠ **只做 exact**(內容完全相同)。inexact(相似度分數)還沒做,interop 有兩條檢查
  **明確斷言這個分歧**,做出來的時候那兩條會紅,要更新而不是繞過。
  ⚠ **比對用 `sg_diff_side_effective_id`**(Phase 29 從 `diff_out.c` 提升成公開),
  它回 -1 表示「這個 id 沒被驗證過」——**未驗證的兩個 id 就算相同也不算內容相同**,
  那種側一律不配對。失敗方向是「不是改名」,不是「憑空生出一個改名」。
  ⚠ **`sg status` 還沒有改名列**:`sg_status_diff_staged` 是 tree↔index 的第二份實作,
  而且餵著 `apply.c` 的兩道安全閘門,要收斂得先照 Phase 27 的做法列舉分歧。
- **改名的顯示格式有兩套,不要混**(Phase 29):`--name-status` 印成**兩個獨立欄位**
  (`R100\told\tnew`,分數三位補零);`--stat`/`--numstat` 印成**一欄的壓縮配對**
  (`a/{b => z}/c.txt`)。壓縮的前後綴都在 `/` 邊界上算,而且**後綴要掃到底、在每個 `/`
  更新**(取最長的),遇到第一個 `/` 就停會印出 `{h/i => h2/i}/j.txt`。
  ⚠ **需要引用的路徑會讓壓縮整個關掉**(實測),因為括號形式加引號會在路徑中間生出引號。
- **兩種引用規則不可以「統一」**(Phase 25):`sg status --porcelain`/`-s` 用
  `sg_quote_path_porcelain`——**只要含空格就引用**,因為 `?? ` 前綴讓空格變成欄位
  分隔符;長格式與四種機器格式用 `sg_quote_path`——**空格不引**。兩者都引控制字元。
  `tests/interop.sh` 有一組**正面對撞**的檢查在守這件事(同一個 `has space.txt`,
  porcelain 必須引、長格式必須不引),因為若把兩支收斂成同一套錯的規則,
  所有 `cmp` 仍會全綠。
- **`sg_status_list_untracked` 的摺疊參數是必填的**(Phase 25),理由與
  `sg_tree_build_from_workdir` 的 `sg_workdir_missing` 完全相同:靜默挑一邊正是它要
  消滅的 bug。`safety/stash.c` 與 `workdir/tree_build.c` 一律傳「不摺疊」——它們要的是
  **真實檔名**,摺疊會讓 `sg stash -u` 存到一個目錄路徑。
- **unmerged 的七種 stage 組合只有一份對照表**(`cmd_status.c` 的 `unmerged_label`),
  長格式與 porcelain 共用。長格式標籤欄寬 **17**,staged/unstaged 區段是 **12**,
  兩者不同,不要弄混。
- **不受信任的路徑一律過 `sg_path_component_is_safe` / `sg_relpath_is_safe`**
  (`include/sg/workdir.h`,Phase 22)。它們擋 `""`/`.`/`..`/含 `/`,以及 `.git` 的
  任何大小寫變體、尾綴 `.`/空白的形式、和折掉 HFS+ 忽略碼位後等於 `.git` 的名稱。
  **守衛按「來源」放,不是按「危險動作」放**——三個來源各一道:tree 位元組
  (`sg_tree_flatten`,回 `-2` 並填 `bad_path`)、index 條目(`apply.c` 的 `remove()`、
  `cmd_restore.c` 的寫入)、argv(`cmd_add.c`)。刪掉任一道都有一組只有它擋得住的
  輸入,所以不算冗餘防禦。
  **不要把守衛下放到 `sg_write_file_mkdirs`/`sg_path_join`**:`storage/refs.c` 就是
  拿前者把 ref 檔寫進 `.git/refs/`,在那裡擋 `.git` 會直接打死 ref 寫入。
  **也不要上提到 `sg_tree_parse`**:真 git 的物件庫照收壞 tree、`cat-file -p` 讀得
  出來,上提會讓 `sg cat-file -p` 沒辦法檢視壞物件。
  ⚠ **走訪工作目錄時判斷「這是不是 gitdir」不可以用這支判準**,要用
  `strcmp(name, ".git") == 0`:真 git 會把 `.git.` 列為未追蹤目錄,用判準去跳過會
  讓 `sg status` **漏報**(Phase 22 實測)。
- **刪掉一個已追蹤檔案之後要呼叫 `sg_prune_empty_parents`**
  (`include/sg/workdir.h`,Phase 21)。執行點只有兩個:`workdir/apply.c` 與
  `workdir/merge.c` 的 `remove()` 成功之後。⚠ 它**刻意不是 ignore-aware**,與
  `safety/stash.c` 的 `prune_empty_untracked_dirs` **規則相反**:前者會清掉
  「空但被 ignore」的目錄(真 git 2.55.0 實測),後者刻意放過(interop 那條
  `build/` 必須存活的檢查在守它)。**不要「統一」這兩支。**它也會拒絕絕對路徑與
  含 `..` 的 relpath——**因為那些路徑來自 tree 物件,而 `src/object/tree.c` 解析
  entry 名稱時不做任何驗證**。⚠ 同樣未驗證的路徑也被緊鄰的 `remove(abspath)`
  使用(`apply.c`、`merge.c`),那是 Phase 21 之前就有、**尚未修**的缺口:路徑封閉性
  該在解析 tree/寫 index 那一層做,不是每個消費端各補一次。
- 已知重複(碰到時順手收斂,不要再增加下一份):`path_join` 的兩份逐字複本
  (`cmd_add.c`、`status.c`)已在 Phase 21 收斂成 `sg_path_join`,連同 14 個
  `.c` 各自的 `#define SG_PATH_MAX`、`SG_TREE_BUILD_PATH_MAX`、
  `SG_REVPARSE_PATH_MAX` 與 36 處裸字面量 `4096`——**這批不要再長回來**;
  小型 strbuf 仍重複於
  `src/workdir/apply.c` 與 `src/cli/cmd_restore.c`(**兩份並不逐字相同**:前者吃
  prefix + path,後者只吃 path,所以收斂需要先決定介面,不是「順手」);
  Phase 23 已消除它們各自的定長緩衝區。`resolve_commit_tree`
  的六份逐字複本(`cmd_switch.c`、`cmd_merge.c`、`cmd_rebase.c`、`cmd_clone.c`、
  `cmd_reset.c`、`workdir/apply.c`)已在 Phase 15 收斂成
  `sg_commit_tree_of`(`include/sg/objstore.h`);讀 commit 拿它的 tree id 一律
  呼叫這支,不要再手刻一份。index→tree 的兩種建法也已抽成
  `sg_tree_build_from_index`/`sg_tree_build_from_workdir`
  (`include/sg/tree_build.h`),前者只吃 index 的 stage-0 條目、後者會重新雜湊
  工作目錄;新程式碼要哪一種先看標頭註解,不要在呼叫端重寫這段邏輯。
  **後者從 Phase 21 起多吃一個必填的 `sg_workdir_missing`**,決定「index 有、
  工作目錄裡不見了」的路徑怎麼算:`KEEP_INDEX_BLOB`(`sg_snapshot_create`,
  安全網要能還原到刪除之前)與 `RECORD_DELETION`(`sg_stash_push` 建
  `worktree_tree`,要能表示刪除)。**沒有預設值是刻意的**——靜默挑一邊正是它
  要消滅的 bug。注意**同一次 `sg stash push` 裡兩種都會用到**(它自己也呼叫
  `sg_snapshot_create`)。另外「檔案存在但讀不到」在兩個 policy 下**都是硬失敗**,
  所以 `sg_snapshot_create` 的合約是「解析得出來,否則拒絕快照」,不是
  「一定解析得出來」;分類用的 `lstat` **必須排在 `sg_read_file` 失敗之後**,
  前置探測會把良性競態變成硬失敗(理由見 `docs/DESIGN.md` Phase 21)。
  merge/rebase/stash 共用的「把 `sg_merge_result` 落地成工作目錄+index」迴圈
  也已抽成 `sg_merge_result_apply`(`include/sg/merge.h`)。**Phase 20 起它會
  跳過「結果與 ours(HEAD)相同」的條目,不重寫工作目錄,但仍會把每個結果條目
  加進 index**(`add_resolved_entry` 無條件執行,判斷式是
  `sg_merge_entry_touches_ours`,唯一一份定義,不要再寫第二份)。`cmd_merge.c`
  與 `cmd_rebase.c` 都拿這支函式建出來的 index 去建 commit 的 tree——跟著把
  `add_resolved_entry` 也跳過會讓 merge/rebase 的 commit 悄悄少檔案,而
  `make test` 抓不到這個回歸,只有 `interop.sh` 抓得到(Phase 20 實測:10 條
  rebase 相關 interop 檢查變紅,`make test` 全綠)。改這支函式時 `make test`
  綠不算數。`env_or()`(讀 `GIT_AUTHOR_NAME`/`EMAIL` 帶 fallback)仍是**八份**
  逐字複本:`storage/reflog.c`、`storage/chunk.c`、`safety/stash.c`、
  `safety/snapshot.c`、`cli/cmd_rebase.c`、`cli/cmd_merge.c`、`cli/cmd_tag.c`、
  `cli/cmd_commit.c`。碰到時順手收斂,不要再增加下一份。
  **Phase 27 已收斂**:`sg_status_diff_unstaged`(`src/workdir/status.c`)現在是
  `sg_diff_index_workdir` 的薄轉接層,不再是第二份掃描迴圈。收斂前列舉出**恰好三類**
  分歧(`tests/test_status_diff_parity.c`),兩類修掉、一類刻意保留:
  **unmerged 列與其 stage-2 對照列不進 status 清單**(`cmd_status.c` 有自己的
  Unmerged paths 區段)。⚠ 過濾判準必須是「**前一列是 unmerged 且同路徑**」,
  不可以只比路徑——`sg_index_read` 不驗證排序也不去重,損毀的 index 會讓兩個獨立的
  同路徑列相鄰而被靜默丟掉一個,而這份清單餵的是 `switch`/`reset --hard` 的髒判斷。
  ⚠ 收斂後 **純 chmod 會讓工作目錄算成髒**(`switch`/`reset --hard`/`merge`/`rebase`
  都會擋),這與真 git 一致,已實測。
  Phase 25 又長出**一對**:`report_bad_tree_path`(`cli/cmd_diff.c:62`)與
  `report_bad_stash_tree_path`(`cli/cmd_stash.c:337`)幾乎逐字相同(都是把
  `sg_tree_flatten` 的 `-2` 轉成一行指名 `bad_path` 的錯誤)。**Phase 26 已補上兩條 interop 檢查**
  (用 `git mktree` 造出含 `..` entry 的 tree,分別走 `sg diff <rev> <rev>` 與
  `sg stash show`,斷言錯誤訊息指名該路徑),所以現在**可以安全收斂了**。
- 遠端/使用者字串轉成檔案路徑前必須先過閘門函式:`sg_ref_name_is_safe`
  (`include/sg/transport.h:38`)、`sg_ref_branch_name_is_safe`(`include/sg/refs.h:13`)。
  **建立**新 ref 時的 check-ref-format 驗證另有一支
  `sg_ref_name_valid_for_create`(`include/sg/refs.h`),branch 與 tag 共用;
  三者規則不同,標頭註解有寫分工,挑錯會留洞。
- **detached HEAD 是一等狀態(Phase 18)**。`sg_ref_resolve_head` 的 -1 現在
  **只**代表 unborn HEAD,不再兼指 detached——不要再寫「resolve 失敗 = 不在分
  支上」的程式碼。要問「是不是 detached」用 `sg_ref_head_is_detached`,它是三
  態:1 detached、0 symbolic、**-1 損壞**。損壞刻意與 detached 分開,因為
  detached 這個答案正是呼叫端用來決定「可以把裸 sha 寫進 HEAD」的依據,混在一
  起會把損壞的 HEAD 洗成看起來正常的狀態。`sg_ref_current_branch` 回 NULL 同
  樣有這兩種成因,四個曾因此拒絕的指令(merge/reset/rebase/push)都已分流。
  其中 **merge 與 rebase 在 Phase 19 已改成放行 detached、只拒絕損壞**,現在只
  剩 push 還一律拒絕(它的 HEAD 檢查排在遠端 ref 廣播之後,沒有活的 remote 就
  走不到,因此無法測試)。

  **`current_branch == NULL` 現在會流過 merge/rebase 的整條路徑**,新增或修改
  那兩支的程式碼時,任何把它餵給 `%s` 的地方都要自己守衛。Phase 19 為此修了三
  處:`sg_merge_trees` 的 `ours_label`(NULL 會在寫衝突標記時 segfault)、
  `cmd_status.c` 的 rebase 描述、以及 `cmd_rebase.c` 的 fast-forward 捷徑
  ——**最後那處印出 `Fast-forwarded (null) to master.` 而整套測試全綠**,因為
  該捷徑在其他路徑之前就 return、而測試把 stdout 丟掉了。這個平台的 `%s` 吃到
  NULL 只印 `(null)` 不崩潰,連退出碼都是 0,所以三格 CI 都會靜默通過。
  **新增 detached 專屬訊息時要一併加 stdout 斷言**,只驗檔案與 reflog 會漏掉
  整個維度。

  detached 時 merge 與 rebase 都**不碰任何分支 ref**;rebase 更是連
  `rebase (finish)` 那行 reflog 都不寫(`finish_rebase` 對 `branch == NULL`
  直接 return 0)。這不是特例,是「先搬分支、再接回 HEAD」那個兩步模型在沒有
  分支時的自然退化——真 git 實測就是這個形狀。rebase 序列器用磁碟 sentinel
  `detached HEAD`(與真 git 的 `head-name` 同字串)表示起手時 detached,記憶體
  是 NULL;**`orig-branch` 檔案缺席仍算損壞**,不可以當成 detached。
- **使用者給的 revision 字串一律走 `sg_rev_parse_commit`**
  (`include/sg/revparse.h`):`HEAD`/tag/分支/完整 40-hex/完整 `refs/...` 路徑,
  加 `~N`/`^N`/`@{N}`(Phase 17,reflog 索引,必須緊接在 ref 名之後、純數字、
  不支援 `@{<date>}`/`@{upstream}`/裸 `@{N}`),會 peel annotated tag。**不支援
  縮寫 sha**(刻意)。不要再手刻「分支名或 40-hex」的片段。列舉/刪除任一前綴
  底下的 ref 用 `sg_ref_list_under`/`sg_ref_delete_under`(`prefix` 必須以 `/`
  結尾)。
- **使用者給的 commit/tag 訊息一律先過 `sg_message_cleanup`**
  (`include/sg/object.h`),否則產生的物件 id 與真 git 不同。**例外是
  `cmd_rebase.c`**——它轉發既有訊息,必須逐位元組保真,刻意不套用。
- **`sg stash show` 走 diff 地基,不自己解 stash commit**(Phase 25)。四棵樹由
  `sg_stash_load_trees`(`include/sg/stash.h`)一次解出來:`base_tree`(parents[0],
  也就是 diff 的基準)、`theirs_tree`(stash commit 自己)、`index_tree`(parents[1])、
  以及可選的 `untracked_tree`(parents[2])。輸出走 `sg_diff_print`。
  ⚠ **預設格式是 `--stat` 不是 patch**(實測真 git)。⚠ `-u` 與 `--only-untracked`
  **不是兩個獨立布林,是同一個模式選擇器、後寫的贏**(兩種順序都實測過)。
  ⚠ `-u` 的未追蹤那半要拿**空 tree**(`NULL`)vs `untracked_tree` 比,**不是**
  `base_tree` vs `untracked_tree`——後者會把每個只存在於已追蹤側的路徑多報一筆
  幽靈刪除,於是同一路徑印兩次。
- **`sg stash` 支援 `-u`/`--include-untracked`、`-a`/`--all`、`--keep-index`、
  `--index`(Phase 20)**。`sg_stash_push` 吃 `sg_stash_push_opts`
  (`include/sg/stash.h`),不是一串位置參數。列舉未追蹤檔案一律走
  `sg_status_list_untracked`(`include/sg/status.h`,`status`/`-u`/`-a` 共用,
  `include_ignored` 開關),建對應 tree 走 `sg_tree_build_from_untracked`
  (`include/sg/tree_build.h`)。兩處刻意分歧:`-u`/`-a` 撞到既有檔案時全有全無
  拒絕(真 git 部分套用,留下無出口的 entry);dirty apply/pop 撞到已 staged
  的改動一律拒絕(真 git 的 ours 是 index、能合併,sg 的 ours 是 HEAD、放行
  會輾掉 staged 內容)。細節見 `docs/DESIGN.md` Phase 20。
- **工作目錄裡的刪除從 Phase 21 起是可以被 stash 的**:stash 自己的 tree 省略該
  路徑,`pop` 因此把它重新刪掉而不是還原;只有一個刪除也足以建出 stash(不再是
  「No local changes to save」)。index parent(`stash^2`)仍列著該檔,除非刪除
  已 staged。**這是 `sg stash pop` 第一次會真的刪掉工作目錄裡的檔案**——先前
  `worktree_tree` 必含 index 每個路徑,`deleted` 條目的 `ours_present` 恆為 0,
  `merge.c` 一律跳過 `remove()`。新可達的分歧:stash 了未 staged 的刪除、之後
  把同一路徑的刪除 staged、再 pop,sg 拒絕而真 git 不拒絕(同一條「ours 是 HEAD
  不是 index」)。細節見 `docs/DESIGN.md` Phase 21。

## 核心型別速查

行號是撰寫當下的錨點,可能漂移——以名稱為準。

| 概念 | 型別 | 位置 |
|---|---|---|
| 物件種類 | `sg_obj_type` | `include/sg/object.h:8` |
| 已解析物件(content **借用**呼叫者的 buffer) | `sg_object` | `include/sg/object.h:33` |
| tree / commit / tag | `sg_tree`, `sg_commit`, `sg_tag` | `object.h:46,70,91` |
| index 與條目 | `sg_index`, `sg_index_entry` | `include/sg/index.h:23,8` |
| 可成長 byte buffer | `sg_buf` | `include/sg/http.h:6` |
| ref 廣播 | `sg_ref_adv`, `sg_remote_ref` | `include/sg/transport.h:14,19` |
| push 請求/回報 | `sg_push_ref_update`, `sg_push_report` | `include/sg/transport.h:72,78` |
| chunk pointer | `sg_chunk_pointer` | `include/sg/chunk.h:20` |
| SHA-1 長度常數 | `SG_SHA1_RAW_LEN` / `_HEX_LEN` | `include/sg/hash.h:6-7` |

版本字串只有一處定義:`SG_VERSION`(`include/sg/version.h:13`),同時被
`sg --version`、transport 的 agent 字串、`docs/sg.1` 的 `.TH` 行引用——改版本
要同步 man page。

## 程式碼慣例

- 對外符號一律 `sg_` 前綴 + snake_case;typedef 不加 `_t` 後綴;檔內 static
  輔助函式**不加** `sg_` 前綴。include guard 用 `SG_<檔名大寫>_H`,不用 `#pragma once`。
- **錯誤回傳 `int`:0 成功、-1 失敗。沒有統一的 error 型別或 macro**,語意靠
  標頭註解描述。少數讀取路徑有第三態 `-2`(「指標有效但資料損壞」,如
  `sg_chunk_read_blob`,`include/sg/chunk.h:127-131`),簽名要看標頭註解才知道。
- 錯誤訊息由 CLI 層印,底層原則上不印——但 `pack.c` 與 `http.c` 是既有例外,
  它們自己 `fprintf(stderr, "sg: ...")`。不要假設分層是乾淨的。
- 使用者可見輸出:錯誤走 stderr 且前綴 `sg: `;用法錯誤印 `usage: sg <cmd> ...`
  (**不帶** `sg:` 前綴);退出碼只有 0 與 1,沒有第三種。無 `sg_die`/`sg_error`
  helper,各處自己 `fprintf`。
- 記憶體:標準 malloc/free,無 arena。每個複合結構配一個 `_free`。標頭註解會
  寫明 owned 還是 borrowed,新 API 沿用同樣措辭。兩個**故意**不釋放的
  process-lifetime 快取:`pack.c:498` 的 mmap pack registry、`chunk.c:744` 的
  keepalive cache。這兩個曾是 CI 關掉 leak detection 的理由,但那個理由是錯的
  ——兩者都掛在檔案層級全域變數上,LSan 把全域當 root,still-reachable 不算
  leak。**CI 的 ASan job 現在開著 `detect_leaks=1`**,新的 process-lifetime
  快取要照樣掛在全域上,否則會讓 CI 變紅。
- 新增子指令要動三個地方(**不必改 Makefile**,`src` 是 glob 進去的):新增
  `src/cli/cmd_xxx.c`、在 `include/sg/cli.h` 加宣告、在 `src/cli/cli.c` 的
  `COMMANDS[]`(`:13`)加說明並在派發鏈(`:63` 起)加一組 `strcmp`。

  會覆寫工作目錄的新指令要分開決定兩件事,不要當成同一個選擇:

  **(1) 閘門**——髒工作目錄/進行中的 rebase/進行中的 merge 該不該擋?
  - `switch`/`merge`:直接拒絕。`switch` 對 rebase 與 merge 各有一道**明確**
    閘門(`cmd_switch.c`,Phase 14 與 Phase 16),都在任何副作用之前、
    `--force` 繞不過、`-c` 也不會建出分支。**不要靠 `sg_safe_apply_tree` 的
    髒確認代打**——`--force` 正好繞過它,Phase 16 的 bug 就是這樣來的。
  - `stash apply`/`stash pop`:**Phase 20 起不再是全域拒絕**,改成
    `sg_stash_apply_check_dirty`(`include/sg/stash.h`)只擋這次合併真的會動
    到的路徑上的髒改動;工作目錄裡已刪除的路徑不擋。進行中的 rebase 仍然直接
    拒絕(與 switch/merge 一致),這條沒變。
  - `reset --hard`:走 `sg_safe_apply_tree`(確認 + 快照)。
  - `stash push`:**不擋**——「工作目錄是髒的」是它的輸入而不是危險,所以它
    直接呼叫 `sg_apply_tree_to_workdir` 並自己先 `sg_snapshot_create`;用
    `sg_safe_apply_tree` 會因為 `apply.c:311-312` 把 rebase 狀態算成 dirty
    而在 rebase 中誤擋,而且非互動時會要求 `--force`。

  **(2) 收尾**——結束哪些進行中的狀態?
  - `MERGE_HEAD`:任何**真的執行下去**的覆寫工作目錄操作都清掉(真 git
    2.55.0 實測;`stash push` 也清,且不警告——sg 額外印一行 stderr,狀態仍
    完全一致)。`switch` 不在此列:它在上面那道閘門就拒絕了,永遠走不到收尾
    (真 git 的 `switch` 也拒絕,會清的是 `checkout -f`,而 sg 沒有
    `checkout`)。
  - rebase 序列器狀態:**除了 rebase 自己的子指令,誰都不准動**
    (Phase 14 實測)。`stash push` 是「不擋也不清、原封不動」的代表案例。
  - `cmd_undo.c` 仍是唯一例外(無真 git 對應物),它在回傳後自己清。

  **「merge 是否進行中」一律用 `sg_merge_head_exists`**(`include/sg/merge.h`)。
  `sg_merge_head_read` 把「沒有 merge」與「狀態損壞」壓成同一個 -1,拿它當
  判斷式會讓損壞的 `MERGE_HEAD` 被當成「沒有 merge」——結果是 switch 永久拒絕
  而沒有任何指令清得掉它。`src/` 裡 `sg_merge_head_read` **只剩 `cmd_commit.c`
  一個呼叫端**,因為只有它真的需要那個值(第二個 parent);它先問 `_exists`
  再問 `_read`,讀不出來就照真 git 拒絕,不會靜默產出單 parent 的 commit。
  新增「問 merge 在不在」的地方不要再引入第二個 `_read` 呼叫端(Phase 16)。

## 測試慣例

- 50 個獨立單元測試 `.c`,**沒有共用 header、沒有測試框架**。每檔自帶
  `static int failures = 0;` 與同名 `CHECK(cond, ...)` 巨集(失敗印
  `FAIL %s:%d` 並 `failures++`,**不 abort**),`main` 結尾 `failures > 0` 就
  `return 1`。要新增測試就照抄 `tests/test_confirm.c`(75 行,最短完整範例)。
- **丟進 `tests/` 就會被跑到**——`Makefile:48` 用 `find tests -name '*.c'`
  自動收集,不需登記。測試連結 `LIB_OBJS`(排除 `main.o`),可直接呼叫內部函式。
- 需要暫時 repo 時複製現有的 `make_tmp_repo()`(如 `tests/test_apply_tree.c:28`):
  `mkdtemp("/tmp/sg_<name>_test_XXXXXX")` + `sg_repo_init()`,setup 失敗用
  `exit(1)`(語意上與斷言失敗不同)。**沒有共用 fixture helper,不要去找。**
  多數測試不清 `/tmp`,殘留是已知現象。
- 跑單一測試:`make build/tests/test_foo && build/tests/test_foo`。
- **本專案出過兩次「根本不會 FAIL 的空測試」。新增或修改測試後,必須先證明它
  會紅再相信它**。用 `bash tests/mutate.sh <名稱> <檔案> <perl 運算式>
  [<測試二進位>|--interop]`:它會把工作樹複製到暫存目錄、在副本裡套用 mutation、
  完整重建、回報哪些具名檢查變紅。**不要用 `git checkout --` 還原**,曾因此清掉
  整個檔案。這一步由主對話執行,不交給寫測試的人自己驗。

  腳本內建四條踩過坑才有的行為,看輸出時要認得:每輪都從乾淨複本**完整重建**
  (舊 `.o` 的 mtime 會讓 make 跳過重編,mutation 會無聲跨輪累積);**退出碼非 0
  就算被抓到**,不論有沒有 FAIL 行(邊界 mutation 曾讓測試二進位 segfault,只
  grep FAIL 會誤報成死角);perl 運算式**沒匹配到任何東西時直接退出碼 3**,不會
  假裝跑完(什麼都沒改當然不會紅,那是假陰性最常見的來源);以及 `SG_MUTATE_TIMEOUT`
  (預設 300 秒)把**卡死**轉成標示為「逾時」的失敗——mutation 可以讓歸併迴圈的游標
  不再前進而永遠不結束,而「永遠不退出」既不是 0 也不是非 0,舊版腳本只會安靜地
  佔住終端機(Phase 25 實測卡了三十分鐘)。**逾時與崩潰是分開標示的**,因為兩者都只
  證明「改壞了會出事」,不證明那條具名斷言有鑑別力。

  **沒紅的 mutation 有三種,不要混為一談**(Phase 25):**真死角**(那個維度沒有測試,
  要補,而且要錨在外部 oracle);**冗餘守衛**(真正的防線在下一層,把守衛刪掉讓
  mutation 打在那裡);**數學上不可觀測**(那個值後續會被無條件覆寫,記下證明、
  換一條驗得到的性質)。只有第一種是覆蓋缺口,把三者當成同一件事會讓下一個人
  去找一個不存在的測試。

  ⚠ **逐站點 vs 整批**:腳本註解說「字面量出現不只一次一定要加 `/g`」,那是為了回答
  「這條規則有沒有被強制」。要回答「**每個站點是不是各自有覆蓋**」時 `/g` 恰恰是錯的
  ——它把各站的結果糊成一團,只要任一站有覆蓋整體就變紅。分辨同字面量的站點
  用周邊文脈(縮排深度、前一行的呼叫)就夠了,不必靠 `/g`(Phase 25 實測:
  `sg_chunk_effective_id` 的兩個站點,一個有覆蓋、一個是真死角)。

  紅了還不夠,**要紅得有道理**:確認失敗訊息指的正是你要驗的性質。曾經有測試
  在 2-commit 的 fixture 下確實變紅,但原因是 root commit 沒有 parent,與守衛
  無關;也曾有一組「看起來在驗語法」的斷言,實際上是被無關的越界檢查擋下的。
  另外,**冗餘的防禦性檢查會把驗證點藏起來**——與既有程式碼重複的守衛刪掉後
  零測試變紅,真正的防線在下一層,mutation 要打在那裡才算數(Phase 17)。

## 委派(判準與固定條款見全域 `~/.claude/CLAUDE.md`,此處只記本專案特有的)

- 本專案的成本問題是**讀檔案的工作留在主對話**:2026-08-07 基準為平均 context
  356 K、派工密度 2.0 次/100 回合。里程碑開工前先平行派 `surveyor` 分不重疊
  範圍踏勘(本檔就是這樣寫出來的),不要邊做邊在主對話讀 `src/`。
- 派 surveyor 時按上面的模組表切範圍,一個 agent 吃 3–4 個子目錄剛好;`cli/`
  的 19 個 `cmd_*.c` 太碎,要指定具體指令而不是整個目錄。
- **subagent 回報的「全綠」在本專案屢次是錯的**——`make test` / interop.sh 的
  最終閘門由主對話親自重跑,不採信轉述的數字。
- 派工規格要額外寫明:完成標準見本檔「建置與驗證」(含 interop.sh,agent 常
  只跑 `make test` 就宣告完成),動到 ignore/走訪時要跑 fuzz_ignore.py,
  動到 diff 輸出時要跑 fuzz_diff.py 並**回報實際 mismatch 數**(不是「有沒有失敗」)。

## token 節流

- 一次只開一個模組,改完就 `make test` 驗證,不整包重讀。
- 查找類問題派 `Explore`,不要在主對話掃檔案。
- `make test` / interop.sh 輸出用 `2>&1 | tail -40`,或先寫檔再抓 FAIL 行,
  不要讓上千行原始輸出留在對話裡。
- 里程碑邊界 `/clear`,新 session 從本檔 + `docs/DESIGN.md` 最近幾筆續作。

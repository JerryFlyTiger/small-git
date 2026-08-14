# Small_Git

C11 實作的簡化版 git,可執行檔 `sg`。目標是**與真 git 的磁碟格式位元相容**——
物件、index v2、packfile、pkt-line 協定都要能被真 git 直接讀懂,這條由
`tests/interop.sh`(1085 項檢查,拿真 `git` 當 oracle)守住。

在此之上有兩個真 git 沒有的東西:`src/safety/`(破壞性操作前自動快照)與
`src/storage/chunk.c`(大檔案的 content-defined chunking)。

設計決策記在 `docs/DESIGN.md`(**需要時查特定段落,不要整份讀**)。

## 建置與驗證

```bash
make                              # build/sg,含 -g
make test                         # 35 個單元測試二進位,任一失敗即整體失敗
bash tests/interop.sh             # 與真 git 的互通測試(需先 make)
make sanitize                     # clean + ASan/UBSan 重建 + 跑單元測試
python3 tests/fuzz_ignore.py      # .gitignore 一致性 fuzzer(預設 200 輪)
```

**前四道一次跑完:`bash tests/gates.sh`**(`--sanitize` 連第四道一起跑,
`--rebuild` 先 clean)。它印一張摘要表,每一行都附原始 log 的路徑——追不回原始
輸出的摘要只是一個新的說謊地點。重點不是少打字,是**每次都用同一套抽取規則讀
結果**:即興 grep 抽錯數字就等於誤讀閘門,而那是本專案最糟的失敗模式。看摘要時
要認得四件事(腳本註解裡有完整的 WHY):

- 印 `0 個 TU 重編` 那一行**不會給你 warning 數**:make 這次什麼都沒編,數出來
  的 0 是「沒量到」而不是「量到 0」。要真的量,用 `--rebuild`。
- `make test` 那行的 `N/35 個跑到`,N 少於 35 就是**中途中止**(Makefile 在第一
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

**沒有 formatter 也沒有 linter**——無 `.clang-format`、無 `clang-tidy`、Makefile
無 `fmt`/`lint` 目標。全域規則裡的 `cargo fmt`/`clippy` 在這裡沒有對應物,
不要去找。另外 `CFLAGS` 只有 `-Wall -Wextra -Wpedantic`,**沒有 `-Werror`**,
所以綠燈不等於零警告——編譯輸出裡的 warning 要自己看(`Makefile:2`)。

切換建置模式之間一定要 `make clean`:object 檔不記錄自己是用哪組旗標編的
(`Makefile:76-80` 有完整說明)。`release`/`sanitize` 自帶 clean,回到普通
`make` 則要手動清。

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
| `util/` | zlib、SHA-1、levenshtein、LCS 表 | — |
| `storage/` | 物件與 ref 落磁碟:loose、pack、chunk、refs、reflog、repo、revparse | object, workdir |
| `net/` | smart-HTTP:libcurl 封裝、pkt-line、transport | — |
| `workdir/` | 工作目錄:路徑/檔案 I/O、ignore、status、apply、merge、tree_build | 幾乎全部 |
| `safety/` | snapshot(可救回的備份 ref)、rebase 序列器狀態、stash | storage, workdir |
| `cli/` | 24 個 `sg_cmd_*` + 派發器,唯一的組裝點 | 全部 |

- **讀物件一律走 `sg_object_read`**(`include/sg/objstore.h:16`):先 loose 再 pack。
  除了 `loose.c`/`pack.c` 自己以外不要直接呼叫底層。
- **所有 ref 與 HEAD 的寫入一律走 `sg_ref_update` / `sg_ref_set_head` /
  `sg_ref_set_head_detached`**(`include/sg/refs.h`),不要再手刻 `fopen` 寫
  ref 檔、也不要再複製一份 `write_ref_file`。第三支是 Phase 18 加的,把 HEAD
  寫成裸 40-hex(detached)。**不要改用
  `sg_ref_update(git_dir, "HEAD", ...)` 走捷徑**——它寫得出同樣的檔案,但取
  old_id 走 `sg_ref_read_path`,HEAD 還是 symref 時 hex 解析必然失敗、靜默記
  成全零,於是「從 A 分離到 B」被寫成「憑空建出 B」。reflog 的兩條不對稱規則(具體 ref 的 log 只在
  `old != new` 時追加;`logs/HEAD` 永遠追加,且與它所指分支的那一行逐位元組
  相同)與「哪些 namespace 才記 log」的政策閘門都收在這兩支函式裡,繞過去就
  會靜默漏寫——不會報錯,只是那幾行 reflog 悄悄不存在(Phase 17)。
- **`util/` 裡沒有字串緩衝區、也沒有路徑處理**。路徑解析、`mkdir -p`、讀寫檔
  在 `include/sg/workdir.h`(`sg_resolve_repo_path`、`sg_mkdir_parents`、
  `sg_read_file`、`sg_write_file_mkdirs`、`sg_hash_file_blob`)。找路徑工具要去
  `workdir.h`,不要去 `util/`,也不要自己再寫一份。
- 已知重複(碰到時順手收斂,不要再增加下一份):`path_join` 逐字重複於
  `src/cli/cmd_add.c:174` 與 `src/cli/cmd_status.c:118`;小型 strbuf 重複於
  `src/workdir/apply.c:175` 與 `src/cli/cmd_restore.c:104`。`resolve_commit_tree`
  的六份逐字複本(`cmd_switch.c`、`cmd_merge.c`、`cmd_rebase.c`、`cmd_clone.c`、
  `cmd_reset.c`、`workdir/apply.c`)已在 Phase 15 收斂成
  `sg_commit_tree_of`(`include/sg/objstore.h`);讀 commit 拿它的 tree id 一律
  呼叫這支,不要再手刻一份。index→tree 的兩種建法也已抽成
  `sg_tree_build_from_index`/`sg_tree_build_from_workdir`
  (`include/sg/tree_build.h`),前者只吃 index 的 stage-0 條目、後者會重新雜湊
  工作目錄;新程式碼要哪一種先看標頭註解,不要在呼叫端重寫這段邏輯。
  merge/rebase/stash 共用的「把 `sg_merge_result` 落地成工作目錄+index」迴圈
  也已抽成 `sg_merge_result_apply`(`include/sg/merge.h`)。`env_or()`(讀
  `GIT_AUTHOR_NAME`/`EMAIL` 帶 fallback)有**八份**逐字複本:`storage/reflog.c`、
  `storage/chunk.c`、`safety/stash.c`、`safety/snapshot.c`、`cli/cmd_rebase.c`、
  `cli/cmd_merge.c`、`cli/cmd_tag.c`、`cli/cmd_commit.c`。碰到時順手收斂,
  不要再增加下一份。
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
  樣有這兩種成因,四個會因此拒絕的指令(merge/reset/rebase/push)都已分流。
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
  - `switch`/`merge`/`stash pop`:直接拒絕。`switch` 對 rebase 與 merge 各有
    一道**明確**閘門(`cmd_switch.c`,Phase 14 與 Phase 16),都在任何副作用
    之前、`--force` 繞不過、`-c` 也不會建出分支。**不要靠
    `sg_safe_apply_tree` 的髒確認代打**——`--force` 正好繞過它,Phase 16 的
    bug 就是這樣來的。
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

- 35 個獨立單元測試 `.c`,**沒有共用 header、沒有測試框架**。每檔自帶
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

  腳本內建三條踩過坑才有的行為,看輸出時要認得:每輪都從乾淨複本**完整重建**
  (舊 `.o` 的 mtime 會讓 make 跳過重編,mutation 會無聲跨輪累積);**退出碼非 0
  就算被抓到**,不論有沒有 FAIL 行(邊界 mutation 曾讓測試二進位 segfault,只
  grep FAIL 會誤報成死角);perl 運算式**沒匹配到任何東西時直接退出碼 3**,不會
  假裝跑完(什麼都沒改當然不會紅,那是假陰性最常見的來源)。

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
  只跑 `make test` 就宣告完成),以及動到 ignore/走訪時要跑 fuzz_ignore.py。

## token 節流

- 一次只開一個模組,改完就 `make test` 驗證,不整包重讀。
- 查找類問題派 `Explore`,不要在主對話掃檔案。
- `make test` / interop.sh 輸出用 `2>&1 | tail -40`,或先寫檔再抓 FAIL 行,
  不要讓上千行原始輸出留在對話裡。
- 里程碑邊界 `/clear`,新 session 從本檔 + `docs/DESIGN.md` 最近幾筆續作。

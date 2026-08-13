#!/bin/bash
# 完成標準的四道閘門,跑成一支,印精簡摘要 + 原始 log 的路徑。
#
# CLAUDE.md 的「建置與驗證」規定完成標準是 make + make test + interop.sh 全綠,
# 動到記憶體管理或 pack/chunk 時加跑 make sanitize。這支腳本把那條管線固定
# 下來,主要不是為了少打字,而是為了**每次都用同一套抽取規則讀結果**——
# 即興 grep 抽錯數字會導致誤讀閘門結果,那是本專案最糟的失敗模式。
#
# 用法:
#   bash tests/gates.sh              # make + make test + interop.sh
#   bash tests/gates.sh --sanitize   # 再加 make sanitize(會 clean 兩次,慢)
#   bash tests/gates.sh --rebuild    # 先 make clean,讓 warning 數真的有意義
#
# 退出碼:全部跑到的閘門都綠才 0,否則 1(參數錯誤是 2)。
#
# 不含在這裡的:python3 tests/fuzz_ignore.py 是**條件式**閘門(只在動到
# ignore 或目錄走訪時才跑),不是每次都要,所以刻意不併進來;CI 的觀察用
# `gh run watch` 就夠。
#
# WHY(每一條都是踩過坑才有的,不要拿掉):
#
#   1. warning 數要連「有幾個 TU 真的重編」一起報。CFLAGS 沒有 -Werror,所以
#      綠燈不等於零警告——但 build/ 是最新的時候 make 什麼都不編,warning 自然
#      是 0。那個 0 完全不代表沒有警告,只代表沒人看。不標註重編數就會把
#      「沒量到」當成「量到 0」,而這種誤判的方向永遠是「已驗過」。
#
#   2. make test 在第一個失敗的二進位就停(Makefile 的 `$$t || exit 1`),所以
#      FAIL 行的數量是**截斷過的**,而且後面的二進位根本沒跑。摘要報
#      「跑到幾個 / 共幾個」,少於總數就是中途中止,不是「其他都過了」。
#
#   3. 退出碼非 0 但一行 FAIL 都沒有,仍然是失敗(段錯誤、逾時、建置失敗)。
#      只數 FAIL 行會把崩潰讀成綠燈。這條與 tests/mutate.sh 的規則 2 同源。
#
#   4. interop.sh 的 `interop: N/M passed, K skipped` 只有跑到最後才會印。抓
#      不到那一行時要明講「摘要行不存在」,絕不能靜靜地當成沒事。K > 0 一律
#      標 warn:interop.sh 有 63 個 skip 呼叫,少一個 python3 或 git 就整組
#      smart-HTTP 互通跳過,而它自己照樣退出 0。而且 K 還會低估——實測把
#      HTTP_AVAILABLE 關掉,K 只有 32,但 M 從 998 掉到 886(skip() 只加 SKIP,
#      不加 TOTAL,所以沒被 skip() 明講的那 80 項是連數字都不留就消失的)。
#      這就是為什麼 M 自己也要看,不能只看 N == M。
#
#   5. macOS 的 ASan **不做** leak 偵測(detect_leaks is not supported on this
#      platform),所以本機 make sanitize 綠燈對記憶體洩漏是零證據——那要靠
#      CI 的 ubuntu ASan job。Phase 16 就是這樣讓一個 strdup 洩漏過了本機。
#
#   6. sanitize 之後一定要 make clean。object 檔不記錄自己是用哪組旗標編的,
#      不清就直接 make 會把 sanitizer 的 .o 連進普通建置(Makefile:76-80)。

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
cd "$PROJECT_ROOT" || exit 1

WANT_SANITIZE=0
WANT_REBUILD=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --sanitize) WANT_SANITIZE=1 ;;
        --rebuild)  WANT_REBUILD=1 ;;
        -h|--help)
            # 印完整個開頭註解區塊(到第一個非註解、非空白行為止)。三個細節:
            # 不要改成固定行號(之前寫死 2,30p,WHY 3~6 剛好被切掉,而那幾條正
            # 是教人怎麼正確讀懂綠燈的);不要用 "$0"(上面已經 cd 過了,
            # `cd tests && bash gates.sh --help` 會讀不到自己而靜靜印空的);
            # 空行要跳過而不是當終止條件——否則有人在 shebang 後面加一行空白
            # 排版,--help 就永遠印零行,而且照樣退出 0。
            awk 'NR > 1 { if (/^#/) print; else if (NF) exit }' "$SCRIPT_DIR/$(basename "$0")"
            exit 0
            ;;
        *)
            echo "usage: bash tests/gates.sh [--sanitize] [--rebuild]" >&2
            exit 2
            ;;
    esac
    shift
done

LOGDIR=$(mktemp -d "${TMPDIR:-/tmp}/sg_gates_XXXXXX") || exit 1
echo "log 目錄: $LOGDIR"
echo ""

# 摘要列累積在這裡,最後一次印出來——閘門跑很久,中途的輸出會被捲走。
ROWS=()
OVERALL=0

# row <狀態> <閘門名> <說明> <秒數> <log 檔名>
#
# 定寬欄位只放 ASCII:printf 的 %-Ns 數的是位元組,中文一字三位元組,拿它排
# 中文說明會歪掉。所以說明固定放最後一欄,不參與對齊。
row() {
    ROWS+=("$(printf '%-4s %-9s %5ss  %-12s  %s' "$1" "$2" "$4" "$5" "$3")")
    if [ "$1" = "FAIL" ]; then
        OVERALL=1
    fi
}

# 只認建置工具的診斷格式,三種:
#   1. 「src/x.c:12:5: warning: ...」——檔案:行[:欄]。
#   2. 「<command-line>: warning: "FOO" redefined」——GCC 的巨集重定義走這個
#      格式,沒有檔名。本專案特別需要它:Makefile:5-18 整段就是在講
#      _POSIX_C_SOURCE / _DEFAULT_SOURCE / _DARWIN_C_SOURCE 這組 feature-test
#      巨集的跨平台衝突,真踩到的話警告正是長這樣。
#   3. 「clang: warning:」「/usr/bin/ld: warning:」「make: warning: Clock skew」
#      ——工具自己發的,前面可能帶完整路徑,所以不能用 ^ 直接錨住工具名。
#      `make` 也收進來是因為 clock skew 會讓 make 的「誰比較新」判斷失準,連
#      帶讓 count_tus 那條防線不可信,那種事必須看得見。
# clang 結尾的「3 warnings generated.」沒有冒號,不會重複計數。
#
# 不要放寬成 ': warning:' —— sg 自己執行時就會印「sg: warning: 忽略遠端不合法
# 的 ref 名稱 ...」,而 make test 與 make sanitize 的 log 裡有測試二進位的輸出。
# 實測:在 tests/ 種一個未使用變數,寬鬆版數出 7 個 warning(1 個真的 + 6 個
# test_refadv 的執行期訊息)。摘要多報 warning 跟少報一樣糟——它會讓人去 log
# 裡找不存在的東西,下次就不信這個數字了。第一段用 `.+` 而不是 `[^ ]+`,是為了
# 讓含空白的路徑(repo 被 clone 到「My Projects」那種目錄底下)也抓得到;要求
# 中間必須有 `:數字:` 就足以把執行期訊息擋在外面。
count_warnings() {
    grep -cE '^(.+:[0-9]+(:[0-9]+)?|<[^>]*>|([^ ]*/)?(clang|gcc|cc|ld|as|cc1|cc1plus|make)[^: ]*): warning:' "$1"
}

# Makefile 沒有 @ 開頭,編譯指令會原樣印出來;數 `-c src/` 就知道有幾個 TU
# 真的重編了(見 WHY 1)。
count_tus() { grep -c -- ' -c src/' "$1"; }

# 單元測試的 FAIL 行是「FAIL file:line ...」,interop 的是「FAIL: label」,
# 兩者都以 FAIL 開頭。
count_fails() { grep -c '^FAIL' "$1"; }

TOTAL_BINS=$(find tests -name '*.c' | wc -l | tr -d ' ')

# ---------------------------------------------------------------- gate 1: make

if [ "$WANT_REBUILD" = 1 ]; then
    echo "→ make clean(--rebuild)"
    make clean > "$LOGDIR/clean.log" 2>&1
fi

echo "→ make"
t0=$SECONDS
make > "$LOGDIR/build.log" 2>&1
rc=$?
dt=$((SECONDS - t0))
warns=$(count_warnings "$LOGDIR/build.log")
tus=$(count_tus "$LOGDIR/build.log")

if [ "$rc" -ne 0 ]; then
    row "FAIL" "make" "建置失敗(退出碼 $rc)" "$dt" "build.log"
    echo ""
    tail -20 "$LOGDIR/build.log"
    echo ""
    echo "建置失敗,後面的閘門沒有意義,停在這裡。log: $LOGDIR/build.log"
    exit 1
fi

if [ "$tus" -eq 0 ]; then
    # 見 WHY 1:什麼都沒重編時,warning 數是「沒量到」而不是「量到 0」。
    row "ok" "make" "0 個 TU 重編 → warning 數無意義(要量請用 --rebuild)" "$dt" "build.log"
elif [ "$warns" -gt 0 ]; then
    # 沒有 -Werror,所以這裡是 ok 而不是 FAIL——但要看得見。
    row "warn" "make" "$warns 個 warning($tus 個 TU 重編)" "$dt" "build.log"
else
    row "ok" "make" "0 個 warning($tus 個 TU 重編)" "$dt" "build.log"
fi

# ----------------------------------------------------------- gate 2: make test

echo "→ make test"
t0=$SECONDS
make test > "$LOGDIR/test.log" 2>&1
rc=$?
dt=$((SECONDS - t0))
ran=$(grep -c '^== build/tests/' "$LOGDIR/test.log")
fails=$(count_fails "$LOGDIR/test.log")
# 測試檔的 warning 也要數。tests/*.c 是一步編譯+連結(Makefile:65-67,沒有
# `-c`),不會出現在 gate 1 的 log 裡——不在這裡數的話,tests/ 底下的警告在整
# 份摘要中會完全消失,連 warn 都不會標。
warns=$(count_warnings "$LOGDIR/test.log")
# 這道也要分「沒量到」與「量到 0」,理由與 WHY 1 完全相同:連跑兩次而中間沒
# 改任何原始碼時,make 一個測試二進位都不會重編,warns 必然是 0 而那個 0 什麼
# 都不代表。測試二進位是一步編譯+連結,所以數 `-o build/tests/` 就是重編數。
built=$(grep -c -- '-o build/tests/' "$LOGDIR/test.log")

if [ "$rc" -eq 0 ] && [ "$fails" -eq 0 ] && [ "$ran" -eq "$TOTAL_BINS" ]; then
    if [ "$warns" -gt 0 ]; then
        row "warn" "make test" "$ran/$TOTAL_BINS 個二進位全過,但有 $warns 個 warning" "$dt" "test.log"
    elif [ "$built" -eq 0 ]; then
        row "ok" "make test" "$ran/$TOTAL_BINS 個二進位全過(0 個重編 → warning 數無意義)" "$dt" "test.log"
    else
        row "ok" "make test" "$ran/$TOTAL_BINS 個二進位全過,0 個 warning($built 個重編)" "$dt" "test.log"
    fi
elif [ "$fails" -gt 0 ]; then
    # 見 WHY 2:$ran < $TOTAL_BINS 表示中途中止,不是「其他都過了」。
    row "FAIL" "make test" "$ran/$TOTAL_BINS 個跑到,$fails 個 FAIL 行(第一個失敗即中止)" "$dt" "test.log"
else
    # 見 WHY 3:沒有 FAIL 行但退出碼非 0,照樣是失敗。
    row "FAIL" "make test" "$ran/$TOTAL_BINS 個跑到,0 個 FAIL 行但退出碼 $rc(崩潰或建置失敗)" "$dt" "test.log"
fi

# --------------------------------------------------------- gate 3: interop.sh

echo "→ bash tests/interop.sh"
t0=$SECONDS
bash tests/interop.sh > "$LOGDIR/interop.log" 2>&1
rc=$?
dt=$((SECONDS - t0))
summary=$(grep '^interop:' "$LOGDIR/interop.log")
fails=$(count_fails "$LOGDIR/interop.log")

if [ -z "$summary" ]; then
    # 見 WHY 4:摘要行不存在就是腳本中途死了,不能靜靜跳過。
    row "FAIL" "interop" "沒有印出 interop: 摘要行(中途死掉,退出碼 $rc)" "$dt" "interop.log"
else
    # 整行原樣帶出來,包含 skipped——skip 是「這項沒跑」,不是綠燈。
    line=${summary#interop: }
    skipped=$(printf '%s' "$line" | sed -n 's/.*, \([0-9]*\) skipped.*/\1/p')
    if [ "$rc" -eq 0 ] && [ "$fails" -eq 0 ]; then
        if [ -n "$skipped" ] && [ "$skipped" -gt 0 ] 2>/dev/null; then
            # 狀態欄要標 warn 而不是 ok。interop.sh 有 63 個 skip 呼叫,少一個
            # python3 或 git 就會讓整組 smart-HTTP 互通(clone/fetch/push 走真
            # 的線路,格式相容性最關鍵的端到端驗證)全部跳過,而 interop.sh 自
            # 己照樣退出 0。把那種情況印成綠色的 ok、只在句尾附一行小字,正是
            # gate 2 用「跑到幾個/共幾個」擋掉的同一種謊。不判 FAIL 是因為依賴
            # 缺席時 skip 是合法的,但它絕不是綠燈。
            row "warn" "interop" "$line ← skipped 不是綠燈,那 $skipped 項沒跑" "$dt" "interop.log"
        else
            row "ok" "interop" "$line" "$dt" "interop.log"
        fi
    else
        row "FAIL" "interop" "$line($fails 個 FAIL 行,退出碼 $rc)" "$dt" "interop.log"
    fi
fi

# ------------------------------------------------------ gate 4: make sanitize

if [ "$WANT_SANITIZE" = 1 ]; then
    echo "→ make sanitize(自帶 clean,會重建整份)"
    t0=$SECONDS
    make sanitize > "$LOGDIR/sanitize.log" 2>&1
    rc=$?
    dt=$((SECONDS - t0))
    ran=$(grep -c '^== build/tests/' "$LOGDIR/sanitize.log")
    fails=$(count_fails "$LOGDIR/sanitize.log")
    errs=$(grep -cE 'runtime error:|ERROR: (Address|Leak|UndefinedBehavior)Sanitizer' "$LOGDIR/sanitize.log")
    # sanitizer 旗標會開出一些普通建置看不到的警告,同樣不要讓它消失。
    warns=$(count_warnings "$LOGDIR/sanitize.log")

    if [ "$rc" -eq 0 ] && [ "$errs" -eq 0 ] && [ "$fails" -eq 0 ] && [ "$ran" -eq "$TOTAL_BINS" ]; then
        if [ "$warns" -gt 0 ]; then
            row "warn" "sanitize" "$ran/$TOTAL_BINS 個二進位,0 個 sanitizer 錯誤,但有 $warns 個 warning" "$dt" "sanitize.log"
        else
            row "ok" "sanitize" "$ran/$TOTAL_BINS 個二進位,0 個 sanitizer 錯誤" "$dt" "sanitize.log"
        fi
    else
        row "FAIL" "sanitize" "$ran/$TOTAL_BINS 個跑到,$errs 個 sanitizer 錯誤,$fails 個 FAIL 行(退出碼 $rc)" "$dt" "sanitize.log"
    fi

    # 見 WHY 6:不清的話,sanitizer 的 .o 會被普通 make 連進去。
    echo "→ make clean(sanitize 之後必須清,見腳本 WHY 6)"
    make clean >> "$LOGDIR/clean.log" 2>&1
    CLEANED=1
else
    ROWS+=("$(printf '%-4s %-9s %5s   %-12s  %s' "skip" "sanitize" "-" "-" "沒跑(要跑加 --sanitize)")")
    CLEANED=0
fi

# --------------------------------------------------------------------- 摘要

echo ""
echo "=== gates.sh 摘要 ==="
# ${ROWS[@]+"${ROWS[@]}"} 而不是 "${ROWS[@]}":macOS 的系統 bash 是 3.2,在
# set -u 底下展開空陣列會直接 unbound variable 中止。目前 gate 1 保證至少放進
# 一列,所以還踩不到,但那是很脆的不變量——之後只要多一條在 gate 1 之前就跳到
# 摘要區的路徑,整支腳本會在 macOS 上無聲炸掉。
for r in ${ROWS[@]+"${ROWS[@]}"}; do
    echo "$r"
done
echo ""
echo "原始 log: $LOGDIR"

if [ "$WANT_SANITIZE" = 1 ] && [ "$(uname -s)" = "Darwin" ]; then
    # 見 WHY 5。
    echo "注意:macOS 的 ASan 不做 leak 偵測,上面 sanitize 那行對記憶體洩漏是零證據。"
fi
if [ "$CLEANED" = 1 ]; then
    echo "注意:build/ 已清空,下一次 make 會是完整重建。"
fi

exit "$OVERALL"

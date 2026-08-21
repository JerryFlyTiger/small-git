#!/bin/bash
# 定向 mutation 驗證:把一處修法故意改壞,確認「應該變紅的檢查真的變紅」。
#
# 這個專案的紀律是「新增或修改測試後,必須先證明它會紅再相信它」
# (見 CLAUDE.md 的測試慣例)。這支腳本把那個步驟變成可執行的東西,並且
# 內建三條踩過坑才學到的規則——見底下 WHY 段落(第 3 條在 report() 上方,
# 因為它和逾時判定的實作綁在一起)。
#
# 用法:
#   bash tests/mutate.sh <名稱> <目標檔> <perl -0pe 運算式> [測試二進位名]
#   bash tests/mutate.sh <名稱> <目標檔> <perl -0pe 運算式> --interop
#
#   <目標檔> 是相對於專案根目錄的路徑,例如 src/storage/refs.c
#   省略第四個參數時跑整個 make test。
#
# perl 運算式的分隔符要挑過,C 程式碼很容易撞到(每一條都實測踩過):
#   s{...}{...}  樣式裡有不成對的 { 或 }(C 的區塊)就會壞掉
#   s!...!...!   樣式裡有 != 就會壞掉
#   s#...#...#   對 C 來說通常安全,優先用這個
# 另外:**要改的字面量若在檔案裡出現不只一次,一定要加 /g**。實測踩過:同一個
# 格式字串有兩份,沒加 /g 只改到第一份,結果看起來像「測試覆蓋不足」,其實是
# mutation 只改了一半——驗證工具說謊的方向永遠是「已驗過」。
#
# 例:
#   bash tests/mutate.sh atn src/storage/revparse.c \
#       's/entry->new_id/entry->old_id/' test_revparse
#   bash tests/mutate.sh nomkdir src/storage/refs.c \
#       's/sg_write_file_mkdirs/sg_write_file_no_such/' --interop
#
# 輸出:該 mutation 之下有哪些具名檢查變紅。若「什麼都沒紅」,那就是死角
# ——這支腳本最有價值的用途正是找出那些「改了也不會有人發現」的修法。
#
# WHY(三條規則都是踩過坑才加的,不要拿掉):
#
#   1. 每輪都從乾淨的複本重新完整建置。舊的 .o 檔不記錄自己是用哪份原始碼
#      編的,mtime 一舊 make 就跳過重編,於是 mutation 會無聲地跨輪累積,
#      讓後面每一輪的結論都不可信。
#
#   2. 退出碼非 0 就算「被抓到」,不論有沒有 FAIL 行。曾經有一個邊界
#      mutation 讓測試二進位直接 segfault(沒有任何 FAIL 行),舊版腳本因此
#      印「死角」——但 make test 其實抓得到。只 grep FAIL 會把崩潰誤報成
#      沒有鑑別力,而這種誤報的方向永遠是「已驗過」,最危險。

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")

if [ "$#" -lt 3 ]; then
    echo "usage: bash tests/mutate.sh <name> <file> <perl-expr> [<test-binary>|--interop]" >&2
    exit 2
fi

NAME="$1"
FILE="$2"
EXPR="$3"
TARGET="${4:-}"

if [ ! -f "$PROJECT_ROOT/$FILE" ]; then
    echo "error: $FILE 不存在(路徑要相對於專案根目錄)" >&2
    exit 2
fi

# 名稱只是給人看的標籤,可以有空白、斜線、中文。斜線會被 mkdtemp 當成路徑
# 分隔而整個失敗("No such file or directory"),所以這裡只取安全字元當目錄名。
# 失敗是大聲的(mktemp 印錯誤、腳本退出 1),但錯誤訊息看起來像環境問題而不像
# 「你的名字取壞了」,實測浪費了兩輪才看懂 —— 直接不讓它發生。
NAME_SLUG=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9._-' '_')
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sg_mutate_${NAME_SLUG}_XXXXXX") || exit 1
echo "mutation '$NAME' 的工作目錄: $WORK"

# 用工作樹的現況而不是 HEAD:要驗的往往正是還沒 commit 的改動。
# build/ 與 .git/ 不複製——前者要重建(見 WHY 1),後者沒必要且很大。
if command -v rsync > /dev/null 2>&1; then
    rsync -a --exclude build --exclude .git "$PROJECT_ROOT/" "$WORK/" || exit 1
else
    (cd "$PROJECT_ROOT" && tar --exclude=./build --exclude=./.git -cf - .) | (cd "$WORK" && tar -xf -) || exit 1
fi

BEFORE=$(cksum < "$WORK/$FILE")
perl -0pi -e "$EXPR" "$WORK/$FILE" || exit 1
AFTER=$(cksum < "$WORK/$FILE")

if [ "$BEFORE" = "$AFTER" ]; then
    echo "MUTATION-NOT-APPLIED: perl 運算式沒有匹配到任何東西"
    echo "  這是假陰性的來源:什麼都沒改當然不會紅。修好運算式再跑。"
    exit 3
fi

cd "$WORK" || exit 1
if ! make > "$WORK/build.log" 2>&1; then
    echo "BUILD-FAILED:"
    tail -15 "$WORK/build.log"
    echo "  壞掉的 mutation 給出的紅燈不算證據——要改到能編譯為止。"
    exit 4
fi

# WHY 3(第三條規則,和上面兩條一樣是踩過坑才加的):把「卡死」轉成乾淨的失敗。
#
# 上面第 2 條說「退出碼非 0 就算被抓到」。那條規則假設程式會結束——但 mutation
# 可以讓程式**永遠不結束**:把一個歸併迴圈的比較方向反過來,游標就不再前進。
# 2026-08-21 實測(Phase 25,`list_diff_sorted` 的 `cmp < 0` 改成 `cmp > 0`):
# 測試二進位跑了 30 分鐘還在跑,而「永遠不退出」既不是 0 也不是非 0,腳本
# 只會安靜地佔住終端機,不會給出任何結論。
#
# 逾時被視為「被抓到」(卡死在 CI 上一樣會被發現),但**單獨標示**出來,因為
# 它和崩潰一樣,證明的是「改壞了會出事」而不是「那條具名斷言有鑑別力」——
# 看到這行就要換一條不會卡死的 mutation 重驗。
SG_MUTATE_TIMEOUT=${SG_MUTATE_TIMEOUT:-300}
TIMED_OUT=0

# 逾時與否用 marker 檔判定,不用 `kill -0` 探 watchdog 是否還活著:watchdog
# 砍完目標後才自己結束,兩者之間有一段race,探活會時而答錯。marker 只在
# watchdog 真的開火時才存在,判定是確定的。
run_with_timeout() {
    _marker="$WORK/.timed_out"
    TIMED_OUT=0
    rm -f "$_marker"
    "$@" &
    _cmd_pid=$!
    (
        sleep "$SG_MUTATE_TIMEOUT"
        if kill -0 "$_cmd_pid" 2>/dev/null; then
            : > "$_marker"
            kill -9 "$_cmd_pid" 2>/dev/null
        fi
    ) &
    _watchdog_pid=$!
    wait "$_cmd_pid"
    _rc=$?
    kill "$_watchdog_pid" 2>/dev/null
    wait "$_watchdog_pid" 2>/dev/null
    [ -e "$_marker" ] && TIMED_OUT=1
    rm -f "$_marker"
    return $_rc
}

report() {
    rc="$1"
    log="$2"
    if [ "$TIMED_OUT" -ne 0 ]; then
        echo "  (逾時 ${SG_MUTATE_TIMEOUT}s 被砍 —— 算被抓到,但這是「卡死」不是"
        echo "   「斷言看見了」。換一條不會卡死的 mutation 才驗得到鑑別力。)"
        tail -3 "$log" | sed 's/^/    /'
    elif grep -qE '^FAIL' "$log"; then
        grep -E '^FAIL' "$log"
    elif [ "$rc" -ne 0 ]; then
        echo "  (沒有 FAIL 行,但退出碼 $rc —— 仍被抓到,可能是崩潰)"
        tail -3 "$log" | sed 's/^/    /'
    else
        echo "  (退出碼 0 且沒有 FAIL 行 —— 真死角:這處修法改了也沒人發現)"
    fi
}

case "$TARGET" in
    --interop)
        run_with_timeout bash tests/interop.sh > "$WORK/run.log" 2>&1
        rc=$?
        echo "=== $NAME: interop.sh 退出碼 $rc ==="
        grep -E '^interop:' "$WORK/run.log"
        report "$rc" "$WORK/run.log"
        ;;
    "")
        run_with_timeout make test > "$WORK/run.log" 2>&1
        rc=$?
        echo "=== $NAME: make test 退出碼 $rc ==="
        report "$rc" "$WORK/run.log"
        ;;
    *)
        make "build/tests/$TARGET" > /dev/null 2>&1
        if [ ! -x "$WORK/build/tests/$TARGET" ]; then
            echo "error: 建不出 build/tests/$TARGET(名字打錯?)" >&2
            exit 2
        fi
        run_with_timeout "$WORK/build/tests/$TARGET" > "$WORK/run.log" 2>&1
        rc=$?
        echo "=== $NAME: $TARGET 退出碼 $rc ==="
        report "$rc" "$WORK/run.log"
        ;;
esac

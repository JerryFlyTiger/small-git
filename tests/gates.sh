#!/bin/bash
# The four gates that make up the completion standard, run as one script,
# printing a compact summary + the paths of the raw logs.
#
# CLAUDE.md's "Build & verify" section requires make + make test + interop.sh
# all green as the completion standard, plus make sanitize when memory
# management or pack/chunk code was touched. This script pins that pipeline
# down -- not mainly to save typing, but so that **every run reads the results
# with the same extraction rules**: ad hoc grep on the wrong number leads to
# misreading a gate, and that is this project's worst failure mode.
#
# Usage:
#   bash tests/gates.sh              # make + make test + interop.sh
#   bash tests/gates.sh --sanitize   # also run make sanitize (cleans twice, slow)
#   bash tests/gates.sh --rebuild    # make clean first, so the warning count means something
#
# Exit code: 0 only if every gate that ran is green, otherwise 1 (bad
# arguments give 2).
#
# Not included here: python3 tests/fuzz_ignore.py is a **conditional** gate
# (only run when ignore handling or directory traversal is touched), not
# something to run every time, so it is deliberately left out; watching CI
# is a plain `gh run watch`.
#
# WHY (each one is here because of a bug that was hit; do not remove any):
#
#   1. The warning count must be reported alongside how many TUs actually got
#      recompiled. CFLAGS has no -Werror, so green does not mean zero
#      warnings -- but when build/ is already up to date, make compiles
#      nothing and the warning count is naturally 0. That 0 does not mean
#      "no warnings", it means "nobody looked". Failing to flag the recompile
#      count turns "not measured" into "measured as 0", and that kind of
#      misreading always points the wrong way: toward "already verified".
#
#   2. make test stops at the first failing binary (the Makefile's
#      `$$t || exit 1`), so the FAIL line count is **truncated**, and the
#      binaries after the failure never ran at all. The summary reports
#      "ran N of TOTAL"; fewer than the total means it stopped partway
#      through, not "everything else passed".
#
#   3. A non-zero exit code with zero FAIL lines is still a failure (segfault,
#      timeout, build failure). Counting only FAIL lines would read a crash as
#      green. This rule shares its root cause with rule 2 in tests/mutate.sh.
#
#   4. interop.sh's `interop: N/M passed, K skipped` line is only printed once
#      the run reaches the end. When that line cannot be found, say so
#      explicitly -- never silently treat it as fine. K > 0 is always flagged
#      warn: interop.sh has 63 skip() calls, and missing python3 or git alone
#      is enough to skip the entire smart-HTTP interop group, while the
#      script itself still exits 0. And K understates it further -- measured:
#      turning off HTTP_AVAILABLE gives K = 32, but M drops from 998 to 886
#      (skip() only increments SKIP, not TOTAL, so the 80 checks it does not
#      explicitly name vanish without leaving any number behind at all). That
#      is why M itself must be checked too, not just N == M.
#
#   5. macOS's ASan does **not** do leak detection (detect_leaks is not
#      supported on this platform), so a green `make sanitize` on this
#      machine is zero evidence against memory leaks -- that has to come from
#      CI's ubuntu ASan job. Phase 16 is exactly how a strdup leak slipped
#      past this machine once.
#
#   6. make clean is mandatory after sanitize. Object files do not record
#      which flag set they were built with, so skipping the clean and running
#      a plain make afterward links the sanitizer's .o files into the normal
#      build (Makefile:76-80).

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
            # Print the whole header comment block (up to the first
            # non-comment, non-blank line). Three details: do not switch this
            # to a fixed line range (it used to be hardcoded as 2,30p, which
            # cut off WHY 3-6 right when those are exactly the ones that teach
            # people how to read a green result correctly); do not use "$0"
            # (we already cd'd above, so `cd tests && bash gates.sh --help`
            # would silently print nothing because it cannot find itself);
            # blank lines must be skipped rather than treated as a stop
            # condition -- otherwise someone adding a blank formatting line
            # right after the shebang would make --help print zero lines
            # forever, while still exiting 0.
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
echo "log directory: $LOGDIR"
echo ""

# Summary rows accumulate here and get printed all at once at the end -- the
# gates take a while to run, and mid-run output would otherwise scroll away.
ROWS=()
OVERALL=0

# row <status> <gate name> <description> <seconds> <log filename>
#
# Fixed-width fields only hold ASCII: printf's %-Ns counts bytes, and CJK
# characters are three bytes each, which would throw off alignment for a
# CJK description. That is why the description always goes in the last
# column and does not participate in alignment.
row() {
    ROWS+=("$(printf '%-4s %-9s %5ss  %-12s  %s' "$1" "$2" "$4" "$5" "$3")")
    if [ "$1" = "FAIL" ]; then
        OVERALL=1
    fi
}

# Only recognizes the build tools' diagnostic formats, three of them:
#   1. "src/x.c:12:5: warning: ..." -- file:line[:col].
#   2. "<command-line>: warning: \"FOO\" redefined" -- GCC's macro
#      redefinition warning uses this format, with no filename. This project
#      specifically needs it: Makefile:5-18 is entirely about the
#      cross-platform clash between the _POSIX_C_SOURCE / _DEFAULT_SOURCE /
#      _DARWIN_C_SOURCE feature-test macros, and hitting that in practice
#      looks exactly like this.
#   3. "clang: warning:" / "/usr/bin/ld: warning:" / "make: warning: Clock
#      skew" -- emitted by the tool itself, possibly with a full path in
#      front, so the tool name cannot be anchored with ^. `make` is included
#      here because a clock skew warning undermines make's own idea of "what
#      is newer than what", which in turn makes the count_tus safety net
#      untrustworthy -- that has to be visible.
# clang's trailing "3 warnings generated." line has no colon, so it will not
# be double-counted.
#
# Do not loosen this to just ': warning:' -- sg itself prints
# "sg: warning: ignoring invalid remote ref name ..." when it runs, and the
# make test / make sanitize logs contain test-binary output too. Measured: a
# planted unused variable in tests/ makes the loose version count 7 warnings
# (1 real one + 6 runtime messages from test_refadv). Over-reporting warnings
# is just as bad as under-reporting -- it sends people to the log looking for
# something that is not there, and they stop trusting the number after that.
# The first alternative uses `.+` rather than `[^ ]+` so that paths containing
# spaces (a repo cloned under a directory like "My Projects") are still
# matched; requiring a `:<number>:` in the middle is enough to keep runtime
# messages out.
count_warnings() {
    grep -cE '^(.+:[0-9]+(:[0-9]+)?|<[^>]*>|([^ ]*/)?(clang|gcc|cc|ld|as|cc1|cc1plus|make)[^: ]*): warning:' "$1"
}

# The Makefile has no @ prefix, so compile commands are echoed verbatim;
# counting `-c src/` occurrences tells us how many TUs actually got
# recompiled (see WHY 1).
count_tus() { grep -c -- ' -c src/' "$1"; }

# Unit test FAIL lines look like "FAIL file:line ...", interop's like
# "FAIL: label" -- both start with FAIL.
count_fails() { grep -c '^FAIL' "$1"; }

TOTAL_BINS=$(find tests -name '*.c' | wc -l | tr -d ' ')

# ---------------------------------------------------------------- gate 1: make

if [ "$WANT_REBUILD" = 1 ]; then
    echo "-> make clean (--rebuild)"
    make clean > "$LOGDIR/clean.log" 2>&1
fi

echo "-> make"
t0=$SECONDS
make > "$LOGDIR/build.log" 2>&1
rc=$?
dt=$((SECONDS - t0))
warns=$(count_warnings "$LOGDIR/build.log")
tus=$(count_tus "$LOGDIR/build.log")

if [ "$rc" -ne 0 ]; then
    row "FAIL" "make" "build failed (exit code $rc)" "$dt" "build.log"
    echo ""
    tail -20 "$LOGDIR/build.log"
    echo ""
    echo "build failed, later gates would be meaningless, stopping here. log: $LOGDIR/build.log"
    exit 1
fi

if [ "$tus" -eq 0 ]; then
    # See WHY 1: when nothing got recompiled, the warning count is "not
    # measured", not "measured as 0".
    row "ok" "make" "0 TUs recompiled -> warning count is meaningless (use --rebuild to measure)" "$dt" "build.log"
elif [ "$warns" -gt 0 ]; then
    # No -Werror, so this is ok rather than FAIL -- but it must be visible.
    row "warn" "make" "$warns warning(s) ($tus TU(s) recompiled)" "$dt" "build.log"
else
    row "ok" "make" "0 warnings ($tus TU(s) recompiled)" "$dt" "build.log"
fi

# ----------------------------------------------------------- gate 2: make test

echo "-> make test"
t0=$SECONDS
make test > "$LOGDIR/test.log" 2>&1
rc=$?
dt=$((SECONDS - t0))
ran=$(grep -c '^== build/tests/' "$LOGDIR/test.log")
fails=$(count_fails "$LOGDIR/test.log")
# Test-file warnings must be counted too. tests/*.c is compiled and linked in
# one step (Makefile:65-67, no `-c`), so it never shows up in gate 1's log --
# without counting it here, warnings under tests/ would vanish from the
# summary entirely, without even being flagged warn.
warns=$(count_warnings "$LOGDIR/test.log")
# This gate also needs to distinguish "not measured" from "measured as 0",
# for exactly the same reason as WHY 1: running twice in a row with no
# source changes in between recompiles zero test binaries, so warns is
# necessarily 0 and that 0 means nothing. Test binaries are compiled and
# linked in one step, so counting `-o build/tests/` gives the recompile count.
built=$(grep -c -- '-o build/tests/' "$LOGDIR/test.log")

if [ "$rc" -eq 0 ] && [ "$fails" -eq 0 ] && [ "$ran" -eq "$TOTAL_BINS" ]; then
    if [ "$warns" -gt 0 ]; then
        row "warn" "make test" "$ran/$TOTAL_BINS binaries all passed, but $warns warning(s)" "$dt" "test.log"
    elif [ "$built" -eq 0 ]; then
        row "ok" "make test" "$ran/$TOTAL_BINS binaries all passed (0 recompiled -> warning count is meaningless)" "$dt" "test.log"
    else
        row "ok" "make test" "$ran/$TOTAL_BINS binaries all passed, 0 warnings ($built recompiled)" "$dt" "test.log"
    fi
elif [ "$fails" -gt 0 ]; then
    # See WHY 2: $ran < $TOTAL_BINS means it stopped partway, not "everything
    # else passed".
    row "FAIL" "make test" "$ran/$TOTAL_BINS ran, $fails FAIL line(s) (stopped at the first failure)" "$dt" "test.log"
else
    # See WHY 3: no FAIL lines but a non-zero exit code is still a failure.
    row "FAIL" "make test" "$ran/$TOTAL_BINS ran, 0 FAIL lines but exit code $rc (crash or build failure)" "$dt" "test.log"
fi

# --------------------------------------------------------- gate 3: interop.sh

echo "-> bash tests/interop.sh"
t0=$SECONDS
bash tests/interop.sh > "$LOGDIR/interop.log" 2>&1
rc=$?
dt=$((SECONDS - t0))
summary=$(grep '^interop:' "$LOGDIR/interop.log")
fails=$(count_fails "$LOGDIR/interop.log")

if [ -z "$summary" ]; then
    # See WHY 4: a missing summary line means the script died partway
    # through, and that must not be silently skipped over.
    row "FAIL" "interop" "no interop: summary line printed (died partway, exit code $rc)" "$dt" "interop.log"
else
    # Carry the whole line through as-is, including skipped -- skipped means
    # "this check did not run", not green.
    line=${summary#interop: }
    skipped=$(printf '%s' "$line" | sed -n 's/.*, \([0-9]*\) skipped.*/\1/p')
    if [ "$rc" -eq 0 ] && [ "$fails" -eq 0 ]; then
        if [ -n "$skipped" ] && [ "$skipped" -gt 0 ] 2>/dev/null; then
            # The status column must say warn, not ok. interop.sh has 63
            # skip() calls, and missing python3 or git alone is enough to
            # skip the entire smart-HTTP interop group (clone/fetch/push over
            # the real wire, the most critical end-to-end check of format
            # compatibility), while interop.sh itself still exits 0. Printing
            # that as green ok with only a small note at the end of the line
            # is exactly the same lie that gate 2 blocks with "ran N of
            # TOTAL". It is not marked FAIL because skipping is legitimate
            # when a dependency is missing -- but it is never green either.
            row "warn" "interop" "$line <- skipped is not green, $skipped item(s) did not run" "$dt" "interop.log"
        else
            row "ok" "interop" "$line" "$dt" "interop.log"
        fi
    else
        row "FAIL" "interop" "$line ($fails FAIL line(s), exit code $rc)" "$dt" "interop.log"
    fi
fi

# ------------------------------------------------------ gate 4: make sanitize

if [ "$WANT_SANITIZE" = 1 ]; then
    echo "-> make sanitize (cleans and rebuilds everything itself)"
    t0=$SECONDS
    make sanitize > "$LOGDIR/sanitize.log" 2>&1
    rc=$?
    dt=$((SECONDS - t0))
    ran=$(grep -c '^== build/tests/' "$LOGDIR/sanitize.log")
    fails=$(count_fails "$LOGDIR/sanitize.log")
    errs=$(grep -cE 'runtime error:|ERROR: (Address|Leak|UndefinedBehavior)Sanitizer' "$LOGDIR/sanitize.log")
    # The sanitizer flags surface some warnings a normal build never shows;
    # those must not disappear either.
    warns=$(count_warnings "$LOGDIR/sanitize.log")

    if [ "$rc" -eq 0 ] && [ "$errs" -eq 0 ] && [ "$fails" -eq 0 ] && [ "$ran" -eq "$TOTAL_BINS" ]; then
        if [ "$warns" -gt 0 ]; then
            row "warn" "sanitize" "$ran/$TOTAL_BINS binaries, 0 sanitizer errors, but $warns warning(s)" "$dt" "sanitize.log"
        else
            row "ok" "sanitize" "$ran/$TOTAL_BINS binaries, 0 sanitizer errors" "$dt" "sanitize.log"
        fi
    else
        row "FAIL" "sanitize" "$ran/$TOTAL_BINS ran, $errs sanitizer error(s), $fails FAIL line(s) (exit code $rc)" "$dt" "sanitize.log"
    fi

    # See WHY 6: without a clean, the sanitizer's .o files would get linked
    # into a subsequent plain make.
    echo "-> make clean (mandatory after sanitize, see script WHY 6)"
    make clean >> "$LOGDIR/clean.log" 2>&1
    CLEANED=1
else
    ROWS+=("$(printf '%-4s %-9s %5s   %-12s  %s' "skip" "sanitize" "-" "-" "not run (add --sanitize to run it)")")
    CLEANED=0
fi

# --------------------------------------------------------------------- summary

echo ""
echo "=== gates.sh summary ==="
# ${ROWS[@]+"${ROWS[@]}"} instead of "${ROWS[@]}": macOS's system bash is 3.2,
# where expanding an empty array under set -u aborts with an unbound variable
# error. Right now gate 1 guarantees at least one row gets added, so this is
# not hit today, but it is a fragile invariant -- if a path that jumps to the
# summary before gate 1 is ever added, this script would die silently on
# macOS.
for r in ${ROWS[@]+"${ROWS[@]}"}; do
    echo "$r"
done
echo ""
echo "raw logs: $LOGDIR"

if [ "$WANT_SANITIZE" = 1 ] && [ "$(uname -s)" = "Darwin" ]; then
    # See WHY 5.
    echo "note: macOS's ASan does not do leak detection, so the sanitize row above is zero evidence against memory leaks."
fi
if [ "$CLEANED" = 1 ]; then
    echo "note: build/ has been cleaned, the next make will be a full rebuild."
fi

exit "$OVERALL"

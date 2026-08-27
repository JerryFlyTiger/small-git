#!/bin/bash
# Directed mutation verification: deliberately break one fix and confirm the
# check that is supposed to catch it actually turns red.
#
# This project's discipline is "after adding or changing a test, you must
# prove it turns red before trusting it" (see the test conventions in
# CLAUDE.md). This script turns that step into something runnable, and bakes
# in three rules learned the hard way -- see the WHY section below (rule 3 is
# written just above report(), because it is tied to how the timeout
# detection is implemented).
#
# Usage:
#   bash tests/mutate.sh <name> <target-file> <perl -0pe expression> [test-binary]
#   bash tests/mutate.sh <name> <target-file> <perl -0pe expression> --interop
#
#   <target-file> is a path relative to the project root, e.g.
#   src/storage/refs.c
#   Omitting the fourth argument runs the whole make test.
#
# The perl expression's delimiter needs to be chosen carefully, since C code
# collides with the common ones easily (each of these was hit in practice):
#   s{...}{...}  breaks if the pattern has an unpaired { or } (a C block)
#   s!...!...!   breaks if the pattern has !=
#   s#...#...#   usually safe for C, prefer this one
# Also: **if the literal being changed appears more than once in the file,
# always add /g**. Measured in practice: the same format string existed in
# two copies, and forgetting /g only changed the first one -- the result
# looked like "insufficient test coverage" when it was actually a mutation
# that only touched half its targets. The verification tooling's failure
# mode always lies in the direction of "already verified".
#
# Example:
#   bash tests/mutate.sh atn src/storage/revparse.c \
#       's/entry->new_id/entry->old_id/' test_revparse
#   bash tests/mutate.sh nomkdir src/storage/refs.c \
#       's/sg_write_file_mkdirs/sg_write_file_no_such/' --interop
#
# Output: which named checks turn red under that mutation. If nothing turns
# red at all, that is a blind spot -- this script's most valuable use is
# finding exactly those fixes that can be broken without anyone noticing.
#
# WHY (all three rules were added because of a bug that was hit; do not
# remove any of them):
#
#   1. Every run does a full rebuild from a fresh copy. A stale .o file does
#      not record which source it was built from, and a stale mtime makes
#      make skip recompiling it, so a mutation would silently accumulate
#      across runs, making every later run's conclusion untrustworthy.
#
#   2. A non-zero exit code counts as "caught", regardless of whether there
#      are any FAIL lines. A boundary mutation once made a test binary
#      segfault outright (with no FAIL lines at all), and the old version of
#      this script reported it as a blind spot -- but make test actually did
#      catch it. Grepping only for FAIL would misreport a crash as having no
#      discriminating power, and that kind of false report always lies in
#      the direction of "already verified", which is the most dangerous kind.

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
    echo "error: $FILE does not exist (the path must be relative to the project root)" >&2
    exit 2
fi

# The name is just a human-facing label and may contain spaces, slashes,
# anything. A slash would be read by mkdtemp as a path separator and fail
# outright ("No such file or directory"), so only safe characters are kept
# for the directory name. The failure is loud (mktemp prints an error, the
# script exits 1), but the error message looks like an environment problem
# rather than "your name choice broke this" -- measured wasting two runs
# before that was understood. Just do not let it happen.
NAME_SLUG=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9._-' '_')
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sg_mutate_${NAME_SLUG}_XXXXXX") || exit 1
echo "working directory for mutation '$NAME': $WORK"

# Use the working tree's current state rather than HEAD: what needs
# verifying is usually a change that has not been committed yet. build/ and
# .git/ are not copied -- the former has to be rebuilt anyway (see WHY 1),
# and the latter is unnecessary and large.
if command -v rsync > /dev/null 2>&1; then
    rsync -a --exclude build --exclude .git "$PROJECT_ROOT/" "$WORK/" || exit 1
else
    (cd "$PROJECT_ROOT" && tar --exclude=./build --exclude=./.git -cf - .) | (cd "$WORK" && tar -xf -) || exit 1
fi

BEFORE=$(cksum < "$WORK/$FILE")
perl -0pi -e "$EXPR" "$WORK/$FILE" || exit 1
AFTER=$(cksum < "$WORK/$FILE")

if [ "$BEFORE" = "$AFTER" ]; then
    echo "MUTATION-NOT-APPLIED: the perl expression did not match anything"
    echo "  This is a source of false negatives: nothing changed, so of course nothing turns red. Fix the expression and try again."
    exit 3
fi

cd "$WORK" || exit 1
if ! make > "$WORK/build.log" 2>&1; then
    echo "BUILD-FAILED:"
    tail -15 "$WORK/build.log"
    echo "  A red result from a mutation that does not even compile is not evidence -- fix it until it builds."
    exit 4
fi

# WHY 3 (the third rule, added because of a bug hit in practice just like the
# other two): turn "hung forever" into a clean failure.
#
# Rule 2 above says "a non-zero exit code counts as caught". That rule
# assumes the program eventually terminates -- but a mutation can make it
# **never terminate**: flip the comparison direction in a merge loop and the
# cursor stops advancing. Measured on 2026-08-21 (Phase 25, `list_diff_sorted`
# `cmp < 0` flipped to `cmp > 0`): the test binary was still running 30
# minutes later, and "never exits" is neither 0 nor non-zero -- the script
# would just sit there quietly hogging the terminal, with no conclusion at
# all.
#
# A timeout is treated as "caught" (a hang would be found in CI too, just as
# well), but it is **flagged separately**, because like a crash it only
# proves "breaking this causes trouble", not "that named assertion has
# discriminating power" -- seeing this line means picking a different
# mutation that does not hang and re-verifying with that instead.
SG_MUTATE_TIMEOUT=${SG_MUTATE_TIMEOUT:-300}
TIMED_OUT=0

# Whether a timeout happened is decided with a marker file, not by probing
# whether the watchdog is still alive with `kill -0`: the watchdog only exits
# itself after it has killed the target, and there is a race window between
# the two, so a liveness probe would sometimes get the wrong answer. The
# marker only exists when the watchdog actually fired, so the check is
# deterministic.
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
        echo "  (timed out after ${SG_MUTATE_TIMEOUT}s and was killed -- counts as caught, but this is"
        echo "   a hang, not \"the assertion saw it\". Pick a mutation that does not hang to verify discriminating power.)"
        tail -3 "$log" | sed 's/^/    /'
    elif grep -qE '^FAIL' "$log"; then
        grep -E '^FAIL' "$log"
    elif [ "$rc" -ne 0 ]; then
        echo "  (no FAIL lines, but exit code $rc -- still caught, possibly a crash)"
        tail -3 "$log" | sed 's/^/    /'
    else
        echo "  (exit code 0 and no FAIL lines -- a real blind spot: this fix can be broken without anyone noticing)"
    fi
}

case "$TARGET" in
    --interop)
        run_with_timeout bash tests/interop.sh > "$WORK/run.log" 2>&1
        rc=$?
        echo "=== $NAME: interop.sh exit code $rc ==="
        grep -E '^interop:' "$WORK/run.log"
        report "$rc" "$WORK/run.log"
        ;;
    "")
        run_with_timeout make test > "$WORK/run.log" 2>&1
        rc=$?
        echo "=== $NAME: make test exit code $rc ==="
        report "$rc" "$WORK/run.log"
        ;;
    *)
        make "build/tests/$TARGET" > /dev/null 2>&1
        if [ ! -x "$WORK/build/tests/$TARGET" ]; then
            echo "error: could not build build/tests/$TARGET (name misspelled?)" >&2
            exit 2
        fi
        run_with_timeout "$WORK/build/tests/$TARGET" > "$WORK/run.log" 2>&1
        rc=$?
        echo "=== $NAME: $TARGET exit code $rc ==="
        report "$rc" "$WORK/run.log"
        ;;
esac

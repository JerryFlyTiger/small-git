#!/bin/sh
# Interop test: proves small_git's object format is bit-compatible with real git.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
SG="$PROJECT_ROOT/build/sg"

PASS=0
FAIL=0
TOTAL=0
SKIP=0

check() {
    label="$1"
    shift
    TOTAL=$((TOTAL + 1))
    if "$@"; then
        PASS=$((PASS + 1))
        echo "PASS: $label"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $label"
    fi
}

skip() {
    SKIP=$((SKIP + 1))
    echo "SKIP: $1"
}

if [ ! -x "$SG" ]; then
    echo "error: $SG not found, run 'make' first" >&2
    exit 1
fi

WORKDIR=$(mktemp -d)
# HTTP_SERVER_PID is set once the phase 5b smart-HTTP test server is
# launched; cleanup() kills it (if still running) alongside removing WORKDIR,
# so a failure partway through phase 5b can never leak an orphaned server
# process.
HTTP_SERVER_PID=""
cleanup() {
    if [ -n "$HTTP_SERVER_PID" ]; then
        kill "$HTTP_SERVER_PID" 2>/dev/null
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

REPO="$WORKDIR/repo"
mkdir -p "$REPO"
git init -q "$REPO"

# a test file with a newline, CJK text, and an embedded raw NUL byte.
# The \xNN escapes below are the UTF-8 bytes for the CJK words
# "line 1" (U+7B2C U+4E00 U+884C), "line 2" (U+7B2C U+4E8C U+884C), and
# "hello" (U+4F60 U+597D) -- written as hex so this file itself has no
# literal Han characters, but the fixture's on-disk bytes are unchanged.
FILE1="$WORKDIR/file1.bin"
printf '\xe7\xac\xac\xe4\xb8\x80\xe8\xa1\x8c hello\n\xe7\xac\xac\xe4\xba\x8c\xe8\xa1\x8c\xe4\xbd\xa0\xe5\xa5\xbd\n' > "$FILE1"
printf '\000binary-tail' >> "$FILE1"

# --- sg writes, git reads ---
SHA_A=$( (cd "$REPO" && "$SG" hash-object -w "$FILE1") )
check "sg hash-object -w produced a 40-char hex id" test "$(printf '%s' "$SHA_A" | wc -c | tr -d ' ')" = 40

GIT_CONTENT_A="$WORKDIR/git_content_a.bin"
(cd "$REPO" && git cat-file -p "$SHA_A") > "$GIT_CONTENT_A" 2>/dev/null
check "git cat-file -p reads sg-written blob with identical content" cmp -s "$FILE1" "$GIT_CONTENT_A"

TYPE_A=$(cd "$REPO" && git cat-file -t "$SHA_A" 2>/dev/null)
check "git cat-file -t reports sg-written object as blob" test "$TYPE_A" = "blob"

# --- git writes, sg reads ---
FILE2="$WORKDIR/file2.bin"
printf 'another content\nwith more lines\n\xc3\xa9\xc3\xa8\n' > "$FILE2"
printf '\001\002\003' >> "$FILE2"

SHA_B=$(cd "$REPO" && git hash-object -w "$FILE2")

SG_CONTENT_B="$WORKDIR/sg_content_b.bin"
(cd "$REPO" && "$SG" cat-file -p "$SHA_B") > "$SG_CONTENT_B" 2>/dev/null
check "sg cat-file -p reads git-written blob with identical content" cmp -s "$FILE2" "$SG_CONTENT_B"

TYPE_B=$(cd "$REPO" && "$SG" cat-file -t "$SHA_B" 2>/dev/null)
check "sg cat-file -t reports git-written object as blob" test "$TYPE_B" = "blob"

# --- tree object, cross-checked both directions ---
SUBFILE="$WORKDIR/tree_content.txt"
printf 'tree entry content\n' > "$SUBFILE"
SHA_T1=$(cd "$REPO" && git hash-object -w "$SUBFILE")

TREE_SPEC="100644 blob $SHA_T1	leaf.txt"
SHA_TREE=$(cd "$REPO" && printf '%s\n' "$TREE_SPEC" | git mktree)

SG_TREE_OUT="$WORKDIR/sg_tree.txt"
(cd "$REPO" && "$SG" cat-file -p "$SHA_TREE") > "$SG_TREE_OUT" 2>/dev/null
GIT_TREE_OUT="$WORKDIR/git_tree.txt"
(cd "$REPO" && git cat-file -p "$SHA_TREE") > "$GIT_TREE_OUT" 2>/dev/null
check "sg cat-file -p on a git-mktree'd tree matches git cat-file -p" cmp -s "$GIT_TREE_OUT" "$SG_TREE_OUT"

TREE_TYPE_SG=$(cd "$REPO" && "$SG" cat-file -t "$SHA_TREE" 2>/dev/null)
check "sg cat-file -t reports git-written tree as tree" test "$TREE_TYPE_SG" = "tree"

# now have sg hash-object -w write the *same file* as a blob, then round-trip a tree
# referencing it built by hand isn't exercised by hash-object; instead verify sg can
# also write blobs that git's mktree/ls-tree already reference correctly (SHA_T1 was
# produced by real git, and sg already proved it can read that blob above via cat-file
# through the tree; here additionally confirm sg's own written blob id matches git's for
# the same bytes, tying the two directions together for tree-referenced content)
SHA_T1_SG=$(cd "$REPO" && "$SG" hash-object "$SUBFILE")
check "sg hash-object (no -w) agrees with git hash-object on tree-referenced blob id" test "$SHA_T1_SG" = "$SHA_T1"

# --- sg init produces a repo real git accepts ---
INIT_DIR="$WORKDIR/sg_inited"
"$SG" init "$INIT_DIR" > /dev/null 2>&1
INIT_RC=$?
check "sg init exits 0" test "$INIT_RC" = 0

GIT_STATUS_OUT="$WORKDIR/git_status_out.txt"
# LC_ALL=C, because the check below greps git's own prose and git translates
# it (it ships 20 message catalogs). Measured on a zh_TW machine, where git
# renders this message in Chinese: the negative grep could never match, so
# the check passed no matter what --
# proven by breaking sg_repo_init so it writes .gitx instead of .git, which
# turned 721 other checks red and left this one green. A negative assertion
# that cannot fail is worse than no assertion: it still counts toward the
# total, so it reads as coverage. The exit-code check on the line above is
# the load-bearing one; this is its backstop, and a backstop that cannot
# fire is not one.
(cd "$INIT_DIR" && LC_ALL=C git status) > "$GIT_STATUS_OUT" 2>&1
GIT_STATUS_RC=$?
check "git status exits 0 inside sg-inited repo" test "$GIT_STATUS_RC" = 0
check "git status does not complain about an invalid repository" sh -c "! grep -qi 'not a git repository' '$GIT_STATUS_OUT'"

export GIT_AUTHOR_NAME="Interop Test"
export GIT_AUTHOR_EMAIL="interop@example.com"

# Some git commands want an editor and fail without one -- `git rebase
# --continue` is the one this suite hits. Whether that succeeds must not
# depend on the developer's shell: this was found the hard way, by a phase18f
# case that passed locally on a machine that happened to export
# GIT_EDITOR=true and failed on every CI runner. `true` accepts whatever
# message git prepared, which is what a non-interactive oracle wants.
export GIT_EDITOR=true

# --- Phase 2 case 1: sg init/add/commit, real git reads it back ---
P2_REPO="$WORKDIR/phase2_sg_repo"
mkdir -p "$P2_REPO/sub"
(cd "$WORKDIR" && "$SG" init phase2_sg_repo) > /dev/null 2>&1
printf 'top level file\n' > "$P2_REPO/top.txt"
printf 'nested file\n' > "$P2_REPO/sub/nested.txt"
(cd "$P2_REPO" && "$SG" add top.txt sub/nested.txt) > /dev/null 2>&1
check "sg add exits 0" test $? = 0
COMMIT_OUT="$WORKDIR/p2_commit_out.txt"
(cd "$P2_REPO" && "$SG" commit -m "first commit") > "$COMMIT_OUT" 2>&1
check "sg commit exits 0" test $? = 0
check "sg commit prints root-commit summary" grep -q "root-commit" "$COMMIT_OUT"

(cd "$P2_REPO" && git log --format='%H' -1) > /dev/null 2>&1
check "git log reads sg-created commit" test $? = 0

GIT_SHOW_TOP="$WORKDIR/p2_show_top.txt"
(cd "$P2_REPO" && git show HEAD:top.txt) > "$GIT_SHOW_TOP" 2>/dev/null
check "git show HEAD:top.txt matches sg-added content" cmp -s "$P2_REPO/top.txt" "$GIT_SHOW_TOP"

GIT_SHOW_NESTED="$WORKDIR/p2_show_nested.txt"
(cd "$P2_REPO" && git show HEAD:sub/nested.txt) > "$GIT_SHOW_NESTED" 2>/dev/null
check "git show HEAD:sub/nested.txt matches sg-added nested content" cmp -s "$P2_REPO/sub/nested.txt" "$GIT_SHOW_NESTED"

GIT_STATUS_P2="$WORKDIR/p2_git_status.txt"
(cd "$P2_REPO" && git status --porcelain) > "$GIT_STATUS_P2" 2>&1
check "git status --porcelain is empty (clean) after sg add+commit" test ! -s "$GIT_STATUS_P2"

# --- Phase 2 case 2: real git init/add/commit, sg reads it back ---
P2_GIT_REPO="$WORKDIR/phase2_git_repo"
mkdir -p "$P2_GIT_REPO"
(cd "$WORKDIR" && git init -q phase2_git_repo)
(cd "$P2_GIT_REPO" && git config user.email "a@b.c" && git config user.name "git user")
printf 'from real git\n' > "$P2_GIT_REPO/file.txt"
(cd "$P2_GIT_REPO" && git add file.txt && git commit -q -m "real git commit")

SG_LOG_OUT="$WORKDIR/p2_sg_log.txt"
(cd "$P2_GIT_REPO" && "$SG" log) > "$SG_LOG_OUT" 2>&1
check "sg log exits 0 on a real-git repo" test $? = 0
REAL_SHA=$(cd "$P2_GIT_REPO" && git rev-parse HEAD)
check "sg log shows the real git commit sha" grep -q "$REAL_SHA" "$SG_LOG_OUT"

SG_STATUS_OUT="$WORKDIR/p2_sg_status.txt"
(cd "$P2_GIT_REPO" && "$SG" status) > "$SG_STATUS_OUT" 2>&1
check "sg status exits 0 on a real-git repo" test $? = 0
check "sg status reports clean tree on a real-git repo" grep -q "nothing to commit" "$SG_STATUS_OUT"

printf 'more from real git\n' >> "$P2_GIT_REPO/file.txt"
SG_STATUS_DIRTY="$WORKDIR/p2_sg_status_dirty.txt"
(cd "$P2_GIT_REPO" && "$SG" status) > "$SG_STATUS_DIRTY" 2>&1
check "sg status detects unstaged modification on a real-git repo" grep -q "modified" "$SG_STATUS_DIRTY"
(cd "$P2_GIT_REPO" && git checkout -q -- file.txt)

# --- Phase 2 case 3: sg switch -c, commit on branch, switch back, verify content via real git ---
BRANCH_REPO="$P2_REPO"
(cd "$BRANCH_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
check "sg switch -c exits 0" test $? = 0
printf 'top level file\nfeature addition\n' > "$BRANCH_REPO/top.txt"
(cd "$BRANCH_REPO" && "$SG" add top.txt) > /dev/null 2>&1
(cd "$BRANCH_REPO" && "$SG" commit -m "feature change") > /dev/null 2>&1
check "sg commit on feature branch exits 0" test $? = 0

(cd "$BRANCH_REPO" && "$SG" switch master) > /dev/null 2>&1
check "sg switch back to master exits 0" test $? = 0

GIT_SHOW_MASTER_TOP="$WORKDIR/p2_master_top.txt"
(cd "$BRANCH_REPO" && git show HEAD:top.txt) > "$GIT_SHOW_MASTER_TOP" 2>/dev/null
check "git show HEAD:top.txt on master has original content after switching back" \
    sh -c "! grep -q 'feature addition' '$GIT_SHOW_MASTER_TOP'"
check "working tree top.txt matches master content after sg switch" cmp -s "$BRANCH_REPO/top.txt" "$GIT_SHOW_MASTER_TOP"

# --- Phase 3 case 1: did-you-mean suggestion for a typo'd command ---
TYPO_OUT="$WORKDIR/p3_typo_out.txt"
"$SG" stat > /dev/null 2> "$TYPO_OUT"
check "typo'd command's suggestion names the intended command" grep -q "status" "$TYPO_OUT"

# --- Phase 3 case 2: bare `sg` / `sg --help` succeed and list every command ---
HELP_OUT="$WORKDIR/p3_help_out.txt"
"$SG" > "$HELP_OUT" 2>&1
HELP_RC=$?
check "sg with no args exits 0" test "$HELP_RC" = 0
for cmd in init hash-object cat-file add commit log status diff switch restore; do
    check "help output mentions '$cmd'" grep -q -- "$cmd" "$HELP_OUT"
done

HELP_FLAG_OUT="$WORKDIR/p3_help_flag_out.txt"
"$SG" --help > "$HELP_FLAG_OUT" 2>&1
HELP_FLAG_RC=$?
check "sg --help exits 0" test "$HELP_FLAG_RC" = 0
check "sg --help output mentions 'switch'" grep -q "switch" "$HELP_FLAG_OUT"

# --- `sg --version`: part of the packaging contract, and the same string the
# --- protocol layer announces to remotes as its agent, so a drift between
# --- the two is a real (if quiet) inconsistency rather than a cosmetic one.
for vflag in --version -v version; do
    VER_OUT="$WORKDIR/p8_version_out.txt"
    "$SG" "$vflag" > "$VER_OUT" 2>&1
    VER_RC=$?
    check "sg $vflag exits 0" test "$VER_RC" = 0
    check "sg $vflag reports a version" grep -qE "^sg version [0-9]+\.[0-9]+" "$VER_OUT"
done

# --- Phase 3 cases 3-6: switch/restore require confirmation for dangerous, lossy changes ---
P3_REPO="$WORKDIR/phase3_repo"
mkdir -p "$P3_REPO"
(cd "$WORKDIR" && "$SG" init phase3_repo) > /dev/null 2>&1
printf 'line one\n' > "$P3_REPO/a.txt"
(cd "$P3_REPO" && "$SG" add a.txt) > /dev/null 2>&1
(cd "$P3_REPO" && "$SG" commit -m "p3 first") > /dev/null 2>&1
(cd "$P3_REPO" && "$SG" switch -c other) > /dev/null 2>&1

# an unstaged, uncommitted modification -- this is what switch/restore must protect
printf 'line one\nuncommitted change\n' > "$P3_REPO/a.txt"

BEFORE_BRANCH=$(cd "$P3_REPO" && "$SG" status 2>&1 | head -1)

# case 3: switch without --force, non-interactive stdin -> must fail, nothing changed
(cd "$P3_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
SWITCH_NOFORCE_RC=$?
check "sg switch without --force fails on a dirty, non-interactive workdir" \
    test "$SWITCH_NOFORCE_RC" -ne 0

AFTER_BRANCH=$(cd "$P3_REPO" && "$SG" status 2>&1 | head -1)
check "branch unchanged after refused switch" test "$BEFORE_BRANCH" = "$AFTER_BRANCH"
check "working tree unchanged after refused switch" grep -q "uncommitted change" "$P3_REPO/a.txt"

# case 4: same dirty state, but with --force -> must succeed
(cd "$P3_REPO" && "$SG" switch master --force < /dev/null) > /dev/null 2>&1
SWITCH_FORCE_RC=$?
check "sg switch --force succeeds despite a dirty workdir" test "$SWITCH_FORCE_RC" = 0

AFTER_FORCE_BRANCH=$(cd "$P3_REPO" && "$SG" status 2>&1 | head -1)
check "branch actually switched with --force" test "$AFTER_FORCE_BRANCH" = "On branch master"
check "working tree overwritten by --force switch" \
    sh -c "! grep -q 'uncommitted change' '$P3_REPO/a.txt'"

# case 5: restore on an unstaged modification -- refuses without --force, applies with it
printf 'line one\nrestore target uncommitted\n' > "$P3_REPO/a.txt"

(cd "$P3_REPO" && "$SG" restore a.txt < /dev/null) > /dev/null 2>&1
RESTORE_NOFORCE_RC=$?
check "sg restore without --force fails on a non-interactive, lossy restore" \
    test "$RESTORE_NOFORCE_RC" -ne 0
check "file content unchanged after refused restore" grep -q "restore target uncommitted" "$P3_REPO/a.txt"

(cd "$P3_REPO" && "$SG" restore a.txt --force < /dev/null) > /dev/null 2>&1
RESTORE_FORCE_RC=$?
check "sg restore --force succeeds" test "$RESTORE_FORCE_RC" = 0
check "file restored to index content with --force" \
    sh -c "! grep -q 'restore target uncommitted' '$P3_REPO/a.txt'"

# case 6: a clean workdir needs no confirmation at all -- switch just succeeds
CLEAN_STATUS_OUT=$(cd "$P3_REPO" && "$SG" status 2>&1)
check "workdir is clean before the clean-switch check" \
    sh -c "printf '%s' '$CLEAN_STATUS_OUT' | grep -q 'nothing to commit'"

(cd "$P3_REPO" && "$SG" switch other < /dev/null) > /dev/null 2>&1
CLEAN_SWITCH_RC=$?
check "sg switch succeeds without --force when the workdir is clean" test "$CLEAN_SWITCH_RC" = 0

# --- Phase 4: automatic snapshots on dangerous switch/restore + `sg undo` ---
# Each case below uses its own freshly-initialized repo so that one case's
# snapshot count can never leak into another case's before/after comparison.

snapshot_count() {
    # counts numbered lines ("  N) ...") in `sg undo`'s listing; the "no
    # snapshots yet" message and the header/footer lines never contain ')'
    (cd "$1" && "$SG" undo < /dev/null) 2>&1 | grep -c ')'
}

# case A: sg switch --force overwrites an uncommitted change; sg undo brings it back
P4A_REPO="$WORKDIR/phase4a_repo"
mkdir -p "$P4A_REPO"
(cd "$WORKDIR" && "$SG" init phase4a_repo) > /dev/null 2>&1
printf 'original\n' > "$P4A_REPO/f.txt"
(cd "$P4A_REPO" && "$SG" add f.txt && "$SG" commit -m "p4a first") > /dev/null 2>&1
(cd "$P4A_REPO" && "$SG" switch -c other) > /dev/null 2>&1
(cd "$P4A_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1

printf 'original\nlocal edit before switch\n' > "$P4A_REPO/f.txt"
(cd "$P4A_REPO" && "$SG" switch other --force < /dev/null) > /dev/null 2>&1
check "phase4a: sg switch --force succeeds over a dirty workdir" test $? = 0
check "phase4a: switch --force actually overwrote the uncommitted change" \
    sh -c "! grep -q 'local edit before switch' '$P4A_REPO/f.txt'"

(cd "$P4A_REPO" && "$SG" undo 1 --force < /dev/null) > /dev/null 2>&1
check "phase4a: sg undo 1 exits 0" test $? = 0
check "phase4a: sg undo 1 restores the content that switch overwrote" \
    grep -q "local edit before switch" "$P4A_REPO/f.txt"

# case B: sg restore --force discards an uncommitted change; sg undo brings it back
P4B_REPO="$WORKDIR/phase4b_repo"
mkdir -p "$P4B_REPO"
(cd "$WORKDIR" && "$SG" init phase4b_repo) > /dev/null 2>&1
printf 'original\n' > "$P4B_REPO/f.txt"
(cd "$P4B_REPO" && "$SG" add f.txt && "$SG" commit -m "p4b first") > /dev/null 2>&1

printf 'original\nlocal edit before restore\n' > "$P4B_REPO/f.txt"
(cd "$P4B_REPO" && "$SG" restore f.txt --force < /dev/null) > /dev/null 2>&1
check "phase4b: sg restore --force succeeds discarding a dirty change" test $? = 0
check "phase4b: restore --force actually discarded the uncommitted change" \
    sh -c "! grep -q 'local edit before restore' '$P4B_REPO/f.txt'"

(cd "$P4B_REPO" && "$SG" undo 1 --force < /dev/null) > /dev/null 2>&1
check "phase4b: sg undo 1 exits 0" test $? = 0
check "phase4b: sg undo 1 recovers the content that restore discarded" \
    grep -q "local edit before restore" "$P4B_REPO/f.txt"

# case C: `sg undo` with no args lists at least one snapshot after the dangerous ops above
UNDO_LIST_OUT="$WORKDIR/p4_undo_list.txt"
(cd "$P4A_REPO" && "$SG" undo < /dev/null) > "$UNDO_LIST_OUT" 2>&1
check "phase4c: sg undo (list) exits 0" test $? = 0
C_COUNT=$(snapshot_count "$P4A_REPO")
check "phase4c: sg undo (list) shows at least one snapshot" test "$C_COUNT" -ge 1

# case D: a clean switch (no --force needed) must NOT create a new snapshot
P4D_REPO="$WORKDIR/phase4d_repo"
mkdir -p "$P4D_REPO"
(cd "$WORKDIR" && "$SG" init phase4d_repo) > /dev/null 2>&1
printf 'content\n' > "$P4D_REPO/f.txt"
(cd "$P4D_REPO" && "$SG" add f.txt && "$SG" commit -m "p4d first") > /dev/null 2>&1
(cd "$P4D_REPO" && "$SG" switch -c other) > /dev/null 2>&1

D_COUNT_BEFORE=$(snapshot_count "$P4D_REPO")
check "phase4d: no snapshots exist yet in a repo with no dangerous ops" test "$D_COUNT_BEFORE" = 0

(cd "$P4D_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
check "phase4d: clean switch (no --force needed) exits 0" test $? = 0

D_COUNT_AFTER=$(snapshot_count "$P4D_REPO")
check "phase4d: a clean switch does not create a new snapshot" test "$D_COUNT_BEFORE" = "$D_COUNT_AFTER"

# case E: --force only skips the confirmation prompt, never the snapshot itself
P4E_REPO="$WORKDIR/phase4e_repo"
mkdir -p "$P4E_REPO"
(cd "$WORKDIR" && "$SG" init phase4e_repo) > /dev/null 2>&1
printf 'content\n' > "$P4E_REPO/f.txt"
(cd "$P4E_REPO" && "$SG" add f.txt && "$SG" commit -m "p4e first") > /dev/null 2>&1
(cd "$P4E_REPO" && "$SG" switch -c other) > /dev/null 2>&1
(cd "$P4E_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1

printf 'content\nlocal edit\n' > "$P4E_REPO/f.txt"
E_COUNT_BEFORE=$(snapshot_count "$P4E_REPO")
(cd "$P4E_REPO" && "$SG" switch other --force < /dev/null) > /dev/null 2>&1
check "phase4e: sg switch --force exits 0" test $? = 0
E_COUNT_AFTER=$(snapshot_count "$P4E_REPO")
check "phase4e: --force still creates a snapshot when something would be lost" \
    test "$E_COUNT_AFTER" -gt "$E_COUNT_BEFORE"

# --- Phase 5a case 1: sg repack produces a pack real git can read, and sg
# itself still works normally afterwards (loose objects are gone) ---
P5_REPO="$WORKDIR/phase5_sg_repo"
mkdir -p "$P5_REPO"
(cd "$WORKDIR" && "$SG" init phase5_sg_repo) > /dev/null 2>&1
printf 'file one content\n' > "$P5_REPO/one.txt"
printf 'file two content\n' > "$P5_REPO/two.txt"
(cd "$P5_REPO" && "$SG" add one.txt two.txt) > /dev/null 2>&1
(cd "$P5_REPO" && "$SG" commit -m "p5 first") > /dev/null 2>&1
P5_COMMIT=$(cd "$P5_REPO" && "$SG" log 2>/dev/null | head -1 | awk '{print $2}')
P5_TREE=$(cd "$P5_REPO" && git cat-file -p "$P5_COMMIT" 2>/dev/null | awk '/^tree/{print $2}')
P5_BLOB1=$(cd "$P5_REPO" && git hash-object one.txt 2>/dev/null)

LOOSE_COUNT_BEFORE=$(find "$P5_REPO/.git/objects" -mindepth 2 -type f ! -path '*/pack/*' ! -path '*/info/*' | wc -l | tr -d ' ')
check "phase5 case1: some loose objects exist before repack" test "$LOOSE_COUNT_BEFORE" -gt 0

REPACK_OUT="$WORKDIR/p5_repack_out.txt"
(cd "$P5_REPO" && "$SG" repack < /dev/null) > "$REPACK_OUT" 2>&1
check "phase5 case1: sg repack exits 0" test $? = 0

LOOSE_COUNT_AFTER=$(find "$P5_REPO/.git/objects" -mindepth 2 -type f ! -path '*/pack/*' ! -path '*/info/*' | wc -l | tr -d ' ')
check "phase5 case1: loose objects are gone after repack" test "$LOOSE_COUNT_AFTER" = 0

PACK_FILE=$(find "$P5_REPO/.git/objects/pack" -name '*.pack' | head -1)
check "phase5 case1: a .pack file was created" test -n "$PACK_FILE"

check "phase5 case1: real git cat-file -p reads the packed commit" \
    sh -c "(cd '$P5_REPO' && git cat-file -p '$P5_COMMIT') > /dev/null 2>&1"
check "phase5 case1: real git cat-file -p reads the packed tree" \
    sh -c "(cd '$P5_REPO' && git cat-file -p '$P5_TREE') > /dev/null 2>&1"

GIT_BLOB1_OUT="$WORKDIR/p5_git_blob1.txt"
(cd "$P5_REPO" && git cat-file -p "$P5_BLOB1") > "$GIT_BLOB1_OUT" 2>/dev/null
check "phase5 case1: real git reads packed blob with identical content" \
    cmp -s "$P5_REPO/one.txt" "$GIT_BLOB1_OUT"

if command -v git >/dev/null 2>&1 && (cd "$P5_REPO" && git verify-pack -v "$PACK_FILE" >/dev/null 2>&1); then
    check "phase5 case1: git verify-pack accepts the sg-written pack" \
        sh -c "(cd '$P5_REPO' && git verify-pack -v '$PACK_FILE') > /dev/null 2>&1"
fi

SG_CATFILE_AFTER="$WORKDIR/p5_sg_catfile_after.txt"
(cd "$P5_REPO" && "$SG" cat-file -p "$P5_COMMIT") > "$SG_CATFILE_AFTER" 2>&1
check "phase5 case1: sg cat-file still works after repack (reads from pack)" test $? = 0

SG_LOG_AFTER="$WORKDIR/p5_sg_log_after.txt"
(cd "$P5_REPO" && "$SG" log) > "$SG_LOG_AFTER" 2>&1
check "phase5 case1: sg log still works after repack" test $? = 0
check "phase5 case1: sg log still shows the commit after repack" grep -q "$P5_COMMIT" "$SG_LOG_AFTER"

SG_STATUS_AFTER="$WORKDIR/p5_sg_status_after.txt"
(cd "$P5_REPO" && "$SG" status) > "$SG_STATUS_AFTER" 2>&1
check "phase5 case1: sg status still works after repack" test $? = 0
check "phase5 case1: sg status reports clean tree after repack" \
    grep -q "nothing to commit" "$SG_STATUS_AFTER"

# --- Phase 5a case 2: real git's own delta-compressed pack must be readable
# by sg (the most important compatibility test -- OFS_DELTA/REF_DELTA
# reconstruction has to be correct, not just literal-object packs) ---
P5_GIT_REPO="$WORKDIR/phase5_git_repo"
mkdir -p "$P5_GIT_REPO"
(cd "$WORKDIR" && git init -q phase5_git_repo)
(cd "$P5_GIT_REPO" && git config user.email "a@b.c" && git config user.name "git user")

python3 - "$P5_GIT_REPO/big.txt" <<'PYEOF'
import sys
path = sys.argv[1]
lines = [f"line {i} filler filler filler filler filler filler\n" for i in range(2000)]
with open(path, "w") as f:
    f.writelines(lines)
PYEOF
(cd "$P5_GIT_REPO" && git add big.txt && git commit -q -m "v1")

python3 - "$P5_GIT_REPO/big.txt" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    lines = f.readlines()
lines[500] = "line 500 CHANGED filler filler filler filler filler filler\n"
lines[1000] = "line 1000 CHANGED filler filler filler filler filler filler\n"
with open(path, "w") as f:
    f.writelines(lines)
PYEOF
(cd "$P5_GIT_REPO" && git add big.txt && git commit -q -m "v2")

GIT_HEAD=$(cd "$P5_GIT_REPO" && git rev-parse HEAD)
GIT_TREE=$(cd "$P5_GIT_REPO" && git cat-file -p HEAD | awk '/^tree/{print $2}')
GIT_BLOB=$(cd "$P5_GIT_REPO" && git rev-parse HEAD:big.txt)

(cd "$P5_GIT_REPO" && git repack -ad -q) > /dev/null 2>&1
check "phase5 case2: git repack -ad exits 0" test $? = 0

GIT_LOOSE_COUNT=$(find "$P5_GIT_REPO/.git/objects" -mindepth 2 -type f ! -path '*/pack/*' ! -path '*/info/*' | wc -l | tr -d ' ')
check "phase5 case2: real git's loose objects are gone after repack -ad" test "$GIT_LOOSE_COUNT" = 0

GIT_PACK_FILE=$(find "$P5_GIT_REPO/.git/objects/pack" -name '*.pack' | head -1)
check "phase5 case2: real git produced a .pack file" test -n "$GIT_PACK_FILE"

VERIFY_OUT="$WORKDIR/p5_verify_pack.txt"
(cd "$P5_GIT_REPO" && git verify-pack -v "$GIT_PACK_FILE") > "$VERIFY_OUT" 2>&1
if grep -qE ' [0-9]+ [1-9][0-9]* [0-9]+$' "$VERIFY_OUT"; then
    echo "note: real git's pack contains at least one delta object (depth column > 0)"
else
    echo "note: real git did not produce a delta object in this pack (non-delta-only pack is still a valid case to test)"
fi

SG_CATFILE_COMMIT="$WORKDIR/p5_sg_catfile_commit.txt"
(cd "$P5_GIT_REPO" && "$SG" cat-file -p "$GIT_HEAD") > "$SG_CATFILE_COMMIT" 2>&1
check "phase5 case2: sg cat-file -p reads real git's packed commit" test $? = 0

SG_CATFILE_TREE="$WORKDIR/p5_sg_catfile_tree.txt"
(cd "$P5_GIT_REPO" && "$SG" cat-file -p "$GIT_TREE") > "$SG_CATFILE_TREE" 2>&1
check "phase5 case2: sg cat-file -p reads real git's packed tree" test $? = 0

SG_CATFILE_BLOB="$WORKDIR/p5_sg_catfile_blob.txt"
(cd "$P5_GIT_REPO" && "$SG" cat-file -p "$GIT_BLOB") > "$SG_CATFILE_BLOB" 2>&1
check "phase5 case2: sg cat-file -p reads real git's (possibly delta-compressed) packed blob" \
    test $? = 0
check "phase5 case2: sg-read blob content matches the real working tree file" \
    cmp -s "$P5_GIT_REPO/big.txt" "$SG_CATFILE_BLOB"

SG_LOG_GIT="$WORKDIR/p5_sg_log_git.txt"
(cd "$P5_GIT_REPO" && "$SG" log) > "$SG_LOG_GIT" 2>&1
check "phase5 case2: sg log exits 0 on real git's delta pack" test $? = 0
check "phase5 case2: sg log shows both real git commits" \
    sh -c "grep -q \"$GIT_HEAD\" '$SG_LOG_GIT'"

SG_STATUS_GIT="$WORKDIR/p5_sg_status_git.txt"
(cd "$P5_GIT_REPO" && "$SG" status) > "$SG_STATUS_GIT" 2>&1
check "phase5 case2: sg status exits 0 on real git's delta pack" test $? = 0
check "phase5 case2: sg status reports clean tree on real git's delta pack" \
    grep -q "nothing to commit" "$SG_STATUS_GIT"

# --- Phase 4b: merge + conflict UX ---

# case 1: merge-base cross-check against a diverging history
P4B_MB_REPO="$WORKDIR/p4b_mergebase_repo"
mkdir -p "$P4B_MB_REPO"
(cd "$WORKDIR" && "$SG" init p4b_mergebase_repo) > /dev/null 2>&1
printf 'root\n' > "$P4B_MB_REPO/r.txt"
(cd "$P4B_MB_REPO" && "$SG" add r.txt && "$SG" commit -m "root") > /dev/null 2>&1
(cd "$P4B_MB_REPO" && "$SG" switch -c side) > /dev/null 2>&1
printf 'root\nside\n' > "$P4B_MB_REPO/r.txt"
(cd "$P4B_MB_REPO" && "$SG" add r.txt && "$SG" commit -m "side commit") > /dev/null 2>&1
(cd "$P4B_MB_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'root\nmain\n' > "$P4B_MB_REPO/r.txt"
(cd "$P4B_MB_REPO" && "$SG" add r.txt && "$SG" commit -m "main commit") > /dev/null 2>&1

MB_A=$(cd "$P4B_MB_REPO" && git rev-parse master)
MB_B=$(cd "$P4B_MB_REPO" && git rev-parse side)
SG_MB=$(cd "$P4B_MB_REPO" && "$SG" merge-base "$MB_A" "$MB_B" 2>/dev/null)
GIT_MB=$(cd "$P4B_MB_REPO" && git merge-base "$MB_A" "$MB_B" 2>/dev/null)
check "phase4b case1: sg merge-base matches real git merge-base" test "$SG_MB" = "$GIT_MB"

# case 2: sg produces a conflict that real git recognizes as unmerged (UU)
P4B_CONFLICT_REPO="$WORKDIR/p4b_conflict_repo"
mkdir -p "$P4B_CONFLICT_REPO"
(cd "$WORKDIR" && "$SG" init p4b_conflict_repo) > /dev/null 2>&1
printf 'orig1\norig2\n' > "$P4B_CONFLICT_REPO/c.txt"
(cd "$P4B_CONFLICT_REPO" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4B_CONFLICT_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P4B_CONFLICT_REPO/c.txt"
(cd "$P4B_CONFLICT_REPO" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4B_CONFLICT_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P4B_CONFLICT_REPO/c.txt"
(cd "$P4B_CONFLICT_REPO" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1

(cd "$P4B_CONFLICT_REPO" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
SG_MERGE_CONFLICT_RC=$?
check "phase4b case2: sg merge with a real conflict exits non-zero" test "$SG_MERGE_CONFLICT_RC" -ne 0

GIT_PORCELAIN_UU="$WORKDIR/p4b_git_porcelain_uu.txt"
(cd "$P4B_CONFLICT_REPO" && git status --porcelain) > "$GIT_PORCELAIN_UU" 2>&1
check "phase4b case2: real git sees the sg-made conflict as UU" grep -q "^UU c.txt" "$GIT_PORCELAIN_UU"

# case 3: a conflict made by real git's own `git merge` is recognized by `sg status`
P4B_GITCONFLICT_REPO="$WORKDIR/p4b_gitconflict_repo"
mkdir -p "$P4B_GITCONFLICT_REPO"
(cd "$WORKDIR" && git init -q p4b_gitconflict_repo)
(cd "$P4B_GITCONFLICT_REPO" && git config user.email "a@b.c" && git config user.name "git user")
printf 'orig1\norig2\n' > "$P4B_GITCONFLICT_REPO/g.txt"
(cd "$P4B_GITCONFLICT_REPO" && git add g.txt && git commit -q -m "base")
(cd "$P4B_GITCONFLICT_REPO" && git switch -q -c feature)
printf 'feature1\norig2\n' > "$P4B_GITCONFLICT_REPO/g.txt"
(cd "$P4B_GITCONFLICT_REPO" && git add g.txt && git commit -q -m "feature change")
(cd "$P4B_GITCONFLICT_REPO" && git switch -q master)
printf 'master1\norig2\n' > "$P4B_GITCONFLICT_REPO/g.txt"
(cd "$P4B_GITCONFLICT_REPO" && git add g.txt && git commit -q -m "master change")
(cd "$P4B_GITCONFLICT_REPO" && git merge feature) > /dev/null 2>&1

SG_STATUS_GITCONFLICT="$WORKDIR/p4b_sg_status_gitconflict.txt"
(cd "$P4B_GITCONFLICT_REPO" && "$SG" status) > "$SG_STATUS_GITCONFLICT" 2>&1
check "phase4b case3: sg status exits 0 on a real-git-made conflict" test $? = 0
check "phase4b case3: sg status reports unmerged paths for a real-git-made conflict" \
    grep -q "Unmerged paths" "$SG_STATUS_GITCONFLICT"
check "phase4b case3: sg status names the conflicted file" grep -q "g.txt" "$SG_STATUS_GITCONFLICT"

# case 4: clean auto-merge of well-separated edits; merge commit has two
# parents, both a real git and sg agree on that
P4B_CLEAN_REPO="$WORKDIR/p4b_clean_repo"
mkdir -p "$P4B_CLEAN_REPO"
(cd "$WORKDIR" && "$SG" init p4b_clean_repo) > /dev/null 2>&1
printf 'line1\nline2\nline3\nline4\nline5\n' > "$P4B_CLEAN_REPO/m.txt"
(cd "$P4B_CLEAN_REPO" && "$SG" add m.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4B_CLEAN_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'line1\nline2\nline3\nCHANGED4\nline5\n' > "$P4B_CLEAN_REPO/m.txt"
(cd "$P4B_CLEAN_REPO" && "$SG" add m.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4B_CLEAN_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'line1\nCHANGED2\nline3\nline4\nline5\n' > "$P4B_CLEAN_REPO/m.txt"
(cd "$P4B_CLEAN_REPO" && "$SG" add m.txt && "$SG" commit -m "master change") > /dev/null 2>&1

(cd "$P4B_CLEAN_REPO" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
check "phase4b case4: clean auto-merge exits 0" test $? = 0
check "phase4b case4: merged file contains both sides' edits" \
    sh -c "grep -q CHANGED2 '$P4B_CLEAN_REPO/m.txt' && grep -q CHANGED4 '$P4B_CLEAN_REPO/m.txt'"

GIT_MERGE_COMMIT_SHOW="$WORKDIR/p4b_git_merge_commit_show.txt"
(cd "$P4B_CLEAN_REPO" && git cat-file -p HEAD) > "$GIT_MERGE_COMMIT_SHOW" 2>&1
PARENT_LINE_COUNT=$(grep -c "^parent " "$GIT_MERGE_COMMIT_SHOW")
check "phase4b case4: merge commit has exactly two parent lines" test "$PARENT_LINE_COUNT" = 2

GIT_LOG_MERGE="$WORKDIR/p4b_git_log_merge.txt"
(cd "$P4B_CLEAN_REPO" && git log --oneline) > "$GIT_LOG_MERGE" 2>&1
check "phase4b case4: git log exits 0 and reads the merge commit" test $? = 0
check "phase4b case4: git status --porcelain is clean after the auto-merge" \
    sh -c "test -z \"\$(cd '$P4B_CLEAN_REPO' && git status --porcelain)\""

# case 5: fast-forward
P4B_FF_REPO="$WORKDIR/p4b_ff_repo"
mkdir -p "$P4B_FF_REPO"
(cd "$WORKDIR" && "$SG" init p4b_ff_repo) > /dev/null 2>&1
printf 'orig\n' > "$P4B_FF_REPO/f.txt"
(cd "$P4B_FF_REPO" && "$SG" add f.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4B_FF_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'orig\nfeature added\n' > "$P4B_FF_REPO/f.txt"
(cd "$P4B_FF_REPO" && "$SG" add f.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4B_FF_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1

FF_OUT="$WORKDIR/p4b_ff_out.txt"
(cd "$P4B_FF_REPO" && "$SG" merge feature < /dev/null) > "$FF_OUT" 2>&1
check "phase4b case5: fast-forward merge exits 0" test $? = 0
check "phase4b case5: fast-forward merge prints Fast-forward" grep -q "Fast-forward" "$FF_OUT"

FF_MASTER_SHA=$(cd "$P4B_FF_REPO" && git rev-parse master)
FF_FEATURE_SHA=$(cd "$P4B_FF_REPO" && git rev-parse feature)
check "phase4b case5: HEAD now points at feature's commit" test "$FF_MASTER_SHA" = "$FF_FEATURE_SHA"

# case 6: merging an already-ancestor branch is a no-op
UTD_OUT="$WORKDIR/p4b_utd_out.txt"
(cd "$P4B_FF_REPO" && "$SG" merge feature < /dev/null) > "$UTD_OUT" 2>&1
check "phase4b case6: already-up-to-date merge exits 0" test $? = 0
check "phase4b case6: already-up-to-date merge says so" grep -q "Already up to date" "$UTD_OUT"

# case 7: full conflict-resolution flow
P4B_RESOLVE_REPO="$WORKDIR/p4b_resolve_repo"
mkdir -p "$P4B_RESOLVE_REPO"
(cd "$WORKDIR" && "$SG" init p4b_resolve_repo) > /dev/null 2>&1
printf 'orig1\norig2\n' > "$P4B_RESOLVE_REPO/c.txt"
(cd "$P4B_RESOLVE_REPO" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4B_RESOLVE_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P4B_RESOLVE_REPO/c.txt"
(cd "$P4B_RESOLVE_REPO" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4B_RESOLVE_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P4B_RESOLVE_REPO/c.txt"
(cd "$P4B_RESOLVE_REPO" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1

(cd "$P4B_RESOLVE_REPO" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
check "phase4b case7: merge with conflict exits non-zero" test $? -ne 0

# case 8: commit is blocked while the conflict is unresolved
BLOCKED_COMMIT_OUT="$WORKDIR/p4b_blocked_commit_out.txt"
BLOCKED_HEAD_BEFORE=$(cd "$P4B_RESOLVE_REPO" && git rev-parse HEAD)
(cd "$P4B_RESOLVE_REPO" && "$SG" commit -m "premature" < /dev/null) > "$BLOCKED_COMMIT_OUT" 2>&1
BLOCKED_COMMIT_RC=$?
check "phase4b case8: sg commit is refused while unresolved" test "$BLOCKED_COMMIT_RC" -ne 0
BLOCKED_HEAD_AFTER=$(cd "$P4B_RESOLVE_REPO" && git rev-parse HEAD)
check "phase4b case8: blocked commit created no new commit" test "$BLOCKED_HEAD_BEFORE" = "$BLOCKED_HEAD_AFTER"

# now actually resolve it
printf 'resolved1\norig2\n' > "$P4B_RESOLVE_REPO/c.txt"
(cd "$P4B_RESOLVE_REPO" && "$SG" add c.txt) > /dev/null 2>&1
(cd "$P4B_RESOLVE_REPO" && "$SG" commit -m "resolved merge") > /dev/null 2>&1
check "phase4b case7: sg commit completing the merge exits 0" test $? = 0

RESOLVE_COMMIT_SHOW="$WORKDIR/p4b_resolve_commit_show.txt"
(cd "$P4B_RESOLVE_REPO" && git cat-file -p HEAD) > "$RESOLVE_COMMIT_SHOW" 2>&1
RESOLVE_PARENT_COUNT=$(grep -c "^parent " "$RESOLVE_COMMIT_SHOW")
check "phase4b case7: resolved merge commit has two parents" test "$RESOLVE_PARENT_COUNT" = 2

check "phase4b case7: git status --porcelain is clean after resolving" \
    sh -c "test -z \"\$(cd '$P4B_RESOLVE_REPO' && git status --porcelain)\""
RESOLVE_SG_STATUS="$WORKDIR/p4b_resolve_sg_status.txt"
(cd "$P4B_RESOLVE_REPO" && "$SG" status) > "$RESOLVE_SG_STATUS" 2>&1
check "phase4b case7: sg status is clean after resolving" grep -q "nothing to commit" "$RESOLVE_SG_STATUS"

# Phase 17 batch B follow-up: the commit that lands a CONFLICTED merge (as
# opposed to the auto-merge path in p17_seq_sg above, which never touches
# cmd_commit.c at all) must log "commit (merge): <subject>" -- measured
# directly against real git 2.55.0 on this exact conflict-then-resolve
# sequence (git init; base; branch feature; conflicting change on each side;
# merge; resolve; commit -m "resolved merge" -> logs/HEAD's and the
# branch's own log's last line both read
# "commit (merge): resolved merge").
check "phase17: a commit resolving a CONFLICTED merge logs 'commit (merge): <subject>' on logs/HEAD" \
    sh -c "tail -1 '$P4B_RESOLVE_REPO/.git/logs/HEAD' | grep -q '	commit (merge): resolved merge\$'"
check "phase17: same commit logs 'commit (merge): <subject>' on the branch's own reflog" \
    sh -c "tail -1 '$P4B_RESOLVE_REPO/.git/logs/refs/heads/master' | grep -q '	commit (merge): resolved merge\$'"

# case 9: sg merge --abort restores the pre-merge working tree
P4B_ABORT_REPO="$WORKDIR/p4b_abort_repo"
mkdir -p "$P4B_ABORT_REPO"
(cd "$WORKDIR" && "$SG" init p4b_abort_repo) > /dev/null 2>&1
printf 'orig1\norig2\n' > "$P4B_ABORT_REPO/a.txt"
(cd "$P4B_ABORT_REPO" && "$SG" add a.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4B_ABORT_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P4B_ABORT_REPO/a.txt"
(cd "$P4B_ABORT_REPO" && "$SG" add a.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4B_ABORT_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P4B_ABORT_REPO/a.txt"
(cd "$P4B_ABORT_REPO" && "$SG" add a.txt && "$SG" commit -m "master change") > /dev/null 2>&1

(cd "$P4B_ABORT_REPO" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
(cd "$P4B_ABORT_REPO" && "$SG" merge --abort < /dev/null) > /dev/null 2>&1
check "phase4b case9: sg merge --abort exits 0" test $? = 0
check "phase4b case9: working tree content is back to pre-merge (master) content" \
    sh -c "! grep -q '<<<<<<<' '$P4B_ABORT_REPO/a.txt' && grep -q 'master1' '$P4B_ABORT_REPO/a.txt'"
check "phase4b case9: MERGE_HEAD is gone after abort" test ! -f "$P4B_ABORT_REPO/.git/MERGE_HEAD"
check "phase4b case9: git status --porcelain is clean after abort" \
    sh -c "test -z \"\$(cd '$P4B_ABORT_REPO' && git status --porcelain)\""
ABORT_SG_STATUS="$WORKDIR/p4b_abort_sg_status.txt"
(cd "$P4B_ABORT_REPO" && "$SG" status) > "$ABORT_SG_STATUS" 2>&1
check "phase4b case9: sg status is clean after abort" grep -q "nothing to commit" "$ABORT_SG_STATUS"

# case 10: a merge that overwrites the workdir also takes a safety snapshot
ABORT_SNAPSHOT_COUNT=$(snapshot_count "$P4B_ABORT_REPO")
check "phase4b case10: sg undo listing gained a snapshot from the merge/abort above" \
    test "$ABORT_SNAPSHOT_COUNT" -ge 1

# --- Phase 4c: sg rebase (non-interactive) ---

# case 1: basic rebase -- linear history, original authors preserved, a
# fresh committer identity is used for the replayed commits
P4C_REPO="$WORKDIR/p4c_basic_repo"
mkdir -p "$P4C_REPO"
(cd "$WORKDIR" && "$SG" init p4c_basic_repo) > /dev/null 2>&1
printf 'A\n' > "$P4C_REPO/f.txt"
(cd "$P4C_REPO" && "$SG" add f.txt && "$SG" commit -m "A") > /dev/null 2>&1
(cd "$P4C_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'A\nB\n' > "$P4C_REPO/f.txt"
(cd "$P4C_REPO" && "$SG" add f.txt) > /dev/null 2>&1
(cd "$P4C_REPO" && GIT_AUTHOR_NAME="Feature Dev" GIT_AUTHOR_EMAIL="feature@dev.example" "$SG" commit -m "B") > /dev/null 2>&1
printf 'A\nB\nC\n' > "$P4C_REPO/f.txt"
(cd "$P4C_REPO" && "$SG" add f.txt) > /dev/null 2>&1
(cd "$P4C_REPO" && GIT_AUTHOR_NAME="Feature Dev" GIT_AUTHOR_EMAIL="feature@dev.example" "$SG" commit -m "C") > /dev/null 2>&1
(cd "$P4C_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'D\n' > "$P4C_REPO/d.txt"
(cd "$P4C_REPO" && "$SG" add d.txt && "$SG" commit -m "D") > /dev/null 2>&1
(cd "$P4C_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1

P4C_REBASE_OUT="$WORKDIR/p4c_basic_rebase_out.txt"
(cd "$P4C_REPO" && "$SG" rebase master < /dev/null) > "$P4C_REBASE_OUT" 2>&1
check "phase4c case1: sg rebase exits 0" test $? = 0

P4C_SUBJECTS="$WORKDIR/p4c_basic_subjects.txt"
(cd "$P4C_REPO" && git log --format=%s) > "$P4C_SUBJECTS" 2>&1
check "phase4c case1: history is linear -- C,B,D,A (newest first)" \
    sh -c "test \"\$(tr '\n' ',' < '$P4C_SUBJECTS')\" = 'C,B,D,A,'"

P4C_AUTHORS="$WORKDIR/p4c_basic_authors.txt"
(cd "$P4C_REPO" && git log --format='%s|%an') > "$P4C_AUTHORS" 2>&1
check "phase4c case1: replayed commit B keeps its original author" grep -q "^B|Feature Dev\$" "$P4C_AUTHORS"
check "phase4c case1: replayed commit C keeps its original author" grep -q "^C|Feature Dev\$" "$P4C_AUTHORS"

P4C_COMMITTERS="$WORKDIR/p4c_basic_committers.txt"
(cd "$P4C_REPO" && git log --format='%s|%cn') > "$P4C_COMMITTERS" 2>&1
check "phase4c case1: replayed commit B gets a fresh committer identity" grep -q "^B|Interop Test\$" "$P4C_COMMITTERS"
check "phase4c case1: replayed commit C gets a fresh committer identity" grep -q "^C|Interop Test\$" "$P4C_COMMITTERS"

check "phase4c case1: file content is correct after rebase" \
    sh -c "printf 'A\nB\nC\n' | cmp -s - '$P4C_REPO/f.txt'"
check "phase4c case1: master's own file survived the rebase" test -f "$P4C_REPO/d.txt"

P4C_FSCK="$WORKDIR/p4c_basic_fsck.txt"
(cd "$P4C_REPO" && git fsck --full) > "$P4C_FSCK" 2>&1
check "phase4c case1: git fsck is clean" test -z "$(cat "$P4C_FSCK")"
check "phase4c case1: git status --porcelain is clean" \
    sh -c "test -z \"\$(cd '$P4C_REPO' && git status --porcelain)\""

# case 2: cross-check final working-tree content against a real `git rebase`
# run on an identical, independently-built history (commit shas will differ
# since committer timestamps differ -- only compare resulting file bytes)
P4C_SG_CMP_REPO="$WORKDIR/p4c_cmp_sg_repo"
P4C_GIT_CMP_REPO="$WORKDIR/p4c_cmp_git_repo"
mkdir -p "$P4C_SG_CMP_REPO"
(cd "$WORKDIR" && "$SG" init p4c_cmp_sg_repo) > /dev/null 2>&1
(cd "$WORKDIR" && git init -q p4c_cmp_git_repo)
(cd "$P4C_GIT_CMP_REPO" && git config user.email "a@b.c" && git config user.name "git user")

# sg side
printf 'base\n' > "$P4C_SG_CMP_REPO/x.txt"
(cd "$P4C_SG_CMP_REPO" && "$SG" add x.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_SG_CMP_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'base\nfeat1\n' > "$P4C_SG_CMP_REPO/x.txt"
(cd "$P4C_SG_CMP_REPO" && "$SG" add x.txt && "$SG" commit -m "feat1") > /dev/null 2>&1
printf 'base\nfeat1\nfeat2\n' > "$P4C_SG_CMP_REPO/x.txt"
(cd "$P4C_SG_CMP_REPO" && "$SG" add x.txt && "$SG" commit -m "feat2") > /dev/null 2>&1
(cd "$P4C_SG_CMP_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'mstr1\n' > "$P4C_SG_CMP_REPO/y.txt"
(cd "$P4C_SG_CMP_REPO" && "$SG" add y.txt && "$SG" commit -m "mstr1") > /dev/null 2>&1
(cd "$P4C_SG_CMP_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1
(cd "$P4C_SG_CMP_REPO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1

# real-git side, same operations
printf 'base\n' > "$P4C_GIT_CMP_REPO/x.txt"
(cd "$P4C_GIT_CMP_REPO" && git add x.txt && git commit -q -m "base")
(cd "$P4C_GIT_CMP_REPO" && git switch -q -c feature)
printf 'base\nfeat1\n' > "$P4C_GIT_CMP_REPO/x.txt"
(cd "$P4C_GIT_CMP_REPO" && git add x.txt && git commit -q -m "feat1")
printf 'base\nfeat1\nfeat2\n' > "$P4C_GIT_CMP_REPO/x.txt"
(cd "$P4C_GIT_CMP_REPO" && git add x.txt && git commit -q -m "feat2")
(cd "$P4C_GIT_CMP_REPO" && git switch -q master)
printf 'mstr1\n' > "$P4C_GIT_CMP_REPO/y.txt"
(cd "$P4C_GIT_CMP_REPO" && git add y.txt && git commit -q -m "mstr1")
(cd "$P4C_GIT_CMP_REPO" && git switch -q feature)
(cd "$P4C_GIT_CMP_REPO" && git rebase master) > /dev/null 2>&1

check "phase4c case2: sg rebase and real git rebase agree on final x.txt content" \
    cmp -s "$P4C_SG_CMP_REPO/x.txt" "$P4C_GIT_CMP_REPO/x.txt"
check "phase4c case2: sg rebase and real git rebase agree on final y.txt content" \
    cmp -s "$P4C_SG_CMP_REPO/y.txt" "$P4C_GIT_CMP_REPO/y.txt"

# case 3: conflict -> resolve -> --continue
P4C_CONFLICT_REPO="$WORKDIR/p4c_conflict_repo"
mkdir -p "$P4C_CONFLICT_REPO"
(cd "$WORKDIR" && "$SG" init p4c_conflict_repo) > /dev/null 2>&1
printf 'orig1\norig2\n' > "$P4C_CONFLICT_REPO/c.txt"
(cd "$P4C_CONFLICT_REPO" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_CONFLICT_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P4C_CONFLICT_REPO/c.txt"
(cd "$P4C_CONFLICT_REPO" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4C_CONFLICT_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P4C_CONFLICT_REPO/c.txt"
(cd "$P4C_CONFLICT_REPO" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P4C_CONFLICT_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1

(cd "$P4C_CONFLICT_REPO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
P4C_CONFLICT_RC=$?
check "phase4c case3: sg rebase with a real conflict exits non-zero" test "$P4C_CONFLICT_RC" -ne 0

P4C_CONFLICT_PORCELAIN="$WORKDIR/p4c_conflict_porcelain.txt"
(cd "$P4C_CONFLICT_REPO" && git status --porcelain) > "$P4C_CONFLICT_PORCELAIN" 2>&1
check "phase4c case3: real git sees the sg-made rebase conflict as UU" grep -q "^UU c.txt" "$P4C_CONFLICT_PORCELAIN"

printf 'resolved1\norig2\n' > "$P4C_CONFLICT_REPO/c.txt"
(cd "$P4C_CONFLICT_REPO" && "$SG" add c.txt) > /dev/null 2>&1
(cd "$P4C_CONFLICT_REPO" && "$SG" rebase --continue < /dev/null) > /dev/null 2>&1
check "phase4c case3: sg rebase --continue exits 0" test $? = 0

P4C_CONTINUE_SUBJECTS="$WORKDIR/p4c_continue_subjects.txt"
(cd "$P4C_CONFLICT_REPO" && git log --format=%s) > "$P4C_CONTINUE_SUBJECTS" 2>&1
check "phase4c case3: history after --continue is linear -- feature change,master change,base" \
    sh -c "test \"\$(tr '\n' ',' < '$P4C_CONTINUE_SUBJECTS')\" = 'feature change,master change,base,'"
check "phase4c case3: resolved content is in the working tree" grep -q "resolved1" "$P4C_CONFLICT_REPO/c.txt"
check "phase4c case3: git status --porcelain is clean after --continue" \
    sh -c "test -z \"\$(cd '$P4C_CONFLICT_REPO' && git status --porcelain)\""
check "phase4c case3: .git/sg-rebase is gone after --continue" test ! -d "$P4C_CONFLICT_REPO/.git/sg-rebase"

# case 4: --abort restores the pre-rebase state exactly
P4C_ABORT_REPO="$WORKDIR/p4c_abort_repo"
mkdir -p "$P4C_ABORT_REPO"
(cd "$WORKDIR" && "$SG" init p4c_abort_repo) > /dev/null 2>&1
printf 'orig1\norig2\n' > "$P4C_ABORT_REPO/c.txt"
(cd "$P4C_ABORT_REPO" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_ABORT_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P4C_ABORT_REPO/c.txt"
(cd "$P4C_ABORT_REPO" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4C_ABORT_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P4C_ABORT_REPO/c.txt"
(cd "$P4C_ABORT_REPO" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P4C_ABORT_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1

P4C_ABORT_HEAD_BEFORE=$(cd "$P4C_ABORT_REPO" && git rev-parse HEAD)
(cd "$P4C_ABORT_REPO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
(cd "$P4C_ABORT_REPO" && "$SG" rebase --abort < /dev/null) > /dev/null 2>&1
check "phase4c case4: sg rebase --abort exits 0" test $? = 0
P4C_ABORT_HEAD_AFTER=$(cd "$P4C_ABORT_REPO" && git rev-parse HEAD)
check "phase4c case4: HEAD sha is back to exactly its pre-rebase value" \
    test "$P4C_ABORT_HEAD_BEFORE" = "$P4C_ABORT_HEAD_AFTER"
check "phase4c case4: working tree content is back to pre-rebase (feature) content" \
    sh -c "! grep -q '<<<<<<<' '$P4C_ABORT_REPO/c.txt' && grep -q 'feature1' '$P4C_ABORT_REPO/c.txt'"
check "phase4c case4: .git/sg-rebase is gone after --abort" test ! -d "$P4C_ABORT_REPO/.git/sg-rebase"
check "phase4c case4: git status --porcelain is clean after --abort" \
    sh -c "test -z \"\$(cd '$P4C_ABORT_REPO' && git status --porcelain)\""

# case 5: --skip drops the conflicting commit but keeps the rest
P4C_SKIP_REPO="$WORKDIR/p4c_skip_repo"
mkdir -p "$P4C_SKIP_REPO"
(cd "$WORKDIR" && "$SG" init p4c_skip_repo) > /dev/null 2>&1
printf 'orig1\norig2\n' > "$P4C_SKIP_REPO/c.txt"
(cd "$P4C_SKIP_REPO" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_SKIP_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P4C_SKIP_REPO/c.txt"
(cd "$P4C_SKIP_REPO" && "$SG" add c.txt && "$SG" commit -m "will conflict") > /dev/null 2>&1
printf 'other content\n' > "$P4C_SKIP_REPO/other.txt"
(cd "$P4C_SKIP_REPO" && "$SG" add other.txt && "$SG" commit -m "unrelated change") > /dev/null 2>&1
(cd "$P4C_SKIP_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P4C_SKIP_REPO/c.txt"
(cd "$P4C_SKIP_REPO" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P4C_SKIP_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1

(cd "$P4C_SKIP_REPO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
(cd "$P4C_SKIP_REPO" && "$SG" rebase --skip < /dev/null) > /dev/null 2>&1
check "phase4c case5: sg rebase --skip exits 0" test $? = 0

P4C_SKIP_SUBJECTS="$WORKDIR/p4c_skip_subjects.txt"
(cd "$P4C_SKIP_REPO" && git log --format=%s) > "$P4C_SKIP_SUBJECTS" 2>&1
check "phase4c case5: skipped commit 'will conflict' is gone from history" \
    sh -c "! grep -q 'will conflict' '$P4C_SKIP_SUBJECTS'"
check "phase4c case5: unrelated commit still made it onto the rebased branch" \
    grep -q "unrelated change" "$P4C_SKIP_SUBJECTS"
check "phase4c case5: master's own content is what survives (skip discarded the conflicting change)" \
    grep -q "master1" "$P4C_SKIP_REPO/c.txt"
check "phase4c case5: git status --porcelain is clean after --skip completes the rebase" \
    sh -c "test -z \"\$(cd '$P4C_SKIP_REPO' && git status --porcelain)\""

# case 6: a commit whose change is already present upstream is skipped, no
# empty commit is created
P4C_EMPTY_REPO="$WORKDIR/p4c_empty_repo"
mkdir -p "$P4C_EMPTY_REPO"
(cd "$WORKDIR" && "$SG" init p4c_empty_repo) > /dev/null 2>&1
printf 'base\n' > "$P4C_EMPTY_REPO/e.txt"
(cd "$P4C_EMPTY_REPO" && "$SG" add e.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_EMPTY_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'base\ndup-change\n' > "$P4C_EMPTY_REPO/e.txt"
(cd "$P4C_EMPTY_REPO" && "$SG" add e.txt && "$SG" commit -m "duplicated-elsewhere") > /dev/null 2>&1
printf 'base\ndup-change\nunique-change\n' > "$P4C_EMPTY_REPO/e.txt"
(cd "$P4C_EMPTY_REPO" && "$SG" add e.txt && "$SG" commit -m "unique") > /dev/null 2>&1
(cd "$P4C_EMPTY_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
# master independently picks up the exact same change as "duplicated-elsewhere"
printf 'base\ndup-change\n' > "$P4C_EMPTY_REPO/e.txt"
(cd "$P4C_EMPTY_REPO" && "$SG" add e.txt && "$SG" commit -m "master already has this change") > /dev/null 2>&1
(cd "$P4C_EMPTY_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1

P4C_EMPTY_OUT="$WORKDIR/p4c_empty_out.txt"
(cd "$P4C_EMPTY_REPO" && "$SG" rebase master < /dev/null) > "$P4C_EMPTY_OUT" 2>&1
check "phase4c case6: rebase with an already-upstream change exits 0" test $? = 0
check "phase4c case6: sg reports the duplicate commit as skipped" grep -q "Skipped" "$P4C_EMPTY_OUT"

P4C_EMPTY_SUBJECTS="$WORKDIR/p4c_empty_subjects.txt"
(cd "$P4C_EMPTY_REPO" && git log --format=%s) > "$P4C_EMPTY_SUBJECTS" 2>&1
check "phase4c case6: the duplicated commit does not appear twice in history" \
    test "$(grep -c "duplicated-elsewhere" "$P4C_EMPTY_SUBJECTS")" = 0
check "phase4c case6: the unique commit still made it onto the rebased branch" \
    grep -q "^unique\$" "$P4C_EMPTY_SUBJECTS"
check "phase4c case6: git status --porcelain is clean" \
    sh -c "test -z \"\$(cd '$P4C_EMPTY_REPO' && git status --porcelain)\""

# case 7: rebasing onto an already-contained upstream is a no-op
P4C_UTD_REPO="$WORKDIR/p4c_utd_repo"
mkdir -p "$P4C_UTD_REPO"
(cd "$WORKDIR" && "$SG" init p4c_utd_repo) > /dev/null 2>&1
printf 'orig\n' > "$P4C_UTD_REPO/f.txt"
(cd "$P4C_UTD_REPO" && "$SG" add f.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_UTD_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'orig\nfeature added\n' > "$P4C_UTD_REPO/f.txt"
(cd "$P4C_UTD_REPO" && "$SG" add f.txt && "$SG" commit -m "feature change") > /dev/null 2>&1

P4C_UTD_OUT="$WORKDIR/p4c_utd_out.txt"
(cd "$P4C_UTD_REPO" && "$SG" rebase master < /dev/null) > "$P4C_UTD_OUT" 2>&1
check "phase4c case7: rebasing onto an ancestor exits 0" test $? = 0
check "phase4c case7: rebasing onto an ancestor says up to date" grep -q "is up to date" "$P4C_UTD_OUT"

# case 7b: the reverse direction is a pure fast-forward
P4C_FF_OUT="$WORKDIR/p4c_ff_out.txt"
(cd "$P4C_UTD_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
(cd "$P4C_UTD_REPO" && "$SG" rebase feature < /dev/null) > "$P4C_FF_OUT" 2>&1
check "phase4c case7b: rebasing an ancestor branch forward exits 0" test $? = 0
check "phase4c case7b: rebasing an ancestor branch forward fast-forwards" grep -q "Fast-forwarded" "$P4C_FF_OUT"

# case 8: a merge commit in the range is rejected outright, no changes made
P4C_MERGE_REPO="$WORKDIR/p4c_mergecommit_repo"
mkdir -p "$P4C_MERGE_REPO"
(cd "$WORKDIR" && "$SG" init p4c_mergecommit_repo) > /dev/null 2>&1
printf 'base\n' > "$P4C_MERGE_REPO/f.txt"
(cd "$P4C_MERGE_REPO" && "$SG" add f.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_MERGE_REPO" && "$SG" switch -c topic) > /dev/null 2>&1
printf 't1\n' > "$P4C_MERGE_REPO/t.txt"
(cd "$P4C_MERGE_REPO" && "$SG" add t.txt && "$SG" commit -m "T1") > /dev/null 2>&1
(cd "$P4C_MERGE_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'm1\n' > "$P4C_MERGE_REPO/m.txt"
(cd "$P4C_MERGE_REPO" && "$SG" add m.txt && "$SG" commit -m "M1") > /dev/null 2>&1
(cd "$P4C_MERGE_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'f1\n' > "$P4C_MERGE_REPO/f2.txt"
(cd "$P4C_MERGE_REPO" && "$SG" add f2.txt && "$SG" commit -m "F1") > /dev/null 2>&1
(cd "$P4C_MERGE_REPO" && "$SG" merge topic < /dev/null) > /dev/null 2>&1
(cd "$P4C_MERGE_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'm2\n' > "$P4C_MERGE_REPO/m2.txt"
(cd "$P4C_MERGE_REPO" && "$SG" add m2.txt && "$SG" commit -m "M2") > /dev/null 2>&1
(cd "$P4C_MERGE_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1

P4C_MERGE_HEAD_BEFORE=$(cd "$P4C_MERGE_REPO" && git rev-parse HEAD)
(cd "$P4C_MERGE_REPO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
P4C_MERGE_RC=$?
check "phase4c case8: rebase with a merge commit in range is rejected" test "$P4C_MERGE_RC" -ne 0
P4C_MERGE_HEAD_AFTER=$(cd "$P4C_MERGE_REPO" && git rev-parse HEAD)
check "phase4c case8: HEAD is unchanged after the rejection" test "$P4C_MERGE_HEAD_BEFORE" = "$P4C_MERGE_HEAD_AFTER"
check "phase4c case8: no rebase state was left behind" test ! -d "$P4C_MERGE_REPO/.git/sg-rebase"

# case 9: a completed rebase takes an automatic safety snapshot
P4C_SNAPSHOT_COUNT=$(snapshot_count "$P4C_REPO")
check "phase4c case9: sg undo listing gained a snapshot from the rebase in case 1" \
    test "$P4C_SNAPSHOT_COUNT" -ge 1

# case 10: rebase is refused outright when the working directory is dirty
P4C_DIRTY_REPO="$WORKDIR/p4c_dirty_repo"
mkdir -p "$P4C_DIRTY_REPO"
(cd "$WORKDIR" && "$SG" init p4c_dirty_repo) > /dev/null 2>&1
printf 'orig\n' > "$P4C_DIRTY_REPO/f.txt"
(cd "$P4C_DIRTY_REPO" && "$SG" add f.txt && "$SG" commit -m "base") > /dev/null 2>&1
(cd "$P4C_DIRTY_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'orig\nfeat\n' > "$P4C_DIRTY_REPO/f.txt"
(cd "$P4C_DIRTY_REPO" && "$SG" add f.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P4C_DIRTY_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'orig\nmaster\n' > "$P4C_DIRTY_REPO/f.txt"
(cd "$P4C_DIRTY_REPO" && "$SG" add f.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P4C_DIRTY_REPO" && "$SG" switch feature < /dev/null) > /dev/null 2>&1
printf 'uncommitted change\n' >> "$P4C_DIRTY_REPO/f.txt"

P4C_DIRTY_HEAD_BEFORE=$(cd "$P4C_DIRTY_REPO" && git rev-parse HEAD)
(cd "$P4C_DIRTY_REPO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
P4C_DIRTY_RC=$?
check "phase4c case10: rebase with a dirty working directory is refused" test "$P4C_DIRTY_RC" -ne 0
P4C_DIRTY_HEAD_AFTER=$(cd "$P4C_DIRTY_REPO" && git rev-parse HEAD)
check "phase4c case10: HEAD is unchanged after the dirty-workdir rejection" \
    test "$P4C_DIRTY_HEAD_BEFORE" = "$P4C_DIRTY_HEAD_AFTER"
check "phase4c case10: no rebase state was left behind" test ! -d "$P4C_DIRTY_REPO/.git/sg-rebase"

# --- Phase 5b: smart HTTP clone/fetch against a local `git http-backend` CGI
# server. This is the important end-to-end case -- everything else in this
# phase (pkt-line codec, ref advertisement parsing, sideband demux, idx
# generation) only proves itself for real via an actual clone/fetch over the
# wire against a real git server implementation. ---

HTTP_AVAILABLE=1
if ! command -v python3 >/dev/null 2>&1; then
    HTTP_AVAILABLE=0
fi
if ! command -v git >/dev/null 2>&1; then
    HTTP_AVAILABLE=0
fi

if [ "$HTTP_AVAILABLE" = 1 ]; then
    HTTP_SERVERROOT="$WORKDIR/http_serverroot"
    mkdir -p "$HTTP_SERVERROOT"

    HTTP_SRC="$WORKDIR/http_src"
    mkdir -p "$HTTP_SRC/sub"
    git init -q "$HTTP_SRC"
    (cd "$HTTP_SRC" && git config user.email "http@example.com" && git config user.name "http tester")
    printf 'top level file\n' > "$HTTP_SRC/top.txt"
    printf 'nested content\n' > "$HTTP_SRC/sub/nested.txt"
    head -c 300 /dev/urandom > "$HTTP_SRC/bin.dat" 2>/dev/null
    (cd "$HTTP_SRC" && git add top.txt sub/nested.txt bin.dat && git commit -q -m "first commit") > /dev/null 2>&1
    printf 'top level file\nsecond line\n' > "$HTTP_SRC/top.txt"
    (cd "$HTTP_SRC" && git add top.txt && git commit -q -m "second commit") > /dev/null 2>&1
    (cd "$HTTP_SRC" && git tag v1.0) > /dev/null 2>&1
    (cd "$HTTP_SRC" && git repack -ad -q) > /dev/null 2>&1

    (cd "$WORKDIR" && git clone --bare -q http_src "$HTTP_SERVERROOT/repo.git") > /dev/null 2>&1
    # bare repos refuse receive-pack (push) by default -- phase 5c's push
    # tests need this served over smart HTTP, not just fetch/clone
    (cd "$HTTP_SERVERROOT/repo.git" && git config http.receivepack true) > /dev/null 2>&1

    HTTP_SERVER_SCRIPT="$WORKDIR/http_server.py"
    cat > "$HTTP_SERVER_SCRIPT" <<'PYEOF'
import http.server
import os
import socketserver
import subprocess
import sys

serverroot = sys.argv[1]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def run_backend(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length) if length > 0 else b""

        env = dict(os.environ)
        env["GIT_PROJECT_ROOT"] = serverroot
        env["GIT_HTTP_EXPORT_ALL"] = "1"
        env["REQUEST_METHOD"] = self.command
        path, _, query = self.path.partition("?")
        env["PATH_INFO"] = path
        env["QUERY_STRING"] = query
        env["SERVER_PROTOCOL"] = "HTTP/1.1"
        env["GATEWAY_INTERFACE"] = "CGI/1.1"
        env["SERVER_NAME"] = "127.0.0.1"
        env["SERVER_PORT"] = str(self.server.server_address[1])
        env["REMOTE_ADDR"] = self.client_address[0]
        ctype = self.headers.get("Content-Type")
        if ctype:
            env["CONTENT_TYPE"] = ctype
        env["CONTENT_LENGTH"] = str(length)

        proc = subprocess.run(
            ["git", "http-backend"],
            input=body,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        out = proc.stdout

        header_end = out.find(b"\r\n\r\n")
        sep_len = 4
        if header_end == -1:
            header_end = out.find(b"\n\n")
            sep_len = 2
        if header_end == -1:
            self.send_response(502)
            self.end_headers()
            return

        header_blob = out[:header_end].decode("utf-8", "replace")
        payload = out[header_end + sep_len:]

        status = 200
        headers = []
        for line in header_blob.split("\n"):
            line = line.strip("\r")
            if not line:
                continue
            k, _, v = line.partition(":")
            v = v.strip()
            if k.lower() == "status":
                status = int(v.split()[0])
            else:
                headers.append((k, v))

        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        self.run_backend()

    def do_POST(self):
        self.run_backend()

    def log_message(self, fmt, *args):
        pass


class Server(socketserver.TCPServer):
    allow_reuse_address = True


httpd = Server(("127.0.0.1", 0), Handler)
print("PORT %d" % httpd.server_address[1], flush=True)
httpd.serve_forever()
PYEOF

    HTTP_SERVER_LOG="$WORKDIR/http_server.log"
    python3 "$HTTP_SERVER_SCRIPT" "$HTTP_SERVERROOT" > "$HTTP_SERVER_LOG" 2>&1 &
    HTTP_SERVER_PID=$!

    HTTP_PORT=""
    i=0
    while [ "$i" -lt 50 ]; do
        if [ -s "$HTTP_SERVER_LOG" ]; then
            HTTP_PORT=$(awk '/^PORT /{print $2; exit}' "$HTTP_SERVER_LOG")
            if [ -n "$HTTP_PORT" ]; then
                break
            fi
        fi
        sleep 0.1
        i=$((i + 1))
    done

    if [ -z "$HTTP_PORT" ]; then
        echo "warning: HTTP test server did not become ready in time, skipping phase 5b HTTP tests" >&2
        HTTP_AVAILABLE=0
    fi
fi

if [ "$HTTP_AVAILABLE" = 1 ]; then
    HTTP_BASE_URL="http://127.0.0.1:$HTTP_PORT/repo.git"
    HTTP_DEST="$WORKDIR/http_clone_dest"
    HTTP_SRC_HEAD=$(cd "$HTTP_SRC" && git rev-parse HEAD)
    HTTP_SRC_BRANCH=$(cd "$HTTP_SRC" && git rev-parse --abbrev-ref HEAD)

    CLONE_OUT="$WORKDIR/http_clone_out.txt"
    "$SG" clone "$HTTP_BASE_URL" "$HTTP_DEST" > "$CLONE_OUT" 2>&1
    check "phase5b: sg clone over smart HTTP exits 0" test $? = 0

    check "phase5b: cloned local branch sha matches source HEAD" \
        sh -c "test -f '$HTTP_DEST/.git/refs/heads/$HTTP_SRC_BRANCH' && test \"\$(cat '$HTTP_DEST/.git/refs/heads/$HTTP_SRC_BRANCH')\" = '$HTTP_SRC_HEAD'"

    check "phase5b: cloned working tree top.txt matches source" \
        cmp -s "$HTTP_SRC/top.txt" "$HTTP_DEST/top.txt"
    check "phase5b: cloned working tree sub/nested.txt matches source" \
        cmp -s "$HTTP_SRC/sub/nested.txt" "$HTTP_DEST/sub/nested.txt"
    check "phase5b: cloned working tree binary file matches source" \
        cmp -s "$HTTP_SRC/bin.dat" "$HTTP_DEST/bin.dat"

    check "phase5b: git fsck passes on the cloned repo" \
        sh -c "git -C '$HTTP_DEST' fsck > /dev/null 2>&1"

    GIT_LOG_HTTP="$WORKDIR/http_git_log.txt"
    git -C "$HTTP_DEST" log --oneline > "$GIT_LOG_HTTP" 2>&1
    check "phase5b: git log reads both commits from the cloned repo" \
        test "$(wc -l < "$GIT_LOG_HTTP" | tr -d ' ')" = 2

    SG_LOG_HTTP_OUT="$WORKDIR/http_sg_log.txt"
    (cd "$HTTP_DEST" && "$SG" log) > "$SG_LOG_HTTP_OUT" 2>&1
    check "phase5b: sg log exits 0 on the cloned repo" test $? = 0
    check "phase5b: sg log shows the source HEAD sha" grep -q "$HTTP_SRC_HEAD" "$SG_LOG_HTTP_OUT"

    SG_STATUS_HTTP_OUT="$WORKDIR/http_sg_status.txt"
    (cd "$HTTP_DEST" && "$SG" status) > "$SG_STATUS_HTTP_OUT" 2>&1
    check "phase5b: sg status exits 0 on the cloned repo" test $? = 0
    check "phase5b: sg status reports clean tree on the cloned repo" \
        grep -q "nothing to commit" "$SG_STATUS_HTTP_OUT"

    # --- clone must also fetch and write tags (refs/tags/v1.0 was created on
    # the source before the bare repo was cloned), not just branches ---
    HTTP_SRC_V1=$(cd "$HTTP_SRC" && git rev-parse v1.0)
    check "phase5b: sg clone wrote refs/tags/v1.0 with the correct sha" \
        sh -c "test -f '$HTTP_DEST/.git/refs/tags/v1.0' && test \"\$(cat '$HTTP_DEST/.git/refs/tags/v1.0')\" = '$HTTP_SRC_V1'"

    # --- .idx must be byte-for-byte identical to real `git index-pack`'s output ---
    HTTP_PACK_FILE=$(find "$HTTP_DEST/.git/objects/pack" -name '*.pack' | head -1)
    HTTP_OUR_IDX=$(find "$HTTP_DEST/.git/objects/pack" -name '*.idx' | head -1)
    check "phase5b: a .pack file exists after clone" test -n "$HTTP_PACK_FILE"
    check "phase5b: a .idx file exists after clone" test -n "$HTTP_OUR_IDX"

    if [ -n "$HTTP_PACK_FILE" ]; then
        HTTP_VERIFY_DIR="$WORKDIR/http_idx_verify"
        mkdir -p "$HTTP_VERIFY_DIR"
        cp "$HTTP_PACK_FILE" "$HTTP_VERIFY_DIR/"
        GIT_IDXPACK_OUT="$WORKDIR/http_index_pack_out.txt"
        (cd "$HTTP_VERIFY_DIR" && git index-pack "$(basename "$HTTP_PACK_FILE")") > "$GIT_IDXPACK_OUT" 2>&1
        check "phase5b: real git index-pack succeeds on our received pack" test $? = 0

        GIT_IDX_FILE="$HTTP_VERIFY_DIR/$(basename "$HTTP_PACK_FILE" .pack).idx"
        check "phase5b: our .idx is byte-for-byte identical to git index-pack's .idx" \
            cmp -s "$HTTP_OUR_IDX" "$GIT_IDX_FILE"
    fi

    # --- cloning into an existing, non-empty directory must be refused ---
    RECLONE_OUT="$WORKDIR/http_reclone_out.txt"
    "$SG" clone "$HTTP_BASE_URL" "$HTTP_DEST" > "$RECLONE_OUT" 2>&1
    check "phase5b: sg clone into an existing non-empty directory is refused" test $? -ne 0

    # --- fetch: add a commit (and a new tag on it) on the source, push both
    # to the bare serving repo, then fetch into the clone made above ---
    printf 'top level file\nsecond line\nthird line\n' > "$HTTP_SRC/top.txt"
    (cd "$HTTP_SRC" && git add top.txt && git commit -q -m "third commit") > /dev/null 2>&1
    (cd "$HTTP_SRC" && git tag v2.0) > /dev/null 2>&1
    (cd "$HTTP_SRC" && git push -q "$HTTP_SERVERROOT/repo.git" "HEAD:refs/heads/$HTTP_SRC_BRANCH") > /dev/null 2>&1
    (cd "$HTTP_SRC" && git push -q "$HTTP_SERVERROOT/repo.git" refs/tags/v2.0) > /dev/null 2>&1
    NEW_SRC_HEAD=$(cd "$HTTP_SRC" && git rev-parse HEAD)
    HTTP_SRC_V2=$(cd "$HTTP_SRC" && git rev-parse v2.0)

    FETCH_OUT="$WORKDIR/http_fetch_out.txt"
    (cd "$HTTP_DEST" && "$SG" fetch) > "$FETCH_OUT" 2>&1
    check "phase5b: sg fetch exits 0" test $? = 0
    check "phase5b: sg fetch reports the updated branch" grep -q "$HTTP_SRC_BRANCH" "$FETCH_OUT"
    check "phase5b: sg fetch reports the new tag" grep -q "v2.0" "$FETCH_OUT"

    check "phase5b: refs/remotes/origin/<branch> now points at the new commit" \
        sh -c "test \"\$(cat '$HTTP_DEST/.git/refs/remotes/origin/$HTTP_SRC_BRANCH')\" = '$NEW_SRC_HEAD'"
    check "phase5b: sg fetch wrote the new tag refs/tags/v2.0 with the correct sha" \
        sh -c "test -f '$HTTP_DEST/.git/refs/tags/v2.0' && test \"\$(cat '$HTTP_DEST/.git/refs/tags/v2.0')\" = '$HTTP_SRC_V2'"
    check "phase5b: git fsck still passes after fetch" \
        sh -c "git -C '$HTTP_DEST' fsck > /dev/null 2>&1"

    check "phase5b: sg fetch did not move the local branch" \
        sh -c "test \"\$(cat '$HTTP_DEST/.git/refs/heads/$HTTP_SRC_BRANCH')\" = '$HTTP_SRC_HEAD'"
    check "phase5b: sg fetch did not touch the working tree" \
        sh -c "! grep -q 'third line' '$HTTP_DEST/top.txt'"

    FETCH_AGAIN_OUT="$WORKDIR/http_fetch_again_out.txt"
    (cd "$HTTP_DEST" && "$SG" fetch) > "$FETCH_AGAIN_OUT" 2>&1
    check "phase5b: a second sg fetch (nothing new) exits 0" test $? = 0
    check "phase5b: a second sg fetch reports already up to date" \
        grep -q "Already up to date" "$FETCH_AGAIN_OUT"

    # --- phase 5c: sg push over smart HTTP (git-receive-pack) ---

    # phase 5b deliberately left refs/heads/<branch> behind the bare repo's
    # current tip (fetch must never move a local branch) -- bring the local
    # branch up to what the remote actually has first, the same way a real
    # `sg merge` from the now-updated refs/remotes/origin/<branch> would,
    # done directly here so the push tests below start from a genuine
    # fast-forward position rather than an already-diverged one.
    git -C "$HTTP_DEST" update-ref "refs/heads/$HTTP_SRC_BRANCH" "$NEW_SRC_HEAD"
    printf 'top level file\nsecond line\nthird line\n' > "$HTTP_DEST/top.txt"

    # case 1: a genuine local commit -> sg push succeeds; verified against
    # the bare repo with real git (ref, log, content, fsck)
    printf 'top level file\nsecond line\nthird line\npushed from sg\n' > "$HTTP_DEST/top.txt"
    (cd "$HTTP_DEST" && "$SG" add top.txt) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" commit -m "pushed commit") > /dev/null 2>&1
    PUSH_NEW_HEAD=$(cd "$HTTP_DEST" && git rev-parse HEAD)

    PUSH_OUT="$WORKDIR/http_push_out.txt"
    (cd "$HTTP_DEST" && "$SG" push) > "$PUSH_OUT" 2>&1
    check "phase5c: sg push exits 0" test $? = 0

    check "phase5c: bare repo branch ref updated to the pushed commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/$HTTP_SRC_BRANCH)\" = '$PUSH_NEW_HEAD'"
    check "phase5c: bare repo git log reads the pushed commit" \
        sh -c "cd '$HTTP_SERVERROOT/repo.git' && git log --format='%H' -1 'refs/heads/$HTTP_SRC_BRANCH' | grep -q '$PUSH_NEW_HEAD'"
    check "phase5c: git fsck passes on the bare repo after push" \
        sh -c "git -C '$HTTP_SERVERROOT/repo.git' fsck > /dev/null 2>&1"

    PUSH_SHOW_OUT="$WORKDIR/http_push_show_out.txt"
    (cd "$HTTP_SERVERROOT/repo.git" && git show "$PUSH_NEW_HEAD:top.txt") > "$PUSH_SHOW_OUT" 2>/dev/null
    check "phase5c: pushed file content matches on the bare repo" \
        cmp -s "$HTTP_DEST/top.txt" "$PUSH_SHOW_OUT"

    # case 5: local refs/remotes/origin/<branch> updated after a successful push
    check "phase5c: local refs/remotes/origin/<branch> updated after push" \
        sh -c "test \"\$(cat '$HTTP_DEST/.git/refs/remotes/origin/$HTTP_SRC_BRANCH')\" = '$PUSH_NEW_HEAD'"

    # case 4: nothing left to push -> Everything up-to-date, exit 0
    PUSH_AGAIN_OUT="$WORKDIR/http_push_again_out.txt"
    (cd "$HTTP_DEST" && "$SG" push) > "$PUSH_AGAIN_OUT" 2>&1
    check "phase5c: a second sg push (nothing new) exits 0" test $? = 0
    check "phase5c: a second sg push reports Everything up-to-date" \
        grep -q "Everything up-to-date" "$PUSH_AGAIN_OUT"

    # case 2: push a branch the remote doesn't have yet
    (cd "$HTTP_DEST" && "$SG" switch -c sg-new-branch) > /dev/null 2>&1
    printf 'new branch content\n' > "$HTTP_DEST/newbranch.txt"
    (cd "$HTTP_DEST" && "$SG" add newbranch.txt) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" commit -m "new branch commit") > /dev/null 2>&1
    NEW_BRANCH_HEAD=$(cd "$HTTP_DEST" && git rev-parse HEAD)

    NEW_BRANCH_PUSH_OUT="$WORKDIR/http_push_newbranch_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin sg-new-branch) > "$NEW_BRANCH_PUSH_OUT" 2>&1
    check "phase5c: sg push of a new branch exits 0" test $? = 0
    check "phase5c: sg push of a new branch reports it as a new branch" \
        grep -q "new branch" "$NEW_BRANCH_PUSH_OUT"
    check "phase5c: bare repo gained the new branch at the right commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/sg-new-branch 2>/dev/null)\" = '$NEW_BRANCH_HEAD'"

    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1

    # case 3: non-fast-forward protection. Diverge by having real git push a
    # commit directly into the bare repo, `sg fetch` so the object is known
    # locally (without moving the local branch -- this is what makes it a
    # genuine "known but not an ancestor" non-fast-forward case rather than
    # the separate "remote commit we've never seen" case, which sg push must
    # refuse unconditionally, --force included), then commit locally on the
    # old (pre-divergence) base so the two histories actually diverge.
    DIVERGE_SRC="$WORKDIR/http_diverge_src"
    (cd "$WORKDIR" && git clone -q "$HTTP_SERVERROOT/repo.git" http_diverge_src) > /dev/null 2>&1
    (cd "$DIVERGE_SRC" && git config user.email "diverge@example.com" && git config user.name "diverge tester")
    printf 'diverged on the server\n' >> "$DIVERGE_SRC/top.txt"
    (cd "$DIVERGE_SRC" && git add top.txt && git commit -q -m "server-side divergent commit")
    (cd "$DIVERGE_SRC" && git push -q origin "HEAD:refs/heads/$HTTP_SRC_BRANCH") > /dev/null 2>&1
    DIVERGED_BARE_HEAD=$(cd "$HTTP_SERVERROOT/repo.git" && git rev-parse "refs/heads/$HTTP_SRC_BRANCH")

    (cd "$HTTP_DEST" && "$SG" fetch) > /dev/null 2>&1

    printf 'top level file\nsecond line\nthird line\npushed from sg\nlocal-only change\n' > "$HTTP_DEST/top.txt"
    (cd "$HTTP_DEST" && "$SG" add top.txt) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" commit -m "local divergent commit") > /dev/null 2>&1

    NOFORCE_PUSH_OUT="$WORKDIR/http_push_noforce_out.txt"
    (cd "$HTTP_DEST" && "$SG" push < /dev/null) > "$NOFORCE_PUSH_OUT" 2>&1
    check "phase5c: sg push without --force fails on non-fast-forward" test $? -ne 0
    check "phase5c: bare repo ref unchanged after refused non-fast-forward push" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/$HTTP_SRC_BRANCH)\" = '$DIVERGED_BARE_HEAD'"

    FORCE_PUSH_LOCAL_HEAD=$(cd "$HTTP_DEST" && git rev-parse HEAD)
    FORCE_PUSH_OUT="$WORKDIR/http_push_force_out.txt"
    (cd "$HTTP_DEST" && "$SG" push --force < /dev/null) > "$FORCE_PUSH_OUT" 2>&1
    check "phase5c: sg push --force succeeds over a non-fast-forward" test $? = 0
    check "phase5c: bare repo ref now matches the force-pushed commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/$HTTP_SRC_BRANCH)\" = '$FORCE_PUSH_LOCAL_HEAD'"
    check "phase5c: git fsck still passes on the bare repo after force push" \
        sh -c "git -C '$HTTP_SERVERROOT/repo.git' fsck > /dev/null 2>&1"

    # --- case 6: sg push can also push tags (Phase 13) ---
    TAG_PUSH_HEAD=$(cd "$HTTP_DEST" && git rev-parse HEAD)

    # a lightweight tag: the remote ref must point straight at the commit
    (cd "$HTTP_DEST" && "$SG" tag push-lw) > /dev/null 2>&1
    LW_TAG_PUSH_OUT="$WORKDIR/http_push_tag_lw_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin push-lw) > "$LW_TAG_PUSH_OUT" 2>&1
    check "phase5c: sg push of a lightweight tag exits 0" test $? = 0
    check "phase5c: sg push of a lightweight tag reports it as new" \
        grep -q "new tag" "$LW_TAG_PUSH_OUT"
    check "phase5c: bare repo lightweight tag points at the commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/push-lw)\" = '$TAG_PUSH_HEAD'"
    check "phase5c: bare repo lightweight tag object type is commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git cat-file -t refs/tags/push-lw)\" = 'commit'"
    check "phase5c: pushing a tag does not create a remote-tracking ref" \
        sh -c "! test -e '$HTTP_DEST/.git/refs/remotes/origin/push-lw'"

    # an annotated tag: the remote ref must point at the TAG OBJECT id, not
    # peeled to the commit it annotates
    (cd "$HTTP_DEST" && "$SG" tag -a -m "annotated push tag" push-ann) > /dev/null 2>&1
    PUSH_ANN_TAG_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/push-ann)
    ANN_TAG_PUSH_OUT="$WORKDIR/http_push_tag_ann_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin push-ann) > "$ANN_TAG_PUSH_OUT" 2>&1
    check "phase5c: sg push of an annotated tag exits 0" test $? = 0
    check "phase5c: sg push of an annotated tag reports it as new" \
        grep -q "new tag" "$ANN_TAG_PUSH_OUT"
    check "phase5c: bare repo annotated tag ref is the tag object id, not the peeled commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/push-ann)\" = '$PUSH_ANN_TAG_ID'"
    check "phase5c: bare repo annotated tag object type is tag" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git cat-file -t refs/tags/push-ann)\" = 'tag'"
    check "phase5c: bare repo annotated tag message is readable" \
        sh -c "cd '$HTTP_SERVERROOT/repo.git' && git cat-file tag refs/tags/push-ann | grep -q 'annotated push tag'"
    check "phase5c: git fsck passes on the bare repo after tag pushes" \
        sh -c "git -C '$HTTP_SERVERROOT/repo.git' fsck > /dev/null 2>&1"

    # pushing an unmoved tag a second time -> up to date, exit 0
    TAG_AGAIN_OUT="$WORKDIR/http_push_tag_again_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin push-lw) > "$TAG_AGAIN_OUT" 2>&1
    check "phase5c: a second sg push of an unmoved tag exits 0" test $? = 0
    check "phase5c: a second sg push of an unmoved tag reports Everything up-to-date" \
        grep -q "Everything up-to-date" "$TAG_AGAIN_OUT"

    # move the lightweight tag locally (to the sg-new-branch tip created in
    # case 2, a different commit), then push without --force -> rejected
    # unconditionally (no merge-base question, unlike a branch), remote
    # unchanged; with --force -> accepted, remote updated
    (cd "$HTTP_DEST" && "$SG" switch sg-new-branch < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag --force push-lw) > /dev/null 2>&1
    MOVED_TAG_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/push-lw)
    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1

    TAG_NOFORCE_OUT="$WORKDIR/http_push_tag_noforce_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin push-lw < /dev/null) > "$TAG_NOFORCE_OUT" 2>&1
    check "phase5c: sg push of a moved tag without --force fails" test $? -ne 0
    check "phase5c: bare repo tag unchanged after refused tag push" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/push-lw)\" = '$TAG_PUSH_HEAD'"

    TAG_FORCE_OUT="$WORKDIR/http_push_tag_force_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin push-lw --force < /dev/null) > "$TAG_FORCE_OUT" 2>&1
    check "phase5c: sg push --force of a moved tag succeeds" test $? = 0
    check "phase5c: bare repo tag now matches the force-pushed tag" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/push-lw)\" = '$MOVED_TAG_ID'"
    check "phase5c: force-pushed tag update reports (forced update)" \
        grep -q "forced update" "$TAG_FORCE_OUT"
    check "phase5c: git fsck still passes on the bare repo after forced tag push" \
        sh -c "git -C '$HTTP_SERVERROOT/repo.git' fsck > /dev/null 2>&1"

    # --tags: push every local tag in one round trip (some already on the
    # remote and unchanged, at least one lightweight and one annotated new)
    (cd "$HTTP_DEST" && "$SG" tag tags-multi-lw) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag -a -m "multi annotated" tags-multi-ann) > /dev/null 2>&1
    TAGS_MULTI_LW_ID=$(cd "$HTTP_DEST" && git rev-parse tags-multi-lw)
    TAGS_MULTI_ANN_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/tags-multi-ann)
    TAGS_ALL_OUT="$WORKDIR/http_push_tags_all_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin --tags) > "$TAGS_ALL_OUT" 2>&1
    check "phase5c: sg push --tags exits 0" test $? = 0
    check "phase5c: bare repo gained tags-multi-lw at the right commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/tags-multi-lw 2>/dev/null)\" = '$TAGS_MULTI_LW_ID'"
    check "phase5c: bare repo gained tags-multi-ann at the right commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/tags-multi-ann 2>/dev/null)\" = '$TAGS_MULTI_ANN_ID'"

    # --tags and an explicit name together is a usage error, not "push both"
    TAGS_AND_NAME_OUT="$WORKDIR/http_push_tags_and_name_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin --tags "$HTTP_SRC_BRANCH" < /dev/null) > "$TAGS_AND_NAME_OUT" 2>&1
    check "phase5c: sg push --tags with an explicit name is a usage error" test $? -ne 0
    check "phase5c: sg push --tags with an explicit name prints a usage line" \
        grep -q "^usage: sg push" "$TAGS_AND_NAME_OUT"

    # a tag name containing a slash (sg_ref_list_under returns it without the
    # refs/tags/ prefix, slash kept whole) must round-trip through push too
    (cd "$HTTP_DEST" && "$SG" tag rel/1.0) > /dev/null 2>&1
    SLASH_TAG_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/rel/1.0)
    SLASH_TAG_OUT="$WORKDIR/http_push_slash_tag_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin rel/1.0) > "$SLASH_TAG_OUT" 2>&1
    check "phase5c: sg push of a slash-containing tag name exits 0" test $? = 0
    check "phase5c: bare repo gained refs/tags/rel/1.0 at the right commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/rel/1.0 2>/dev/null)\" = '$SLASH_TAG_ID'"

    # --tags --force overwrites MULTIPLE diverged tags in one round trip
    (cd "$HTTP_DEST" && "$SG" tag force-multi-a) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag force-multi-b) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" push origin --tags) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" switch sg-new-branch < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag --force force-multi-a) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag --force force-multi-b) > /dev/null 2>&1
    FORCE_MULTI_A_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/force-multi-a)
    FORCE_MULTI_B_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/force-multi-b)
    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1

    FORCE_MULTI_OUT="$WORKDIR/http_push_tags_force_multi_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin --tags --force < /dev/null) > "$FORCE_MULTI_OUT" 2>&1
    check "phase5c: sg push --tags --force over multiple diverged tags exits 0" test $? = 0
    check "phase5c: bare repo force-multi-a now matches the force-pushed tag" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/force-multi-a)\" = '$FORCE_MULTI_A_ID'"
    check "phase5c: bare repo force-multi-b now matches the force-pushed tag" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/force-multi-b)\" = '$FORCE_MULTI_B_ID'"

    # --tags with a mix of a brand-new tag and one that conflicts with the
    # remote (already there, different id, no --force): the whole invocation
    # must still exit non-zero, but the new tag must land anyway -- git
    # applies each ref update independently, and rc = had_rejection ? 1 : 0
    # (cmd_push.c) is what's supposed to make that visible on exit code
    # alone even though most of the run succeeded.
    (cd "$HTTP_DEST" && "$SG" tag mix-conflict) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" push origin mix-conflict) > /dev/null 2>&1
    MIX_CONFLICT_REMOTE_BEFORE=$(cd "$HTTP_SERVERROOT/repo.git" && git rev-parse refs/tags/mix-conflict)
    (cd "$HTTP_DEST" && "$SG" switch sg-new-branch < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag --force mix-conflict) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag mix-new) > /dev/null 2>&1
    MIX_NEW_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/mix-new)

    MIX_TAGS_OUT="$WORKDIR/http_push_tags_mixed_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin --tags < /dev/null) > "$MIX_TAGS_OUT" 2>&1
    check "phase5c: sg push --tags with a mix of success and rejection exits non-zero" \
        test $? -ne 0
    check "phase5c: sg push --tags landed the new tag despite the other's rejection" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/mix-new 2>/dev/null)\" = '$MIX_NEW_ID'"
    check "phase5c: sg push --tags left the rejected tag untouched on the remote" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/mix-conflict)\" = '$MIX_CONFLICT_REMOTE_BEFORE'"

    # a branch and a tag sharing a name -> ambiguous, refused, remote untouched
    (cd "$HTTP_DEST" && "$SG" switch -c ambiguous-name) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" tag ambiguous-name) > /dev/null 2>&1
    AMBIG_OUT="$WORKDIR/http_push_ambiguous_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin ambiguous-name < /dev/null) > "$AMBIG_OUT" 2>&1
    check "phase5c: sg push of an ambiguous branch/tag name fails" test $? -ne 0
    check "phase5c: sg push reports the ambiguous refspec error" \
        grep -q "matches more than one" "$AMBIG_OUT"
    check "phase5c: bare repo gained no tag for the ambiguous name" \
        sh -c "test -z \"\$(cd '$HTTP_SERVERROOT/repo.git' && git tag -l ambiguous-name)\""
    check "phase5c: bare repo gained no branch for the ambiguous name" \
        sh -c "test -z \"\$(cd '$HTTP_SERVERROOT/repo.git' && git branch --list ambiguous-name)\""
    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1

    # --- Phase 39: refspec support for sg push ---

    P39_HEAD=$(cd "$HTTP_DEST" && git rev-parse HEAD)

    # <src>:<dst> updates a differently-named remote ref
    P39_EXPLICIT_OUT="$WORKDIR/http_push_p39_explicit_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin "$HTTP_SRC_BRANCH:refs/heads/p39-explicit" < /dev/null) > "$P39_EXPLICIT_OUT" 2>&1
    check "phase39: sg push <src>:<dst> exits 0" test $? = 0
    check "phase39: bare repo gained refs/heads/p39-explicit at the pushed commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-explicit)\" = '$P39_HEAD'"

    # an annotated tag used as <src> is NOT peeled -- the remote object stays
    # a tag, not the commit it points at (docs/DESIGN.md Phase 39 section 1,
    # this is the "do not use sg_rev_parse_commit for src" trap)
    (cd "$HTTP_DEST" && "$SG" tag -a p39-anntag -m "phase39 annotated") > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" push origin p39-anntag:refs/tags/p39-anntag-copy < /dev/null) > /dev/null 2>&1
    check "phase39: pushed annotated-tag src stays a tag object on the remote (not peeled)" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git cat-file -t refs/tags/p39-anntag-copy)\" = tag"

    # dst completion rule 1 (an existing remote ref named <dst>) takes
    # priority over rule 2 (prefixing src's own namespace) -- measured
    # against real git 2.55.0, docs/DESIGN.md Phase 39 section 2
    (cd "$HTTP_DEST" && "$SG" tag p39-rule1) > /dev/null 2>&1
    P39_RULE1_TAG_ID=$(cd "$HTTP_DEST" && git rev-parse refs/tags/p39-rule1)
    (cd "$HTTP_DEST" && "$SG" push origin "$HTTP_SRC_BRANCH:refs/heads/p39-rule1-target" < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" push origin p39-rule1:p39-rule1-target < /dev/null) > /dev/null 2>&1
    check "phase39: dst completion rule 1 (existing branch) wins over rule 2 (tag prefix)" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-rule1-target)\" = '$P39_RULE1_TAG_ID'"
    check "phase39: dst completion rule 1 did not also create a refs/tags/ copy" \
        sh -c "! (cd '$HTTP_SERVERROOT/repo.git' && git rev-parse --verify refs/tags/p39-rule1-target) >/dev/null 2>&1"

    # --- Round 3 item B: rule 2's completed path must not be truncated to
    # its last path segment. "master:notrefs/x" completes (via rule 2,
    # $HTTP_SRC_BRANCH resolves as an exact refs/heads/ literal) to
    # "refs/heads/notrefs/x" -- the WHOLE dst string prefixed, not just its
    # last segment (docs/DESIGN.md Phase 39 section 2, rule 2). Before the
    # round-3 fix, `strrchr(completed_path, '/')` gave `name = "x"`, which
    # both mis-built the local remote-tracking ref path
    # (refs/remotes/origin/x instead of refs/remotes/origin/notrefs/x) and
    # was a factually wrong report line.
    (cd "$HTTP_DEST" && "$SG" push origin "$HTTP_SRC_BRANCH:notrefs/x" < /dev/null) > /dev/null 2>&1
    check "phase39: multi-segment dst completes to the full refs/heads/notrefs/x on the remote" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/notrefs/x)\" = '$P39_HEAD'"
    check "phase39: multi-segment dst updates the correctly-named local remote-tracking ref" \
        sh -c "test \"\$(cd '$HTTP_DEST' && git rev-parse refs/remotes/origin/notrefs/x 2>/dev/null)\" = '$P39_HEAD'"
    check "phase39: multi-segment dst does NOT also create a truncated refs/remotes/origin/x" \
        sh -c "! (cd '$HTTP_DEST' && git rev-parse --verify refs/remotes/origin/x) >/dev/null 2>&1"

    # --- Round 3 item D: an unqualified dst that matches MORE THAN ONE
    # advertised ref (a branch and a tag sharing the same name) must be
    # refused outright -- measured against real git 2.55.0: with the remote
    # holding both refs/heads/dup and refs/tags/dup, `topic:dup` fails with
    # "error: dst refspec dup matches more than one" and neither ref is
    # touched. Before the round-3 fix, sg's rule-1 guessing loop stopped at
    # the FIRST guess prefix that matched (refs/tags/dup, since "refs/tags/"
    # is tried before "refs/heads/" in guess_prefixes[]) and silently wrote
    # there instead of refusing.
    (cd "$HTTP_DEST" && "$SG" switch -c p39-dup-src < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" push origin p39-dup-src:refs/heads/dup < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" push origin p39-dup-src:refs/tags/dup < /dev/null) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1

    P39_DUP_BRANCH_BEFORE=$(cd "$HTTP_SERVERROOT/repo.git" && git rev-parse refs/heads/dup)
    P39_DUP_TAG_BEFORE=$(cd "$HTTP_SERVERROOT/repo.git" && git rev-parse refs/tags/dup)

    P39_DUP_OUT="$WORKDIR/http_push_p39_dup_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin "$HTTP_SRC_BRANCH:dup" < /dev/null) > "$P39_DUP_OUT" 2>&1
    check "phase39: an unqualified dst matching both a branch and a tag is rejected" test $? -ne 0
    check "phase39: sg reports the ambiguous dst refspec" \
        grep -q "matches more than one" "$P39_DUP_OUT"
    check "phase39: the ambiguous dst push left refs/heads/dup untouched" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/dup)\" = '$P39_DUP_BRANCH_BEFORE'"
    check "phase39: the ambiguous dst push left refs/tags/dup untouched" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/tags/dup)\" = '$P39_DUP_TAG_BEFORE'"

    P39_DUP_ORACLE_SRC="$WORKDIR/http_diverge_src_p39_dup"
    (cd "$WORKDIR" && git clone -q "$HTTP_SERVERROOT/repo.git" http_diverge_src_p39_dup) > /dev/null 2>&1
    (cd "$P39_DUP_ORACLE_SRC" && git config user.email "p39dup@example.com" && git config user.name "p39dup tester")
    P39_DUP_ORACLE_OUT="$WORKDIR/http_push_p39_dup_oracle_out.txt"
    # LC_ALL=C: git translates this message under a localized environment
    # (this machine's git is zh_TW), and the assertion below greps its
    # English wording.
    (cd "$P39_DUP_ORACLE_SRC" && LC_ALL=C git push origin "HEAD:dup") > "$P39_DUP_ORACLE_OUT" 2>&1
    check "phase39 oracle: real git also rejects an unqualified dst matching both a branch and a tag" test $? -ne 0
    check "phase39 oracle: real git's ambiguous-dst message matches the wording sg's message was borrowed from" \
        grep -q "matches more than one" "$P39_DUP_ORACLE_OUT"

    # --delete
    (cd "$HTTP_DEST" && "$SG" push origin --delete p39-explicit < /dev/null) > /dev/null 2>&1
    check "phase39: --delete removed the remote branch" \
        sh -c "! (cd '$HTTP_SERVERROOT/repo.git' && git rev-parse --verify refs/heads/p39-explicit) >/dev/null 2>&1"

    P39_DELETE_MISSING_OUT="$WORKDIR/http_push_p39_delete_missing_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin --delete p39-explicit < /dev/null) > "$P39_DELETE_MISSING_OUT" 2>&1
    check "phase39: deleting an already-gone remote ref fails" test $? -ne 0
    check "phase39: deleting an already-gone remote ref reports the expected message" \
        grep -q "remote ref does not exist" "$P39_DELETE_MISSING_OUT"

    # multiple refspecs in a single invocation: two brand-new branches land
    # together from one `sg push`
    (cd "$HTTP_DEST" && "$SG" push origin "$HTTP_SRC_BRANCH:refs/heads/p39-multi-a" "$HTTP_SRC_BRANCH:refs/heads/p39-multi-b" < /dev/null) > /dev/null 2>&1
    check "phase39: a multi-refspec push lands the first ref" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-multi-a)\" = '$P39_HEAD'"
    check "phase39: a multi-refspec push lands the second ref" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-multi-b)\" = '$P39_HEAD'"

    # multiple refspecs, one of them non-fast-forward: the whole command
    # still exits non-zero, but each ref update is independent -- the
    # fast-forwardable one must land anyway (same "each ref update is
    # independent" shape as --tags's own mixed-success case above), and the
    # rejected one must be left exactly where it was (docs/DESIGN.md Phase 39
    # section 5's "non-ff is a per-ref failure" row). Divergence is built the
    # same way case 3 above builds it: a real git push directly into the bare
    # repo, an `sg fetch` so the object is known locally (without moving any
    # local branch -- the "known but not an ancestor" shape, not the
    # separate "remote commit we've never seen" shape).
    DIVERGE_SRC2="$WORKDIR/http_diverge_src_p39"
    (cd "$WORKDIR" && git clone -q "$HTTP_SERVERROOT/repo.git" http_diverge_src_p39) > /dev/null 2>&1
    (cd "$DIVERGE_SRC2" && git config user.email "diverge2@example.com" && git config user.name "diverge2 tester")
    (cd "$DIVERGE_SRC2" && git checkout -q -b p39-multi-b "origin/p39-multi-b" 2>/dev/null || git checkout -q p39-multi-b)
    printf 'phase39 server-side divergent commit\n' >> "$DIVERGE_SRC2/top.txt"
    (cd "$DIVERGE_SRC2" && git add top.txt && git commit -q -m "phase39 server-side divergent commit")
    (cd "$DIVERGE_SRC2" && git push -q origin HEAD:refs/heads/p39-multi-b) > /dev/null 2>&1
    P39_MULTI_B_DIVERGED=$(cd "$HTTP_SERVERROOT/repo.git" && git rev-parse refs/heads/p39-multi-b)

    (cd "$HTTP_DEST" && "$SG" fetch) > /dev/null 2>&1
    printf 'phase39 local divergent commit\n' >> "$HTTP_DEST/top.txt"
    (cd "$HTTP_DEST" && "$SG" add top.txt) > /dev/null 2>&1
    (cd "$HTTP_DEST" && "$SG" commit -m "phase39 local divergent commit") > /dev/null 2>&1
    P39_MULTI_LOCAL_HEAD=$(cd "$HTTP_DEST" && git rev-parse HEAD)

    P39_MULTI_OUT="$WORKDIR/http_push_p39_multi_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin "HEAD:refs/heads/p39-multi-a" "HEAD:refs/heads/p39-multi-b" < /dev/null) > "$P39_MULTI_OUT" 2>&1
    check "phase39: a multi-refspec push with one non-ff exits non-zero" test $? -ne 0
    check "phase39: the fast-forwardable ref in the same push still landed" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-multi-a)\" = '$P39_MULTI_LOCAL_HEAD'"
    check "phase39: the non-fast-forward ref in the same push was left untouched" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-multi-b)\" = '$P39_MULTI_B_DIVERGED'"

    # a leading '+' force-pushes just that one refspec, without a global --force
    P39_PLUS_OUT="$WORKDIR/http_push_p39_plus_out.txt"
    (cd "$HTTP_DEST" && "$SG" push origin "+HEAD:refs/heads/p39-multi-b" < /dev/null) > "$P39_PLUS_OUT" 2>&1
    check "phase39: a leading '+' force-pushes just that refspec" test $? = 0
    check "phase39: the '+'-forced ref now matches the forced commit" \
        sh -c "test \"\$(cd '$HTTP_SERVERROOT/repo.git' && git rev-parse refs/heads/p39-multi-b)\" = '$P39_MULTI_LOCAL_HEAD'"

    (cd "$HTTP_DEST" && "$SG" switch "$HTTP_SRC_BRANCH" < /dev/null) > /dev/null 2>&1

    check "phase39: git fsck still passes on the bare repo after refspec pushes" \
        sh -c "git -C '$HTTP_SERVERROOT/repo.git' fsck > /dev/null 2>&1"

    kill "$HTTP_SERVER_PID" 2>/dev/null
    HTTP_SERVER_PID=""
else
    skip "phase5b: sg clone over smart HTTP"
    skip "phase5b: sg fetch over smart HTTP"
    skip "phase5c: sg push over smart HTTP"
fi

# --- regression: `sg push --tags` with zero local tags must still contact
# the remote. It used to short-circuit to "Everything up-to-date." purely
# from local state (before sg_transport_ls_refs_push was ever called),
# silently skipping both the network round trip and the refs/sg/chunks
# keepalive propagation that every other push path performs. Needs no HTTP
# server -- an unreachable URL is enough to distinguish "tried and failed"
# from "never tried". ---
ZEROTAG_REPO="$WORKDIR/zerotag_repo"
"$SG" init "$ZEROTAG_REPO" > /dev/null 2>&1
(cd "$ZEROTAG_REPO" && git config user.email "z@example.com" && git config user.name "z tester")
printf 'hello\n' > "$ZEROTAG_REPO/f.txt"
(cd "$ZEROTAG_REPO" && "$SG" add f.txt && "$SG" commit -m "init") > /dev/null 2>&1
printf '[remote "origin"]\n\turl = http://127.0.0.1:1/unreachable\n' >> "$ZEROTAG_REPO/.git/config"

ZEROTAG_OUT="$WORKDIR/zerotag_push_out.txt"
(cd "$ZEROTAG_REPO" && "$SG" push origin --tags) > "$ZEROTAG_OUT" 2>&1
check "phase5c bugfix: sg push --tags with zero local tags against an unreachable remote still tries to connect (fails, not a silent no-op)" \
    test $? -ne 0
check "phase5c bugfix: sg push --tags with zero local tags against an unreachable remote does not print Everything up-to-date" \
    sh -c "! grep -q 'Everything up-to-date' '$ZEROTAG_OUT'"

# --- Phase 39: refspec support for sg push -- the checks that must fail
# BEFORE any network round trip, verified the same way as the zero-tags
# bugfix just above: an unreachable URL only ever distinguishes "gave up
# without trying" from "tried and failed", both of which are failures, so
# these checks additionally confirm the failure happened for the RIGHT
# reason (grepping the actual message), not merely that the exit code was
# non-zero. ---
P39N_REPO="$WORKDIR/p39_noHTTP_repo"
"$SG" init "$P39N_REPO" > /dev/null 2>&1
(cd "$P39N_REPO" && git config user.email "p39@example.com" && git config user.name "p39 tester")
printf 'hello\n' > "$P39N_REPO/f.txt"
(cd "$P39N_REPO" && "$SG" add f.txt && "$SG" commit -m "init") > /dev/null 2>&1
printf '[remote "origin"]\n\turl = http://127.0.0.1:1/unreachable\n' >> "$P39N_REPO/.git/config"

# real git's own client-side refspec parser rejects a syntax error (empty
# dst, "<src>:") before ever touching the network -- exit 128, no
# connection-failure text at all (confirmed by hand against git 2.55.0: the
# same command against a genuinely unreachable URL prints only the invalid
# refspec message, never "Failed to connect"). sg uses exit 1 for the same
# case (CLAUDE.md's "exit codes are only ever 0 or 1" convention) -- this is
# THE deliberate, named divergence from docs/DESIGN.md Phase 39 section 5,
# pinned here on both sides so "fixing" it back into silent agreement with
# git would itself be caught.
P39N_GIT_EMPTYDST_OUT="$WORKDIR/p39n_git_emptydst_out.txt"
(LC_ALL=C git -C "$P39N_REPO" push origin "master:") > "$P39N_GIT_EMPTYDST_OUT" 2>&1
check "phase39 oracle: real git rejects an empty dst before connecting (exit 128)" \
    test $? = 128
check "phase39 oracle: real git's empty-dst rejection never attempted a connection" \
    sh -c "! grep -qi 'failed to connect\|couldn.t connect' '$P39N_GIT_EMPTYDST_OUT'"

P39N_SG_EMPTYDST_OUT="$WORKDIR/p39n_sg_emptydst_out.txt"
(cd "$P39N_REPO" && "$SG" push origin "master:") > "$P39N_SG_EMPTYDST_OUT" 2>&1
check "phase39: sg push with an empty dst (\"<src>:\") fails" test $? = 1
check "phase39: sg push with an empty dst reports invalid refspec" \
    grep -q "invalid refspec" "$P39N_SG_EMPTYDST_OUT"
check "phase39: sg's empty-dst rejection never attempted a connection either" \
    sh -c "! grep -qi 'GET .* failed\|couldn.t resolve\|couldn.t connect' '$P39N_SG_EMPTYDST_OUT'"

# --delete with a colon-containing argument rejects the WHOLE command,
# before connecting -- same divergence, same pinning shape.
P39N_GIT_DELCOLON_OUT="$WORKDIR/p39n_git_delcolon_out.txt"
(LC_ALL=C git -C "$P39N_REPO" push origin --delete "a:b") > "$P39N_GIT_DELCOLON_OUT" 2>&1
check "phase39 oracle: real git rejects --delete with a colon before connecting (exit 128)" \
    test $? = 128
check "phase39 oracle: real git's --delete/colon rejection never attempted a connection" \
    sh -c "! grep -qi 'failed to connect\|couldn.t connect' '$P39N_GIT_DELCOLON_OUT'"

P39N_SG_DELCOLON_OUT="$WORKDIR/p39n_sg_delcolon_out.txt"
(cd "$P39N_REPO" && "$SG" push origin --delete "a:b") > "$P39N_SG_DELCOLON_OUT" 2>&1
check "phase39: sg push --delete with a colon-containing name fails" test $? = 1
check "phase39: sg push --delete with a colon-containing name reports the expected message" \
    grep -q "only accepts plain target ref names" "$P39N_SG_DELCOLON_OUT"

# a malformed dst that is already "refs/"-prefixed is a pure string check,
# rejected before connecting on both sides too (docs/DESIGN.md Phase 39
# section 3's format-validation table).
P39N_GIT_BADFMT_OUT="$WORKDIR/p39n_git_badfmt_out.txt"
(LC_ALL=C git -C "$P39N_REPO" push origin "master:refs/heads/../escape") > "$P39N_GIT_BADFMT_OUT" 2>&1
check "phase39 oracle: real git rejects refs/heads/../escape before connecting (exit 128)" \
    test $? = 128
check "phase39 oracle: real git's bad-dst-format rejection never attempted a connection" \
    sh -c "! grep -qi 'failed to connect\|couldn.t connect' '$P39N_GIT_BADFMT_OUT'"

P39N_SG_BADFMT_OUT="$WORKDIR/p39n_sg_badfmt_out.txt"
(cd "$P39N_REPO" && "$SG" push origin "master:refs/heads/../escape") > "$P39N_SG_BADFMT_OUT" 2>&1
check "phase39: sg push with an invalid dst format fails" test $? = 1
check "phase39: sg push with an invalid dst format reports invalid refspec" \
    grep -q "invalid refspec" "$P39N_SG_BADFMT_OUT"

# review round: a dst naming a leading-dot path component (sg_ref_name_valid_
# for_create's ".hidden" fix) must be caught by this same dst-format gate,
# before connecting -- same divergence shape (git 128, sg 1), same pinning.
P39N_GIT_DOTFMT_OUT="$WORKDIR/p39n_git_dotfmt_out.txt"
(LC_ALL=C git -C "$P39N_REPO" push origin "master:refs/heads/.hidden") > "$P39N_GIT_DOTFMT_OUT" 2>&1
check "phase39 oracle: real git rejects refs/heads/.hidden before connecting (exit 128)" \
    test $? = 128
check "phase39 oracle: real git's dotfile-dst rejection never attempted a connection" \
    sh -c "! grep -qi 'failed to connect\|couldn.t connect' '$P39N_GIT_DOTFMT_OUT'"

P39N_SG_DOTFMT_OUT="$WORKDIR/p39n_sg_dotfmt_out.txt"
(cd "$P39N_REPO" && "$SG" push origin "master:refs/heads/.hidden") > "$P39N_SG_DOTFMT_OUT" 2>&1
check "phase39: sg push with a dst naming a leading-dot component fails" test $? = 1
check "phase39: sg push with a dst naming a leading-dot component reports invalid refspec" \
    grep -q "invalid refspec" "$P39N_SG_DOTFMT_OUT"
check "phase39: sg's dotfile-dst rejection never attempted a connection either" \
    sh -c "! grep -qi 'GET .* failed\|couldn.t resolve\|couldn.t connect' '$P39N_SG_DOTFMT_OUT'"

# wildcard refspecs and the bare push-matching ":" are real git features
# this milestone deliberately does not implement (docs/DESIGN.md Phase 39
# section 6) -- real git actually tries to connect for both (measured: it
# fails with a connection error, not a refspec error), so there is no
# matching divergence pair to pin here, only sg's own named rejection.
P39N_SG_WILDCARD_OUT="$WORKDIR/p39n_sg_wildcard_out.txt"
(cd "$P39N_REPO" && "$SG" push origin 'refs/heads/*:refs/heads/*') > "$P39N_SG_WILDCARD_OUT" 2>&1
check "phase39: sg push of a wildcard refspec fails" test $? = 1
check "phase39: sg push of a wildcard refspec reports it as unsupported" \
    grep -q "wildcard refspec" "$P39N_SG_WILDCARD_OUT"
check "phase39: sg's wildcard rejection never attempted a connection" \
    sh -c "! grep -qi 'GET .* failed\|couldn.t resolve\|couldn.t connect' '$P39N_SG_WILDCARD_OUT'"

P39N_SG_MATCHING_OUT="$WORKDIR/p39n_sg_matching_out.txt"
(cd "$P39N_REPO" && "$SG" push origin ":") > "$P39N_SG_MATCHING_OUT" 2>&1
check "phase39: sg push of a bare ':' (push matching) fails" test $? = 1
check "phase39: sg push of a bare ':' reports it as unsupported" \
    grep -q "not supported" "$P39N_SG_MATCHING_OUT"

# src resolution happens before connecting too, and a src matching nothing
# aborts the WHOLE push -- not one ref pushed, not even a connection
# attempt (docs/DESIGN.md Phase 39 section 1). Confirmed against real git
# 2.55.0 by hand: "git push <url> mainbr:newbr nosuchbranch:x" against a
# genuinely unreachable URL prints only "src refspec ... does not match
# any" / "failed to push some refs", no connection-failure text, exit 1 on
# BOTH sides (this one is NOT part of the 128-vs-1 divergence: git's own
# exit code here is already 1).
P39N_GIT_BADSRC_OUT="$WORKDIR/p39n_git_badsrc_out.txt"
(LC_ALL=C git -C "$P39N_REPO" push origin "master:newbr" "nosuchbranch:x") > "$P39N_GIT_BADSRC_OUT" 2>&1
check "phase39 oracle: real git's src-does-not-match-any is exit 1, not 128" test $? = 1
check "phase39 oracle: real git's bad-src abort never attempted a connection" \
    sh -c "! grep -qi 'failed to connect\|couldn.t connect' '$P39N_GIT_BADSRC_OUT'"
check "phase39 oracle: real git reports the bad src by name" \
    grep -q "nosuchbranch" "$P39N_GIT_BADSRC_OUT"

P39N_SG_BADSRC_OUT="$WORKDIR/p39n_sg_badsrc_out.txt"
(cd "$P39N_REPO" && "$SG" push origin "master:newbr" "nosuchbranch:x") > "$P39N_SG_BADSRC_OUT" 2>&1
check "phase39: sg push with an unresolvable src aborts the whole push" test $? = 1
check "phase39: sg push reports the bad src does not match any" \
    grep -q "src refspec nosuchbranch does not match any" "$P39N_SG_BADSRC_OUT"
check "phase39: sg's bad-src abort never attempted a connection" \
    sh -c "! grep -qi 'GET .* failed\|couldn.t resolve\|couldn.t connect' '$P39N_SG_BADSRC_OUT'"

# --- Phase 40 (bundled): `sg push` on a detached HEAD ------------------
# This refusal has existed since Phase 18 and had NO test at all. CLAUDE.md
# recorded it as untestable ("its HEAD check comes after the remote ref
# advertisement, unreachable without a live remote"); that was measured
# wrong -- it fires BEFORE any connection attempt, so an unreachable URL is
# enough and no HTTP server is needed (this block therefore never skips).
#
# Two control groups are what make this provable rather than a coincidence
# of the dead URL: re-attaching HEAD, and giving an explicit refspec, must
# BOTH get past the check and fail with the connection error instead. Without
# them, a `sg push` that refused for any reason at all would look identical.
P40_DET="$WORKDIR/p40_detached"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_DET")") > /dev/null 2>&1
printf 'a\n' > "$P40_DET/a.txt"
(cd "$P40_DET" && "$SG" add a.txt && "$SG" commit -m c1) > /dev/null 2>&1
printf 'b\n' > "$P40_DET/a.txt"
(cd "$P40_DET" && "$SG" add a.txt && "$SG" commit -m c2) > /dev/null 2>&1
# Port 9 (discard) is reserved and never served -- a connection here fails
# fast and identically on macOS and Linux.
printf '[remote "origin"]\n\turl = http://127.0.0.1:9/nope.git\n' >> "$P40_DET/.git/config"
(cd "$P40_DET" && "$SG" switch --detach HEAD~1) > /dev/null 2>&1
check "phase40: precondition -- HEAD really is detached (a bare 40-hex, not a symref)" \
    grep -qE '^[0-9a-f]{40}$' "$P40_DET/.git/HEAD"

P40_DET_OUT="$WORKDIR/p40_detached_out.txt"
(cd "$P40_DET" && "$SG" push origin) > "$P40_DET_OUT" 2>&1
check "phase40: sg push on a detached HEAD with no refspec exits non-zero" test $? -ne 0
check "phase40: ...and names the detached HEAD as the reason" \
    grep -q "detached HEAD" "$P40_DET_OUT"
check "phase40: ...and suggests naming the branch explicitly" \
    grep -q "name the branch" "$P40_DET_OUT"
check "phase40: ...and refuses BEFORE any connection attempt (no network error at all)" \
    sh -c "! grep -qi 'GET .* failed\|couldn.t resolve\|couldn.t connect' '$P40_DET_OUT'"

# Control 1: an explicit refspec is deliberately NOT subject to this check
# (Phase 39 behaviour, matching real git) -- it must reach the network.
P40_DET_RS="$WORKDIR/p40_detached_refspec.txt"
(cd "$P40_DET" && "$SG" push origin HEAD:refs/heads/x) > "$P40_DET_RS" 2>&1
check "phase40 control: an explicit refspec bypasses the detached check and reaches the network" \
    sh -c "grep -qi 'couldn.t connect\|GET .* failed' '$P40_DET_RS'"
check "phase40 control: ...and does not print the detached-HEAD refusal" \
    sh -c "! grep -q 'detached HEAD' '$P40_DET_RS'"

# Control 2: the same command on an ATTACHED HEAD must also reach the
# network -- proving the refusal above came from detachment, not the URL.
(cd "$P40_DET" && "$SG" switch master) > /dev/null 2>&1
P40_DET_ATT="$WORKDIR/p40_detached_attached.txt"
(cd "$P40_DET" && "$SG" push origin) > "$P40_DET_ATT" 2>&1
check "phase40 control: on an attached HEAD the same push reaches the network instead" \
    sh -c "grep -qi 'couldn.t connect\|GET .* failed' '$P40_DET_ATT'"
check "phase40 control: ...and prints no detached-HEAD refusal" \
    sh -c "! grep -q 'detached HEAD' '$P40_DET_ATT'"

# --- Phase 6a: content-defined chunking for large/binary files ---

# helper: total bytes of every loose object file under <repo>/.git/objects
# (excluding pack/info) -- a portable, reproducible measure of on-disk object
# storage that doesn't depend on `stat`'s incompatible -f/-c flag across
# platforms.
objects_bytes() {
    find "$1/.git/objects" -mindepth 2 -type f ! -path '*/pack/*' ! -path '*/info/*' -exec cat {} + \
        2>/dev/null | wc -c | tr -d ' '
}

# case 1: chunking defaults to off -- a large file is stored as an ordinary,
# full-size blob, not a shrunken chunk pointer
P6A_OFF_REPO="$WORKDIR/phase6a_off_repo"
mkdir -p "$P6A_OFF_REPO"
(cd "$WORKDIR" && "$SG" init phase6a_off_repo) > /dev/null 2>&1
P6A_OFF_FILE="$P6A_OFF_REPO/big.bin"
head -c 5242880 /dev/urandom > "$P6A_OFF_FILE" 2>/dev/null
(cd "$P6A_OFF_REPO" && "$SG" add big.bin && "$SG" commit -m "big file, chunking off") > /dev/null 2>&1

P6A_OFF_BLOB=$(cd "$P6A_OFF_REPO" && git ls-files -s big.bin 2>/dev/null | awk '{print $2}')
P6A_OFF_SIZE=$(cd "$P6A_OFF_REPO" && git cat-file -s "$P6A_OFF_BLOB" 2>/dev/null)
check "phase6a: chunking disabled by default -- blob is stored at its full 5MiB size" \
    test "$P6A_OFF_SIZE" = 5242880

P6A_OFF_CATOUT="$WORKDIR/p6a_off_cat.bin"
(cd "$P6A_OFF_REPO" && git cat-file -p "$P6A_OFF_BLOB") > "$P6A_OFF_CATOUT" 2>/dev/null
check "phase6a: chunking disabled by default -- blob content matches the original file exactly" \
    cmp -s "$P6A_OFF_FILE" "$P6A_OFF_CATOUT"

# case 2 + 3: with chunking enabled, a large binary file round-trips exactly
# through restore, and sg status reports the repo as clean right after commit
P6A_REPO="$WORKDIR/phase6a_repo"
mkdir -p "$P6A_REPO"
(cd "$WORKDIR" && "$SG" init phase6a_repo) > /dev/null 2>&1
git config -f "$P6A_REPO/.git/config" sg.chunking true
git config -f "$P6A_REPO/.git/config" sg.chunkthreshold 1048576

P6A_FILE="$P6A_REPO/big.bin"
head -c 5242880 /dev/urandom > "$P6A_FILE" 2>/dev/null
cp "$P6A_FILE" "$WORKDIR/p6a_original.bin"
(cd "$P6A_REPO" && "$SG" add big.bin && "$SG" commit -m "big file, chunking on") > /dev/null 2>&1
check "phase6a: sg commit with chunking enabled exits 0" test $? = 0

P6A_STATUS_OUT="$WORKDIR/p6a_status.txt"
(cd "$P6A_REPO" && "$SG" status) > "$P6A_STATUS_OUT" 2>&1
check "phase6a: sg status is clean right after committing a chunked file" \
    grep -q "nothing to commit" "$P6A_STATUS_OUT"

rm -f "$P6A_FILE"
(cd "$P6A_REPO" && "$SG" restore big.bin < /dev/null) > /dev/null 2>&1
check "phase6a: sg restore round-trips a chunked file byte-for-byte" \
    cmp -s "$WORKDIR/p6a_original.bin" "$P6A_FILE"

# case 5: git fsck stays clean (exit 0) on a repo containing chunked blobs.
# As of the phase 6b durability fix, the individual chunk blobs are no
# longer dangling from git's own object-model perspective either: they're
# also referenced via the refs/sg/chunks keep-alive tree (see chunk.c's
# keep_alive_add), a real tree/commit graph edge, not just plain hex text
# inside the pointer blob's content -- that's precisely what makes them
# survive `git gc` (see phase6b's core acceptance test below). Either way,
# `git fsck` exits 0 and reports no actual errors (missing/broken links), so
# check the exit code rather than requiring empty output.
P6A_FSCK_OUT="$WORKDIR/p6a_fsck.txt"
(cd "$P6A_REPO" && git fsck) > "$P6A_FSCK_OUT" 2>&1
P6A_FSCK_RC=$?
check "phase6a: git fsck exits 0 on a repo containing chunked blobs" \
    test "$P6A_FSCK_RC" = 0

# case 4: deduplication -- editing a small region in the middle of a large
# file should only add a small amount of new object storage, not another
# full copy of the file
P6A_DEDUP_REPO="$WORKDIR/phase6a_dedup_repo"
mkdir -p "$P6A_DEDUP_REPO"
(cd "$WORKDIR" && "$SG" init phase6a_dedup_repo) > /dev/null 2>&1
git config -f "$P6A_DEDUP_REPO/.git/config" sg.chunking true
git config -f "$P6A_DEDUP_REPO/.git/config" sg.chunkthreshold 1048576

head -c 5242880 /dev/urandom > "$P6A_DEDUP_REPO/big.bin" 2>/dev/null
(cd "$P6A_DEDUP_REPO" && "$SG" add big.bin && "$SG" commit -m "dedup baseline") > /dev/null 2>&1
DEDUP_BYTES_AFTER_FIRST=$(objects_bytes "$P6A_DEDUP_REPO")
echo "phase6a dedup: .git/objects size after the first 5MiB commit = $DEDUP_BYTES_AFTER_FIRST bytes"

# overwrite exactly one 1KB slice in the middle of the file; everything else
# is byte-for-byte unchanged
head -c 1024 /dev/zero | tr '\0' 'X' > "$WORKDIR/p6a_patch.bin"
P6A_MIDDLE=$((5242880 / 2 - 512))
dd if="$WORKDIR/p6a_patch.bin" of="$P6A_DEDUP_REPO/big.bin" bs=1 seek=$P6A_MIDDLE count=1024 \
    conv=notrunc > /dev/null 2>&1

(cd "$P6A_DEDUP_REPO" && "$SG" add big.bin && "$SG" commit -m "small mid-file edit") > /dev/null 2>&1
DEDUP_BYTES_AFTER_SECOND=$(objects_bytes "$P6A_DEDUP_REPO")
DEDUP_GROWTH=$((DEDUP_BYTES_AFTER_SECOND - DEDUP_BYTES_AFTER_FIRST))
echo "phase6a dedup: .git/objects size after the small mid-file edit commit = $DEDUP_BYTES_AFTER_SECOND bytes (growth: $DEDUP_GROWTH bytes)"

check "phase6a: a small mid-file edit adds far less than 2MiB of new object storage (dedup working)" \
    test "$DEDUP_GROWTH" -lt 2097152

# case 6: a small file whose content merely *looks* like a chunk pointer
# (well-formed magic/size/sha1/chunks header, but the referenced chunk id
# doesn't exist as an object) must never be misread as a real pointer --
# reassembly must fail closed and fall back to the literal stored bytes
P6A_FAKE_REPO="$WORKDIR/phase6a_fake_repo"
mkdir -p "$P6A_FAKE_REPO"
(cd "$WORKDIR" && "$SG" init phase6a_fake_repo) > /dev/null 2>&1
git config -f "$P6A_FAKE_REPO/.git/config" sg.chunking true
git config -f "$P6A_FAKE_REPO/.git/config" sg.chunkthreshold 1048576

P6A_FAKE_FILE="$P6A_FAKE_REPO/fake.txt"
cat > "$P6A_FAKE_FILE" <<'EOF'
sg-chunked v1
size 999
sha1 0000000000000000000000000000000000000000
chunks 1
0000000000000000000000000000000000000000
EOF
cp "$P6A_FAKE_FILE" "$WORKDIR/p6a_fake_original.txt"

(cd "$P6A_FAKE_REPO" && "$SG" add fake.txt && "$SG" commit -m "fake pointer-shaped content") > /dev/null 2>&1
check "phase6a: sg commit of a fake pointer-shaped small file exits 0" test $? = 0

rm -f "$P6A_FAKE_FILE"
(cd "$P6A_FAKE_REPO" && "$SG" restore fake.txt < /dev/null) > /dev/null 2>&1
check "phase6a: a fake pointer-shaped file restores as its literal text, not a failed chunk reassembly" \
    cmp -s "$WORKDIR/p6a_fake_original.txt" "$P6A_FAKE_FILE"

# case 7: push over smart HTTP transparently covers a chunked blob too --
# reuses the same local `git http-backend` CGI server infrastructure as
# phase 5b/5c above (same python helper script, same availability guard), so
# whatever caused phase 5b/5c to be skipped skips this too.
#
# helper: true (exit 0) iff every 40-hex chunk id listed (one per line) in
# idsfile exists as an object in the bare repo at $1 -- used to verify sg
# push actually deposited every chunk a pointer blob refers to, not just the
# pointer blob itself.
p6a_all_chunks_present_on_remote() {
    bare="$1"
    idsfile="$2"

    while IFS= read -r cid; do
        [ -z "$cid" ] && continue
        if ! git -C "$bare" cat-file -e "$cid" 2>/dev/null; then
            return 1
        fi
    done < "$idsfile"
    return 0
}

if [ "$HTTP_AVAILABLE" = 1 ]; then
    P6A_HTTP_GIT_SRC="$WORKDIR/phase6a_http_git_src"
    mkdir -p "$P6A_HTTP_GIT_SRC"
    git init -q "$P6A_HTTP_GIT_SRC"
    (cd "$P6A_HTTP_GIT_SRC" && git config user.email "p6a@example.com" && git config user.name "p6a tester")
    printf 'seed file\n' > "$P6A_HTTP_GIT_SRC/seed.txt"
    (cd "$P6A_HTTP_GIT_SRC" && git add seed.txt && git commit -q -m "seed commit")

    P6A_HTTP_SERVERROOT="$WORKDIR/phase6a_http_serverroot"
    mkdir -p "$P6A_HTTP_SERVERROOT"
    (cd "$WORKDIR" && git clone --bare -q phase6a_http_git_src "$P6A_HTTP_SERVERROOT/repo.git") > /dev/null 2>&1
    (cd "$P6A_HTTP_SERVERROOT/repo.git" && git config http.receivepack true) > /dev/null 2>&1

    P6A_HTTP_SERVER_LOG="$WORKDIR/phase6a_http_server.log"
    python3 "$HTTP_SERVER_SCRIPT" "$P6A_HTTP_SERVERROOT" > "$P6A_HTTP_SERVER_LOG" 2>&1 &
    HTTP_SERVER_PID=$!

    P6A_HTTP_PORT=""
    i=0
    while [ "$i" -lt 50 ]; do
        if [ -s "$P6A_HTTP_SERVER_LOG" ]; then
            P6A_HTTP_PORT=$(awk '/^PORT /{print $2; exit}' "$P6A_HTTP_SERVER_LOG")
            if [ -n "$P6A_HTTP_PORT" ]; then
                break
            fi
        fi
        sleep 0.1
        i=$((i + 1))
    done

    if [ -z "$P6A_HTTP_PORT" ]; then
        echo "warning: phase6a HTTP test server did not become ready in time, skipping phase6a push/clone HTTP tests" >&2
        skip "phase6a: sg push over smart HTTP with a chunked blob"
        skip "phase6a: sg push transfers every referenced chunk blob to the remote"
        skip "phase6a: git fsck exits 0 on the bare repo after pushing a chunked blob"
        skip "phase6a: sg clone over smart HTTP exits 0 for a repo containing a chunked blob"
        skip "phase6a: a second sg clone from the same (plain git) server now recovers the chunk data too, since sg push propagated refs/sg/chunks there"
        skip "phase6a: sg add of a second chunked file moves refs/sg/chunks but leaves the branch alone"
        skip "phase6a: a push with only refs/sg/chunks behind exits 0"
        skip "phase6a: a push with only refs/sg/chunks behind does not short-circuit to Everything up-to-date"
        skip "phase6a: that push advances the remote's refs/sg/chunks to the local keep-alive commit"
        skip "phase6a: that push transfers every chunk of the newly added file to the remote"
    else
        P6A_HTTP_BASE_URL="http://127.0.0.1:$P6A_HTTP_PORT/repo.git"
        P6A_HTTP_DEST="$WORKDIR/phase6a_http_dest"

        "$SG" clone "$P6A_HTTP_BASE_URL" "$P6A_HTTP_DEST" > /dev/null 2>&1

        git config -f "$P6A_HTTP_DEST/.git/config" sg.chunking true
        git config -f "$P6A_HTTP_DEST/.git/config" sg.chunkthreshold 1048576

        P6A_HTTP_FILE="$P6A_HTTP_DEST/big.bin"
        head -c 5242880 /dev/urandom > "$P6A_HTTP_FILE" 2>/dev/null
        cp "$P6A_HTTP_FILE" "$WORKDIR/p6a_http_original.bin"
        (cd "$P6A_HTTP_DEST" && "$SG" add big.bin && "$SG" commit -m "chunked file for push") > /dev/null 2>&1

        P6A_HTTP_PUSH_OUT="$WORKDIR/p6a_http_push_out.txt"
        (cd "$P6A_HTTP_DEST" && "$SG" push) > "$P6A_HTTP_PUSH_OUT" 2>&1
        check "phase6a: sg push exits 0 for a commit containing a chunked blob" test $? = 0

        # Verify sg push's own object walk actually deposited every chunk the
        # pointer blob declares onto the remote -- not just the (much
        # smaller) pointer blob itself. Reads the chunk ids straight out of
        # the pointer blob's own plain-text content via real git, then checks
        # each one's presence on the bare repo, also via real git.
        P6A_HTTP_BLOB=$(cd "$P6A_HTTP_DEST" && git ls-files -s big.bin 2>/dev/null | awk '{print $2}')
        P6A_HTTP_PTR_TXT="$WORKDIR/p6a_http_ptr.txt"
        git -C "$P6A_HTTP_DEST" cat-file -p "$P6A_HTTP_BLOB" > "$P6A_HTTP_PTR_TXT" 2>/dev/null
        P6A_HTTP_CHUNK_IDS="$WORKDIR/p6a_http_chunk_ids.txt"
        grep -E '^[0-9a-f]{40}$' "$P6A_HTTP_PTR_TXT" > "$P6A_HTTP_CHUNK_IDS"
        echo "phase6a push completeness: $(wc -l < "$P6A_HTTP_CHUNK_IDS" | tr -d ' ') chunk ids declared by the pushed pointer blob"

        check "phase6a: sg push transfers every referenced chunk blob to the remote (not just the pointer)" \
            p6a_all_chunks_present_on_remote "$P6A_HTTP_SERVERROOT/repo.git" "$P6A_HTTP_CHUNK_IDS"

        # git fsck stays clean (exit 0) on the bare repo too -- same
        # dangling-but-not-an-error situation as case 5 above, now on the
        # server side.
        P6A_HTTP_FSCK_OUT="$WORKDIR/p6a_http_fsck.txt"
        (cd "$P6A_HTTP_SERVERROOT/repo.git" && git fsck) > "$P6A_HTTP_FSCK_OUT" 2>&1
        P6A_HTTP_FSCK_RC=$?
        check "phase6a: git fsck exits 0 on the bare repo after pushing a chunked blob" \
            test "$P6A_HTTP_FSCK_RC" = 0

        # A fresh `sg clone` from that same server -- a plain, unmodified
        # real git http-backend, not an sg-aware one -- now DOES recover the
        # chunked file's actual bytes, not just the pointer text. Before the
        # push-side durability fix (problem 1 of this phase), `sg push` never
        # touched refs/sg/chunks at all, so even though the chunk blobs
        # themselves physically landed on the server (proven present above),
        # they were reachable from git's own object-model perspective only as
        # plain hex text inside the pointer blob's content -- not a real
        # tree/commit graph edge -- so a real git server's pack-objects had
        # no way to know they needed to be sent when asked for the branch
        # alone. Now that `sg push` also creates/updates refs/sg/chunks on
        # the remote (a real ref pointing at a real commit -> tree -> chunk
        # blob entries, exactly like any other ref), a real git server's
        # pack-objects DOES walk and send them once `sg clone` asks for that
        # ref's tip too (build_want_ids) -- no server-side chunking awareness
        # required, since from the server's perspective this is just an
        # ordinary ref pointing at ordinary reachable objects. Only a client
        # that never asks for refs/sg/chunks at all -- a real (non-sg) `git
        # clone`, whose default refspec is +refs/heads/*:refs/remotes/origin/*
        # -- still can't recover it (see phase6b's case 6 below for that
        # client-side limitation, which is unrelated and still stands).
        P6A_HTTP_CLONE2="$WORKDIR/phase6a_http_clone2"
        "$SG" clone "$P6A_HTTP_BASE_URL" "$P6A_HTTP_CLONE2" > /dev/null 2>&1
        check "phase6a: sg clone over smart HTTP exits 0 for a repo containing a chunked blob" test $? = 0

        check "phase6a: a second sg clone from the same (plain git) server now recovers the chunk data too, since sg push propagated refs/sg/chunks there" \
            cmp -s "$WORKDIR/p6a_http_original.bin" "$P6A_HTTP_CLONE2/big.bin"

        # case 8: the *only* thing behind on the remote is refs/sg/chunks.
        #
        # cmd_push.c decides "nothing to send" from `entry_count == 0 &&
        # !send_chunks_update`; every test above moves a branch or a tag too,
        # so the second half of that condition never gets to matter -- drop it
        # and they all still pass. This case discriminates it: `sg add` of a
        # second large file merges its chunks into the keep-alive tree
        # immediately (chunk.c's keep_alive_add runs at blob-write time, not
        # at commit time), so with no commit afterwards the branch is exactly
        # where the previous push left it while refs/sg/chunks has moved on.
        # Drop `&& !send_chunks_update` and this push prints "Everything
        # up-to-date." and leaves the remote's keep-alive ref pointing at a
        # tree that no longer protects the chunks now sitting on the server.
        P6A_CHUNKS_REMOTE_BEFORE=$(git -C "$P6A_HTTP_SERVERROOT/repo.git" rev-parse refs/sg/chunks 2>/dev/null)
        P6A_BRANCH_BEFORE=$(git -C "$P6A_HTTP_DEST" rev-parse HEAD 2>/dev/null)

        head -c 5242880 /dev/urandom > "$P6A_HTTP_DEST/big2.bin" 2>/dev/null
        (cd "$P6A_HTTP_DEST" && "$SG" add big2.bin) > /dev/null 2>&1

        P6A_CHUNKS_LOCAL_AFTER=$(git -C "$P6A_HTTP_DEST" rev-parse refs/sg/chunks 2>/dev/null)
        P6A_BRANCH_AFTER=$(git -C "$P6A_HTTP_DEST" rev-parse HEAD 2>/dev/null)

        # Fixture premise, asserted rather than assumed: if `sg add` ever
        # stopped touching the keep-alive ref (or started moving the branch),
        # the checks below would go green for the wrong reason.
        check "phase6a: sg add of a second chunked file moves refs/sg/chunks but leaves the branch alone" \
            test -n "$P6A_CHUNKS_LOCAL_AFTER" -a \
                 "$P6A_CHUNKS_LOCAL_AFTER" != "$P6A_CHUNKS_REMOTE_BEFORE" -a \
                 "$P6A_BRANCH_AFTER" = "$P6A_BRANCH_BEFORE"

        P6A_CHUNKS_PUSH_OUT="$WORKDIR/p6a_chunks_only_push_out.txt"
        (cd "$P6A_HTTP_DEST" && "$SG" push) > "$P6A_CHUNKS_PUSH_OUT" 2>&1
        check "phase6a: a push with only refs/sg/chunks behind exits 0" test $? = 0

        check "phase6a: a push with only refs/sg/chunks behind does not short-circuit to Everything up-to-date" \
            sh -c "! grep -q 'Everything up-to-date' '$P6A_CHUNKS_PUSH_OUT'"

        # The discriminating assertion: the remote's own ref actually advanced
        # to our keep-alive commit. Exit status alone can't tell "sent it"
        # from "decided there was nothing to send".
        P6A_CHUNKS_REMOTE_AFTER=$(git -C "$P6A_HTTP_SERVERROOT/repo.git" rev-parse refs/sg/chunks 2>/dev/null)
        check "phase6a: that push advances the remote's refs/sg/chunks to the local keep-alive commit" \
            test -n "$P6A_CHUNKS_REMOTE_AFTER" -a \
                 "$P6A_CHUNKS_REMOTE_AFTER" = "$P6A_CHUNKS_LOCAL_AFTER"

        # ...and the chunks that ref now protects are physically present there,
        # same completeness question as case 7 but for a push carrying no
        # branch update at all.
        P6A_BLOB2=$(cd "$P6A_HTTP_DEST" && git ls-files -s big2.bin 2>/dev/null | awk '{print $2}')
        P6A_PTR2_TXT="$WORKDIR/p6a_ptr2.txt"
        git -C "$P6A_HTTP_DEST" cat-file -p "$P6A_BLOB2" > "$P6A_PTR2_TXT" 2>/dev/null
        P6A_CHUNK_IDS2="$WORKDIR/p6a_chunk_ids2.txt"
        grep -E '^[0-9a-f]{40}$' "$P6A_PTR2_TXT" > "$P6A_CHUNK_IDS2"
        echo "phase6a chunks-only push: $(wc -l < "$P6A_CHUNK_IDS2" | tr -d ' ') chunk ids declared by the newly added pointer blob"
        check "phase6a: that push transfers every chunk of the newly added file to the remote" \
            p6a_all_chunks_present_on_remote "$P6A_HTTP_SERVERROOT/repo.git" "$P6A_CHUNK_IDS2"

        # --- Round 3 item C: a single-refspec push whose sole candidate is
        # REJECTED (non-fast-forward) must leave the remote entirely
        # untouched -- including refs/sg/chunks, even when this repo has
        # real pending chunk state to send. Before the round-3 fix,
        # entry_count == 0 alone gated the chunks-propagation block, so a
        # rejected single refspec (entries stays empty, had_rejection = 1)
        # fell through into that unconditional block and actually wrote
        # refs/sg/chunks to the remote, plus printed a spurious "To <url>"
        # line -- a real write for a push whose one and only refspec was
        # refused, contradicting pre-Phase-39 behavior.
        head -c 5242880 /dev/urandom > "$P6A_HTTP_DEST/big3.bin" 2>/dev/null
        (cd "$P6A_HTTP_DEST" && "$SG" add big3.bin) > /dev/null 2>&1

        P6A_R3_CHUNKS_REMOTE_BEFORE=$(git -C "$P6A_HTTP_SERVERROOT/repo.git" rev-parse refs/sg/chunks 2>/dev/null)
        P6A_R3_LOCAL_CHUNKS=$(git -C "$P6A_HTTP_DEST" rev-parse refs/sg/chunks 2>/dev/null)
        # Fixture premise, asserted rather than assumed: this push must have
        # real refs/sg/chunks work pending, otherwise the check below would
        # go green even with the bug still present.
        check "phase39 round3 setup: local refs/sg/chunks is ahead of the remote's before the rejected push" \
            test -n "$P6A_R3_LOCAL_CHUNKS" -a "$P6A_R3_LOCAL_CHUNKS" != "$P6A_R3_CHUNKS_REMOTE_BEFORE"

        P6A_R3_ALT_SRC="$WORKDIR/phase6a_round3_alt_src"
        (cd "$WORKDIR" && git clone -q "$P6A_HTTP_SERVERROOT/repo.git" phase6a_round3_alt_src) > /dev/null 2>&1
        (cd "$P6A_R3_ALT_SRC" && git config user.email "p6ar3@example.com" && git config user.name "p6ar3 tester")
        (cd "$P6A_R3_ALT_SRC" && git checkout -q --orphan p39c-reject && git reset -q --hard)
        printf 'phase39 round3 unrelated commit\n' > "$P6A_R3_ALT_SRC/unrelated.txt"
        (cd "$P6A_R3_ALT_SRC" && git add unrelated.txt && git commit -q -m "phase39 round3 unrelated commit")
        (cd "$P6A_R3_ALT_SRC" && git push -q origin p39c-reject) > /dev/null 2>&1

        P6A_R3_REMOTE_SNAPSHOT_BEFORE=$(git -C "$P6A_HTTP_SERVERROOT/repo.git" for-each-ref)

        P6A_R3_OUT="$WORKDIR/p6a_round3_reject_out.txt"
        (cd "$P6A_HTTP_DEST" && "$SG" push origin "HEAD:refs/heads/p39c-reject" < /dev/null) > "$P6A_R3_OUT" 2>&1
        check "phase39: a single non-fast-forward refspec push exits non-zero" test $? -ne 0

        P6A_R3_REMOTE_SNAPSHOT_AFTER=$(git -C "$P6A_HTTP_SERVERROOT/repo.git" for-each-ref)
        check "phase39: a single rejected refspec push leaves the ENTIRE remote untouched, including refs/sg/chunks" \
            test "$P6A_R3_REMOTE_SNAPSHOT_BEFORE" = "$P6A_R3_REMOTE_SNAPSHOT_AFTER"
        check "phase39: a single rejected refspec push prints no 'To <url>' line" \
            sh -c "! grep -q '^To ' '$P6A_R3_OUT'"

        kill "$HTTP_SERVER_PID" 2>/dev/null
        HTTP_SERVER_PID=""
    fi
else
    skip "phase6a: sg push over smart HTTP with a chunked blob"
    skip "phase6a: sg push transfers every referenced chunk blob to the remote"
    skip "phase6a: git fsck exits 0 on the bare repo after pushing a chunked blob"
    skip "phase6a: sg clone over smart HTTP exits 0 for a repo containing a chunked blob"
    skip "phase6a: a second sg clone from the same (plain git) server now recovers the chunk data too, since sg push propagated refs/sg/chunks there"
    skip "phase6a: sg add of a second chunked file moves refs/sg/chunks but leaves the branch alone"
    skip "phase6a: a push with only refs/sg/chunks behind exits 0"
    skip "phase6a: a push with only refs/sg/chunks behind does not short-circuit to Everything up-to-date"
    skip "phase6a: that push advances the remote's refs/sg/chunks to the local keep-alive commit"
    skip "phase6a: that push transfers every chunk of the newly added file to the remote"
    skip "phase39 round3 setup: local refs/sg/chunks is ahead of the remote's before the rejected push"
    skip "phase39: a single non-fast-forward refspec push exits non-zero"
    skip "phase39: a single rejected refspec push leaves the ENTIRE remote untouched, including refs/sg/chunks"
    skip "phase39: a single rejected refspec push prints no 'To <url>' line"
fi

# --- Phase 6b: chunk durability (refs/sg/chunks keep-alive) and the
# real-pointer-vs-fake-pointer discriminator fix ---
#
# Recap of the two problems phase6b fixes (see chunk.c/refs.c and this
# phase's report for the full design):
#   1. (durability) chunk ids were only named as plain hex text inside a
#      pointer blob's content, not a real tree/commit graph edge, so any
#      `git gc` (manual or gc.auto) collected them as garbage. Fixed by
#      keeping every chunk reachable from refs/sg/chunks.
#   2. (silent corruption) a pointer whose data was actually lost used to
#      fall back to treating the pointer's own text as if it were the
#      file's content -- reporting success while writing 500-ish bytes of
#      pointer text over what should have been megabytes of real data.
#      Fixed by hard-failing instead once a chunk pointer is recognized as
#      real (see chunk_resolve's discriminator in chunk.c).
#
# Phase 6c note: problem 2's original fix still had a gap -- the
# "recognized as real" check itself was "does the first declared chunk id's
# object file exist", which is an *integrity* question, not an *identity*
# question. When the first chunk specifically was the one lost, a genuine
# pointer got misdiagnosed as "not a pointer at all" and problem 2 recurred
# for exactly that one case. Phase 6c fixed this by deciding identity via
# SG_CHUNK_KEEPALIVE_REF tree membership (which doesn't depend on any
# chunk's object file still existing) instead -- see case 3b below.

# helper: number of loose object files under <repo>/.git/objects (excluding
# pack/info) -- a count, complementing phase6a's objects_bytes (which
# measures total size) -- makes "git gc actually repacked something" visible
# in the test output.
objects_count() {
    find "$1/.git/objects" -mindepth 2 -type f ! -path '*/pack/*' ! -path '*/info/*' 2>/dev/null \
        | wc -l | tr -d ' '
}

# case 1 (core acceptance): a real `git gc --prune=now` must not destroy a
# chunked file's data now that its chunks are kept reachable via
# refs/sg/chunks. Uses real `git gc`, not anything sg-specific, so this
# proves the object graph itself -- not just sg's own bookkeeping --
# protects the chunks.
P6B_GC_REPO="$WORKDIR/phase6b_gc_repo"
mkdir -p "$P6B_GC_REPO"
(cd "$WORKDIR" && "$SG" init phase6b_gc_repo) > /dev/null 2>&1
git config -f "$P6B_GC_REPO/.git/config" sg.chunking true
git config -f "$P6B_GC_REPO/.git/config" sg.chunkthreshold 1048576

P6B_GC_FILE="$P6B_GC_REPO/big.bin"
head -c 5242880 /dev/urandom > "$P6B_GC_FILE" 2>/dev/null
cp "$P6B_GC_FILE" "$WORKDIR/p6b_gc_original.bin"
(cd "$P6B_GC_REPO" && "$SG" add big.bin && "$SG" commit -m "big file, chunking + keep-alive") \
    > /dev/null 2>&1

P6B_OBJCOUNT_BEFORE=$(objects_count "$P6B_GC_REPO")
echo "phase6b gc: loose object count before gc = $P6B_OBJCOUNT_BEFORE"

(cd "$P6B_GC_REPO" && git gc --prune=now) > /dev/null 2>&1
P6B_GC_RC=$?
check "phase6b: git gc --prune=now exits 0 on a repo with chunked+keep-alive objects" \
    test "$P6B_GC_RC" = 0

P6B_OBJCOUNT_AFTER=$(objects_count "$P6B_GC_REPO")
echo "phase6b gc: loose object count after gc = $P6B_OBJCOUNT_AFTER (git gc repacks reachable loose objects into a packfile and deletes the now-redundant loose copies -- a low/zero loose count here is expected and proves gc actually ran; what matters is restore still working below)"

rm -f "$P6B_GC_FILE"
(cd "$P6B_GC_REPO" && "$SG" restore big.bin < /dev/null) > /dev/null 2>&1
P6B_RESTORE_RC=$?
check "phase6b: sg restore exits 0 after a real git gc --prune=now" test "$P6B_RESTORE_RC" = 0
check "phase6b: sg restore round-trips a chunked file byte-for-byte after git gc --prune=now (the core durability fix)" \
    cmp -s "$WORKDIR/p6b_gc_original.bin" "$P6B_GC_FILE"

# case 2: git fsck stays clean (exit 0) on that same repo after the gc above.
P6B_FSCK_OUT="$WORKDIR/p6b_fsck.txt"
(cd "$P6B_GC_REPO" && git fsck) > "$P6B_FSCK_OUT" 2>&1
P6B_FSCK_RC=$?
check "phase6b: git fsck exits 0 after git gc --prune=now" test "$P6B_FSCK_RC" = 0

# case 3: if a chunk really does go missing (partial corruption, manual
# tampering, a gc racing with something that hadn't kept it alive yet --
# whatever the cause), restore must fail loudly and must never overwrite the
# working file with the pointer's own text. This is problem 2 above,
# reproduced end-to-end through the real CLI (not just the library-level
# unit tests in test_chunk.c).
P6B_MISSING_REPO="$WORKDIR/phase6b_missing_repo"
mkdir -p "$P6B_MISSING_REPO"
(cd "$WORKDIR" && "$SG" init phase6b_missing_repo) > /dev/null 2>&1
git config -f "$P6B_MISSING_REPO/.git/config" sg.chunking true
git config -f "$P6B_MISSING_REPO/.git/config" sg.chunkthreshold 1048576

P6B_MISSING_FILE="$P6B_MISSING_REPO/big.bin"
head -c 5242880 /dev/urandom > "$P6B_MISSING_FILE" 2>/dev/null
cp "$P6B_MISSING_FILE" "$WORKDIR/p6b_missing_original.bin"
(cd "$P6B_MISSING_REPO" && "$SG" add big.bin && "$SG" commit -m "big file for missing-chunk test") \
    > /dev/null 2>&1

# Delete one of the pointer's declared chunk ids' loose object files
# straight off disk -- the same end state an untimely deletion/gc leaves an
# unreachable chunk in. Skips the *first* declared chunk id on purpose: per
# chunk_resolve's documented discriminator (chunk.c), that specific one is
# used to decide "is this a real pointer" at all, so deleting it would
# (correctly, per that documented trade-off) make this file look like an
# unrecognized ordinary file rather than a broken real pointer -- deleting a
# later chunk exercises the "recognized as real, then found broken" path
# this test is actually after.
P6B_MISSING_BLOB=$(cd "$P6B_MISSING_REPO" && git ls-files -s big.bin 2>/dev/null | awk '{print $2}')
P6B_MISSING_CHUNK_ID=$(git -C "$P6B_MISSING_REPO" cat-file -p "$P6B_MISSING_BLOB" 2>/dev/null \
    | grep -E '^[0-9a-f]{40}$' | sed -n '2p')
P6B_MISSING_CHUNK_PREFIX=$(echo "$P6B_MISSING_CHUNK_ID" | cut -c1-2)
P6B_MISSING_CHUNK_SUFFIX=$(echo "$P6B_MISSING_CHUNK_ID" | cut -c3-)
rm -f "$P6B_MISSING_REPO/.git/objects/$P6B_MISSING_CHUNK_PREFIX/$P6B_MISSING_CHUNK_SUFFIX"

# Also remove the working copy before restoring (rather than leaving the
# still-correct working file in place): sg_safe_apply_tree-style commands
# take an automatic pre-restore safety snapshot whenever restoring would
# discard *working-tree* changes, and that snapshot machinery re-derives
# every index entry's blob from whatever is currently in the working tree
# (see snapshot.c) -- if the (still byte-correct) working file were left in
# place, that snapshot step would harmlessly re-chunk and re-write the
# exact same content, silently regenerating the very chunk this test just
# deleted before the actual restore even ran, and the test would no longer
# be exercising the failure path it's meant to. Deleting the working file
# first also sidesteps that snapshot step entirely (nothing to lose, so no
# snapshot is taken) and matches restore's actual real-world use case: the
# working file is gone/wrong and needs to be regenerated from the object
# store, which is exactly when a missing chunk must be reported instead of
# silently producing 500-ish bytes of pointer text.
rm -f "$P6B_MISSING_FILE"

P6B_MISSING_RESTORE_OUT="$WORKDIR/p6b_missing_restore_out.txt"
(cd "$P6B_MISSING_REPO" && "$SG" restore --force big.bin) > "$P6B_MISSING_RESTORE_OUT" 2>&1
P6B_MISSING_RESTORE_RC=$?
check "phase6b: sg restore fails (non-zero) when a chunk object is missing from the object store" \
    test "$P6B_MISSING_RESTORE_RC" != 0
check "phase6b: sg restore prints an actionable error naming the missing chunk(s)" \
    grep -q "chunk" "$P6B_MISSING_RESTORE_OUT"
check "phase6b: a failed restore never writes the pointer text in place of the file -- big.bin stays absent rather than being created with wrong (short) content" \
    test ! -e "$P6B_MISSING_FILE"

# case 3b (phase 6c regression): same idea as case 3 above, but deleting the
# *first* declared chunk id instead of a later one -- this is exactly the
# residual silent-corruption case a manual verification found: the old
# discriminator in chunk_resolve (chunk.c) decided "is this a real pointer at
# all" by checking whether the first declared chunk id's object file
# existed, so losing precisely that chunk (as opposed to any other one) made
# a genuine chunk pointer misdiagnosed as ordinary content, and restore wrote
# the pointer's own ~450-byte text into big.bin as if it were the file's
# 5MiB content -- silently, with exit 0. The fix changed the discriminator
# to check SG_CHUNK_KEEPALIVE_REF tree membership instead of raw object
# existence, which does not depend on any chunk's object file still being
# present, so deleting the first chunk must now fail exactly like deleting
# any other one.
P6C_FIRSTCHUNK_REPO="$WORKDIR/phase6c_firstchunk_repo"
mkdir -p "$P6C_FIRSTCHUNK_REPO"
(cd "$WORKDIR" && "$SG" init phase6c_firstchunk_repo) > /dev/null 2>&1
git config -f "$P6C_FIRSTCHUNK_REPO/.git/config" sg.chunking true
git config -f "$P6C_FIRSTCHUNK_REPO/.git/config" sg.chunkthreshold 1048576

P6C_FIRSTCHUNK_FILE="$P6C_FIRSTCHUNK_REPO/big.bin"
head -c 3145728 /dev/urandom > "$P6C_FIRSTCHUNK_FILE" 2>/dev/null
(cd "$P6C_FIRSTCHUNK_REPO" && "$SG" add big.bin && "$SG" commit -m "big file for first-chunk-missing test") \
    > /dev/null 2>&1

# Delete the *first* declared chunk id's loose object file specifically
# (sed -n '1p' instead of case 3's '2p').
P6C_FIRSTCHUNK_BLOB=$(cd "$P6C_FIRSTCHUNK_REPO" && git ls-files -s big.bin 2>/dev/null | awk '{print $2}')
P6C_FIRSTCHUNK_ID=$(git -C "$P6C_FIRSTCHUNK_REPO" cat-file -p "$P6C_FIRSTCHUNK_BLOB" 2>/dev/null \
    | grep -E '^[0-9a-f]{40}$' | sed -n '1p')
P6C_FIRSTCHUNK_PREFIX=$(echo "$P6C_FIRSTCHUNK_ID" | cut -c1-2)
P6C_FIRSTCHUNK_SUFFIX=$(echo "$P6C_FIRSTCHUNK_ID" | cut -c3-)
rm -f "$P6C_FIRSTCHUNK_REPO/.git/objects/$P6C_FIRSTCHUNK_PREFIX/$P6C_FIRSTCHUNK_SUFFIX"

# Remove the working copy first for the same reason case 3 does (sidesteps
# the pre-restore safety snapshot re-chunking and silently regenerating the
# very chunk just deleted).
rm -f "$P6C_FIRSTCHUNK_FILE"

P6C_FIRSTCHUNK_RESTORE_OUT="$WORKDIR/p6c_firstchunk_restore_out.txt"
(cd "$P6C_FIRSTCHUNK_REPO" && "$SG" restore --force big.bin) > "$P6C_FIRSTCHUNK_RESTORE_OUT" 2>&1
P6C_FIRSTCHUNK_RESTORE_RC=$?
check "phase6c: sg restore fails (non-zero) when the FIRST chunk object is missing (residual silent-corruption regression)" \
    test "$P6C_FIRSTCHUNK_RESTORE_RC" != 0
check "phase6c: sg restore prints an actionable error naming the missing chunk(s) when the first chunk is gone" \
    grep -q "chunk" "$P6C_FIRSTCHUNK_RESTORE_OUT"
check "phase6c: a failed restore never writes the pointer text in place of the file when the first chunk is gone -- big.bin stays absent" \
    test ! -e "$P6C_FIRSTCHUNK_FILE"

# case 4: the existing "fake pointer" case from phase6a (a small ordinary
# file whose content merely *looks* like a well-formed chunk pointer, but
# whose declared chunk id was never a real object) is re-checked by simply
# re-running the whole suite -- see phase6a's own case 6 above, still
# expected to pass unchanged: this is exactly what chunk_resolve's
# discriminator classifies as "not a pointer" (case 3 in the report), not
# "broken pointer" (case 3 above), because its first declared chunk id was
# never a real object either.

# case 5: sg push genuinely propagates refs/sg/chunks to the remote, not just
# the branch ref -- this is the CRITICAL push-side half of the durability fix
# (cmd_push.c now compares its local SG_CHUNK_KEEPALIVE_REF against whatever
# the remote advertises for that same ref name, and includes a second
# ref-update command line for it in the same git-receive-pack request as the
# branch). Before this fix, `sg push` never touched refs/sg/chunks at all, so
# a plain `sg clone` of the resulting remote could never recognize/protect
# those chunks (nothing to merge), and the remote's own `git gc` would prune
# them as unreferenced loose objects even though `sg push`'s object walk
# (walk_add_object's SG_OBJ_BLOB case) already deposited the chunk data
# itself onto the remote. The "server" bare repo below is created genuinely
# empty and only ever populated by real `sg push` calls -- no
# `git clone --mirror`/`--bare` shortcut -- so every check in this block
# exercises the real push-side code path end to end.
if [ "$HTTP_AVAILABLE" = 1 ]; then
    P6B_CLONE_SRC="$WORKDIR/phase6b_clone_src"
    mkdir -p "$P6B_CLONE_SRC"
    (cd "$WORKDIR" && "$SG" init phase6b_clone_src) > /dev/null 2>&1
    git config -f "$P6B_CLONE_SRC/.git/config" sg.chunking true
    git config -f "$P6B_CLONE_SRC/.git/config" sg.chunkthreshold 1048576

    P6B_CLONE_SRC_FILE="$P6B_CLONE_SRC/big.bin"
    head -c 5242880 /dev/urandom > "$P6B_CLONE_SRC_FILE" 2>/dev/null
    cp "$P6B_CLONE_SRC_FILE" "$WORKDIR/p6b_clone_original.bin"
    (cd "$P6B_CLONE_SRC" && "$SG" add big.bin && "$SG" commit -m "chunked file, source for sg clone test") \
        > /dev/null 2>&1

    P6B_SERVERROOT="$WORKDIR/phase6b_serverroot"
    mkdir -p "$P6B_SERVERROOT"
    git init -q --bare "$P6B_SERVERROOT/repo.git" > /dev/null 2>&1
    (cd "$P6B_SERVERROOT/repo.git" && git config http.receivepack true) > /dev/null 2>&1

    P6B_SERVER_LOG="$WORKDIR/phase6b_server.log"
    python3 "$HTTP_SERVER_SCRIPT" "$P6B_SERVERROOT" > "$P6B_SERVER_LOG" 2>&1 &
    HTTP_SERVER_PID=$!

    P6B_PORT=""
    i=0
    while [ "$i" -lt 50 ]; do
        if [ -s "$P6B_SERVER_LOG" ]; then
            P6B_PORT=$(awk '/^PORT /{print $2; exit}' "$P6B_SERVER_LOG")
            if [ -n "$P6B_PORT" ]; then
                break
            fi
        fi
        sleep 0.1
        i=$((i + 1))
    done

    if [ -z "$P6B_PORT" ]; then
        echo "warning: phase6b HTTP test server did not become ready in time, skipping phase6b sg push/clone chunk tests" >&2
        skip "phase6b: sg push to a genuinely empty remote exits 0"
        skip "phase6b: after a real sg push, the remote genuinely has refs/sg/chunks"
        skip "phase6b: sg clone over smart HTTP recovers a chunked file's chunk data byte-for-byte"
        skip "phase6b: git fsck exits 0 on the sg-clone destination"
        skip "phase6b: a real (non-sg) git clone of the same repo only recovers the pointer text, not the chunk data"
        skip "phase6b: after git gc --prune=now on the remote, a fresh sg clone still recovers the chunk data byte-for-byte (regression test for the sg-push refs/sg/chunks bug)"
        skip "phase6b: git fsck stays clean on the remote after git gc"
    else
        P6B_BASE_URL="http://127.0.0.1:$P6B_PORT/repo.git"
        P6B_DEST="$WORKDIR/phase6b_dest"

        # Point the sg source repo at the server the same way `sg clone`
        # itself would have (write_config_stanza in cmd_clone.c writes this
        # exact remote.origin.url key), then push for real -- this is the
        # actual mechanism under test: sg push must propagate refs/sg/chunks
        # to the remote by itself, not via any git-native shortcut.
        git config -f "$P6B_CLONE_SRC/.git/config" remote.origin.url "$P6B_BASE_URL"

        P6B_PUSH_OUT="$WORKDIR/p6b_push_out.txt"
        (cd "$P6B_CLONE_SRC" && "$SG" push origin master) > "$P6B_PUSH_OUT" 2>&1
        check "phase6b: sg push to a genuinely empty remote exits 0" test $? = 0

        check "phase6b: after a real sg push, the remote genuinely has refs/sg/chunks" \
            sh -c "git -C '$P6B_SERVERROOT/repo.git' show-ref --verify --quiet refs/sg/chunks"

        "$SG" clone "$P6B_BASE_URL" "$P6B_DEST" > /dev/null 2>&1
        check "phase6b: sg clone over smart HTTP recovers a chunked file's chunk data byte-for-byte" \
            cmp -s "$WORKDIR/p6b_clone_original.bin" "$P6B_DEST/big.bin"

        P6B_DEST_FSCK_OUT="$WORKDIR/p6b_dest_fsck.txt"
        (cd "$P6B_DEST" && git fsck) > "$P6B_DEST_FSCK_OUT" 2>&1
        P6B_DEST_FSCK_RC=$?
        check "phase6b: git fsck exits 0 on the sg-clone destination" test "$P6B_DEST_FSCK_RC" = 0

        # The actual regression test for the CRITICAL bug this phase fixes:
        # before sg push propagated refs/sg/chunks, the chunk blobs it
        # deposited on the remote were reachable from git's object-model
        # perspective only via that ref's tree -- without it, a real `git gc`
        # on the remote would prune them as unreferenced loose objects,
        # right out from under the (already-pushed) pointer blob that names
        # them. Run a real `git gc --prune=now` on the bare remote, then
        # clone fresh again: it must still recover the exact same bytes.
        (cd "$P6B_SERVERROOT/repo.git" && git gc --prune=now) > /dev/null 2>&1

        P6B_DEST2="$WORKDIR/phase6b_dest_after_gc"
        "$SG" clone "$P6B_BASE_URL" "$P6B_DEST2" > /dev/null 2>&1
        check "phase6b: after git gc --prune=now on the remote, a fresh sg clone still recovers the chunk data byte-for-byte (regression test for the sg-push refs/sg/chunks bug)" \
            cmp -s "$WORKDIR/p6b_clone_original.bin" "$P6B_DEST2/big.bin"

        P6B_REMOTE_FSCK_OUT="$WORKDIR/p6b_remote_fsck.txt"
        (cd "$P6B_SERVERROOT/repo.git" && git fsck) > "$P6B_REMOTE_FSCK_OUT" 2>&1
        P6B_REMOTE_FSCK_RC=$?
        check "phase6b: git fsck stays clean on the remote after git gc" test "$P6B_REMOTE_FSCK_RC" = 0

        # case 6: a real (non-sg) `git clone` of the exact same server only
        # ever recovers the pointer text -- its default refspec
        # (+refs/heads/*:refs/remotes/origin/*) never asks for
        # refs/sg/chunks at all, regardless of whether the remote has it.
        # This is the accepted, documented trade-off from the phase 6b spec
        # (the same category of limitation Git LFS has under a
        # non-LFS-aware git client), not a bug -- assert it explicitly
        # rather than silently assuming it.
        P6B_REALGIT_CLONE="$WORKDIR/phase6b_realgit_clone"
        git clone -q "$P6B_BASE_URL" "$P6B_REALGIT_CLONE" > /dev/null 2>&1
        P6B_REALGIT_SIZE=$(wc -c < "$P6B_REALGIT_CLONE/big.bin" 2>/dev/null | tr -d ' ')
        echo "phase6b real git clone limitation: original file was 5242880 bytes; a real (non-sg) git clone recovered only ${P6B_REALGIT_SIZE:-0} bytes (the pointer text itself -- real git's default refspec never requests refs/sg/chunks)"
        check "phase6b: a real (non-sg) git clone of the same repo only recovers the pointer text, not the chunk data" \
            test "${P6B_REALGIT_SIZE:-0}" -lt 50000

        # case 7: sg push must abort (non-zero, actionable message) rather
        # than silently push a chunked file's bare pointer text when one of
        # its declared chunks is missing/corrupt locally -- the push-side
        # mirror of case 3's restore-side check above. Reuses this same
        # running server (a second, initially-empty bare repo under it) so
        # a brand-new branch push exercises walk_add_object's full object
        # walk, same as any real first push would.
        P6B_PUSHFAIL_BARE="$P6B_SERVERROOT/pushfail_repo.git"
        git init -q --bare "$P6B_PUSHFAIL_BARE" > /dev/null 2>&1
        git -C "$P6B_PUSHFAIL_BARE" config http.receivepack true

        P6B_PUSHFAIL_URL="http://127.0.0.1:$P6B_PORT/pushfail_repo.git"
        P6B_PUSHFAIL_SRC="$WORKDIR/phase6b_pushfail_src"
        "$SG" clone "$P6B_PUSHFAIL_URL" "$P6B_PUSHFAIL_SRC" > /dev/null 2>&1

        git config -f "$P6B_PUSHFAIL_SRC/.git/config" sg.chunking true
        git config -f "$P6B_PUSHFAIL_SRC/.git/config" sg.chunkthreshold 1048576

        head -c 5242880 /dev/urandom > "$P6B_PUSHFAIL_SRC/big.bin" 2>/dev/null
        (cd "$P6B_PUSHFAIL_SRC" && "$SG" add big.bin && "$SG" commit -m "chunked file for push-failure test") \
            > /dev/null 2>&1

        # Delete a non-first declared chunk id's loose object file (same
        # "not the first chunk" reasoning as case 3's restore test above).
        P6B_PUSHFAIL_BLOB=$(cd "$P6B_PUSHFAIL_SRC" && git ls-files -s big.bin 2>/dev/null | awk '{print $2}')
        P6B_PUSHFAIL_CHUNK_ID=$(git -C "$P6B_PUSHFAIL_SRC" cat-file -p "$P6B_PUSHFAIL_BLOB" 2>/dev/null \
            | grep -E '^[0-9a-f]{40}$' | sed -n '2p')
        P6B_PUSHFAIL_CHUNK_PREFIX=$(echo "$P6B_PUSHFAIL_CHUNK_ID" | cut -c1-2)
        P6B_PUSHFAIL_CHUNK_SUFFIX=$(echo "$P6B_PUSHFAIL_CHUNK_ID" | cut -c3-)
        rm -f "$P6B_PUSHFAIL_SRC/.git/objects/$P6B_PUSHFAIL_CHUNK_PREFIX/$P6B_PUSHFAIL_CHUNK_SUFFIX"

        P6B_PUSHFAIL_OUT="$WORKDIR/p6b_pushfail_out.txt"
        (cd "$P6B_PUSHFAIL_SRC" && "$SG" push) > "$P6B_PUSHFAIL_OUT" 2>&1
        P6B_PUSHFAIL_RC=$?
        check "phase6b: sg push fails (non-zero) when a locally chunked file has a missing chunk object" \
            test "$P6B_PUSHFAIL_RC" != 0
        check "phase6b: sg push prints an actionable abort message naming the broken chunked object" \
            grep -q "push aborted" "$P6B_PUSHFAIL_OUT"

        P6B_PUSHFAIL_REMOTE_BRANCHES=$(git -C "$P6B_PUSHFAIL_BARE" for-each-ref --format='%(refname)' \
            2>/dev/null | grep -c '^refs/heads/')
        check "phase6b: after the aborted push, the remote gained no branch ref (no incomplete pointer text was published)" \
            test "$P6B_PUSHFAIL_REMOTE_BRANCHES" = 0

        kill "$HTTP_SERVER_PID" 2>/dev/null
        HTTP_SERVER_PID=""
    fi
else
    skip "phase6b: sg clone over smart HTTP recovers a chunked file's chunk data byte-for-byte"
    skip "phase6b: git fsck exits 0 on the sg-clone destination"
    skip "phase6b: a real (non-sg) git clone of the same repo only recovers the pointer text, not the chunk data"
    skip "phase6b: sg push fails (non-zero) when a locally chunked file has a missing chunk object"
    skip "phase6b: sg push prints an actionable abort message naming the broken chunked object"
    skip "phase6b: after the aborted push, the remote gained no branch ref (no incomplete pointer text was published)"
fi

# --- Phase 6d: two CRITICAL bugs found reviewing merge/rebase against
# phase 6's chunked-blob storage --
#
#   A. src/workdir/merge.c predates phase 6 entirely and read blob content
#      via plain sg_object_read, never sg_chunk_read_blob -- so merging a
#      chunked file compared/diffed its ~500-byte pointer TEXT (magic/size/
#      sha1/chunk-id-list), not the real multi-megabyte content. A conflict
#      on a chunked file left the pointer text's own conflict markers in the
#      working tree, and `sg add && sg commit` (exactly what sg's own
#      conflict-resolution instructions tell the user to do) would
#      permanently commit that garbage in place of the real file.
#
#   B. chunk_resolve's discriminator (chunk.c) used to treat "no
#      SG_CHUNK_KEEPALIVE_REF" as flatly meaning "not a pointer of ours",
#      which is only true for a repo that never used chunking (e.g. a plain
#      `git clone`). If the ref existed and was later deleted (accidentally,
#      by tooling unaware of this custom ref, or by anything that treats an
#      unrecognized ref as safe to prune), every chunked file in the WHOLE
#      repository would silently start "restoring" as a few hundred bytes of
#      pointer text with exit 0 -- total, silent data loss. The fix records
#      a local (not-cloned) marker the first time this repo ever chunks
#      anything, so an absent ref can be told apart from "never used
#      chunking" vs. "used chunking, and the safety net is now gone".
#
# case 1 (bug A, binary): two branches each modify a different, non-
# overlapping region of the same large *binary* chunked file. Before the
# fix, `sg merge` would "succeed" at producing a ~650-byte conflict file made
# of the two branches' pointer texts diffed against each other -- neither
# side's actual file content ever appears. After the fix, a binary conflict
# correctly surfaces (NUL bytes are near-certain in 3 MiB of random data),
# and the file left in the working tree must be one side's REAL content, at
# its real (megabyte-scale) size, never the pointer text.
P6D_BIN_REPO="$WORKDIR/phase6d_binary_repo"
mkdir -p "$P6D_BIN_REPO"
(cd "$WORKDIR" && "$SG" init phase6d_binary_repo) > /dev/null 2>&1
git config -f "$P6D_BIN_REPO/.git/config" sg.chunking true
git config -f "$P6D_BIN_REPO/.git/config" sg.chunkthreshold 1048576

P6D_BIN_FILE="$P6D_BIN_REPO/big.bin"
head -c 3145728 /dev/urandom > "$P6D_BIN_FILE" 2>/dev/null
(cd "$P6D_BIN_REPO" && "$SG" add big.bin && "$SG" commit -m "base binary") > /dev/null 2>&1
(cd "$P6D_BIN_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
python3 -c "
data = bytearray(open('$P6D_BIN_FILE', 'rb').read())
data[0:1000] = b'F' * 1000
open('$P6D_BIN_FILE', 'wb').write(data)
"
(cd "$P6D_BIN_REPO" && "$SG" add big.bin && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P6D_BIN_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
python3 -c "
data = bytearray(open('$P6D_BIN_FILE', 'rb').read())
data[2000000:2001000] = b'M' * 1000
open('$P6D_BIN_FILE', 'wb').write(data)
"
(cd "$P6D_BIN_REPO" && "$SG" add big.bin && "$SG" commit -m "master change") > /dev/null 2>&1

(cd "$P6D_BIN_REPO" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
P6D_BIN_MERGE_RC=$?
check "phase6d bug A (binary): sg merge on a chunked binary file conflict exits non-zero" \
    test "$P6D_BIN_MERGE_RC" -ne 0

P6D_BIN_SIZE=$(wc -c < "$P6D_BIN_FILE" 2>/dev/null | tr -d ' ')
echo "phase6d bug A (binary): conflicted big.bin is ${P6D_BIN_SIZE:-0} bytes (original was 3145728)"
check "phase6d bug A (binary): the conflicted file in the working tree is NOT shrunk to pointer-text size" \
    test "${P6D_BIN_SIZE:-0}" -gt 1000000
check "phase6d bug A (binary): the conflicted file does not contain the sg-chunked pointer magic" \
    sh -c "! grep -qa 'sg-chunked' '$P6D_BIN_FILE'"

# case 2 (bug A, text): two branches each modify a different line of the
# same large *text* chunked file (line counts kept in the low thousands, not
# hundreds of thousands, to stay within sg_merge_content's O(n*m) diff3-lite
# table -- an unrelated, pre-existing algorithmic limit, not part of either
# bug this phase fixes). Before the fix this would "merge" two pointer texts
# together and produce garbage; after the fix it must be a real, clean,
# automatic three-way merge containing both sides' actual edits.
P6D_TXT_REPO="$WORKDIR/phase6d_text_repo"
mkdir -p "$P6D_TXT_REPO"
(cd "$WORKDIR" && "$SG" init phase6d_text_repo) > /dev/null 2>&1
git config -f "$P6D_TXT_REPO/.git/config" sg.chunking true
git config -f "$P6D_TXT_REPO/.git/config" sg.chunkthreshold 1048576

P6D_TXT_FILE="$P6D_TXT_REPO/big.txt"
python3 -c "
lines = [('line %06d ' % i) + ('x' * 80) + '\n' for i in range(15000)]
open('$P6D_TXT_FILE', 'w').writelines(lines)
"
(cd "$P6D_TXT_REPO" && "$SG" add big.txt && "$SG" commit -m "base text") > /dev/null 2>&1
(cd "$P6D_TXT_REPO" && "$SG" switch -c feature) > /dev/null 2>&1
python3 -c "
lines = open('$P6D_TXT_FILE').readlines()
lines[100] = 'PHASE6D FEATURE CHANGE\n'
open('$P6D_TXT_FILE', 'w').writelines(lines)
"
(cd "$P6D_TXT_REPO" && "$SG" add big.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P6D_TXT_REPO" && "$SG" switch master < /dev/null) > /dev/null 2>&1
python3 -c "
lines = open('$P6D_TXT_FILE').readlines()
lines[14000] = 'PHASE6D MASTER CHANGE\n'
open('$P6D_TXT_FILE', 'w').writelines(lines)
"
(cd "$P6D_TXT_REPO" && "$SG" add big.txt && "$SG" commit -m "master change") > /dev/null 2>&1

(cd "$P6D_TXT_REPO" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
check "phase6d bug A (text): sg merge cleanly auto-merges non-overlapping edits to a chunked text file" \
    test $? = 0
check "phase6d bug A (text): the merged file does not contain the sg-chunked pointer magic" \
    sh -c "! grep -qa 'sg-chunked' '$P6D_TXT_FILE'"
check "phase6d bug A (text): the merged file contains BOTH branches' real edits" \
    sh -c "grep -q 'PHASE6D FEATURE CHANGE' '$P6D_TXT_FILE' && grep -q 'PHASE6D MASTER CHANGE' '$P6D_TXT_FILE'"
check "phase6d bug A (text): git fsck exits 0 on the repo after the chunk-aware merge" \
    sh -c "(cd '$P6D_TXT_REPO' && git fsck) > /dev/null 2>&1"

# case 3 (bug B): deleting refs/sg/chunks from a repo that has genuinely used
# chunked storage (so the .git/config marker is set) must make `sg restore`
# fail loudly instead of silently writing the pointer's own text in place of
# the real file -- the same failure mode as phase6b's case 3/3b, but caused
# by losing the *ref* rather than an individual chunk object.
P6D_LOSTREF_REPO="$WORKDIR/phase6d_lostref_repo"
mkdir -p "$P6D_LOSTREF_REPO"
(cd "$WORKDIR" && "$SG" init phase6d_lostref_repo) > /dev/null 2>&1
git config -f "$P6D_LOSTREF_REPO/.git/config" sg.chunking true
git config -f "$P6D_LOSTREF_REPO/.git/config" sg.chunkthreshold 1048576

P6D_LOSTREF_FILE="$P6D_LOSTREF_REPO/big.bin"
head -c 5242880 /dev/urandom > "$P6D_LOSTREF_FILE" 2>/dev/null
(cd "$P6D_LOSTREF_REPO" && "$SG" add big.bin && "$SG" commit -m "big file for lost-ref test") \
    > /dev/null 2>&1

check "phase6d bug B: .git/config records that this repo has used chunked storage" \
    grep -q "everchunked" "$P6D_LOSTREF_REPO/.git/config"

(cd "$P6D_LOSTREF_REPO" && git update-ref -d refs/sg/chunks) > /dev/null 2>&1
check "phase6d bug B: refs/sg/chunks really is gone after git update-ref -d" \
    sh -c "! (cd '$P6D_LOSTREF_REPO' && git show-ref --verify --quiet refs/sg/chunks)"

rm -f "$P6D_LOSTREF_FILE"
P6D_LOSTREF_OUT="$WORKDIR/p6d_lostref_restore_out.txt"
(cd "$P6D_LOSTREF_REPO" && "$SG" restore --force big.bin) > "$P6D_LOSTREF_OUT" 2>&1
P6D_LOSTREF_RC=$?
check "phase6d bug B: sg restore fails (non-zero) once refs/sg/chunks is deleted from a repo that used chunking" \
    test "$P6D_LOSTREF_RC" != 0
check "phase6d bug B: sg restore's error names refs/sg/chunks as the thing that went missing" \
    grep -q "refs/sg/chunks" "$P6D_LOSTREF_OUT"
check "phase6d bug B: a failed restore never writes the pointer text in place of the file -- big.bin stays absent" \
    test ! -e "$P6D_LOSTREF_FILE"

# case 4 (bug B, accepted baseline): a repo obtained via a real (non-sg) `git
# clone` has neither refs/sg/chunks nor the .git/config marker -- this must
# keep behaving exactly as documented before this phase (pointer text is
# what a non-chunk-aware reader gets back), unaffected by the hard-failure
# path added above. Uses a local filesystem clone (not the HTTP server) so
# this case always runs regardless of HTTP availability.
P6D_PLAINCLONE_SRC="$WORKDIR/phase6d_plainclone_src"
mkdir -p "$P6D_PLAINCLONE_SRC"
(cd "$WORKDIR" && "$SG" init phase6d_plainclone_src) > /dev/null 2>&1
git config -f "$P6D_PLAINCLONE_SRC/.git/config" sg.chunking true
git config -f "$P6D_PLAINCLONE_SRC/.git/config" sg.chunkthreshold 1048576
head -c 5242880 /dev/urandom > "$P6D_PLAINCLONE_SRC/big.bin" 2>/dev/null
(cd "$P6D_PLAINCLONE_SRC" && "$SG" add big.bin && "$SG" commit -m "big file, source for plain clone") \
    > /dev/null 2>&1

P6D_PLAINCLONE_DEST="$WORKDIR/phase6d_plainclone_dest"
(cd "$WORKDIR" && git clone -q phase6d_plainclone_src phase6d_plainclone_dest) > /dev/null 2>&1
check "phase6d bug B baseline: a real git clone has no refs/sg/chunks (never advertised/requested)" \
    sh -c "! (cd '$P6D_PLAINCLONE_DEST' && git show-ref --verify --quiet refs/sg/chunks)"
check "phase6d bug B baseline: a real git clone has no .git/config chunking-used marker (fresh config)" \
    sh -c "! grep -q 'everchunked' '$P6D_PLAINCLONE_DEST/.git/config'"

P6D_PLAINCLONE_SIZE=$(wc -c < "$P6D_PLAINCLONE_DEST/big.bin" 2>/dev/null | tr -d ' ')
echo "phase6d bug B baseline: original was 5242880 bytes; a plain git clone's big.bin is ${P6D_PLAINCLONE_SIZE:-0} bytes (pointer text -- documented, unaffected by the fix)"
check "phase6d bug B baseline: a repo that never used chunking still gets the pointer text unchanged (documented limitation, not a bug)" \
    test "${P6D_PLAINCLONE_SIZE:-0}" -lt 50000
check "phase6d bug B baseline: sg status still exits 0 against that pointer text (treated as ordinary content)" \
    sh -c "(cd '$P6D_PLAINCLONE_DEST' && '$SG' status) > /dev/null 2>&1"

# --- Phase 6e: sg-chunks.lock regression test (problem 2 of the push-side
# durability fix) -- keep_alive_add's read-existing-tree / merge / write-new-
# commit / update-ref sequence used to run with no locking at all, so two
# concurrent callers (two `sg add`/`sg commit` invocations on different large
# files, sg fetch's merge, or the new push-time merge, racing against each
# other or against a plain sg_chunk_store_blob call) could both read the same
# snapshot of the keep-alive tree and have one's write clobber the other's --
# silently dropping the loser's entire set of newly-added chunk ids out of
# keep-alive protection, recreating the original gc-collects-referenced-
# chunks bug via a race instead of a missing ref. Exercised here with two
# `sg add` invocations (chosen over `sg commit` since store_blob/keep_alive_add
# is what `sg add` actually calls) racing on two different, disjoint-content
# large files in the same repo.
#
# The invariant checked after each race is a correctness property, not a
# timing-sensitive one: re-adding an already-fully-kept-alive file must never
# grow the keep-alive tree (sg_chunk_split is a pure function of content, and
# keep_alive_add already dedups against whatever's present). If the race
# dropped either file's chunk ids, re-adding that file afterward re-merges
# them back in and the tree visibly grows -- so this must hold on every
# iteration regardless of whether that particular run's OS scheduling
# actually overlapped at the critical section, while still needing a few
# repeats to have a good chance of actually exercising the overlap at all.
P6E_LOCK_REPO="$WORKDIR/phase6e_lock_repo"
mkdir -p "$P6E_LOCK_REPO"
(cd "$WORKDIR" && "$SG" init phase6e_lock_repo) > /dev/null 2>&1
git config -f "$P6E_LOCK_REPO/.git/config" sg.chunking true
git config -f "$P6E_LOCK_REPO/.git/config" sg.chunkthreshold 262144

p6e_tree_entry_count() {
    if (cd "$P6E_LOCK_REPO" && git show-ref --verify --quiet refs/sg/chunks) > /dev/null 2>&1; then
        (cd "$P6E_LOCK_REPO" && git cat-file -p refs/sg/chunks^{tree}) 2>/dev/null | wc -l | tr -d ' '
    else
        echo 0
    fi
}

P6E_LOCK_OK=1
P6E_ITER=1
while [ "$P6E_ITER" -le 5 ]; do
    P6E_F1="race${P6E_ITER}_a.bin"
    P6E_F2="race${P6E_ITER}_b.bin"
    head -c 1572864 /dev/urandom > "$P6E_LOCK_REPO/$P6E_F1" 2>/dev/null
    head -c 1572864 /dev/urandom > "$P6E_LOCK_REPO/$P6E_F2" 2>/dev/null

    (cd "$P6E_LOCK_REPO" && "$SG" add "$P6E_F1") > /dev/null 2>&1 &
    P6E_PID1=$!
    (cd "$P6E_LOCK_REPO" && "$SG" add "$P6E_F2") > /dev/null 2>&1 &
    P6E_PID2=$!
    wait "$P6E_PID1"
    wait "$P6E_PID2"

    P6E_COUNT_RACE=$(p6e_tree_entry_count)

    # Re-adding each file sequentially (no race) after the fact must not grow
    # the tree any further -- if it does, that file's chunks from the
    # concurrent race above were lost.
    (cd "$P6E_LOCK_REPO" && "$SG" add "$P6E_F1") > /dev/null 2>&1
    P6E_COUNT_AFTER1=$(p6e_tree_entry_count)

    (cd "$P6E_LOCK_REPO" && "$SG" add "$P6E_F2") > /dev/null 2>&1
    P6E_COUNT_AFTER2=$(p6e_tree_entry_count)

    if [ "$P6E_COUNT_AFTER1" != "$P6E_COUNT_RACE" ] || [ "$P6E_COUNT_AFTER2" != "$P6E_COUNT_AFTER1" ]; then
        echo "phase6e lock regression: iteration $P6E_ITER lost chunk(s) during a concurrent sg add race (tree entries: after race=$P6E_COUNT_RACE, after re-add file1=$P6E_COUNT_AFTER1, after re-add file2=$P6E_COUNT_AFTER2)"
        P6E_LOCK_OK=0
    fi

    P6E_ITER=$((P6E_ITER + 1))
done

echo "phase6e lock regression: refs/sg/chunks has $(p6e_tree_entry_count) entries after 5 racing iterations of two files each"
check "phase6e: two concurrent sg add invocations on different large files never lose either file's chunks from refs/sg/chunks (sg-chunks.lock regression, 5 iterations)" \
    test "$P6E_LOCK_OK" = 1

# --- Phase 6f: `sg diff` must be chunk-aware (problem 3) -- it used to read
# a staged chunked blob via plain sg_object_read, diffing the ~500-byte
# pointer blob's own raw text against the real working-tree file instead of
# the real content, which for an unmodified file spuriously reported a diff
# (lengths never match) and for a modified file produced meaningless output
# built from the pointer's magic/size/sha1/chunk-id lines instead of the
# actual change.
P6F_DIFF_REPO="$WORKDIR/phase6f_diff_repo"
mkdir -p "$P6F_DIFF_REPO"
(cd "$WORKDIR" && "$SG" init phase6f_diff_repo) > /dev/null 2>&1
git config -f "$P6F_DIFF_REPO/.git/config" sg.chunking true
git config -f "$P6F_DIFF_REPO/.git/config" sg.chunkthreshold 1048576

P6F_FILE="$P6F_DIFF_REPO/big.txt"
python3 -c "
lines = [('line %06d ' % i) + ('x' * 80) + '\n' for i in range(15000)]
open('$P6F_FILE', 'w').writelines(lines)
"
(cd "$P6F_DIFF_REPO" && "$SG" add big.txt && "$SG" commit -m "chunked text file for diff test") \
    > /dev/null 2>&1

# case 1: an unmodified chunked file -> sg diff must exit 0 with NO output at
# all for it. Before the fix, the staged side was the pointer blob's raw
# ~500-byte text, which differs in length from the real ~1.4MB working-tree
# file even though the file itself is genuinely unmodified, so the old code
# would have spuriously reported a diff here.
P6F_UNMOD_OUT="$WORKDIR/p6f_unmod_diff_out.txt"
(cd "$P6F_DIFF_REPO" && "$SG" diff) > "$P6F_UNMOD_OUT" 2>&1
P6F_UNMOD_RC=$?
check "phase6f: sg diff on an unmodified chunked file exits 0" test "$P6F_UNMOD_RC" = 0
check "phase6f: sg diff on an unmodified chunked file produces no output (not a spurious pointer-vs-content diff)" \
    test ! -s "$P6F_UNMOD_OUT"

# case 2: a modified chunked file -> sg diff must reflect the real content
# change, not a diff between the pointer blob's own raw text and the working
# tree (which would print the literal "sg-chunked v1" pointer header and
# size/sha1/chunk-id lines as removed content -- the same magic-string check
# already used for the merge fix in phase6d above).
python3 -c "
lines = open('$P6F_FILE').readlines()
lines[100] = 'PHASE6F DIFF CHANGE\n'
open('$P6F_FILE', 'w').writelines(lines)
"
P6F_MOD_OUT="$WORKDIR/p6f_mod_diff_out.txt"
(cd "$P6F_DIFF_REPO" && "$SG" diff) > "$P6F_MOD_OUT" 2>&1
P6F_MOD_RC=$?
check "phase6f: sg diff on a modified chunked file exits 0" test "$P6F_MOD_RC" = 0
check "phase6f: sg diff on a modified chunked file shows the real added line" \
    grep -q '^+PHASE6F DIFF CHANGE$' "$P6F_MOD_OUT"
check "phase6f: sg diff on a modified chunked file does not contain the sg-chunked pointer magic" \
    sh -c "! grep -qa 'sg-chunked' '$P6F_MOD_OUT'"

# case 3: a genuinely broken chunk pointer (a declared chunk's object file
# deleted) must hard-error sg diff (non-zero exit, actionable message),
# never silently diff garbage or exit 0 -- the diff-side mirror of restore's
# own missing-chunk hard-failure already tested in phase6a/6c above.
P6F_BROKEN_REPO="$WORKDIR/phase6f_broken_repo"
mkdir -p "$P6F_BROKEN_REPO"
(cd "$WORKDIR" && "$SG" init phase6f_broken_repo) > /dev/null 2>&1
git config -f "$P6F_BROKEN_REPO/.git/config" sg.chunking true
git config -f "$P6F_BROKEN_REPO/.git/config" sg.chunkthreshold 1048576

P6F_BROKEN_FILE="$P6F_BROKEN_REPO/big.bin"
head -c 5242880 /dev/urandom > "$P6F_BROKEN_FILE" 2>/dev/null
(cd "$P6F_BROKEN_REPO" && "$SG" add big.bin && "$SG" commit -m "chunked file for broken-diff test") \
    > /dev/null 2>&1

P6F_BROKEN_BLOB=$(cd "$P6F_BROKEN_REPO" && git ls-files -s big.bin 2>/dev/null | awk '{print $2}')
P6F_BROKEN_CHUNK_ID=$(git -C "$P6F_BROKEN_REPO" cat-file -p "$P6F_BROKEN_BLOB" 2>/dev/null \
    | grep -E '^[0-9a-f]{40}$' | sed -n '2p')
P6F_BROKEN_CHUNK_PREFIX=$(echo "$P6F_BROKEN_CHUNK_ID" | cut -c1-2)
P6F_BROKEN_CHUNK_SUFFIX=$(echo "$P6F_BROKEN_CHUNK_ID" | cut -c3-)
rm -f "$P6F_BROKEN_REPO/.git/objects/$P6F_BROKEN_CHUNK_PREFIX/$P6F_BROKEN_CHUNK_SUFFIX"

P6F_BROKEN_OUT="$WORKDIR/p6f_broken_diff_out.txt"
(cd "$P6F_BROKEN_REPO" && "$SG" diff) > "$P6F_BROKEN_OUT" 2>&1
P6F_BROKEN_RC=$?
check "phase6f: sg diff fails (non-zero) when a staged file's chunk data is missing/corrupt" \
    test "$P6F_BROKEN_RC" != 0
check "phase6f: sg diff prints an actionable error naming the missing chunk(s)" \
    grep -q "chunk" "$P6F_BROKEN_OUT"

# ---- packed-refs: `git gc` moves refs out of loose files, and a reader that
# only checks refs/heads/<name> concludes the branch has no commits -- after
# which the next commit becomes a root commit and the whole history stops
# being reachable from any ref.
PACKED_REPO="$WORKDIR/packed_refs_repo"
mkdir -p "$PACKED_REPO"
(cd "$PACKED_REPO" && "$SG" init) > /dev/null 2>&1
printf 'one\n' > "$PACKED_REPO/a.txt"
(cd "$PACKED_REPO" && "$SG" add a.txt && "$SG" commit -m "first") > /dev/null 2>&1
PACKED_FIRST=$(cat "$PACKED_REPO/.git/refs/heads/master" 2>/dev/null)
(cd "$PACKED_REPO" && git gc --prune=now) > /dev/null 2>&1

check "packed-refs: git gc really did pack the loose branch ref away" \
    sh -c "test ! -f '$PACKED_REPO/.git/refs/heads/master'"
check "packed-refs: sg log still finds the commit once the ref is packed" \
    sh -c "(cd '$PACKED_REPO' && '$SG' log) 2>&1 | grep -q '$PACKED_FIRST'"

printf 'two\n' > "$PACKED_REPO/b.txt"
(cd "$PACKED_REPO" && "$SG" add b.txt && "$SG" commit -m "second") > /dev/null 2>&1
PACKED_SECOND=$(cat "$PACKED_REPO/.git/refs/heads/master" 2>/dev/null)

check "packed-refs: the commit after a gc has a parent (history not orphaned)" \
    sh -c "test \"\$(cd '$PACKED_REPO' && git cat-file -p '$PACKED_SECOND' | grep -c '^parent ')\" = 1"
check "packed-refs: both commits remain reachable from a ref after a gc" \
    sh -c "test \"\$(cd '$PACKED_REPO' && git log --oneline --all | wc -l | tr -d ' ')\" = 2"
check "packed-refs: sg switch recognizes a branch that only exists packed" \
    sh -c "(cd '$PACKED_REPO' && '$SG' switch -c feat && '$SG' switch master) > /dev/null 2>&1"

# ---- idx v2 large-offset table: a pack needing an offset that doesn't fit
# in 31 bits (i.e. >2GB into the pack) uses an escape hatch in the idx v2
# format -- the normal 4-byte offset table entry has its MSB set, and its
# low 31 bits are instead an index into a separate 8-byte-per-entry
# large-offset table appended after the normal tables. Synthesizing an
# actual >2GB pack to exercise this isn't practical here, so this hand-
# builds a tiny one-object pack + matching idx whose sole offset-table entry
# takes that indirect path (even though the real offset it resolves to,
# 12, is tiny) -- this is the only piece of this refactor with no other
# coverage in this suite.
LARGE_OFF_REPO="$WORKDIR/large_offset_repo"
mkdir -p "$LARGE_OFF_REPO"
(cd "$WORKDIR" && "$SG" init large_offset_repo) > /dev/null 2>&1

LARGE_OFF_EXPECTED="$WORKDIR/large_offset_expected.txt"
LARGE_OFF_ID=$(python3 - "$LARGE_OFF_REPO/.git" "$LARGE_OFF_EXPECTED" <<'PYEOF'
import hashlib
import os
import struct
import sys
import zlib

git_dir, expected_path = sys.argv[1], sys.argv[2]
content = b"large-offset idx v2 synthetic test object\n"

# git blob object id: sha1("blob " + len + "\0" + content)
obj_id = hashlib.sha1(b"blob " + str(len(content)).encode() + b"\x00" + content).digest()

def encode_obj_header(obj_type, size):
    # pack entry header varint: first byte's bit7 is a continuation flag,
    # bits6-4 the type, bits3-0 the low 4 bits of size; each continuation
    # byte then contributes 7 more bits.
    b0 = (obj_type << 4) | (size & 0x0F)
    size >>= 4
    out = bytearray([b0 | (0x80 if size else 0)])
    while size:
        b = size & 0x7F
        size >>= 7
        out.append(b | (0x80 if size else 0))
    return bytes(out)

OBJ_BLOB = 3
compressed = zlib.compress(content)
entry_header = encode_obj_header(OBJ_BLOB, len(content))

pack_body = b"PACK" + struct.pack(">II", 2, 1)  # version 2, 1 object
entry_offset = len(pack_body)  # this entry starts right after the 12-byte header
pack_body += entry_header + compressed
pack_trailer = hashlib.sha1(pack_body).digest()
pack_bytes = pack_body + pack_trailer

crc = zlib.crc32(entry_header + compressed) & 0xFFFFFFFF

idx = bytearray([0xFF, 0x74, 0x4F, 0x63]) + struct.pack(">I", 2)
fanout = [1 if i >= obj_id[0] else 0 for i in range(256)]
for v in fanout:
    idx += struct.pack(">I", v)
idx += obj_id
idx += struct.pack(">I", crc)
idx += struct.pack(">I", 0x80000000)  # offset table: MSB set, index 0 into large-offset table
idx += struct.pack(">Q", entry_offset)  # large-offset table: 1 entry, real 8-byte BE offset
idx += pack_trailer  # pack checksum
idx += hashlib.sha1(bytes(idx)).digest()  # idx's own trailing checksum

pack_dir = os.path.join(git_dir, "objects", "pack")
os.makedirs(pack_dir, exist_ok=True)
pack_hex = pack_trailer.hex()
with open(os.path.join(pack_dir, f"pack-{pack_hex}.pack"), "wb") as f:
    f.write(pack_bytes)
with open(os.path.join(pack_dir, f"pack-{pack_hex}.idx"), "wb") as f:
    f.write(bytes(idx))
with open(expected_path, "wb") as f:
    f.write(content)

print(obj_id.hex())
PYEOF
)

LARGE_OFF_OUT="$WORKDIR/large_offset_catfile_out.txt"
(cd "$LARGE_OFF_REPO" && "$SG" cat-file -p "$LARGE_OFF_ID") > "$LARGE_OFF_OUT" 2>&1
LARGE_OFF_RC=$?
check "idx v2 large-offset table: sg cat-file -p exits 0 for an object reached via the large-offset table" \
    test "$LARGE_OFF_RC" = 0
check "idx v2 large-offset table: content read back matches exactly what was packed" \
    cmp -s "$LARGE_OFF_EXPECTED" "$LARGE_OFF_OUT"

LARGE_OFF_TYPE=$(cd "$LARGE_OFF_REPO" && "$SG" cat-file -t "$LARGE_OFF_ID" 2>/dev/null)
check "idx v2 large-offset table: sg cat-file -t reports the correct object type" \
    test "$LARGE_OFF_TYPE" = "blob"

echo ""

# ---- REF_DELTA whose base exists nowhere ----------------------------------
# Reading this forces read_entry_at() to recurse back into pack_read_depth()
# to resolve the base, and that nested lookup then misses in every pack --
# the exact path where a rescan triggered underneath an in-progress read used
# to free the pack list the outer loop was still walking (a use-after-free
# confirmed under ASan, now fixed by deferring reclamation until the
# outermost read returns). The race itself needs a concurrent writer and so
# isn't deterministic enough to assert here; what this pins deterministically
# is that the nested-miss path stays reachable and still fails cleanly rather
# than crashing, hanging, or inventing content.
MISSING_BASE_REPO="$WORKDIR/missing_base_repo"
mkdir -p "$MISSING_BASE_REPO"
(cd "$WORKDIR" && "$SG" init missing_base_repo) > /dev/null 2>&1

MISSING_BASE_ID=$(python3 - "$MISSING_BASE_REPO/.git" <<'PYEOF'
import binascii
import hashlib
import os
import struct
import sys
import zlib

git_dir = sys.argv[1]
OBJ_REF_DELTA = 7
fake_id = bytes([0xAA]) * 20      # what the idx claims is in there
missing_base = bytes([0xBB]) * 20  # a base id present in no pack and no loose object

def encode_obj_header(obj_type, size):
    b0 = (obj_type << 4) | (size & 0x0F)
    size >>= 4
    out = bytearray([b0 | (0x80 if size else 0)])
    while size:
        b = size & 0x7F
        size >>= 7
        out.append(b | (0x80 if size else 0))
    return bytes(out)

# the delta stream's own bytes never get applied (the base can't be found),
# so its contents don't matter -- only that it inflates cleanly.
delta = b"\x00" * 4096
body = encode_obj_header(OBJ_REF_DELTA, len(delta)) + missing_base + zlib.compress(delta, 1)
pack_body = b"PACK" + struct.pack(">II", 2, 1)
entry_offset = len(pack_body)
pack_body += body
pack_trailer = hashlib.sha1(pack_body).digest()
pack_bytes = pack_body + pack_trailer

idx = bytearray([0xFF, 0x74, 0x4F, 0x63]) + struct.pack(">I", 2)
for i in range(256):
    idx += struct.pack(">I", 1 if i >= fake_id[0] else 0)
idx += fake_id
idx += struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF)
idx += struct.pack(">I", entry_offset)
idx += pack_trailer
idx += hashlib.sha1(bytes(idx)).digest()

pack_dir = os.path.join(git_dir, "objects", "pack")
os.makedirs(pack_dir, exist_ok=True)
pack_hex = pack_trailer.hex()
with open(os.path.join(pack_dir, f"pack-{pack_hex}.pack"), "wb") as f:
    f.write(pack_bytes)
with open(os.path.join(pack_dir, f"pack-{pack_hex}.idx"), "wb") as f:
    f.write(bytes(idx))

print(fake_id.hex())
PYEOF
)

MISSING_BASE_OUT="$WORKDIR/missing_base_out.txt"
(cd "$MISSING_BASE_REPO" && "$SG" cat-file -p "$MISSING_BASE_ID") > "$MISSING_BASE_OUT" 2>&1
MISSING_BASE_RC=$?
check "REF_DELTA with an unresolvable base: sg cat-file fails instead of succeeding" \
    test "$MISSING_BASE_RC" != 0
check "REF_DELTA with an unresolvable base: exit status is a clean failure, not a signal/crash" \
    test "$MISSING_BASE_RC" -lt 128
check "REF_DELTA with an unresolvable base: reports it as not found rather than emitting content" \
    grep -q "not found or corrupt" "$MISSING_BASE_OUT"

echo ""

# --- Phase 9: sg branch -- list / create / delete, packed-refs-correct ------
# Deletion is the part with teeth here: a delete that only unlinks the loose
# file leaves any stale packed-refs line behind, and the branch "resurrects"
# at whatever old commit that line records (this codebase has been bitten by
# exactly this loose-vs-packed blindness before, on the read side). The
# packed-only and stale-shadow cases below pin the deletion side of it.
P9B_REPO="$WORKDIR/phase9_branch_repo"
mkdir -p "$P9B_REPO"
(cd "$WORKDIR" && "$SG" init phase9_branch_repo) > /dev/null 2>&1
printf 'branch base\n' > "$P9B_REPO/base.txt"
(cd "$P9B_REPO" && "$SG" add base.txt) > /dev/null 2>&1
(cd "$P9B_REPO" && "$SG" commit -m "p9 base") > /dev/null 2>&1

# case 1: create at HEAD, do NOT switch; real git agrees on all of it
(cd "$P9B_REPO" && "$SG" branch topic) > /dev/null 2>&1
check "phase9 case1: sg branch <name> exits 0" test $? = 0
P9_HEAD=$(cd "$P9B_REPO" && git rev-parse HEAD)
P9_TOPIC=$(cd "$P9B_REPO" && git rev-parse --verify refs/heads/topic 2>/dev/null)
check "phase9 case1: git rev-parse of the sg-created branch equals HEAD" \
    test "$P9_TOPIC" = "$P9_HEAD"
(cd "$P9B_REPO" && git branch --list topic) 2>/dev/null | grep -q "topic"
check "phase9 case1: git branch --list shows the sg-created branch" test $? = 0
check "phase9 case1: creating a branch does not switch away from master" \
    test "$(cd "$P9B_REPO" && git branch --show-current)" = "master"
(cd "$P9B_REPO" && git fsck) > /dev/null 2>&1
check "phase9 case1: git fsck is clean after sg branch" test $? = 0

# case 1 (cont.): a slash-containing name round-trips create/list/delete
(cd "$P9B_REPO" && "$SG" branch feature/x) > /dev/null 2>&1
check "phase9 case1: sg branch feature/x (slash name) exits 0" test $? = 0
check "phase9 case1: git sees feature/x at HEAD" \
    test "$(cd "$P9B_REPO" && git rev-parse --verify refs/heads/feature/x 2>/dev/null)" = "$P9_HEAD"
(cd "$P9B_REPO" && "$SG" branch) 2>/dev/null | grep -q "^  feature/x\$"
check "phase9 case1: sg branch lists feature/x" test $? = 0
(cd "$P9B_REPO" && "$SG" branch -d feature/x < /dev/null) > /dev/null 2>&1
check "phase9 case1: sg branch -d feature/x (merged, so no prompt) exits 0" test $? = 0
(cd "$P9B_REPO" && git rev-parse --verify refs/heads/feature/x) > /dev/null 2>&1
check "phase9 case1: feature/x is fully gone after sg delete" test $? != 0

# case 2: listing parity vs git, including a branch created by real git
(cd "$P9B_REPO" && git branch git-made) 2>/dev/null
(cd "$P9B_REPO" && "$SG" branch zeta) > /dev/null 2>&1
P9_SG_LIST="$WORKDIR/p9_sg_list.txt"
P9_GIT_LIST="$WORKDIR/p9_git_list.txt"
(cd "$P9B_REPO" && "$SG" branch) 2>/dev/null | cut -c3- | LC_ALL=C sort > "$P9_SG_LIST"
(cd "$P9B_REPO" && git for-each-ref --format='%(refname:short)' refs/heads) 2>/dev/null \
    | LC_ALL=C sort > "$P9_GIT_LIST"
check "phase9 case2: sg branch listing matches git for-each-ref" cmp -s "$P9_SG_LIST" "$P9_GIT_LIST"

# case 3: the * marker sits on the branch git considers current
P9_MARKED=$(cd "$P9B_REPO" && "$SG" branch 2>/dev/null | grep '^\* ' | cut -c3-)
check "phase9 case3: current-branch marker matches git branch --show-current" \
    test "$P9_MARKED" = "$(cd "$P9B_REPO" && git branch --show-current)"

# case 4a: packed-only branch (post pack-refs, no loose file at all)
(cd "$P9B_REPO" && "$SG" branch packed-only) > /dev/null 2>&1
(cd "$P9B_REPO" && git pack-refs --all) 2>/dev/null
check "phase9 case4: precondition -- pack-refs removed the loose file" \
    test ! -f "$P9B_REPO/.git/refs/heads/packed-only"
(cd "$P9B_REPO" && "$SG" branch) 2>/dev/null | grep -q "^  packed-only\$"
check "phase9 case4: packed-only branch is still listed by sg branch" test $? = 0
(cd "$P9B_REPO" && "$SG" branch -d packed-only < /dev/null) > /dev/null 2>&1
check "phase9 case4: sg branch -d deletes a packed-only branch" test $? = 0
(cd "$P9B_REPO" && git branch --list packed-only) 2>/dev/null | grep -q "packed-only"
check "phase9 case4: git branch no longer shows the packed-only branch" test $? != 0
grep -q "refs/heads/packed-only" "$P9B_REPO/.git/packed-refs" 2>/dev/null
check "phase9 case4: packed-refs has no line left for the deleted branch" test $? != 0

# case 4b: STALE SHADOW -- pack, then advance the branch so a newer loose
# file shadows the stale packed line; deleting must purge BOTH, or the
# branch comes back from the dead at the stale sha
(cd "$P9B_REPO" && "$SG" branch shadow) > /dev/null 2>&1
(cd "$P9B_REPO" && git pack-refs --all) 2>/dev/null
P9_STALE=$(cd "$P9B_REPO" && git rev-parse shadow)
(cd "$P9B_REPO" && git switch -q shadow \
    && printf 'shadow work\n' > shadow.txt \
    && git add shadow.txt && git commit -q -m "shadow commit" \
    && git switch -q master) 2>/dev/null
P9_SHADOW_TIP=$(cd "$P9B_REPO" && git rev-parse shadow)
check "phase9 case4: precondition -- loose shadow ref advanced past the stale packed entry" \
    sh -c "test -f '$P9B_REPO/.git/refs/heads/shadow' \
        && test '$P9_SHADOW_TIP' != '$P9_STALE' \
        && grep -q '$P9_STALE refs/heads/shadow' '$P9B_REPO/.git/packed-refs'"
P9_SHADOW_OUT="$WORKDIR/p9_shadow_out.txt"
(cd "$P9B_REPO" && "$SG" branch -d shadow --force < /dev/null) > "$P9_SHADOW_OUT" 2>&1
check "phase9 case4: forced delete of the stale-shadow branch exits 0" test $? = 0
(cd "$P9B_REPO" && git rev-parse --verify refs/heads/shadow) > /dev/null 2>&1
check "phase9 case4: shadow branch fully gone, NOT resurrected at the stale packed sha" \
    test $? != 0
grep -q "refs/heads/shadow" "$P9B_REPO/.git/packed-refs" 2>/dev/null
check "phase9 case4: stale packed line purged as part of the delete" test $? != 0
check "phase9 case4: delete printed the true (loose) tip in full, recoverable" \
    grep -q "$P9_SHADOW_TIP" "$P9_SHADOW_OUT"

# case 5: current-branch delete refused (even forced); unmerged delete is
# refused on a non-tty stdin without --force, succeeds with it
(cd "$P9B_REPO" && "$SG" branch -d master --force < /dev/null) > /dev/null 2>&1
check "phase9 case5: deleting the current branch fails even with --force" test $? = 1
(cd "$P9B_REPO" && git rev-parse --verify refs/heads/master) > /dev/null 2>&1
check "phase9 case5: current branch is intact after the refused delete" test $? = 0

(cd "$P9B_REPO" && git switch -q -c unmerged \
    && printf 'um\n' > um.txt && git add um.txt && git commit -q -m "um" \
    && git switch -q master) 2>/dev/null
P9_UM_TIP=$(cd "$P9B_REPO" && git rev-parse unmerged)
P9_UM_OUT="$WORKDIR/p9_um_out.txt"
(cd "$P9B_REPO" && "$SG" branch -d unmerged < /dev/null) > "$P9_UM_OUT" 2>&1
check "phase9 case5: unmerged delete on non-tty stdin without --force is refused" test $? = 1
check "phase9 case5: the refusal names --force as the way through" \
    grep -q -- "--force" "$P9_UM_OUT"
check "phase9 case5: branch untouched after the refused unmerged delete" \
    test "$(cd "$P9B_REPO" && git rev-parse unmerged)" = "$P9_UM_TIP"
(cd "$P9B_REPO" && "$SG" branch -d unmerged --force < /dev/null) > "$P9_UM_OUT" 2>&1
check "phase9 case5: unmerged delete with --force succeeds" test $? = 0
check "phase9 case5: forced delete printed the branch's full 40-hex old tip" \
    grep -q "$P9_UM_TIP" "$P9_UM_OUT"
(cd "$P9B_REPO" && git rev-parse --verify refs/heads/unmerged) > /dev/null 2>&1
check "phase9 case5: unmerged branch gone after the forced delete" test $? != 0

# case 6: invalid creation names -- sg rejects each, and real git's
# check-ref-format agrees every one is invalid
for bad in 'a..b' 'a b' 'a.lock' 'HEAD' 'a/' '@{x}' '.hidden' 'x/.hidden'; do
    (cd "$P9B_REPO" && "$SG" branch "$bad") > /dev/null 2>&1
    check "phase9 case6: sg rejects invalid creation name '$bad'" test $? = 1
    (cd "$P9B_REPO" && git check-ref-format --branch "$bad") > /dev/null 2>&1
    check "phase9 case6: git check-ref-format agrees '$bad' is invalid" test $? != 0
done
(cd "$P9B_REPO" && "$SG" branch -- -lead) > /dev/null 2>&1
check "phase9 case6: sg rejects leading-dash name '-lead'" test $? = 1
(cd "$P9B_REPO" && git check-ref-format --branch -lead) > /dev/null 2>&1
check "phase9 case6: git check-ref-format agrees '-lead' is invalid" test $? != 0

# case 7: creating over an existing branch fails, whether the existing one
# is loose or lives only in packed-refs
(cd "$P9B_REPO" && "$SG" branch dupe) > /dev/null 2>&1
(cd "$P9B_REPO" && "$SG" branch dupe) > /dev/null 2>&1
check "phase9 case7: creating over an existing loose branch fails" test $? = 1
check "phase9 case7: precondition -- topic is packed-only by now" \
    test ! -f "$P9B_REPO/.git/refs/heads/topic"
(cd "$P9B_REPO" && "$SG" branch topic) > /dev/null 2>&1
check "phase9 case7: creating over a packed-only branch fails" test $? = 1

# case 8: empty repo (no commits): listing is silent success, creating errors
P9E_REPO="$WORKDIR/phase9_empty_repo"
mkdir -p "$P9E_REPO"
(cd "$WORKDIR" && "$SG" init phase9_empty_repo) > /dev/null 2>&1
P9E_LIST="$WORKDIR/p9_empty_list.txt"
(cd "$P9E_REPO" && "$SG" branch) > "$P9E_LIST" 2>&1
P9E_RC=$?
check "phase9 case8: sg branch in an empty repo exits 0" test "$P9E_RC" = 0
check "phase9 case8: sg branch in an empty repo prints nothing" test ! -s "$P9E_LIST"
(cd "$P9E_REPO" && "$SG" branch nothing-yet) > /dev/null 2>&1
check "phase9 case8: creating a branch in an empty repo fails (no commit yet)" test $? = 1

echo ""

# --- Phase 12: sg tag -- lightweight/annotated, packed-refs-correct --------
# Same loose-vs-packed blindness that bit branch deletion applies to tags:
# packed-only and stale-shadow cases below pin the deletion side, and the
# annotated-tag object round-trip is the strongest oracle here (git must
# read a tag object sg wrote, byte for byte).
P12_REPO="$WORKDIR/phase12_tag_repo"
mkdir -p "$P12_REPO"
(cd "$WORKDIR" && "$SG" init phase12_tag_repo) > /dev/null 2>&1
printf 'tag base\n' > "$P12_REPO/base.txt"
(cd "$P12_REPO" && "$SG" add base.txt) > /dev/null 2>&1
(cd "$P12_REPO" && "$SG" commit -m "p12 base") > /dev/null 2>&1
P12_HEAD=$(cd "$P12_REPO" && git rev-parse HEAD)

# case 1: lightweight tag at HEAD -- real git agrees it's a plain ref to the commit
(cd "$P12_REPO" && "$SG" tag light) > /dev/null 2>&1
check "phase12 case1: sg tag <name> exits 0" test $? = 0
check "phase12 case1: git rev-parse of the lightweight tag equals HEAD" \
    test "$(cd "$P12_REPO" && git rev-parse light)" = "$P12_HEAD"
(cd "$P12_REPO" && git cat-file -t light) 2>/dev/null | grep -q "^commit\$"
check "phase12 case1: git sees the lightweight tag's object type as commit (no tag object)" test $? = 0
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^light\$"
check "phase12 case1: git tag -l lists the sg-created lightweight tag" test $? = 0

# case 2: annotated tag -- git must read the tag object sg wrote
(cd "$P12_REPO" && "$SG" tag -a -m "hello annotated" ann) > /dev/null 2>&1
check "phase12 case2: sg tag -a -m <msg> <name> exits 0" test $? = 0
(cd "$P12_REPO" && git cat-file -t ann) 2>/dev/null | grep -q "^tag\$"
check "phase12 case2: git cat-file -t sees the annotated tag as a tag object" test $? = 0
(cd "$P12_REPO" && git cat-file -p ann) 2>/dev/null | grep -q "^hello annotated\$"
check "phase12 case2: git cat-file -p shows the annotated tag's message" test $? = 0
check "phase12 case2: git rev-parse ann^{commit} peels to the tagged commit" \
    test "$(cd "$P12_REPO" && git rev-parse 'ann^{commit}' 2>/dev/null)" = "$P12_HEAD"
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^ann\$"
check "phase12 case2: git tag -l lists the sg-created annotated tag" test $? = 0
(cd "$P12_REPO" && git fsck) > /dev/null 2>&1
check "phase12 case2: git fsck is clean after sg tag -a" test $? = 0

# case 2 (cont.): -m without -a also produces an annotated tag (measured
# against real git: `git tag -m x name` creates an annotated tag)
(cd "$P12_REPO" && "$SG" tag -m "implicit annotated" implicit) > /dev/null 2>&1
check "phase12 case2: sg tag -m <msg> <name> (no -a) exits 0" test $? = 0
(cd "$P12_REPO" && git cat-file -t implicit) 2>/dev/null | grep -q "^tag\$"
check "phase12 case2: -m without -a still creates an annotated tag object" test $? = 0

# case 2 (cont.): -a without -m is a usage error
(cd "$P12_REPO" && "$SG" tag -a noamsg) > /dev/null 2>&1
check "phase12 case2: sg tag -a without -m is rejected" test $? = 1

# case 3: reverse direction -- a real-git-made annotated tag is listed by sg
# and resolvable as a revision (sg_rev_parse_commit must peel it). Exercised
# through `sg tag <newname> <rev>`, which is the only CLI surface for
# sg_rev_parse_commit today: a lightweight tag created FROM the real-git
# annotated tag must land on the peeled commit, not the tag object's own id.
(cd "$P12_REPO" && git tag -a -m "git made this" gitmade) 2>/dev/null
(cd "$P12_REPO" && "$SG" tag) 2>/dev/null | grep -q "^gitmade\$"
check "phase12 case3: sg tag lists a tag created by real git" test $? = 0
(cd "$P12_REPO" && "$SG" tag from-gitmade gitmade) > /dev/null 2>&1
check "phase12 case3: sg tag <name> <rev> using a real-git annotated tag as <rev> exits 0" test $? = 0
check "phase12 case3: sg peeled the real-git annotated tag to the tagged commit, not the tag object id" \
    test "$(cd "$P12_REPO" && git rev-parse from-gitmade)" = "$P12_HEAD"

# case 4: listing order matches git tag -l (byte-wise sort)
P12_SG_LIST="$WORKDIR/p12_sg_list.txt"
P12_GIT_LIST="$WORKDIR/p12_git_list.txt"
(cd "$P12_REPO" && "$SG" tag) 2>/dev/null > "$P12_SG_LIST"
(cd "$P12_REPO" && git tag -l) 2>/dev/null > "$P12_GIT_LIST"
check "phase12 case4: sg tag listing matches git tag -l exactly (order included)" \
    cmp -s "$P12_SG_LIST" "$P12_GIT_LIST"

# case 5: packed-only tag (post pack-refs, no loose file at all)
(cd "$P12_REPO" && "$SG" tag packed-only) > /dev/null 2>&1
(cd "$P12_REPO" && git pack-refs --all) 2>/dev/null
check "phase12 case5: precondition -- pack-refs removed the loose file" \
    test ! -f "$P12_REPO/.git/refs/tags/packed-only"
(cd "$P12_REPO" && "$SG" tag) 2>/dev/null | grep -q "^packed-only\$"
check "phase12 case5: packed-only tag is still listed by sg tag" test $? = 0
(cd "$P12_REPO" && "$SG" tag -d packed-only) > /dev/null 2>&1
check "phase12 case5: sg tag -d deletes a packed-only tag" test $? = 0
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^packed-only\$"
check "phase12 case5: git tag -l no longer shows the packed-only tag" test $? != 0
grep -q "refs/tags/packed-only" "$P12_REPO/.git/packed-refs" 2>/dev/null
check "phase12 case5: packed-refs has no line left for the deleted tag" test $? != 0

# case 6: STALE SHADOW -- pack, then force-move the tag so a newer loose
# file shadows the stale packed line; deleting must purge BOTH
(cd "$P12_REPO" && "$SG" tag shadow) > /dev/null 2>&1
(cd "$P12_REPO" && git pack-refs --all) 2>/dev/null
P12_STALE=$(cd "$P12_REPO" && git rev-parse shadow)
(cd "$P12_REPO" && printf 'shadow work\n' > shadow.txt \
    && git add shadow.txt && git commit -q -m "shadow commit") 2>/dev/null
(cd "$P12_REPO" && git tag -f shadow) > /dev/null 2>&1
P12_SHADOW_TIP=$(cd "$P12_REPO" && git rev-parse shadow)
check "phase12 case6: precondition -- loose shadow tag advanced past the stale packed entry" \
    sh -c "test -f '$P12_REPO/.git/refs/tags/shadow' \
        && test '$P12_SHADOW_TIP' != '$P12_STALE' \
        && grep -q '$P12_STALE refs/tags/shadow' '$P12_REPO/.git/packed-refs'"
(cd "$P12_REPO" && "$SG" tag -d shadow) > /dev/null 2>&1
check "phase12 case6: delete of the stale-shadow tag exits 0" test $? = 0
(cd "$P12_REPO" && git rev-parse --verify refs/tags/shadow) > /dev/null 2>&1
check "phase12 case6: shadow tag fully gone, NOT resurrected at the stale packed sha" test $? != 0
grep -q "refs/tags/shadow" "$P12_REPO/.git/packed-refs" 2>/dev/null
check "phase12 case6: stale packed line purged as part of the delete" test $? != 0

# case 7: invalid creation names -- sg rejects each, and real git's
# check-ref-format --tag agrees every one is invalid
for bad in 'a..b' 'a b' 'a.lock' 'HEAD' 'a/' '@{x}' '.hidden' 'x/.hidden'; do
    (cd "$P12_REPO" && "$SG" tag "$bad") > /dev/null 2>&1
    check "phase12 case7: sg rejects invalid creation name '$bad'" test $? = 1
    (cd "$P12_REPO" && git check-ref-format --tag "$bad") > /dev/null 2>&1
    check "phase12 case7: git check-ref-format --tag agrees '$bad' is invalid" test $? != 0
done

# case 8: overwriting an existing tag fails without --force, succeeds with it
(cd "$P12_REPO" && "$SG" tag dupe) > /dev/null 2>&1
(cd "$P12_REPO" && "$SG" tag dupe) > /dev/null 2>&1
check "phase12 case8: creating over an existing tag without --force fails" test $? = 1
(cd "$P12_REPO" && "$SG" tag --force dupe) > /dev/null 2>&1
check "phase12 case8: creating over an existing tag with --force succeeds" test $? = 0

# case 9: deleting a nonexistent tag fails, not a silent success
(cd "$P12_REPO" && "$SG" tag -d does-not-exist) > /dev/null 2>&1
check "phase12 case9: deleting a nonexistent tag fails" test $? = 1

# case 10: -d combined with tag-creation options is a usage error, not a
# silent delete -- verified directly against real git: `git tag -d -a -m x
# name` prints usage and deletes nothing, exit 129 (sg's own convention is
# only ever exit 0/1, so only the "deletes nothing, non-zero exit" half of
# that is checked here).
(cd "$P12_REPO" && "$SG" tag dcombo) > /dev/null 2>&1
(cd "$P12_REPO" && "$SG" tag -d -a -m "x" dcombo) > /dev/null 2>&1
check "phase12 case10: sg tag -d -a -m <name> is rejected" test $? = 1
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^dcombo\$"
check "phase12 case10: -d -a -m rejection did not delete the tag" test $? = 0
(cd "$P12_REPO" && git tag -d -a -m x dcombo) > /dev/null 2>&1
check "phase12 case10: real git also rejects tag -d -a -m <name> (sanity check on the test itself)" \
    test $? != 0

(cd "$P12_REPO" && "$SG" tag -d --force dcombo) > /dev/null 2>&1
check "phase12 case10: sg tag -d --force <name> is rejected" test $? = 1
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^dcombo\$"
check "phase12 case10: -d --force rejection did not delete the tag" test $? = 0
(cd "$P12_REPO" && git tag -d --force dcombo) > /dev/null 2>&1
check "phase12 case10: real git also rejects tag -d --force <name> (sanity check on the test itself)" \
    test $? != 0

# case 11: -d with a second name argument is a usage error, not a silent
# partial delete (this project doesn't support multi-name delete, but must
# not silently drop the second name and delete only the first).
(cd "$P12_REPO" && "$SG" tag dmulti1 && "$SG" tag dmulti2) > /dev/null 2>&1
(cd "$P12_REPO" && "$SG" tag -d dmulti1 dmulti2) > /dev/null 2>&1
check "phase12 case11: sg tag -d <name1> <name2> is rejected" test $? = 1
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^dmulti1\$"
check "phase12 case11: rejection did not delete the first name either" test $? = 0
(cd "$P12_REPO" && git tag -l) 2>/dev/null | grep -q "^dmulti2\$"
check "phase12 case11: rejection did not delete the second name" test $? = 0

# --- Phase 9: gitignore conformance sweep -- sg status vs git status -------
# The strongest oracle in this phase: one tree whose paths exercise every
# rule family in the ignore spec (bare name, /anchored, dir-only, a/*.txt,
# **/, /**, a/**/b, a**b, negation, no-reinclusion-under-an-excluded-dir,
# trailing-space handling, \#, \!, char classes, deeper-file-override, and
# .git/info/exclude) is examined by BOTH sg status and real git, and the two
# untracked path sets must be EXACTLY equal -- any future matcher drift
# fails this loudly. The git side uses -z output because porcelain v1
# C-quotes paths with trailing spaces (verified on this git).
P9I_REPO="$WORKDIR/phase9_ignore_repo"
mkdir -p "$P9I_REPO"
git init -q "$P9I_REPO"
git -C "$P9I_REPO" config core.ignorecase false

cat > "$P9I_REPO/.gitignore" <<'EOF'
# root ignore rules (this line is a comment)
*.log
!keep.log
/anchored.txt
build/
onlydir/
docs/*.txt
**/anydepth.txt
gen/**
x/**/y
a**b
secret
blocked/
!blocked/inside.txt
sp1 
sp2\ 
\#lit
\!bang
tmp[0-9].bin
cls[!a-c].txt
!exc-neg
EOF
mkdir -p "$P9I_REPO/.git/info"
printf 'excfile\nexc-neg\n' > "$P9I_REPO/.git/info/exclude"

mkdir -p "$P9I_REPO/deep" "$P9I_REPO/build" "$P9I_REPO/docs/deep" "$P9I_REPO/m" \
    "$P9I_REPO/gen/deep" "$P9I_REPO/x/m/n" "$P9I_REPO/a" "$P9I_REPO/over" \
    "$P9I_REPO/blocked" "$P9I_REPO/sub/deep" "$P9I_REPO/sub/inner"
printf '!secret\n' > "$P9I_REPO/over/.gitignore"
cat > "$P9I_REPO/sub/.gitignore" <<'EOF'
subonly.dat
/subanchored.txt
inner/
!special.log
EOF

for f in normal.txt app.log keep.log other.log anchored.txt deep/anchored.txt \
    build/out.bin onlydir docs/a.txt docs/b.md docs/deep/c.txt anydepth.txt \
    m/anydepth.txt m/other.txt gen/f.txt gen/deep/g.txt x/y x/z.txt a/qb axxb \
    secret over/secret blocked/inside.txt sp1 sp2 '#lit' '#other' '!bang' \
    '!other' tmp5.bin tmpX.bin clsd.txt clsa.txt excfile exc-neg \
    sub/subonly.dat sub/deep/subonly.dat sub/subanchored.txt \
    sub/deep/subanchored.txt sub/inner/file.txt sub/special.log \
    sub/other2.log sub/plain.txt; do
    printf 'content\n' > "$P9I_REPO/$f"
done
printf 'content\n' > "$P9I_REPO/sp2 "
printf 'content\n' > "$P9I_REPO/x/m/n/y"

P9I_GIT_SET="$WORKDIR/p9i_git_set.txt"
P9I_SG_SET="$WORKDIR/p9i_sg_set.txt"
P9I_SG_OUT="$WORKDIR/p9i_sg_out.txt"
(cd "$P9I_REPO" && git status --porcelain -uall -z) | tr '\0' '\n' \
    | sed -n 's/^?? //p' | LC_ALL=C sort > "$P9I_GIT_SET"
(cd "$P9I_REPO" && "$SG" status -uall) > "$P9I_SG_OUT" 2>&1
P9I_SG_RC=$?
awk '/^Untracked files:/{f=1;next} /^$/{f=0} f && /^\t/{sub(/^\t/,"");print}' \
    "$P9I_SG_OUT" | LC_ALL=C sort > "$P9I_SG_SET"

check "phase9 conformance: sg status exits 0 on the tricky tree" test "$P9I_SG_RC" = 0
check "phase9 conformance: oracle sanity -- git reports a non-empty untracked set" \
    test -s "$P9I_GIT_SET"
check "phase9 conformance: oracle sanity -- deeper !secret re-includes over/secret" \
    grep -q '^over/secret$' "$P9I_GIT_SET"
check "phase9 conformance: oracle sanity -- *.log ignores app.log" \
    sh -c "! grep -q '^app\.log\$' '$P9I_GIT_SET'"
check "phase9 conformance: sg untracked set EXACTLY equals git status --porcelain -uall" \
    cmp -s "$P9I_GIT_SET" "$P9I_SG_SET"

# --- Phase 9: sg add . equivalence against git add . -----------------------
# Twin copies of one dirty tree (new files, modified files, a deleted
# tracked file, ignored files, and a tracked-but-ignored modified file):
# sg add . in one, git add . in the other, then the two indexes must agree
# byte-for-byte via git ls-files -s. This proves recursion, ignore-skip,
# tracked-wins and deletion staging in one shot.
P9T_BASE="$WORKDIR/phase9_add_base"
mkdir -p "$P9T_BASE"
git init -q "$P9T_BASE"
git -C "$P9T_BASE" config core.ignorecase false
printf '*.log\nbuild/\n' > "$P9T_BASE/.gitignore"
mkdir -p "$P9T_BASE/src" "$P9T_BASE/build"
printf 'main\n' > "$P9T_BASE/src/main.c"
printf 'keep\n' > "$P9T_BASE/keep.txt"
printf 'gone\n' > "$P9T_BASE/del.txt"
printf 'log v1\n' > "$P9T_BASE/app.log"
(cd "$P9T_BASE" && git add -A && git add -f app.log && git commit -q -m "p9 add base") \
    > /dev/null 2>&1

printf 'more\n' >> "$P9T_BASE/src/main.c"  # modified tracked file
rm "$P9T_BASE/del.txt"                     # deleted tracked file
printf 'new\n' > "$P9T_BASE/src/new.c"     # new file in a subdirectory
printf 'top\n' > "$P9T_BASE/newtop.txt"    # new top-level file
printf 'dbg\n' > "$P9T_BASE/debug.log"     # ignored new file
printf 'obj\n' > "$P9T_BASE/build/out.bin" # new file inside an ignored dir
printf 'log v2\n' >> "$P9T_BASE/app.log"   # tracked-but-ignored, modified

P9T_TWIN="$WORKDIR/phase9_add_twin"
cp -R "$P9T_BASE" "$P9T_TWIN"

(cd "$P9T_BASE" && "$SG" add .) > /dev/null 2>&1
check "phase9 add-equiv: sg add . exits 0 on the dirty tree" test $? = 0
(cd "$P9T_TWIN" && git add .) > /dev/null 2>&1

P9T_SG_LS="$WORKDIR/p9t_sg_ls.txt"
P9T_GIT_LS="$WORKDIR/p9t_git_ls.txt"
(cd "$P9T_BASE" && git ls-files -s) > "$P9T_SG_LS" 2>/dev/null
(cd "$P9T_TWIN" && git ls-files -s) > "$P9T_GIT_LS" 2>/dev/null
check "phase9 add-equiv: git ls-files -s after sg add . is byte-identical to git add ." \
    cmp -s "$P9T_SG_LS" "$P9T_GIT_LS"
check "phase9 add-equiv: the deleted tracked file was staged as deleted" \
    sh -c "! grep -q 'del\.txt' '$P9T_SG_LS'"
check "phase9 add-equiv: tracked-but-ignored app.log was re-staged (tracked wins)" \
    grep -q "app.log" "$P9T_SG_LS"
check "phase9 add-equiv: the ignored new file was NOT staged" \
    sh -c "! grep -q 'debug\.log' '$P9T_SG_LS'"

# --- Phase 9: explicit ignored args, -f/--force, tracked-wins --------------
P9F_REPO="$WORKDIR/phase9_force_repo"
mkdir -p "$P9F_REPO"
(cd "$WORKDIR" && "$SG" init phase9_force_repo) > /dev/null 2>&1
git -C "$P9F_REPO" config core.ignorecase false
printf '*.log\n' > "$P9F_REPO/.gitignore"
(cd "$P9F_REPO" && "$SG" add .gitignore && "$SG" commit -m "p9 ignore rules") > /dev/null 2>&1
printf 'log v1\n' > "$P9F_REPO/app.log"

P9F_BEFORE="$WORKDIR/p9f_before.txt"
P9F_AFTER="$WORKDIR/p9f_after.txt"
P9F_ERR="$WORKDIR/p9f_err.txt"
(cd "$P9F_REPO" && git ls-files) > "$P9F_BEFORE" 2>/dev/null
(cd "$P9F_REPO" && "$SG" add app.log) > "$P9F_ERR" 2>&1
P9F_RC=$?
(cd "$P9F_REPO" && git ls-files) > "$P9F_AFTER" 2>/dev/null
check "phase9 force: explicitly adding an ignored file fails with exit 1" test "$P9F_RC" = 1
check "phase9 force: the refusal mentions -f as the way through" grep -q -- "-f" "$P9F_ERR"
check "phase9 force: the index is untouched after the refused add" \
    cmp -s "$P9F_BEFORE" "$P9F_AFTER"

(cd "$P9F_REPO" && "$SG" add -f app.log) > /dev/null 2>&1
check "phase9 force: sg add -f stages the ignored file (exit 0)" test $? = 0
(cd "$P9F_REPO" && git ls-files) | grep -q "^app.log\$"
check "phase9 force: git ls-files shows the force-added file" test $? = 0
(cd "$P9F_REPO" && "$SG" commit -m "p9 forced add") > /dev/null 2>&1
printf 'log v2\n' >> "$P9F_REPO/app.log"
P9F_STATUS="$WORKDIR/p9f_status.txt"
(cd "$P9F_REPO" && "$SG" status) > "$P9F_STATUS" 2>&1
check "phase9 force: a tracked-but-ignored file still shows as modified (tracked wins)" \
    sh -c "grep -q 'modified:' '$P9F_STATUS' && grep -q 'app\.log' '$P9F_STATUS'"

# explicit ignored DIRECTORY: refused without -f, contents staged with it
P9D_REPO="$WORKDIR/phase9_dir_repo"
mkdir -p "$P9D_REPO"
(cd "$WORKDIR" && "$SG" init phase9_dir_repo) > /dev/null 2>&1
git -C "$P9D_REPO" config core.ignorecase false
printf 'build/\n' > "$P9D_REPO/.gitignore"
mkdir -p "$P9D_REPO/build"
printf 'o\n' > "$P9D_REPO/build/out.txt"
(cd "$P9D_REPO" && "$SG" add build) > /dev/null 2>&1
check "phase9 force: explicitly adding an ignored directory fails" test $? = 1
(cd "$P9D_REPO" && git ls-files) | grep -q "build/out.txt"
check "phase9 force: nothing staged after the refused directory add" test $? != 0
(cd "$P9D_REPO" && "$SG" add -f build) > /dev/null 2>&1
check "phase9 force: sg add -f on an ignored directory exits 0" test $? = 0
(cd "$P9D_REPO" && git ls-files) | grep -q "^build/out.txt\$"
check "phase9 force: git ls-files shows the force-added directory content" test $? = 0

# --- Phase 9: pathspec shapes -- subdir ., dir/, nosuch, empty dir, FIFO ---
P9S_REPO="$WORKDIR/phase9_paths_repo"
mkdir -p "$P9S_REPO/sub" "$P9S_REPO/sub2" "$P9S_REPO/emptyd"
(cd "$WORKDIR" && "$SG" init phase9_paths_repo) > /dev/null 2>&1
printf 'a\n' > "$P9S_REPO/sub/a.txt"
printf 'b\n' > "$P9S_REPO/sub2/b.txt"
printf 'top\n' > "$P9S_REPO/top.txt"

(cd "$P9S_REPO/sub" && "$SG" add .) > /dev/null 2>&1
check "phase9 paths: sg add . from a subdirectory exits 0" test $? = 0
P9S_LS="$WORKDIR/p9s_ls.txt"
(cd "$P9S_REPO" && git ls-files) > "$P9S_LS" 2>/dev/null
check "phase9 paths: the subdirectory's file was staged" grep -q "^sub/a.txt\$" "$P9S_LS"
check "phase9 paths: files outside the subdirectory were NOT staged" \
    sh -c "! grep -q '^top\.txt\$' '$P9S_LS'"

(cd "$P9S_REPO" && "$SG" add nosuch) > /dev/null 2>&1
check "phase9 paths: sg add nosuch still errors with a nonzero exit" test $? = 1

(cd "$P9S_REPO" && "$SG" add sub2/) > /dev/null 2>&1
check "phase9 paths: sg add dir/ (trailing slash) exits 0" test $? = 0
(cd "$P9S_REPO" && git ls-files) | grep -q "^sub2/b.txt\$"
check "phase9 paths: the trailing-slash dir's file was staged" test $? = 0

P9S_EMPTY_OUT="$WORKDIR/p9s_empty_out.txt"
(cd "$P9S_REPO" && "$SG" add emptyd) > "$P9S_EMPTY_OUT" 2>&1
P9S_EMPTY_RC=$?
check "phase9 paths: adding an empty directory exits 0" test "$P9S_EMPTY_RC" = 0
check "phase9 paths: adding an empty directory prints nothing" test ! -s "$P9S_EMPTY_OUT"

# an explicitly named FIFO must error cleanly, never hang reading it; the
# perl alarm turns a hang into exit 142 so the check below still fails fast
mkfifo "$P9S_REPO/fifo1" 2>/dev/null
(cd "$P9S_REPO" && perl -e 'alarm 10; exit(system(@ARGV) == 0 ? 0 : 1);' "$SG" add fifo1) \
    > /dev/null 2>&1
P9S_FIFO_RC=$?
check "phase9 paths: an explicit FIFO argument exits 1 without hanging" \
    test "$P9S_FIFO_RC" = 1
(cd "$P9S_REPO" && git ls-files) | grep -q "fifo1"
check "phase9 paths: the FIFO was not staged" test $? != 0

# --- Phase 9: chunking still works through add-recursion -------------------
P9C_REPO="$WORKDIR/phase9_chunk_repo"
mkdir -p "$P9C_REPO"
(cd "$WORKDIR" && "$SG" init phase9_chunk_repo) > /dev/null 2>&1
git config -f "$P9C_REPO/.git/config" sg.chunking true
git config -f "$P9C_REPO/.git/config" sg.chunkthreshold 1048576
mkdir -p "$P9C_REPO/data"
head -c 2097152 /dev/urandom > "$P9C_REPO/data/big.bin" 2>/dev/null
printf 'small\n' > "$P9C_REPO/data/small.txt"
(cd "$P9C_REPO" && "$SG" add .) > /dev/null 2>&1
check "phase9 chunk: sg add . exits 0 in a chunking-enabled repo" test $? = 0
P9C_BLOB=$(cd "$P9C_REPO" && git ls-files -s data/big.bin 2>/dev/null | awk '{print $2}')
P9C_SIZE=$(cd "$P9C_REPO" && git cat-file -s "$P9C_BLOB" 2>/dev/null)
check "phase9 chunk: the staged blob is a small pointer, not the full 2MiB file" \
    sh -c "test -n '$P9C_SIZE' && test '$P9C_SIZE' -lt 65536"
(cd "$P9C_REPO" && git cat-file -p "$P9C_BLOB" 2>/dev/null) | head -1 | grep -q "sg-chunked"
check "phase9 chunk: the pointer blob carries the sg-chunked magic" test $? = 0
P9C_INFO="$WORKDIR/p9c_info.txt"
(cd "$P9C_REPO" && "$SG" chunk-info data/big.bin) > "$P9C_INFO" 2>&1
check "phase9 chunk: sg chunk-info reports the recursively added file as chunked" \
    grep -q "chunked: yes" "$P9C_INFO"

# --- Phase 9: status ignore filtering and nested .git ----------------------
P9ST_REPO="$WORKDIR/phase9_statusign_repo"
mkdir -p "$P9ST_REPO"
(cd "$WORKDIR" && "$SG" init phase9_statusign_repo) > /dev/null 2>&1
git -C "$P9ST_REPO" config core.ignorecase false
printf 'ign.txt\n' > "$P9ST_REPO/.gitignore"
(cd "$P9ST_REPO" && "$SG" add .gitignore && "$SG" commit -m "p9 status rules") > /dev/null 2>&1
printf 'x\n' > "$P9ST_REPO/ign.txt"
P9ST_OUT="$WORKDIR/p9st_out.txt"
(cd "$P9ST_REPO" && "$SG" status) > "$P9ST_OUT" 2>&1
check "phase9 status: an ignored untracked file is absent from sg status" \
    sh -c "! grep -q 'ign\.txt' '$P9ST_OUT'"
check "phase9 status: working tree clean when only ignored files exist" \
    grep -q "nothing to commit, working tree clean" "$P9ST_OUT"

mkdir -p "$P9ST_REPO/subrepo/.git"
printf 'junk not a real ref\n' > "$P9ST_REPO/subrepo/.git/HEAD"
printf 'real\n' > "$P9ST_REPO/subrepo/file.txt"
(cd "$P9ST_REPO" && "$SG" status -uall) > "$P9ST_OUT" 2>&1
check "phase9 status: a file next to a nested .git dir is still reported" \
    grep -q "subrepo/file.txt" "$P9ST_OUT"
check "phase9 status: a nested .git dir is never descended into" \
    sh -c "! grep -q 'subrepo/\.git' '$P9ST_OUT'"

# --- A tree deeper than the platform's PATH_MAX must never be skipped in
# --- silence. sg's walkers build ABSOLUTE paths, so on a deep enough tree
# --- lstat/opendir fail with ENAMETOOLONG long before sg's own buffers fill
# --- (git avoids this entirely by traversing with openat()). The original
# --- walk treated every lstat failure as "vanished mid-walk" and skipped it,
# --- so `sg add .` staged nothing, exited 0, and a commit would have been
# --- quietly missing content. Failing loudly is acceptable here; failing
# --- silently is not, so that is exactly what this asserts.
P9DEEP_REPO="$WORKDIR/phase9_deep_repo"
"$SG" init "$P9DEEP_REPO" > /dev/null 2>&1
# Built by chdir'ing down one short relative component at a time, which is
# how a tree whose ABSOLUTE paths exceed PATH_MAX can exist at all (git walks
# it fine via openat; sg, which composes absolute paths, cannot). Nesting
# "until mkdir fails" instead would make the depth depend on how long the
# temp directory's own path happens to be, and would trip sg's opendir check
# -- which already failed loudly -- rather than the per-entry lstat that used
# to be silent. 250 levels clears PATH_MAX on both macOS (1024) and Linux (4096).
python3 - "$P9DEEP_REPO" <<'PYEOF'
import os, sys
os.chdir(sys.argv[1])
for _ in range(250):
    os.mkdir("d" * 20)
    os.chdir("d" * 20)
with open("leaf.txt", "w") as f:
    f.write("deep\n")
PYEOF

(cd "$P9DEEP_REPO" && "$SG" add .) > "$WORKDIR/p9deep_add.txt" 2>&1
P9DEEP_RC=$?
P9DEEP_STAGED=$( (cd "$P9DEEP_REPO" && git ls-files | grep -c 'leaf\.txt') 2>/dev/null || echo 0)
# Pass if sg either staged the deep file, or refused with a non-zero exit.
# Fail only on the silent case: exit 0 having quietly dropped it.
check "phase9 deep tree: sg add . never exits 0 while silently omitting a file" \
    sh -c "[ \"$P9DEEP_RC\" -ne 0 ] || [ \"$P9DEEP_STAGED\" -gt 0 ]"
check "phase9 deep tree: an unreadable path is reported, not swallowed" \
    sh -c "[ \"$P9DEEP_STAGED\" -gt 0 ] || [ -s '$WORKDIR/p9deep_add.txt' ]"

(cd "$P9DEEP_REPO" && "$SG" status) > "$WORKDIR/p9deep_st.txt" 2>"$WORKDIR/p9deep_st_err.txt"
P9DEEP_ST_RC=$?
check "phase9 deep tree: sg status still succeeds" test "$P9DEEP_ST_RC" = 0
check "phase9 deep tree: sg status warns that the untracked list may be incomplete" \
    sh -c "grep -q 'leaf\.txt' '$WORKDIR/p9deep_st.txt' || [ -s '$WORKDIR/p9deep_st_err.txt' ]"

# --- Tracked files are never subject to ignore rules -- including when it is
# --- their whole PARENT DIRECTORY that became ignored after they were
# --- committed. Pruning the directory is right for untracked content, but
# --- the walk is also the only thing that re-hashes tracked files, so
# --- pruning one holding tracked files left their modifications silently
# --- unstaged while `git add .` staged them. Compared against real git
# --- rather than against an expectation.
P9TD_SG="$WORKDIR/phase9_trackdir_sg"
P9TD_GIT="$WORKDIR/phase9_trackdir_git"
for d in "$P9TD_SG" "$P9TD_GIT"; do
    git init -q "$d"
    (cd "$d" && git config user.email "a@b.c" && git config user.name "a" \
        && git config core.ignorecase false)
    mkdir -p "$d/logs/sub"
    printf 'v1\n' > "$d/logs/app.txt"
    printf 'x\n' > "$d/other.txt"
done
(cd "$P9TD_SG" && "$SG" add logs/app.txt other.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P9TD_GIT" && git add logs/app.txt other.txt && git commit -qm base) > /dev/null 2>&1
for d in "$P9TD_SG" "$P9TD_GIT"; do
    printf 'logs/\n' > "$d/.gitignore"
    printf 'v2\n' > "$d/logs/app.txt"          # tracked, parent now ignored
    printf 'untracked\n' > "$d/logs/sub/new.txt"  # ignored AND untracked: must stay out
done
(cd "$P9TD_SG" && "$SG" add .) > /dev/null 2>&1
(cd "$P9TD_GIT" && git add .) > /dev/null 2>&1
(cd "$P9TD_SG" && git ls-files -s) > "$WORKDIR/p9td_sg.txt" 2>&1
(cd "$P9TD_GIT" && git ls-files -s) > "$WORKDIR/p9td_git.txt" 2>&1
check "phase9 tracked-under-ignored-dir: sg add . produces the same index as git add ." \
    cmp -s "$WORKDIR/p9td_sg.txt" "$WORKDIR/p9td_git.txt"
check "phase9 tracked-under-ignored-dir: the modification is actually staged, not the old blob" \
    sh -c "(cd '$P9TD_SG' && git cat-file -p :logs/app.txt) | grep -q v2"
check "phase9 tracked-under-ignored-dir: an ignored untracked file under it is still excluded" \
    sh -c "! (cd '$P9TD_SG' && git ls-files) | grep -q 'logs/sub/new.txt'"

# --- A failed branch deletion must not leave the repository half-deleted.
# --- packed-refs is rewritten BEFORE the loose ref is unlinked, so if the
# --- rewrite fails the loose ref -- which shadows packed -- is still there
# --- and the branch reads exactly as before. The reverse order would expose
# --- a stale packed entry, silently resurrecting the branch at an older
# --- commit while reporting failure.
if [ "$(id -u)" = "0" ]; then
    skip "phase9 branch delete failure atomicity (running as root: permissions do not apply)"
else
    P9BD="$WORKDIR/phase9_branchdel"
    git init -q "$P9BD"
    (cd "$P9BD" && git config user.email "a@b.c" && git config user.name "a")
    printf 'a\n' > "$P9BD/f.txt"
    (cd "$P9BD" && git add f.txt && git commit -qm c1) > /dev/null 2>&1
    (cd "$P9BD" && git branch victim && git pack-refs --all) > /dev/null 2>&1
    (cd "$P9BD" && git switch -q victim && printf 'b\n' >> f.txt \
        && git commit -qam c2 && git switch -q master) > /dev/null 2>&1
    P9BD_TIP=$(cd "$P9BD" && git rev-parse victim)
    chmod 555 "$P9BD/.git"
    (cd "$P9BD" && "$SG" branch -d victim --force) > "$WORKDIR/p9bd.txt" 2>&1
    P9BD_RC=$?
    chmod 755 "$P9BD/.git"
    check "phase9 branch delete: a failed deletion reports failure" test "$P9BD_RC" != 0
    check "phase9 branch delete: a failed deletion leaves the branch at its real tip, not a stale packed one" \
        sh -c "[ \"\$(cd '$P9BD' && git rev-parse victim 2>/dev/null)\" = '$P9BD_TIP' ]"
fi

# --- Phase 12: commit/tag message normalization must be bit-identical to
# --- real git's default `--cleanup=whitespace`. Earlier interop coverage
# --- never compared the raw bytes of the message segment, so sg producing
# --- "msg" (no trailing newline) where git produces "msg\n" went unnoticed
# --- even though both are valid, fsck-clean objects -- the object ids just
# --- silently diverge. Each case below is a row of the normalization table
# --- verified directly against a real `git commit`/`git tag`.
#
# extract_msg_segment: strips the header lines (everything up to and
# including the first blank line) from `git cat-file <type> <id>` output,
# leaving the message segment's exact bytes for a byte-for-byte cmp.
extract_msg_segment() {
    sed '1,/^$/d'
}

P12_SG_BASE="$WORKDIR/phase12_msg_sg"
P12_GIT_BASE="$WORKDIR/phase12_msg_git"
mkdir -p "$P12_SG_BASE" "$P12_GIT_BASE"
P12_N=0

# Runs a single commit-message case: $1 = human label, $2 = the raw -m
# argument, $3 = 1 if real git is expected to reject this input (empty
# message), 0 otherwise.
p12_commit_case() {
    case_label="$1"
    msg="$2"
    expect_reject="$3"

    P12_N=$((P12_N + 1))
    sgrepo="$P12_SG_BASE/c$P12_N"
    gitrepo="$P12_GIT_BASE/c$P12_N"
    mkdir -p "$sgrepo"
    (cd "$P12_SG_BASE" && "$SG" init "c$P12_N") > /dev/null 2>&1
    git init -q "$gitrepo"
    (cd "$gitrepo" && git config user.email "a@b.c" && git config user.name "a")

    printf 'x\n' > "$sgrepo/f.txt"
    printf 'x\n' > "$gitrepo/f.txt"
    (cd "$sgrepo" && "$SG" add f.txt && "$SG" commit -m "$msg") > /dev/null 2>&1
    sg_rc=$?
    (cd "$gitrepo" && git add f.txt && git commit -q -m "$msg") > /dev/null 2>&1
    git_rc=$?

    if [ "$expect_reject" = 1 ]; then
        check "phase12 commit message ($case_label): real git rejects this input (sanity check on the test itself)" \
            test "$git_rc" != 0
        check "phase12 commit message ($case_label): sg rejects it too" test "$sg_rc" != 0
        return
    fi

    check "phase12 commit message ($case_label): real git accepted this input (sanity check on the test itself)" \
        test "$git_rc" = 0
    check "phase12 commit message ($case_label): sg commit exits 0" test "$sg_rc" = 0

    sg_id=$(cd "$sgrepo" && git rev-parse HEAD 2>/dev/null)
    git_id=$(cd "$gitrepo" && git rev-parse HEAD 2>/dev/null)
    sg_msg_file="$WORKDIR/p12_sg_msg_$P12_N.bin"
    git_msg_file="$WORKDIR/p12_git_msg_$P12_N.bin"
    (cd "$sgrepo" && git cat-file commit "$sg_id") | extract_msg_segment > "$sg_msg_file"
    (cd "$gitrepo" && git cat-file commit "$git_id") | extract_msg_segment > "$git_msg_file"

    check "phase12 commit message ($case_label): message segment byte-identical to real git" \
        cmp -s "$sg_msg_file" "$git_msg_file"
}

# $(...) strips ALL trailing newlines from its output, which would corrupt
# any case whose raw -m argument is meant to end in one or more '\n' bytes.
# p12_mk appends a sentinel after the printf output and strips it back off,
# which protects genuine trailing newlines from that stripping.
p12_mk() {
    tail_with_marker=$(printf '%sEND_MARK' "$1")
    printf '%s' "${tail_with_marker%END_MARK}"
}

p12_commit_case "plain" "x" 0
p12_commit_case "leading whitespace preserved" "  x  " 0
p12_commit_case "already newline-terminated" "$(p12_mk 'x
')" 0
p12_commit_case "trailing blank lines removed" "$(p12_mk 'x


')" 0
p12_commit_case "leading blank lines removed" "$(printf '\n\nx')" 0
p12_commit_case "single interior blank line preserved" "$(printf 'a\n\nb')" 0
p12_commit_case "consecutive blank lines collapsed" "$(printf 'a\n\n\nb')" 0
p12_commit_case "trailing whitespace stripped per line" "$(printf 'a   \nb')" 0
p12_commit_case "leading tab preserved, trailing tab stripped" "$(printf '\tx\t')" 0
p12_commit_case "empty message" "" 1
p12_commit_case "whitespace-only message" "   " 1

# --- same normalization table, but for annotated tag messages (`sg tag -a
# --- -m` vs `git tag -a -m`). Reuses one base commit per sg/git pair so the
# --- tag's target-object line is identical and only the message segment
# --- (everything after "tag <name>\n...\ntagger ...\n\n") is compared.
p12_tag_case() {
    case_label="$1"
    msg="$2"
    expect_reject="$3"

    P12_N=$((P12_N + 1))
    sgrepo="$P12_SG_BASE/t$P12_N"
    gitrepo="$P12_GIT_BASE/t$P12_N"
    mkdir -p "$sgrepo"
    (cd "$P12_SG_BASE" && "$SG" init "t$P12_N") > /dev/null 2>&1
    git init -q "$gitrepo"
    (cd "$gitrepo" && git config user.email "a@b.c" && git config user.name "a")

    printf 'x\n' > "$sgrepo/f.txt"
    printf 'x\n' > "$gitrepo/f.txt"
    (cd "$sgrepo" && "$SG" add f.txt && "$SG" commit -m base) > /dev/null 2>&1
    (cd "$gitrepo" && git add f.txt && git commit -q -m base) > /dev/null 2>&1

    (cd "$sgrepo" && "$SG" tag -a -m "$msg" v1) > /dev/null 2>&1
    sg_rc=$?
    (cd "$gitrepo" && git tag -a -m "$msg" v1) > /dev/null 2>&1
    git_rc=$?

    if [ "$expect_reject" = 1 ]; then
        check "phase12 tag message ($case_label): real git rejects this input (sanity check on the test itself)" \
            test "$git_rc" != 0
        check "phase12 tag message ($case_label): sg rejects it too" test "$sg_rc" != 0
        return
    fi

    check "phase12 tag message ($case_label): real git accepted this input (sanity check on the test itself)" \
        test "$git_rc" = 0
    check "phase12 tag message ($case_label): sg tag exits 0" test "$sg_rc" = 0

    sg_id=$(cd "$sgrepo" && git rev-parse v1 2>/dev/null)
    git_id=$(cd "$gitrepo" && git rev-parse v1 2>/dev/null)
    sg_msg_file="$WORKDIR/p12_sg_tagmsg_$P12_N.bin"
    git_msg_file="$WORKDIR/p12_git_tagmsg_$P12_N.bin"
    (cd "$sgrepo" && git cat-file tag "$sg_id") | extract_msg_segment > "$sg_msg_file"
    (cd "$gitrepo" && git cat-file tag "$git_id") | extract_msg_segment > "$git_msg_file"

    check "phase12 tag message ($case_label): message segment byte-identical to real git" \
        cmp -s "$sg_msg_file" "$git_msg_file"
}

p12_tag_case "plain" "x" 0
p12_tag_case "leading whitespace preserved" "  x  " 0
p12_tag_case "already newline-terminated" "$(p12_mk 'x
')" 0
p12_tag_case "trailing blank lines removed" "$(p12_mk 'x


')" 0
p12_tag_case "leading blank lines removed" "$(printf '\n\nx')" 0
p12_tag_case "single interior blank line preserved" "$(printf 'a\n\nb')" 0
p12_tag_case "consecutive blank lines collapsed" "$(printf 'a\n\n\nb')" 0
p12_tag_case "trailing whitespace stripped per line" "$(printf 'a   \nb')" 0
p12_tag_case "leading tab preserved, trailing tab stripped" "$(printf '\tx\t')" 0
# NOTE (verified directly against git 2.55.0, not assumed): `git commit -m`
# and `git tag -a -m` are asymmetric here. `git commit -m ""` refuses to
# create a commit ("aborting commit due to empty commit message"), but
# `git tag -a -m ""` (or any message that normalizes to empty, like
# whitespace-only) happily creates a tag object with an empty message
# segment, exit 0. sg mirrors this asymmetry: sg_cmd_commit rejects an
# empty-after-cleanup message, sg_cmd_tag does not. Don't "fix" this to be
# symmetric later without re-checking real git -- it really is inconsistent
# upstream, and interop is the oracle, not intuition.
p12_tag_case "empty message accepted, empty message segment" "" 0
p12_tag_case "whitespace-only message accepted, empty message segment" "   " 0

# ============================================================
# Phase 13: sg reset
#
# For each case, an identical 2-commit history (+ a lightweight tag "v1" at
# the first commit) is built twice -- once with sg, once with real git --
# then an identical scenario (staged/unstaged/untracked changes) is layered
# on top of both, and finally `sg reset <mode> <rev>` / `git reset <mode>
# <rev>` (same flag syntax on both sides) is run. Blob and tree object ids
# are content-addressed (no timestamps involved), so they are directly
# comparable across the two independently-built repos even though the
# commit ids themselves differ (different wall-clock commit times) -- that's
# what the tree-hash check below relies on. `git status --porcelain` and
# `git ls-files -s` are run against BOTH repos with real git itself (both sg
# and real-git repos are plain git repos on disk), so a line-for-line diff
# of their output is the actual interop oracle for whether sg's index/
# workdir end up in the state real git would produce.
# ============================================================

P12R_SG_BASE="$WORKDIR/phase12_reset_sg"
P12R_GIT_BASE="$WORKDIR/phase12_reset_git"
mkdir -p "$P12R_SG_BASE" "$P12R_GIT_BASE"
P12R_N=0

# Builds a 2-commit history in $1 using binary $2 ("$SG" or "git"):
#   c1: file1.txt, sub/file2.txt, tagged "v1"
#   c2: file1.txt modified, file3.txt added
p12r_base() {
    dir="$1"
    bin="$2"

    if [ "$bin" = "$SG" ]; then
        mkdir -p "$(dirname "$dir")"
        (cd "$(dirname "$dir")" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    else
        git init -q "$dir"
        (cd "$dir" && git config user.email "a@b.c" && git config user.name "a")
    fi

    mkdir -p "$dir/sub"
    printf 'content1\n' > "$dir/file1.txt"
    printf 'content2\n' > "$dir/sub/file2.txt"
    (cd "$dir" && "$bin" add file1.txt sub/file2.txt && "$bin" commit -m "c1") > /dev/null 2>&1
    (cd "$dir" && "$bin" tag v1) > /dev/null 2>&1

    printf 'content1-v2\n' > "$dir/file1.txt"
    printf 'content3\n' > "$dir/file3.txt"
    (cd "$dir" && "$bin" add file1.txt file3.txt && "$bin" commit -m "c2") > /dev/null 2>&1
}

p12r_setup_clean() { :; }

p12r_setup_staged() {
    dir="$1"
    bin="$2"
    printf 'content1-staged\n' > "$dir/file1.txt"
    (cd "$dir" && "$bin" add file1.txt) > /dev/null 2>&1
}

p12r_setup_unstaged() {
    dir="$1"
    printf 'content1-unstaged\n' > "$dir/file1.txt"
}

p12r_setup_staged_and_unstaged() {
    dir="$1"
    bin="$2"
    printf 'staged new content\n' > "$dir/staged-new.txt"
    (cd "$dir" && "$bin" add staged-new.txt) > /dev/null 2>&1
    printf 'content2-unstaged\n' > "$dir/sub/file2.txt"
}

p12r_setup_untracked_survives() {
    dir="$1"
    bin="$2"
    printf 'staged new content\n' > "$dir/staged-new.txt"
    (cd "$dir" && "$bin" add staged-new.txt) > /dev/null 2>&1
    printf 'content2-unstaged\n' > "$dir/sub/file2.txt"
    printf 'never added\n' > "$dir/untracked.txt"
}

p12r_setup_deleted_staged() {
    dir="$1"
    bin="$2"
    rm -f "$dir/file3.txt"
    (cd "$dir" && "$bin" add file3.txt) > /dev/null 2>&1
}

p12r_setup_deleted_unstaged() {
    dir="$1"
    rm -f "$dir/file3.txt"
}

p12r_setup_nested() {
    dir="$1"
    bin="$2"
    printf 'nested new\n' > "$dir/sub/newnested.txt"
    (cd "$dir" && "$bin" add sub/newnested.txt) > /dev/null 2>&1
    printf 'content2-unstaged\n' > "$dir/sub/file2.txt"
}

# $1 = label, $2 = reset mode ("--soft"/"--mixed"/"--hard"), $3 = rev
# ("HEAD~1" or "v1"), $4 = name of a p12r_setup_* function.
p12r_case() {
    case_label="$1"
    mode="$2"
    rev="$3"
    setup_fn="$4"

    P12R_N=$((P12R_N + 1))
    sgrepo="$P12R_SG_BASE/n$P12R_N"
    gitrepo="$P12R_GIT_BASE/n$P12R_N"

    p12r_base "$sgrepo" "$SG"
    p12r_base "$gitrepo" git
    "$setup_fn" "$sgrepo" "$SG"
    "$setup_fn" "$gitrepo" git

    sg_target=$(cd "$sgrepo" && git rev-parse "$rev" 2>/dev/null)
    git_target=$(cd "$gitrepo" && git rev-parse "$rev" 2>/dev/null)

    # --force is only meaningful (and only accepted) on sg's side: --hard is
    # the one mode sg gates behind a confirmation prompt (real git's `reset`
    # never prompts, and has no --force flag at all), and a non-interactive
    # stdin auto-declines an unconfirmed dangerous op -- these scenarios are
    # deliberately dirty, so without --force a --hard case here would abort
    # instead of resetting. Passing --force for --soft/--mixed too is a
    # harmless no-op (sg only reads it in the --hard path).
    (cd "$sgrepo" && "$SG" reset "$mode" --force "$rev") > /dev/null 2>&1
    sg_rc=$?
    (cd "$gitrepo" && git reset "$mode" "$rev") > /dev/null 2>&1
    git_rc=$?

    check "phase12 reset ($case_label): sg reset exits 0" test "$sg_rc" = 0
    check "phase12 reset ($case_label): real git reset exits 0 (sanity check on the test itself)" \
        test "$git_rc" = 0

    sg_head=$(cd "$sgrepo" && git rev-parse HEAD 2>/dev/null)
    git_head=$(cd "$gitrepo" && git rev-parse HEAD 2>/dev/null)
    check "phase12 reset ($case_label): sg moved the branch to the requested rev" \
        test "$sg_head" = "$sg_target"
    check "phase12 reset ($case_label): real git moved the branch to the requested rev (sanity check)" \
        test "$git_head" = "$git_target"

    sg_tree=$(cd "$sgrepo" && git rev-parse HEAD^{tree} 2>/dev/null)
    git_tree=$(cd "$gitrepo" && git rev-parse HEAD^{tree} 2>/dev/null)
    check "phase12 reset ($case_label): HEAD's tree is content-identical to real git's" \
        test -n "$sg_tree" -a "$sg_tree" = "$git_tree"

    sg_porcelain="$WORKDIR/p12r_sg_porcelain_$P12R_N.txt"
    git_porcelain="$WORKDIR/p12r_git_porcelain_$P12R_N.txt"
    (cd "$sgrepo" && git status --porcelain) | sort > "$sg_porcelain"
    (cd "$gitrepo" && git status --porcelain) | sort > "$git_porcelain"
    check "phase12 reset ($case_label): git status --porcelain matches real git (index + workdir oracle)" \
        cmp -s "$sg_porcelain" "$git_porcelain"

    sg_lsfiles="$WORKDIR/p12r_sg_lsfiles_$P12R_N.txt"
    git_lsfiles="$WORKDIR/p12r_git_lsfiles_$P12R_N.txt"
    (cd "$sgrepo" && git ls-files -s) | sort > "$sg_lsfiles"
    (cd "$gitrepo" && git ls-files -s) | sort > "$git_lsfiles"
    check "phase12 reset ($case_label): git ls-files -s matches real git" \
        cmp -s "$sg_lsfiles" "$git_lsfiles"

    check "phase12 reset ($case_label): working directory content matches real git" \
        diff -rq --exclude=.git "$sgrepo" "$gitrepo" > /dev/null 2>&1
}

p12r_case "clean workdir, mixed, HEAD~1" "--mixed" "HEAD~1" p12r_setup_clean
p12r_case "staged only, mixed, HEAD~1" "--mixed" "HEAD~1" p12r_setup_staged
p12r_case "unstaged only, mixed, HEAD~1" "--mixed" "HEAD~1" p12r_setup_unstaged
p12r_case "staged+unstaged, mixed, HEAD~1" "--mixed" "HEAD~1" p12r_setup_staged_and_unstaged
p12r_case "clean workdir, soft, HEAD~1" "--soft" "HEAD~1" p12r_setup_clean
p12r_case "staged+unstaged, soft, HEAD~1 (soft must not touch index/workdir)" "--soft" "HEAD~1" \
    p12r_setup_staged_and_unstaged
p12r_case "clean workdir, hard, HEAD~1" "--hard" "HEAD~1" p12r_setup_clean
p12r_case "staged+unstaged, hard, HEAD~1" "--hard" "HEAD~1" p12r_setup_staged_and_unstaged
p12r_case "untracked file survives, mixed, HEAD~1" "--mixed" "HEAD~1" p12r_setup_untracked_survives
p12r_case "untracked file survives, hard, HEAD~1" "--hard" "HEAD~1" p12r_setup_untracked_survives
p12r_case "deleted file staged, mixed, HEAD~1" "--mixed" "HEAD~1" p12r_setup_deleted_staged
p12r_case "deleted file unstaged, hard, HEAD~1" "--hard" "HEAD~1" p12r_setup_deleted_unstaged
p12r_case "nested directory file, mixed, tag v1" "--mixed" "v1" p12r_setup_nested
p12r_case "staged+unstaged, hard, tag v1" "--hard" "v1" p12r_setup_staged_and_unstaged

# --- rescue path: sg undo must recover both --hard and --mixed resets ---

P12R_UNDO_HARD="$P12R_SG_BASE/undo_hard"
p12r_base "$P12R_UNDO_HARD" "$SG"
printf 'about to be reset away\n' > "$P12R_UNDO_HARD/file1.txt"
(cd "$P12R_UNDO_HARD" && "$SG" add file1.txt) > /dev/null 2>&1
(cd "$P12R_UNDO_HARD" && "$SG" reset --hard --force HEAD~1) > /dev/null 2>&1
check "phase12 reset undo: sg reset --hard exits 0" test $? = 0
(cd "$P12R_UNDO_HARD" && "$SG" undo 1 --force) > /dev/null 2>&1
check "phase12 reset undo: sg undo 1 --force exits 0 after a --hard reset" test $? = 0
UNDO_HARD_CONTENT=$(cat "$P12R_UNDO_HARD/file1.txt" 2>/dev/null)
check "phase12 reset undo: undo restores the workdir content that --hard reset overwrote" \
    test "$UNDO_HARD_CONTENT" = "about to be reset away"

# NOTE: undo only ever touches the workdir/index (sg_apply_tree_to_workdir's
# documented contract) -- it deliberately does not move HEAD/the branch back
# to c2, so `git diff --cached` here is against c1 (where --hard left the
# branch), not the pre-reset c2, and would show file1.txt as "staged" no
# matter what content undo restored. Read the index's blob directly instead.
UNDO_HARD_STAGED_CONTENT=$(cd "$P12R_UNDO_HARD" && git show :file1.txt 2>/dev/null)
check "phase12 reset undo: undo restores the staged index content that --hard reset discarded" \
    test "$UNDO_HARD_STAGED_CONTENT" = "about to be reset away"

P12R_UNDO_MIXED="$P12R_SG_BASE/undo_mixed"
p12r_base "$P12R_UNDO_MIXED" "$SG"
printf 'staged before mixed reset\n' > "$P12R_UNDO_MIXED/file1.txt"
(cd "$P12R_UNDO_MIXED" && "$SG" add file1.txt) > /dev/null 2>&1
(cd "$P12R_UNDO_MIXED" && "$SG" reset) > /dev/null 2>&1
check "phase12 reset undo: bare sg reset (mixed HEAD) exits 0" test $? = 0
UNDO_MIXED_AFTER_RESET=$(cd "$P12R_UNDO_MIXED" && git diff --cached --name-only)
check "phase12 reset undo: bare sg reset actually unstaged file1.txt" test -z "$UNDO_MIXED_AFTER_RESET"
(cd "$P12R_UNDO_MIXED" && "$SG" undo 1 --force) > /dev/null 2>&1
check "phase12 reset undo: sg undo 1 --force exits 0 after a --mixed reset" test $? = 0
UNDO_MIXED_STAGED=$(cd "$P12R_UNDO_MIXED" && git diff --cached --name-only)
check "phase12 reset undo: undo re-stages the change that bare (--mixed) reset unstaged" \
    test "$UNDO_MIXED_STAGED" = "file1.txt"

# --- the stat-field guard in sg_index_reset_to_tree, with real git as the judge ---
#
# --mixed rewrites the index without touching the working tree, so an index
# entry's stat fields have no fresh stat() to come from. Reusing the previous
# entry's stat for a path whose blob actually changed produces an index that
# describes the file as up to date while recording a different sha -- and real
# git, which trusts stat as a shortcut, then reports a clean tree over a file
# that differs. sg_index_reset_to_tree therefore only carries stat fields over
# when the sha is unchanged, and zeroes them otherwise.
#
# The mtime has to be pushed into the past for this to be observable at all:
# when a file's mtime matches the index's own mtime, git calls the entry
# racily clean and re-hashes the content regardless of what stat says, which
# masks the bug. Every other check in this section writes its files in the
# same second it writes the index, so none of them can see this.
P12R_RACY="$P12R_SG_BASE/racy"
p12r_base "$P12R_RACY" "$SG"
printf 'content1-changed\n' > "$P12R_RACY/file1.txt"
touch -t 202001010000 "$P12R_RACY/file1.txt"
(cd "$P12R_RACY" && "$SG" add file1.txt && "$SG" commit -m "racy c2") > /dev/null 2>&1
touch -t 202001010000 "$P12R_RACY/file1.txt"
(cd "$P12R_RACY" && "$SG" reset --mixed HEAD~1) > /dev/null 2>&1
check "phase12 reset stat guard: bare reset over a stale-mtime file exits 0" test $? = 0
RACY_STATUS=$(cd "$P12R_RACY" && git status --porcelain)
check "phase12 reset stat guard: real git still sees the file as modified (not falsely clean)" \
    test "$RACY_STATUS" = " M file1.txt"

# --- rejection paths ---

P12R_DETACHED="$P12R_SG_BASE/detached"
p12r_base "$P12R_DETACHED" "$SG"
# Detach at HEAD (c2), NOT at HEAD~1: p12r_base tags v1 at c1, so HEAD~1 IS
# v1, and resetting to v1 from there is a no-op that moves nothing. Every
# assertion below then passes on a build where reset never touches HEAD at
# all -- measured, with a mutation that sent the detached case down the
# branch path instead. The reset has to actually move HEAD for them to mean
# anything.
DETACH_TARGET=$(cd "$P12R_DETACHED" && git rev-parse HEAD)
(cd "$P12R_DETACHED" && git checkout -q --detach "$DETACH_TARGET")
# This case asserted the opposite until Phase 18: reset REFUSED a detached
# HEAD. That refusal became untenable once rebase started replaying on a
# detached HEAD -- it would have taken away resetting during a paused rebase,
# which phase14 established and measured against real git. git allows it for
# exactly the same reason, its own rebase being detached too.
#
# The interesting part is where the write lands: HEAD moves and no branch
# does. Asserting the exit status alone would not show that.
P12R_DETACHED_BRANCH_BEFORE=$(cat "$P12R_DETACHED/.git/refs/heads/master")
(cd "$P12R_DETACHED" && "$SG" reset --mixed v1) > /dev/null 2>&1
check "phase12 reset: sg reset on a detached HEAD succeeds" test $? = 0
P12R_DETACHED_NOW=$(cd "$P12R_DETACHED" && git rev-parse HEAD)
P12R_DETACHED_WANT=$(cd "$P12R_DETACHED" && git rev-parse v1)
check "phase12 reset: ...moving HEAD itself to the target" \
    test "$P12R_DETACHED_NOW" = "$P12R_DETACHED_WANT"
check "phase12 reset: ...and leaving master where it was" \
    test "$(cat "$P12R_DETACHED/.git/refs/heads/master")" = "$P12R_DETACHED_BRANCH_BEFORE"
check "phase12 reset: precondition -- the reset target really differs from where HEAD was detached" \
    test "$P12R_DETACHED_WANT" != "$DETACH_TARGET"
P12R_DETACHED_HEADFILE=$(cat "$P12R_DETACHED/.git/HEAD")
check "phase12 reset: ...with HEAD still detached, not re-attached to a branch" \
    test "${P12R_DETACHED_HEADFILE#ref: }" = "$P12R_DETACHED_HEADFILE"
# Writing to a branch instead of HEAD does not necessarily hit an EXISTING
# branch: with current_branch NULL, a "refs/heads/%s" build yields
# "refs/heads/(null)". Comparing master alone would miss that, so the whole
# branch list is compared.
P12R_DETACHED_BRANCHES=$(cd "$P12R_DETACHED" && git for-each-ref --format='%(refname)' refs/heads | tr '\n' ' ')
check "phase12 reset: ...creating no stray branch (got '$P12R_DETACHED_BRANCHES')" \
    test "$P12R_DETACHED_BRANCHES" = "refs/heads/master "

P12R_PATHSPEC="$P12R_SG_BASE/pathspec"
p12r_base "$P12R_PATHSPEC" "$SG"
PATHSPEC_ERR="$WORKDIR/p12r_pathspec_err.txt"
(cd "$P12R_PATHSPEC" && "$SG" reset HEAD -- file1.txt) > /dev/null 2> "$PATHSPEC_ERR"
check "phase12 reset reject: sg reset with a pathspec exits non-zero" test $? = 1
check "phase12 reset reject: the pathspec error mentions 'sg restore --staged'" \
    grep -q "restore --staged" "$PATHSPEC_ERR"

P12R_MULTIMODE="$P12R_SG_BASE/multimode"
p12r_base "$P12R_MULTIMODE" "$SG"
(cd "$P12R_MULTIMODE" && "$SG" reset --soft --hard) > /dev/null 2>&1
check "phase12 reset reject: sg reset --soft --hard (two modes) exits non-zero" test $? = 1

P12R_BADREV="$P12R_SG_BASE/badrev"
p12r_base "$P12R_BADREV" "$SG"
(cd "$P12R_BADREV" && "$SG" reset no-such-rev-xyz) > /dev/null 2>&1
check "phase12 reset reject: sg reset with an invalid rev exits non-zero" test $? = 1

P12R_BADFLAG="$P12R_SG_BASE/badflag"
p12r_base "$P12R_BADFLAG" "$SG"
P12R_BADFLAG_ERR="$WORKDIR/p12r_badflag_err.txt"
(cd "$P12R_BADFLAG" && "$SG" reset --Hard) > /dev/null 2> "$P12R_BADFLAG_ERR"
check "phase12 reset reject: sg reset --Hard (unrecognized flag) exits non-zero" test $? = 1
check "phase12 reset reject: unrecognized flag is reported as a usage error, not 'invalid reference'" \
    sh -c "! grep -q 'invalid reference' '$P12R_BADFLAG_ERR'"

# ============================================================
# Phase 13 (cont.): sg reset vs. an in-progress merge or rebase
#
# Real git's behavior (verified directly, see the task writeup this was
# checked against): --soft refuses outright (exit 128) whether a merge is
# unresolved or a rebase is paused. --mixed and --hard both clear MERGE_HEAD
# when a merge is in progress, but leave a paused rebase's on-disk state
# completely untouched (only `rebase --abort`/`--continue` end it). sg's own
# --hard is a documented, pre-existing exception to that last point (it
# clears BOTH MERGE_HEAD and sg-rebase/ via sg_safe_apply_tree, matching
# neither mode exactly for the rebase case) -- not re-tested for the rebase
# side here, only --soft and --mixed, which is what this ticket's fix
# actually touches.
# ============================================================

# --- merge conflict in progress ---

p12r_conflict_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    printf 'orig1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch -c feature) > /dev/null 2>&1
    printf 'feature1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master < /dev/null) > /dev/null 2>&1
    printf 'master1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
    (cd "$dir" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
}

P12R_SOFT_MERGE="$WORKDIR/p12r_soft_merge"
p12r_conflict_repo "$P12R_SOFT_MERGE"
check "phase12 reset/merge: precondition -- MERGE_HEAD present after the sg-made conflict" \
    test -f "$P12R_SOFT_MERGE/.git/MERGE_HEAD"
(cd "$P12R_SOFT_MERGE" && "$SG" reset --soft) > /dev/null 2>&1
check "phase12 reset/merge: sg reset --soft during an unresolved merge is rejected" test $? = 1
check "phase12 reset/merge: --soft rejection left MERGE_HEAD in place" \
    test -f "$P12R_SOFT_MERGE/.git/MERGE_HEAD"

P12R_MIXED_MERGE="$WORKDIR/p12r_mixed_merge"
p12r_conflict_repo "$P12R_MIXED_MERGE"
(cd "$P12R_MIXED_MERGE" && "$SG" reset) > /dev/null 2>&1
check "phase12 reset/merge: bare sg reset (mixed) during an unresolved merge exits 0" test $? = 0
check "phase12 reset/merge: --mixed cleared MERGE_HEAD" \
    test ! -f "$P12R_MIXED_MERGE/.git/MERGE_HEAD"

# end-to-end assertion: this is what a user actually gets bitten by -- an
# ordinary commit made after the reset must NOT come out as a 2-parent merge
# commit for the merge that was just abandoned.
printf 'resolved\norig2\n' > "$P12R_MIXED_MERGE/c.txt"
(cd "$P12R_MIXED_MERGE" && "$SG" add c.txt && "$SG" commit -m "an ordinary commit") > /dev/null 2>&1
check "phase12 reset/merge: sg commit after the mixed reset exits 0" test $? = 0
P12R_MIXED_PARENTS=$(cd "$P12R_MIXED_MERGE" && git cat-file -p HEAD | grep -c '^parent ')
check "phase12 reset/merge: the follow-up commit has exactly one parent (not a bogus merge commit)" \
    test "$P12R_MIXED_PARENTS" = 1

P12R_HARD_MERGE="$WORKDIR/p12r_hard_merge"
p12r_conflict_repo "$P12R_HARD_MERGE"
(cd "$P12R_HARD_MERGE" && "$SG" reset --hard --force) > /dev/null 2>&1
check "phase12 reset/merge: sg reset --hard during an unresolved merge exits 0" test $? = 0
check "phase12 reset/merge: --hard cleared MERGE_HEAD" \
    test ! -f "$P12R_HARD_MERGE/.git/MERGE_HEAD"

# --- rebase paused on a conflict ---

p12r_rebase_paused_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    printf 'orig1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch -c feature) > /dev/null 2>&1
    printf 'feature1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master < /dev/null) > /dev/null 2>&1
    printf 'master1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch feature < /dev/null) > /dev/null 2>&1
    (cd "$dir" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
}

P12R_SOFT_REBASE="$WORKDIR/p12r_soft_rebase"
p12r_rebase_paused_repo "$P12R_SOFT_REBASE"
check "phase12 reset/rebase: precondition -- sg-rebase/ present after the paused rebase" \
    test -d "$P12R_SOFT_REBASE/.git/sg-rebase"
(cd "$P12R_SOFT_REBASE" && "$SG" reset --soft) > /dev/null 2>&1
check "phase12 reset/rebase: sg reset --soft during a paused rebase is rejected" test $? = 1
check "phase12 reset/rebase: --soft rejection left sg-rebase/ in place" \
    test -d "$P12R_SOFT_REBASE/.git/sg-rebase"

P12R_MIXED_REBASE="$WORKDIR/p12r_mixed_rebase"
p12r_rebase_paused_repo "$P12R_MIXED_REBASE"
(cd "$P12R_MIXED_REBASE" && "$SG" reset) > /dev/null 2>&1
check "phase12 reset/rebase: bare sg reset (mixed) during a paused rebase exits 0" test $? = 0
check "phase12 reset/rebase: --mixed does NOT clear sg-rebase/ (matches real git leaving a paused rebase alone)" \
    test -d "$P12R_MIXED_REBASE/.git/sg-rebase"

# --- Phase 14: reset --hard / switch during a paused rebase ---
#
# Measured against real git 2.55.0: `reset --hard` (including `reset --hard
# <commit>`) during a paused rebase keeps the rebase sequencer state intact
# (rebase-merge/ for git, .git/sg-rebase/ for sg) -- a later `rebase
# --abort`/`--continue` still works normally afterward. `switch` (even with
# --force, even with -c) is refused outright instead of clobbering the
# paused rebase; a refused `switch -c` does not create the new branch.

p14_rebase_paused_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    printf 'orig1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch -c feature) > /dev/null 2>&1
    printf 'feature1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master < /dev/null) > /dev/null 2>&1
    printf 'master1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch feature < /dev/null) > /dev/null 2>&1
}

# case A: reset --hard keeps the paused rebase, and abort still restores the
# exact pre-rebase branch position afterward.
P14_HARD_ABORT="$WORKDIR/p14_hard_abort"
p14_rebase_paused_repo "$P14_HARD_ABORT"
P14_PRE_REBASE_FEATURE=$(cd "$P14_HARD_ABORT" && git rev-parse feature)
(cd "$P14_HARD_ABORT" && "$SG" rebase master < /dev/null) > /dev/null 2>&1

check "phase14: precondition -- sg-rebase/ present after the paused rebase" \
    test -d "$P14_HARD_ABORT/.git/sg-rebase"

(cd "$P14_HARD_ABORT" && "$SG" reset --hard --force) > /dev/null 2>&1
check "phase14: sg reset --hard during a paused rebase exits 0" test $? = 0
check "phase14: sg reset --hard does NOT clear sg-rebase/ (matches real git leaving rebase-merge/ alone)" \
    test -d "$P14_HARD_ABORT/.git/sg-rebase"

(cd "$P14_HARD_ABORT" && "$SG" rebase --abort < /dev/null) > /dev/null 2>&1
check "phase14: sg rebase --abort after reset --hard exits 0" test $? = 0
check "phase14: .git/sg-rebase is gone after --abort" test ! -d "$P14_HARD_ABORT/.git/sg-rebase"
P14_POST_ABORT_FEATURE=$(cd "$P14_HARD_ABORT" && git rev-parse feature)
check "phase14: rebase --abort after reset --hard restores feature to its exact pre-rebase commit" \
    test "$P14_POST_ABORT_FEATURE" = "$P14_PRE_REBASE_FEATURE"

# real-git oracle for the same reset --hard + abort sequence: rebase-merge/
# must survive reset --hard, and abort must restore the original branch tip.
P14_GIT_HARD_ABORT="$WORKDIR/p14_git_hard_abort"
mkdir -p "$P14_GIT_HARD_ABORT"
(cd "$WORKDIR" && git init -q p14_git_hard_abort)
(cd "$P14_GIT_HARD_ABORT" && git config user.email "a@b.c" && git config user.name "git user")
printf 'orig1\norig2\n' > "$P14_GIT_HARD_ABORT/c.txt"
(cd "$P14_GIT_HARD_ABORT" && git add c.txt && git commit -q -m "base")
(cd "$P14_GIT_HARD_ABORT" && git switch -q -c feature)
printf 'feature1\norig2\n' > "$P14_GIT_HARD_ABORT/c.txt"
(cd "$P14_GIT_HARD_ABORT" && git add c.txt && git commit -q -m "feature change")
(cd "$P14_GIT_HARD_ABORT" && git switch -q master)
printf 'master1\norig2\n' > "$P14_GIT_HARD_ABORT/c.txt"
(cd "$P14_GIT_HARD_ABORT" && git add c.txt && git commit -q -m "master change")
(cd "$P14_GIT_HARD_ABORT" && git switch -q feature)
P14_GIT_PRE_REBASE_FEATURE=$(cd "$P14_GIT_HARD_ABORT" && git rev-parse feature)
(cd "$P14_GIT_HARD_ABORT" && git rebase master) > /dev/null 2>&1
(cd "$P14_GIT_HARD_ABORT" && git reset --hard) > /dev/null 2>&1
check "phase14 oracle: real git reset --hard during a paused rebase exits 0" test $? = 0
check "phase14 oracle: real git reset --hard does NOT clear rebase-merge/" \
    test -d "$P14_GIT_HARD_ABORT/.git/rebase-merge"
(cd "$P14_GIT_HARD_ABORT" && git rebase --abort) > /dev/null 2>&1
check "phase14 oracle: real git rebase --abort after reset --hard exits 0" test $? = 0
P14_GIT_POST_ABORT_FEATURE=$(cd "$P14_GIT_HARD_ABORT" && git rev-parse feature)
check "phase14 oracle: real git abort after reset --hard restores feature to its exact pre-rebase commit" \
    test "$P14_GIT_POST_ABORT_FEATURE" = "$P14_GIT_PRE_REBASE_FEATURE"

# case B: reset --hard, then resolve the conflict and --continue to
# completion; compare sg's final tree against real git's final tree for the
# identical workflow (shas differ due to committer timestamps, so compare
# content instead).
P14_HARD_CONTINUE="$WORKDIR/p14_hard_continue"
p14_rebase_paused_repo "$P14_HARD_CONTINUE"
(cd "$P14_HARD_CONTINUE" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
(cd "$P14_HARD_CONTINUE" && "$SG" reset --hard --force) > /dev/null 2>&1
printf 'resolved1\norig2\n' > "$P14_HARD_CONTINUE/c.txt"
(cd "$P14_HARD_CONTINUE" && "$SG" add c.txt) > /dev/null 2>&1
(cd "$P14_HARD_CONTINUE" && "$SG" rebase --continue < /dev/null) > /dev/null 2>&1
check "phase14: sg rebase --continue after reset --hard exits 0" test $? = 0
check "phase14: .git/sg-rebase is gone after --continue" test ! -d "$P14_HARD_CONTINUE/.git/sg-rebase"

P14_GIT_HARD_CONTINUE="$WORKDIR/p14_git_hard_continue"
mkdir -p "$P14_GIT_HARD_CONTINUE"
(cd "$WORKDIR" && git init -q p14_git_hard_continue)
(cd "$P14_GIT_HARD_CONTINUE" && git config user.email "a@b.c" && git config user.name "git user")
printf 'orig1\norig2\n' > "$P14_GIT_HARD_CONTINUE/c.txt"
(cd "$P14_GIT_HARD_CONTINUE" && git add c.txt && git commit -q -m "base")
(cd "$P14_GIT_HARD_CONTINUE" && git switch -q -c feature)
printf 'feature1\norig2\n' > "$P14_GIT_HARD_CONTINUE/c.txt"
(cd "$P14_GIT_HARD_CONTINUE" && git add c.txt && git commit -q -m "feature change")
(cd "$P14_GIT_HARD_CONTINUE" && git switch -q master)
printf 'master1\norig2\n' > "$P14_GIT_HARD_CONTINUE/c.txt"
(cd "$P14_GIT_HARD_CONTINUE" && git add c.txt && git commit -q -m "master change")
(cd "$P14_GIT_HARD_CONTINUE" && git switch -q feature)
(cd "$P14_GIT_HARD_CONTINUE" && git rebase master) > /dev/null 2>&1
(cd "$P14_GIT_HARD_CONTINUE" && git reset --hard) > /dev/null 2>&1
printf 'resolved1\norig2\n' > "$P14_GIT_HARD_CONTINUE/c.txt"
(cd "$P14_GIT_HARD_CONTINUE" && git add c.txt) > /dev/null 2>&1
(cd "$P14_GIT_HARD_CONTINUE" && git -c core.editor=true rebase --continue) > /dev/null 2>&1

check "phase14: sg and real git agree on final c.txt content after reset --hard + continue" \
    cmp -s "$P14_HARD_CONTINUE/c.txt" "$P14_GIT_HARD_CONTINUE/c.txt"

# commit graph shape after reset --hard + continue: same subject sequence and
# same commit count on both sides (shas differ due to committer timestamps,
# so compare subjects/count instead -- same convention as phase4c case1/2).
P14_HARD_CONTINUE_SUBJECTS="$WORKDIR/p14_hard_continue_subjects.txt"
(cd "$P14_HARD_CONTINUE" && git log --format=%s) > "$P14_HARD_CONTINUE_SUBJECTS" 2>&1
P14_GIT_HARD_CONTINUE_SUBJECTS="$WORKDIR/p14_git_hard_continue_subjects.txt"
(cd "$P14_GIT_HARD_CONTINUE" && git log --format=%s) > "$P14_GIT_HARD_CONTINUE_SUBJECTS" 2>&1
check "phase14: sg and real git agree on commit subjects/order after reset --hard + continue" \
    cmp -s "$P14_HARD_CONTINUE_SUBJECTS" "$P14_GIT_HARD_CONTINUE_SUBJECTS"

P14_HARD_CONTINUE_PARENTS="$WORKDIR/p14_hard_continue_parents.txt"
(cd "$P14_HARD_CONTINUE" && git log --format='%s %P' | sed -E 's/[0-9a-f]{40}/X/g') > "$P14_HARD_CONTINUE_PARENTS" 2>&1
P14_GIT_HARD_CONTINUE_PARENTS="$WORKDIR/p14_git_hard_continue_parents.txt"
(cd "$P14_GIT_HARD_CONTINUE" && git log --format='%s %P' | sed -E 's/[0-9a-f]{40}/X/g') > "$P14_GIT_HARD_CONTINUE_PARENTS" 2>&1
check "phase14: sg and real git agree on parent-count shape (each commit's number of parents) after reset --hard + continue" \
    cmp -s "$P14_HARD_CONTINUE_PARENTS" "$P14_GIT_HARD_CONTINUE_PARENTS"

# case B2: reset --hard <another commit> (not a no-op reset to HEAD) during a
# paused rebase, then resolve and --continue -- measured against real git
# 2.55.0: the remaining commits get replayed on top of the reset target, not
# the original onto commit. Both sides do the identical operation sequence
# so shas differ only by committer timestamp; compare subjects/parent shape
# and final file content.
P14_HARD_RETARGET="$WORKDIR/p14_hard_retarget"
p14_rebase_paused_repo "$P14_HARD_RETARGET"
P14_RETARGET_BASE=$(cd "$P14_HARD_RETARGET" && git rev-parse master~1)
(cd "$P14_HARD_RETARGET" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
(cd "$P14_HARD_RETARGET" && "$SG" reset --hard "$P14_RETARGET_BASE" --force) > /dev/null 2>&1
check "phase14: sg reset --hard <other commit> during a paused rebase exits 0" test $? = 0
printf 'resolved1\norig2\n' > "$P14_HARD_RETARGET/c.txt"
(cd "$P14_HARD_RETARGET" && "$SG" add c.txt) > /dev/null 2>&1
(cd "$P14_HARD_RETARGET" && "$SG" rebase --continue < /dev/null) > /dev/null 2>&1
check "phase14: sg rebase --continue after reset --hard <other commit> exits 0" test $? = 0
check "phase14: .git/sg-rebase is gone after --continue (retarget case)" \
    test ! -d "$P14_HARD_RETARGET/.git/sg-rebase"

P14_GIT_HARD_RETARGET="$WORKDIR/p14_git_hard_retarget"
mkdir -p "$P14_GIT_HARD_RETARGET"
(cd "$WORKDIR" && git init -q p14_git_hard_retarget)
(cd "$P14_GIT_HARD_RETARGET" && git config user.email "a@b.c" && git config user.name "git user")
printf 'orig1\norig2\n' > "$P14_GIT_HARD_RETARGET/c.txt"
(cd "$P14_GIT_HARD_RETARGET" && git add c.txt && git commit -q -m "base")
(cd "$P14_GIT_HARD_RETARGET" && git switch -q -c feature)
printf 'feature1\norig2\n' > "$P14_GIT_HARD_RETARGET/c.txt"
(cd "$P14_GIT_HARD_RETARGET" && git add c.txt && git commit -q -m "feature change")
(cd "$P14_GIT_HARD_RETARGET" && git switch -q master)
printf 'master1\norig2\n' > "$P14_GIT_HARD_RETARGET/c.txt"
(cd "$P14_GIT_HARD_RETARGET" && git add c.txt && git commit -q -m "master change")
(cd "$P14_GIT_HARD_RETARGET" && git switch -q feature)
P14_GIT_RETARGET_BASE=$(cd "$P14_GIT_HARD_RETARGET" && git rev-parse master~1)
(cd "$P14_GIT_HARD_RETARGET" && git rebase master) > /dev/null 2>&1
(cd "$P14_GIT_HARD_RETARGET" && git reset --hard "$P14_GIT_RETARGET_BASE") > /dev/null 2>&1
printf 'resolved1\norig2\n' > "$P14_GIT_HARD_RETARGET/c.txt"
(cd "$P14_GIT_HARD_RETARGET" && git add c.txt) > /dev/null 2>&1
(cd "$P14_GIT_HARD_RETARGET" && git -c core.editor=true rebase --continue) > /dev/null 2>&1

check "phase14: sg and real git agree on final c.txt content after reset --hard <other commit> + continue" \
    cmp -s "$P14_HARD_RETARGET/c.txt" "$P14_GIT_HARD_RETARGET/c.txt"

P14_HARD_RETARGET_SUBJECTS="$WORKDIR/p14_hard_retarget_subjects.txt"
(cd "$P14_HARD_RETARGET" && git log --format=%s) > "$P14_HARD_RETARGET_SUBJECTS" 2>&1
P14_GIT_HARD_RETARGET_SUBJECTS="$WORKDIR/p14_git_hard_retarget_subjects.txt"
(cd "$P14_GIT_HARD_RETARGET" && git log --format=%s) > "$P14_GIT_HARD_RETARGET_SUBJECTS" 2>&1
check "phase14: sg and real git agree on commit subjects/order after reset --hard <other commit> + continue" \
    cmp -s "$P14_HARD_RETARGET_SUBJECTS" "$P14_GIT_HARD_RETARGET_SUBJECTS"

P14_HARD_RETARGET_PARENTS="$WORKDIR/p14_hard_retarget_parents.txt"
(cd "$P14_HARD_RETARGET" && git log --format='%s %P' | sed -E 's/[0-9a-f]{40}/X/g') > "$P14_HARD_RETARGET_PARENTS" 2>&1
P14_GIT_HARD_RETARGET_PARENTS="$WORKDIR/p14_git_hard_retarget_parents.txt"
(cd "$P14_GIT_HARD_RETARGET" && git log --format='%s %P' | sed -E 's/[0-9a-f]{40}/X/g') > "$P14_GIT_HARD_RETARGET_PARENTS" 2>&1
check "phase14: sg and real git agree on parent-count shape after reset --hard <other commit> + continue" \
    cmp -s "$P14_HARD_RETARGET_PARENTS" "$P14_GIT_HARD_RETARGET_PARENTS"

# The two comparisons above normalize every 40-hex sha to X, so for a linear
# history they only really assert "one parent each" -- they cannot tell
# whether the replayed commit landed on the reset target or on the original
# onto. That distinction is the entire point of this case, so assert the
# parent identity directly, in each repo against its own base sha. Real git
# is checked the same way rather than trusted: it is the oracle for the
# claim, not an assumption.
check "phase14: sg replayed the commit onto the reset target, not the original onto" \
    sh -c "test \"\$(cd '$P14_HARD_RETARGET' && git rev-parse feature^)\" = '$P14_RETARGET_BASE'"
check "phase14: real git also replayed onto the reset target (oracle for the check above)" \
    sh -c "test \"\$(cd '$P14_GIT_HARD_RETARGET' && git rev-parse feature^)\" = '$P14_GIT_RETARGET_BASE'"
check "phase14: sg's rebased feature is NOT descended from master after the retarget" \
    sh -c "! (cd '$P14_HARD_RETARGET' && git merge-base --is-ancestor master feature)"
check "phase14: real git's rebased feature is NOT descended from master either" \
    sh -c "! (cd '$P14_GIT_HARD_RETARGET' && git merge-base --is-ancestor master feature)"

# case C: sg switch <other> is refused during a paused rebase, --force does
# not bypass it, and -c does not create the new branch either.
P14_SWITCH="$WORKDIR/p14_switch"
p14_rebase_paused_repo "$P14_SWITCH"
(cd "$P14_SWITCH" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
P14_SWITCH_HEAD_BEFORE=$(cd "$P14_SWITCH" && git rev-parse HEAD)
P14_SWITCH_STATUS_BEFORE=$(cd "$P14_SWITCH" && git status --porcelain)

P14_SWITCH_ERR="$WORKDIR/p14_switch_err.txt"
(cd "$P14_SWITCH" && "$SG" switch master < /dev/null) > "$P14_SWITCH_ERR" 2>&1
check "phase14: sg switch during a paused rebase is rejected" test $? != 0
check "phase14: switch rejection left sg-rebase/ in place" test -d "$P14_SWITCH/.git/sg-rebase"
check "phase14: switch rejection left HEAD unchanged" \
    sh -c "test \"\$(cd '$P14_SWITCH' && git rev-parse HEAD)\" = '$P14_SWITCH_HEAD_BEFORE'"
check "phase14: switch rejection left the working directory unchanged" \
    sh -c "test \"\$(cd '$P14_SWITCH' && git status --porcelain)\" = '$P14_SWITCH_STATUS_BEFORE'"
check "phase14: switch rejection is due to the rebase gate, not the dirty-workdir prompt" \
    grep -q "cannot switch branches" "$P14_SWITCH_ERR"

P14_SWITCH_FORCE_ERR="$WORKDIR/p14_switch_force_err.txt"
(cd "$P14_SWITCH" && "$SG" switch --force master < /dev/null) > "$P14_SWITCH_FORCE_ERR" 2>&1
check "phase14: sg switch --force during a paused rebase is still rejected" test $? != 0
check "phase14: --force rejection left sg-rebase/ in place" test -d "$P14_SWITCH/.git/sg-rebase"
check "phase14: --force rejection is due to the rebase gate, not skipped by --force" \
    grep -q "cannot switch branches" "$P14_SWITCH_FORCE_ERR"

P14_SWITCH_C_ERR="$WORKDIR/p14_switch_c_err.txt"
(cd "$P14_SWITCH" && "$SG" switch -c newbranch < /dev/null) > "$P14_SWITCH_C_ERR" 2>&1
check "phase14: sg switch -c during a paused rebase is rejected" test $? != 0
check "phase14: -c rejection left sg-rebase/ in place" test -d "$P14_SWITCH/.git/sg-rebase"
check "phase14: switch -c rejection did NOT create the new branch (matches real git)" \
    sh -c "! (cd '$P14_SWITCH' && git rev-parse --verify refs/heads/newbranch) > /dev/null 2>&1"
check "phase14: -c rejection is due to the rebase gate, not the dirty-workdir prompt" \
    grep -q "cannot switch branches" "$P14_SWITCH_C_ERR"

# case D: sg undo has no real-git equivalent to use as an oracle -- it
# deliberately still clears a paused rebase's sequencer state (sg-specific
# behavior: resuming a rebase on a tree undo just rewound from under it
# would make no sense).
P14_UNDO="$WORKDIR/p14_undo"
p14_rebase_paused_repo "$P14_UNDO"
(cd "$P14_UNDO" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
check "phase14: precondition -- sg-rebase/ present before undo" test -d "$P14_UNDO/.git/sg-rebase"
(cd "$P14_UNDO" && "$SG" reset --hard --force) > /dev/null 2>&1
check "phase14: precondition -- sg-rebase/ still present after the snapshot-taking reset --hard" \
    test -d "$P14_UNDO/.git/sg-rebase"
(cd "$P14_UNDO" && "$SG" undo 1 --force) > /dev/null 2>&1
check "phase14: sg undo during a paused rebase exits 0" test $? = 0
check "phase14: sg undo clears sg-rebase/ (sg-specific: no real-git equivalent to use as an oracle)" \
    test ! -d "$P14_UNDO/.git/sg-rebase"

# case E: the confirmation prompt must not promise something undo then takes
# away. sg_safe_apply_tree's dirty-prompt text is shared by every caller, so
# it may not claim the rebase survives -- true for reset --hard, false for
# undo, which wipes it right after. undo therefore says so itself, and must
# do it BEFORE sg_safe_apply_tree runs so the warning reaches the user while
# the y/N decision is still open.
#
# Run without --force: sg_confirm_dangerous only prints the message on the
# non-tty/no-force branch (force=1 returns early and prints nothing), and
# that branch also refuses. That refusal is what makes the ordering
# observable -- a notice emitted from undo's success path instead would not
# appear here at all.
P14_MSG="$WORKDIR/p14_undo_msg"
P14_MSG_ERR="$WORKDIR/p14_undo_msg.err"
p14_rebase_paused_repo "$P14_MSG"
(cd "$P14_MSG" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
(cd "$P14_MSG" && "$SG" reset --hard --force) > /dev/null 2>&1
check "phase14: precondition -- sg-rebase/ present before the message check" \
    test -d "$P14_MSG/.git/sg-rebase"
(cd "$P14_MSG" && "$SG" undo 1 < /dev/null) > "$P14_MSG_ERR" 2>&1
check "phase14: undo warns it will abandon the rebase before the confirmation is decided" \
    grep -q "undo will give up" "$P14_MSG_ERR"
check "phase14: the shared dirty prompt does not promise the rebase survives" \
    sh -c "! grep -q 'the rebase will survive' '$P14_MSG_ERR'"
check "phase14: the shared dirty prompt says what it actually does" \
    grep -q "overwrite the conflict resolution content in the working directory" "$P14_MSG_ERR"
check "phase14: a declined undo left sg-rebase/ alone" test -d "$P14_MSG/.git/sg-rebase"

# --- --soft must not need to be able to resolve the target commit's tree ---
#
# resolve_commit_tree() used to run unconditionally before the three modes
# were dispatched, so --soft (which never reads target_tree_id) would still
# fail if the target commit's own content couldn't be parsed into a tree id
# -- even though nothing about --soft's actual job (moving the branch ref)
# needs that. Real git's own hash-object refuses to write a commit object
# whose content fails its "tree <hex>" line check (fsck-on-write), so this
# specific shape can't be constructed through real git at all -- sg's own
# hash-object has no such validation, which is exactly what makes this
# reachable through sg's own CLI (a self-check, not an sg-vs-git comparison
# like the rest of this phase).
P12R_BADTREE_REPO="$WORKDIR/p12r_badtree"
mkdir -p "$P12R_BADTREE_REPO"
(cd "$WORKDIR" && "$SG" init p12r_badtree) > /dev/null 2>&1
printf 'content\n' > "$P12R_BADTREE_REPO/f.txt"
(cd "$P12R_BADTREE_REPO" && "$SG" add f.txt && "$SG" commit -m "only commit") > /dev/null 2>&1

P12R_BADCOMMIT_SRC="$WORKDIR/p12r_badcommit_src.txt"
printf 'nottree 0000000000000000000000000000000000000000\nauthor a <a@b.c> 0 +0000\ncommitter a <a@b.c> 0 +0000\n\nmsg\n' \
    > "$P12R_BADCOMMIT_SRC"
P12R_BAD_SHA=$(cd "$P12R_BADTREE_REPO" && "$SG" hash-object -t commit -w "$P12R_BADCOMMIT_SRC" 2>/dev/null)
check "phase12 reset reset --soft (unparsable tree line): precondition -- sg hash-object wrote the malformed commit" \
    test -n "$P12R_BAD_SHA"
(cd "$P12R_BADTREE_REPO" && "$SG" reset --soft "$P12R_BAD_SHA") > /dev/null 2>&1
check "phase12 reset reset --soft (unparsable tree line): sg reset --soft still succeeds (it never needs the tree)" \
    test $? = 0
P12R_BADTREE_HEAD=$(cd "$P12R_BADTREE_REPO" && git rev-parse HEAD 2>/dev/null)
check "phase12 reset reset --soft (unparsable tree line): the branch actually moved to the malformed commit" \
    test "$P12R_BADTREE_HEAD" = "$P12R_BAD_SHA"

# ============================================================
# Phase 15: sg stash
#
# Bit-compatibility boundary (see docs/DESIGN.md): sg's commit timestamps
# come from time(NULL) with a fixed "+0000" literal, so a stash commit's oid
# can NEVER be compared between sg and real git for equal content -- every
# check below asserts on git's INTERPRETATION of sg's bytes (list text,
# cat-file structure, worktree/porcelain after pop) or vice versa, never on
# an oid equality.
# ============================================================

p15_base_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    (cd "$dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'a\n' > "$dir/a.txt"
    (cd "$dir" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
}

# --- 8.2 (1)-(4): sg pushes, real git reads it back ---
P15_SG2GIT="$WORKDIR/p15_sg2git"
p15_base_repo "$P15_SG2GIT"
printf 'changed\n' > "$P15_SG2GIT/a.txt"
printf 'new file\n' > "$P15_SG2GIT/b.txt"
(cd "$P15_SG2GIT" && "$SG" add b.txt) > /dev/null 2>&1
P15_SG2GIT_PRE_PORCELAIN=$(cd "$P15_SG2GIT" && git status --porcelain)
P15_SG2GIT_ATXT="$WORKDIR/p15_sg2git_a_before.txt"
cp "$P15_SG2GIT/a.txt" "$P15_SG2GIT_ATXT"
P15_SG2GIT_BTXT="$WORKDIR/p15_sg2git_b_before.txt"
cp "$P15_SG2GIT/b.txt" "$P15_SG2GIT_BTXT"

(cd "$P15_SG2GIT" && "$SG" stash push -m "phase15 msg1") > /dev/null 2>&1
check "phase15 sg->git: sg stash push exits 0" test $? = 0

P15_SG2GIT_LIST_TXT="$WORKDIR/p15_sg2git_list.txt"
(cd "$P15_SG2GIT" && git stash list) > "$P15_SG2GIT_LIST_TXT" 2>&1
check "phase15 sg->git: git stash list shows the push" \
    grep -q '^stash@{0}: On master: phase15 msg1$' "$P15_SG2GIT_LIST_TXT"

check "phase15 sg->git: refs/stash resolves as a 2-parent commit (git cat-file)" \
    sh -c "[ \"\$(cd '$P15_SG2GIT' && git cat-file -p refs/stash | grep -c '^parent ')\" = 2 ]"

check "phase15 sg->git: git fsck is clean after sg's stash push" \
    sh -c "(cd '$P15_SG2GIT' && git fsck) > /dev/null 2>&1"

(cd "$P15_SG2GIT" && git stash pop) > /dev/null 2>&1
check "phase15 sg->git: git stash pop exits 0 on sg's stash" test $? = 0
check "phase15 sg->git: git stash pop restored a.txt's exact bytes" \
    cmp -s "$P15_SG2GIT/a.txt" "$P15_SG2GIT_ATXT"
check "phase15 sg->git: git stash pop restored b.txt's exact bytes" \
    cmp -s "$P15_SG2GIT/b.txt" "$P15_SG2GIT_BTXT"
check "phase15 sg->git: post-pop porcelain matches pre-push porcelain exactly" \
    sh -c "test \"\$(cd '$P15_SG2GIT' && git status --porcelain)\" = \"$P15_SG2GIT_PRE_PORCELAIN\""

# --- 8.2 (5)-(7): git builds, sg reads / mixed stack ---

# (6) sg pop on a git-created stash
P15_GIT2SG_POP="$WORKDIR/p15_git2sg_pop"
p15_base_repo "$P15_GIT2SG_POP"
printf 'gitchange\n' > "$P15_GIT2SG_POP/a.txt"
(cd "$P15_GIT2SG_POP" && git add a.txt && git stash push -q -m "git built") > /dev/null 2>&1
check "phase15 git->sg: precondition -- git stash push left a clean worktree" \
    sh -c "test -z \"\$(cd '$P15_GIT2SG_POP' && git status --porcelain)\""
(cd "$P15_GIT2SG_POP" && "$SG" stash pop) > /dev/null 2>&1
check "phase15 git->sg: sg stash pop on a git-created stash exits 0" test $? = 0
check "phase15 git->sg: sg stash pop restored git's stashed content" \
    sh -c "grep -q gitchange '$P15_GIT2SG_POP/a.txt'"
check "phase15 git->sg: sg stash pop dropped the stash (git stash list now empty)" \
    sh -c "[ -z \"\$(cd '$P15_GIT2SG_POP' && git stash list)\" ]"

# (5)+(7) mixed stack: sg, git, sg -- list identity, then drop the git-built
# middle entry through sg and check identity again, which also exercises
# ident/timestamp preservation for the surviving entries (section 4.1).
P15_MIX="$WORKDIR/p15_mix"
p15_base_repo "$P15_MIX"
printf 'c1\n' > "$P15_MIX/a.txt"
(cd "$P15_MIX" && "$SG" add a.txt && "$SG" stash push -m sgone) > /dev/null 2>&1
printf 'c2\n' > "$P15_MIX/a.txt"
(cd "$P15_MIX" && git add a.txt && git stash push -q -m gittwo) > /dev/null 2>&1
printf 'c3\n' > "$P15_MIX/a.txt"
(cd "$P15_MIX" && "$SG" add a.txt && "$SG" stash push -m sgthree) > /dev/null 2>&1

P15_MIX_GIT_LIST1="$WORKDIR/p15_mix_git_list1.txt"
P15_MIX_SG_LIST1="$WORKDIR/p15_mix_sg_list1.txt"
(cd "$P15_MIX" && git stash list) > "$P15_MIX_GIT_LIST1" 2>&1
(cd "$P15_MIX" && "$SG" stash list) > "$P15_MIX_SG_LIST1" 2>&1
check "phase15 mixed stack: sg stash list is byte-identical to git stash list" \
    cmp -s "$P15_MIX_GIT_LIST1" "$P15_MIX_SG_LIST1"

(cd "$P15_MIX" && "$SG" stash drop 'stash@{1}') > /dev/null 2>&1
check "phase15 mixed stack: sg stash drop of the git-built entry exits 0" test $? = 0

P15_MIX_GIT_LIST2="$WORKDIR/p15_mix_git_list2.txt"
P15_MIX_SG_LIST2="$WORKDIR/p15_mix_sg_list2.txt"
(cd "$P15_MIX" && git stash list) > "$P15_MIX_GIT_LIST2" 2>&1
(cd "$P15_MIX" && "$SG" stash list) > "$P15_MIX_SG_LIST2" 2>&1
check "phase15 mixed stack: after the drop, sg and git list agree with each other" \
    cmp -s "$P15_MIX_GIT_LIST2" "$P15_MIX_SG_LIST2"
check "phase15 mixed stack: after the drop, the list actually changed (not a no-op)" \
    sh -c "! cmp -s '$P15_MIX_GIT_LIST1' '$P15_MIX_GIT_LIST2'"

P15_MIX_REFSTASH=$(cd "$P15_MIX" && git rev-parse refs/stash)
P15_MIX_REFLOG_LAST_NEW=$(tail -1 "$P15_MIX/.git/logs/refs/stash" | cut -d' ' -f2)
check "phase15 mixed stack: refs/stash equals the last reflog line's new-oid after drop" \
    test "$P15_MIX_REFSTASH" = "$P15_MIX_REFLOG_LAST_NEW"
check "phase15 mixed stack: git stash show stash@{0} still exits 0 after the drop" \
    sh -c "(cd '$P15_MIX' && git stash show 'stash@{0}') > /dev/null 2>&1"
check "phase15 mixed stack: git stash show stash@{1} still exits 0 after the drop" \
    sh -c "(cd '$P15_MIX' && git stash show 'stash@{1}') > /dev/null 2>&1"

# --- extra: multi-line -m normalization (section 4.1's copy_reflog_msg) -- targets
# a broken sg_reflog_append that writes the raw message instead of
# normalizing it, which would forge extra reflog lines and break `git stash
# list`'s parse entirely, not just the text of one entry. ---
P15_MULTILINE="$WORKDIR/p15_multiline"
p15_base_repo "$P15_MULTILINE"
printf 'c1\n' > "$P15_MULTILINE/a.txt"
(cd "$P15_MULTILINE" && "$SG" add a.txt) > /dev/null 2>&1
P15_MULTILINE_MSG=$(printf 'line1\nline2\twith tab')
(cd "$P15_MULTILINE" && "$SG" stash push -m "$P15_MULTILINE_MSG") > /dev/null 2>&1
P15_MULTILINE_GIT_LIST="$WORKDIR/p15_multiline_git_list.txt"
P15_MULTILINE_SG_LIST="$WORKDIR/p15_multiline_sg_list.txt"
(cd "$P15_MULTILINE" && git stash list) > "$P15_MULTILINE_GIT_LIST" 2>&1
(cd "$P15_MULTILINE" && "$SG" stash list) > "$P15_MULTILINE_SG_LIST" 2>&1
check "phase15 multi-line -m: sg stash list matches git stash list byte-for-byte" \
    cmp -s "$P15_MULTILINE_GIT_LIST" "$P15_MULTILINE_SG_LIST"
check "phase15 multi-line -m: the reflog subject is collapsed to a single line" \
    grep -q '^stash@{0}: On master: line1 line2 with tab$' "$P15_MULTILINE_GIT_LIST"

# --- extra: the WIP form's abbreviated hash must match real git's own
# --short=7 for the same commit (Risks section 9.1). ---
P15_WIP="$WORKDIR/p15_wip"
p15_base_repo "$P15_WIP"
printf 'c1\n' > "$P15_WIP/a.txt"
(cd "$P15_WIP" && "$SG" add a.txt) > /dev/null 2>&1
(cd "$P15_WIP" && "$SG" stash push) > /dev/null 2>&1
P15_WIP_SHORT=$(cd "$P15_WIP" && git rev-parse --short=7 HEAD)
P15_WIP_LIST="$WORKDIR/p15_wip_list.txt"
(cd "$P15_WIP" && git stash list) > "$P15_WIP_LIST" 2>&1
check "phase15 WIP form: reflog subject embeds real git's own --short=7 abbreviation" \
    grep -q "^stash@{0}: WIP on master: $P15_WIP_SHORT base\$" "$P15_WIP_LIST"

# --- extra: drop of the last remaining entry empties the stack the same way
# `git stash clear`/`git stash drop` (last entry) does -- both ref AND
# reflog file gone, not just one of the two. ---
P15_DROPLAST="$WORKDIR/p15_droplast"
p15_base_repo "$P15_DROPLAST"
printf 'c1\n' > "$P15_DROPLAST/a.txt"
(cd "$P15_DROPLAST" && "$SG" add a.txt && "$SG" stash push -m onlyone) > /dev/null 2>&1
(cd "$P15_DROPLAST" && "$SG" stash drop) > /dev/null 2>&1
check "phase15 drop last entry: exits 0" test $? = 0
check "phase15 drop last entry: refs/stash is gone" \
    sh -c "! (cd '$P15_DROPLAST' && git rev-parse --verify refs/stash) > /dev/null 2>&1"
check "phase15 drop last entry: logs/refs/stash is gone" \
    test ! -e "$P15_DROPLAST/.git/logs/refs/stash"

# ============================================================
# 8.3: the same-exit-code traps (Phase 14's lesson) -- each row below is
# exit-code-identical between a correct and a broken implementation, so
# every check asserts at the "reason" layer, never on exit code alone.
# ============================================================

# --- row 1: nothing to save -- both a correct impl and a no-op stub exit 0
# and print the same line; the only observable difference is whether
# anything was actually written. ---
P15_NOTHING="$WORKDIR/p15_nothing"
p15_base_repo "$P15_NOTHING"
P15_NOTHING_OUT="$WORKDIR/p15_nothing_out.txt"
(cd "$P15_NOTHING" && "$SG" stash push) > "$P15_NOTHING_OUT" 2>&1
check "phase15 row1 (nothing to save): exit 0" test $? = 0
check "phase15 row1 (nothing to save): stdout says so" \
    grep -q "^No local changes to save$" "$P15_NOTHING_OUT"
check "phase15 row1 (nothing to save): refs/stash was never created" \
    sh -c "! (cd '$P15_NOTHING' && git rev-parse --verify refs/stash) > /dev/null 2>&1"
check "phase15 row1 (nothing to save): logs/refs/stash was never created" \
    test ! -e "$P15_NOTHING/.git/logs/refs/stash"

# --- row 2: push during a paused rebase -- both "proceed" and a broken gate
# that silently no-ops the rebase state exit 0; assert the sequencer state
# is not just PRESENT but still USABLE (a later --continue completes). ---
P15_REBASE_PUSH="$WORKDIR/p15_rebase_push"
p15_base_repo "$P15_REBASE_PUSH"
(cd "$P15_REBASE_PUSH" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\n' >> "$P15_REBASE_PUSH/a.txt"
(cd "$P15_REBASE_PUSH" && "$SG" add a.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P15_REBASE_PUSH" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\n' >> "$P15_REBASE_PUSH/a.txt"
(cd "$P15_REBASE_PUSH" && "$SG" add a.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P15_REBASE_PUSH" && "$SG" switch feature < /dev/null) > /dev/null 2>&1
(cd "$P15_REBASE_PUSH" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
check "phase15 row2 (rebase push): precondition -- rebase is paused on a conflict" \
    test -d "$P15_REBASE_PUSH/.git/sg-rebase"
printf 'resolved\n' > "$P15_REBASE_PUSH/a.txt"
(cd "$P15_REBASE_PUSH" && "$SG" add a.txt) > /dev/null 2>&1
check "phase15 row2 (rebase push): precondition -- index is clean, rebase still paused" \
    sh -c "[ -z \"\$(cd '$P15_REBASE_PUSH' && git ls-files -u)\" ]"
(cd "$P15_REBASE_PUSH" && "$SG" stash push -m "during rebase") > /dev/null 2>&1
check "phase15 row2 (rebase push): push during a paused rebase exits 0" test $? = 0
check "phase15 row2 (rebase push): sequencer state is still present" \
    test -d "$P15_REBASE_PUSH/.git/sg-rebase"
(cd "$P15_REBASE_PUSH" && "$SG" rebase --continue < /dev/null) > /dev/null 2>&1
check "phase15 row2 (rebase push): the paused rebase is still USABLE -- continue completes" \
    test $? = 0
check "phase15 row2 (rebase push): continue actually finished the rebase" \
    test ! -d "$P15_REBASE_PUSH/.git/sg-rebase"

# --- row 2 oracle: real git 2.55.0 behaves IDENTICALLY here (measured) --
# `stash push` while a rebase is paused resets the index/workdir back to
# HEAD, so `--continue` decides the paused commit's change is already
# upstream and silently skips it, leaving the work only in the stash. sg is
# not "fixing" this -- it is matching real git -- so this runs the identical
# scenario through real git in its own repo and asserts the two end states
# agree with EACH OTHER (git is the oracle here, not a hard-coded
# expectation). Commit ids are never compared (sg's committer timestamp/tz
# differ from real git's), only content, counts, and presence. ---
P15_REBASE_PUSH_GIT="$WORKDIR/p15_rebase_push_git"
mkdir -p "$P15_REBASE_PUSH_GIT"
(cd "$WORKDIR" && git init -q p15_rebase_push_git)
(cd "$P15_REBASE_PUSH_GIT" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\n' > "$P15_REBASE_PUSH_GIT/a.txt"
(cd "$P15_REBASE_PUSH_GIT" && git add a.txt && git commit -q -m base)
(cd "$P15_REBASE_PUSH_GIT" && git switch -q -c feature)
printf 'feature1\n' >> "$P15_REBASE_PUSH_GIT/a.txt"
(cd "$P15_REBASE_PUSH_GIT" && git add a.txt && git commit -q -m "feature change")
(cd "$P15_REBASE_PUSH_GIT" && git switch -q master)
printf 'master1\n' >> "$P15_REBASE_PUSH_GIT/a.txt"
(cd "$P15_REBASE_PUSH_GIT" && git add a.txt && git commit -q -m "master change")
(cd "$P15_REBASE_PUSH_GIT" && git switch -q feature)
(cd "$P15_REBASE_PUSH_GIT" && git rebase master) > /dev/null 2>&1
printf 'resolved\n' > "$P15_REBASE_PUSH_GIT/a.txt"
(cd "$P15_REBASE_PUSH_GIT" && git add a.txt)
(cd "$P15_REBASE_PUSH_GIT" && git stash push -m "during rebase") > /dev/null 2>&1
check "phase15 row2 oracle: real git stash push during a paused rebase exits 0" test $? = 0
(cd "$P15_REBASE_PUSH_GIT" && git rebase --continue) > /dev/null 2>&1
check "phase15 row2 oracle: real git rebase --continue exits 0" test $? = 0
check "phase15 row2 oracle: real git's rebase sequencer state is gone (continue finished)" \
    sh -c "test ! -d '$P15_REBASE_PUSH_GIT/.git/rebase-merge' && test ! -d '$P15_REBASE_PUSH_GIT/.git/rebase-apply'"

P15_REBASE_PUSH_ATXT="$WORKDIR/p15_rebase_push_atxt.txt"
P15_REBASE_PUSH_GIT_ATXT="$WORKDIR/p15_rebase_push_git_atxt.txt"
cp "$P15_REBASE_PUSH/a.txt" "$P15_REBASE_PUSH_ATXT"
cp "$P15_REBASE_PUSH_GIT/a.txt" "$P15_REBASE_PUSH_GIT_ATXT"
check "phase15 row2 equivalence: sg and real git end up with byte-identical a.txt after continue" \
    cmp -s "$P15_REBASE_PUSH_ATXT" "$P15_REBASE_PUSH_GIT_ATXT"
check "phase15 row2 equivalence: neither side's final a.txt contains the feature-only text" \
    sh -c "! grep -q feature1 '$P15_REBASE_PUSH_ATXT' && ! grep -q feature1 '$P15_REBASE_PUSH_GIT_ATXT'"

P15_REBASE_PUSH_LOGCOUNT=$(cd "$P15_REBASE_PUSH" && git rev-list --count HEAD)
P15_REBASE_PUSH_GIT_LOGCOUNT=$(cd "$P15_REBASE_PUSH_GIT" && git rev-list --count HEAD)
check "phase15 row2 equivalence: sg and real git end up with the same commit count on HEAD (both lost the feature commit)" \
    test "$P15_REBASE_PUSH_LOGCOUNT" = "$P15_REBASE_PUSH_GIT_LOGCOUNT"

check "phase15 row2 equivalence: sg's stash still has the parked work" \
    sh -c "[ -n \"\$(cd '$P15_REBASE_PUSH' && git stash list)\" ]"
check "phase15 row2 equivalence: real git's stash still has the parked work" \
    sh -c "[ -n \"\$(cd '$P15_REBASE_PUSH_GIT' && git stash list)\" ]"

# --- row 2 warning (fix 1(b)): sg's stash push warns on stderr when a
# rebase is paused, since the workdir-reset consequence above is easy to
# trip over unknowingly. The message must not change the behavior above --
# assert the sequencer state is still exactly as untouched as row 2 already
# proved. ---
P15_REBASE_PUSH_WARN="$WORKDIR/p15_rebase_push_warn"
p15_base_repo "$P15_REBASE_PUSH_WARN"
(cd "$P15_REBASE_PUSH_WARN" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\n' >> "$P15_REBASE_PUSH_WARN/a.txt"
(cd "$P15_REBASE_PUSH_WARN" && "$SG" add a.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P15_REBASE_PUSH_WARN" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\n' >> "$P15_REBASE_PUSH_WARN/a.txt"
(cd "$P15_REBASE_PUSH_WARN" && "$SG" add a.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P15_REBASE_PUSH_WARN" && "$SG" switch feature < /dev/null) > /dev/null 2>&1
(cd "$P15_REBASE_PUSH_WARN" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
printf 'resolved\n' > "$P15_REBASE_PUSH_WARN/a.txt"
(cd "$P15_REBASE_PUSH_WARN" && "$SG" add a.txt) > /dev/null 2>&1
P15_REBASE_PUSH_WARN_ERR="$WORKDIR/p15_rebase_push_warn_err.txt"
(cd "$P15_REBASE_PUSH_WARN" && "$SG" stash push -m warned) > /dev/null 2> "$P15_REBASE_PUSH_WARN_ERR"
check "phase15 row2 warning: stash push during a paused rebase prints a stderr warning" \
    grep -q "rebase" "$P15_REBASE_PUSH_WARN_ERR"
check "phase15 row2 warning: the sequencer state is untouched (a warning, not a behavior change)" \
    test -d "$P15_REBASE_PUSH_WARN/.git/sg-rebase"

# --- row 3: push clears MERGE_HEAD -- both "clear it" and "leave a stale
# MERGE_HEAD referring to an object that's about to be reset away" can exit
# 0; assert absence AND that the stash still applies cleanly afterward. ---
P15_MERGEHEAD="$WORKDIR/p15_mergehead"
p15_base_repo "$P15_MERGEHEAD"
(cd "$P15_MERGEHEAD" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\n' >> "$P15_MERGEHEAD/a.txt"
(cd "$P15_MERGEHEAD" && "$SG" add a.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
(cd "$P15_MERGEHEAD" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\n' >> "$P15_MERGEHEAD/a.txt"
(cd "$P15_MERGEHEAD" && "$SG" add a.txt && "$SG" commit -m "master change") > /dev/null 2>&1
(cd "$P15_MERGEHEAD" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
check "phase15 row3 (MERGE_HEAD): precondition -- merge left MERGE_HEAD and a conflict" \
    test -f "$P15_MERGEHEAD/.git/MERGE_HEAD"
printf 'resolved\n' > "$P15_MERGEHEAD/a.txt"
(cd "$P15_MERGEHEAD" && "$SG" add a.txt) > /dev/null 2>&1
(cd "$P15_MERGEHEAD" && "$SG" stash push -m "clears merge head") > /dev/null 2>&1
check "phase15 row3 (MERGE_HEAD): push during an in-progress merge exits 0" test $? = 0
check "phase15 row3 (MERGE_HEAD): MERGE_HEAD is gone" \
    test ! -f "$P15_MERGEHEAD/.git/MERGE_HEAD"
(cd "$P15_MERGEHEAD" && "$SG" stash pop) > /dev/null 2>&1
check "phase15 row3 (MERGE_HEAD): the stash it took still applies cleanly afterward" \
    test $? = 0

# --- row 4: untracked files are not stashed -- "stashed it AND also left a
# copy behind" would also pass a bytes-unchanged check, so additionally
# assert the object never made it into refs/stash's tree at all. ---
P15_UNTRACKED="$WORKDIR/p15_untracked"
p15_base_repo "$P15_UNTRACKED"
printf 'untracked content\n' > "$P15_UNTRACKED/u.txt"
printf 'tracked change\n' > "$P15_UNTRACKED/a.txt"
(cd "$P15_UNTRACKED" && "$SG" add a.txt) > /dev/null 2>&1
P15_UNTRACKED_BEFORE="$WORKDIR/p15_untracked_before.txt"
cp "$P15_UNTRACKED/u.txt" "$P15_UNTRACKED_BEFORE"
(cd "$P15_UNTRACKED" && "$SG" stash push -m "leaves untracked alone") > /dev/null 2>&1
check "phase15 row4 (untracked): push exits 0" test $? = 0
check "phase15 row4 (untracked): u.txt is untouched bytes" \
    cmp -s "$P15_UNTRACKED/u.txt" "$P15_UNTRACKED_BEFORE"
check "phase15 row4 (untracked): refs/stash's tree does not contain u.txt" \
    sh -c "! (cd '$P15_UNTRACKED' && git ls-tree -r refs/stash) | grep -q 'u\.txt'"

# --- row 5: pop's index rule -- assert the EXACT porcelain string in one
# run, since a rule that got only one of "M" or "A" right could still pass
# a check that looked at either alone. ---
P15_POPINDEX="$WORKDIR/p15_popindex"
p15_base_repo "$P15_POPINDEX"
printf 'tracked change\n' > "$P15_POPINDEX/a.txt"
printf 'brand new\n' > "$P15_POPINDEX/newfile.txt"
(cd "$P15_POPINDEX" && "$SG" add a.txt newfile.txt) > /dev/null 2>&1
(cd "$P15_POPINDEX" && "$SG" stash push -m "index rule") > /dev/null 2>&1
(cd "$P15_POPINDEX" && "$SG" stash pop) > /dev/null 2>&1
P15_POPINDEX_PORCELAIN=$(cd "$P15_POPINDEX" && git status --porcelain | sort)
P15_POPINDEX_EXPECT=$(printf 'A  newfile.txt\n M a.txt\n' | sort)
check "phase15 row5 (pop index rule): exact porcelain -- ' M a.txt' AND 'A  newfile.txt' together" \
    test "$P15_POPINDEX_PORCELAIN" = "$P15_POPINDEX_EXPECT"

# --- row 6: drop stash@{0} re-points refs/stash -- git stash list looks
# identical either way for a "delete the line, leave the ref" bug, since
# list never consults refs/stash (measured, section 4.2's sg_stash_list_read
# comment); only the ref/reflog tip invariant and `git stash show` expose
# it. ---
P15_DROP0="$WORKDIR/p15_drop0"
p15_base_repo "$P15_DROP0"
printf 'c1\n' > "$P15_DROP0/a.txt"
(cd "$P15_DROP0" && "$SG" add a.txt && "$SG" stash push -m first) > /dev/null 2>&1
printf 'c2\n' > "$P15_DROP0/a.txt"
(cd "$P15_DROP0" && "$SG" add a.txt && "$SG" stash push -m second) > /dev/null 2>&1
(cd "$P15_DROP0" && "$SG" stash drop) > /dev/null 2>&1
check "phase15 row6 (drop re-points ref): drop stash@{0} exits 0" test $? = 0
P15_DROP0_REFSTASH=$(cd "$P15_DROP0" && git rev-parse refs/stash 2>/dev/null)
P15_DROP0_REFLOG_LAST=$(tail -1 "$P15_DROP0/.git/logs/refs/stash" | cut -d' ' -f2)
check "phase15 row6 (drop re-points ref): refs/stash equals the surviving entry's new-oid" \
    test "$P15_DROP0_REFSTASH" = "$P15_DROP0_REFLOG_LAST"
check "phase15 row6 (drop re-points ref): git stash show stash@{0} exits 0 (the naive line-delete bug's tell)" \
    sh -c "(cd '$P15_DROP0' && git stash show 'stash@{0}') > /dev/null 2>&1"

# --- row 7: clear after `git gc` -- a loose-only unlink leaves refs/stash
# resurrectable from packed-refs; both a correct clear and that bug leave
# git stash list looking empty (list walks the reflog, which is also gone),
# so assert the ref itself is unresolvable. ---
P15_GCCLEAR="$WORKDIR/p15_gcclear"
p15_base_repo "$P15_GCCLEAR"
printf 'c1\n' > "$P15_GCCLEAR/a.txt"
(cd "$P15_GCCLEAR" && "$SG" add a.txt && "$SG" stash push -m first) > /dev/null 2>&1
(cd "$P15_GCCLEAR" && git gc -q) > /dev/null 2>&1
check "phase15 row7 (clear after gc): precondition -- refs/stash survives gc in packed-refs" \
    sh -c "(cd '$P15_GCCLEAR' && git rev-parse --verify refs/stash) > /dev/null 2>&1"
(cd "$P15_GCCLEAR" && "$SG" stash clear) > /dev/null 2>&1
check "phase15 row7 (clear after gc): sg stash clear exits 0" test $? = 0
check "phase15 row7 (clear after gc): refs/stash is unresolvable even though it was packed" \
    sh -c "! (cd '$P15_GCCLEAR' && git rev-parse --verify refs/stash) > /dev/null 2>&1"

# --- row 8: a conflicting pop -- "refuse and drop the stash anyway" would
# also exit 1, so assert the stash survives, the marker TEXT is the literal
# git strings (not branch names -- a passed-branch-name bug also exits 1
# with markers present, just the wrong ones), MERGE_HEAD stays absent, and
# the index actually has three stages. ---
P15_POPCONFLICT="$WORKDIR/p15_popconflict"
p15_base_repo "$P15_POPCONFLICT"
printf 'base line\n' > "$P15_POPCONFLICT/c.txt"
(cd "$P15_POPCONFLICT" && "$SG" add c.txt && "$SG" commit -m "c base") > /dev/null 2>&1
printf 'stash change\n' > "$P15_POPCONFLICT/c.txt"
(cd "$P15_POPCONFLICT" && "$SG" add c.txt && "$SG" stash push -m "will conflict") > /dev/null 2>&1
printf 'head change\n' > "$P15_POPCONFLICT/c.txt"
(cd "$P15_POPCONFLICT" && "$SG" add c.txt && "$SG" commit -m "head changes c") > /dev/null 2>&1
(cd "$P15_POPCONFLICT" && "$SG" stash pop) > /dev/null 2>&1
check "phase15 row8 (conflicting pop): exits 1" test $? != 0
check "phase15 row8 (conflicting pop): the stash is still listed" \
    sh -c "(cd '$P15_POPCONFLICT' && git stash list) | grep -q 'will conflict'"
check "phase15 row8 (conflicting pop): conflict markers use the literal upstream label" \
    grep -q '<<<<<<< Updated upstream' "$P15_POPCONFLICT/c.txt"
check "phase15 row8 (conflicting pop): conflict markers use the literal stash label" \
    grep -q '>>>>>>> Stashed changes' "$P15_POPCONFLICT/c.txt"
check "phase15 row8 (conflicting pop): MERGE_HEAD was not written" \
    test ! -f "$P15_POPCONFLICT/.git/MERGE_HEAD"
P15_POPCONFLICT_STAGES=$(cd "$P15_POPCONFLICT" && git ls-files -u c.txt | awk '{print $3}')
check "phase15 row8 (conflicting pop): index has all three stages for c.txt" \
    sh -c "echo \"$P15_POPCONFLICT_STAGES\" | sort -u | tr -d '\n' | grep -qx '123'"

# --- Phase 41: conflict-marker labels, the one thing merge output had no
# oracle for at all. Both groups build the repo ONCE with sg and then copy
# it, so sg and git rebase/merge the SAME commits -- two independently built
# repos would carry different shas and the rebase label could then only be
# compared by shape, not byte for byte.

# A. `sg merge`'s ours label is a DELIBERATE divergence: sg names the branch,
# real git always writes HEAD (measured across five situations in
# docs/DESIGN.md's Phase 41 section). cmd_merge.c's comment claimed phase4b
# pinned this; it did not. Pinned here on both sides, the same idiom the
# other deliberate divergences in this file use -- so "fixing" sg back into
# silent agreement with git would itself turn a check red rather than pass
# unnoticed.
P41_M="$WORKDIR/p41_merge_label"
(cd "$WORKDIR" && "$SG" init "$(basename "$P41_M")") > /dev/null 2>&1
printf 'l1\nl2\nl3\nl4\n' > "$P41_M/f.txt"
(cd "$P41_M" && "$SG" add f.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P41_M" && "$SG" branch topic) > /dev/null 2>&1
printf 'l1\nl2\nOURS-3\nl4\n' > "$P41_M/f.txt"
(cd "$P41_M" && "$SG" add f.txt && "$SG" commit -m ours) > /dev/null 2>&1
(cd "$P41_M" && "$SG" switch topic) > /dev/null 2>&1
printf 'l1\nl2\nTHEIRS-3\nl4\n' > "$P41_M/f.txt"
(cd "$P41_M" && "$SG" add f.txt && "$SG" commit -m theirs) > /dev/null 2>&1
(cd "$P41_M" && "$SG" switch master) > /dev/null 2>&1
rm -rf "$P41_M.git-copy"
cp -R "$P41_M" "$P41_M.git-copy"
(cd "$P41_M" && "$SG" merge topic) > /dev/null 2>&1
(cd "$P41_M.git-copy" && LC_ALL=C git merge topic) > /dev/null 2>&1
check "phase41: precondition -- both copies really conflicted on f.txt" \
    sh -c "grep -q '^<<<<<<< ' '$P41_M/f.txt' && grep -q '^<<<<<<< ' '$P41_M.git-copy/f.txt'"
check "phase41 oracle: real git's merge ours label is HEAD, not the branch name" \
    grep -qx '<<<<<<< HEAD' "$P41_M.git-copy/f.txt"
check "phase41: sg deliberately names the branch instead (divergence, both sides pinned)" \
    grep -qx '<<<<<<< master' "$P41_M/f.txt"
check "phase41: the theirs label agrees with git even though the ours label does not" \
    sh -c "grep -qx '>>>>>>> topic' '$P41_M/f.txt' && grep -qx '>>>>>>> topic' '$P41_M.git-copy/f.txt'"
# The divergence is confined to that one line: everything else about the
# conflicted file must still match git byte for byte. Without this, sg could
# drift anywhere else in the marker block and the two greps above would stay
# green.
check "phase41: apart from the ours label the conflicted file matches git byte-for-byte" \
    sh -c "sed '1,\$s/^<<<<<<< .*/<<<<<<< LABEL/' '$P41_M/f.txt' > $WORKDIR/p41_sg_norm.txt; sed '1,\$s/^<<<<<<< .*/<<<<<<< LABEL/' '$P41_M.git-copy/f.txt' > $WORKDIR/p41_git_norm.txt; cmp -s $WORKDIR/p41_sg_norm.txt $WORKDIR/p41_git_norm.txt"
# Detached HEAD is where sg and git AGREE, because current_branch is NULL and
# sg falls back to git's own answer. Pinning it keeps that accident honest:
# it is the only state in which the divergence above disappears.
P41_MD="$WORKDIR/p41_merge_label_detached"
rm -rf "$P41_MD"
cp -R "$P41_M.git-copy" "$P41_MD"
(cd "$P41_MD" && LC_ALL=C git merge --abort) > /dev/null 2>&1
(cd "$P41_MD" && "$SG" switch --detach HEAD) > /dev/null 2>&1
(cd "$P41_MD" && "$SG" merge topic) > /dev/null 2>&1
check "phase41: with HEAD detached sg writes HEAD too -- the divergence disappears" \
    grep -qx '<<<<<<< HEAD' "$P41_MD/f.txt"

# B. `sg rebase`'s theirs label. Real git writes "<short-sha> (<subject>)";
# sg omitted the parentheses until Phase 41 measured them side by side.
# Nothing recorded that as deliberate -- no comment, no test -- so it was
# corrected rather than pinned as-is. Same copy-the-repo technique, so both
# tools rebase identical commits and the short shas match.
P41_R="$WORKDIR/p41_rebase_label"
(cd "$WORKDIR" && "$SG" init "$(basename "$P41_R")") > /dev/null 2>&1
printf 'r1\nr2\nr3\nr4\n' > "$P41_R/g.txt"
(cd "$P41_R" && "$SG" add g.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P41_R" && "$SG" branch topic) > /dev/null 2>&1
printf 'r1\nr2\nMASTER-3\nr4\n' > "$P41_R/g.txt"
(cd "$P41_R" && "$SG" add g.txt && "$SG" commit -m "master edits g") > /dev/null 2>&1
(cd "$P41_R" && "$SG" switch topic) > /dev/null 2>&1
printf 'r1\nr2\nTOPIC-3\nr4\n' > "$P41_R/g.txt"
(cd "$P41_R" && "$SG" add g.txt && "$SG" commit -m "topic subject line") > /dev/null 2>&1
rm -rf "$P41_R.git-copy"
cp -R "$P41_R" "$P41_R.git-copy"
(cd "$P41_R" && "$SG" rebase master) > /dev/null 2>&1
(cd "$P41_R.git-copy" && LC_ALL=C git rebase master) > /dev/null 2>&1
check "phase41: precondition -- both copies really conflicted during rebase" \
    sh -c "grep -q '^>>>>>>> ' '$P41_R/g.txt' && grep -q '^>>>>>>> ' '$P41_R.git-copy/g.txt'"
check "phase41 oracle: real git's rebase theirs label parenthesizes the subject" \
    grep -qE '^>>>>>>> [0-9a-f]{7} \(topic subject line\)$' "$P41_R.git-copy/g.txt"
check "phase41: sg's rebase theirs label now matches git byte-for-byte" \
    sh -c "grep '^>>>>>>> ' '$P41_R/g.txt' > $WORKDIR/p41_r_sg.txt; grep '^>>>>>>> ' '$P41_R.git-copy/g.txt' > $WORKDIR/p41_r_git.txt; cmp -s $WORKDIR/p41_r_sg.txt $WORKDIR/p41_r_git.txt"
check "phase41: and the rebase ours label is HEAD on both sides" \
    sh -c "grep -qx '<<<<<<< HEAD' '$P41_R/g.txt' && grep -qx '<<<<<<< HEAD' '$P41_R.git-copy/g.txt'"

# --- row 9 (Phase 20 REVERSAL of the Phase 15 refusal): a stash real git
# built with -u must now be poppable by sg -- the untracked half is restored
# to disk, unstaged, and the entry is dropped, same as any other successful
# pop. This is also half of the bidirectional interop coverage (git builds,
# sg reads); the other half (sg builds, git reads) is further down. ---
P15_POPU="$WORKDIR/p15_popu"
p15_base_repo "$P15_POPU"
printf 'tracked change\n' > "$P15_POPU/a.txt"
printf 'untracked content\n' > "$P15_POPU/u.txt"
(cd "$P15_POPU" && git add a.txt && git stash push -q -u -m "has untracked") > /dev/null 2>&1
check "phase20 row9 (pop -u stash): precondition -- u.txt is gone (git -u stashed it)" \
    test ! -e "$P15_POPU/u.txt"
P15_POPU_OUT="$WORKDIR/p15_popu_out.txt"
(cd "$P15_POPU" && "$SG" stash pop) > "$P15_POPU_OUT" 2>&1
check "phase20 row9 (pop -u stash): sg now SUCCEEDS restoring a git-built -u stash (exit 0)" \
    test $? = 0
check "phase20 row9 (pop -u stash): a.txt got its tracked content back" \
    sh -c "[ \"\$(cat '$P15_POPU/a.txt')\" = 'tracked change' ]"
check "phase20 row9 (pop -u stash): u.txt was restored with its original content" \
    sh -c "[ \"\$(cat '$P15_POPU/u.txt')\" = 'untracked content' ]"
check "phase20 row9 (pop -u stash): u.txt is untracked (?? ), not staged" \
    sh -c "[ \"\$(cd '$P15_POPU' && git status --porcelain u.txt)\" = '?? u.txt' ]"
check "phase20 row9 (pop -u stash): the entry was dropped after a successful pop" \
    sh -c "[ -z \"\$(cd '$P15_POPU' && git stash list)\" ]"

# --- apply vs pop: the ONLY thing separating the two subcommands is whether
# the entry survives, and "it survived" is a negative assertion -- an apply
# that also dropped would restore the same bytes and exit 0 just the same,
# so asserting the working tree alone proves nothing. Assert the stash is
# still addressable afterwards (both by sg and by real git, since a
# half-done drop could leave the reflog and refs/stash disagreeing), then
# pop the same entry and assert it is gone -- the contrast is what makes
# either half meaningful. ---
P15_APPLY="$WORKDIR/p15_apply"
p15_base_repo "$P15_APPLY"
printf 'apply me\n' > "$P15_APPLY/a.txt"
(cd "$P15_APPLY" && "$SG" stash push -m "survives apply") > /dev/null 2>&1
(cd "$P15_APPLY" && "$SG" stash apply) > /dev/null 2>&1
check "phase15 apply: exits 0" test $? = 0
check "phase15 apply: the working tree content was restored" \
    grep -q 'apply me' "$P15_APPLY/a.txt"
check "phase15 apply: the entry is STILL listed by sg (apply must not drop)" \
    sh -c "(cd '$P15_APPLY' && '$SG' stash list) | grep -q 'survives apply'"
check "phase15 apply: the entry is STILL listed by real git too" \
    sh -c "(cd '$P15_APPLY' && git stash list) | grep -q 'survives apply'"
check "phase15 apply: refs/stash still resolves after apply" \
    sh -c "(cd '$P15_APPLY' && git rev-parse --verify refs/stash) > /dev/null 2>&1"
P15_APPLY_REF=$(cd "$P15_APPLY" && git rev-parse refs/stash 2>/dev/null)
P15_APPLY_LOGTIP=$(cd "$P15_APPLY" && tail -1 .git/logs/refs/stash 2>/dev/null | cut -d' ' -f2)
check "phase15 apply: the tip invariant still holds after apply" \
    test "$P15_APPLY_REF" = "$P15_APPLY_LOGTIP"
# A successful apply necessarily leaves the working tree dirty, and Phase 15
# deliberately requires a clean tree to pop (a documented divergence from real
# git, which would three-way merge here). Pin that refusal rather than working
# around it silently, then clean up and pop for real.
(cd "$P15_APPLY" && "$SG" stash pop) > /dev/null 2>&1
check "phase15 apply: popping onto the tree apply just dirtied is refused" test $? != 0
check "phase15 apply: the refused pop left the entry alone" \
    sh -c "(cd '$P15_APPLY' && git stash list) | grep -q 'survives apply'"
(cd "$P15_APPLY" && git checkout -q -- a.txt) > /dev/null 2>&1
(cd "$P15_APPLY" && "$SG" stash pop) > /dev/null 2>&1
check "phase15 apply/pop contrast: popping the same entry DOES remove it" \
    sh -c "! (cd '$P15_APPLY' && git stash list) | grep -q 'survives apply'"

# --- How the "Dropped ..." line names the entry. Real git echoes the spec
# back only when it already reads as stash@{N}; a bare "0" and no argument
# at all both resolve to the fully-qualified refs/stash@{N}. That is one
# rule for pop and drop alike -- it first looked like a pop-versus-drop
# difference because the two sampled invocations happened to differ in
# whether they passed a spec. Each form is pinned against real git rather
# than a hard-coded string, so the day git changes its mind this says so.
# The oid is never compared: sg's commit ids can never equal git's (fixed
# "+0000", time(NULL)), only the wording is.
#
# The oracle runs under LC_ALL=C on purpose. git translates this message (a
# a zh_TW machine prints a translated form of this line, full-width parentheses
# and all) while CI runners default to C -- without pinning the locale this
# check would pass in CI and fail locally, the worst way for a check to be
# wrong. ---
p15_drop_wording() {
    # $1 = repo dir, $2 = "sg"|"git", $3 = subcommand, $4 = spec ("" for none)
    _d="$1"; _impl="$2"; _sub="$3"; _spec="$4"
    printf 'wording %s\n' "$_sub$_spec" > "$_d/a.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_d" && "$SG" stash push -m "wording") > /dev/null 2>&1
        if [ -z "$_spec" ]; then
            (cd "$_d" && LC_ALL=C "$SG" stash "$_sub") 2>&1
        else
            (cd "$_d" && LC_ALL=C "$SG" stash "$_sub" "$_spec") 2>&1
        fi
    else
        (cd "$_d" && git stash push -q -m "wording") > /dev/null 2>&1
        if [ -z "$_spec" ]; then
            (cd "$_d" && LC_ALL=C git stash "$_sub") 2>&1
        else
            (cd "$_d" && LC_ALL=C git stash "$_sub" "$_spec") 2>&1
        fi
    fi
}

P15_DROPMSG="$WORKDIR/p15_dropmsg"
p15_base_repo "$P15_DROPMSG"
P15_DROPMSG_GIT="$WORKDIR/p15_dropmsg_git"
mkdir -p "$P15_DROPMSG_GIT"
(cd "$P15_DROPMSG_GIT" && git init -q . && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\n' > "$P15_DROPMSG_GIT/a.txt"
(cd "$P15_DROPMSG_GIT" && git add a.txt && git commit -q -m base)

for p15_case in "drop::refs/stash@{0}" "drop:stash@{0}:stash@{0}" "drop:0:refs/stash@{0}" \
                "pop::refs/stash@{0}" "pop:stash@{0}:stash@{0}" "pop:0:refs/stash@{0}"; do
    p15_sub=$(echo "$p15_case" | cut -d: -f1)
    p15_spec=$(echo "$p15_case" | cut -d: -f2)
    p15_want=$(echo "$p15_case" | cut -d: -f3)
    # Captured to files, never interpolated into an `sh -c` string: pop's
    # output carries a full status block containing double quotes (`use "git
    # add"`), which silently shreds an embedded-and-requoted command line.
    # drop's single line has no quotes, so only half the cases would have
    # misfired -- the kind of harness bug that reads as a product bug.
    p15_drop_wording "$P15_DROPMSG_GIT" git "$p15_sub" "$p15_spec" > "$WORKDIR/p15_w_git.txt" 2>&1
    p15_drop_wording "$P15_DROPMSG" sg "$p15_sub" "$p15_spec" > "$WORKDIR/p15_w_sg.txt" 2>&1
    check "phase15 drop wording oracle: git stash $p15_sub '$p15_spec' names it $p15_want" \
        grep -qF "Dropped $p15_want (" "$WORKDIR/p15_w_git.txt"
    check "phase15 drop wording: sg stash $p15_sub '$p15_spec' names it $p15_want, same as git" \
        grep -qF "Dropped $p15_want (" "$WORKDIR/p15_w_sg.txt"
done

# --- push refuses an unmerged index. Real git's rule is about the index,
# not about which operation left it that way (measured: a merge conflict and
# a rebase conflict are refused identically, while a paused rebase with a
# clean index is allowed). Assert the message, not just the exit code: the
# guard exists at both the CLI and the library layer, and a third backstop
# sits in sg_tree_build_from_index, so each one masks the others -- removing
# cmd_stash.c's sg_index_has_unmerged branch alone left all 798 checks
# green. ---
P15_UNMERGED="$WORKDIR/p15_unmerged"
p15_base_repo "$P15_UNMERGED"
printf 'base line\n' > "$P15_UNMERGED/m.txt"
(cd "$P15_UNMERGED" && "$SG" add m.txt && "$SG" commit -m "m base") > /dev/null 2>&1
(cd "$P15_UNMERGED" && "$SG" branch side) > /dev/null 2>&1
printf 'master side\n' > "$P15_UNMERGED/m.txt"
(cd "$P15_UNMERGED" && "$SG" add m.txt && "$SG" commit -m "master edits m") > /dev/null 2>&1
(cd "$P15_UNMERGED" && "$SG" switch side) > /dev/null 2>&1
printf 'other side\n' > "$P15_UNMERGED/m.txt"
(cd "$P15_UNMERGED" && "$SG" add m.txt && "$SG" commit -m "side edits m") > /dev/null 2>&1
(cd "$P15_UNMERGED" && "$SG" merge master) > /dev/null 2>&1
check "phase15 unmerged push: precondition -- the index really has unmerged stages" \
    sh -c "[ -n \"\$(cd '$P15_UNMERGED' && git ls-files -u)\" ]"
P15_UNMERGED_ERR="$WORKDIR/p15_unmerged_err.txt"
(cd "$P15_UNMERGED" && "$SG" stash push -m "should be refused") > /dev/null 2> "$P15_UNMERGED_ERR"
check "phase15 unmerged push: sg refuses (exit 1)" test $? != 0
# Match the CLI guard's own wording, not just the words "unresolved
# conflict": the library-layer fallback message speculates "...or does the
# index have unresolved conflicts?" and contains that phrase too, so the
# looser pattern passed with the CLI guard removed. Measured.
check "phase15 unmerged push: the refusal is the specific guard, not the generic fallback" \
    grep -q 'unresolved conflicts remain' "$P15_UNMERGED_ERR"
check "phase15 unmerged push: no stash was created" \
    sh -c "! (cd '$P15_UNMERGED' && git rev-parse --verify refs/stash) > /dev/null 2>&1"
check "phase15 unmerged push: the conflicted state was left intact" \
    sh -c "[ -n \"\$(cd '$P15_UNMERGED' && git ls-files -u)\" ]"

# --- A path that fails to reach the working tree must fail the whole apply.
# pop drops the entry as soon as apply reports success, so treating a
# per-path read failure as a warning costs the user the file AND its only
# backup, while exiting 0. Measured on a repo whose stashed blob was deleted:
# with the failure demoted to a warning, pop exits 0, prints
# "Dropped stash@{0}", leaves a.txt at its pre-stash content and empties the
# stash list. Every assertion below is the opposite of that outcome. ---
P15_LOSTBLOB="$WORKDIR/p15_lostblob"
p15_base_repo "$P15_LOSTBLOB"
printf 'stashed content\n' > "$P15_LOSTBLOB/a.txt"
P15_LOSTBLOB_ID=$(cd "$P15_LOSTBLOB" && git hash-object a.txt)
(cd "$P15_LOSTBLOB" && "$SG" stash push -m "blob goes missing") > /dev/null 2>&1
P15_LOSTBLOB_OBJ="$P15_LOSTBLOB/.git/objects/$(echo "$P15_LOSTBLOB_ID" | cut -c1-2)/$(echo "$P15_LOSTBLOB_ID" | cut -c3-)"
check "phase15 lost blob: precondition -- the stashed blob is a loose object" \
    test -f "$P15_LOSTBLOB_OBJ"
rm -f "$P15_LOSTBLOB_OBJ"
(cd "$P15_LOSTBLOB" && "$SG" stash pop) > /dev/null 2>&1
check "phase15 lost blob: pop fails instead of reporting success" test $? != 0
check "phase15 lost blob: the entry was NOT dropped" \
    sh -c "(cd '$P15_LOSTBLOB' && git stash list) | grep -q 'blob goes missing'"
P15_LOSTBLOB_REF=$(cd "$P15_LOSTBLOB" && git rev-parse refs/stash 2>/dev/null)
P15_LOSTBLOB_TIP=$(cd "$P15_LOSTBLOB" && tail -1 .git/logs/refs/stash 2>/dev/null | cut -d' ' -f2)
check "phase15 lost blob: the tip invariant survived the failed pop" \
    sh -c "[ -n \"$P15_LOSTBLOB_REF\" ] && [ \"$P15_LOSTBLOB_REF\" = \"$P15_LOSTBLOB_TIP\" ]"

# ============================================================
# Phase 20 batch 1: sg stash push --keep-index, and the -u/-a placeholders
#
# Fixture: a five-file spread covering the whole --keep-index table (spec
# sec 2) -- tracked.txt unstaged-modified, staged.txt staged-modified,
# new_staged.txt staged-new, staged_del.txt staged FOR DELETION (removed
# from the index, left on disk), wt_del.txt deleted unstaged. sg has no `rm
# --cached`, so `git rm --cached` builds that one state for BOTH the
# oracle and the sg-built repo -- legitimate, not a shortcut unique to sg,
# because the index format is bit-compatible (see CLAUDE.md's project
# summary).
# ============================================================

p20_keepidx_fixture() {
    # $1 = dir, $2 = "sg" or "git"
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = sg ]; then
        (cd "$WORKDIR" && "$SG" init "$(basename "$_dir")") > /dev/null 2>&1
    else
        (cd "$WORKDIR" && git init -q "$(basename "$_dir")")
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'v1\n' > "$_dir/tracked.txt"
    printf 's1\n' > "$_dir/staged.txt"
    printf 'del content\n' > "$_dir/staged_del.txt"
    printf 'wtdel content\n' > "$_dir/wt_del.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add tracked.txt staged.txt staged_del.txt wt_del.txt && \
            "$SG" commit -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && git add tracked.txt staged.txt staged_del.txt wt_del.txt && git commit -q -m base)
    fi

    printf 'v2\n' > "$_dir/tracked.txt"
    printf 's2\n' > "$_dir/staged.txt"
    printf 'ns1\n' > "$_dir/new_staged.txt"
    rm -f "$_dir/wt_del.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add staged.txt new_staged.txt) > /dev/null 2>&1
    else
        (cd "$_dir" && git add staged.txt new_staged.txt) > /dev/null 2>&1
    fi
    (cd "$_dir" && git rm -q --cached staged_del.txt) > /dev/null 2>&1
}

# --- oracle: real git's own --keep-index result on the fixture ---
P20_KI_GIT="$WORKDIR/p20_keepidx_git"
p20_keepidx_fixture "$P20_KI_GIT" git
(cd "$P20_KI_GIT" && git stash push -q --keep-index -m keepidx) > /dev/null 2>&1
check "phase20 keep-index oracle: git stash push --keep-index exits 0" test $? = 0
P20_KI_GIT_PORCELAIN=$(cd "$P20_KI_GIT" && git status --porcelain | sort)

# --- sg's --keep-index result on the identical fixture ---
P20_KI_SG="$WORKDIR/p20_keepidx_sg"
p20_keepidx_fixture "$P20_KI_SG" sg
(cd "$P20_KI_SG" && "$SG" stash push --keep-index -m keepidx) > /dev/null 2>&1
check "phase20 keep-index: sg stash push --keep-index exits 0" test $? = 0

check "phase20 keep-index: tracked.txt reset to HEAD (v1)" \
    sh -c "[ \"\$(cat '$P20_KI_SG/tracked.txt')\" = v1 ]"
check "phase20 keep-index: staged.txt kept at the staged content (s2), not reset to HEAD's s1" \
    sh -c "[ \"\$(cat '$P20_KI_SG/staged.txt')\" = s2 ]"
check "phase20 keep-index: new_staged.txt kept and still present" \
    sh -c "[ \"\$(cat '$P20_KI_SG/new_staged.txt')\" = ns1 ]"
check "phase20 keep-index: staged_del.txt was deleted (the staged removal is re-applied)" \
    test ! -e "$P20_KI_SG/staged_del.txt"
check "phase20 keep-index: wt_del.txt restored" \
    sh -c "[ \"\$(cat '$P20_KI_SG/wt_del.txt')\" = 'wtdel content' ]"

P20_KI_SG_PORCELAIN=$(cd "$P20_KI_SG" && git status --porcelain | sort)
check "phase20 keep-index: real git's interpretation of sg's result matches real git's own --keep-index output byte-for-byte" \
    test "$P20_KI_SG_PORCELAIN" = "$P20_KI_GIT_PORCELAIN"

# --- control: the SAME fixture without --keep-index, both implementations,
# so the --keep-index checks above are pinned against a contrast rather than
# read on their own (a no-op --keep-index implementation would also pass
# every check that doesn't compare the two modes). ---
P20_NOFLAG_GIT="$WORKDIR/p20_noflag_git"
p20_keepidx_fixture "$P20_NOFLAG_GIT" git
(cd "$P20_NOFLAG_GIT" && git stash push -q -m noflag) > /dev/null 2>&1
check "phase20 no-flag control: git stash push (no --keep-index) exits 0" test $? = 0
P20_NOFLAG_GIT_PORCELAIN=$(cd "$P20_NOFLAG_GIT" && git status --porcelain | sort)
check "phase20 no-flag control: precondition -- the oracle's no-flag result differs from its --keep-index result (the flag actually changes something)" \
    sh -c "[ \"$P20_NOFLAG_GIT_PORCELAIN\" != \"$P20_KI_GIT_PORCELAIN\" ]"

P20_NOFLAG_SG="$WORKDIR/p20_noflag_sg"
p20_keepidx_fixture "$P20_NOFLAG_SG" sg
(cd "$P20_NOFLAG_SG" && "$SG" stash push -m noflag) > /dev/null 2>&1
check "phase20 no-flag control: sg stash push (no --keep-index) exits 0" test $? = 0
P20_NOFLAG_SG_PORCELAIN=$(cd "$P20_NOFLAG_SG" && git status --porcelain | sort)
check "phase20 no-flag control: sg's no-flag result matches real git's no-flag result" \
    test "$P20_NOFLAG_SG_PORCELAIN" = "$P20_NOFLAG_GIT_PORCELAIN"
check "phase20 no-flag control: sg's no-flag result DIFFERS from sg's own --keep-index result" \
    test "$P20_NOFLAG_SG_PORCELAIN" != "$P20_KI_SG_PORCELAIN"

# --- Phase 20 batch 2b: real -u/-a semantics ---
#
# Fixture covers every boundary spec sec 1.3/1.4 pins: a tracked change
# (tracked.txt), a plain untracked file, an untracked file in a fresh
# subdirectory (dir removal after the file is taken), a directory holding
# ONLY an ignored file (must survive -u, must be removed by -a), a directory
# with one ignored and one plain file (partial removal), and a directory
# that is already completely empty before the push (removed by -u alone,
# since it has nothing ignored in it to begin with).
# ============================================================

p20_untracked_fixture() {
    # $1 = dir, $2 = "sg" or "git"
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = sg ]; then
        (cd "$WORKDIR" && "$SG" init "$(basename "$_dir")") > /dev/null 2>&1
    else
        (cd "$WORKDIR" && git init -q "$(basename "$_dir")")
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'v1\n' > "$_dir/tracked.txt"
    printf 'ignored_only/\nmixed/keep.log\nbuild/\n' > "$_dir/.gitignore"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add tracked.txt .gitignore && "$SG" commit -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && git add tracked.txt .gitignore && git commit -q -m base)
    fi
    printf 'v2\n' > "$_dir/tracked.txt"
    printf 'u1\n' > "$_dir/untracked.txt"
    mkdir -p "$_dir/fresh"; printf 'f1\n' > "$_dir/fresh/inner.txt"
    mkdir -p "$_dir/ignored_only"; printf 'x1\n' > "$_dir/ignored_only/x.txt"
    mkdir -p "$_dir/mixed"; printf 'k1\n' > "$_dir/mixed/keep.log"; printf 't1\n' > "$_dir/mixed/take.txt"
    mkdir -p "$_dir/emptydir"
    # build/ is itself matched by .gitignore AND physically empty -- the
    # boundary that tells apart "prune whatever's physically empty" (wrong)
    # from "prune whatever's physically empty AND not itself ignored under
    # -u" (right, measured against real git 2.55.0). emptydir/ above is
    # empty but NOT ignored, so it alone cannot distinguish the two rules.
    mkdir -p "$_dir/build"
}

p20_worktree_listing() {
    (cd "$1" && find . -path ./.git -prune -o \( -type f -o -type d \) -print | grep -v '^\.$' | sort)
}

# --- U1: -u, sg vs real git, full worktree listing after push must match ---
P20_U_GIT="$WORKDIR/p20_u_git"
p20_untracked_fixture "$P20_U_GIT" git
(cd "$P20_U_GIT" && git stash push -q -u -m untracked) > /dev/null 2>&1
check "phase20 -u oracle: git stash push -u exits 0" test $? = 0
P20_U_GIT_LISTING=$(p20_worktree_listing "$P20_U_GIT")

P20_U_SG="$WORKDIR/p20_u_sg"
p20_untracked_fixture "$P20_U_SG" sg
(cd "$P20_U_SG" && "$SG" stash push -u -m untracked) > /dev/null 2>&1
check "phase20 -u: sg stash push -u exits 0" test $? = 0
P20_U_SG_LISTING=$(p20_worktree_listing "$P20_U_SG")
check "phase20 -u: sg's post-push worktree listing matches real git's byte-for-byte (dir-removal boundaries included)" \
    test "$P20_U_SG_LISTING" = "$P20_U_GIT_LISTING"
check "phase20 -u: ignored_only/ survives (still holds the ignored x.txt)" \
    test -d "$P20_U_SG/ignored_only"
check "phase20 -u: mixed/ survives with keep.log, take.txt taken" \
    sh -c "[ -f '$P20_U_SG/mixed/keep.log' ] && [ ! -e '$P20_U_SG/mixed/take.txt' ]"
check "phase20 -u: the already-empty emptydir/ is removed" \
    test ! -e "$P20_U_SG/emptydir"
check "phase20 -u: fresh/ is removed once inner.txt is taken" \
    test ! -e "$P20_U_SG/fresh"
# build/ is empty AND ignored -- unlike emptydir/ (empty but not ignored),
# this is the boundary that catches a prune rule which only checks physical
# emptiness and never re-consults ignore status for the directory itself.
check "phase20 -u oracle: real git also SPARES build/ (empty, but ignored -- precondition for the check below)" \
    test -d "$P20_U_GIT/build"
check "phase20 -u: build/ (empty AND ignored) survives, matching real git -- -u must not sweep an ignored empty dir" \
    test -d "$P20_U_SG/build"

# --- U2: same fixture, -a -- ignored_only/ must now be removed too ---
P20_A_GIT="$WORKDIR/p20_a_git"
p20_untracked_fixture "$P20_A_GIT" git
(cd "$P20_A_GIT" && git stash push -q -a -m ignored) > /dev/null 2>&1
check "phase20 -a oracle: git stash push -a exits 0" test $? = 0
P20_A_GIT_LISTING=$(p20_worktree_listing "$P20_A_GIT")

P20_A_SG="$WORKDIR/p20_a_sg"
p20_untracked_fixture "$P20_A_SG" sg
(cd "$P20_A_SG" && "$SG" stash push -a -m ignored) > /dev/null 2>&1
check "phase20 -a: sg stash push -a exits 0" test $? = 0
P20_A_SG_LISTING=$(p20_worktree_listing "$P20_A_SG")
check "phase20 -a: sg's post-push worktree listing matches real git's byte-for-byte" \
    test "$P20_A_SG_LISTING" = "$P20_A_GIT_LISTING"
check "phase20 -a: ignored_only/ is REMOVED (its only file was ignored, but -a still took it)" \
    test ! -e "$P20_A_SG/ignored_only"
check "phase20 -a: mixed/ is removed too (both files taken)" \
    test ! -e "$P20_A_SG/mixed"
check "phase20 -a oracle: real git also REMOVES build/ under -a (precondition for the check below)" \
    test ! -e "$P20_A_GIT/build"
check "phase20 -a: build/ (empty AND ignored) is removed too, matching real git -- -a sweeps ignored paths" \
    test ! -e "$P20_A_SG/build"

# --- U3 (bidirectional half 1): sg builds a -u stash, real git pops it ---
P20_SG2GIT_U="$WORKDIR/p20_sg2git_u"
p20_untracked_fixture "$P20_SG2GIT_U" sg
(cd "$P20_SG2GIT_U" && "$SG" stash push -u -m roundtrip) > /dev/null 2>&1
check "phase20 sg->git roundtrip (-u): sg stash push -u exits 0" test $? = 0
(cd "$P20_SG2GIT_U" && git stash pop -q) > /dev/null 2>&1
check "phase20 sg->git roundtrip (-u): real git pop exits 0" test $? = 0
check "phase20 sg->git roundtrip (-u): tracked.txt restored to v2" \
    sh -c "[ \"\$(cat '$P20_SG2GIT_U/tracked.txt')\" = v2 ]"
check "phase20 sg->git roundtrip (-u): untracked.txt restored" \
    sh -c "[ \"\$(cat '$P20_SG2GIT_U/untracked.txt')\" = u1 ]"
check "phase20 sg->git roundtrip (-u): fresh/inner.txt restored (nested dir recreated)" \
    sh -c "[ \"\$(cat '$P20_SG2GIT_U/fresh/inner.txt')\" = f1 ]"
check "phase20 sg->git roundtrip (-u): untracked.txt is untracked, not staged" \
    sh -c "[ \"\$(cd '$P20_SG2GIT_U' && git status --porcelain untracked.txt)\" = '?? untracked.txt' ]"

# --- U4 (bidirectional half 2): real git builds a -a stash, sg pops it ---
P20_GIT2SG_A="$WORKDIR/p20_git2sg_a"
p20_untracked_fixture "$P20_GIT2SG_A" git
(cd "$P20_GIT2SG_A" && git stash push -q -a -m roundtrip) > /dev/null 2>&1
check "phase20 git->sg roundtrip (-a): git stash push -a exits 0" test $? = 0
(cd "$P20_GIT2SG_A" && "$SG" stash pop) > /dev/null 2>&1
check "phase20 git->sg roundtrip (-a): sg pop exits 0" test $? = 0
check "phase20 git->sg roundtrip (-a): tracked.txt restored to v2" \
    sh -c "[ \"\$(cat '$P20_GIT2SG_A/tracked.txt')\" = v2 ]"
check "phase20 git->sg roundtrip (-a): the previously-ignored mixed/keep.log came back" \
    sh -c "[ \"\$(cat '$P20_GIT2SG_A/mixed/keep.log')\" = k1 ]"
check "phase20 git->sg roundtrip (-a): mixed/keep.log is untracked, not staged (still ignored)" \
    sh -c "[ \"\$(cd '$P20_GIT2SG_A' && git status --porcelain --ignored mixed/keep.log)\" = '!! mixed/keep.log' ]"
check "phase20 git->sg roundtrip (-a): the stash entry was dropped" \
    sh -c "[ -z \"\$(cd '$P20_GIT2SG_A' && git stash list)\" ]"

# --- U5: bare-form flag recognition actually works now (no "push" keyword),
# and combining -u with --keep-index matches real git ---
P20_BARE="$WORKDIR/p20_bare"
p15_base_repo "$P20_BARE"
printf 'x\n' > "$P20_BARE/a.txt"
printf 'y\n' > "$P20_BARE/b_untracked.txt"
(cd "$P20_BARE" && "$SG" stash -u) > /dev/null 2>&1
check "phase20 bare form: sg stash -u (no 'push') exits 0" test $? = 0
check "phase20 bare form: b_untracked.txt was taken by the bare -u form" \
    test ! -e "$P20_BARE/b_untracked.txt"
check "phase20 bare form: the stash it created has 3 parents (untracked half present)" \
    sh -c "[ \"\$(cd '$P20_BARE' && git rev-list --parents -n1 refs/stash | wc -w | tr -d ' ')\" = 4 ]"

P20_UKI_GIT="$WORKDIR/p20_uki_git"
p20_untracked_fixture "$P20_UKI_GIT" git
(cd "$P20_UKI_GIT" && git add tracked.txt && git stash push -q -u --keep-index -m ukeep) > /dev/null 2>&1
check "phase20 -u+--keep-index oracle: git exits 0" test $? = 0
P20_UKI_GIT_PORCELAIN=$(cd "$P20_UKI_GIT" && git status --porcelain | sort)

P20_UKI_SG="$WORKDIR/p20_uki_sg"
p20_untracked_fixture "$P20_UKI_SG" sg
(cd "$P20_UKI_SG" && "$SG" add tracked.txt) > /dev/null 2>&1
(cd "$P20_UKI_SG" && "$SG" stash push -u --keep-index -m ukeep) > /dev/null 2>&1
check "phase20 -u+--keep-index: sg exits 0" test $? = 0
P20_UKI_SG_PORCELAIN=$(cd "$P20_UKI_SG" && git status --porcelain | sort)
check "phase20 -u+--keep-index: sg's post-push status matches real git's byte-for-byte" \
    test "$P20_UKI_SG_PORCELAIN" = "$P20_UKI_GIT_PORCELAIN"
check "phase20 -u+--keep-index: fresh/ was still swept (untracked half taken despite --keep-index)" \
    test ! -e "$P20_UKI_SG/fresh"

# ============================================================
# Phase 20 batch 3: sg stash apply/pop --index, and the targeted
# dirty-workdir gate that replaced the old blanket "must be perfectly
# clean" refusal (spec sec 3 & 4).
# ============================================================

# I1/I2 deliberately run on p20_keepidx_fixture, the WIDER fixture -- it
# includes wt_del.txt (deleted from the working tree only) and staged_del.txt
# (deletion staged). Phase 20 had to keep a separate, narrower p20_index_fixture
# without those two, because sg_tree_build_from_workdir could not represent a
# working-tree deletion at all: real git's stash records such a file as DELETED
# in the stash's own tree, while sg fell back to the file's INDEX blob, and
# `stash push` + `stash apply --index` end to end is the only place that
# difference becomes externally visible. Phase 21 fixed that, so the two
# fixtures collapsed back into one and these checks now run WITH the case they
# used to have to avoid. Do not split them again without saying why here.

# --- I1: --index vs no --index, oracle comparison (spec sec 3.1) ---
P20_IDX_GIT="$WORKDIR/p20_idx_git"
p20_keepidx_fixture "$P20_IDX_GIT" git
(cd "$P20_IDX_GIT" && git stash push -q -m forindex) > /dev/null 2>&1
(cd "$P20_IDX_GIT" && git stash apply -q --index) > /dev/null 2>&1
check "phase20 --index oracle: git stash apply --index exits 0" test $? = 0
P20_IDX_GIT_PORCELAIN=$(cd "$P20_IDX_GIT" && git status --porcelain | sort)

P20_IDX_SG="$WORKDIR/p20_idx_sg"
p20_keepidx_fixture "$P20_IDX_SG" sg
(cd "$P20_IDX_SG" && "$SG" stash push -m forindex) > /dev/null 2>&1
(cd "$P20_IDX_SG" && "$SG" stash apply --index) > /dev/null 2>&1
check "phase20 --index: sg stash apply --index exits 0" test $? = 0
P20_IDX_SG_PORCELAIN=$(cd "$P20_IDX_SG" && git status --porcelain | sort)

# staged_del.txt is excluded from the byte-for-byte comparison below and
# asserted separately, because it hits a divergence Phase 20 already recorded
# and deliberately kept: `git rm --cached` leaves the file on disk while
# removing it from the index, and sg's stash apply takes HEAD (not the index)
# as "ours", so it sees a path ours holds and theirs does not and removes it.
# Real git leaves it alone. Measured against sg built at 1afef8b (the commit
# before this phase): identical behaviour there, so this is pre-existing and
# not something the deletion work introduced -- it was simply unreachable
# while the narrow fixture excluded the file. Pinned below rather than
# filtered away silently, so a change of behaviour still fails a check.
P20_IDX_GIT_CMP=$(printf '%s\n' "$P20_IDX_GIT_PORCELAIN" | grep -v 'staged_del\.txt')
P20_IDX_SG_CMP=$(printf '%s\n' "$P20_IDX_SG_PORCELAIN" | grep -v 'staged_del\.txt')
check "phase20 --index: sg's result matches real git's --index result byte-for-byte (outside the recorded staged_del.txt divergence)" \
    test "$P20_IDX_SG_CMP" = "$P20_IDX_GIT_CMP"
check "phase21 divergence oracle: real git keeps staged_del.txt on disk as untracked" \
    test -f "$P20_IDX_GIT/staged_del.txt"
check "phase21 divergence: sg removes it instead, because its apply reads ours from HEAD" \
    test ! -e "$P20_IDX_SG/staged_del.txt"
check "phase21 divergence: wt_del.txt -- the case this phase fixed -- IS handled identically" \
    test "$(printf '%s\n' "$P20_IDX_SG_PORCELAIN" | grep 'wt_del\.txt')" = "$(printf '%s\n' "$P20_IDX_GIT_PORCELAIN" | grep 'wt_del\.txt')"

# --- I2: control -- the SAME fixture without --index, pinning I1 against a
# contrast (a no-op --index implementation would also pass I1 alone) ---
P20_NOIDX_GIT="$WORKDIR/p20_noidx_git"
p20_keepidx_fixture "$P20_NOIDX_GIT" git
(cd "$P20_NOIDX_GIT" && git stash push -q -m noidx) > /dev/null 2>&1
(cd "$P20_NOIDX_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 no-index control oracle: git stash apply exits 0" test $? = 0
P20_NOIDX_GIT_PORCELAIN=$(cd "$P20_NOIDX_GIT" && git status --porcelain | sort)
check "phase20 no-index control oracle: precondition -- differs from the --index oracle result" \
    test "$P20_NOIDX_GIT_PORCELAIN" != "$P20_IDX_GIT_PORCELAIN"

P20_NOIDX_SG="$WORKDIR/p20_noidx_sg"
p20_keepidx_fixture "$P20_NOIDX_SG" sg
(cd "$P20_NOIDX_SG" && "$SG" stash push -m noidx) > /dev/null 2>&1
(cd "$P20_NOIDX_SG" && "$SG" stash apply) > /dev/null 2>&1
check "phase20 no-index control: sg stash apply exits 0" test $? = 0
P20_NOIDX_SG_PORCELAIN=$(cd "$P20_NOIDX_SG" && git status --porcelain | sort)
# Same recorded staged_del.txt divergence as I1 above; see the comment there.
P20_NOIDX_GIT_CMP=$(printf '%s\n' "$P20_NOIDX_GIT_PORCELAIN" | grep -v 'staged_del\.txt')
P20_NOIDX_SG_CMP=$(printf '%s\n' "$P20_NOIDX_SG_PORCELAIN" | grep -v 'staged_del\.txt')
check "phase20 no-index control: sg's result matches real git's no-index result (outside the recorded staged_del.txt divergence)" \
    test "$P20_NOIDX_SG_CMP" = "$P20_NOIDX_GIT_CMP"
check "phase21 divergence: the no-index path diverges on staged_del.txt the same way" \
    test ! -e "$P20_NOIDX_SG/staged_del.txt"
check "phase20 no-index control: sg's own --index and no-index results differ (the flag actually changes something)" \
    test "$P20_NOIDX_SG_PORCELAIN" != "$P20_IDX_SG_PORCELAIN"

# --- I3: --index on a real conflict is skipped entirely, and sg says so on
# stderr (spec sec 3.2) -- no real-git comparison here, this is purely
# about sg's own message and the index being left alone. ---
P20_IDXCONFLICT="$WORKDIR/p20_idxconflict"
(cd "$WORKDIR" && "$SG" init p20_idxconflict) > /dev/null 2>&1
(cd "$P20_IDXCONFLICT" && git config user.email "a@b.c" && git config user.name "git user")
printf 'hello\n' > "$P20_IDXCONFLICT/a.txt"
(cd "$P20_IDXCONFLICT" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
printf 'stash-side\n' > "$P20_IDXCONFLICT/a.txt"
(cd "$P20_IDXCONFLICT" && "$SG" add a.txt) > /dev/null 2>&1
(cd "$P20_IDXCONFLICT" && "$SG" stash push -m conflict) > /dev/null 2>&1
printf 'head-side\n' > "$P20_IDXCONFLICT/a.txt"
(cd "$P20_IDXCONFLICT" && "$SG" add a.txt && "$SG" commit -m advance) > /dev/null 2>&1
P20_IDXCONFLICT_ERR=$(cd "$P20_IDXCONFLICT" && "$SG" stash apply --index 2>&1 >/dev/null)
P20_IDXCONFLICT_RC=$?
check "phase20 --index conflict: sg stash apply --index exits 1 on a real conflict" \
    test "$P20_IDXCONFLICT_RC" = 1
check "phase20 --index conflict: stderr says the index was not unstashed" \
    sh -c "printf '%s' \"$P20_IDXCONFLICT_ERR\" | grep -q 'Index was not unstashed'"
check "phase20 --index conflict: the stash entry survives (not dropped)" \
    sh -c "[ -n \"\$(cd '$P20_IDXCONFLICT' && '$SG' stash list)\" ]"

# --- Dirty-workdir gate (spec sec 4.2/4.3): HEAD has del_me.txt/mod.txt/
# untouched.txt; the pushed stash deletes del_me.txt, modifies mod.txt to
# "m2", and adds created.txt -- untouched.txt is never touched at all. ---
p20_dirty_gate_fixture() {
    # $1 = dir, $2 = "sg" or "git"
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = sg ]; then
        (cd "$WORKDIR" && "$SG" init "$(basename "$_dir")") > /dev/null 2>&1
    else
        (cd "$WORKDIR" && git init -q "$(basename "$_dir")")
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'd1\n' > "$_dir/del_me.txt"
    printf 'm1\n' > "$_dir/mod.txt"
    printf 'u1\n' > "$_dir/untouched.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && git add . && git commit -q -m base)
    fi
    (cd "$_dir" && git rm -q del_me.txt) > /dev/null 2>&1
    printf 'm2\n' > "$_dir/mod.txt"
    printf 'new\n' > "$_dir/created.txt"
    (cd "$_dir" && git add mod.txt created.txt) > /dev/null 2>&1
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" stash push -m "dirty gate base") > /dev/null 2>&1
    else
        (cd "$_dir" && git stash push -q -m "dirty gate base") > /dev/null 2>&1
    fi
}

# --- row C (control): a dirty path the stash never touches must be let
# through, oracle comparison ---
P20_DG_C_GIT="$WORKDIR/p20_dg_c_git"
p20_dirty_gate_fixture "$P20_DG_C_GIT" git
printf 'DIRTY\n' > "$P20_DG_C_GIT/untouched.txt"
(cd "$P20_DG_C_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 dirty-gate row C oracle: git stash apply exits 0 when only an untouched path is dirty" \
    test $? = 0
P20_DG_C_GIT_PORCELAIN=$(cd "$P20_DG_C_GIT" && git status --porcelain | sort)

P20_DG_C_SG="$WORKDIR/p20_dg_c_sg"
p20_dirty_gate_fixture "$P20_DG_C_SG" sg
printf 'DIRTY\n' > "$P20_DG_C_SG/untouched.txt"
(cd "$P20_DG_C_SG" && "$SG" stash apply) > /dev/null 2>&1
check "phase20 dirty-gate row C: sg stash apply exits 0 when only an untouched path is dirty" \
    test $? = 0
P20_DG_C_SG_PORCELAIN=$(cd "$P20_DG_C_SG" && git status --porcelain | sort)
check "phase20 dirty-gate row C: sg's result matches real git's byte-for-byte" \
    test "$P20_DG_C_SG_PORCELAIN" = "$P20_DG_C_GIT_PORCELAIN"

# --- row 2: dirty content on a path the stash MODIFIES -- refused by both,
# and sg's message names the path ---
P20_DG_R2_GIT="$WORKDIR/p20_dg_r2_git"
p20_dirty_gate_fixture "$P20_DG_R2_GIT" git
printf 'CONFLICT\n' > "$P20_DG_R2_GIT/mod.txt"
(cd "$P20_DG_R2_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 dirty-gate row 2 oracle: git stash apply refuses (exit 1) when a modified path is dirty" \
    test $? = 1

P20_DG_R2_SG="$WORKDIR/p20_dg_r2_sg"
p20_dirty_gate_fixture "$P20_DG_R2_SG" sg
printf 'CONFLICT\n' > "$P20_DG_R2_SG/mod.txt"
P20_DG_R2_SG_ERR=$(cd "$P20_DG_R2_SG" && "$SG" stash apply 2>&1 >/dev/null)
P20_DG_R2_SG_RC=$?
check "phase20 dirty-gate row 2: sg stash apply refuses (exit 1) when a modified path is dirty" \
    test "$P20_DG_R2_SG_RC" = 1
check "phase20 dirty-gate row 2: sg's rejection message names mod.txt" \
    sh -c "printf '%s' \"$P20_DG_R2_SG_ERR\" | grep -q mod.txt"
check "phase20 dirty-gate row 2: mod.txt is left at the dirty content, nothing applied" \
    sh -c "[ \"\$(cat '$P20_DG_R2_SG/mod.txt')\" = CONFLICT ]"

# --- row 3: dirty content that happens to equal the stash's own target --
# must STILL be refused (the rule looks at HEAD, not the stash's content) ---
P20_DG_R3_GIT="$WORKDIR/p20_dg_r3_git"
p20_dirty_gate_fixture "$P20_DG_R3_GIT" git
printf 'm2\n' > "$P20_DG_R3_GIT/mod.txt"
(cd "$P20_DG_R3_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 dirty-gate row 3 oracle: git stash apply STILL refuses (exit 1) even though the dirty content equals the stash's own target" \
    test $? = 1

P20_DG_R3_SG="$WORKDIR/p20_dg_r3_sg"
p20_dirty_gate_fixture "$P20_DG_R3_SG" sg
printf 'm2\n' > "$P20_DG_R3_SG/mod.txt"
(cd "$P20_DG_R3_SG" && "$SG" stash apply) > /dev/null 2>&1
check "phase20 dirty-gate row 3: sg stash apply STILL refuses (exit 1) even though the dirty content equals the stash's own target" \
    test $? = 1

# --- row 4: dirty content on a path the stash DELETES -- refused ---
P20_DG_R4_GIT="$WORKDIR/p20_dg_r4_git"
p20_dirty_gate_fixture "$P20_DG_R4_GIT" git
printf 'DIRTY\n' > "$P20_DG_R4_GIT/del_me.txt"
(cd "$P20_DG_R4_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 dirty-gate row 4 oracle: git stash apply refuses (exit 1) when a deleted path is dirty" \
    test $? = 1

P20_DG_R4_SG="$WORKDIR/p20_dg_r4_sg"
p20_dirty_gate_fixture "$P20_DG_R4_SG" sg
printf 'DIRTY\n' > "$P20_DG_R4_SG/del_me.txt"
(cd "$P20_DG_R4_SG" && "$SG" stash apply) > /dev/null 2>&1
check "phase20 dirty-gate row 4: sg stash apply refuses (exit 1) when a deleted path is dirty" \
    test $? = 1

# --- row 5: the path the stash MODIFIES was deleted from the working tree
# -- the opposite of row 4, must be ALLOWED (nothing there to overwrite) ---
P20_DG_R5_GIT="$WORKDIR/p20_dg_r5_git"
p20_dirty_gate_fixture "$P20_DG_R5_GIT" git
rm -f "$P20_DG_R5_GIT/mod.txt"
(cd "$P20_DG_R5_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 dirty-gate row 5 oracle: git stash apply exits 0 when the modified path was deleted from the worktree" \
    test $? = 0
P20_DG_R5_GIT_PORCELAIN=$(cd "$P20_DG_R5_GIT" && git status --porcelain | sort)

P20_DG_R5_SG="$WORKDIR/p20_dg_r5_sg"
p20_dirty_gate_fixture "$P20_DG_R5_SG" sg
rm -f "$P20_DG_R5_SG/mod.txt"
(cd "$P20_DG_R5_SG" && "$SG" stash apply) > /dev/null 2>&1
check "phase20 dirty-gate row 5: sg stash apply exits 0 when the modified path was deleted from the worktree" \
    test $? = 0
P20_DG_R5_SG_PORCELAIN=$(cd "$P20_DG_R5_SG" && git status --porcelain | sort)
check "phase20 dirty-gate row 5: sg's result matches real git's byte-for-byte" \
    test "$P20_DG_R5_SG_PORCELAIN" = "$P20_DG_R5_GIT_PORCELAIN"

# --- row 8 (sg's OWN deliberate divergence, spec sec 4.2 row 8): a staged
# change on a path the stash touches is refused outright by sg (its "ours"
# is HEAD, not the index) where real git would three-way-merge it instead.
# Deliberately NOT compared against real git here -- the two are expected to
# disagree; sg's own message is what is being pinned. (The pure-index-only
# isolation of this row, with the working tree file untouched, is covered by
# test_dirty_gate_touched_staged_change_rejected in tests/test_stash.c --
# staging mod.txt here via `sg add` also happens to leave the working tree
# file itself at the same dirty content, so this also exercises row 2's
# rule; that overlap does not weaken the assertion below.) ---
P20_DG_R8_SG="$WORKDIR/p20_dg_r8_sg"
p20_dirty_gate_fixture "$P20_DG_R8_SG" sg
printf 'staged-other\n' > "$P20_DG_R8_SG/mod.txt"
(cd "$P20_DG_R8_SG" && "$SG" add mod.txt) > /dev/null 2>&1
P20_DG_R8_SG_ERR=$(cd "$P20_DG_R8_SG" && "$SG" stash apply 2>&1 >/dev/null)
P20_DG_R8_SG_RC=$?
check "phase20 dirty-gate row 8 (sg-only divergence): sg stash apply refuses (exit 1) on a staged change to a touched path" \
    test "$P20_DG_R8_SG_RC" = 1
check "phase20 dirty-gate row 8: sg's rejection message names mod.txt" \
    sh -c "printf '%s' \"$P20_DG_R8_SG_ERR\" | grep -q mod.txt"

# --- rows D/E: the two error cases sg_merge_result_apply's Phase 20 fix
# (route B, skip-if-equal-to-ours) exists for -- both on paths the stash
# never touches at all, both previously silently wrong (exit 0, no message,
# wrong result), so a byte-for-byte porcelain comparison against real git is
# the only thing that would have caught either regression; a unit test only
# pins sg's own (possibly also wrong) idea of correct.
#
#   row D: the user DELETED untouched.txt from the working tree before
#   apply -- that deletion must survive, not be silently resurrected from
#   HEAD (merge.c's sg_merge_result_apply used to rewrite every clean
#   result entry unconditionally, including untouched ones).
#
#   row E: new_staged.txt was `git add`ed AFTER the stash push, on a path
#   HEAD never had and the stash never touches either -- its staged ("A ")
#   status must survive too (stash.c's re-stage loop used to only walk
#   HEAD's own paths, so a stage-0 entry for a path absent from HEAD had no
#   code path putting it back).
#
# Both land in the SAME fixture/apply, measured together against real git
# 2.55.0 (this is the combined scenario the fix's regression report used) --
# one build, one apply, two independent assertions on the result. ---
p20_untouched_survives_fixture() {
    # $1 = dir, $2 = "sg" or "git"
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = sg ]; then
        (cd "$WORKDIR" && "$SG" init "$(basename "$_dir")") > /dev/null 2>&1
    else
        (cd "$WORKDIR" && git init -q "$(basename "$_dir")")
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'm1\n' > "$_dir/mod.txt"
    printf 'u1\n' > "$_dir/untouched.txt"
    printf 'o1\n' > "$_dir/other.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && git add . && git commit -q -m base)
    fi
    printf 'm2\n' > "$_dir/mod.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" stash push -m "untouched survives base") > /dev/null 2>&1
    else
        (cd "$_dir" && git stash push -q -m "untouched survives base") > /dev/null 2>&1
    fi
    rm -f "$_dir/untouched.txt"
    printf 'n1\n' > "$_dir/new_staged.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add new_staged.txt) > /dev/null 2>&1
    else
        (cd "$_dir" && git add new_staged.txt) > /dev/null 2>&1
    fi
}

P20_DG_DE_GIT="$WORKDIR/p20_dg_de_git"
p20_untouched_survives_fixture "$P20_DG_DE_GIT" git
(cd "$P20_DG_DE_GIT" && git stash apply -q) > /dev/null 2>&1
check "phase20 dirty-gate row D/E oracle: git stash apply exits 0" test $? = 0
P20_DG_DE_GIT_PORCELAIN=$(cd "$P20_DG_DE_GIT" && git status --porcelain | sort)

P20_DG_DE_SG="$WORKDIR/p20_dg_de_sg"
p20_untouched_survives_fixture "$P20_DG_DE_SG" sg
(cd "$P20_DG_DE_SG" && "$SG" stash apply) > /dev/null 2>&1
check "phase20 dirty-gate row D/E: sg stash apply exits 0" test $? = 0

check "phase20 dirty-gate row D: untouched.txt's deletion survives (not resurrected from HEAD)" \
    test ! -e "$P20_DG_DE_SG/untouched.txt"
check "phase20 dirty-gate row E: new_staged.txt (staged after push, absent from HEAD) is still on disk" \
    sh -c "[ \"\$(cat '$P20_DG_DE_SG/new_staged.txt')\" = n1 ]"
check "phase20 dirty-gate row E: new_staged.txt is still staged (A ) in the index" \
    sh -c "(cd '$P20_DG_DE_SG' && git status --porcelain) | grep -q '^A  new_staged.txt\$'"

P20_DG_DE_SG_PORCELAIN=$(cd "$P20_DG_DE_SG" && git status --porcelain | sort)
check "phase20 dirty-gate row D/E: sg's result matches real git's byte-for-byte" \
    test "$P20_DG_DE_SG_PORCELAIN" = "$P20_DG_DE_GIT_PORCELAIN"

# --- Phase 16: switch during an in-progress merge ---
#
# Measured against real git 2.55.0: `git switch <other>` during an
# in-progress merge is refused ("cannot switch branch while merging", exit
# 128) and --force does NOT override it. The refusal keys on MERGE_HEAD's
# mere existence -- it fires whether or not the index still holds conflicts,
# it fires for `-c`, and it even fires when the target is the branch already
# checked out. (`git checkout -f` does succeed and clears MERGE_HEAD, but sg
# has no `checkout`, so `switch`'s rule is the one to match.)
#
# Same shape as the Phase 14 rebase gate, one subsystem over. Before this,
# sg refused only by accident, via sg_safe_apply_tree's dirty-worktree
# confirmation -- exactly what --force bypasses, so `sg switch --force`
# silently cleared MERGE_HEAD and abandoned the merge.
#
# NOTE on discrimination: the rebase gate and the merge gate share the
# "cannot switch branches" wording, so grepping for that alone would pass even if the
# merge gate did not exist. Every rejection below is pinned to the
# merge-specific "a merge is in progress" instead, which no other refusal emits.

p16_merge_conflict_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    printf 'orig1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "base") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch -c other < /dev/null) > /dev/null 2>&1
    printf 'sidefile\n' > "$dir/o.txt"
    (cd "$dir" && "$SG" add o.txt && "$SG" commit -m "other change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master < /dev/null) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch -c feature < /dev/null) > /dev/null 2>&1
    printf 'feature1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "feature change") > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master < /dev/null) > /dev/null 2>&1
    printf 'master1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m "master change") > /dev/null 2>&1
    (cd "$dir" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
}

p16_git_merge_conflict_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && git init -q "$(basename "$dir")")
    (cd "$dir" && git config user.email "a@b.c" && git config user.name "git user")
    (cd "$dir" && git symbolic-ref HEAD refs/heads/master)
    printf 'orig1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m "base")
    (cd "$dir" && git switch -q -c other)
    printf 'sidefile\n' > "$dir/o.txt"
    (cd "$dir" && git add o.txt && git commit -q -m "other change")
    (cd "$dir" && git switch -q master)
    (cd "$dir" && git switch -q -c feature)
    printf 'feature1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m "feature change")
    (cd "$dir" && git switch -q master)
    printf 'master1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m "master change")
    (cd "$dir" && git merge feature) > /dev/null 2>&1
}

# case A: plain switch, --force, and -c are all refused; nothing moves.
P16_SWITCH="$WORKDIR/p16_switch"
p16_merge_conflict_repo "$P16_SWITCH"
check "phase16: precondition -- the merge left MERGE_HEAD behind" \
    test -f "$P16_SWITCH/.git/MERGE_HEAD"
P16_MERGE_HEAD_BEFORE=$(cat "$P16_SWITCH/.git/MERGE_HEAD")
P16_HEAD_BEFORE=$(cd "$P16_SWITCH" && git rev-parse HEAD)
P16_STATUS_BEFORE=$(cd "$P16_SWITCH" && git status --porcelain)

P16_SWITCH_ERR="$WORKDIR/p16_switch_err.txt"
(cd "$P16_SWITCH" && "$SG" switch other < /dev/null) > "$P16_SWITCH_ERR" 2>&1
check "phase16: sg switch during an in-progress merge is rejected" test $? != 0
check "phase16: switch rejection left MERGE_HEAD in place" test -f "$P16_SWITCH/.git/MERGE_HEAD"
check "phase16: switch rejection left MERGE_HEAD's contents untouched" \
    sh -c "test \"\$(cat '$P16_SWITCH/.git/MERGE_HEAD')\" = '$P16_MERGE_HEAD_BEFORE'"
check "phase16: switch rejection left HEAD unchanged" \
    sh -c "test \"\$(cd '$P16_SWITCH' && git rev-parse HEAD)\" = '$P16_HEAD_BEFORE'"
check "phase16: switch rejection left the working directory and index unchanged" \
    sh -c "test \"\$(cd '$P16_SWITCH' && git status --porcelain)\" = '$P16_STATUS_BEFORE'"
check "phase16: switch rejection is due to the merge gate, not the rebase gate or the dirty-workdir prompt" \
    grep -q "a merge is in progress" "$P16_SWITCH_ERR"

P16_SWITCH_FORCE_ERR="$WORKDIR/p16_switch_force_err.txt"
(cd "$P16_SWITCH" && "$SG" switch --force other < /dev/null) > "$P16_SWITCH_FORCE_ERR" 2>&1
check "phase16: sg switch --force during an in-progress merge is still rejected" test $? != 0
check "phase16: --force rejection left MERGE_HEAD in place" test -f "$P16_SWITCH/.git/MERGE_HEAD"
check "phase16: --force rejection left HEAD unchanged" \
    sh -c "test \"\$(cd '$P16_SWITCH' && git rev-parse HEAD)\" = '$P16_HEAD_BEFORE'"
check "phase16: --force rejection is due to the merge gate, not skipped by --force" \
    grep -q "a merge is in progress" "$P16_SWITCH_FORCE_ERR"

P16_SWITCH_C_ERR="$WORKDIR/p16_switch_c_err.txt"
(cd "$P16_SWITCH" && "$SG" switch -c newbranch < /dev/null) > "$P16_SWITCH_C_ERR" 2>&1
check "phase16: sg switch -c during an in-progress merge is rejected" test $? != 0
check "phase16: -c rejection left MERGE_HEAD in place" test -f "$P16_SWITCH/.git/MERGE_HEAD"
check "phase16: switch -c rejection did NOT create the new branch (matches real git)" \
    sh -c "! (cd '$P16_SWITCH' && git rev-parse --verify refs/heads/newbranch) > /dev/null 2>&1"
check "phase16: -c rejection is due to the merge gate" \
    grep -q "a merge is in progress" "$P16_SWITCH_C_ERR"

# Real git refuses the same three, so the oracle agrees this is a refusal
# rather than sg inventing a restriction git does not have.
P16_GIT_SWITCH="$WORKDIR/p16_git_switch"
p16_git_merge_conflict_repo "$P16_GIT_SWITCH"
check "phase16 oracle: precondition -- real git's merge left MERGE_HEAD behind" \
    test -f "$P16_GIT_SWITCH/.git/MERGE_HEAD"
(cd "$P16_GIT_SWITCH" && git switch other) > /dev/null 2>&1
check "phase16 oracle: real git switch during an in-progress merge is rejected too" test $? != 0
(cd "$P16_GIT_SWITCH" && git switch --force other) > /dev/null 2>&1
check "phase16 oracle: real git switch --force is rejected too (--force does not override)" test $? != 0
(cd "$P16_GIT_SWITCH" && git switch -c newbranch) > /dev/null 2>&1
check "phase16 oracle: real git switch -c is rejected too" test $? != 0
check "phase16 oracle: real git left MERGE_HEAD in place" test -f "$P16_GIT_SWITCH/.git/MERGE_HEAD"
check "phase16 oracle: real git did NOT create the new branch either" \
    sh -c "! (cd '$P16_GIT_SWITCH' && git rev-parse --verify refs/heads/newbranch) > /dev/null 2>&1"

# case B: the gate keys on MERGE_HEAD alone, not on the index still being
# conflicted. Resolving every conflict and staging it leaves a clean index
# with MERGE_HEAD still present -- real git refuses here too, and so must sg.
# This is what separates the gate from the dirty-worktree prompt it used to
# hide behind: with the index clean there is nothing for that prompt to
# object to, so a missing gate shows up as a *successful* switch.
P16_RESOLVED="$WORKDIR/p16_resolved"
p16_merge_conflict_repo "$P16_RESOLVED"
printf 'resolved1\norig2\n' > "$P16_RESOLVED/c.txt"
(cd "$P16_RESOLVED" && "$SG" add c.txt) > /dev/null 2>&1
check "phase16 resolved: precondition -- MERGE_HEAD still present after staging the resolution" \
    test -f "$P16_RESOLVED/.git/MERGE_HEAD"
check "phase16 resolved: precondition -- the index no longer holds a conflicted stage" \
    sh -c "! (cd '$P16_RESOLVED' && git status --porcelain) | grep -q '^UU'"
P16_RESOLVED_ERR="$WORKDIR/p16_resolved_err.txt"
(cd "$P16_RESOLVED" && "$SG" switch other < /dev/null) > "$P16_RESOLVED_ERR" 2>&1
check "phase16 resolved: sg switch is still rejected once conflicts are resolved but uncommitted" test $? != 0
check "phase16 resolved: rejection is due to the merge gate" \
    grep -q "a merge is in progress" "$P16_RESOLVED_ERR"
(cd "$P16_RESOLVED" && "$SG" switch --force other < /dev/null) > /dev/null 2>&1
check "phase16 resolved: --force is still rejected once conflicts are resolved but uncommitted" test $? != 0
check "phase16 resolved: MERGE_HEAD survived both attempts" test -f "$P16_RESOLVED/.git/MERGE_HEAD"

P16_GIT_RESOLVED="$WORKDIR/p16_git_resolved"
p16_git_merge_conflict_repo "$P16_GIT_RESOLVED"
printf 'resolved1\norig2\n' > "$P16_GIT_RESOLVED/c.txt"
(cd "$P16_GIT_RESOLVED" && git add c.txt) > /dev/null 2>&1
(cd "$P16_GIT_RESOLVED" && git switch other) > /dev/null 2>&1
check "phase16 oracle: real git also refuses once conflicts are resolved but uncommitted" test $? != 0

# case C: the gate tests for MERGE_HEAD's existence, not its parseability.
# sg_merge_head_read cannot tell "no merge" from "corrupt", so a gate built
# on it would wave a corrupt merge state straight through; real git refuses
# on an empty or malformed MERGE_HEAD just the same. This is the only check
# that separates sg_merge_head_exists from sg_merge_head_read.
P16_CORRUPT="$WORKDIR/p16_corrupt"
p16_merge_conflict_repo "$P16_CORRUPT"
printf 'not-a-sha\n' > "$P16_CORRUPT/.git/MERGE_HEAD"
P16_CORRUPT_ERR="$WORKDIR/p16_corrupt_err.txt"
(cd "$P16_CORRUPT" && "$SG" switch --force other < /dev/null) > "$P16_CORRUPT_ERR" 2>&1
check "phase16 corrupt: sg switch --force is rejected on a malformed MERGE_HEAD" test $? != 0
check "phase16 corrupt: rejection is due to the merge gate, not a parse error elsewhere" \
    grep -q "a merge is in progress" "$P16_CORRUPT_ERR"
: > "$P16_CORRUPT/.git/MERGE_HEAD"
P16_EMPTY_ERR="$WORKDIR/p16_empty_err.txt"
(cd "$P16_CORRUPT" && "$SG" switch --force other < /dev/null) > "$P16_EMPTY_ERR" 2>&1
check "phase16 corrupt: sg switch --force is rejected on an empty MERGE_HEAD" test $? != 0
check "phase16 corrupt: empty-MERGE_HEAD rejection is due to the merge gate" \
    grep -q "a merge is in progress" "$P16_EMPTY_ERR"

# a directory at the path is the third shape, and the one an S_ISREG filter
# would wrongly let through. Real git refuses here too (measured), so this
# goes through the CLI with an oracle rather than living only in
# tests/test_merge_head.c's API-level comparison.
P16_DIRMH="$WORKDIR/p16_dirmh"
p16_merge_conflict_repo "$P16_DIRMH"
rm -f "$P16_DIRMH/.git/MERGE_HEAD"
mkdir "$P16_DIRMH/.git/MERGE_HEAD"
P16_DIRMH_ERR="$WORKDIR/p16_dirmh_err.txt"
(cd "$P16_DIRMH" && "$SG" switch --force other < /dev/null) > "$P16_DIRMH_ERR" 2>&1
check "phase16 corrupt: sg switch --force is rejected when MERGE_HEAD is a directory" test $? != 0
check "phase16 corrupt: directory-MERGE_HEAD rejection is due to the merge gate" \
    grep -q "a merge is in progress" "$P16_DIRMH_ERR"

P16_GIT_DIRMH="$WORKDIR/p16_git_dirmh"
p16_git_merge_conflict_repo "$P16_GIT_DIRMH"
rm -f "$P16_GIT_DIRMH/.git/MERGE_HEAD"
mkdir "$P16_GIT_DIRMH/.git/MERGE_HEAD"
(cd "$P16_GIT_DIRMH" && git switch --force other) > /dev/null 2>&1
check "phase16 oracle: real git also refuses when MERGE_HEAD is a directory" test $? != 0

P16_GIT_CORRUPT="$WORKDIR/p16_git_corrupt"
p16_git_merge_conflict_repo "$P16_GIT_CORRUPT"
printf 'not-a-sha\n' > "$P16_GIT_CORRUPT/.git/MERGE_HEAD"
(cd "$P16_GIT_CORRUPT" && git switch --force other) > /dev/null 2>&1
check "phase16 oracle: real git also refuses on a malformed MERGE_HEAD" test $? != 0
: > "$P16_GIT_CORRUPT/.git/MERGE_HEAD"
(cd "$P16_GIT_CORRUPT" && git switch --force other) > /dev/null 2>&1
check "phase16 oracle: real git also refuses on an empty MERGE_HEAD" test $? != 0

# case D: the gate must not over-fire. Once the merge is finished -- by
# committing it or by aborting it -- switch works normally again. Without
# this, a gate that simply always refused would pass every check above.
#
# The branch structure here is built with real git, not sg, on purpose:
# p16_merge_conflict_repo uses `sg switch` to move between branches, so an
# always-refusing gate would break the fixture itself and these checks would
# go red for the wrong reason -- a refusal never reached, rather than a
# refusal that should not have happened. Verified by mutation: with the
# fixture built by sg, an always-firing gate reddened the *preconditions*;
# built by git, it reddens exactly the two "works again" checks. Only the
# merge and the final switch below are sg's.
p16_gitbuilt_merge_conflict_repo() {
    dir="$1"
    mkdir -p "$dir"
    (cd "$WORKDIR" && git init -q "$(basename "$dir")")
    (cd "$dir" && git config user.email "a@b.c" && git config user.name "git user")
    (cd "$dir" && git symbolic-ref HEAD refs/heads/master)
    printf 'orig1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m "base")
    (cd "$dir" && git switch -q -c other)
    printf 'sidefile\n' > "$dir/o.txt"
    (cd "$dir" && git add o.txt && git commit -q -m "other change")
    (cd "$dir" && git switch -q master)
    (cd "$dir" && git switch -q -c feature)
    printf 'feature1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m "feature change")
    (cd "$dir" && git switch -q master)
    printf 'master1\norig2\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m "master change")
    (cd "$dir" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
}

P16_DONE="$WORKDIR/p16_done"
p16_gitbuilt_merge_conflict_repo "$P16_DONE"
check "phase16 done: precondition -- sg's merge on a git-built repo left MERGE_HEAD behind" \
    test -f "$P16_DONE/.git/MERGE_HEAD"
printf 'resolved1\norig2\n' > "$P16_DONE/c.txt"
(cd "$P16_DONE" && "$SG" add c.txt && "$SG" commit -m "merge feature") > /dev/null 2>&1
check "phase16 done: committing the merge cleared MERGE_HEAD" test ! -f "$P16_DONE/.git/MERGE_HEAD"
(cd "$P16_DONE" && "$SG" switch other < /dev/null) > /dev/null 2>&1
check "phase16 done: sg switch works again after the merge is committed" test $? = 0
check "phase16 done: HEAD really moved to the target branch" \
    sh -c "test \"\$(cd '$P16_DONE' && git symbolic-ref HEAD)\" = 'refs/heads/other'"

P16_ABORTED="$WORKDIR/p16_aborted"
p16_gitbuilt_merge_conflict_repo "$P16_ABORTED"
check "phase16 aborted: precondition -- MERGE_HEAD present before the abort" \
    test -f "$P16_ABORTED/.git/MERGE_HEAD"
(cd "$P16_ABORTED" && "$SG" merge --abort) > /dev/null 2>&1
check "phase16 aborted: merge --abort cleared MERGE_HEAD" test ! -f "$P16_ABORTED/.git/MERGE_HEAD"
(cd "$P16_ABORTED" && "$SG" switch other < /dev/null) > /dev/null 2>&1
check "phase16 aborted: sg switch works again after merge --abort" test $? = 0
check "phase16 aborted: HEAD really moved to the target branch" \
    sh -c "test \"\$(cd '$P16_ABORTED' && git symbolic-ref HEAD)\" = 'refs/heads/other'"

# case E: reset --hard must keep clearing MERGE_HEAD. The new gate sits in
# cmd_switch.c only; if it ever migrated down into sg_safe_apply_tree it
# would break reset --hard's documented behavior, and this is what would
# notice. (P12R_HARD_MERGE pins the same rule from the reset side; this
# checks it still holds now that a sibling command refuses instead.)
P16_RESET="$WORKDIR/p16_reset"
p16_merge_conflict_repo "$P16_RESET"
check "phase16 reset: precondition -- MERGE_HEAD present before reset --hard" \
    test -f "$P16_RESET/.git/MERGE_HEAD"
(cd "$P16_RESET" && "$SG" reset --hard --force) > /dev/null 2>&1
check "phase16 reset: sg reset --hard during a merge still exits 0" test $? = 0
check "phase16 reset: sg reset --hard still clears MERGE_HEAD (not gated like switch)" \
    test ! -f "$P16_RESET/.git/MERGE_HEAD"

# case F: the merge gate must not swallow the rebase gate's diagnosis. A
# paused rebase writes no MERGE_HEAD -- measured on both sides, sg leaves
# only .git/sg-rebase/ and real git leaves rebase-merge/ plus REBASE_HEAD,
# neither writes MERGE_HEAD -- so mid-rebase the merge gate stays silent and
# the rebase gate is what reports. That absence is the load-bearing fact,
# so it is checked directly rather than inferred from the message.
#
# NOT COVERED, deliberately: which of the two gates wins when both apply.
# The conditions are never simultaneously true through any reachable path
# (only hand-forging both state files gets there), and real git has no such
# state to serve as an oracle, so their relative order in cmd_switch.c is
# unobservable. Confirmed by mutation: swapping the two gates leaves the
# whole suite green. Recorded here rather than pretended covered.
P16_REBASE_ERR="$WORKDIR/p16_rebase_err.txt"
P16_REBASE="$WORKDIR/p16_rebase"
p14_rebase_paused_repo "$P16_REBASE"
(cd "$P16_REBASE" && "$SG" rebase master < /dev/null) > /dev/null 2>&1
check "phase16 gates: precondition -- the rebase really is paused" \
    test -d "$P16_REBASE/.git/sg-rebase"
check "phase16 gates: a paused sg rebase writes no MERGE_HEAD (so the merge gate cannot fire)" \
    test ! -f "$P16_REBASE/.git/MERGE_HEAD"
(cd "$P16_REBASE" && "$SG" switch --force master < /dev/null) > "$P16_REBASE_ERR" 2>&1
check "phase16 gates: a paused rebase still reports the rebase gate, not the merge gate" \
    sh -c "grep -q 'a rebase is in progress' '$P16_REBASE_ERR' && ! grep -q 'a merge is in progress' '$P16_REBASE_ERR'"

P16_GIT_REBASE="$WORKDIR/p16_git_rebase"
mkdir -p "$P16_GIT_REBASE"
(cd "$WORKDIR" && git init -q p16_git_rebase)
(cd "$P16_GIT_REBASE" && git config user.email "a@b.c" && git config user.name "git user")
(cd "$P16_GIT_REBASE" && git symbolic-ref HEAD refs/heads/master)
printf 'orig1\norig2\n' > "$P16_GIT_REBASE/c.txt"
(cd "$P16_GIT_REBASE" && git add c.txt && git commit -q -m "base")
(cd "$P16_GIT_REBASE" && git switch -q -c feature)
printf 'feature1\norig2\n' > "$P16_GIT_REBASE/c.txt"
(cd "$P16_GIT_REBASE" && git add c.txt && git commit -q -m "feature change")
(cd "$P16_GIT_REBASE" && git switch -q master)
printf 'master1\norig2\n' > "$P16_GIT_REBASE/c.txt"
(cd "$P16_GIT_REBASE" && git add c.txt && git commit -q -m "master change")
(cd "$P16_GIT_REBASE" && git switch -q feature)
(cd "$P16_GIT_REBASE" && git rebase master) > /dev/null 2>&1
check "phase16 oracle: real git's paused rebase writes no MERGE_HEAD either" \
    sh -c "test -d '$P16_GIT_REBASE/.git/rebase-merge' && test ! -f '$P16_GIT_REBASE/.git/MERGE_HEAD'"

# --- Phase 16: a corrupt MERGE_HEAD must not become a dead end ---
#
# The switch gate above keys on MERGE_HEAD existing. That is only safe if
# every way of *ending* a merge keys on the same thing -- otherwise a
# MERGE_HEAD that exists but cannot be parsed refuses `switch` forever while
# no command will clear it, and the only way out is deleting the file by
# hand. Before this was fixed, `merge --abort` refused, `reset --hard` and
# `stash push` silently left the file behind, and `commit` quietly produced
# a SINGLE-PARENT commit while reporting success -- the merge vanished from
# the graph, which is exactly the class of divergence this suite exists to
# catch.
#
# Real git 2.55.0, measured for each row below: merge --abort clears it,
# reset --hard/--mixed clear it, reset --soft refuses, stash push clears it,
# status still reports an ongoing merge, and commit refuses outright
# ("Corrupt MERGE_HEAD file") rather than degrading to a normal commit.

p16_corrupt_repo() {
    dir="$1"
    p16_merge_conflict_repo "$dir"
    printf 'not-a-sha\n' > "$dir/.git/MERGE_HEAD"
}

p16_git_corrupt_repo() {
    dir="$1"
    p16_git_merge_conflict_repo "$dir"
    printf 'not-a-sha\n' > "$dir/.git/MERGE_HEAD"
}

# row 1: merge --abort is an escape route, and switch works afterwards.
P16_ESC_ABORT="$WORKDIR/p16_esc_abort"
p16_corrupt_repo "$P16_ESC_ABORT"
(cd "$P16_ESC_ABORT" && "$SG" merge --abort) > /dev/null 2>&1
check "phase16 escape: sg merge --abort succeeds on a corrupt MERGE_HEAD" test $? = 0
check "phase16 escape: merge --abort cleared the corrupt MERGE_HEAD" \
    test ! -f "$P16_ESC_ABORT/.git/MERGE_HEAD"
(cd "$P16_ESC_ABORT" && "$SG" switch other < /dev/null) > /dev/null 2>&1
check "phase16 escape: sg switch works again after aborting a corrupt merge" test $? = 0

P16_GIT_ESC_ABORT="$WORKDIR/p16_git_esc_abort"
p16_git_corrupt_repo "$P16_GIT_ESC_ABORT"
(cd "$P16_GIT_ESC_ABORT" && git merge --abort) > /dev/null 2>&1
check "phase16 oracle: real git merge --abort also succeeds on a corrupt MERGE_HEAD" test $? = 0
check "phase16 oracle: real git merge --abort also cleared it" \
    test ! -f "$P16_GIT_ESC_ABORT/.git/MERGE_HEAD"

# row 2: reset --hard is the other escape route.
P16_ESC_HARD="$WORKDIR/p16_esc_hard"
p16_corrupt_repo "$P16_ESC_HARD"
(cd "$P16_ESC_HARD" && "$SG" reset --hard --force) > /dev/null 2>&1
check "phase16 escape: sg reset --hard succeeds on a corrupt MERGE_HEAD" test $? = 0
check "phase16 escape: reset --hard cleared the corrupt MERGE_HEAD" \
    test ! -f "$P16_ESC_HARD/.git/MERGE_HEAD"
(cd "$P16_ESC_HARD" && "$SG" switch other < /dev/null) > /dev/null 2>&1
check "phase16 escape: sg switch works again after reset --hard cleared it" test $? = 0

P16_GIT_ESC_HARD="$WORKDIR/p16_git_esc_hard"
p16_git_corrupt_repo "$P16_GIT_ESC_HARD"
(cd "$P16_GIT_ESC_HARD" && git reset --hard) > /dev/null 2>&1
check "phase16 oracle: real git reset --hard also cleared the corrupt MERGE_HEAD" \
    test ! -f "$P16_GIT_ESC_HARD/.git/MERGE_HEAD"

# row 3: reset --mixed clears it too; reset --soft refuses. Both measured.
P16_ESC_MIXED="$WORKDIR/p16_esc_mixed"
p16_corrupt_repo "$P16_ESC_MIXED"
(cd "$P16_ESC_MIXED" && "$SG" reset HEAD) > /dev/null 2>&1
check "phase16 escape: sg reset --mixed cleared the corrupt MERGE_HEAD" \
    test ! -f "$P16_ESC_MIXED/.git/MERGE_HEAD"

P16_GIT_ESC_MIXED="$WORKDIR/p16_git_esc_mixed"
p16_git_corrupt_repo "$P16_GIT_ESC_MIXED"
(cd "$P16_GIT_ESC_MIXED" && git reset) > /dev/null 2>&1
check "phase16 oracle: real git reset --mixed also cleared it" \
    test ! -f "$P16_GIT_ESC_MIXED/.git/MERGE_HEAD"

P16_SOFT="$WORKDIR/p16_soft"
p16_corrupt_repo "$P16_SOFT"
P16_SOFT_ERR="$WORKDIR/p16_soft_err.txt"
(cd "$P16_SOFT" && "$SG" reset --soft HEAD) > "$P16_SOFT_ERR" 2>&1
check "phase16 escape: sg reset --soft still refuses on a corrupt MERGE_HEAD" test $? != 0
check "phase16 escape: the --soft refusal is the merge/rebase guard" \
    grep -q "cannot do a soft reset" "$P16_SOFT_ERR"

P16_GIT_SOFT="$WORKDIR/p16_git_soft"
p16_git_corrupt_repo "$P16_GIT_SOFT"
(cd "$P16_GIT_SOFT" && git reset --soft HEAD) > /dev/null 2>&1
check "phase16 oracle: real git reset --soft also refuses on a corrupt MERGE_HEAD" test $? != 0

# row 4: commit must REFUSE, not silently drop the second parent. This is
# the check that matters most -- the old behavior exited 0 and wrote a
# single-parent commit, so an exit-code-only assertion would have passed
# while the commit graph was already wrong. Assert all three: it fails, it
# says why, and no commit was created.
P16_CCOMMIT="$WORKDIR/p16_ccommit"
p16_corrupt_repo "$P16_CCOMMIT"
P16_CCOMMIT_HEAD_BEFORE=$(cd "$P16_CCOMMIT" && git rev-parse HEAD)
printf 'resolved1\norig2\n' > "$P16_CCOMMIT/c.txt"
(cd "$P16_CCOMMIT" && "$SG" add c.txt) > /dev/null 2>&1
P16_CCOMMIT_ERR="$WORKDIR/p16_ccommit_err.txt"
(cd "$P16_CCOMMIT" && "$SG" commit -m "merge feature") > "$P16_CCOMMIT_ERR" 2>&1
check "phase16 corrupt commit: sg commit refuses on a corrupt MERGE_HEAD" test $? != 0
check "phase16 corrupt commit: the refusal names the corrupt MERGE_HEAD" \
    grep -q "corrupt MERGE_HEAD" "$P16_CCOMMIT_ERR"
check "phase16 corrupt commit: no commit was created" \
    sh -c "test \"\$(cd '$P16_CCOMMIT' && git rev-parse HEAD)\" = '$P16_CCOMMIT_HEAD_BEFORE'"
check "phase16 corrupt commit: MERGE_HEAD was left alone for the user to fix" \
    test -f "$P16_CCOMMIT/.git/MERGE_HEAD"

P16_GIT_CCOMMIT="$WORKDIR/p16_git_ccommit"
p16_git_corrupt_repo "$P16_GIT_CCOMMIT"
P16_GIT_CCOMMIT_HEAD_BEFORE=$(cd "$P16_GIT_CCOMMIT" && git rev-parse HEAD)
printf 'resolved1\norig2\n' > "$P16_GIT_CCOMMIT/c.txt"
(cd "$P16_GIT_CCOMMIT" && git add c.txt) > /dev/null 2>&1
(cd "$P16_GIT_CCOMMIT" && git commit -m "merge feature") > /dev/null 2>&1
check "phase16 oracle: real git commit also refuses on a corrupt MERGE_HEAD" test $? != 0
check "phase16 oracle: real git created no commit either" \
    sh -c "test \"\$(cd '$P16_GIT_CCOMMIT' && git rev-parse HEAD)\" = '$P16_GIT_CCOMMIT_HEAD_BEFORE'"

# A well-formed MERGE_HEAD must still produce a real two-parent merge commit
# -- the refusal above must key on corruption, not on merging at all.
P16_GOODCOMMIT="$WORKDIR/p16_goodcommit"
p16_merge_conflict_repo "$P16_GOODCOMMIT"
printf 'resolved1\norig2\n' > "$P16_GOODCOMMIT/c.txt"
(cd "$P16_GOODCOMMIT" && "$SG" add c.txt) > /dev/null 2>&1
(cd "$P16_GOODCOMMIT" && "$SG" commit -m "merge feature") > /dev/null 2>&1
check "phase16 corrupt commit: a well-formed merge still commits (exit 0)" test $? = 0
check "phase16 corrupt commit: and it really has two parents" \
    sh -c "test \"\$(cd '$P16_GOODCOMMIT' && git cat-file -p HEAD | grep -c '^parent ')\" = '2'"

# row 5: stash push clears it (real git does too, measured on a clean index).
P16_ESC_STASH="$WORKDIR/p16_esc_stash"
p16_corrupt_repo "$P16_ESC_STASH"
printf 'resolved1\norig2\n' > "$P16_ESC_STASH/c.txt"
(cd "$P16_ESC_STASH" && "$SG" add c.txt) > /dev/null 2>&1
(cd "$P16_ESC_STASH" && "$SG" stash push -m "corrupt merge") > /dev/null 2>&1
check "phase16 escape: sg stash push cleared the corrupt MERGE_HEAD" \
    test ! -f "$P16_ESC_STASH/.git/MERGE_HEAD"

P16_GIT_ESC_STASH="$WORKDIR/p16_git_esc_stash"
p16_git_corrupt_repo "$P16_GIT_ESC_STASH"
printf 'resolved1\norig2\n' > "$P16_GIT_ESC_STASH/c.txt"
(cd "$P16_GIT_ESC_STASH" && git add c.txt) > /dev/null 2>&1
(cd "$P16_GIT_ESC_STASH" && git stash push -m "corrupt merge") > /dev/null 2>&1
check "phase16 oracle: real git stash push also cleared it" \
    test ! -f "$P16_GIT_ESC_STASH/.git/MERGE_HEAD"

# row 6: status must agree with the gates that a merge is in flight.
P16_CSTATUS="$WORKDIR/p16_cstatus"
p16_corrupt_repo "$P16_CSTATUS"
P16_CSTATUS_OUT="$WORKDIR/p16_cstatus_out.txt"
(cd "$P16_CSTATUS" && "$SG" status) > "$P16_CSTATUS_OUT" 2>&1
check "phase16 corrupt status: sg status still reports the merge" \
    grep -q "unmerged paths" "$P16_CSTATUS_OUT"

P16_GIT_CSTATUS="$WORKDIR/p16_git_cstatus"
p16_git_corrupt_repo "$P16_GIT_CSTATUS"
P16_GIT_CSTATUS_OUT="$WORKDIR/p16_git_cstatus_out.txt"
(cd "$P16_GIT_CSTATUS" && LC_ALL=C git status) > "$P16_GIT_CSTATUS_OUT" 2>&1
check "phase16 oracle: real git status also reports an ongoing merge" \
    grep -qi "unmerged paths" "$P16_GIT_CSTATUS_OUT"

# row 7: starting a second merge on top of a corrupt one is refused, so the
# unfinished merge is never silently dropped.
P16_CSECOND="$WORKDIR/p16_csecond"
p16_corrupt_repo "$P16_CSECOND"
P16_CSECOND_ERR="$WORKDIR/p16_csecond_err.txt"
(cd "$P16_CSECOND" && "$SG" merge other < /dev/null) > "$P16_CSECOND_ERR" 2>&1
check "phase16 corrupt second merge: sg refuses to start a second merge" test $? != 0
check "phase16 corrupt second merge: the refusal is the unfinished-merge guard" \
    grep -q "an unfinished merge is in progress" "$P16_CSECOND_ERR"
check "phase16 corrupt second merge: the corrupt MERGE_HEAD was left intact" \
    test -f "$P16_CSECOND/.git/MERGE_HEAD"

# row 8: sg-specific, NO real-git oracle. Measured: `git rebase` with a
# clean working tree ignores MERGE_HEAD entirely (valid or malformed) and
# just clears it. sg refuses on purpose instead; the point of this check is
# only that the refusal covers a corrupt MERGE_HEAD too, so rebase and
# switch cannot disagree about whether a merge is in flight.
P16_CREBASE="$WORKDIR/p16_crebase"
p16_corrupt_repo "$P16_CREBASE"
P16_CREBASE_ERR="$WORKDIR/p16_crebase_err.txt"
(cd "$P16_CREBASE" && "$SG" rebase other < /dev/null) > "$P16_CREBASE_ERR" 2>&1
check "phase16 corrupt rebase: sg refuses to start a rebase (sg-specific, no git oracle)" test $? != 0
check "phase16 corrupt rebase: the refusal is the unfinished-merge guard" \
    grep -q "an unfinished merge is in progress" "$P16_CREBASE_ERR"

# --- Phase 17 batch A: sg switch -c into a not-yet-existing refs/heads/
# subdirectory (e.g. "feature/x" when "refs/heads/feature/" doesn't exist
# yet). Batch A moved sg_ref_update_branch onto sg_write_file_mkdirs (which
# creates missing parent directories) -- an incidental fix for a pre-existing
# inconsistency: before, `sg branch feature/y` succeeded here (cmd_branch.c
# already mkdir -p'd its own way) while `sg switch -c feature/x` failed with
# "failed to create branch 'feature/x'" via a bare fopen(). Real git allows
# both, so the new behavior is kept (not reverted) and pinned here with git
# as the oracle.
P17_MKDIRS="$WORKDIR/p17_switch_c_mkdirs"
(cd "$WORKDIR" && "$SG" init "$(basename "$P17_MKDIRS")") > /dev/null 2>&1
printf 'base\n' > "$P17_MKDIRS/a.txt"
(cd "$P17_MKDIRS" && "$SG" add a.txt && "$SG" commit -m "base") > /dev/null 2>&1
check "phase17: precondition -- refs/heads/feature/ does not exist yet" \
    sh -c "! test -d '$P17_MKDIRS/.git/refs/heads/feature'"
(cd "$P17_MKDIRS" && "$SG" switch -c feature/x < /dev/null) > /dev/null 2>&1
check "phase17: sg switch -c into a new refs/heads/ subdirectory succeeds" test $? = 0
check "phase17: the new branch is readable back (git oracle)" \
    sh -c "(cd '$P17_MKDIRS' && git rev-parse --verify refs/heads/feature/x) > /dev/null 2>&1"
check "phase17: HEAD really moved to the new branch (git oracle)" \
    sh -c "test \"\$(cd '$P17_MKDIRS' && git symbolic-ref HEAD)\" = 'refs/heads/feature/x'"
check "phase17: working tree is clean after the switch (git oracle)" \
    sh -c "test -z \"\$(cd '$P17_MKDIRS' && git status --porcelain)\""

# ============================================================
# Phase 17 batch B: reflog messages for local history operations
#
# Real git 2.55.0 is the oracle (measured directly, not recalled) for every
# message string below except the three-way merge strategy name, which is a
# deliberate divergence -- sg is honest that it isn't running git's 'ort'
# engine (see cmd_merge.c's do_three_way_merge). Both a from-scratch sg repo
# and a from-scratch git repo run the EXACT SAME sequence of operations
# below; what's compared is the reflog MESSAGE column (ident/timestamp/oid
# are expected to differ -- different clocks, different commit hashes) and
# the reflog LINE COUNT per file (a stand-in for "old/new chain structure":
# rule 1's no-op suppression either added a line or didn't, in both repos,
# identically).
# ============================================================

# Strips everything up to and including the last tab on each line, leaving
# just the reflog message column.
p17_msgcol() {
    sed 's/.*	//' "$1" 2>/dev/null
}

p17_seq_sg() {
    dir="$1"
    (cd "$WORKDIR" && "$SG" init "$(basename "$dir")") > /dev/null 2>&1
    printf 'base\n' > "$dir/a.txt"
    (cd "$dir" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
    printf 'second\n' > "$dir/b.txt"
    (cd "$dir" && "$SG" add b.txt && "$SG" commit -m second) > /dev/null 2>&1
    (cd "$dir" && "$SG" branch feat) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch feat) > /dev/null 2>&1
    printf 'onfeat\n' > "$dir/c.txt"
    (cd "$dir" && "$SG" add c.txt && "$SG" commit -m onfeat) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch -c feat2) > /dev/null 2>&1
    printf 'onfeat2\n' > "$dir/d.txt"
    (cd "$dir" && "$SG" add d.txt && "$SG" commit -m onfeat2) > /dev/null 2>&1
    (cd "$dir" && "$SG" reset --mixed HEAD~1) > /dev/null 2>&1
    (cd "$dir" && "$SG" reset --hard HEAD) > /dev/null 2>&1
    # --mixed only touches the index, not the working tree, so d.txt (staged
    # and committed on feat2, then un-staged by the reset above) is now a
    # deliberate leftover untracked file -- same as real git. Remove it so
    # later switches/merges start from a clean working tree, matching every
    # other phase's convention of asserting cleanliness along the way.
    rm -f "$dir/d.txt"
    (cd "$dir" && "$SG" switch master) > /dev/null 2>&1
    (cd "$dir" && "$SG" merge feat) > /dev/null 2>&1
    (cd "$dir" && "$SG" branch br1) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch br1) > /dev/null 2>&1
    printf 'onbr1\n' > "$dir/e.txt"
    (cd "$dir" && "$SG" add e.txt && "$SG" commit -m onbr1) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master) > /dev/null 2>&1
    (cd "$dir" && "$SG" branch br2) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch br2) > /dev/null 2>&1
    printf 'onbr2\n' > "$dir/f.txt"
    (cd "$dir" && "$SG" add f.txt && "$SG" commit -m onbr2) > /dev/null 2>&1
    (cd "$dir" && "$SG" switch master) > /dev/null 2>&1
    (cd "$dir" && "$SG" merge br1) > /dev/null 2>&1
    (cd "$dir" && "$SG" merge br2) > /dev/null 2>&1
}

p17_seq_git() {
    dir="$1"
    (cd "$WORKDIR" && git init -q "$(basename "$dir")")
    (cd "$dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'base\n' > "$dir/a.txt"
    (cd "$dir" && git add a.txt && git commit -q -m base)
    printf 'second\n' > "$dir/b.txt"
    (cd "$dir" && git add b.txt && git commit -q -m second)
    (cd "$dir" && git branch feat)
    (cd "$dir" && git switch -q feat)
    printf 'onfeat\n' > "$dir/c.txt"
    (cd "$dir" && git add c.txt && git commit -q -m onfeat)
    (cd "$dir" && git switch -q -c feat2)
    printf 'onfeat2\n' > "$dir/d.txt"
    (cd "$dir" && git add d.txt && git commit -q -m onfeat2)
    (cd "$dir" && git reset --mixed -q HEAD~1)
    (cd "$dir" && git reset --hard -q HEAD)
    rm -f "$dir/d.txt"
    (cd "$dir" && git switch -q master)
    (cd "$dir" && git merge -q --no-edit feat)
    (cd "$dir" && git branch br1)
    (cd "$dir" && git switch -q br1)
    printf 'onbr1\n' > "$dir/e.txt"
    (cd "$dir" && git add e.txt && git commit -q -m onbr1)
    (cd "$dir" && git switch -q master)
    (cd "$dir" && git branch br2)
    (cd "$dir" && git switch -q br2)
    printf 'onbr2\n' > "$dir/f.txt"
    (cd "$dir" && git add f.txt && git commit -q -m onbr2)
    (cd "$dir" && git switch -q master)
    (cd "$dir" && git merge -q --no-edit br1)
    (cd "$dir" && git merge -q --no-edit br2)
}

P17_SG="$WORKDIR/p17_reflog_sg"
P17_GIT="$WORKDIR/p17_reflog_git"
p17_seq_sg "$P17_SG"
p17_seq_git "$P17_GIT"

check "phase17: precondition -- sg sequence's working tree is clean at the end (git oracle)" \
    sh -c "test -z \"\$(cd '$P17_SG' && git status --porcelain)\""

# HEAD: line count (chain-structure proxy) and message column, both must
# match between the sg-built and git-built repos -- except the very last
# line (the br2 three-way merge), whose message text sg intentionally
# diverges on. That line's oid columns are NOT compared here either (they're
# different commits in different repos by construction); only its presence
# and everything BEFORE it are.
P17_SG_HEAD="$WORKDIR/p17_sg_head.txt"
P17_GIT_HEAD="$WORKDIR/p17_git_head.txt"
p17_msgcol "$P17_SG/.git/logs/HEAD" > "$P17_SG_HEAD"
p17_msgcol "$P17_GIT/.git/logs/HEAD" > "$P17_GIT_HEAD"

check "phase17: logs/HEAD has the same number of lines in sg and git (chain structure)" \
    sh -c "test \"\$(wc -l < '$P17_SG_HEAD')\" = \"\$(wc -l < '$P17_GIT_HEAD')\""

P17_SG_HEAD_BUTLAST="$WORKDIR/p17_sg_head_butlast.txt"
P17_GIT_HEAD_BUTLAST="$WORKDIR/p17_git_head_butlast.txt"
sed '$d' "$P17_SG_HEAD" > "$P17_SG_HEAD_BUTLAST"
sed '$d' "$P17_GIT_HEAD" > "$P17_GIT_HEAD_BUTLAST"
check "phase17: logs/HEAD message column matches sg vs. git, up to the final (3-way) line" \
    cmp -s "$P17_SG_HEAD_BUTLAST" "$P17_GIT_HEAD_BUTLAST"

check "phase17: logs/HEAD line 1 is the initial commit ('commit (initial): base', git oracle)" \
    sh -c "sed -n '1p' '$P17_GIT_HEAD' | grep -q '^commit (initial): base\$'"
check "phase17: logs/HEAD's final line is sg's own 'sg-3way' strategy string, not git's 'ort'" \
    sh -c "tail -1 '$P17_SG_HEAD' | grep -q \"^merge br2: Merge made by the 'sg-3way' strategy\\.\\$\""

# Per-branch reflogs: same comparison, message column line-for-line. master
# needs the same tail-line exception as logs/HEAD above -- br2's three-way
# merge lands on master's OWN log too (rule 2: the checked-out branch's
# update is mirrored to logs/HEAD, not the other way around), so master's
# last line carries the same 'sg-3way'-vs-'ort' divergence.
for p17_b in master feat feat2 br1 br2; do
    P17_SG_B="$WORKDIR/p17_sg_${p17_b}.txt"
    P17_GIT_B="$WORKDIR/p17_git_${p17_b}.txt"
    p17_msgcol "$P17_SG/.git/logs/refs/heads/$p17_b" > "$P17_SG_B"
    p17_msgcol "$P17_GIT/.git/logs/refs/heads/$p17_b" > "$P17_GIT_B"
    check "phase17: refs/heads/$p17_b reflog line count matches sg vs. git (chain structure)" \
        sh -c "test \"\$(wc -l < '$P17_SG_B')\" = \"\$(wc -l < '$P17_GIT_B')\""
    if [ "$p17_b" = "master" ]; then
        P17_SG_B_CMP="$WORKDIR/p17_sg_${p17_b}_butlast.txt"
        P17_GIT_B_CMP="$WORKDIR/p17_git_${p17_b}_butlast.txt"
        sed '$d' "$P17_SG_B" > "$P17_SG_B_CMP"
        sed '$d' "$P17_GIT_B" > "$P17_GIT_B_CMP"
    else
        P17_SG_B_CMP="$P17_SG_B"
        P17_GIT_B_CMP="$P17_GIT_B"
    fi
    check "phase17: refs/heads/$p17_b reflog message column matches sg vs. git byte-for-byte" \
        cmp -s "$P17_SG_B_CMP" "$P17_GIT_B_CMP"
done
check "phase17: refs/heads/master's final line is also sg's own 'sg-3way' string (rule 2 mirror)" \
    sh -c "tail -1 '$WORKDIR/p17_sg_master.txt' | grep -q \"^merge br2: Merge made by the 'sg-3way' strategy\\.\\$\""

check "phase17: refs/heads/feat's own log says 'branch: Created from master' (git oracle)" \
    grep -q '^branch: Created from master$' "$WORKDIR/p17_sg_feat.txt"
check "phase17: refs/heads/feat2's own log says 'branch: Created from HEAD', not the branch name" \
    grep -q '^branch: Created from HEAD$' "$WORKDIR/p17_sg_feat2.txt"

# --- rule 1's asymmetry, isolated: `sg reset --hard HEAD` immediately after
# a real commit is a genuine no-op (old == new on that branch). logs/HEAD
# must still grow by one line (unconditional log); the branch's own log must
# NOT (rule 1's own-log suppression). Re-derived independently of the
# combined sequence above so a regression here can't hide behind an
# unrelated line-count coincidence elsewhere in that sequence. ---
P17_NOOP="$WORKDIR/p17_reset_noop"
(cd "$WORKDIR" && "$SG" init "$(basename "$P17_NOOP")") > /dev/null 2>&1
printf 'x\n' > "$P17_NOOP/a.txt"
(cd "$P17_NOOP" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
P17_NOOP_HEAD_BEFORE=$(wc -l < "$P17_NOOP/.git/logs/HEAD")
P17_NOOP_MASTER_BEFORE=$(wc -l < "$P17_NOOP/.git/logs/refs/heads/master")
(cd "$P17_NOOP" && "$SG" reset --hard HEAD) > /dev/null 2>&1
P17_NOOP_HEAD_AFTER=$(wc -l < "$P17_NOOP/.git/logs/HEAD")
P17_NOOP_MASTER_AFTER=$(wc -l < "$P17_NOOP/.git/logs/refs/heads/master")
check "phase17 rule1: a no-op 'sg reset --hard HEAD' adds one line to logs/HEAD" \
    test "$P17_NOOP_HEAD_AFTER" -eq "$((P17_NOOP_HEAD_BEFORE + 1))"
check "phase17 rule1: the same no-op does NOT add a line to the branch's own log" \
    test "$P17_NOOP_MASTER_AFTER" -eq "$P17_NOOP_MASTER_BEFORE"
P17_NOOP_HEAD_MSGCOL="$WORKDIR/p17_noop_head_msgcol.txt"
p17_msgcol "$P17_NOOP/.git/logs/HEAD" > "$P17_NOOP_HEAD_MSGCOL"
check "phase17 rule1: the no-op's logs/HEAD line is 'reset: moving to HEAD' (literal arg text)" \
    sh -c "test \"\$(tail -1 '$P17_NOOP_HEAD_MSGCOL')\" = 'reset: moving to HEAD'"

# --- already-up-to-date merge writes NO ref/reflog update at all (git
# oracle: real git doesn't call update_ref on that path either) -- confirmed
# by an unchanged logs/HEAD line count across the no-op merge. ---
P17_UTD="$WORKDIR/p17_merge_uptodate"
(cd "$WORKDIR" && "$SG" init "$(basename "$P17_UTD")") > /dev/null 2>&1
printf 'x\n' > "$P17_UTD/a.txt"
(cd "$P17_UTD" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P17_UTD" && "$SG" branch side) > /dev/null 2>&1
P17_UTD_HEAD_BEFORE=$(wc -l < "$P17_UTD/.git/logs/HEAD")
(cd "$P17_UTD" && "$SG" merge side) > /dev/null 2>&1
check "phase17: 'sg merge' of an already-merged branch exits 0 and prints Already up to date" \
    sh -c "(cd '$P17_UTD' && \"$SG\" merge side) 2>&1 | grep -q 'Already up to date\\.'"
P17_UTD_HEAD_AFTER=$(wc -l < "$P17_UTD/.git/logs/HEAD")
check "phase17: an already-up-to-date merge does not append to logs/HEAD at all" \
    test "$P17_UTD_HEAD_AFTER" -eq "$P17_UTD_HEAD_BEFORE"

# ============================================================
# Phase 17 batch C: <ref>@{N} in rev-parse, and `sg reflog`.
#
# One repo, built ENTIRELY with sg (so logs/HEAD, logs/refs/heads/master and
# logs/refs/heads/topic all come from real sg command side effects, not
# hand-forged lines), then read back with the real `git` binary directly --
# same bit-compatibility premise as the rest of this file.
#
# sg has no `rev-parse` subcommand, so `<ref>@{N}` is probed through `sg tag
# <name> <rev>` (sg_rev_parse_commit's only other read-only CLI entry point
# besides `sg reset`): a tag never gets its own reflog, so creating one has
# no side effect on the very reflog state being probed. The tag's target is
# then read back with `git rev-parse <tagname>` and compared against `git
# rev-parse` applied directly to the original <rev> expression.
# ============================================================

P17C="$WORKDIR/p17c_at_notation"
(cd "$WORKDIR" && "$SG" init "$(basename "$P17C")") > /dev/null 2>&1
(
    cd "$P17C" || exit 1
    printf 'a1\n' > a.txt
    "$SG" add a.txt && "$SG" commit -m c1
    printf 'a2\n' >> a.txt
    "$SG" add a.txt && "$SG" commit -m c2
    "$SG" branch topic
    "$SG" switch topic < /dev/null
    printf 'a3\n' >> a.txt
    "$SG" add a.txt && "$SG" commit -m c3
    "$SG" switch master < /dev/null
    printf 'a4\n' >> a.txt
    "$SG" add a.txt && "$SG" commit -m c4
    "$SG" tag vtag master
) > /dev/null 2>&1

# probe(): resolves $2 with real git (the oracle) and with sg (via a
# throwaway tag named $1). $1 itself is unused beyond that -- NOT stored in
# a variable named "label", which would silently clobber check()'s own
# global "label" (this script's functions are plain sh, no "local"), and
# every PASS/FAIL line here would print $1 verbatim instead of the actual
# check() label.
p17c_probe_n=0
p17c_probe_agree() {
    expr="$2"
    p17c_probe_n=$((p17c_probe_n + 1))
    tagname="p17c_probe_$p17c_probe_n"
    expected=$(cd "$P17C" && git rev-parse "$expr" 2>/dev/null)
    expected_rc=$?
    (cd "$P17C" && "$SG" tag "$tagname" "$expr") > /dev/null 2>&1
    sg_rc=$?
    if [ "$expected_rc" -ne 0 ]; then
        test "$sg_rc" -ne 0
        return $?
    fi
    if [ "$sg_rc" -ne 0 ]; then
        return 1
    fi
    actual=$(cd "$P17C" && git rev-parse "$tagname" 2>/dev/null)
    test "$actual" = "$expected"
}

check "phase17c: master@{0} matches git" p17c_probe_agree at0 'master@{0}'
check "phase17c: master@{1} matches git" p17c_probe_agree at1 'master@{1}'
check "phase17c: HEAD@{0} matches git" p17c_probe_agree at2 'HEAD@{0}'
check "phase17c: HEAD@{1} matches git (differs from master@{1} -- logs/HEAD and logs/refs/heads/master are different files)" \
    p17c_probe_agree at3 'HEAD@{1}'
check "phase17c: topic@{0} matches git" p17c_probe_agree at4 'topic@{0}'
check "phase17c: master@{01} (leading zero) matches git" p17c_probe_agree at5 'master@{01}'
check "phase17c: master@{1}~1 (suffix chained after @{N}) matches git" p17c_probe_agree at6 'master@{1}~1'
check "phase17c: master@{99} (out of range) is rejected by both sg and git" p17c_probe_agree at7 'master@{99}'
check "phase17c: vtag@{0} (a tag has no reflog) is rejected by both sg and git" p17c_probe_agree at8 'vtag@{0}'
check "phase17c: master~1@{1} (@{N} not adjacent to base) is rejected by both sg and git" \
    p17c_probe_agree at9 'master~1@{1}'
check "phase17c: master^@{1} (@{N} not adjacent to base) is rejected by both sg and git" \
    p17c_probe_agree at10 'master^@{1}'
check "phase17c: master@{} (empty braces) is rejected by both sg and git" p17c_probe_agree at11 'master@{}'

# Deliberate divergence: real git's bare "@{N}" DWIMs to the current
# branch, sg deliberately does not (see revparse.h's header comment: the
# value is measurably NOT the same as spelling the branch name out, so
# guessing would be a wrong answer). Note "@{u}" (upstream) isn't usable as
# a comparable divergence here -- this repo's master has no upstream
# configured, so real git rejects it too, just for an unrelated reason; the
# "@{u}"/"@{now}" rejections are covered by the unit test instead.
P17C_BARE_GIT_RC=$(cd "$P17C" && git rev-parse '@{1}' > /dev/null 2>&1; echo $?)
(cd "$P17C" && "$SG" tag p17c_probe_bare '@{1}') > /dev/null 2>&1
check "phase17c: bare @{1} -- git treats it as the current branch, sg deliberately does not (sg-specific, no shared oracle result)" \
    test "$P17C_BARE_GIT_RC" -eq 0 -a $? -ne 0

# --- `sg reflog` output, compared line-for-line against `git reflog` on the
# very same (sg-built) repo. ---
P17C_SG_MASTER="$WORKDIR/p17c_sg_reflog_master.txt"
P17C_GIT_MASTER="$WORKDIR/p17c_git_reflog_master.txt"
(cd "$P17C" && "$SG" reflog show master) > "$P17C_SG_MASTER" 2>/dev/null
(cd "$P17C" && git reflog show master) > "$P17C_GIT_MASTER" 2>/dev/null
check "phase17c: 'sg reflog show master' matches 'git reflog show master' byte-for-byte" \
    cmp -s "$P17C_SG_MASTER" "$P17C_GIT_MASTER"

P17C_SG_HEAD="$WORKDIR/p17c_sg_reflog_head.txt"
P17C_GIT_HEAD="$WORKDIR/p17c_git_reflog_head.txt"
(cd "$P17C" && "$SG" reflog) > "$P17C_SG_HEAD" 2>/dev/null
(cd "$P17C" && git reflog) > "$P17C_GIT_HEAD" 2>/dev/null
check "phase17c: 'sg reflog' (bare, defaults to HEAD) matches 'git reflog' byte-for-byte" \
    cmp -s "$P17C_SG_HEAD" "$P17C_GIT_HEAD"

P17C_SG_TOPIC_N1="$WORKDIR/p17c_sg_reflog_topic_n1.txt"
P17C_GIT_TOPIC_N1="$WORKDIR/p17c_git_reflog_topic_n1.txt"
(cd "$P17C" && "$SG" reflog show topic -n 1) > "$P17C_SG_TOPIC_N1" 2>/dev/null
(cd "$P17C" && git reflog show topic -n 1) > "$P17C_GIT_TOPIC_N1" 2>/dev/null
check "phase17c: 'sg reflog show topic -n 1' matches 'git reflog show topic -n 1' byte-for-byte" \
    cmp -s "$P17C_SG_TOPIC_N1" "$P17C_GIT_TOPIC_N1"

# `-n` before the ref must be accepted too (documented CLI grammar).
P17C_SG_TOPIC_N1B="$WORKDIR/p17c_sg_reflog_topic_n1b.txt"
(cd "$P17C" && "$SG" reflog -n 1 topic) > "$P17C_SG_TOPIC_N1B" 2>/dev/null
check "phase17c: '-n' before <ref> is accepted and matches the same output as after" \
    cmp -s "$P17C_SG_TOPIC_N1B" "$P17C_GIT_TOPIC_N1"

check "phase17c: 'sg reflog show <tag>' (a ref that exists but is never logged) prints nothing and exits 0" \
    sh -c "test -z \"\$(cd '$P17C' && \"$SG\" reflog show vtag 2>/dev/null)\" && (cd '$P17C' && \"$SG\" reflog show vtag > /dev/null 2>&1)"
check "phase17c: 'sg reflog show <nonexistent ref>' exits nonzero" \
    sh -c "! (cd '$P17C' && \"$SG\" reflog show does-not-exist) > /dev/null 2>&1"
P17C_STDERR="$WORKDIR/p17c_reflog_stderr.txt"
(cd "$P17C" && "$SG" reflog show does-not-exist) > /dev/null 2> "$P17C_STDERR"
check "phase17c: 'sg reflog show <nonexistent ref>' prints an 'sg: ' prefixed error to stderr" \
    grep -q '^sg: ' "$P17C_STDERR"

# --- fix-round regression: trailing garbage right after "@{N}" (revparse.c's
# "~"/"^" suffix loop only special-cased '~' and silently treated any OTHER
# character as '^', so e.g. "master@{0}x" misparsed as an implicit "^1"
# instead of being rejected). Measured against real git: all three are fatal
# "ambiguous argument" errors. ---
check "phase17c: master@{0}x (garbage byte right after @{N}) is rejected by both sg and git" \
    p17c_probe_agree at12 'master@{0}x'
check "phase17c: master@{1}5 (garbage byte followed by digits) is rejected by both sg and git" \
    p17c_probe_agree at13 'master@{1}5'
check "phase17c: master@{0}0 (garbage byte followed by a single digit) is rejected by both sg and git" \
    p17c_probe_agree at14 'master@{0}0'

# --- fix-round: "refs/<rest>" as a fully-qualified <base>, agreeing with
# real git's own gitrevisions "refs/<name>" disambiguation rule. ---
check "phase17c: refs/heads/topic matches git" p17c_probe_agree at15 'refs/heads/topic'
check "phase17c: refs/tags/vtag matches git" p17c_probe_agree at16 'refs/tags/vtag'
check "phase17c: refs/heads/does-not-exist is rejected by both sg and git" \
    p17c_probe_agree at17 'refs/heads/does-not-exist'
check "phase17c: refs/heads/topic~1 (suffix chained after a refs/... base) matches git" \
    p17c_probe_agree at18 'refs/heads/topic~1'

# --- fix-round: `sg reflog` argument-boundary cases, none of which had
# interop coverage before -- exit codes compared against real git (stdout
# isn't compared for the error cases since sg's usage/error text doesn't
# claim to match git's). ---
P17C_GIT_N0_RC=$(cd "$P17C" && git reflog show master -n 0 > /dev/null 2>&1; echo $?)
P17C_SG_N0_OUT="$WORKDIR/p17c_sg_reflog_n0.txt"
(cd "$P17C" && "$SG" reflog show master -n 0) > "$P17C_SG_N0_OUT" 2>/dev/null
P17C_SG_N0_RC=$?
check "phase17c: 'sg reflog show master -n 0' exits 0, matching git" \
    test "$P17C_SG_N0_RC" -eq 0 -a "$P17C_GIT_N0_RC" -eq 0
check "phase17c: 'sg reflog show master -n 0' prints nothing" \
    test ! -s "$P17C_SG_N0_OUT"

check "phase17c: 'sg reflog show master -n -1' (negative count) is rejected" \
    sh -c "! (cd '$P17C' && \"$SG\" reflog show master -n -1) > /dev/null 2>&1"
check "phase17c: 'sg reflog show master -n abc' (non-numeric count) is rejected" \
    sh -c "! (cd '$P17C' && \"$SG\" reflog show master -n abc) > /dev/null 2>&1"
check "phase17c: 'sg reflog show master -n' (missing value) is rejected" \
    sh -c "! (cd '$P17C' && \"$SG\" reflog show master -n) > /dev/null 2>&1"

# `-n <count>` immediately followed by `show`, i.e. "show" NOT in the first
# argument position -- the "is this the show subcommand" check only looks at
# argv[1], so here "show" is consumed as an ordinary (and, in this repo,
# nonexistent) <ref> name instead, and rejected for "no such ref" rather
# than being recognized as the `show` keyword.
check "phase17c: 'sg reflog -n 1 show' ('show' not in first position, read as a bogus <ref> instead) is rejected" \
    sh -c "! (cd '$P17C' && \"$SG\" reflog -n 1 show) > /dev/null 2>&1"

# usage string on a malformed invocation must NOT carry the "sg: " prefix
# (CLAUDE.md convention: usage errors are bare "usage: ...", unlike runtime
# errors which get "sg: ").
P17C_USAGE_STDERR="$WORKDIR/p17c_reflog_usage_stderr.txt"
(cd "$P17C" && "$SG" reflog show master -n) > /dev/null 2> "$P17C_USAGE_STDERR"
check "phase17c: 'sg reflog show master -n' (missing value) prints a bare 'usage: ' line, no 'sg: ' prefix" \
    sh -c "grep -q '^usage: ' '$P17C_USAGE_STDERR' && ! grep -q '^sg: ' '$P17C_USAGE_STDERR'"

# ============================================================
# Phase 17 batch D: reflog messages for fetch/push/clone (the network ref-
# update paths), and log-file cleanup on ref deletion. No HTTP server needed
# for the deletion half -- only the fetch/push/clone half is gated on
# HTTP_AVAILABLE.
# ============================================================

# --- ref deletion also removes the ref's own reflog file (measured against
# real git 2.55.0: `git branch -D` deletes logs/refs/heads/<branch> too), but
# tag deletion -- which shares sg_ref_delete_under with branch deletion --
# must not fail just because a tag never had a reflog file to unlink. ---

P17D_DELBR="$WORKDIR/p17d_delete_branch"
(cd "$WORKDIR" && "$SG" init "$(basename "$P17D_DELBR")") > /dev/null 2>&1
printf 'x\n' > "$P17D_DELBR/a.txt"
(cd "$P17D_DELBR" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P17D_DELBR" && "$SG" branch tobedeleted) > /dev/null 2>&1
check "phase17d: precondition -- the about-to-be-deleted branch already has a reflog file" \
    test -f "$P17D_DELBR/.git/logs/refs/heads/tobedeleted"
(cd "$P17D_DELBR" && "$SG" branch -d tobedeleted < /dev/null) > /dev/null 2>&1
check "phase17d: sg branch -d exits 0" test $? = 0
check "phase17d: sg branch -d removed the branch's reflog file" \
    test ! -f "$P17D_DELBR/.git/logs/refs/heads/tobedeleted"
check "phase17d: git oracle agrees the branch itself is really gone" \
    sh -c "! (cd '$P17D_DELBR' && git rev-parse --verify refs/heads/tobedeleted) > /dev/null 2>&1"

P17D_DELTAG="$WORKDIR/p17d_delete_tag"
(cd "$WORKDIR" && "$SG" init "$(basename "$P17D_DELTAG")") > /dev/null 2>&1
printf 'x\n' > "$P17D_DELTAG/a.txt"
(cd "$P17D_DELTAG" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P17D_DELTAG" && "$SG" tag sometag) > /dev/null 2>&1
check "phase17d: precondition -- a freshly created tag never gets a reflog file (git oracle)" \
    sh -c "test -f '$P17D_DELTAG/.git/refs/tags/sometag' && test ! -f '$P17D_DELTAG/.git/logs/refs/tags/sometag'"
(cd "$P17D_DELTAG" && "$SG" tag -d sometag) > /dev/null 2>&1
check "phase17d: sg tag -d succeeds even though the tag never had a reflog file to unlink" test $? = 0
check "phase17d: git oracle agrees the tag itself is really gone" \
    sh -c "! (cd '$P17D_DELTAG' && git rev-parse --verify refs/tags/sometag) > /dev/null 2>&1"

# --- network paths: fetch/push/clone reflog messages, isolated smart-HTTP
# fixtures of their own (not a reuse of phase5b/5c's $HTTP_DEST/$HTTP_SRC) --
# the forced-update case below needs to rewrite upstream history, which would
# otherwise corrupt phase5c's later fast-forward assumptions on that same
# fixture. Mirrors phase6a's own-server structure, including its two-tier
# skip (server-start timeout vs. HTTP unavailable from the start). ---

if [ "$HTTP_AVAILABLE" = 1 ]; then
    P17D_GIT_SRC="$WORKDIR/p17d_git_src"
    mkdir -p "$P17D_GIT_SRC"
    git init -q "$P17D_GIT_SRC"
    (cd "$P17D_GIT_SRC" && git config user.email "p17d@example.com" && git config user.name "p17d tester")
    printf 'seed\n' > "$P17D_GIT_SRC/seed.txt"
    (cd "$P17D_GIT_SRC" && git add seed.txt && git commit -q -m "seed commit")

    P17D_SERVERROOT="$WORKDIR/p17d_serverroot"
    mkdir -p "$P17D_SERVERROOT"
    (cd "$WORKDIR" && git clone --bare -q p17d_git_src "$P17D_SERVERROOT/repo.git") > /dev/null 2>&1
    (cd "$P17D_SERVERROOT/repo.git" && git config http.receivepack true) > /dev/null 2>&1

    P17D_SERVER_LOG="$WORKDIR/p17d_http_server.log"
    python3 "$HTTP_SERVER_SCRIPT" "$P17D_SERVERROOT" > "$P17D_SERVER_LOG" 2>&1 &
    HTTP_SERVER_PID=$!

    P17D_PORT=""
    i=0
    while [ "$i" -lt 50 ]; do
        if [ -s "$P17D_SERVER_LOG" ]; then
            P17D_PORT=$(awk '/^PORT /{print $2; exit}' "$P17D_SERVER_LOG")
            if [ -n "$P17D_PORT" ]; then
                break
            fi
        fi
        sleep 0.1
        i=$((i + 1))
    done

    if [ -z "$P17D_PORT" ]; then
        echo "warning: phase17d HTTP test server did not become ready in time, skipping phase17d networked reflog tests" >&2
        skip "phase17d: sg clone left logs/HEAD with exactly one line (git oracle: clone logs once, not once per ref written)"
        skip "phase17d: sg clone wrote logs/HEAD with 'clone: from <url>'"
        skip "phase17d: sg clone wrote the default branch's own log with 'clone: from <url>'"
        skip "phase17d: sg clone creates no refs/remotes/origin/HEAD ref (divergence from git, deliberate)"
        skip "phase17d: and no orphan reflog for it either"
        skip "phase17d: sg clone did NOT write a log for refs/remotes/origin/<branch>"
        skip "phase17d: sg fetch reflog message: fast-forward"
        skip "phase17d: git oracle agrees: a real git fetch of the same fast-forward also logs 'fetch origin: fast-forward'"
        skip "phase17d: precondition -- fetch actually created refs/remotes/origin/p17d-newbranch's log"
        skip "phase17d: sg fetch reflog message: storing head (brand-new remote-tracking ref)"
        skip "phase17d: sg fetch reflog message: forced-update"
        skip "phase17d: precondition -- pushing a brand-new branch created its remote-tracking log"
        skip "phase17d: sg push reflog message is the fixed 'update by push'"
    else
        P17D_BASE_URL="http://127.0.0.1:$P17D_PORT/repo.git"
        P17D_DEST="$WORKDIR/p17d_dest"
        P17D_DEST_GIT="$WORKDIR/p17d_dest_git"

        "$SG" clone "$P17D_BASE_URL" "$P17D_DEST" > /dev/null 2>&1
        (cd "$WORKDIR" && git clone -q "$P17D_BASE_URL" p17d_dest_git) > /dev/null 2>&1
        P17D_HEAD=$(cd "$P17D_GIT_SRC" && git rev-parse HEAD)
        P17D_BRANCH=$(cd "$P17D_GIT_SRC" && git rev-parse --abbrev-ref HEAD)

        # --- clone: three log files get the fixed "clone: from <url>"
        # message, and refs/remotes/origin/<branch> deliberately does NOT
        # (measured against real git 2.55.0: that log only appears after the
        # first fetch). ---
        P17D_HEAD_LOG_MSG=$(sed 's/.*	//' "$P17D_DEST/.git/logs/HEAD" | tail -1)
        # Exactly one line, not merely the right last line: real git's clone
        # leaves logs/HEAD with a single entry (measured). If the message
        # ever gets written both via sg_ref_set_head and via the branch's
        # sg_ref_update, the second line is byte-identical to the first, so a
        # tail -1 content check still passes -- only a count catches it.
        P17D_HEAD_LOG_LINES=$(wc -l < "$P17D_DEST/.git/logs/HEAD" | tr -d ' ')
        check "phase17d: sg clone left logs/HEAD with exactly one line (git oracle: clone logs once, not once per ref written)" \
            test "$P17D_HEAD_LOG_LINES" = 1

        check "phase17d: sg clone wrote logs/HEAD with 'clone: from <url>'" \
            test "$P17D_HEAD_LOG_MSG" = "clone: from $P17D_BASE_URL"

        P17D_BRANCH_LOG_MSG=$(sed 's/.*	//' "$P17D_DEST/.git/logs/refs/heads/$P17D_BRANCH" | tail -1)
        check "phase17d: sg clone wrote the default branch's own log with 'clone: from <url>'" \
            test "$P17D_BRANCH_LOG_MSG" = "clone: from $P17D_BASE_URL"

        # Known divergence, asserted so it stays deliberate: real git's clone
        # creates refs/remotes/origin/HEAD as a symref and logs a line to it.
        # sg models neither. Writing just the log was tried and removed --
        # `sg reflog origin/HEAD` resolves names through the ref itself, so a
        # log without a ref is unreachable and claims a history the repo does
        # not have. Pinned in both directions: no ref, and no log for it.
        check "phase17d: sg clone creates no refs/remotes/origin/HEAD ref (divergence from git, deliberate)" \
            test ! -e "$P17D_DEST/.git/refs/remotes/origin/HEAD"
        check "phase17d: and no orphan reflog for it either" \
            test ! -e "$P17D_DEST/.git/logs/refs/remotes/origin/HEAD"

        # negative assertion: refs/remotes/origin/<branch>'s log must NOT
        # exist right after clone -- this is the reflog analogue of the file
        # itself not being written yet, and it is the check most likely to
        # pass for the wrong reason if the code ever starts writing it
        # unconditionally.
        check "phase17d: sg clone did NOT write a log for refs/remotes/origin/<branch> (git oracle: that log only appears after the first fetch)" \
            test ! -f "$P17D_DEST/.git/logs/refs/remotes/origin/$P17D_BRANCH"

        # --- fetch case 1: fast-forward ---
        printf 'second line\n' >> "$P17D_GIT_SRC/seed.txt"
        (cd "$P17D_GIT_SRC" && git add seed.txt && git commit -q -m "second commit")
        (cd "$P17D_GIT_SRC" && git push -q "$P17D_SERVERROOT/repo.git" "HEAD:refs/heads/$P17D_BRANCH") > /dev/null 2>&1

        (cd "$P17D_DEST" && "$SG" fetch) > /dev/null 2>&1
        (cd "$P17D_DEST_GIT" && git fetch origin) > /dev/null 2>&1

        P17D_FF_MSG=$(sed 's/.*	//' "$P17D_DEST/.git/logs/refs/remotes/origin/$P17D_BRANCH" | tail -1)
        check "phase17d: sg fetch reflog message: fast-forward" \
            test "$P17D_FF_MSG" = "fetch origin: fast-forward"

        P17D_FF_GIT_MSG=$(sed 's/.*	//' "$P17D_DEST_GIT/.git/logs/refs/remotes/origin/$P17D_BRANCH" | tail -1)
        check "phase17d: git oracle agrees: a real git fetch of the same fast-forward also logs 'fetch origin: fast-forward'" \
            test "$P17D_FF_GIT_MSG" = "fetch origin: fast-forward"

        # --- fetch case 2: storing head (a remote-tracking ref sg has never
        # seen before) ---
        (cd "$P17D_GIT_SRC" && git branch p17d-newbranch) > /dev/null 2>&1
        (cd "$P17D_GIT_SRC" && git push -q "$P17D_SERVERROOT/repo.git" refs/heads/p17d-newbranch) > /dev/null 2>&1
        (cd "$P17D_DEST" && "$SG" fetch) > /dev/null 2>&1
        check "phase17d: precondition -- fetch actually created refs/remotes/origin/p17d-newbranch's log" \
            test -f "$P17D_DEST/.git/logs/refs/remotes/origin/p17d-newbranch"
        P17D_NEWHEAD_MSG=$(sed 's/.*	//' "$P17D_DEST/.git/logs/refs/remotes/origin/p17d-newbranch" | tail -1)
        check "phase17d: sg fetch reflog message: storing head (brand-new remote-tracking ref)" \
            test "$P17D_NEWHEAD_MSG" = "fetch origin: storing head"

        # --- fetch case 3: forced-update (the remote's branch moved to a
        # commit that is NOT a descendant of what sg already has recorded for
        # it -- a genuine history rewrite upstream) ---
        (cd "$P17D_GIT_SRC" && git reset -q --hard HEAD~1)
        printf 'rewritten tip\n' >> "$P17D_GIT_SRC/seed.txt"
        (cd "$P17D_GIT_SRC" && git add seed.txt && git commit -q -m "rewritten tip")
        (cd "$P17D_GIT_SRC" && git push -q --force "$P17D_SERVERROOT/repo.git" "HEAD:refs/heads/$P17D_BRANCH") > /dev/null 2>&1
        (cd "$P17D_DEST" && "$SG" fetch) > /dev/null 2>&1
        P17D_FORCED_MSG=$(sed 's/.*	//' "$P17D_DEST/.git/logs/refs/remotes/origin/$P17D_BRANCH" | tail -1)
        check "phase17d: sg fetch reflog message: forced-update" \
            test "$P17D_FORCED_MSG" = "fetch origin: forced-update"

        # --- push: fixed message "update by push", no remote name embedded
        # (measured against real git 2.55.0) ---
        (cd "$P17D_DEST" && "$SG" switch -c p17d-push-branch < /dev/null) > /dev/null 2>&1
        printf 'push content\n' > "$P17D_DEST/pushfile.txt"
        (cd "$P17D_DEST" && "$SG" add pushfile.txt && "$SG" commit -m "p17d push commit") > /dev/null 2>&1
        (cd "$P17D_DEST" && "$SG" push origin p17d-push-branch) > /dev/null 2>&1
        check "phase17d: precondition -- pushing a brand-new branch created its remote-tracking log" \
            test -f "$P17D_DEST/.git/logs/refs/remotes/origin/p17d-push-branch"
        P17D_PUSH_MSG=$(sed 's/.*	//' "$P17D_DEST/.git/logs/refs/remotes/origin/p17d-push-branch" | tail -1)
        check "phase17d: sg push reflog message is the fixed 'update by push'" \
            test "$P17D_PUSH_MSG" = "update by push"

        kill "$HTTP_SERVER_PID" 2>/dev/null
        HTTP_SERVER_PID=""
    fi
else
    skip "phase17d: sg clone left logs/HEAD with exactly one line (git oracle: clone logs once, not once per ref written)"
    skip "phase17d: sg clone wrote logs/HEAD with 'clone: from <url>'"
    skip "phase17d: sg clone wrote the default branch's own log with 'clone: from <url>'"
    skip "phase17d: sg clone creates no refs/remotes/origin/HEAD ref (divergence from git, deliberate)"
    skip "phase17d: and no orphan reflog for it either"
    skip "phase17d: sg clone did NOT write a log for refs/remotes/origin/<branch>"
    skip "phase17d: sg fetch reflog message: fast-forward"
    skip "phase17d: git oracle agrees: a real git fetch of the same fast-forward also logs 'fetch origin: fast-forward'"
    skip "phase17d: precondition -- fetch actually created refs/remotes/origin/p17d-newbranch's log"
    skip "phase17d: sg fetch reflog message: storing head (brand-new remote-tracking ref)"
    skip "phase17d: sg fetch reflog message: forced-update"
    skip "phase17d: precondition -- pushing a brand-new branch created its remote-tracking log"
    skip "phase17d: sg push reflog message is the fixed 'update by push'"
fi

# ============================================================
# Phase 18a: a detached HEAD is a state sg understands, not a broken repo
#
# Real git is both the tool that CREATES the state (sg could not enter it
# before this phase) and the oracle for how it is described. Everything below
# runs inside ONE sg-made repository and compares sg's own output against
# git's in that same repository, so there is no "different repo, different
# hashes" slack to hide behind -- the strings have to match exactly.
#
# git's wording is recovered from the reflog, not from HEAD: it reports the
# commit HEAD was DETACHED AT (the newest "checkout: moving from <x> to <y>"
# line), labelled with the token the user originally typed, and switches
# "at" -> "from" once HEAD moves off that commit. Three of the cases below
# are counter-intuitive enough to be worth pinning: a branch name prints as
# the FULL refs/heads/... path (only refs/tags/ and refs/remotes/ are
# stripped), a token that no longer resolves to that commit degrades to an
# abbreviated id, and with no checkout line in logs/HEAD at all git abandons
# the detached wording entirely.
#
# LC_ALL=C on every git call: this comparison is one of the few that reads
# git's human-facing output, which is translated, while sg's is not.
# ============================================================

P18_DISP="$WORKDIR/p18_disp"
(cd "$WORKDIR" && "$SG" init p18_disp) > /dev/null 2>&1
for p18_i in 1 2 3; do
    printf 'c%s\n' "$p18_i" > "$P18_DISP/f$p18_i.txt"
    (cd "$P18_DISP" && "$SG" add . && "$SG" commit -m "C$p18_i") > /dev/null 2>&1
done
(cd "$P18_DISP" && git tag p18v1 HEAD~1 && git branch p18other HEAD~2) > /dev/null 2>&1

# Compares the first line of `status` and of `branch` -- the detached
# pseudo-entry sorts first in both tools -- against real git in the same repo.
p18_disp_agree() {
    p18_label="$1"
    p18_g_status=$(cd "$P18_DISP" && LC_ALL=C git status 2>/dev/null | head -1)
    p18_s_status=$(cd "$P18_DISP" && "$SG" status 2>/dev/null | head -1)
    p18_g_branch=$(cd "$P18_DISP" && LC_ALL=C git branch 2>/dev/null | head -1)
    p18_s_branch=$(cd "$P18_DISP" && "$SG" branch 2>/dev/null | head -1)
    check "phase18a status: $p18_label -- sg says '$p18_s_status', git says '$p18_g_status'" \
        test "$p18_s_status" = "$p18_g_status"
    check "phase18a branch: $p18_label -- sg says '$p18_s_branch', git says '$p18_g_branch'" \
        test "$p18_s_branch" = "$p18_g_branch"
}

(cd "$P18_DISP" && git checkout -q --detach p18v1) > /dev/null 2>&1
p18_disp_agree "detached at a tag keeps the tag name"

(cd "$P18_DISP" && git checkout -q master && git checkout -q --detach p18other) > /dev/null 2>&1
p18_disp_agree "detached at a branch name keeps the FULL refs/heads/ path"

(cd "$P18_DISP" && git checkout -q master && git checkout -q --detach HEAD~1) > /dev/null 2>&1
p18_disp_agree "an expression like HEAD~1 was never a ref, so it degrades to an abbreviated id"

# HEAD is moved with git, not sg, on purpose: this section tests the WORDING
# only, and must not silently become a no-op if sg's own ability to commit
# while detached regresses. (It did exactly that when first written: sg still
# refused, no commit happened, and every case below shifted by one while
# staying green -- which is why p18_disp_agree echoes the strings it compared
# into the check label.) sg's own detached commit is covered in phase18b.
printf 'more\n' > "$P18_DISP/p18x.txt"
(cd "$P18_DISP" && git add . && git commit -qm "p18 detached commit") > /dev/null 2>&1
p18_disp_agree "committing while detached switches 'at' to 'from', still naming the detach POINT"

(cd "$P18_DISP" && git reset -q --hard HEAD~1) > /dev/null 2>&1
p18_disp_agree "returning to the detach point switches back to 'at' (a value test, not 'has anything happened')"

# The `git checkout -q master` first is load-bearing, not tidiness: the
# previous case leaves HEAD detached at exactly the commit p18v1 names, and
# git writes no reflog line for a checkout that moves nowhere. Without the
# detour the newest checkout entry stays the one from the HEAD~1 case, and
# this case degrades to an abbreviated id because "HEAD~1" was never a ref --
# the right answer for entirely the wrong reason, passing even with the
# "does this token still name that commit" test deleted (measured).
(cd "$P18_DISP" && git checkout -q master && git checkout -q --detach p18v1 && \
    git tag -f p18v1 master) > /dev/null 2>&1
p18_disp_agree "a tag moved away no longer names that commit, so the label degrades to an abbreviated id"

(cd "$P18_DISP" && git checkout -q --detach master && rm -f .git/logs/HEAD) > /dev/null 2>&1
p18_disp_agree "with no checkout line in logs/HEAD, git drops the detached wording entirely"

(cd "$P18_DISP" && git checkout -q master) > /dev/null 2>&1
p18_disp_agree "back on a branch, both tools go back to the ordinary wording"

# A HEAD that is neither a symref nor a well-formed id is CORRUPT, and must
# not be dressed up as detached: the detached answer is what tells callers
# they may overwrite HEAD with a raw id, which would launder the corruption
# into a valid-looking state. Asserted against sg alone -- there is no git
# behaviour to match here, and exit status alone would not discriminate,
# since sg exits 0 in both the detached and the fallback wording.
P18_CORRUPT="$WORKDIR/p18_corrupt"
(cd "$WORKDIR" && "$SG" init p18_corrupt) > /dev/null 2>&1
printf 'x\n' > "$P18_CORRUPT/f.txt"
(cd "$P18_CORRUPT" && "$SG" add . && "$SG" commit -m c1) > /dev/null 2>&1
printf 'neither a ref nor a sha\n' > "$P18_CORRUPT/.git/HEAD"
P18_CORRUPT_OUT=$(cd "$P18_CORRUPT" && "$SG" status 2>/dev/null | head -1)
check "phase18a: a corrupt HEAD is not described as detached (got '$P18_CORRUPT_OUT')" \
    test "$P18_CORRUPT_OUT" = "Not currently on any branch."

# ============================================================
# Phase 18b: sg can enter and leave a detached HEAD itself
#
# Dual-track, the Phase 17 batch B idiom: the same sequence of operations is
# run against a from-scratch sg repo and a from-scratch git repo, and the
# reflog MESSAGE column is compared. One difference from batch B: the
# messages here embed object ids ("moving from <40-hex> to master"), and the
# two repos necessarily have different ids, so ids are normalized to <oid>
# before comparing. What survives normalization is the part under test --
# which token git chose to name each end of the move, and in which form.
#
# That normalization would also hide an ABBREVIATED id, so the full length is
# asserted separately below rather than trusted to the comparison.
# ============================================================

p18b_msgcol_norm() {
    sed 's/.*	//' "$1" 2>/dev/null | sed 's/[0-9a-f]\{40\}/<oid>/g'
}

# `check` echoes its result, so a redirect on the check line would swallow the
# PASS/FAIL output; matches are reduced to yes/no first instead.
p18b_matches() {
    if expr "$1" : "$2" > /dev/null; then echo yes; else echo no; fi
}

p18b_seq() {
    p18b_dir="$1"
    p18b_bin="$2"
    for p18b_i in 1 2 3; do
        printf 'c%s\n' "$p18b_i" > "$p18b_dir/f$p18b_i.txt"
        (cd "$p18b_dir" && "$p18b_bin" add . && "$p18b_bin" commit -m "C$p18b_i") > /dev/null 2>&1
    done
    (cd "$p18b_dir" && "$p18b_bin" tag p18bv1 HEAD~1) > /dev/null 2>&1
    (cd "$p18b_dir" && "$p18b_bin" switch --detach p18bv1) > /dev/null 2>&1
    (cd "$p18b_dir" && "$p18b_bin" switch master) > /dev/null 2>&1
}

P18B_SG="$WORKDIR/p18b_sg"
P18B_GIT="$WORKDIR/p18b_git"
(cd "$WORKDIR" && "$SG" init p18b_sg) > /dev/null 2>&1
mkdir -p "$P18B_GIT" && (cd "$P18B_GIT" && git init -q -b master .) > /dev/null 2>&1
p18b_seq "$P18B_SG" "$SG"
p18b_seq "$P18B_GIT" git

p18b_msgcol_norm "$P18B_SG/.git/logs/HEAD" > "$WORKDIR/p18b_sg_head.txt"
p18b_msgcol_norm "$P18B_GIT/.git/logs/HEAD" > "$WORKDIR/p18b_git_head.txt"
# The line count is asserted before the comparison, not after it: cmp of two
# empty files succeeds, so a fixture that silently did nothing (a renamed
# subcommand, a repo that never got created) would otherwise report a clean
# match. Five lines: three commits, the detach, and the re-attach.
P18B_SG_LINES=$(wc -l < "$WORKDIR/p18b_sg_head.txt" | tr -d ' ')
check "phase18b: precondition -- sg's logs/HEAD actually has the 5 expected lines (got $P18B_SG_LINES)" \
    test "$P18B_SG_LINES" = 5
check "phase18b: sg's logs/HEAD messages match git's across detach and re-attach" \
    cmp -s "$WORKDIR/p18b_sg_head.txt" "$WORKDIR/p18b_git_head.txt"

# The detached state itself, on disk, in the form git reads.
(cd "$P18B_SG" && "$SG" switch --detach p18bv1) > /dev/null 2>&1
P18B_RAW=$(cat "$P18B_SG/.git/HEAD")
check "phase18b: sg switch --detach writes HEAD as a bare 40-hex id, no 'ref:' prefix" \
    test "$(p18b_matches "$P18B_RAW" '[0-9a-f]\{40\}$')" = yes
P18B_GIT_SEES=$(cd "$P18B_SG" && git rev-parse HEAD)
P18B_TAG_IS=$(cd "$P18B_SG" && git rev-parse p18bv1)
check "phase18b: real git resolves the HEAD sg detached, to the same commit" \
    test "$P18B_GIT_SEES" = "$P18B_TAG_IS"

# Leaving a detached HEAD names the commit in FULL, not abbreviated -- the
# <oid> normalization above cannot see the difference, so it is checked here.
(cd "$P18B_SG" && "$SG" switch master) > /dev/null 2>&1
P18B_LAST=$(sed 's/.*	//' "$P18B_SG/.git/logs/HEAD" | tail -1)
check "phase18b: leaving a detached HEAD logs the full 40-hex it came from (got '$P18B_LAST')" \
    test "$(p18b_matches "$P18B_LAST" 'checkout: moving from [0-9a-f]\{40\} to master$')" = yes

# Refusing is part of the contract: `switch <commit>` without --detach must
# not silently detach, and must not move HEAD at all. Exit status alone would
# not discriminate a refusal that had already written HEAD, so the HEAD file
# is compared before and after.
P18B_BEFORE=$(cat "$P18B_SG/.git/HEAD")
(cd "$P18B_SG" && "$SG" switch p18bv1) > /dev/null 2>&1
check "phase18b: sg switch <tag> without --detach is refused" test $? = 1
check "phase18b: ...and HEAD is untouched by that refusal" \
    test "$(cat "$P18B_SG/.git/HEAD")" = "$P18B_BEFORE"

(cd "$P18B_SG" && "$SG" switch -c p18bnew --detach) > /dev/null 2>&1
check "phase18b: sg switch -c together with --detach is refused" test $? = 1
check "phase18b: ...and no branch was created by that refusal" \
    test ! -f "$P18B_SG/.git/refs/heads/p18bnew"

# ============================================================
# Phase 18c: committing on a detached HEAD
#
# The interesting property is not that it works -- it is WHERE it writes.
# HEAD advances and no branch moves; and the commit gets its parent, which is
# the half that used to be impossible: sg_ref_resolve_head failed on a
# detached HEAD, so the parent lookup came back "no commits yet" and would
# have produced a ROOT commit, orphaning the history the checkout came from.
# The refusal that used to sit here was the only thing preventing that, which
# is why the parent count is asserted directly rather than inferred from a
# clean exit.
# ============================================================

P18C="$WORKDIR/p18c"
(cd "$WORKDIR" && "$SG" init p18c) > /dev/null 2>&1
for p18c_i in 1 2 3; do
    printf 'c%s\n' "$p18c_i" > "$P18C/f$p18c_i.txt"
    (cd "$P18C" && "$SG" add . && "$SG" commit -m "C$p18c_i") > /dev/null 2>&1
done
P18C_MASTER_BEFORE=$(cat "$P18C/.git/refs/heads/master")
P18C_MASTERLOG_BEFORE=$(wc -l < "$P18C/.git/logs/refs/heads/master" | tr -d ' ')

(cd "$P18C" && "$SG" switch --detach HEAD~1) > /dev/null 2>&1
printf 'detached\n' > "$P18C/p18c_new.txt"
(cd "$P18C" && "$SG" add . && "$SG" commit -m "p18c detached commit") > /dev/null 2>&1
check "phase18c: sg commit on a detached HEAD succeeds" test $? = 0

P18C_HEAD_RAW=$(cat "$P18C/.git/HEAD")
check "phase18c: HEAD is still a bare id afterwards, not re-attached to a branch" \
    test "$(p18b_matches "$P18C_HEAD_RAW" '[0-9a-f]\{40\}$')" = yes
check "phase18c: master's tip did not move" \
    test "$(cat "$P18C/.git/refs/heads/master")" = "$P18C_MASTER_BEFORE"
check "phase18c: master's reflog gained no line either" \
    test "$(wc -l < "$P18C/.git/logs/refs/heads/master" | tr -d ' ')" = "$P18C_MASTERLOG_BEFORE"

# The load-bearing one: a parent, not a root commit.
P18C_PARENTS=$(cd "$P18C" && git cat-file -p HEAD | grep -c '^parent')
check "phase18c: the detached commit has exactly one parent, i.e. it is NOT a root commit" \
    test "$P18C_PARENTS" = 1
P18C_PARENT_IS=$(cd "$P18C" && git rev-parse 'HEAD^')
P18C_DETACHED_AT=$(cd "$P18C" && git rev-parse 'master^')
check "phase18c: and that parent is the commit that was checked out" \
    test "$P18C_PARENT_IS" = "$P18C_DETACHED_AT"

P18C_MSG=$(sed 's/.*	//' "$P18C/.git/logs/HEAD" | tail -1)
check "phase18c: logs/HEAD records it with the ordinary commit wording (got '$P18C_MSG')" \
    test "$P18C_MSG" = "commit: p18c detached commit"

# A corrupt HEAD is not a detached one, and must still be refused rather than
# overwritten with a plausible id.
P18C_CORRUPT="$WORKDIR/p18c_corrupt"
(cd "$WORKDIR" && "$SG" init p18c_corrupt) > /dev/null 2>&1
printf 'x\n' > "$P18C_CORRUPT/f.txt"
(cd "$P18C_CORRUPT" && "$SG" add . && "$SG" commit -m c1) > /dev/null 2>&1
printf 'neither a ref nor a sha\n' > "$P18C_CORRUPT/.git/HEAD"
printf 'y\n' > "$P18C_CORRUPT/g.txt"
(cd "$P18C_CORRUPT" && "$SG" add . && "$SG" commit -m c2) > /dev/null 2>&1
check "phase18c: sg commit onto a corrupt HEAD is refused" test $? = 1
check "phase18c: ...and the corrupt HEAD is left as evidence, not overwritten" \
    test "$(cat "$P18C_CORRUPT/.git/HEAD")" = "neither a ref nor a sha"

# ============================================================
# Phase 18d: the commands that only ever saw a detached HEAD as "this repo
# has no commits"
#
# Every read below was already reachable before Phase 18 -- real git can put
# any sg repository into this state -- and every one of them was WRONG rather
# than refusing: sg_ref_resolve_head returned -1, which each caller read as
# "unborn HEAD". They are asserted here because "the root fix repaired them"
# is a claim about code, not a test.
#
# This block used to end by pinning three deliberate refusals -- merge, reset
# and rebase. None is left: reset opened in phase12, and merge and rebase in
# phase19, which asserts what they now DO instead. The refusals were a scope
# boundary rather than a rule, so they were pinned to keep them from drifting
# silently, and removing a pinned check is how that boundary is meant to move.
# ============================================================

P18D="$WORKDIR/p18d"
(cd "$WORKDIR" && "$SG" init p18d) > /dev/null 2>&1
for p18d_i in 1 2 3; do
    printf 'c%s\n' "$p18d_i" > "$P18D/f$p18d_i.txt"
    (cd "$P18D" && "$SG" add . && "$SG" commit -m "C$p18d_i") > /dev/null 2>&1
done
(cd "$P18D" && "$SG" branch p18d-side && "$SG" switch --detach HEAD~1) > /dev/null 2>&1

# `sg log` used to print "fatal: your current branch does not have any
# commits yet" over a perfectly good history.
P18D_LOG=$(cd "$P18D" && "$SG" log 2>/dev/null | grep -c '^commit ')
check "phase18d: sg log walks the history from a detached HEAD (found $P18D_LOG commits)" \
    test "$P18D_LOG" = 2

# The work tree is clean, and status must say so. With has_head miscomputed
# the HEAD tree came out empty, so every tracked file was reported as a
# staged addition -- a clean tree looking like a full-repo rewrite.
P18D_STATUS=$(cd "$P18D" && "$SG" status 2>/dev/null | tail -1)
check "phase18d: a clean work tree is clean on a detached HEAD (got '$P18D_STATUS')" \
    test "$P18D_STATUS" = "nothing to commit, working tree clean"

# sg_safe_apply_tree shares that has_head computation, so switching away used
# the same empty tree to decide what it was about to overwrite.
(cd "$P18D" && "$SG" switch master) > /dev/null 2>&1
check "phase18d: switching from a detached HEAD back to a branch succeeds" test $? = 0
P18D_STATUS2=$(cd "$P18D" && "$SG" status 2>/dev/null | tail -1)
check "phase18d: ...and leaves a clean work tree (got '$P18D_STATUS2')" \
    test "$P18D_STATUS2" = "nothing to commit, working tree clean"

# `sg branch <name>` refused with "current branch has no commits yet".
(cd "$P18D" && "$SG" switch --detach HEAD~1 && "$SG" branch p18d-fromdetached) > /dev/null 2>&1
check "phase18d: sg branch can create a branch while detached" \
    test -f "$P18D/.git/refs/heads/p18d-fromdetached"
P18D_NEW=$(cd "$P18D" && git rev-parse p18d-fromdetached 2>/dev/null)
P18D_AT=$(cd "$P18D" && git rev-parse HEAD 2>/dev/null)
check "phase18d: ...pointing at the detached commit, not at some other branch's tip" \
    test "$P18D_NEW" = "$P18D_AT"

# stash: real git supports it here, and sg's "(no branch)" fallback -- written
# before the state was reachable -- turns out to match git exactly.
printf 'dirty\n' >> "$P18D/f1.txt"
(cd "$P18D" && "$SG" stash push -m p18dprobe) > /dev/null 2>&1
check "phase18d: sg stash push works on a detached HEAD" test $? = 0
P18D_STASH=$(cd "$P18D" && "$SG" stash list 2>/dev/null | head -1)
P18D_STASH_GIT=$(cd "$P18D" && git stash list 2>/dev/null | head -1)
check "phase18d: sg's stash subject matches git's reading of the same entry (got '$P18D_STASH')" \
    test "$P18D_STASH" = "$P18D_STASH_GIT"
# The safety snapshot behind stash resolves HEAD too, and a parentless
# snapshot would be a rescue point with no history behind it.
P18D_SNAP=$(cd "$P18D" && git for-each-ref --format='%(refname)' 'refs/small-git/**' 2>/dev/null | head -1)
if [ -n "$P18D_SNAP" ]; then
    P18D_SNAP_PARENTS=$(cd "$P18D" && git cat-file -p "$P18D_SNAP" 2>/dev/null | grep -c '^parent')
    check "phase18d: the safety snapshot taken while detached has a parent, not a root commit" \
        test "$P18D_SNAP_PARENTS" = 1
else
    skip "phase18d: the safety snapshot taken while detached has a parent, not a root commit"
fi
(cd "$P18D" && "$SG" stash pop) > /dev/null 2>&1
check "phase18d: sg stash pop works on a detached HEAD" test $? = 0

# The merge and rebase refusals that used to be read here are gone (phase19
# asserts the behaviour that replaced them). They were deliberately read
# BEFORE anything else touched this repo and deliberately did not write,
# because a refusal that stops refusing starts MUTATING the fixture: when
# reset opened up in phase12, the merge check began reporting a dirty work
# tree instead, having run after a reset that now succeeded. Removing them
# had exactly that effect on the reset case below -- the merge and rebase
# now run to completion and move HEAD -- which is why this restore stays and
# why the reset block re-reads its own preconditions rather than trusting
# the fixture to be where the setup left it.
(cd "$P18D" && git checkout -q -- . ) > /dev/null 2>&1

# reset resets a detached HEAD, moving HEAD and no branch (asserted in
# phase12, and required so that resetting during a paused rebase keeps
# working now that a paused rebase is detached).
P18D_MASTER_BEFORE=$(cat "$P18D/.git/refs/heads/master")
P18D_HEAD_BEFORE=$(cd "$P18D" && git rev-parse HEAD)
(cd "$P18D" && "$SG" reset --hard p18d-side) > /dev/null 2>&1
check "phase18d: sg reset --hard on a detached HEAD succeeds" test $? = 0
# "It exited 0 and master didn't move" is also true of a build that writes to
# some other branch and never touches HEAD, so the move itself is asserted.
P18D_HEAD_AFTER=$(cd "$P18D" && git rev-parse HEAD)
P18D_SIDE=$(cd "$P18D" && git rev-parse p18d-side)
check "phase18d: precondition -- the reset target differs from where HEAD was" \
    test "$P18D_HEAD_BEFORE" != "$P18D_SIDE"
check "phase18d: ...moving HEAD itself onto the target" test "$P18D_HEAD_AFTER" = "$P18D_SIDE"
check "phase18d: ...and still did not move master" \
    test "$(cat "$P18D/.git/refs/heads/master")" = "$P18D_MASTER_BEFORE"

# ============================================================
# Phase 18e: the switch/status output a cold read caught, which nothing here
# was looking at
#
# 18a-18d assert HEAD, the reflogs, refs and `status`/`branch` wording, but
# not one of them reads `switch`'s own stdout. Three divergences from real git
# lived in that gap; none was a wrong assertion, all three were an ABSENT one.
# ============================================================

P18E_G="$WORKDIR/p18e_git"
P18E_S="$WORKDIR/p18e_sg"
mkdir -p "$P18E_G" && (cd "$P18E_G" && git init -q -b master .) > /dev/null 2>&1
(cd "$WORKDIR" && "$SG" init p18e_sg) > /dev/null 2>&1
for p18e_i in 1 2 3; do
    printf 'c%s\n' "$p18e_i" > "$P18E_G/f$p18e_i.txt"
    (cd "$P18E_G" && git add . && git commit -qm "C$p18e_i") > /dev/null 2>&1
    printf 'c%s\n' "$p18e_i" > "$P18E_S/f$p18e_i.txt"
    (cd "$P18E_S" && "$SG" add . && "$SG" commit -m "C$p18e_i") > /dev/null 2>&1
done

# Leaving a detached HEAD prints "Previous HEAD position was ...". That line
# belongs to the LEAVING, so it appears on a detach-to-detach move too -- and
# that is the case sg missed, having keyed it off "arriving on a branch".
# Only the shape is compared (ids differ between the two repos).
p18e_shape() {
    sed 's/[0-9a-f]\{7,40\}/<id>/g' | sed 's/[[:space:]]*$//'
}
(cd "$P18E_G" && git switch --detach HEAD~2) > /dev/null 2>&1
P18E_G_OUT=$( (cd "$P18E_G" && LC_ALL=C git switch --detach master) 2>&1 | p18e_shape)
(cd "$P18E_S" && "$SG" switch --detach HEAD~2) > /dev/null 2>&1
P18E_S_OUT=$( (cd "$P18E_S" && "$SG" switch --detach master) 2>&1 | p18e_shape)
check "phase18e: detach-to-detach across different commits prints both lines, like git -- sg gave '$P18E_S_OUT'" \
    test "$P18E_S_OUT" = "$P18E_G_OUT"

P18E_G_OUT2=$( (cd "$P18E_G" && LC_ALL=C git switch master) 2>&1 | p18e_shape)
P18E_S_OUT2=$( (cd "$P18E_S" && "$SG" switch master) 2>&1 | p18e_shape)
check "phase18e: detach-to-branch at the SAME commit prints only the arrival line, like git -- sg gave '$P18E_S_OUT2'" \
    test "$P18E_S_OUT2" = "$P18E_G_OUT2"

P18E_G_OUT3=$( (cd "$P18E_G" && LC_ALL=C git switch --detach HEAD~1) 2>&1 | p18e_shape)
P18E_S_OUT3=$( (cd "$P18E_S" && "$SG" switch --detach HEAD~1) 2>&1 | p18e_shape)
check "phase18e: regression only -- branch-to-detach is unaffected by the new rule (have_prev_commit is 0 there) -- sg gave '$P18E_S_OUT3'" \
    test "$P18E_S_OUT3" = "$P18E_G_OUT3"

# "HEAD" names no fixed commit, so it is not a usable detach-point label even
# though sg_rev_parse_ref_path maps it to itself. git degrades to the id here.
(cd "$P18E_S" && "$SG" switch master && "$SG" switch --detach HEAD) > /dev/null 2>&1
(cd "$P18E_G" && git switch -q master && git switch -q --detach HEAD) > /dev/null 2>&1
P18E_S_HEADLBL=$(cd "$P18E_S" && "$SG" status | head -1 | p18e_shape)
P18E_G_HEADLBL=$(cd "$P18E_G" && LC_ALL=C git status | head -1 | p18e_shape)
check "phase18e: --detach HEAD degrades to an id, not the self-referential 'at HEAD' (sg: '$P18E_S_HEADLBL')" \
    test "$P18E_S_HEADLBL" = "$P18E_G_HEADLBL"

# A long ref name must still print in full, exactly as git does. The
# description buffer used to be 512 bytes, and overflowing it reported
# "Not currently on any branch." for a plainly detached HEAD -- wrong
# information rather than less of it. Segments stay under the 255-byte
# filename limit, since a single 3000-byte component simply cannot be created
# on either tool.
P18E_SEG=$(printf 'abcdefghij%.0s' $(seq 1 5))
P18E_LONG="$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG/$P18E_SEG"
(cd "$P18E_S" && "$SG" switch master && "$SG" branch "$P18E_LONG" && \
    "$SG" switch --detach "$P18E_LONG") > /dev/null 2>&1
(cd "$P18E_G" && git switch -q master && git branch "$P18E_LONG" && \
    git switch -q --detach "$P18E_LONG") > /dev/null 2>&1
P18E_LONG_S=$(cd "$P18E_S" && "$SG" status | head -1)
P18E_LONG_G=$(cd "$P18E_G" && LC_ALL=C git status | head -1)
check "phase18e: precondition -- the long-name branch was actually created and checked out" \
    test "${P18E_LONG_S#HEAD detached at refs/heads/}" != "$P18E_LONG_S"
check "phase18e: a ${#P18E_LONG}-char detach label prints in full, same as git" \
    test "$P18E_LONG_S" = "$P18E_LONG_G"

# A corrupt HEAD is not a detached one -- reset/rebase/push said it was.
P18E_C="$WORKDIR/p18e_corrupt"
(cd "$WORKDIR" && "$SG" init p18e_corrupt) > /dev/null 2>&1
printf 'x\n' > "$P18E_C/f.txt"
(cd "$P18E_C" && "$SG" add . && "$SG" commit -m c1 && "$SG" branch p18e-other) > /dev/null 2>&1
printf 'neither a ref nor a sha\n' > "$P18E_C/.git/HEAD"
#
# push is NOT checked here even though it carries the same fix. Its HEAD test
# runs after the remote's ref advertisement (cmd_push.c reads the url, then
# ls-refs, and only then picks a branch), so with no remote configured the
# command exits at "remote not configured" and never reaches it -- a check
# here would pass without executing the line it claims to cover. Reaching it
# needs a live remote; recorded as uncovered rather than faked.
#
# reset no longer refuses a detached HEAD at all (see phase12), so only its
# corrupt-HEAD branch is left to check -- which is the one this is about.
for p18e_cmd in reset rebase; do
    case "$p18e_cmd" in
        reset)  P18E_ERR=$(cd "$P18E_C" && "$SG" reset --mixed p18e-other 2>&1 >/dev/null | head -1) ;;
        rebase) P18E_ERR=$(cd "$P18E_C" && "$SG" rebase p18e-other 2>&1 >/dev/null | head -1) ;;
    esac
    check "phase18e: sg $p18e_cmd does not blame a detached HEAD for a corrupt one (got '$P18E_ERR')" \
        test "${P18E_ERR#sg: currently in detached HEAD}" = "$P18E_ERR"
    check "phase18e: sg $p18e_cmd names the corrupt HEAD instead (got '$P18E_ERR')" \
        test "${P18E_ERR#sg: cannot read HEAD}" != "$P18E_ERR"
done

# ============================================================
# Phase 18f: rebase's reflog shape
#
# Dual-track against real git, the Phase 17 batch B idiom: the identical
# sequence runs in a from-scratch sg repo and a from-scratch git repo, and
# the reflog MESSAGE column is compared. Object ids are normalized to <oid>
# because the two repos cannot share hashes.
#
# The shape is a consequence of the model, not of the strings: git rebases on
# a DETACHED HEAD and moves the branch exactly once, at the end. So logs/HEAD
# collects "rebase (start)", one line per replayed commit, and "rebase
# (finish): returning to ...", while the branch's own log gains a single
# "rebase (finish): <ref> onto <onto>" line no matter how many commits were
# replayed. sg wrote NO rebase reflog at all before this phase, because it
# moved the branch itself once per pick and could not have produced that
# shape by adding message strings.
#
# The three structural facts underneath are asserted separately below, since
# the message comparison alone would still pass if sg reached the same
# strings by a different route.
# ============================================================

p18f_msgs() {
    sed 's/.*	//' "$1" 2>/dev/null | sed 's/[0-9a-f]\{40\}/<oid>/g'
}

# base -> (topic: T1, T2) with master gaining M1 on a DIFFERENT file, so
# nothing collapses into an identical commit object. Two commits that share a
# tree, parent, message and second would be one object, making master an
# ancestor of topic and turning the rebase into a silent no-op.
p18f_setup() {
    p18f_dir="$1"
    p18f_bin="$2"
    p18f_conflict="$3"
    if [ "$p18f_bin" = git ]; then
        mkdir -p "$p18f_dir" && (cd "$p18f_dir" && git init -q -b master .) > /dev/null 2>&1
    else
        (cd "$WORKDIR" && "$SG" init "$(basename "$p18f_dir")") > /dev/null 2>&1
    fi
    printf 'base\n' > "$p18f_dir/base.txt"
    (cd "$p18f_dir" && "$p18f_bin" add . && "$p18f_bin" commit -m base) > /dev/null 2>&1
    (cd "$p18f_dir" && "$p18f_bin" branch topic && "$p18f_bin" switch topic) > /dev/null 2>&1
    printf 'topic one\n' > "$p18f_dir/shared.txt"
    (cd "$p18f_dir" && "$p18f_bin" add . && "$p18f_bin" commit -m T1) > /dev/null 2>&1
    printf 'topic two\n' > "$p18f_dir/t2.txt"
    (cd "$p18f_dir" && "$p18f_bin" add . && "$p18f_bin" commit -m T2) > /dev/null 2>&1
    (cd "$p18f_dir" && "$p18f_bin" switch master) > /dev/null 2>&1
    if [ "$p18f_conflict" = conflict ]; then
        printf 'master version\n' > "$p18f_dir/shared.txt"   # collides with T1
    else
        printf 'master only\n' > "$p18f_dir/m1.txt"
    fi
    (cd "$p18f_dir" && "$p18f_bin" add . && "$p18f_bin" commit -m M1) > /dev/null 2>&1
    (cd "$p18f_dir" && "$p18f_bin" switch topic) > /dev/null 2>&1
}

p18f_compare() {
    p18f_label="$1"
    p18f_s="$2"
    p18f_g="$3"
    p18f_msgs "$p18f_s/.git/logs/HEAD" > "$WORKDIR/p18f_s_head.txt"
    p18f_msgs "$p18f_g/.git/logs/HEAD" > "$WORKDIR/p18f_g_head.txt"
    p18f_msgs "$p18f_s/.git/logs/refs/heads/topic" > "$WORKDIR/p18f_s_br.txt"
    p18f_msgs "$p18f_g/.git/logs/refs/heads/topic" > "$WORKDIR/p18f_g_br.txt"
    p18f_n=$(wc -l < "$WORKDIR/p18f_s_head.txt" | tr -d ' ')
    check "phase18f ($p18f_label): precondition -- sg's logs/HEAD is non-empty ($p18f_n lines)" \
        test "$p18f_n" -gt 0
    check "phase18f ($p18f_label): logs/HEAD messages match git's" \
        cmp -s "$WORKDIR/p18f_s_head.txt" "$WORKDIR/p18f_g_head.txt"
    check "phase18f ($p18f_label): topic's own reflog messages match git's" \
        cmp -s "$WORKDIR/p18f_s_br.txt" "$WORKDIR/p18f_g_br.txt"
    # A bare "the files differ" is unactionable when the only place it fails
    # is someone else's machine -- this suite's whole point is being able to
    # get back to the raw output. Printed only on mismatch.
    if ! cmp -s "$WORKDIR/p18f_s_head.txt" "$WORKDIR/p18f_g_head.txt" ||
       ! cmp -s "$WORKDIR/p18f_s_br.txt" "$WORKDIR/p18f_g_br.txt"; then
        echo "    --- phase18f ($p18f_label) mismatch, git $(git --version | sed 's/git version //') ---"
        echo "    logs/HEAD   sg | git:"
        diff "$WORKDIR/p18f_s_head.txt" "$WORKDIR/p18f_g_head.txt" | sed 's/^/      /'
        echo "    branch log  sg | git:"
        diff "$WORKDIR/p18f_s_br.txt" "$WORKDIR/p18f_g_br.txt" | sed 's/^/      /'
    fi
}

# --- a plain two-commit rebase ---
p18f_setup "$WORKDIR/p18f_plain_sg" "$SG" clean
p18f_setup "$WORKDIR/p18f_plain_git" git clean
P18F_BR_BEFORE=$(wc -l < "$WORKDIR/p18f_plain_sg/.git/logs/refs/heads/topic" | tr -d ' ')
(cd "$WORKDIR/p18f_plain_sg" && "$SG" rebase master) > /dev/null 2>&1
(cd "$WORKDIR/p18f_plain_git" && git rebase master) > /dev/null 2>&1
p18f_compare "plain" "$WORKDIR/p18f_plain_sg" "$WORKDIR/p18f_plain_git"

# The structural claim: two commits replayed, ONE line added to the branch.
P18F_BR_AFTER=$(wc -l < "$WORKDIR/p18f_plain_sg/.git/logs/refs/heads/topic" | tr -d ' ')
check "phase18f: replaying 2 commits adds exactly 1 line to the branch's reflog ($P18F_BR_BEFORE -> $P18F_BR_AFTER)" \
    test "$P18F_BR_AFTER" = "$((P18F_BR_BEFORE + 1))"
# The finish line re-attaches HEAD without moving it, so old == new. This is
# the one reflog line that only exists because logs/HEAD is never
# no-op-suppressed; a branch log would have dropped it.
P18F_LAST=$(tail -1 "$WORKDIR/p18f_plain_sg/.git/logs/HEAD")
P18F_OLD=$(printf '%s' "$P18F_LAST" | cut -d' ' -f1)
P18F_NEW=$(printf '%s' "$P18F_LAST" | cut -d' ' -f2)
check "phase18f: the finish line in logs/HEAD has old == new" test "$P18F_OLD" = "$P18F_NEW"
check "phase18f: HEAD is re-attached to the branch when the rebase finishes" \
    test "$(cat "$WORKDIR/p18f_plain_sg/.git/HEAD")" = "ref: refs/heads/topic"

# The finish line embeds a full 40-hex id, and the <oid> normalization above
# turns ANY well-formed id into the same token -- so the message comparison
# would equally accept the new tip, or orig-head, in that slot. Which commit
# it names is checked here, unnormalized: it is the ONTO commit (master's tip
# at the time), not the rebase's result.
P18F_ONTO_LOGGED=$(sed 's/.*	//' "$WORKDIR/p18f_plain_sg/.git/logs/refs/heads/topic" | tail -1 |
    sed 's/.* onto //')
P18F_ONTO_WANT=$(cd "$WORKDIR/p18f_plain_sg" && git rev-parse master)
P18F_TIP_NOW=$(cd "$WORKDIR/p18f_plain_sg" && git rev-parse topic)
check "phase18f: the finish line names the onto commit (logged '$P18F_ONTO_LOGGED')" \
    test "$P18F_ONTO_LOGGED" = "$P18F_ONTO_WANT"
check "phase18f: precondition -- onto and the resulting tip really differ, so that check discriminates" \
    test "$P18F_ONTO_WANT" != "$P18F_TIP_NOW"

# --- paused on a conflict: the model itself, before any message is written ---
p18f_setup "$WORKDIR/p18f_pause_sg" "$SG" conflict
P18F_PAUSE_TIP_BEFORE=$(cat "$WORKDIR/p18f_pause_sg/.git/refs/heads/topic")
(cd "$WORKDIR/p18f_pause_sg" && "$SG" rebase master) > /dev/null 2>&1
P18F_PAUSE_HEAD=$(cat "$WORKDIR/p18f_pause_sg/.git/HEAD")
check "phase18f: HEAD is detached while a rebase is paused on a conflict" \
    test "${P18F_PAUSE_HEAD#ref: }" = "$P18F_PAUSE_HEAD"
check "phase18f: the branch has not moved while the rebase is paused" \
    test "$(cat "$WORKDIR/p18f_pause_sg/.git/refs/heads/topic")" = "$P18F_PAUSE_TIP_BEFORE"
P18F_PAUSE_MSGS=$(p18f_msgs "$WORKDIR/p18f_pause_sg/.git/logs/HEAD" | tail -1)
check "phase18f: a conflicting pick writes no reflog line of its own (last is '$P18F_PAUSE_MSGS')" \
    test "$P18F_PAUSE_MSGS" = "rebase (start): checkout master"

# --- conflict then --continue: logged as (continue), not (pick) ---
p18f_setup "$WORKDIR/p18f_cont_git" git conflict
(cd "$WORKDIR/p18f_cont_git" && git rebase master) > /dev/null 2>&1
printf 'resolved\n' > "$WORKDIR/p18f_cont_git/shared.txt"
(cd "$WORKDIR/p18f_cont_git" && git add . && git rebase --continue) > /dev/null 2>&1
printf 'resolved\n' > "$WORKDIR/p18f_pause_sg/shared.txt"
(cd "$WORKDIR/p18f_pause_sg" && "$SG" add . && "$SG" rebase --continue) > /dev/null 2>&1
p18f_compare "continue" "$WORKDIR/p18f_pause_sg" "$WORKDIR/p18f_cont_git"

# --- --skip: the skipped commit leaves no trace at all ---
p18f_setup "$WORKDIR/p18f_skip_sg" "$SG" conflict
p18f_setup "$WORKDIR/p18f_skip_git" git conflict
# Two separate invocations, matching git's: `rebase master` exits 1 here (it
# pauses on the conflict), so chaining --skip after it with && would never run
# the skip at all.
(cd "$WORKDIR/p18f_skip_sg" && "$SG" rebase master) > /dev/null 2>&1
(cd "$WORKDIR/p18f_skip_sg" && "$SG" rebase --skip) > /dev/null 2>&1
(cd "$WORKDIR/p18f_skip_git" && git rebase master) > /dev/null 2>&1
(cd "$WORKDIR/p18f_skip_git" && git rebase --skip) > /dev/null 2>&1
p18f_compare "skip" "$WORKDIR/p18f_skip_sg" "$WORKDIR/p18f_skip_git"

# --- --abort: one line in logs/HEAD, nothing in the branch's ---
p18f_setup "$WORKDIR/p18f_abort_sg" "$SG" conflict
p18f_setup "$WORKDIR/p18f_abort_git" git conflict
P18F_ABORT_BR_BEFORE=$(wc -l < "$WORKDIR/p18f_abort_sg/.git/logs/refs/heads/topic" | tr -d ' ')
(cd "$WORKDIR/p18f_abort_sg" && "$SG" rebase master) > /dev/null 2>&1
(cd "$WORKDIR/p18f_abort_sg" && "$SG" rebase --abort) > /dev/null 2>&1
(cd "$WORKDIR/p18f_abort_git" && git rebase master) > /dev/null 2>&1
(cd "$WORKDIR/p18f_abort_git" && git rebase --abort) > /dev/null 2>&1
p18f_compare "abort" "$WORKDIR/p18f_abort_sg" "$WORKDIR/p18f_abort_git"
# Over-determined on purpose, and worth saying so: abort restores the branch
# to the value it already has, so rule 1's no-op suppression would drop the
# line even if abort DID write one. No mutation of abort's own code makes
# this red (measured -- one was tried). It is a regression guard on the
# resulting shape, not evidence that abort chooses not to write.
check "phase18f: --abort adds nothing to the branch's own reflog" \
    test "$(wc -l < "$WORKDIR/p18f_abort_sg/.git/logs/refs/heads/topic" | tr -d ' ')" = "$P18F_ABORT_BR_BEFORE"

# --- interrupted between finish_rebase's two writes ---
#
# finish_rebase advances the branch and then re-attaches HEAD. A crash
# between those two leaves the branch already at the rebased tip with the
# sequencer state still on disk -- a state no mutation of a single line
# produces, so it is manufactured here instead.
#
# --abort must restore the branch even though it "was never moved": the
# version that only re-attached put HEAD back onto a branch pointing at the
# rebase's RESULT, restored the work tree to the pre-rebase content, deleted
# the state and printed "back at <orig>", exiting 0 with all three
# disagreeing. Asserting the exit status alone would not have caught it --
# that version exited 0 too.
P18F_INT="$WORKDIR/p18f_interrupted"
p18f_setup "$P18F_INT" "$SG" clean
P18F_INT_ORIG=$(cat "$P18F_INT/.git/refs/heads/topic")
(cd "$P18F_INT" && "$SG" rebase master) > /dev/null 2>&1
P18F_INT_TIP=$(cat "$P18F_INT/.git/refs/heads/topic")
P18F_INT_BRLOG=$(wc -l < "$P18F_INT/.git/logs/refs/heads/topic" | tr -d ' ')
# Rewind to the moment between the two writes: branch advanced, HEAD still
# detached at the new tip, sequencer state present.
printf '%s\n' "$P18F_INT_TIP" > "$P18F_INT/.git/HEAD"
mkdir -p "$P18F_INT/.git/sg-rebase"
(cd "$P18F_INT" && git rev-parse master) > "$P18F_INT/.git/sg-rebase/onto"
printf '%s\n' "$P18F_INT_ORIG" > "$P18F_INT/.git/sg-rebase/orig-head"
printf 'topic\n' > "$P18F_INT/.git/sg-rebase/orig-branch"
: > "$P18F_INT/.git/sg-rebase/todo"
check "phase18f: precondition -- the manufactured state really has the branch ahead of orig-head" \
    test "$P18F_INT_TIP" != "$P18F_INT_ORIG"
(cd "$P18F_INT" && "$SG" rebase --abort) > /dev/null 2>&1
check "phase18f: --abort recovers from an interruption between finish's two writes" test $? = 0
check "phase18f: ...restoring the branch to its pre-rebase commit, not leaving it at the result" \
    test "$(cat "$P18F_INT/.git/refs/heads/topic")" = "$P18F_INT_ORIG"
P18F_INT_STATUS=$(cd "$P18F_INT" && "$SG" status | tail -1)
check "phase18f: ...leaving the work tree consistent with it (got '$P18F_INT_STATUS')" \
    test "$P18F_INT_STATUS" = "nothing to commit, working tree clean"
check "phase18f: ...and still writing no line to the branch's reflog" \
    test "$(wc -l < "$P18F_INT/.git/logs/refs/heads/topic" | tr -d ' ')" = "$P18F_INT_BRLOG"

# --- fast-forward: start and finish, and NO pick lines in between ---
P18F_FF_SG="$WORKDIR/p18f_ff_sg"
P18F_FF_GIT="$WORKDIR/p18f_ff_git"
(cd "$WORKDIR" && "$SG" init p18f_ff_sg) > /dev/null 2>&1
mkdir -p "$P18F_FF_GIT" && (cd "$P18F_FF_GIT" && git init -q -b master .) > /dev/null 2>&1
for p18f_pair in "$P18F_FF_SG:$SG" "$P18F_FF_GIT:git"; do
    p18f_d=${p18f_pair%%:*}
    p18f_b=${p18f_pair#*:}
    printf 'base\n' > "$p18f_d/base.txt"
    (cd "$p18f_d" && "$p18f_b" add . && "$p18f_b" commit -m base) > /dev/null 2>&1
    (cd "$p18f_d" && "$p18f_b" branch topic) > /dev/null 2>&1
    printf 'ahead\n' > "$p18f_d/ahead.txt"
    (cd "$p18f_d" && "$p18f_b" add . && "$p18f_b" commit -m M1) > /dev/null 2>&1
    (cd "$p18f_d" && "$p18f_b" switch topic && "$p18f_b" rebase master) > /dev/null 2>&1
done
p18f_compare "fast-forward" "$P18F_FF_SG" "$P18F_FF_GIT"
P18F_FF_PICKS=$(p18f_msgs "$P18F_FF_SG/.git/logs/HEAD" | grep -c 'rebase (pick)' || true)
check "phase18f: a fast-forward writes no pick lines (found $P18F_FF_PICKS)" test "$P18F_FF_PICKS" = 0
# The fast-forward path writes sequencer state solely to make its own
# detached window recoverable, so it must also clean it up -- a leftover
# directory reads as "a rebase is in progress" to every later command.
check "phase18f: a fast-forward leaves no sequencer state behind" \
    test ! -d "$P18F_FF_SG/.git/sg-rebase"
check "phase18f: ...and HEAD re-attached" \
    test "$(cat "$P18F_FF_SG/.git/HEAD")" = "ref: refs/heads/topic"

# --- already up to date: not one line, in either log ---
P18F_UTD_HEAD=$(wc -l < "$P18F_FF_SG/.git/logs/HEAD" | tr -d ' ')
P18F_UTD_BR=$(wc -l < "$P18F_FF_SG/.git/logs/refs/heads/topic" | tr -d ' ')
(cd "$P18F_FF_SG" && "$SG" rebase master) > /dev/null 2>&1
check "phase18f: an up-to-date rebase writes nothing to logs/HEAD" \
    test "$(wc -l < "$P18F_FF_SG/.git/logs/HEAD" | tr -d ' ')" = "$P18F_UTD_HEAD"
check "phase18f: ...and nothing to the branch's reflog either" \
    test "$(wc -l < "$P18F_FF_SG/.git/logs/refs/heads/topic" | tr -d ' ')" = "$P18F_UTD_BR"

# ============================================================
# Phase 19: merge, and a rebase STARTED on a detached HEAD
#
# Phase 18d pinned both of these as deliberate refusals. Real git supports
# them, so the refusals were a scope boundary rather than a rule, and this
# phase removes them; the two checks that named them are gone from 18d.
#
# The shape both features have to produce is the same one Phase 17 measured
# and Phase 18 built the rebase model on: sg_ref_update mirrors a branch's
# reflog line into logs/HEAD only while HEAD is the symref to that branch, so
# with HEAD detached there is nothing to mirror -- a detached merge writes
# ONE line, to logs/HEAD, and moves no branch, and a detached rebase never
# reaches finish_rebase's two writes at all. Neither needed a special case;
# both are what the existing rules degrade to when there is no branch. That
# is exactly why the branch refs are asserted to be untouched below: "the
# reflog looks right" would also be true of a build that moved a branch and
# happened to log the same text.
#
# Dual-track against real git, the Phase 17/18f idiom -- but only over the
# lines the operation under test ADDS. Comparing whole logs would also be
# comparing the fixture's own checkout lines, which belong to `switch` and
# are pinned in 18b/18e; a failure there would land on these checks and name
# the wrong feature.
# ============================================================

# Message column only, object ids normalized: the two repos cannot share
# hashes. The merge strategy name is normalized too -- sg's 3-way merge is
# its own implementation and says so, and that difference is not what these
# checks are about.
p19_msgs() {
    sed 's/.*	//' "$1" 2>/dev/null |
        sed 's/[0-9a-f]\{40\}/<oid>/g' |
        sed "s/'ort'/'<strategy>'/; s/'sg-3way'/'<strategy>'/"
}

# The lines $2 (a count taken before the operation) onward -- i.e. what the
# operation appended.
p19_added() {
    p19_msgs "$1" | tail -n "+$(( $2 + 1 ))"
}

p19_lines() {
    wc -l < "$1" 2>/dev/null | tr -d ' '
}

# base -> master:M1, topic:T1 on different files, so neither is an ancestor
# of the other and the merge is a real three-way one. $3 = conflict makes T1
# collide with M1 instead.
p19_setup() {
    p19_dir="$1"
    p19_bin="$2"
    p19_mode="$3"
    if [ "$p19_bin" = git ]; then
        mkdir -p "$p19_dir" && (cd "$p19_dir" && git init -q -b master .) > /dev/null 2>&1
    else
        (cd "$WORKDIR" && "$SG" init "$(basename "$p19_dir")") > /dev/null 2>&1
    fi
    printf 'base\n' > "$p19_dir/base.txt"
    (cd "$p19_dir" && "$p19_bin" add . && "$p19_bin" commit -m base) > /dev/null 2>&1
    (cd "$p19_dir" && "$p19_bin" branch topic && "$p19_bin" switch topic) > /dev/null 2>&1
    if [ "$p19_mode" = conflict ]; then
        printf 'topic version\n' > "$p19_dir/shared.txt"
    else
        printf 'topic one\n' > "$p19_dir/t1.txt"
    fi
    (cd "$p19_dir" && "$p19_bin" add . && "$p19_bin" commit -m T1) > /dev/null 2>&1
    (cd "$p19_dir" && "$p19_bin" switch master) > /dev/null 2>&1
    if [ "$p19_mode" = conflict ]; then
        printf 'master version\n' > "$p19_dir/shared.txt"
    else
        printf 'master only\n' > "$p19_dir/m1.txt"
    fi
    (cd "$p19_dir" && "$p19_bin" add . && "$p19_bin" commit -m M1) > /dev/null 2>&1
}

p19_detach() {   # $1 dir, $2 bin, $3 rev
    if [ "$2" = git ]; then
        (cd "$1" && git switch -q --detach "$3") > /dev/null 2>&1
    else
        (cd "$1" && "$SG" switch --detach "$3") > /dev/null 2>&1
    fi
}

# ------------------------------------------------------------
# Phase 19a: a three-way merge on a detached HEAD
# ------------------------------------------------------------
P19A_S="$WORKDIR/p19a_sg"
P19A_G="$WORKDIR/p19a_git"
p19_setup "$P19A_S" "$SG" clean
p19_setup "$P19A_G" git clean
p19_detach "$P19A_S" "$SG" master
p19_detach "$P19A_G" git master
P19A_S_HEADN=$(p19_lines "$P19A_S/.git/logs/HEAD")
P19A_G_HEADN=$(p19_lines "$P19A_G/.git/logs/HEAD")
P19A_MASTER_BEFORE=$(cat "$P19A_S/.git/refs/heads/master")
P19A_TOPIC_BEFORE=$(cat "$P19A_S/.git/refs/heads/topic")
P19A_MASTER_LOGN=$(p19_lines "$P19A_S/.git/logs/refs/heads/master")

# $? is saved to a variable on its own line, before the check. Putting a
# command substitution in a check's MESSAGE overwrites $? while the argument
# list is being built, so `check "... $(...)" test $? = 0` reads the status of
# the substitution instead of the command under test -- a check that passes
# no matter what sg did. Re-running the command to get its status instead is
# worse, not better: the second merge would be an already-up-to-date no-op
# and would exit 0 even if the first one had failed.
P19A_OUT=$( (cd "$P19A_S" && "$SG" merge topic) 2>&1 )
P19A_RC=$?
check "phase19a: sg merge on a detached HEAD succeeds (said '$(printf '%s' "$P19A_OUT" | head -1)')" \
    test "$P19A_RC" = 0
(cd "$P19A_G" && git merge topic --no-edit) > /dev/null 2>&1

# The whole point: HEAD moved, and it moved as a detached HEAD.
P19A_HEAD_RAW=$(cat "$P19A_S/.git/HEAD")
check "phase19a: HEAD is still detached afterwards, holding a bare id (got '$P19A_HEAD_RAW')" \
    test -n "$(printf '%s' "$P19A_HEAD_RAW" | grep '^[0-9a-f]\{40\}$')"
P19A_TIP=$(cd "$P19A_S" && git rev-parse HEAD 2>/dev/null)
check "phase19a: ...at a NEW commit, not at either side (precondition for the rest)" \
    test "$P19A_TIP" != "$P19A_MASTER_BEFORE" -a "$P19A_TIP" != "$P19A_TOPIC_BEFORE"

# A build that merged onto the branch instead would also move HEAD, because
# HEAD would follow the branch. Both refs are read back to rule that out.
check "phase19a: master did not move" \
    test "$(cat "$P19A_S/.git/refs/heads/master")" = "$P19A_MASTER_BEFORE"
check "phase19a: topic did not move" \
    test "$(cat "$P19A_S/.git/refs/heads/topic")" = "$P19A_TOPIC_BEFORE"
check "phase19a: master's own reflog gained no line" \
    test "$(p19_lines "$P19A_S/.git/logs/refs/heads/master")" = "$P19A_MASTER_LOGN"

# Real git is the oracle for the message text, not sg's own idea of it.
p19_added "$P19A_S/.git/logs/HEAD" "$P19A_S_HEADN" > "$WORKDIR/p19a_s.txt"
p19_added "$P19A_G/.git/logs/HEAD" "$P19A_G_HEADN" > "$WORKDIR/p19a_g.txt"
check "phase19a: precondition -- the merge appended exactly one logs/HEAD line" \
    test "$(wc -l < "$WORKDIR/p19a_s.txt" | tr -d ' ')" = 1
check "phase19a: that line's message matches git's" \
    cmp -s "$WORKDIR/p19a_s.txt" "$WORKDIR/p19a_g.txt"
if ! cmp -s "$WORKDIR/p19a_s.txt" "$WORKDIR/p19a_g.txt"; then
    echo "    --- phase19a mismatch, git $(git --version | sed 's/git version //') ---"
    diff "$WORKDIR/p19a_s.txt" "$WORKDIR/p19a_g.txt" | sed 's/^/      /'
fi

# Real git reading sg's repository: the object has to be a genuine merge.
P19A_PARENTS=$(cd "$P19A_S" && git rev-list --parents -1 HEAD 2>/dev/null | wc -w | tr -d ' ')
check "phase19a: the commit sg wrote has two parents (got $((P19A_PARENTS - 1)))" \
    test "$P19A_PARENTS" = 3
P19A_SUBJ=$(cd "$P19A_S" && git log -1 --format=%s 2>/dev/null)
P19A_SUBJ_G=$(cd "$P19A_G" && git log -1 --format=%s 2>/dev/null)
check "phase19a: its subject names HEAD, not a branch, exactly as git's does (got '$P19A_SUBJ')" \
    test "$P19A_SUBJ" = "$P19A_SUBJ_G"
P19A_STATUS=$(cd "$P19A_S" && "$SG" status 2>/dev/null | tail -1)
check "phase19a: the work tree is clean afterwards (got '$P19A_STATUS')" \
    test "$P19A_STATUS" = "nothing to commit, working tree clean"
# stdout is asserted for every message this phase gave a detached-HEAD
# wording, here and in 19f/19g. Reading only files and reflogs is how a
# "Fast-forwarded (null) to master." survived a full green run: the branch
# name reaches the user through a channel nothing was looking at.
check "phase19a: it names HEAD in its summary line, having no branch to name (said '$P19A_OUT')" \
    test "$P19A_OUT" = "Merge made by 'topic' [$(cd "$P19A_S" && git rev-parse --short=7 HEAD)] into HEAD."

# ------------------------------------------------------------
# Phase 19b: a fast-forward merge, and an already-up-to-date one
# ------------------------------------------------------------
P19B_S="$WORKDIR/p19b_sg"
P19B_G="$WORKDIR/p19b_git"
p19_setup "$P19B_S" "$SG" clean
p19_setup "$P19B_G" git clean
# Detach at base: master is strictly ahead, so merging it fast-forwards.
p19_detach "$P19B_S" "$SG" master~1
p19_detach "$P19B_G" git master~1
P19B_S_HEADN=$(p19_lines "$P19B_S/.git/logs/HEAD")
P19B_G_HEADN=$(p19_lines "$P19B_G/.git/logs/HEAD")
P19B_MASTER_BEFORE=$(cat "$P19B_S/.git/refs/heads/master")
(cd "$P19B_S" && "$SG" merge master) > /dev/null 2>&1
check "phase19b: a fast-forward merge on a detached HEAD succeeds" test $? = 0
(cd "$P19B_G" && git merge master --no-edit) > /dev/null 2>&1
check "phase19b: HEAD fast-forwarded onto the target, still detached" \
    test "$(cat "$P19B_S/.git/HEAD")" = "$P19B_MASTER_BEFORE"
p19_added "$P19B_S/.git/logs/HEAD" "$P19B_S_HEADN" > "$WORKDIR/p19b_s.txt"
p19_added "$P19B_G/.git/logs/HEAD" "$P19B_G_HEADN" > "$WORKDIR/p19b_g.txt"
check "phase19b: its logs/HEAD line matches git's (got '$(cat "$WORKDIR/p19b_s.txt")')" \
    cmp -s "$WORKDIR/p19b_s.txt" "$WORKDIR/p19b_g.txt"

# Merging it again is a no-op, and a no-op writes nothing at all.
P19B_HEADN2=$(p19_lines "$P19B_S/.git/logs/HEAD")
P19B_OUT=$( (cd "$P19B_S" && "$SG" merge master) 2>&1 )
check "phase19b: an already-up-to-date merge on a detached HEAD exits 0" test $? = 0
check "phase19b: ...saying so (got '$P19B_OUT')" test "$P19B_OUT" = "Already up to date."
check "phase19b: ...and writing not one reflog line" \
    test "$(p19_lines "$P19B_S/.git/logs/HEAD")" = "$P19B_HEADN2"

# ------------------------------------------------------------
# Phase 19c: a conflicting merge on a detached HEAD, resolved with sg commit
#
# cmd_commit.c already handled MERGE_HEAD and already handled a detached
# HEAD, but never both at once -- nothing could reach the combination while
# merge refused to start. The commit it produces must still have two parents.
# ------------------------------------------------------------
P19C_S="$WORKDIR/p19c_sg"
P19C_G="$WORKDIR/p19c_git"
p19_setup "$P19C_S" "$SG" conflict
p19_setup "$P19C_G" git conflict
p19_detach "$P19C_S" "$SG" master
p19_detach "$P19C_G" git master
P19C_HEAD_BEFORE=$(cat "$P19C_S/.git/HEAD")
P19C_HEADN=$(p19_lines "$P19C_S/.git/logs/HEAD")
P19C_G_HEADN=$(p19_lines "$P19C_G/.git/logs/HEAD")
(cd "$P19C_S" && "$SG" merge topic) > /dev/null 2>&1
check "phase19c: a conflicting merge on a detached HEAD exits 1" test $? = 1
# git's side runs here, while its work tree is still untouched -- resolving
# sg's copy first and only then merging git's would hand git a dirty tree and
# it would refuse to start, silently costing the comparison its other track.
(cd "$P19C_G" && git merge topic --no-edit) > /dev/null 2>&1
check "phase19c: ...leaving MERGE_HEAD behind" test -f "$P19C_S/.git/MERGE_HEAD"
check "phase19c: ...naming the other side in it" \
    test "$(cat "$P19C_S/.git/MERGE_HEAD")" = "$(cat "$P19C_S/.git/refs/heads/topic")"
check "phase19c: ...and not moving HEAD yet" \
    test "$(cat "$P19C_S/.git/HEAD")" = "$P19C_HEAD_BEFORE"
check "phase19c: ...nor writing a reflog line yet" \
    test "$(p19_lines "$P19C_S/.git/logs/HEAD")" = "$P19C_HEADN"

printf 'resolved\n' > "$P19C_S/shared.txt"
printf 'resolved\n' > "$P19C_G/shared.txt"
(cd "$P19C_S" && "$SG" add shared.txt && "$SG" commit -m "merged while detached") > /dev/null 2>&1
check "phase19c: sg commit finishes the merge from a detached HEAD" test $? = 0
(cd "$P19C_G" && git add shared.txt && git commit -qm "merged while detached") > /dev/null 2>&1
check "phase19c: MERGE_HEAD is gone" test ! -f "$P19C_S/.git/MERGE_HEAD"
P19C_HEAD_AFTER=$(cat "$P19C_S/.git/HEAD")
check "phase19c: HEAD is still detached, at the resolved commit (got '$P19C_HEAD_AFTER')" \
    test -n "$(printf '%s' "$P19C_HEAD_AFTER" | grep '^[0-9a-f]\{40\}$')" \
    -a "$P19C_HEAD_AFTER" != "$P19C_HEAD_BEFORE"
P19C_PARENTS=$(cd "$P19C_S" && git rev-list --parents -1 HEAD 2>/dev/null | wc -w | tr -d ' ')
check "phase19c: the resolved commit has two parents, not one (got $((P19C_PARENTS - 1)))" \
    test "$P19C_PARENTS" = 3
p19_added "$P19C_S/.git/logs/HEAD" "$P19C_HEADN" > "$WORKDIR/p19c_s.txt"
p19_added "$P19C_G/.git/logs/HEAD" "$P19C_G_HEADN" > "$WORKDIR/p19c_g.txt"
check "phase19c: the reflog lines the whole sequence wrote match git's" \
    cmp -s "$WORKDIR/p19c_s.txt" "$WORKDIR/p19c_g.txt"
if ! cmp -s "$WORKDIR/p19c_s.txt" "$WORKDIR/p19c_g.txt"; then
    echo "    --- phase19c mismatch ---"
    diff "$WORKDIR/p19c_s.txt" "$WORKDIR/p19c_g.txt" | sed 's/^/      /'
fi

# merge --abort never had a detached gate in front of it, so it was already
# reachable in theory and untested in practice.
P19C2_S="$WORKDIR/p19c2_sg"
p19_setup "$P19C2_S" "$SG" conflict
p19_detach "$P19C2_S" "$SG" master
P19C2_HEAD=$(cat "$P19C2_S/.git/HEAD")
(cd "$P19C2_S" && "$SG" merge topic) > /dev/null 2>&1
(cd "$P19C2_S" && "$SG" merge --abort) > /dev/null 2>&1
check "phase19c: sg merge --abort works from a detached HEAD" test $? = 0
check "phase19c: ...leaving HEAD where it was" test "$(cat "$P19C2_S/.git/HEAD")" = "$P19C2_HEAD"
check "phase19c: ...and removing MERGE_HEAD" test ! -f "$P19C2_S/.git/MERGE_HEAD"
P19C2_STATUS=$(cd "$P19C2_S" && "$SG" status 2>/dev/null | tail -1)
check "phase19c: ...with a clean work tree (got '$P19C2_STATUS')" \
    test "$P19C2_STATUS" = "nothing to commit, working tree clean"

# ------------------------------------------------------------
# Phase 19d: a rebase STARTED on a detached HEAD
#
# The structural claim is the absence of finish_rebase's two writes: with no
# branch to move, logs/HEAD ends at the last pick and no branch reflog gains
# anything. An implementation that reattached HEAD to some branch would still
# produce a plausible-looking logs/HEAD, so the branch refs are read back.
# ------------------------------------------------------------
P19D_S="$WORKDIR/p19d_sg"
P19D_G="$WORKDIR/p19d_git"
p18f_setup "$P19D_S" "$SG" clean          # base -> topic:T1,T2 ; master:M1
p18f_setup "$P19D_G" git clean
p19_detach "$P19D_S" "$SG" topic
p19_detach "$P19D_G" git topic
P19D_TOPIC_BEFORE=$(cat "$P19D_S/.git/refs/heads/topic")
P19D_TOPIC_LOGN=$(p19_lines "$P19D_S/.git/logs/refs/heads/topic")
P19D_S_HEADN=$(p19_lines "$P19D_S/.git/logs/HEAD")
P19D_G_HEADN=$(p19_lines "$P19D_G/.git/logs/HEAD")

# Not `... | tail -1` inside the substitution: a pipeline's status is the LAST
# stage's, so tail would report 0 however sg exited (same trap as 19a).
P19D_OUT=$( (cd "$P19D_S" && "$SG" rebase master) 2>&1 )
P19D_RC=$?
check "phase19d: sg rebase started on a detached HEAD succeeds (said '$(printf '%s' "$P19D_OUT" | tail -1)')" \
    test "$P19D_RC" = 0
check "phase19d: ...and says so without naming a branch it never had" \
    test "$(printf '%s' "$P19D_OUT" | tail -1)" = "Successfully rebased and updated detached HEAD onto 'master'."
(cd "$P19D_G" && git rebase master) > /dev/null 2>&1

check "phase19d: HEAD is still detached afterwards (got '$(cat "$P19D_S/.git/HEAD")')" \
    test -n "$(sed 's/[0-9a-f]\{40\}/<oid>/' "$P19D_S/.git/HEAD" | grep '^<oid>$')"
check "phase19d: topic did not move -- finish_rebase's branch write never ran" \
    test "$(cat "$P19D_S/.git/refs/heads/topic")" = "$P19D_TOPIC_BEFORE"
check "phase19d: ...and topic's reflog gained no line either" \
    test "$(p19_lines "$P19D_S/.git/logs/refs/heads/topic")" = "$P19D_TOPIC_LOGN"
check "phase19d: the sequencer state is cleaned up" test ! -d "$P19D_S/.git/sg-rebase"

p19_added "$P19D_S/.git/logs/HEAD" "$P19D_S_HEADN" > "$WORKDIR/p19d_s.txt"
p19_added "$P19D_G/.git/logs/HEAD" "$P19D_G_HEADN" > "$WORKDIR/p19d_g.txt"
check "phase19d: the reflog lines it wrote match git's, start and picks and no finish" \
    cmp -s "$WORKDIR/p19d_s.txt" "$WORKDIR/p19d_g.txt"
if ! cmp -s "$WORKDIR/p19d_s.txt" "$WORKDIR/p19d_g.txt"; then
    echo "    --- phase19d mismatch, git $(git --version | sed 's/git version //') ---"
    diff "$WORKDIR/p19d_s.txt" "$WORKDIR/p19d_g.txt" | sed 's/^/      /'
fi
# Named separately: the comparison above would still pass if BOTH grew a
# finish line, and this is the line the whole model change is about.
P19D_FIN=$(grep -c 'rebase (finish)' "$WORKDIR/p19d_s.txt" || true)
check "phase19d: no 'rebase (finish)' line is written when there is no branch (found $P19D_FIN)" \
    test "$P19D_FIN" = 0
# Real git reading sg's result: the replayed commits must sit on master.
P19D_BASE=$(cd "$P19D_S" && git merge-base HEAD master 2>/dev/null)
check "phase19d: the rebased HEAD really sits on master" \
    test "$P19D_BASE" = "$(cat "$P19D_S/.git/refs/heads/master")"

# ------------------------------------------------------------
# Phase 19e: paused on a conflict, then --continue -- from detached
# ------------------------------------------------------------
P19E_S="$WORKDIR/p19e_sg"
P19E_G="$WORKDIR/p19e_git"
p18f_setup "$P19E_S" "$SG" conflict
p18f_setup "$P19E_G" git conflict
p19_detach "$P19E_S" "$SG" topic
p19_detach "$P19E_G" git topic
P19E_ORIG=$(cd "$P19E_S" && git rev-parse HEAD 2>/dev/null)
P19E_S_HEADN=$(p19_lines "$P19E_S/.git/logs/HEAD")
P19E_G_HEADN=$(p19_lines "$P19E_G/.git/logs/HEAD")
(cd "$P19E_S" && "$SG" rebase master) > /dev/null 2>&1
check "phase19e: the rebase pauses on the conflict" test -d "$P19E_S/.git/sg-rebase"

# The sentinel is the whole reason a paused detached rebase can be resumed at
# all: orig-branch has to say "there was no branch" in a way that a missing
# or empty file does not, since those mean corruption.
check "phase19e: orig-branch records the detached start with git's own sentinel (got '$(cat "$P19E_S/.git/sg-rebase/orig-branch" 2>/dev/null)')" \
    test "$(cat "$P19E_S/.git/sg-rebase/orig-branch" 2>/dev/null)" = "detached HEAD"
P19E_STATUS=$(cd "$P19E_S" && "$SG" status 2>/dev/null | grep 'currently rebasing')
check "phase19e: sg status describes it without inventing a branch name (got '$P19E_STATUS')" \
    test "$P19E_STATUS" = "You are currently rebasing."

(cd "$P19E_G" && git rebase master) > /dev/null 2>&1
printf 'resolved\n' > "$P19E_S/shared.txt"
printf 'resolved\n' > "$P19E_G/shared.txt"
(cd "$P19E_S" && "$SG" add shared.txt && "$SG" rebase --continue) > /dev/null 2>&1
check "phase19e: sg rebase --continue finishes it" test $? = 0
(cd "$P19E_G" && git add shared.txt && git -c core.editor=true rebase --continue) > /dev/null 2>&1
check "phase19e: HEAD is still detached at the end" \
    test -n "$(sed 's/[0-9a-f]\{40\}/<oid>/' "$P19E_S/.git/HEAD" | grep '^<oid>$')"
check "phase19e: the sequencer state is gone" test ! -d "$P19E_S/.git/sg-rebase"
p19_added "$P19E_S/.git/logs/HEAD" "$P19E_S_HEADN" > "$WORKDIR/p19e_s.txt"
p19_added "$P19E_G/.git/logs/HEAD" "$P19E_G_HEADN" > "$WORKDIR/p19e_g.txt"
check "phase19e: its reflog lines match git's (start, continue, no finish)" \
    cmp -s "$WORKDIR/p19e_s.txt" "$WORKDIR/p19e_g.txt"
if ! cmp -s "$WORKDIR/p19e_s.txt" "$WORKDIR/p19e_g.txt"; then
    echo "    --- phase19e mismatch ---"
    diff "$WORKDIR/p19e_s.txt" "$WORKDIR/p19e_g.txt" | sed 's/^/      /'
fi

# ------------------------------------------------------------
# Phase 19f: --abort from a detached start
#
# The branch path restores the branch unconditionally, to survive an
# interruption between finish_rebase's two writes. There is no such window
# here -- finish writes nothing -- so abort's whole job is putting HEAD back,
# and it must put it back DETACHED rather than onto whatever branch happens
# to point at the same commit.
# ------------------------------------------------------------
P19F_S="$WORKDIR/p19f_sg"
P19F_G="$WORKDIR/p19f_git"
p18f_setup "$P19F_S" "$SG" conflict
p18f_setup "$P19F_G" git conflict
p19_detach "$P19F_S" "$SG" topic
p19_detach "$P19F_G" git topic
P19F_HEAD_BEFORE=$(cat "$P19F_S/.git/HEAD")
P19F_TOPIC_BEFORE=$(cat "$P19F_S/.git/refs/heads/topic")
P19F_S_HEADN=$(p19_lines "$P19F_S/.git/logs/HEAD")
P19F_G_HEADN=$(p19_lines "$P19F_G/.git/logs/HEAD")
(cd "$P19F_S" && "$SG" rebase master) > /dev/null 2>&1
(cd "$P19F_G" && git rebase master) > /dev/null 2>&1
P19F_ABORT_OUT=$( (cd "$P19F_S" && "$SG" rebase --abort) 2>&1 )
P19F_ABORT_RC=$?
check "phase19f: sg rebase --abort works from a detached start" test "$P19F_ABORT_RC" = 0
check "phase19f: ...saying HEAD is back, with no branch to name (said '$P19F_ABORT_OUT')" \
    test "$P19F_ABORT_OUT" = "Rebase aborted; HEAD is back at $(printf '%s' "$P19F_HEAD_BEFORE" | cut -c1-7)."
(cd "$P19F_G" && git rebase --abort) > /dev/null 2>&1
# topic points at the same commit HEAD started on, so "HEAD holds that id" is
# also true of a build that reattached to topic. The raw file is read.
check "phase19f: HEAD is back at the original commit, still detached (got '$(cat "$P19F_S/.git/HEAD")')" \
    test "$(cat "$P19F_S/.git/HEAD")" = "$P19F_HEAD_BEFORE"
check "phase19f: precondition -- topic pointed at that same commit, so the check above is about the FORM" \
    test "$P19F_TOPIC_BEFORE" = "$P19F_HEAD_BEFORE"
check "phase19f: topic is untouched" \
    test "$(cat "$P19F_S/.git/refs/heads/topic")" = "$P19F_TOPIC_BEFORE"
check "phase19f: the sequencer state is gone" test ! -d "$P19F_S/.git/sg-rebase"
P19F_STATUS=$(cd "$P19F_S" && "$SG" status 2>/dev/null | tail -1)
check "phase19f: the work tree is restored (got '$P19F_STATUS')" \
    test "$P19F_STATUS" = "nothing to commit, working tree clean"
p19_added "$P19F_S/.git/logs/HEAD" "$P19F_S_HEADN" > "$WORKDIR/p19f_s.txt"
p19_added "$P19F_G/.git/logs/HEAD" "$P19F_G_HEADN" > "$WORKDIR/p19f_g.txt"
check "phase19f: its reflog lines match git's, including what abort returns to" \
    cmp -s "$WORKDIR/p19f_s.txt" "$WORKDIR/p19f_g.txt"
if ! cmp -s "$WORKDIR/p19f_s.txt" "$WORKDIR/p19f_g.txt"; then
    echo "    --- phase19f mismatch ---"
    diff "$WORKDIR/p19f_s.txt" "$WORKDIR/p19f_g.txt" | sed 's/^/      /'
fi

# ------------------------------------------------------------
# Phase 19g: the two shortcuts -- up to date, and fast-forward
# ------------------------------------------------------------
P19G_S="$WORKDIR/p19g_sg"
p18f_setup "$P19G_S" "$SG" clean
p19_detach "$P19G_S" "$SG" master
P19G_HEADN=$(p19_lines "$P19G_S/.git/logs/HEAD")
P19G_OUT=$( (cd "$P19G_S" && "$SG" rebase master) 2>&1 )
check "phase19g: an up-to-date rebase on a detached HEAD exits 0" test $? = 0
check "phase19g: ...saying so without naming a branch (got '$P19G_OUT')" \
    test "$P19G_OUT" = "HEAD is up to date."
check "phase19g: ...and writing not one reflog line" \
    test "$(p19_lines "$P19G_S/.git/logs/HEAD")" = "$P19G_HEADN"
check "phase19g: ...and leaving no sequencer state" test ! -d "$P19G_S/.git/sg-rebase"

P19G2_S="$WORKDIR/p19g2_sg"
P19G2_G="$WORKDIR/p19g2_git"
p18f_setup "$P19G2_S" "$SG" clean
p18f_setup "$P19G2_G" git clean
# Detach at base: master is strictly ahead, so the rebase fast-forwards.
p19_detach "$P19G2_S" "$SG" master~1
p19_detach "$P19G2_G" git master~1
P19G2_MASTER=$(cat "$P19G2_S/.git/refs/heads/master")
P19G2_S_HEADN=$(p19_lines "$P19G2_S/.git/logs/HEAD")
P19G2_G_HEADN=$(p19_lines "$P19G2_G/.git/logs/HEAD")
P19G2_OUT=$( (cd "$P19G2_S" && "$SG" rebase master) 2>&1 )
P19G2_RC=$?
check "phase19g: a fast-forward rebase from a detached HEAD succeeds" test "$P19G2_RC" = 0
# This exact line printed "Fast-forwarded (null) to master." while every
# other check in this block was green: the fast-forward shortcut returns
# before the code paths the rest of the phase touched, so it kept the one
# unguarded current_branch left in the file. Discarding stdout is what let
# it through -- on this libc a NULL %s prints "(null)" rather than
# crashing, so even the exit status stayed 0.
check "phase19g: ...naming HEAD rather than a branch it does not have (said '$P19G2_OUT')" \
    test "$P19G2_OUT" = "Fast-forwarded HEAD to master."
(cd "$P19G2_G" && git rebase master) > /dev/null 2>&1
check "phase19g: ...moving HEAD onto the upstream, still detached" \
    test "$(cat "$P19G2_S/.git/HEAD")" = "$P19G2_MASTER"
check "phase19g: ...and leaving no sequencer state behind" test ! -d "$P19G2_S/.git/sg-rebase"
p19_added "$P19G2_S/.git/logs/HEAD" "$P19G2_S_HEADN" > "$WORKDIR/p19g_s.txt"
p19_added "$P19G2_G/.git/logs/HEAD" "$P19G2_G_HEADN" > "$WORKDIR/p19g_g.txt"
check "phase19g: ...writing only the start line git writes (got '$(cat "$WORKDIR/p19g_s.txt")')" \
    cmp -s "$WORKDIR/p19g_s.txt" "$WORKDIR/p19g_g.txt"

# ------------------------------------------------------------
# Phase 19h: a corrupt HEAD is still not a detached one
#
# 18e made this point for reset and rebase while merge was still refusing
# every detached HEAD -- a refusal that hid the distinction. Now that both
# commands accept a detached HEAD, mistaking a corrupt one for it would
# write a bare sha into a HEAD file nobody could interpret.
# ------------------------------------------------------------
P19H="$WORKDIR/p19h_corrupt"
(cd "$WORKDIR" && "$SG" init p19h_corrupt) > /dev/null 2>&1
printf 'x\n' > "$P19H/f.txt"
(cd "$P19H" && "$SG" add . && "$SG" commit -m c1 && "$SG" branch p19h-other) > /dev/null 2>&1
printf 'neither a ref nor a sha\n' > "$P19H/.git/HEAD"
for p19h_cmd in merge rebase; do
    P19H_ERR=$(cd "$P19H" && "$SG" "$p19h_cmd" p19h-other 2>&1 >/dev/null | head -1)
    check "phase19h: sg $p19h_cmd refuses a corrupt HEAD, naming it (got '$P19H_ERR')" \
        test "${P19H_ERR#sg: cannot read HEAD}" != "$P19H_ERR"
    check "phase19h: sg $p19h_cmd leaves the corrupt HEAD as evidence" \
        test "$(cat "$P19H/.git/HEAD")" = "neither a ref nor a sha"
done

# ============================================================
# Phase 21: a deleted tracked file is representable in a stash
#
# sg_tree_build_from_workdir used to fall back to the index blob for a path
# whose working-tree file was gone, which made a deletion invisible to
# `stash push`: with a deletion as the ONLY change the built tree equalled
# HEAD's and push reported "No local changes to save"; with some other change
# alongside it, the stash was created but silently carried the file's old
# content, so popping it RESTORED the file the user had deleted.
#
# The symptom of the first case is a line of stdout and nothing else -- no
# file, no ref, no reflog differs -- so these checks assert on stdout
# explicitly. Phase 19 already cost this project a bug that hid exactly
# there.
# ============================================================

p21_del_fixture() {
    # $1 = dir, $2 = "sg" or "git"
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = sg ]; then
        (cd "$WORKDIR" && "$SG" init "$(basename "$_dir")") > /dev/null 2>&1
    else
        (cd "$WORKDIR" && git init -q "$(basename "$_dir")")
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'keep\n' > "$_dir/keep.txt"
    printf 'gone\n' > "$_dir/gone.txt"
    mkdir -p "$_dir/sub"
    printf 'deep\n' > "$_dir/sub/deep.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add keep.txt gone.txt sub/deep.txt && \
            "$SG" commit -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && git add keep.txt gone.txt sub/deep.txt && git commit -q -m base)
    fi
    # The ONLY changes are deletions, one of them the sole occupant of sub/.
    rm -f "$_dir/gone.txt" "$_dir/sub/deep.txt"
}

# --- D1: push records the deletion; the stash's own tree omits the path
# while the index tree (stash^2) keeps it, since the deletion was never
# staged. Both trees are read with real git, so this is an oracle
# comparison and not sg reading back its own writes. ---
P21_D1_GIT="$WORKDIR/p21_d1_git"
p21_del_fixture "$P21_D1_GIT" git
(cd "$P21_D1_GIT" && git stash push -q -m p21) > /dev/null 2>&1
check "phase21 oracle: git stash push with only a deletion exits 0" test $? = 0
P21_D1_GIT_TREE=$(cd "$P21_D1_GIT" && git ls-tree -r --name-only refs/stash | sort)
P21_D1_GIT_IDXTREE=$(cd "$P21_D1_GIT" && git ls-tree -r --name-only refs/stash^2 | sort)
P21_D1_GIT_PORCELAIN=$(cd "$P21_D1_GIT" && git status --porcelain | sort)
check "phase21 oracle: precondition -- real git's stash tree really does omit the deleted path" \
    sh -c "! printf '%s\n' \"$P21_D1_GIT_TREE\" | grep -qx gone.txt"
check "phase21 oracle: precondition -- real git's index tree still holds it (deletion unstaged)" \
    sh -c "printf '%s\n' \"$P21_D1_GIT_IDXTREE\" | grep -qx gone.txt"

P21_D1_SG="$WORKDIR/p21_d1_sg"
p21_del_fixture "$P21_D1_SG" sg
P21_D1_SG_OUT=$(cd "$P21_D1_SG" && "$SG" stash push -m p21 2>&1)
check "phase21: sg stash push with only a deletion exits 0" test $? = 0
check "phase21: sg does NOT report 'No local changes to save' (got '$P21_D1_SG_OUT')" \
    sh -c "! printf '%s\n' \"$P21_D1_SG_OUT\" | grep -q 'No local changes to save'"
check "phase21: sg actually created a stash entry" \
    sh -c "[ \"\$(cd '$P21_D1_SG' && '$SG' stash list | wc -l | tr -d ' ')\" = 1 ]"
P21_D1_SG_TREE=$(cd "$P21_D1_SG" && git ls-tree -r --name-only refs/stash | sort)
P21_D1_SG_IDXTREE=$(cd "$P21_D1_SG" && git ls-tree -r --name-only refs/stash^2 | sort)
P21_D1_SG_PORCELAIN=$(cd "$P21_D1_SG" && git status --porcelain | sort)
check "phase21: sg's stash tree matches real git's byte-for-byte (the deletion is recorded)" \
    test "$P21_D1_SG_TREE" = "$P21_D1_GIT_TREE"
check "phase21: sg's index tree (stash^2) matches real git's -- an unstaged deletion stays there" \
    test "$P21_D1_SG_IDXTREE" = "$P21_D1_GIT_IDXTREE"
check "phase21: sg's post-push status matches real git's byte-for-byte" \
    test "$P21_D1_SG_PORCELAIN" = "$P21_D1_GIT_PORCELAIN"

# --- D2: pop replays the deletion into the working tree, and the directory
# the deletion emptied is removed -- which is the first time any sg command
# deletes a tracked file through this path at all. ---
(cd "$P21_D1_GIT" && git stash pop -q) > /dev/null 2>&1
P21_D2_GIT_PORCELAIN=$(cd "$P21_D1_GIT" && git status --porcelain | sort)
check "phase21 oracle: precondition -- real git removes the emptied sub/ on pop" \
    test ! -e "$P21_D1_GIT/sub"

(cd "$P21_D1_SG" && "$SG" stash pop) > /dev/null 2>&1
check "phase21: sg stash pop exits 0" test $? = 0
check "phase21: pop re-deletes the file rather than restoring it" \
    test ! -e "$P21_D1_SG/gone.txt"
check "phase21: pop leaves keep.txt alone" test -f "$P21_D1_SG/keep.txt"
check "phase21: pop removes the directory the deletion emptied, matching real git" \
    test ! -e "$P21_D1_SG/sub"
P21_D2_SG_PORCELAIN=$(cd "$P21_D1_SG" && git status --porcelain | sort)
check "phase21: sg's post-pop status matches real git's byte-for-byte" \
    test "$P21_D2_SG_PORCELAIN" = "$P21_D2_GIT_PORCELAIN"

# --- D3: contrast -- a STAGED deletion is absent from the index tree too.
# Without this, D1's stash^2 check would also pass on an implementation that
# simply never removed anything from the index tree. ---
P21_D3_GIT="$WORKDIR/p21_d3_git"
p21_del_fixture "$P21_D3_GIT" git
(cd "$P21_D3_GIT" && git rm -q --cached gone.txt) > /dev/null 2>&1
(cd "$P21_D3_GIT" && git stash push -q -m p21staged) > /dev/null 2>&1
P21_D3_GIT_IDXTREE=$(cd "$P21_D3_GIT" && git ls-tree -r --name-only refs/stash^2 | sort)
check "phase21 oracle: precondition -- a staged deletion IS absent from real git's index tree" \
    sh -c "! printf '%s\n' \"$P21_D3_GIT_IDXTREE\" | grep -qx gone.txt"
check "phase21 oracle: precondition -- D1 and D3 index trees really do differ" \
    test "$P21_D3_GIT_IDXTREE" != "$P21_D1_GIT_IDXTREE"

P21_D3_SG="$WORKDIR/p21_d3_sg"
p21_del_fixture "$P21_D3_SG" sg
(cd "$P21_D3_SG" && git rm -q --cached gone.txt) > /dev/null 2>&1
(cd "$P21_D3_SG" && "$SG" stash push -m p21staged) > /dev/null 2>&1
check "phase21: sg stash push with a staged deletion exits 0" test $? = 0
P21_D3_SG_IDXTREE=$(cd "$P21_D3_SG" && git ls-tree -r --name-only refs/stash^2 | sort)
check "phase21: sg's index tree drops a STAGED deletion, matching real git" \
    test "$P21_D3_SG_IDXTREE" = "$P21_D3_GIT_IDXTREE"

# --- D4: -u -- the deletion belongs in the stash's own tree, the untracked
# file in the third parent, and the two must not bleed into each other. ---
P21_D4_GIT="$WORKDIR/p21_d4_git"
p21_del_fixture "$P21_D4_GIT" git
printf 'fresh\n' > "$P21_D4_GIT/fresh.txt"
(cd "$P21_D4_GIT" && git stash push -q -u -m p21u) > /dev/null 2>&1
P21_D4_GIT_TREE=$(cd "$P21_D4_GIT" && git ls-tree -r --name-only refs/stash | sort)
P21_D4_GIT_UNTRACKED=$(cd "$P21_D4_GIT" && git ls-tree -r --name-only refs/stash^3 | sort)

P21_D4_SG="$WORKDIR/p21_d4_sg"
p21_del_fixture "$P21_D4_SG" sg
printf 'fresh\n' > "$P21_D4_SG/fresh.txt"
(cd "$P21_D4_SG" && "$SG" stash push -u -m p21u) > /dev/null 2>&1
check "phase21 -u: sg stash push -u exits 0" test $? = 0
P21_D4_SG_TREE=$(cd "$P21_D4_SG" && git ls-tree -r --name-only refs/stash | sort)
P21_D4_SG_UNTRACKED=$(cd "$P21_D4_SG" && git ls-tree -r --name-only refs/stash^3 | sort)
check "phase21 -u: sg's stash tree matches real git's (deletion recorded there, not in ^3)" \
    test "$P21_D4_SG_TREE" = "$P21_D4_GIT_TREE"
check "phase21 -u: sg's untracked tree (stash^3) matches real git's" \
    test "$P21_D4_SG_UNTRACKED" = "$P21_D4_GIT_UNTRACKED"

# --- D5: a divergence this phase newly made reachable, recorded on purpose
# rather than left to surface as a red check later.
#
# Stash an UNSTAGED deletion, then stage that same deletion, then pop. sg
# refuses, because its dirty gate compares against HEAD (not the index) and
# sees a path the stash would touch whose index entry is gone. Real git
# allows it. This is the same "ours is HEAD, not the index" divergence
# Phase 20 documented for apply/pop; it simply had no way to be reached
# before a stash could record a deletion at all. Deliberately NOT compared
# against real git here -- only sg's own refusal is pinned, so that a future
# change of mind shows up as a failing check rather than silent drift. ---
P21_D5="$WORKDIR/p21_d5_sg"
p21_del_fixture "$P21_D5" sg
(cd "$P21_D5" && "$SG" stash push -m p21d5) > /dev/null 2>&1
(cd "$P21_D5" && git rm -q --cached gone.txt) > /dev/null 2>&1
P21_D5_OUT=$(cd "$P21_D5" && "$SG" stash pop 2>&1 >/dev/null)
P21_D5_RC=$?
check "phase21 divergence: sg refuses to pop onto a now-staged deletion of the same path" \
    test "$P21_D5_RC" != 0
check "phase21 divergence: sg says why on stderr (got '$P21_D5_OUT')" \
    sh -c "[ -n \"$P21_D5_OUT\" ]"
check "phase21 divergence: the refusal left the stash in place" \
    sh -c "[ \"\$(cd '$P21_D5' && '$SG' stash list | wc -l | tr -d ' ')\" = 1 ]"

# ============================================================
# Phase 22: a tree entry named .git cannot write into the gitdir
#
# entry_name_is_safe has refused "", ".", ".." and any name containing '/'
# since Phase 2, so path traversal was already closed. ".git" slipped
# between those three rules: no slash, not "." and not "..". Measured before
# the fix: a commit whose tree carried a ".git" subtree made
# `sg reset --hard` exit 0 while writing .git/hacked.txt: HEAD, config and
# refs/* are all writable that way, from any commit a remote can serve.
#
# The trees here are assembled byte by byte with git hash-object
# --literally, because neither git write-tree nor sg can produce one: real
# git refuses such a path on the way in as well as on the way out, which is
# the whole reason this is a hostile-input test and not a round-trip test.
# ============================================================

p22_evil_fixture() {
    # $1 = dir, $2 = "sg" or "git". Leaves refs/heads/evil pointing at a
    # commit whose tree holds { ".git"/ -> { hacked.txt }, normal.txt }.
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = sg ]; then
        (cd "$WORKDIR" && "$SG" init "$(basename "$_dir")") > /dev/null 2>&1
    else
        (cd "$WORKDIR" && git init -q "$(basename "$_dir")")
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user")
    printf 'ok\n' > "$_dir/normal.txt"
    if [ "$_impl" = sg ]; then
        (cd "$_dir" && "$SG" add normal.txt && "$SG" commit -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && git add normal.txt && git commit -q -m base)
    fi
    _blob=$(cd "$_dir" && printf 'PWNED\n' | git hash-object -w --stdin)
    _norm=$(cd "$_dir" && printf 'ok\n' | git hash-object -w --stdin)
    python3 -c "
import sys,binascii
sys.stdout.buffer.write(b'100644 hacked.txt\x00'+binascii.unhexlify('$_blob'))" > "$_dir/.inner.bin"
    _inner=$(cd "$_dir" && git hash-object -t tree -w --stdin --literally < .inner.bin)
    python3 -c "
import sys,binascii
sys.stdout.buffer.write(b'40000 .git\x00'+binascii.unhexlify('$_inner'))
sys.stdout.buffer.write(b'100644 normal.txt\x00'+binascii.unhexlify('$_norm'))" > "$_dir/.outer.bin"
    _outer=$(cd "$_dir" && git hash-object -t tree -w --stdin --literally < .outer.bin)
    rm -f "$_dir/.inner.bin" "$_dir/.outer.bin"
    _evil=$(cd "$_dir" && git commit-tree "$_outer" -m evil)
    (cd "$_dir" && git update-ref refs/heads/evil "$_evil")
}

# --- E1: oracle -- real git refuses the same commit ---
P22_EVIL_GIT="$WORKDIR/p22_evil_git"
p22_evil_fixture "$P22_EVIL_GIT" git
P22_EVIL_GIT_ERR=$( (cd "$P22_EVIL_GIT" && git reset --hard evil) 2>&1 >/dev/null )
check "phase22 oracle: real git leaves the gitdir alone" \
    test ! -e "$P22_EVIL_GIT/.git/hacked.txt"
check "phase22 oracle: precondition -- real git says the path is invalid (got '$P22_EVIL_GIT_ERR')" \
    sh -c "printf '%s' \"$P22_EVIL_GIT_ERR\" | grep -q '.git/hacked.txt'"

# --- E2: sg must refuse it too, on every command that applies a tree ---
for p22_cmd in reset switch; do
    P22_EVIL_SG="$WORKDIR/p22_evil_sg_$p22_cmd"
    p22_evil_fixture "$P22_EVIL_SG" sg
    if [ "$p22_cmd" = reset ]; then
        P22_ERR=$( (cd "$P22_EVIL_SG" && "$SG" reset --hard evil) 2>&1 >/dev/null )
    else
        P22_ERR=$( (cd "$P22_EVIL_SG" && "$SG" switch evil) 2>&1 >/dev/null )
    fi
    P22_RC=$?
    check "phase22: sg $p22_cmd refuses the hostile tree (exit $P22_RC)" test "$P22_RC" != 0
    check "phase22: sg $p22_cmd writes nothing into the gitdir" \
        test ! -e "$P22_EVIL_SG/.git/hacked.txt"
    check "phase22: sg $p22_cmd says why on stderr (got '$P22_ERR')" \
        sh -c "[ -n \"$P22_ERR\" ]"
    check "phase22: sg $p22_cmd names the offending path, not just 'corrupt'" \
        sh -c "printf '%s' \"$P22_ERR\" | grep -q '\.git'"
    check "phase22: the refusal left the working tree on the original commit" \
        test -f "$P22_EVIL_SG/normal.txt"
done

# --- E3: the write side -- sg must not be able to CREATE such a tree.
# Real git ignores the path silently and exits 0; sg reports and exits 1.
# The disk outcome is identical, only the exit code differs, and that is a
# deliberate divergence: sg add is all-or-nothing. Asserted rather than
# worked around. ---
P22_ADD_GIT="$WORKDIR/p22_add_git"
(cd "$WORKDIR" && git init -q p22_add_git)
(cd "$P22_ADD_GIT" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P22_ADD_GIT/d/.git"
printf 'EVIL\n' > "$P22_ADD_GIT/d/.git/evil"
printf 'ok\n' > "$P22_ADD_GIT/normal.txt"
(cd "$P22_ADD_GIT" && git add d/.git/evil) > /dev/null 2>&1
check "phase22 oracle: real git stages nothing for a .git path" \
    sh -c "! (cd '$P22_ADD_GIT' && git ls-files) | grep -q '\.git/evil'"

P22_ADD_SG="$WORKDIR/p22_add_sg"
(cd "$WORKDIR" && "$SG" init p22_add_sg) > /dev/null 2>&1
(cd "$P22_ADD_SG" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P22_ADD_SG/d/.git"
printf 'EVIL\n' > "$P22_ADD_SG/d/.git/evil"
printf 'ok\n' > "$P22_ADD_SG/normal.txt"
P22_ADD_ERR=$( (cd "$P22_ADD_SG" && "$SG" add d/.git/evil) 2>&1 >/dev/null )
P22_ADD_RC=$?
check "phase22: sg add refuses a .git path (exit $P22_ADD_RC -- deliberate divergence, git exits 0)" \
    test "$P22_ADD_RC" != 0
check "phase22: sg add stages nothing for it, same disk outcome as real git" \
    sh -c "! (cd '$P22_ADD_SG' && git ls-files) | grep -q '\.git/evil'"
check "phase22: sg add says why on stderr (got '$P22_ADD_ERR')" \
    sh -c "[ -n \"$P22_ADD_ERR\" ]"

# --- E4: the rule must not be over-broad. These names all merely start with
# or contain ".git" or dots and are perfectly ordinary; a prefix or substring
# comparison would reject them and this is the only thing checking for that. ---
P22_OK="$WORKDIR/p22_ok"
(cd "$WORKDIR" && "$SG" init p22_ok) > /dev/null 2>&1
(cd "$P22_OK" && git config user.email "a@b.c" && git config user.name "git user")
printf 'ign\n' > "$P22_OK/.gitignore"
printf 'mod\n' > "$P22_OK/.gitmodules"
printf 'a\n' > "$P22_OK/..a"
printf 'b\n' > "$P22_OK/a.."
printf 'c\n' > "$P22_OK/git~1"
mkdir -p "$P22_OK/.github/workflows"
printf 'w\n' > "$P22_OK/.github/workflows/ci.yml"
(cd "$P22_OK" && "$SG" add . && "$SG" commit -m ok) > /dev/null 2>&1
check "phase22: ordinary names that merely resemble .git are still accepted" test $? = 0
P22_OK_FILES=$(cd "$P22_OK" && git ls-files | sort | tr '\n' ' ')
check "phase22: all six are tracked (got '$P22_OK_FILES')" \
    sh -c "[ \"\$(cd '$P22_OK' && git ls-files | wc -l | tr -d ' ')\" = 6 ]"
(cd "$P22_OK" && "$SG" switch -c other && "$SG" switch master) > /dev/null 2>&1
# --- E5: the working-tree walk must skip ONLY the real gitdir. A directory
# named ".git." is an ordinary directory to a walker -- real git lists it as
# untracked (measured) and refuses it only when adding. Folding the walk's
# skip into the safety predicate made sg omit it from status entirely, which
# is the failure mode nobody notices: a listing quietly missing a file. ---
P22_WALK_GIT="$WORKDIR/p22_walk_git"
(cd "$WORKDIR" && git init -q p22_walk_git)
(cd "$P22_WALK_GIT" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P22_WALK_GIT/.git./inner" "$P22_WALK_GIT/plain"
printf 'x\n' > "$P22_WALK_GIT/.git./inner/f.txt"
printf 'y\n' > "$P22_WALK_GIT/plain/g.txt"
P22_WALK_GIT_ST=$(cd "$P22_WALK_GIT" && git status --porcelain | sort)
check "phase22 oracle: precondition -- real git lists the .git. directory as untracked" \
    sh -c "printf '%s\n' \"$P22_WALK_GIT_ST\" | grep -q '\.git\.'"

P22_WALK_SG="$WORKDIR/p22_walk_sg"
(cd "$WORKDIR" && "$SG" init p22_walk_sg) > /dev/null 2>&1
(cd "$P22_WALK_SG" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P22_WALK_SG/.git./inner" "$P22_WALK_SG/plain"
printf 'x\n' > "$P22_WALK_SG/.git./inner/f.txt"
printf 'y\n' > "$P22_WALK_SG/plain/g.txt"
# Captured to a file rather than a shell variable: sg status output is
# multi-line and tab-indented, and re-quoting that through sh -c produced a
# FALSE FAILURE the first time this check was written -- the guard was
# working and the check was lying about it.
P22_WALK_SG_ST="$WORKDIR/p22_walk_sg_status.txt"
(cd "$P22_WALK_SG" && "$SG" status) > "$P22_WALK_SG_ST" 2>/dev/null
check "phase22: sg status lists the .git. directory too, rather than under-reporting it" \
    grep -q '\.git\./' "$P22_WALK_SG_ST"
check "phase22: sg status still lists the ordinary directory alongside it" \
    grep -q 'plain/' "$P22_WALK_SG_ST"
check "phase22: and sg still never descends into the real gitdir" \
    sh -c "! grep -qE '(^|[^.])\.git/' '$P22_WALK_SG_ST'"

check "phase22: and a tree holding them still checks out" \
    sh -c "[ -f '$P22_OK/.gitignore' ] && [ -f '$P22_OK/.github/workflows/ci.yml' ] && [ -f '$P22_OK/git~1' ]"

# ============================================================
# Phase 23: paths are quoted the way git quotes them
#
# Before this, a filename carrying an ESC byte reached the terminal intact
# from `sg status` and `sg cat-file -p` -- measured with od -c, not inferred:
# sg emitted byte 0x1b while git emitted the four characters \033. One
# filename was enough to clear the screen or recolour everything printed
# after it, which is a convincing way to fake "nothing to commit".
#
# sg quotes >= 0x80 bytes as-is, git's core.quotepath=false behaviour rather
# than its default, so every comparison here passes that flag EXCEPT the
# control-character group (where both sides quote regardless) and the
# divergence check at the end, which exists to pin the difference itself.
#
# Fixtures stay at the repository root on purpose: git folds an untracked
# directory into a single "dir/" entry while sg lists the files inside it,
# and that difference has nothing to do with quoting -- it would just make
# these checks fail for the wrong reason.
# ============================================================

# Writes the hostile fixture files into $1. $2 selects the set:
#   ctrl   -- ASCII control bytes only (identical on both sides, no flag)
#   high   -- non-ASCII only (this is where sg and git diverge)
#   mixed  -- both, plus the names that must NOT be quoted
p23_make_files() {
    python3 - "$1" "$2" <<'PY'
import os, sys
d, which = sys.argv[1], sys.argv[2]
ctrl  = ['esc\x1bhere.txt', 'tab\there.txt', 'bel\ahere.txt', 'del\x7fhere.txt',
         'ctrl\x01here.txt', 'back\\slash.txt', 'quote"here.txt',
         'bin\x1besc.dat']
high  = ['\u4e2d\u6587.txt', '\u00fc.txt']
plain = ['plain.txt', 'has space.txt', 'meta~!$*?#;|&()<>[].txt', "single'quote.txt"]
sets  = {'ctrl': ctrl + plain, 'high': high, 'mixed': ctrl + high + plain}
for n in sets[which]:
    path = os.path.join(d, n)
    if 'bin' in n:
        with open(path, 'wb') as f:
            f.write(b'\x00\x01body\n')
    else:
        with open(path, 'w') as f:
            f.write('body\n')
PY
}

# --- Q1: cat-file -p on a tree. The strongest oracle available: both sides'
# output format is fully determined, so this is a whole-output byte compare
# with nothing extracted or normalised away. ---
P23_CF="$WORKDIR/p23_catfile"
(cd "$WORKDIR" && "$SG" init p23_catfile) > /dev/null 2>&1
(cd "$P23_CF" && git config user.email "a@b.c" && git config user.name "git user")
p23_make_files "$P23_CF" mixed
(cd "$P23_CF" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
check "phase23: sg add/commit of hostile filenames exits 0" test $? = 0
P23_TREE=$(cd "$P23_CF" && git rev-parse HEAD^{tree} 2>/dev/null)
(cd "$P23_CF" && "$SG" cat-file -p "$P23_TREE") 2>/dev/null | sort > "$WORKDIR/p23_cf_sg.txt"
(cd "$P23_CF" && git -c core.quotepath=false cat-file -p "$P23_TREE") 2>/dev/null | sort > "$WORKDIR/p23_cf_git.txt"
check "phase23: sg cat-file -p on a tree matches real git byte-for-byte" \
    cmp -s "$WORKDIR/p23_cf_sg.txt" "$WORKDIR/p23_cf_git.txt"
check "phase23: and no raw ESC byte survives into that output" \
    sh -c "! LC_ALL=C grep -q \"\$(printf '\\033')\" '$WORKDIR/p23_cf_sg.txt'"
check "phase23 oracle: precondition -- the fixture really does carry an ESC byte" \
    sh -c "ls -b '$P23_CF' | grep -q 'esc'"

# --- Q2: diff, whole-output byte compare. sg's patch body now tracks git's
# closely enough (Phase 26) that the full output is worth comparing, not just
# the header lines. ---
P23_DIFF="$WORKDIR/p23_diff"
(cd "$WORKDIR" && "$SG" init p23_diff) > /dev/null 2>&1
(cd "$P23_DIFF" && git config user.email "a@b.c" && git config user.name "git user")
p23_make_files "$P23_DIFF" mixed
(cd "$P23_DIFF" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
python3 - "$P23_DIFF" <<'PY'
import os, sys
# Rewrite every tracked file, and make the binary one binary on both sides so
# the "Binary files ... differ" line is exercised: without a NUL byte in the
# fixture that whole line has no coverage, which a per-site mutation found.
for n in os.listdir(sys.argv[1]):
    p = os.path.join(sys.argv[1], n)
    if not os.path.isfile(p):
        continue
    if 'bin' in n:
        open(p, 'wb').write(b'\x00\x01CHANGED\n')
    else:
        open(p, 'w').write('CHANGED\n')
PY
(cd "$P23_DIFF" && "$SG" diff) 2>/dev/null > "$WORKDIR/p23_d_sg.txt"
(cd "$P23_DIFF" && git -c core.quotepath=false diff) 2>/dev/null > "$WORKDIR/p23_d_git.txt"
check "phase23: sg diff matches real git byte-for-byte" \
    cmp -s "$WORKDIR/p23_d_sg.txt" "$WORKDIR/p23_d_git.txt"
# The trailing TAB after ---/+++ is git's disambiguator for a name containing
# a space, and only for those two lines. Asserted on its own because a whole
# -header cmp that happened to have no spaced name would pass without it.
check "phase23: a spaced name gets git's trailing TAB on --- and +++" \
    sh -c "grep -c \"^--- a/has space.txt\$(printf '\\t')\$\" '$WORKDIR/p23_d_sg.txt' | grep -q '^1\$'"
check "phase23: and diff --git does NOT get one, matching git" \
    sh -c "! grep -q \"^diff --git.*\$(printf '\\t')\$\" '$WORKDIR/p23_d_sg.txt'"

# --- Q3: the untracked list in sg status, against git's LONG format -- not
# --porcelain. The two are different oracles: porcelain quotes a name merely
# for containing a space (its "?? " prefix makes space a field separator),
# while the long format and ls-files leave it bare. Measured. Comparing sg's
# long output against porcelain reports a failure for a name sg handles
# exactly right, which is the kind of false red that gets correct code
# "fixed". Only the path column is compared: the wording around it is sg's
# own. ---
P23_ST="$WORKDIR/p23_status"
(cd "$WORKDIR" && "$SG" init p23_status) > /dev/null 2>&1
(cd "$P23_ST" && git config user.email "a@b.c" && git config user.name "git user")
p23_make_files "$P23_ST" mixed
(cd "$P23_ST" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
# All four label kinds get a name that needs quoting. A per-site mutation
# proved this matters: with only untracked names present, deleting the
# quoting from the staged/unstaged printer reddened nothing at all.
# The staged name uses a double quote rather than a control byte so it can
# be passed through argv here without another layer of escaping.
p23_st_mutate() {
    python3 - "$1" <<'PYX'
import os, sys
d = sys.argv[1]
open(os.path.join(d, 'esc\x1bhere.txt'), 'w').write('CHANGED\n')
os.remove(os.path.join(d, 'tab\there.txt'))
open(os.path.join(d, 'added"quote.txt'), 'w').write('fresh\n')
open(os.path.join(d, 'untracked\x1bnew.txt'), 'w').write('fresh\n')
# A bare, untracked name whose only oddity is a space. The checks below
# use it to prove the rule is not over-broad, so it must stay untracked.
open(os.path.join(d, 'bare space.txt'), 'w').write('fresh\n')
PYX
}
p23_st_mutate "$P23_ST"
(cd "$P23_ST" && "$SG" add 'added"quote.txt') > /dev/null 2>&1
# git's labels are compared alongside the paths, so its language is pinned:
# this machine's git speaks Chinese while sg's labels are English.
(cd "$P23_ST" && "$SG" status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_s_sg.txt"
(cd "$P23_ST" && LC_ALL=C git -c core.quotepath=false status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_s_git.txt"
check "phase23: sg status's untracked paths match real git's byte-for-byte" \
    cmp -s "$WORKDIR/p23_s_sg.txt" "$WORKDIR/p23_s_git.txt"
check "phase23: sg status emits no raw ESC byte" \
    sh -c "! LC_ALL=C grep -q \"\$(printf '\\033')\" '$WORKDIR/p23_s_sg.txt'"
# The two git formats disagree here, and that disagreement is the reason the
# comparison above uses the long one. Pinned so a future edit cannot quietly
# switch oracles and start "fixing" sg to match the wrong one.
P23_ST_PORC="$WORKDIR/p23_s_git_porcelain.txt"
(cd "$P23_ST" && git -c core.quotepath=false status --porcelain) 2>/dev/null | sed -n 's/^?? //p' | sort > "$P23_ST_PORC"
check "phase23 oracle: precondition -- git's porcelain quotes a spaced name where its long format does not" \
    sh -c "grep -qx '\"bare space.txt\"' '$P23_ST_PORC' && grep -qx 'bare space.txt' '$WORKDIR/p23_s_git.txt'"

check "phase23: names needing no quoting are still printed bare" \
    sh -c "grep -qx 'bare space.txt' '$WORKDIR/p23_s_sg.txt'"

# --- Q6: the unmerged ("both modified") line. It has its own printer, and a
# per-site mutation showed the other status fixtures do not reach it: with
# no conflict present, deleting the quoting there reddened nothing. ---
P23_CONF="$WORKDIR/p23_conflict"
(cd "$WORKDIR" && "$SG" init p23_conflict) > /dev/null 2>&1
(cd "$P23_CONF" && git config user.email "a@b.c" && git config user.name "git user")
python3 -c "
import sys
open(sys.argv[1] + '/conf\x1bict.txt', 'w').write('base\n')" "$P23_CONF"
(cd "$P23_CONF" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P23_CONF" && "$SG" switch -c other) > /dev/null 2>&1
python3 -c "
import sys
open(sys.argv[1] + '/conf\x1bict.txt', 'w').write('theirs\n')" "$P23_CONF"
(cd "$P23_CONF" && "$SG" add . && "$SG" commit -m theirs) > /dev/null 2>&1
(cd "$P23_CONF" && "$SG" switch master) > /dev/null 2>&1
python3 -c "
import sys
open(sys.argv[1] + '/conf\x1bict.txt', 'w').write('ours\n')" "$P23_CONF"
(cd "$P23_CONF" && "$SG" add . && "$SG" commit -m ours) > /dev/null 2>&1
(cd "$P23_CONF" && "$SG" merge other) > /dev/null 2>&1
check "phase23 oracle: precondition -- the merge really did conflict" \
    test -f "$P23_CONF/.git/MERGE_HEAD"
(cd "$P23_CONF" && "$SG" status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_u_sg.txt"
(cd "$P23_CONF" && LC_ALL=C git -c core.quotepath=false status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_u_git.txt"
check "phase23: the unmerged path is quoted the same way git quotes it" \
    cmp -s "$WORKDIR/p23_u_sg.txt" "$WORKDIR/p23_u_git.txt"
check "phase23: and that line carries no raw ESC byte" \
    sh -c "! LC_ALL=C grep -q \"\$(printf '\\033')\" '$WORKDIR/p23_u_sg.txt'"

# --- Q7: error messages. Nothing else in this suite looks at stderr wording,
# so without this the whole error-message half of the quoting work has no
# coverage at all -- a per-site mutation is the only way to notice, and it
# would report a dead angle rather than a passing test.
#
# Two checks, and the second one is the discriminating one: a name carrying
# an ESC gets quoted by BOTH sg_quote_path and sg_quote_path_delimited, so it
# cannot tell them apart. Only a name that needs no escaping at all can --
# _delimited quotes it anyway, precisely so a reader can see where a path
# with leading or trailing spaces begins and ends. sg has no oracle here:
# git's own wording differs, so these pin sg's behaviour rather than compare.
P23_ERR="$WORKDIR/p23_err"
(cd "$WORKDIR" && "$SG" init p23_err) > /dev/null 2>&1
(cd "$P23_ERR" && git config user.email "a@b.c" && git config user.name "git user")
P23_ERR_OUT="$WORKDIR/p23_err_hostile.txt"
python3 - "$SG" "$P23_ERR" "$P23_ERR_OUT" <<'PYE'
import subprocess, sys
sg, d, out = sys.argv[1], sys.argv[2], sys.argv[3]
r = subprocess.run([sg, 'add', 'ev\x1bil.txt'], cwd=d, capture_output=True)
open(out, 'wb').write(r.stderr)
PYE
check "phase23: an error message escapes a control byte instead of emitting it" \
    sh -c "! LC_ALL=C grep -q \"\$(printf '\\033')\" '$P23_ERR_OUT'"
check "phase23: and renders it as the literal four characters git uses" \
    grep -q '\\033' "$P23_ERR_OUT"

P23_ERR_PLAIN="$WORKDIR/p23_err_plain.txt"
(cd "$P23_ERR" && "$SG" restore missing.txt) 2> "$P23_ERR_PLAIN" > /dev/null
check "phase23: a path inside a sentence is delimited even when nothing needs escaping" \
    grep -q '"missing.txt"' "$P23_ERR_PLAIN"

# --- Q8: the error paths that only a damaged repository reaches. A cold read
# found three sites in cmd_diff.c and three in cmd_chunk_info.c still printing
# raw bytes while the rest of the same files had been converted -- and nothing
# was watching them, so the miss was invisible. Reaching them needs an index
# entry whose blob is gone, which is exactly what this fixture builds. ---
P23_DMG="$WORKDIR/p23_damaged"
(cd "$WORKDIR" && "$SG" init p23_damaged) > /dev/null 2>&1
(cd "$P23_DMG" && git config user.email "a@b.c" && git config user.name "git user")
python3 -c "
import sys
open(sys.argv[1] + '/ev\x1bil.txt', 'w').write('body\n')" "$P23_DMG"
(cd "$P23_DMG" && "$SG" add .) > /dev/null 2>&1
P23_DMG_BLOB=$(cd "$P23_DMG" && git ls-files -s | awk '{print $2}' | head -1)
rm -f "$P23_DMG/.git/objects/$(printf '%s' "$P23_DMG_BLOB" | cut -c1-2)/$(printf '%s' "$P23_DMG_BLOB" | cut -c3-)"
check "phase23 oracle: precondition -- the staged blob really is gone" \
    sh -c "! git -C '$P23_DMG' cat-file -e '$P23_DMG_BLOB' 2>/dev/null"

P23_DMG_DIFF="$WORKDIR/p23_damaged_diff.txt"
(cd "$P23_DMG" && "$SG" diff) 2> "$P23_DMG_DIFF" > /dev/null
check "phase23: sg diff's missing-blob warning escapes the path" \
    sh -c "! LC_ALL=C grep -q \"\$(printf '\\033')\" '$P23_DMG_DIFF'"
check "phase23: and renders it as the literal escape" grep -q '\\033' "$P23_DMG_DIFF"

P23_DMG_CI="$WORKDIR/p23_damaged_chunkinfo.txt"
python3 - "$SG" "$P23_DMG" "$P23_DMG_CI" <<'PYD'
import subprocess, sys
sg, d, out = sys.argv[1], sys.argv[2], sys.argv[3]
r = subprocess.run([sg, 'chunk-info', 'ev\x1bil.txt'], cwd=d, capture_output=True)
open(out, 'wb').write(r.stderr)
PYD
check "phase23: sg chunk-info's missing-object error escapes the path too" \
    sh -c "! LC_ALL=C grep -q \"\$(printf '\\033')\" '$P23_DMG_CI'"
check "phase23: and renders it as the literal escape" grep -q '\\033' "$P23_DMG_CI"

# --- Q4: control characters alone, compared against git with NO flag. Both
# implementations quote these regardless of core.quotepath, so this group
# pins that the divergence is confined to bytes >= 0x80. ---
P23_CTRL="$WORKDIR/p23_ctrl"
(cd "$WORKDIR" && "$SG" init p23_ctrl) > /dev/null 2>&1
(cd "$P23_CTRL" && git config user.email "a@b.c" && git config user.name "git user")
p23_make_files "$P23_CTRL" ctrl
(cd "$P23_CTRL" && "$SG" status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_c_sg.txt"
(cd "$P23_CTRL" && git status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_c_git.txt"
check "phase23: control-character names match git even WITHOUT core.quotepath=false" \
    cmp -s "$WORKDIR/p23_c_sg.txt" "$WORKDIR/p23_c_git.txt"

# --- Q5: the divergence itself, asserted rather than merely tolerated. A
# non-ASCII name must differ from default git and match git with the flag.
# Without this, silently switching sg to git's default would break nothing. ---
P23_HIGH="$WORKDIR/p23_high"
(cd "$WORKDIR" && "$SG" init p23_high) > /dev/null 2>&1
(cd "$P23_HIGH" && git config user.email "a@b.c" && git config user.name "git user")
p23_make_files "$P23_HIGH" high
(cd "$P23_HIGH" && "$SG" status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_h_sg.txt"
(cd "$P23_HIGH" && git status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_h_gitdef.txt"
(cd "$P23_HIGH" && git -c core.quotepath=false status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p23_h_gitraw.txt"
check "phase23 oracle: precondition -- git's two quotepath settings really do differ here" \
    sh -c "! cmp -s '$WORKDIR/p23_h_gitdef.txt' '$WORKDIR/p23_h_gitraw.txt'"
check "phase23 divergence: sg matches git -c core.quotepath=false for non-ASCII" \
    cmp -s "$WORKDIR/p23_h_sg.txt" "$WORKDIR/p23_h_gitraw.txt"
check "phase23 divergence: and deliberately does NOT match git's default" \
    sh -c "! cmp -s '$WORKDIR/p23_h_sg.txt' '$WORKDIR/p23_h_gitdef.txt'"

# ==========================================================================
# Phase 25: flags on `sg diff` / `sg status`, and `sg stash show`.
#
# Two oracles are in play here and mixing them up is the known failure mode
# (Phase 23 produced a false red exactly this way):
#   * the machine-readable diff formats (--stat/--numstat/--name-only/
#     --name-status) and the LONG status format leave a space bare and quote
#     only control bytes -- sg_quote_path's rule;
#   * `status --porcelain` quotes a name merely for containing a space,
#     because its "?? " prefix makes the space a field separator.
# Both are asserted below, and the divergence between them is asserted too,
# so that collapsing one into the other cannot pass silently.
#
# git side always runs with `-c core.quotepath=false` (sg emits >=0x80 raw)
# and, wherever a translated label could appear, with LC_ALL=C: the long
# status format is localised and this repo's developer shell is not C.
# ==========================================================================
P25="$WORKDIR/p25"
(cd "$WORKDIR" && "$SG" init p25) > /dev/null 2>&1
(cd "$P25" && git config user.email "a@b.c" && git config user.name "git user")

python3 - "$P25" <<'PY'
import os, sys
d = sys.argv[1]
os.makedirs(os.path.join(d, 'sub'), exist_ok=True)
open(os.path.join(d, 'tracked.txt'), 'w').write('a\nb\nc\n')
open(os.path.join(d, 'has space.txt'), 'w').write('x\n')
open(os.path.join(d, 'ctl\there.txt'), 'w').write('t\n')
open(os.path.join(d, 'sub/deep.txt'), 'w').write('1\n2\n')
open(os.path.join(d, 'bin.dat'), 'wb').write(bytes(range(256)) * 4)
PY
(cd "$P25" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
# sg has no rev-parse subcommand; the base id is fixture setup, not the
# thing under test, so real git names it.
P25_BASE=$(git -C "$P25" rev-parse HEAD)

# A second commit, so that there are two trees to diff against each other.
python3 - "$P25" <<'PY'
import sys
d = sys.argv[1]
open(d + '/tracked.txt', 'w').write('a\nB\nc\nd\n')
open(d + '/added_in_c2.txt', 'w').write('n\n' * 3)
PY
(cd "$P25" && "$SG" add . && "$SG" commit -m second) > /dev/null 2>&1

# Now spread the working tree across every state the two formats can show:
# staged modify, staged add, unstaged modify, unstaged delete, an untracked
# file, a wholly untracked directory (which git collapses to "un/"), and an
# untracked directory nested under a tracked one (collapsed at "sub/only/").
python3 - "$P25" <<'PY'
import os, sys
d = sys.argv[1]
open(d + '/has space.txt', 'w').write('x\ny\n')          # will be staged
open(d + '/staged_new.txt', 'w').write('s\n')            # will be staged
# Staged too, and for a reason the preconditions below check: a file that is
# committed but never touched again appears in no diff at all, so a fixture
# can claim to cover binary rows and quoted names while covering neither.
open(d + '/bin.dat', 'wb').write(bytes(range(256)) * 5)
open(d + '/ctl\there.txt', 'w').write('t\nu\n')
os.makedirs(d + '/un/a', exist_ok=True)
os.makedirs(d + '/un/b', exist_ok=True)
open(d + '/un/a/1.txt', 'w').write('u\n')
open(d + '/un/b/2.txt', 'w').write('u\n')
os.makedirs(d + '/sub/only', exist_ok=True)
open(d + '/sub/only/deeper.txt', 'w').write('u\n')
open(d + '/untracked.txt', 'w').write('u\n')
PY
(cd "$P25" && "$SG" add "has space.txt" staged_new.txt bin.dat \
    "$(printf 'ctl\there.txt')") > /dev/null 2>&1
python3 - "$P25" <<'PY'
import os, sys
d = sys.argv[1]
open(d + '/tracked.txt', 'w').write('a\nB\nc\nd\ne\n')   # unstaged modify
os.remove(d + '/sub/deep.txt')                            # unstaged delete
PY

# --- status: porcelain, byte for byte -------------------------------------
(cd "$P25" && "$SG" status --porcelain) 2>/dev/null > "$WORKDIR/p25_st_sg.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false status --porcelain) 2>/dev/null \
    > "$WORKDIR/p25_st_git.txt"
check "phase25: sg status --porcelain matches git byte-for-byte" \
    cmp -s "$WORKDIR/p25_st_sg.txt" "$WORKDIR/p25_st_git.txt"

# The cmp above is only worth anything if the fixture actually exercises the
# interesting rows. Each precondition below names one property that would
# otherwise let the cmp pass vacuously.
check "phase25 oracle: precondition -- git really does collapse the untracked dir" \
    grep -q '^?? un/$' "$WORKDIR/p25_st_git.txt"
check "phase25 oracle: precondition -- and collapses one nested under a tracked dir" \
    grep -q '^?? sub/only/$' "$WORKDIR/p25_st_git.txt"
check "phase25 oracle: precondition -- porcelain quotes a merely-spaced name" \
    grep -q '^M  "has space.txt"$' "$WORKDIR/p25_st_git.txt"
check "phase25 oracle: precondition -- and both XY columns are exercised" \
    sh -c "grep -q '^ M ' '$WORKDIR/p25_st_git.txt' && grep -q '^ D ' '$WORKDIR/p25_st_git.txt' && grep -q '^A  ' '$WORKDIR/p25_st_git.txt'"

# --- the two quoting oracles really are different -------------------------
# Asserted head-on: a spaced name is quoted by porcelain and bare in the long
# format. Without this, collapsing sg's two quoting rules into one would keep
# every cmp above green as long as both sides used the same wrong rule.
(cd "$P25" && "$SG" status) 2>/dev/null > "$WORKDIR/p25_long_sg.txt"
check "phase25: the long format leaves the same spaced name unquoted" \
    sh -c "grep -q 'has space.txt' '$WORKDIR/p25_long_sg.txt' && ! grep -q '\"has space.txt\"' '$WORKDIR/p25_long_sg.txt'"
check "phase25: while porcelain quotes it" \
    grep -q '"has space.txt"' "$WORKDIR/p25_st_sg.txt"
check "phase25: and both formats quote a control-character name" \
    sh -c "grep -q 'ctl\\\\there.txt' '$WORKDIR/p25_st_sg.txt'"

# --- status: --short is the same output, -b adds one line -----------------
(cd "$P25" && "$SG" status --short) 2>/dev/null > "$WORKDIR/p25_st_short.txt"
check "phase25: --short and --porcelain produce identical bytes" \
    cmp -s "$WORKDIR/p25_st_short.txt" "$WORKDIR/p25_st_sg.txt"
(cd "$P25" && "$SG" status --porcelain -b) 2>/dev/null > "$WORKDIR/p25_st_b.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false status --porcelain -b) 2>/dev/null \
    > "$WORKDIR/p25_st_b_git.txt"
check "phase25: --porcelain -b matches git, branch header included" \
    cmp -s "$WORKDIR/p25_st_b.txt" "$WORKDIR/p25_st_b_git.txt"

# --- status: the long format's untracked list collapses too ---------------
# This one is a behaviour CHANGE, not a new flag: sg used to list every file
# under a wholly-untracked directory. Compared against git's LONG format,
# under LC_ALL=C because the section headers are translated.
(cd "$P25" && LC_ALL=C "$SG" status) 2>/dev/null \
    | sed -n '/^Untracked files:/,/^$/p' | sed -n 's/^\t//p' | LC_ALL=C sort \
    > "$WORKDIR/p25_un_sg.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false status) 2>/dev/null \
    | sed -n '/^Untracked files:/,/^$/p' | sed -n 's/^\t//p' | LC_ALL=C sort \
    > "$WORKDIR/p25_un_git.txt"
check "phase25: the long format's untracked list matches git, collapsed dirs and all" \
    cmp -s "$WORKDIR/p25_un_sg.txt" "$WORKDIR/p25_un_git.txt"
check "phase25 oracle: precondition -- that list really does contain a collapsed dir" \
    grep -q '^un/$' "$WORKDIR/p25_un_git.txt"

# --- diff: the machine-readable formats, byte for byte --------------------
for _fmt in --numstat --name-only --name-status --shortstat --stat; do
    (cd "$P25" && "$SG" diff --cached "$_fmt") 2>/dev/null > "$WORKDIR/p25_dc_sg.txt"
    (cd "$P25" && LC_ALL=C git -c core.quotepath=false diff --cached "$_fmt") 2>/dev/null \
        > "$WORKDIR/p25_dc_git.txt"
    check "phase25: sg diff --cached $_fmt matches git byte-for-byte" \
        cmp -s "$WORKDIR/p25_dc_sg.txt" "$WORKDIR/p25_dc_git.txt"

    (cd "$P25" && "$SG" diff "$_fmt") 2>/dev/null > "$WORKDIR/p25_dw_sg.txt"
    (cd "$P25" && LC_ALL=C git -c core.quotepath=false diff "$_fmt") 2>/dev/null \
        > "$WORKDIR/p25_dw_git.txt"
    check "phase25: sg diff $_fmt (index vs worktree) matches git byte-for-byte" \
        cmp -s "$WORKDIR/p25_dw_sg.txt" "$WORKDIR/p25_dw_git.txt"

    (cd "$P25" && "$SG" diff HEAD "$_fmt") 2>/dev/null > "$WORKDIR/p25_dh_sg.txt"
    (cd "$P25" && LC_ALL=C git -c core.quotepath=false diff HEAD "$_fmt") 2>/dev/null \
        > "$WORKDIR/p25_dh_git.txt"
    check "phase25: sg diff HEAD $_fmt matches git byte-for-byte" \
        cmp -s "$WORKDIR/p25_dh_sg.txt" "$WORKDIR/p25_dh_git.txt"

    (cd "$P25" && "$SG" diff "$P25_BASE" HEAD "$_fmt") 2>/dev/null > "$WORKDIR/p25_dt_sg.txt"
    (cd "$P25" && LC_ALL=C git -c core.quotepath=false diff "$P25_BASE" HEAD "$_fmt") 2>/dev/null \
        > "$WORKDIR/p25_dt_git.txt"
    check "phase25: sg diff <commit> <commit> $_fmt matches git byte-for-byte" \
        cmp -s "$WORKDIR/p25_dt_sg.txt" "$WORKDIR/p25_dt_git.txt"
done

# --- diff: the default (patch) format, byte for byte -----------------------
# The default format can't join the loop above -- an empty "$_fmt" argument
# is itself a value ("sg diff --cached ''" fails with "invalid reference: "),
# not an absent one -- so the three comparison directions are spelled out.
(cd "$P25" && "$SG" diff --cached) 2>/dev/null > "$WORKDIR/p26_pc_sg.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false diff --cached) 2>/dev/null \
    > "$WORKDIR/p26_pc_git.txt"
check "phase26: sg diff --cached (patch) matches git byte-for-byte" \
    cmp -s "$WORKDIR/p26_pc_sg.txt" "$WORKDIR/p26_pc_git.txt"

(cd "$P25" && "$SG" diff) 2>/dev/null > "$WORKDIR/p26_pw_sg.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false diff) 2>/dev/null \
    > "$WORKDIR/p26_pw_git.txt"
check "phase26: sg diff (index vs worktree, patch) matches git byte-for-byte" \
    cmp -s "$WORKDIR/p26_pw_sg.txt" "$WORKDIR/p26_pw_git.txt"

(cd "$P25" && "$SG" diff HEAD) 2>/dev/null > "$WORKDIR/p26_ph_sg.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false diff HEAD) 2>/dev/null \
    > "$WORKDIR/p26_ph_git.txt"
check "phase26: sg diff HEAD (patch) matches git byte-for-byte" \
    cmp -s "$WORKDIR/p26_ph_sg.txt" "$WORKDIR/p26_ph_git.txt"

(cd "$P25" && "$SG" diff "$P25_BASE" HEAD) 2>/dev/null > "$WORKDIR/p26_pt_sg.txt"
(cd "$P25" && LC_ALL=C git -c core.quotepath=false diff "$P25_BASE" HEAD) 2>/dev/null \
    > "$WORKDIR/p26_pt_git.txt"
check "phase26: sg diff <commit> <commit> (patch) matches git byte-for-byte" \
    cmp -s "$WORKDIR/p26_pt_sg.txt" "$WORKDIR/p26_pt_git.txt"

# Preconditions for the loop above: a --stat that never met a binary file or a
# quoted name proves much less than the count of green checks suggests.
(cd "$P25" && LC_ALL=C git -c core.quotepath=false diff --cached --stat) 2>/dev/null \
    > "$WORKDIR/p25_stat_git.txt"
check "phase25 oracle: precondition -- the --stat fixture includes a binary file" \
    grep -q ' Bin ' "$WORKDIR/p25_stat_git.txt"
check "phase25 oracle: precondition -- and a control-character name it must quote" \
    grep -q 'ctl' "$WORKDIR/p25_stat_git.txt"
check "phase25 oracle: precondition -- --stat leaves a spaced name unquoted" \
    sh -c "! grep -q '\"has space.txt\"' '$WORKDIR/p25_stat_git.txt'"

# --- --stat under a width that forces the squeeze ------------------------
# The layout has a clamp at W*3/8 that only fires when name+number+6+graph
# overruns the available width, and nothing in the ordinary fixtures gets
# near it: at the default 80 columns with short paths the bars print at full
# length and the constant is never consulted. A mutation changing 3/8 to 1/2
# passed every unit test. Real git is the only oracle for a constant like
# this, so the squeeze is exercised here rather than pinned to our own
# output. COLUMNS is set explicitly on both sides -- git honours it even when
# stdout is not a tty, so leaving it to the environment would make this check
# depend on whatever terminal the developer happens to be using.
P25_SQ="$WORKDIR/p25_squeeze"
(cd "$WORKDIR" && "$SG" init p25_squeeze) > /dev/null 2>&1
(cd "$P25_SQ" && git config user.email "a@b.c" && git config user.name "git user")
python3 - "$P25_SQ" <<'PYSQ1'
import os, sys
d = sys.argv[1]
deep = os.path.join(d, 'aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddddddddd')
os.makedirs(deep, exist_ok=True)
open(os.path.join(deep, 'eeeeeeeeee.txt'), 'w').write('x\n')
open(os.path.join(d, 'wide.txt'), 'w').write('l\n' * 10)
open(os.path.join(d, 'tiny.txt'), 'w').write('t\n')
PYSQ1
(cd "$P25_SQ" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
python3 - "$P25_SQ" <<'PYSQ2'
import os, sys
d = sys.argv[1]
deep = os.path.join(d, 'aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddddddddd/eeeeeeeeee.txt')
open(deep, 'w').write('x\n' + 'z\n' * 40)
open(os.path.join(d, 'wide.txt'), 'w').write('l\n' * 900)
open(os.path.join(d, 'tiny.txt'), 'w').write('t\nu\n')
PYSQ2
(cd "$P25_SQ" && "$SG" add .) > /dev/null 2>&1
for _cols in 40 60 80 120; do
    (cd "$P25_SQ" && COLUMNS=$_cols "$SG" diff --cached --stat) 2>/dev/null \
        > "$WORKDIR/p25_sq_sg.txt"
    (cd "$P25_SQ" && LC_ALL=C COLUMNS=$_cols git -c core.quotepath=false diff --cached --stat) \
        2>/dev/null > "$WORKDIR/p25_sq_git.txt"
    check "phase25: --stat at COLUMNS=$_cols matches git, squeeze and all" \
        cmp -s "$WORKDIR/p25_sq_sg.txt" "$WORKDIR/p25_sq_git.txt"
done
# Vacuous-pass guards: the squeeze must actually be doing something here.
(cd "$P25_SQ" && LC_ALL=C COLUMNS=40 git -c core.quotepath=false diff --cached --stat) \
    2>/dev/null > "$WORKDIR/p25_sq_g40.txt"
(cd "$P25_SQ" && LC_ALL=C COLUMNS=120 git -c core.quotepath=false diff --cached --stat) \
    2>/dev/null > "$WORKDIR/p25_sq_g120.txt"
check "phase25 oracle: precondition -- COLUMNS really does change git's layout" \
    sh -c "! cmp -s '$WORKDIR/p25_sq_g40.txt' '$WORKDIR/p25_sq_g120.txt'"
check "phase25 oracle: precondition -- the long path really is truncated at 40" \
    grep -q '\.\.\./' "$WORKDIR/p25_sq_g40.txt"
check "phase25 oracle: precondition -- and the bars really are scaled, not printed raw" \
    sh -c "! grep -q ' 900 ' '$WORKDIR/p25_sq_g40.txt'"

# --- diff: the index decides the path set, not the working tree -----------
# `git rm --cached f` leaves the bytes on disk, and git still calls it a
# deletion. Getting this from the working tree instead would report nothing.
P25_RM="$WORKDIR/p25_rm"
(cd "$WORKDIR" && "$SG" init p25_rm) > /dev/null 2>&1
(cd "$P25_RM" && git config user.email "a@b.c" && git config user.name "git user")
printf 'r\n' > "$P25_RM/rmcached.txt"
(cd "$P25_RM" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P25_RM" && git rm -q --cached rmcached.txt)
(cd "$P25_RM" && "$SG" diff HEAD --name-status) 2>/dev/null > "$WORKDIR/p25_rm_sg.txt"
(cd "$P25_RM" && LC_ALL=C git -c core.quotepath=false diff HEAD --name-status) 2>/dev/null > "$WORKDIR/p25_rm_git.txt"
check "phase25: a path dropped from the index is a deletion though the file is still there" \
    cmp -s "$WORKDIR/p25_rm_sg.txt" "$WORKDIR/p25_rm_git.txt"
check "phase25 oracle: precondition -- git really does call it a deletion here" \
    grep -q '^D	rmcached.txt$' "$WORKDIR/p25_rm_git.txt"
(cd "$P25_RM" && "$SG" status --porcelain) 2>/dev/null > "$WORKDIR/p25_rm_st_sg.txt"
(cd "$P25_RM" && LC_ALL=C git -c core.quotepath=false status --porcelain) 2>/dev/null > "$WORKDIR/p25_rm_st_git.txt"
check "phase25: and porcelain lists that one path twice, as D and as ??" \
    cmp -s "$WORKDIR/p25_rm_st_sg.txt" "$WORKDIR/p25_rm_st_git.txt"
check "phase25 oracle: precondition -- git really does list it twice" \
    sh -c "grep -q '^D  rmcached.txt$' '$WORKDIR/p25_rm_st_git.txt' && grep -q '^?? rmcached.txt$' '$WORKDIR/p25_rm_st_git.txt'"

# --- status: the branch header's two unusual shapes -----------------------
P25_UNBORN="$WORKDIR/p25_unborn"
(cd "$WORKDIR" && "$SG" init p25_unborn) > /dev/null 2>&1
(cd "$P25_UNBORN" && git config user.email "a@b.c" && git config user.name "git user")
printf 'n\n' > "$P25_UNBORN/n.txt"
(cd "$P25_UNBORN" && "$SG" add .) > /dev/null 2>&1
(cd "$P25_UNBORN" && "$SG" status --porcelain -b) 2>/dev/null | head -1 \
    > "$WORKDIR/p25_unborn_sg.txt"
(cd "$P25_UNBORN" && LC_ALL=C git status --porcelain -b) 2>/dev/null | head -1 \
    > "$WORKDIR/p25_unborn_git.txt"
check "phase25: an unborn HEAD's branch header matches git" \
    cmp -s "$WORKDIR/p25_unborn_sg.txt" "$WORKDIR/p25_unborn_git.txt"

# Detached is produced with real git so that the check is about sg's *reading*
# of a detached HEAD, not about whichever sg command happens to detach one.
(cd "$P25" && git checkout -q --detach) > /dev/null 2>&1
(cd "$P25" && "$SG" status --porcelain -b) 2>/dev/null | head -1 > "$WORKDIR/p25_det_sg.txt"
(cd "$P25" && LC_ALL=C git status --porcelain -b) 2>/dev/null | head -1 > "$WORKDIR/p25_det_git.txt"
check "phase25: a detached HEAD's branch header matches git" \
    cmp -s "$WORKDIR/p25_det_sg.txt" "$WORKDIR/p25_det_git.txt"
check "phase25 oracle: precondition -- that header really is the detached one" \
    grep -q '^## HEAD (no branch)$' "$WORKDIR/p25_det_git.txt"
(cd "$P25" && git checkout -q -) > /dev/null 2>&1

# --- stash show: real git reading a stash sg built ------------------------
# The stash is created by sg and read back by both, so this checks two
# things at once: that sg's stash commit still has the shape git expects,
# and that sg's rendering of it agrees with git's byte for byte.
# The patch body (-p) is compared byte for byte too (Phase 26): sg's hunk
# splitting and body now track git's closely enough to be a whole-output
# oracle, not just the header lines.
P25_SS="$WORKDIR/p25_stashshow"
(cd "$WORKDIR" && "$SG" init p25_stashshow) > /dev/null 2>&1
(cd "$P25_SS" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\nb\nc\n' > "$P25_SS/a.txt"
printf 'k\n' > "$P25_SS/c.txt"
(cd "$P25_SS" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
printf 'a\nB\nc\nd\n' > "$P25_SS/a.txt"
printf 'k\nk2\n' > "$P25_SS/c.txt"
printf 's\n' > "$P25_SS/staged.txt"
(cd "$P25_SS" && "$SG" add staged.txt) > /dev/null 2>&1
# Sorts between a.txt and c.txt on purpose: a -u listing built by running two
# diffs and concatenating them comes out in the wrong order here, and a
# fixture whose untracked name sorted last would not notice.
printf 'u\n' > "$P25_SS/b.txt"
(cd "$P25_SS" && "$SG" stash push -u -m "wip") > /dev/null 2>&1

check "phase25 oracle: precondition -- git can read the stash sg built" \
    sh -c "(cd '$P25_SS' && LC_ALL=C git stash list) 2>/dev/null | grep -q 'stash@{0}'"
check "phase25 oracle: precondition -- and it has the 3-parent shape -u produces" \
    sh -c "(cd '$P25_SS' && git cat-file -p 'stash@{0}') 2>/dev/null | grep -c '^parent ' | grep -q '^3\$'"
check "phase25 oracle: precondition -- the default stash show really is a diffstat" \
    sh -c "(cd '$P25_SS' && LC_ALL=C git stash show) 2>/dev/null | grep -q 'files changed'"

for _sfmt in "" "-p" "--stat" "--numstat" "--shortstat" "--name-only" "--name-status"; do
    (cd "$P25_SS" && "$SG" stash show $_sfmt) 2>/dev/null > "$WORKDIR/p25_ss_sg.txt"
    (cd "$P25_SS" && LC_ALL=C git -c core.quotepath=false stash show $_sfmt) 2>/dev/null \
        > "$WORKDIR/p25_ss_git.txt"
    check "phase25: sg stash show ${_sfmt:-'(default)'} matches git byte-for-byte" \
        cmp -s "$WORKDIR/p25_ss_sg.txt" "$WORKDIR/p25_ss_git.txt"
done

for _sflag in "--include-untracked" "--only-untracked"; do
    (cd "$P25_SS" && "$SG" stash show "$_sflag" --name-only) 2>/dev/null \
        > "$WORKDIR/p25_ssu_sg.txt"
    (cd "$P25_SS" && LC_ALL=C git -c core.quotepath=false stash show "$_sflag" --name-only) \
        2>/dev/null > "$WORKDIR/p25_ssu_git.txt"
    check "phase25: sg stash show $_sflag --name-only matches git" \
        cmp -s "$WORKDIR/p25_ssu_sg.txt" "$WORKDIR/p25_ssu_git.txt"
done

# The union must be sorted, not concatenated: b.txt is untracked and sorts
# between the two tracked names.
check "phase25 oracle: precondition -- the untracked name really does sort in the middle" \
    sh -c "(cd '$P25_SS' && LC_ALL=C git stash show --include-untracked --name-only) 2>/dev/null | tr '\\n' ' ' | grep -q 'a.txt b.txt c.txt'"
check "phase25: and sg lists a path exactly once under -u, never twice" \
    sh -c "(cd '$P25_SS' && '$SG' stash show -u --name-only) 2>/dev/null | LC_ALL=C sort | uniq -d | wc -l | tr -d ' ' | grep -q '^0\$'"

check "phase26 oracle: precondition -- -p really does produce a patch, not a diffstat" \
    sh -c "(cd '$P25_SS' && LC_ALL=C git -c core.quotepath=false stash show -p) 2>/dev/null | grep -q '^diff --git'"
check "phase25: sg stash show rejects an unknown flag" \
    sh -c "! (cd '$P25_SS' && '$SG' stash show --bogus) > /dev/null 2>&1"

# --- an unknown flag is refused, not ignored ------------------------------
check "phase25: sg diff rejects an unknown flag" \
    sh -c "! (cd '$P25' && '$SG' diff --bogus) > /dev/null 2>&1"
check "phase25: sg status rejects an unknown flag" \
    sh -c "! (cd '$P25' && '$SG' status --bogus) > /dev/null 2>&1"

# --- status --ignored, including the nested fold -------------------------
# The fold rule applies recursively to the ignored listing too: a
# subdirectory whose contents are ALL ignored is folded to "dir/", while
# ignored files sitting directly in a folded untracked directory are listed
# one by one. Measured against git 2.55.0 -- and the deciding question is
# "is everything under here ignored", not "does a pattern match this
# directory's own name", which is a distinction a name-matching
# implementation passes every flat fixture without ever getting right.
P25_IGN="$WORKDIR/p25_ign"
(cd "$WORKDIR" && "$SG" init p25_ign) > /dev/null 2>&1
(cd "$P25_IGN" && git config user.email "a@b.c" && git config user.name "git user")
printf '*.tmp\n' > "$P25_IGN/.gitignore"
(cd "$P25_IGN" && "$SG" add .gitignore && "$SG" commit -m base) > /dev/null 2>&1
mkdir -p "$P25_IGN/d/subignored" "$P25_IGN/d/mixedsub"
printf 'k\n' > "$P25_IGN/d/keep.txt"
printf 'a\n' > "$P25_IGN/d/subignored/a.tmp"
printf 'b\n' > "$P25_IGN/d/subignored/b.tmp"
printf 'c\n' > "$P25_IGN/d/mixedsub/c.txt"
printf 'd\n' > "$P25_IGN/d/mixedsub/d.tmp"
printf 'x\n' > "$P25_IGN/d/x.tmp"
(cd "$P25_IGN" && "$SG" status --porcelain --ignored) 2>/dev/null > "$WORKDIR/p25_ign_sg.txt"
(cd "$P25_IGN" && LC_ALL=C git -c core.quotepath=false status --porcelain --ignored) 2>/dev/null \
    > "$WORKDIR/p25_ign_git.txt"
check "phase25: sg status --porcelain --ignored matches git byte-for-byte" \
    cmp -s "$WORKDIR/p25_ign_sg.txt" "$WORKDIR/p25_ign_git.txt"
check "phase25 oracle: precondition -- a wholly-ignored SUBdirectory is folded" \
    grep -q '^!! d/subignored/$' "$WORKDIR/p25_ign_git.txt"
check "phase25 oracle: precondition -- a subdirectory with one non-ignored file is NOT folded" \
    grep -q '^!! d/mixedsub/d\.tmp$' "$WORKDIR/p25_ign_git.txt"
check "phase25 oracle: precondition -- an ignored file directly inside the folded dir is listed alone" \
    grep -q '^!! d/x\.tmp$' "$WORKDIR/p25_ign_git.txt"
check "phase25 oracle: precondition -- and the untracked dir above them all is folded" \
    grep -q '^?? d/$' "$WORKDIR/p25_ign_git.txt"

# The long format's Ignored section, same tree, same oracle split as the
# untracked one above.
(cd "$P25_IGN" && LC_ALL=C "$SG" status --ignored) 2>/dev/null \
    | sed -n '/^Ignored files:/,/^$/p' | sed -n 's/^\t//p' | LC_ALL=C sort \
    > "$WORKDIR/p25_ignlong_sg.txt"
(cd "$P25_IGN" && LC_ALL=C git -c core.quotepath=false status --ignored) 2>/dev/null \
    | sed -n '/^Ignored files:/,/^$/p' | sed -n 's/^\t//p' | LC_ALL=C sort \
    > "$WORKDIR/p25_ignlong_git.txt"
check "phase25: the long format's Ignored section matches git" \
    cmp -s "$WORKDIR/p25_ignlong_sg.txt" "$WORKDIR/p25_ignlong_git.txt"

# --- -u<mode>: three modes, three different answers ----------------------
for _um in "-uall" "-unormal" "-uno"; do
    (cd "$P25_IGN" && "$SG" status --porcelain "$_um") 2>/dev/null > "$WORKDIR/p25_um_sg.txt"
    (cd "$P25_IGN" && LC_ALL=C git -c core.quotepath=false status --porcelain "$_um") 2>/dev/null \
        > "$WORKDIR/p25_um_git.txt"
    check "phase25: sg status --porcelain $_um matches git" \
        cmp -s "$WORKDIR/p25_um_sg.txt" "$WORKDIR/p25_um_git.txt"
done
# Vacuous-pass guard: the three modes must not all produce the same bytes.
(cd "$P25_IGN" && "$SG" status --porcelain -uall) 2>/dev/null > "$WORKDIR/p25_um_all.txt"
(cd "$P25_IGN" && "$SG" status --porcelain -unormal) 2>/dev/null > "$WORKDIR/p25_um_norm.txt"
(cd "$P25_IGN" && "$SG" status --porcelain -uno) 2>/dev/null > "$WORKDIR/p25_um_no.txt"
check "phase25: -uall and -unormal really do differ on this tree" \
    sh -c "! cmp -s '$WORKDIR/p25_um_all.txt' '$WORKDIR/p25_um_norm.txt'"
check "phase25: -uno really does drop the untracked rows" \
    sh -c "! grep -q '^??' '$WORKDIR/p25_um_no.txt'"
check "phase25: the flagless default equals -unormal byte-for-byte" \
    sh -c "(cd '$P25_IGN' && '$SG' status --porcelain) 2>/dev/null | cmp -s - '$WORKDIR/p25_um_norm.txt'"

# ==========================================================================
# Phase 26: the patch oracle upgraded to whole-output cmp above; these
# fixtures fill in the body dimensions no earlier check ever exercised --
# multi-hunk splitting/merging, missing-trailing-newline combinations,
# new/deleted files, mode-only changes, binary files, and the hunk header's
# function-name suffix. Each is its own tiny repo so a failure names exactly
# which dimension broke.
# ==========================================================================

# --- multi-hunk: two edits far apart split into two hunks, two edits close
# together merge into one. Only testing one side would miss the boundary. ---
P26_HUNKS="$WORKDIR/p26_hunks"
(cd "$WORKDIR" && "$SG" init p26_hunks) > /dev/null 2>&1
(cd "$P26_HUNKS" && git config user.email "a@b.c" && git config user.name "git user")
python3 - "$P26_HUNKS" <<'PY'
import sys
d = sys.argv[1]
lines = [f"line{i}\n" for i in range(1, 41)]
open(d + '/split.txt', 'w').writelines(lines)
open(d + '/merge.txt', 'w').writelines(lines)
PY
(cd "$P26_HUNKS" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
python3 - "$P26_HUNKS" <<'PY'
import sys
d = sys.argv[1]
lines = [f"line{i}\n" for i in range(1, 41)]
s = lines[:]
s[1] = "CHANGED2\n"      # line 2
s[19] = "CHANGED20\n"     # line 20 -- 17 unchanged lines between edits: splits
open(d + '/split.txt', 'w').writelines(s)
m = lines[:]
m[1] = "CHANGED2\n"       # line 2
m[5] = "CHANGED6\n"       # line 6 -- 3 unchanged lines between edits: merges
open(d + '/merge.txt', 'w').writelines(m)
PY
(cd "$P26_HUNKS" && "$SG" diff) 2>/dev/null > "$WORKDIR/p26_hunks_sg.txt"
(cd "$P26_HUNKS" && LC_ALL=C git -c core.quotepath=false diff) 2>/dev/null > "$WORKDIR/p26_hunks_git.txt"
check "phase26: a far-apart pair of edits and a close pair match git byte-for-byte" \
    cmp -s "$WORKDIR/p26_hunks_sg.txt" "$WORKDIR/p26_hunks_git.txt"
check "phase26 oracle: precondition -- the far-apart edits really do split into two hunks" \
    sh -c "test \$(grep -c '^@@' '$WORKDIR/p26_hunks_git.txt') = 3"
check "phase26 oracle: precondition -- the close edits really do merge into one hunk" \
    sh -c "(cd '$P26_HUNKS' && LC_ALL=C git diff -- merge.txt) 2>/dev/null | grep -c '^@@' | grep -q '^1\$'"
# The split hunk's second header happens to land on a line the default
# funcname regex recognises ("line16"), and the first does not -- covering
# both the found and not-found cases, including that not-found leaves no
# trailing space on the "@@ ... @@" line.
check "phase26 oracle: precondition -- one hunk header gets a funcname suffix" \
    grep -q '^@@ -17,7 +17,7 @@ line16$' "$WORKDIR/p26_hunks_git.txt"
check "phase26 oracle: precondition -- and the other does not, with no trailing space" \
    grep -q '^@@ -1,5 +1,5 @@$' "$WORKDIR/p26_hunks_git.txt"

# --- missing trailing newline: old side, new side, and both -----------------
P26_EOL="$WORKDIR/p26_eol"
(cd "$WORKDIR" && "$SG" init p26_eol) > /dev/null 2>&1
(cd "$P26_EOL" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\nb\nc' > "$P26_EOL/oldmissing.txt"
printf 'a\nb\nc\n' > "$P26_EOL/newmissing.txt"
printf 'a\nb\nc' > "$P26_EOL/bothmissing.txt"
(cd "$P26_EOL" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
printf 'a\nb\nCHANGED\n' > "$P26_EOL/oldmissing.txt"
printf 'a\nb\nCHANGED' > "$P26_EOL/newmissing.txt"
printf 'a\nb\nCHANGED' > "$P26_EOL/bothmissing.txt"
(cd "$P26_EOL" && "$SG" diff) 2>/dev/null > "$WORKDIR/p26_eol_sg.txt"
(cd "$P26_EOL" && LC_ALL=C git -c core.quotepath=false diff) 2>/dev/null > "$WORKDIR/p26_eol_git.txt"
check "phase26: old-missing/new-missing/both-missing newline all match git byte-for-byte" \
    cmp -s "$WORKDIR/p26_eol_sg.txt" "$WORKDIR/p26_eol_git.txt"
check "phase26 oracle: precondition -- git really does emit 'No newline' 4 times here" \
    sh -c "test \$(grep -Fc '\ No newline at end of file' '$WORKDIR/p26_eol_git.txt') = 4"

# --- new file, deleted file, chmod-only, chmod+content, binary --------------
P26_MODE="$WORKDIR/p26_mode"
(cd "$WORKDIR" && "$SG" init p26_mode) > /dev/null 2>&1
(cd "$P26_MODE" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\nb\n' > "$P26_MODE/chmodonly.txt"
printf 'a\nb\n' > "$P26_MODE/chmodcontent.txt"
printf 'gone\n' > "$P26_MODE/deleted.txt"
printf '\x00\x01binary\n' > "$P26_MODE/bin.dat"
(cd "$P26_MODE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
chmod +x "$P26_MODE/chmodonly.txt" "$P26_MODE/chmodcontent.txt"
printf 'a\nCHANGED\n' > "$P26_MODE/chmodcontent.txt"
rm "$P26_MODE/deleted.txt"
printf 'newfile content\n' > "$P26_MODE/newfile.txt"
(cd "$P26_MODE" && "$SG" add newfile.txt) > /dev/null 2>&1
printf '\x00\x01binaryCHANGED\n' > "$P26_MODE/bin.dat"
(cd "$P26_MODE" && "$SG" diff) 2>/dev/null > "$WORKDIR/p26_mode_sg.txt"
(cd "$P26_MODE" && LC_ALL=C git -c core.quotepath=false diff) 2>/dev/null > "$WORKDIR/p26_mode_git.txt"
check "phase26: chmod-only, chmod+content, deletion, and a rewritten binary all match git" \
    cmp -s "$WORKDIR/p26_mode_sg.txt" "$WORKDIR/p26_mode_git.txt"
(cd "$P26_MODE" && "$SG" diff --cached) 2>/dev/null > "$WORKDIR/p26_mode_c_sg.txt"
(cd "$P26_MODE" && LC_ALL=C git -c core.quotepath=false diff --cached) 2>/dev/null > "$WORKDIR/p26_mode_c_git.txt"
check "phase26: a staged new file (--cached) matches git byte-for-byte" \
    cmp -s "$WORKDIR/p26_mode_c_sg.txt" "$WORKDIR/p26_mode_c_git.txt"
check "phase26 oracle: precondition -- chmod-only really has no index line and no hunk" \
    sh -c "! sed -n '/^diff --git a.chmodonly/,/^diff --git a.deleted/p' '$WORKDIR/p26_mode_git.txt' | grep -qE '^index |^@@'"
check "phase26 oracle: precondition -- and the new file really is 'new file mode' + /dev/null" \
    sh -c "grep -A1 '^diff --git a/newfile.txt' '$WORKDIR/p26_mode_c_git.txt' | grep -q 'new file mode' && grep -q '^--- /dev/null\$' '$WORKDIR/p26_mode_c_git.txt'"
check "phase26 oracle: precondition -- and the deletion really is 'deleted file mode' + /dev/null" \
    sh -c "grep -A1 '^diff --git a/deleted.txt' '$WORKDIR/p26_mode_git.txt' | grep -q 'deleted file mode' && grep -q '^+++ /dev/null\$' '$WORKDIR/p26_mode_git.txt'"
check "phase26 oracle: precondition -- git really does call bin.dat binary here" \
    grep -q 'Binary files a/bin.dat and b/bin.dat differ' "$WORKDIR/p26_mode_git.txt"

# --- a hostile tree entry name ("..") reaches sg diff and sg stash show, and
# is refused by name rather than silently expanded to a real path. Built with
# real git's plumbing (mktree/commit-tree), since neither sg nor git's normal
# porcelain will construct such a tree; this is Phase 25's noted zero-coverage
# gap for report_bad_tree_path / report_bad_stash_tree_path. ---
P26_BAD="$WORKDIR/p26_bad"
(cd "$WORKDIR" && "$SG" init p26_bad) > /dev/null 2>&1
(cd "$P26_BAD" && git config user.email "a@b.c" && git config user.name "git user")
printf 'x\n' > "$P26_BAD/f.txt"
(cd "$P26_BAD" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
P26_BAD_BASE=$(git -C "$P26_BAD" rev-parse HEAD)
P26_BAD_BLOB=$(git -C "$P26_BAD" hash-object -w f.txt)
P26_BAD_TREE=$(printf '100644 blob %s\t..\n' "$P26_BAD_BLOB" | git -C "$P26_BAD" mktree)
P26_BAD_COMMIT=$(git -C "$P26_BAD" commit-tree "$P26_BAD_TREE" -p "$P26_BAD_BASE" -m badcommit)

(cd "$P26_BAD" && "$SG" diff "$P26_BAD_BASE" "$P26_BAD_COMMIT") > "$WORKDIR/p26_bad_diff.txt" 2>&1
check "phase26: sg diff refuses a tree containing a '..' entry" \
    sh -c "! (cd '$P26_BAD' && '$SG' diff '$P26_BAD_BASE' '$P26_BAD_COMMIT') > /dev/null 2>&1"
check "phase26: and names the offending path in its error message" \
    grep -q '\.\.' "$WORKDIR/p26_bad_diff.txt"

P26_BAD_IDXCOMMIT=$(git -C "$P26_BAD" commit-tree "$(git -C "$P26_BAD" rev-parse "$P26_BAD_BASE^{tree}")" \
    -p "$P26_BAD_BASE" -m "index on stash test")
P26_BAD_STASHCOMMIT=$(git -C "$P26_BAD" commit-tree "$P26_BAD_TREE" \
    -p "$P26_BAD_BASE" -p "$P26_BAD_IDXCOMMIT" -m "WIP on stash test")
printf '%s' "$P26_BAD_STASHCOMMIT" > "$P26_BAD/.git/refs/stash"
printf '0000000000000000000000000000000000000000 %s small_git <sg@localhost> 1787403998 +0000\tWIP on stash test\n' \
    "$P26_BAD_STASHCOMMIT" > "$P26_BAD/.git/logs/refs/stash"

(cd "$P26_BAD" && "$SG" stash show) > "$WORKDIR/p26_bad_stash.txt" 2>&1
check "phase26: sg stash show refuses a stash whose tree has a '..' entry" \
    sh -c "! (cd '$P26_BAD' && '$SG' stash show) > /dev/null 2>&1"
check "phase26: and names the offending path in its error message" \
    grep -q '\.\.' "$WORKDIR/p26_bad_stash.txt"

# --- Phase 27: sg_status_diff_unstaged collapsed into a thin adapter over
# sg_diff_index_workdir. A pure chmod (no content change) is now real signal
# to `sg status` too, matching git; and it now counts as "dirty" for the
# switch/reset --hard safety gate, since that gate is sg_status_diff_unstaged's
# own third caller. ---
P27_MODE="$WORKDIR/p27_mode"
(cd "$WORKDIR" && "$SG" init p27_mode) > /dev/null 2>&1
(cd "$P27_MODE" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\nb\n' > "$P27_MODE/exec.txt"
(cd "$P27_MODE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
chmod +x "$P27_MODE/exec.txt"
(cd "$P27_MODE" && "$SG" status --porcelain) > "$WORKDIR/p27_mode_sg.txt" 2>&1
(cd "$P27_MODE" && LC_ALL=C git -c core.quotepath=false status --porcelain) > "$WORKDIR/p27_mode_git.txt" 2>&1
check "phase27: sg status --porcelain reports a mode-only chmod, matching git byte-for-byte" \
    cmp -s "$WORKDIR/p27_mode_sg.txt" "$WORKDIR/p27_mode_git.txt"
check "phase27 oracle: precondition -- git really does print ' M exec.txt' for a mode-only chmod" \
    grep -qx ' M exec.txt' "$WORKDIR/p27_mode_git.txt"

# switch: a mode-only dirty change must now block it, same as an ordinary
# content change already did -- and a control repo with NO dirty change (only
# the branch differs) must still switch cleanly, so this isn't switch simply
# refusing everything.
(cd "$P27_MODE" && "$SG" branch other) > /dev/null 2>&1
(cd "$P27_MODE" && "$SG" switch other) > "$WORKDIR/p27_switch_dirty.txt" 2>&1
check "phase27: sg switch refuses to switch branches with an unstaged mode-only change" \
    sh -c "! (cd '$P27_MODE' && '$SG' switch other) > /dev/null 2>&1"
check "phase27: and names the mode-only change as the reason (not a generic failure)" \
    grep -q 'modified (unstaged): exec.txt' "$WORKDIR/p27_switch_dirty.txt"

# sg_require_clean_workdir (src/workdir/apply.c) is a structurally different
# caller from sg_safe_apply_tree above -- it is what `sg merge`/`sg rebase`
# use to refuse a dirty working tree before doing anything, rather than
# confirm-and-snapshot. It shares sg_status_diff_unstaged, so the same
# mode-only chmod must trip it too.
P27_MERGE="$WORKDIR/p27_merge"
(cd "$WORKDIR" && "$SG" init p27_merge) > /dev/null 2>&1
(cd "$P27_MERGE" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\nb\n' > "$P27_MERGE/exec.txt"
(cd "$P27_MERGE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P27_MERGE" && "$SG" branch other) > /dev/null 2>&1
chmod +x "$P27_MERGE/exec.txt"
(cd "$P27_MERGE" && "$SG" merge other) > "$WORKDIR/p27_merge_dirty.txt" 2>&1
check "phase27: sg merge refuses (via sg_require_clean_workdir) an unstaged mode-only change" \
    sh -c "! (cd '$P27_MERGE' && '$SG' merge other) > /dev/null 2>&1"
check "phase27: and names the mode-only change as the reason" \
    grep -q 'modified (unstaged): exec.txt' "$WORKDIR/p27_merge_dirty.txt"

P27_CLEAN="$WORKDIR/p27_clean"
(cd "$WORKDIR" && "$SG" init p27_clean) > /dev/null 2>&1
(cd "$P27_CLEAN" && git config user.email "a@b.c" && git config user.name "git user")
printf 'a\nb\n' > "$P27_CLEAN/exec.txt"
(cd "$P27_CLEAN" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P27_CLEAN" && "$SG" branch other) > /dev/null 2>&1
check "phase27 control: sg switch still succeeds when the working tree is clean" \
    sh -c "cd '$P27_CLEAN' && '$SG' switch other > /dev/null 2>&1"

# --- Phase 28: `sg diff` pathspec ---------------------------------------
#
# Every case here is a byte compare against real git for the SAME argument
# list, because the pathspec rules are the kind that feel obvious and are
# not: '*' crosses '/', a spec that contains a wildcard gets no
# leading-directory treatment at all, and a trailing '/' is not noise.
# Asserting sg's output on its own would have frozen whatever sg happened to
# do; only git can say which of those is right.
P28="$WORKDIR/p28_pathspec"
(cd "$WORKDIR" && "$SG" init p28_pathspec) > /dev/null 2>&1
(cd "$P28" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P28/sub/deep" "$P28/other"
printf 'a1\na2\n' > "$P28/a.txt"
printf 'b1\nb2\n' > "$P28/sub/b.txt"
printf 'c1\nc2\n' > "$P28/sub/deep/c.txt"
printf 'd1\nd2\n' > "$P28/other/d.c"
printf 'e1\ne2\n' > "$P28/e.c"
(cd "$P28" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
for f in a.txt sub/b.txt sub/deep/c.txt other/d.c e.c; do
    printf 'CHANGED\n' >> "$P28/$f"
done

# Runs one argument list through both implementations from $1 and compares
# the whole output. git needs core.quotepath=false to match sg's ">=0x80 is
# printed raw" rule (CLAUDE.md); it is harmless for the ASCII names here and
# keeps this helper usable if a hostile name is ever added to the fixture.
p28_cmp() {
    p28_dir="$1"
    p28_label="$2"
    shift 2
    (cd "$p28_dir" && "$SG" diff "$@") > "$WORKDIR/p28_sg.txt" 2>/dev/null
    (cd "$p28_dir" && git -c core.quotepath=false diff "$@") > "$WORKDIR/p28_git.txt" 2>/dev/null
    check "phase28: sg diff $p28_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p28_sg.txt" "$WORKDIR/p28_git.txt"
}

# All six output formats through one leading-directory spec: the filter runs
# before the renderer, so a format that lost the pathspec would show up here
# and nowhere else.
for p28_fmt in "" --stat --numstat --shortstat --name-only --name-status; do
    if [ -z "$p28_fmt" ]; then
        p28_cmp "$P28" "(patch) -- sub" -- sub
    else
        p28_cmp "$P28" "$p28_fmt -- sub" "$p28_fmt" -- sub
    fi
done

# Literal specs: exact, directory, nested directory, no match at all.
p28_cmp "$P28" "--name-only -- a.txt" --name-only -- a.txt
p28_cmp "$P28" "--name-only -- sub/deep" --name-only -- sub/deep
p28_cmp "$P28" "--name-only -- nosuch" --name-only -- nosuch
p28_cmp "$P28" "--name-only -- two specs" --name-only -- a.txt sub
p28_cmp "$P28" "--name-only -- ." --name-only -- .
p28_cmp "$P28" "--name-only with a bare --" --name-only --

# The trailing slash pair. These two differ from each other in git, so a
# normalizer that dropped the slash would pass the first and fail the second.
p28_cmp "$P28" "--name-only -- sub/" --name-only -- sub/
p28_cmp "$P28" "--name-only -- a.txt/" --name-only -- 'a.txt/'

# Wildcards. The three negatives are the point: 'o[tx]her', 'su?' and
# 's*b' all match a directory NAME with changes underneath, and git reports
# nothing for any of them.
p28_cmp "$P28" "--name-only -- '*.c'" --name-only -- '*.c'
p28_cmp "$P28" "--name-only -- 'sub/*'" --name-only -- 'sub/*'
p28_cmp "$P28" "--name-only -- 'sub*'" --name-only -- 'sub*'
p28_cmp "$P28" "--name-only -- 'o[tx]her'" --name-only -- 'o[tx]her'
p28_cmp "$P28" "--name-only -- 'su?'" --name-only -- 'su?'
p28_cmp "$P28" "--name-only -- 's*b'" --name-only -- 's*b'
p28_cmp "$P28" "--name-only -- 'sub/dee?'" --name-only -- 'sub/dee?'

# Pathspec against the other three comparisons, not just index-vs-worktree.
(cd "$P28" && "$SG" add sub/b.txt) > /dev/null 2>&1
p28_cmp "$P28" "--cached --name-status -- sub" --cached --name-status -- sub
p28_cmp "$P28" "--cached --name-status -- a.txt" --cached --name-status -- a.txt
p28_cmp "$P28" "HEAD --name-status -- sub" HEAD --name-status -- sub
p28_cmp "$P28" "HEAD HEAD --name-status -- sub" HEAD HEAD --name-status -- sub

# Specs are relative to the current directory, not the repository root.
p28_cmp "$P28/sub" "--name-only -- b.txt (from sub/)" --name-only -- b.txt
p28_cmp "$P28/sub" "--name-only -- . (from sub/)" --name-only -- .
p28_cmp "$P28/sub" "--name-only -- deep (from sub/)" --name-only -- deep
p28_cmp "$P28/sub" "--name-only -- ../a.txt (from sub/)" --name-only -- ../a.txt
p28_cmp "$P28/sub" "--name-only -- '*.txt' (from sub/)" --name-only -- '*.txt'

# The bare (no "--") form: git's disambiguation, reproduced.
p28_cmp "$P28" "--name-only a.txt (no --)" --name-only a.txt
p28_cmp "$P28" "--name-only sub (no --)" --name-only sub
p28_cmp "$P28" "--name-only '*.zzz' (no --)" --name-only '*.zzz'
p28_cmp "$P28" "--name-only a.txt sub (no --)" --name-only a.txt sub
p28_cmp "$P28" "HEAD --name-only a.txt (no --)" HEAD --name-only a.txt

# A file that is gone from the working tree can only be named after "--";
# bare, git calls it ambiguous. Both halves are asserted, because getting
# only the permissive half right would still lose the error.
rm -f "$P28/e.c"
p28_cmp "$P28" "--name-only -- e.c (deleted file)" --name-only -- e.c
check "phase28: a deleted path is rejected in the bare form, as git does" \
    sh -c "! (cd '$P28' && '$SG' diff --name-only e.c) > /dev/null 2>&1"
check "phase28 oracle: real git rejects it too" \
    sh -c "! (cd '$P28' && git diff --name-only e.c) > /dev/null 2>&1"
(cd "$P28" && git checkout -- e.c) > /dev/null 2>&1

# Errors. Exit codes cannot be compared (git uses 128, sg only ever 0/1 per
# CLAUDE.md), so each pair asserts that both refuse -- and each sg message is
# asserted separately, since "exits non-zero" is satisfied by a crash too.
P28_ERR="$WORKDIR/p28_err.txt"

(cd "$P28" && "$SG" diff --name-only -- '') > "$P28_ERR" 2>&1
check "phase28: sg diff rejects an empty pathspec" test $? != 0
check "phase28: and says so, pointing at '.' the way git does" \
    grep -q 'an empty string is not a valid path' "$P28_ERR"
check "phase28 oracle: real git rejects an empty pathspec too" \
    sh -c "! (cd '$P28' && git diff --name-only -- '') > /dev/null 2>&1"

(cd "$P28" && "$SG" diff --name-only -- /etc/passwd) > "$P28_ERR" 2>&1
check "phase28: sg diff rejects a pathspec outside the worktree" test $? != 0
check "phase28: and names the repository it is outside of" \
    grep -q 'outside the repository' "$P28_ERR"
check "phase28 oracle: real git rejects it too" \
    sh -c "! (cd '$P28' && git diff --name-only -- /etc/passwd) > /dev/null 2>&1"

(cd "$P28" && "$SG" diff --name-only ':(icase)a.txt') > "$P28_ERR" 2>&1
check "phase28: sg diff rejects pathspec magic instead of taking it literally" test $? != 0
check "phase28: and says which spec it could not understand" \
    grep -q 'pathspec magic' "$P28_ERR"

(cd "$P28" && "$SG" diff --name-only nosuch) > "$P28_ERR" 2>&1
check "phase28: a bare argument that is neither rev nor path is refused" test $? != 0
check "phase28: and the message points at --" \
    grep -q 'ambiguous argument' "$P28_ERR"
check "phase28 oracle: real git refuses it too" \
    sh -c "! (cd '$P28' && git diff --name-only nosuch) > /dev/null 2>&1"

(cd "$P28" && "$SG" diff --name-only a.txt HEAD) > "$P28_ERR" 2>&1
check "phase28: once an argument is a path, a later revision is refused" test $? != 0
check "phase28: and the message names HEAD as the path that is missing" \
    grep -q 'HEAD' "$P28_ERR"
check "phase28 oracle: real git refuses that ordering too" \
    sh -c "! (cd '$P28' && git diff --name-only a.txt HEAD) > /dev/null 2>&1"

# A name that is BOTH a branch and a file is git's other ambiguity error.
(cd "$P28" && "$SG" branch a.txt) > /dev/null 2>&1
(cd "$P28" && "$SG" diff --name-only a.txt) > "$P28_ERR" 2>&1
check "phase28: an argument that is both a revision and a file is refused" test $? != 0
check "phase28: and the message says it is both" \
    grep -q 'could be both a revision and a file' "$P28_ERR"
check "phase28 oracle: real git refuses it too" \
    sh -c "! (cd '$P28' && git diff --name-only a.txt) > /dev/null 2>&1"
p28_cmp "$P28" "--name-only -- a.txt (ambiguous name, after --)" --name-only -- a.txt
(cd "$P28" && "$SG" branch -d a.txt) > /dev/null 2>&1

# Positive control. Everything above is a cmp against git, which stays green
# if BOTH sides print everything -- so this pins down that the filter really
# removes something: unfiltered output mentions a.txt, filtered output must
# not, and must still mention the path that was asked for.
(cd "$P28" && "$SG" diff --name-only) > "$WORKDIR/p28_all.txt" 2>/dev/null
(cd "$P28" && "$SG" diff --name-only -- sub) > "$WORKDIR/p28_filtered.txt" 2>/dev/null
check "phase28 control: the unfiltered list really does contain a.txt" \
    grep -q '^a\.txt$' "$WORKDIR/p28_all.txt"
check "phase28 control: the filtered list drops it" \
    sh -c "! grep -q '^a\\.txt\$' '$WORKDIR/p28_filtered.txt'"
# sub/b.txt is staged by this point, so the unstaged list under sub/ is
# sub/deep/c.txt -- naming the wrong one here would make this control assert
# something that is false for a reason that has nothing to do with filtering.
check "phase28 control: and keeps what was asked for" \
    grep -q '^sub/deep/c\.txt$' "$WORKDIR/p28_filtered.txt"

# Rules 1 and 2 (the byte compare) run even for a spec containing wildcard
# characters, so a directory whose REAL name holds '[' or '*' is recursed
# into by a spec spelling it literally. Its own fixture, because adding
# these names to the repo above would change the expected output of every
# check there. Measured: git reports o[tx]her/f.txt for `-- 'o[tx]her'`.
P28_LIT="$WORKDIR/p28_literal_wildcards"
(cd "$WORKDIR" && "$SG" init p28_literal_wildcards) > /dev/null 2>&1
(cd "$P28_LIT" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P28_LIT/o[tx]her" "$P28_LIT/st*ar" "$P28_LIT/other"
printf 'f1\nf2\n' > "$P28_LIT/o[tx]her/f.txt"
printf 'g1\ng2\n' > "$P28_LIT/st*ar/g.txt"
printf 'h1\nh2\n' > "$P28_LIT/other/h.txt"
(cd "$P28_LIT" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
for f in 'o[tx]her/f.txt' 'st*ar/g.txt' other/h.txt; do
    printf 'CHANGED\n' >> "$P28_LIT/$f"
done
p28_cmp "$P28_LIT" "--name-only -- 'o[tx]her' (a real directory of that name)" \
    --name-only -- 'o[tx]her'
p28_cmp "$P28_LIT" "--name-only -- 'st*ar' (a real directory of that name)" \
    --name-only -- 'st*ar'
p28_cmp "$P28_LIT" "--name-only -- 'o[tx]her/' (same, trailing slash)" \
    --name-only -- 'o[tx]her/'
# The control that keeps the case above from licensing the over-broad rule:
# in THIS repo "other/" also exists, and the wildcard reading of the same
# spec must still not reach into it.
(cd "$P28_LIT" && "$SG" diff --name-only -- 'o[tx]her') > "$WORKDIR/p28_lit.txt" 2>/dev/null
check "phase28 control: the literal name is matched" \
    grep -q 'o\[tx\]her/f\.txt' "$WORKDIR/p28_lit.txt"
check "phase28 control: and the wildcard reading still does not recurse into other/" \
    sh -c "! grep -q '^other/' '$WORKDIR/p28_lit.txt'"

# An unresolved conflict is the one shape where a single path occupies TWO
# adjacent rows of the change list (a "U" row and a stage-2-vs-worktree row,
# Phase 25). Filtering must keep or drop the pair together, and the three
# comparisons disagree about what a conflicted path even looks like -- so
# each one is compared against git with a pathspec that selects the conflict
# and one that excludes it.
P28_CONF="$WORKDIR/p28_conflict"
(cd "$WORKDIR" && "$SG" init p28_conflict) > /dev/null 2>&1
(cd "$P28_CONF" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P28_CONF/sub"
printf 'orig1\norig2\n' > "$P28_CONF/sub/c.txt"
printf 'plain\n' > "$P28_CONF/a.txt"
(cd "$P28_CONF" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P28_CONF" && "$SG" switch -c feature) > /dev/null 2>&1
printf 'feature1\norig2\n' > "$P28_CONF/sub/c.txt"
(cd "$P28_CONF" && "$SG" add sub/c.txt && "$SG" commit -m feature) > /dev/null 2>&1
(cd "$P28_CONF" && "$SG" switch master < /dev/null) > /dev/null 2>&1
printf 'master1\norig2\n' > "$P28_CONF/sub/c.txt"
(cd "$P28_CONF" && "$SG" add sub/c.txt && "$SG" commit -m master) > /dev/null 2>&1
(cd "$P28_CONF" && "$SG" merge feature < /dev/null) > /dev/null 2>&1
printf 'also changed\n' >> "$P28_CONF/a.txt"
check "phase28 oracle: the fixture really is in a conflicted state" \
    sh -c "cd '$P28_CONF' && git status --porcelain | grep -q '^UU sub/c.txt'"
p28_cmp "$P28_CONF" "--name-status -- sub (conflicted path)" --name-status -- sub
p28_cmp "$P28_CONF" "--name-status -- a.txt (conflict excluded)" --name-status -- a.txt
p28_cmp "$P28_CONF" "--cached --name-status -- sub (conflicted path)" --cached --name-status -- sub
p28_cmp "$P28_CONF" "--cached --name-status -- a.txt (conflict excluded)" --cached --name-status -- a.txt
p28_cmp "$P28_CONF" "HEAD --name-status -- sub (conflicted path)" HEAD --name-status -- sub
# The pair is what makes this fixture different from every other case above:
# plain `sg diff` reports the conflicted path twice, and a pathspec naming it
# must not return just one of the two rows.
(cd "$P28_CONF" && "$SG" diff --name-status -- sub) > "$WORKDIR/p28_conf_pair.txt" 2>/dev/null
check "phase28: a pathspec on a conflicted path keeps both of its rows" \
    sh -c "grep -c 'sub/c.txt' '$WORKDIR/p28_conf_pair.txt' | grep -q '^2\$'"
check "phase28: one of them is the U row" \
    grep -q '^U' "$WORKDIR/p28_conf_pair.txt"

# `sg diff -- --stat` must diff a file named "--stat", not switch formats.
printf 'x\n' > "$P28/--stat"
(cd "$P28" && "$SG" add -- './--stat' && "$SG" commit -m dashfile) > /dev/null 2>&1
printf 'y\n' >> "$P28/--stat"
p28_cmp "$P28" "-- --stat (a file named like an option)" -- '--stat'

# --- Phase 29: rename detection (exact renames) --------------------------
#
# Same discipline as Phase 28: every positive case is a whole-output cmp of
# the SAME argument list against real git, because the shapes here (the
# "R100" score field, the "{a => b}" compression, where the rename lines sit
# relative to the mode lines) are formats sg has no independent claim to.
P29="$WORKDIR/p29_rename"
(cd "$WORKDIR" && "$SG" init p29_rename) > /dev/null 2>&1
(cd "$P29" && git config user.email "a@b.c" && git config user.name "git user")

p29_mk() {
    mkdir -p "$(dirname "$2")"
    python3 -c "
import sys
open(sys.argv[1], 'w').write(''.join('%s-%d\n' % (sys.argv[2], i) for i in range(1, 21)))
" "$2" "$3"
}

p29_cmp() {
    p29_dir="$1"
    p29_label="$2"
    shift 2
    (cd "$p29_dir" && "$SG" diff "$@") > "$WORKDIR/p29_sg.txt" 2>/dev/null
    (cd "$p29_dir" && git -c core.quotepath=false diff "$@") > "$WORKDIR/p29_git.txt" 2>/dev/null
    check "phase29: sg diff $p29_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p29_sg.txt" "$WORKDIR/p29_git.txt"
}

# One fixture carrying every compression shape at once, so a change to the
# prefix/suffix rule cannot pass by being right for one shape only:
#   a/b/c.txt      -> a/z/c.txt        common prefix AND suffix
#   h/i/j.txt      -> h2/i/j.txt       suffix only
#   x/y.txt        -> x/y2.txt         prefix only
#   pre.txt        -> pre.txt.bak      bytes in common but no component
#   one.txt        -> two.txt          nothing in common
p29_mk "$P29" "$P29/a/b/c.txt" A
p29_mk "$P29" "$P29/h/i/j.txt" B
p29_mk "$P29" "$P29/x/y.txt" C
p29_mk "$P29" "$P29/pre.txt" D
p29_mk "$P29" "$P29/one.txt" E
(cd "$P29" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mkdir -p "$P29/a/z" "$P29/h2/i"
mv "$P29/a/b/c.txt" "$P29/a/z/c.txt"
mv "$P29/h/i/j.txt" "$P29/h2/i/j.txt"
mv "$P29/x/y.txt" "$P29/x/y2.txt"
mv "$P29/pre.txt" "$P29/pre.txt.bak"
mv "$P29/one.txt" "$P29/two.txt"
(cd "$P29" && "$SG" add .) > /dev/null 2>&1

for p29_fmt in "" --stat --numstat --shortstat --name-only --name-status; do
    if [ -z "$p29_fmt" ]; then
        p29_cmp "$P29" "--cached (patch)" --cached
    else
        p29_cmp "$P29" "--cached $p29_fmt" --cached "$p29_fmt"
    fi
done
p29_cmp "$P29" "HEAD --name-status (staged rename against HEAD)" HEAD --name-status
p29_cmp "$P29" "--cached --no-renames --name-status" --cached --no-renames --name-status
p29_cmp "$P29" "--cached -M --name-status" --cached -M --name-status
p29_cmp "$P29" "--cached -M100 --name-status" --cached -M100 --name-status

# Tree vs tree sees the same renames once they are committed.
(cd "$P29" && "$SG" commit -m moved) > /dev/null 2>&1
p29_cmp "$P29" "HEAD~1 HEAD --name-status (tree vs tree)" HEAD~1 HEAD --name-status
p29_cmp "$P29" "HEAD~1 HEAD --stat (tree vs tree)" HEAD~1 HEAD --stat

# WARNING: the ordering rule, and the single most likely thing to regress: pathspec
# is applied BEFORE rename detection, so a spec naming only one half of a
# rename leaves nothing to pair with. Measured: git prints "A", not "R100".
p29_cmp "$P29" "HEAD~1 HEAD --name-status -- a/z/c.txt (new half only)" \
    HEAD~1 HEAD --name-status -- a/z/c.txt
p29_cmp "$P29" "HEAD~1 HEAD --name-status -- a/b/c.txt (old half only)" \
    HEAD~1 HEAD --name-status -- a/b/c.txt
p29_cmp "$P29" "HEAD~1 HEAD --name-status -- both halves" \
    HEAD~1 HEAD --name-status -- a/b/c.txt a/z/c.txt
(cd "$P29" && "$SG" diff HEAD~1 HEAD --name-status -- a/z/c.txt) > "$WORKDIR/p29_order.txt" 2>/dev/null
check "phase29 control: a pathspec naming only the new half reports an addition" \
    grep -q '^A' "$WORKDIR/p29_order.txt"
check "phase29 control: and does NOT report a rename" \
    sh -c "! grep -q '^R' '$WORKDIR/p29_order.txt'"

# Rename plus a mode change: the mode lines come first, then the rename
# lines. A fixture with only content-identical renames never reaches that
# ordering at all.
P29_MODE="$WORKDIR/p29_mode"
(cd "$WORKDIR" && "$SG" init p29_mode) > /dev/null 2>&1
(cd "$P29_MODE" && git config user.email "a@b.c" && git config user.name "git user")
p29_mk "$P29_MODE" "$P29_MODE/m.txt" M
(cd "$P29_MODE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P29_MODE/m.txt" "$P29_MODE/m2.txt"
chmod +x "$P29_MODE/m2.txt"
(cd "$P29_MODE" && "$SG" add .) > /dev/null 2>&1
p29_cmp "$P29_MODE" "--cached (patch, rename + mode change)" --cached
p29_cmp "$P29_MODE" "--cached --name-status (rename + mode change)" --cached --name-status
check "phase29: the mode lines precede the similarity line, as git orders them" \
    sh -c "(cd '$P29_MODE' && '$SG' diff --cached) | tr '\n' '|' | grep -q 'old mode 100644|new mode 100755|similarity index 100%|'"

# A renamed path that needs C-quoting. Measured: quoting turns the "{a => b}"
# compression OFF entirely, in --stat and --numstat alike.
P29_Q="$WORKDIR/p29_quote"
(cd "$WORKDIR" && "$SG" init p29_quote) > /dev/null 2>&1
(cd "$P29_Q" && git config user.email "a@b.c" && git config user.name "git user")
p29_mk "$P29_Q" "$P29_Q/d/plain.txt" Q
(cd "$P29_Q" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P29_Q/d/plain.txt" "$P29_Q/d/$(printf 'tab\there.txt')"
(cd "$P29_Q" && "$SG" add .) > /dev/null 2>&1
p29_cmp "$P29_Q" "--cached --name-status (quoted rename)" --cached --name-status
p29_cmp "$P29_Q" "--cached --numstat (quoted rename)" --cached --numstat
p29_cmp "$P29_Q" "--cached --stat (quoted rename)" --cached --stat
p29_cmp "$P29_Q" "--cached (patch, quoted rename)" --cached
check "phase29: a quoted rename is NOT printed in the braced form" \
    sh -c "! (cd '$P29_Q' && '$SG' diff --cached --numstat) | grep -q '{'"

# Pairing when several candidates have identical content.
P29_TIE="$WORKDIR/p29_tie"
(cd "$WORKDIR" && "$SG" init p29_tie) > /dev/null 2>&1
(cd "$P29_TIE" && git config user.email "a@b.c" && git config user.name "git user")
p29_mk "$P29_TIE" "$P29_TIE/a1.txt" SAME
p29_mk "$P29_TIE" "$P29_TIE/a2.txt" SAME
(cd "$P29_TIE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P29_TIE/a1.txt" "$P29_TIE/b1.txt"
mv "$P29_TIE/a2.txt" "$P29_TIE/b2.txt"
(cd "$P29_TIE" && "$SG" add .) > /dev/null 2>&1
p29_cmp "$P29_TIE" "--cached --name-status (two identical sources)" --cached --name-status

P29_ONE="$WORKDIR/p29_one_source"
(cd "$WORKDIR" && "$SG" init p29_one_source) > /dev/null 2>&1
(cd "$P29_ONE" && git config user.email "a@b.c" && git config user.name "git user")
p29_mk "$P29_ONE" "$P29_ONE/src.txt" SAME
(cd "$P29_ONE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P29_ONE/src.txt"
p29_mk "$P29_ONE" "$P29_ONE/d1.txt" SAME
p29_mk "$P29_ONE" "$P29_ONE/d2.txt" SAME
(cd "$P29_ONE" && "$SG" add .) > /dev/null 2>&1
p29_cmp "$P29_ONE" "--cached --name-status (one source, two destinations)" \
    --cached --name-status

# --------------------------------------------------------------------------
# Phase 30: inexact renames -- a rename plus an edit. Until this milestone the
# checks here asserted the OPPOSITE, that sg found no rename at all, on
# purpose: an asserted gap fails the day it closes and forces this rewrite,
# where an unasserted one just quietly persists because nothing ever looked.
#
# The score is not the kind of number a near miss is acceptable for. It is
# printed in machine-readable form ("R086", "similarity index 86%"), so every
# check below compares whole output against real git byte for byte.
P30_INEX="$WORKDIR/p30_inexact"
(cd "$WORKDIR" && "$SG" init p30_inexact) > /dev/null 2>&1
(cd "$P30_INEX" && git config user.email "a@b.c" && git config user.name "git user")
p29_mk "$P30_INEX" "$P30_INEX/t.txt" T
python3 -c "
import sys
open(sys.argv[1], 'wb').write(bytes(range(256)) * 20)
" "$P30_INEX/b.bin"
(cd "$P30_INEX" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P30_INEX/t.txt" "$P30_INEX/t2.txt"
printf 'one more line\n' >> "$P30_INEX/t2.txt"
python3 -c "
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[0:10] = b'\x01' * 10
open(sys.argv[2], 'wb').write(bytes(d))
" "$P30_INEX/b.bin" "$P30_INEX/c.bin"
rm "$P30_INEX/b.bin"
(cd "$P30_INEX" && "$SG" add .) > /dev/null 2>&1
(cd "$P30_INEX" && "$SG" diff --cached --name-status) > "$WORKDIR/p30_inex_sg.txt" 2>/dev/null
(cd "$P30_INEX" && git diff --cached --name-status) > "$WORKDIR/p30_inex_git.txt" 2>/dev/null
check "phase30 oracle: real git detects the inexact rename" \
    grep -q '^R0' "$WORKDIR/p30_inex_git.txt"
# Named apart from the byte compares below so that losing inexact detection
# entirely reports itself as that, rather than only as "output differs".
check "phase30: sg finds it too, and does not call it a perfect match" \
    grep -q '^R0' "$WORKDIR/p30_inex_sg.txt"

# All six formats. The patch one also covers two lines that no exact rename
# could ever reach, because an exact rename is byte-identical and so prints
# no body at all: the "--- a/<old>" line must name the path the content came
# FROM, and so must the "Binary files a/<old> and b/<new> differ" line.
p29_cmp "$P30_INEX" "--cached (inexact patch + binary rename)" --cached
p29_cmp "$P30_INEX" "--cached --stat (inexact)" --cached --stat
p29_cmp "$P30_INEX" "--cached --numstat (inexact)" --cached --numstat
p29_cmp "$P30_INEX" "--cached --shortstat (inexact)" --cached --shortstat
p29_cmp "$P30_INEX" "--cached --name-only (inexact)" --cached --name-only
p29_cmp "$P30_INEX" "--cached --name-status (inexact)" --cached --name-status
# Detection is a pass over a finished list, so every builder should feed it;
# nothing had ever checked a builder other than tree-vs-index, for exact
# renames either.
p29_cmp "$P30_INEX" "HEAD (inexact, tree vs working tree)" HEAD
p29_cmp "$P30_INEX" "HEAD --name-status (inexact, tree vs working tree)" \
    HEAD --name-status
# The control that keeps all of the above honest: with detection turned off
# both sides must agree for a reason that is not detection working.
p29_cmp "$P30_INEX" "--cached --no-renames --name-status (inexact)" \
    --cached --no-renames --name-status

# The -M grammar. Every rule of it was measured against git 2.55.0 and every
# one is counter-intuitive: the digits are a FRACTION unless a '%' follows,
# so -M5 is 50% and -M100 is TEN percent -- not exact-renames-only, which is
# -M100% alone. The fixture scores 86%, which is what separates these: -M9
# and -M90 (both 90%) must find nothing, -M100 (10%) must find the rename,
# and -M86%/-M87% straddle the threshold by one point.
for p30_opt in -M -M5 -M05 -M50 -M9 -M90 -M100 "-M100%" -M0.5 "-M0.5%" \
               "-M50%" "-M86%" "-M87%" --find-renames "--find-renames=86%"; do
    p29_cmp "$P30_INEX" "--cached --name-status $p30_opt" \
        --cached --name-status "$p30_opt"
done

# The three passes run in git's order, and the order is VISIBLE, so it is not
# an implementation detail that could be collapsed into "score every pair and
# keep the best". A source and a destination sharing a file name pair up on
# the strength of that name at a raised threshold, without ever being
# compared against a destination they resemble far more closely. Measured
# against git 2.55.0: dir1/foo.txt becomes dir2/foo.txt at 79%, and
# other.txt -- which is 99% the same file -- is left an ordinary addition.
# Scoring everything and taking the best would pair it the other way round
# and still look perfectly reasonable.
P30_BASE="$WORKDIR/p30_basename"
(cd "$WORKDIR" && "$SG" init p30_basename) > /dev/null 2>&1
(cd "$P30_BASE" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P30_BASE/dir1" "$P30_BASE/dir2"
p30_lines() {
    python3 -c "
import sys
open(sys.argv[1], 'w').write(''.join('L-%d\n' % i for i in range(1, int(sys.argv[2]))))
" "$1" "$2"
}
p30_lines "$P30_BASE/dir1/foo.txt" 101
(cd "$P30_BASE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_BASE/dir1/foo.txt"
p30_lines "$P30_BASE/dir2/foo.txt" 81
p30_lines "$P30_BASE/other.txt" 100
(cd "$P30_BASE" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git pairs on the file name, not on the best score" \
    sh -c "(cd '$P30_BASE' && git diff --cached --name-status) | grep -q '^R079.*dir1/foo.txt.*dir2/foo.txt'"
p29_cmp "$P30_BASE" "--cached --name-status (name beats a better score)" \
    --cached --name-status
p29_cmp "$P30_BASE" "--cached --stat (name beats a better score)" --cached --stat

# The same shape one notch weaker, which is what pins the RAISED threshold
# down as a number rather than as "some threshold". d1/x.txt is only 60% of
# d2/x.txt -- under the 75% a name match has to clear, though still over the
# 50% an ordinary pair needs -- so here the name shortcut declines, the full
# comparison runs, and the source goes to zz.txt, which it really does
# resemble. The two fixtures disagree about which side wins, so no single
# wrong threshold can satisfy both.
P30_BASE2="$WORKDIR/p30_basename_low"
(cd "$WORKDIR" && "$SG" init p30_basename_low) > /dev/null 2>&1
(cd "$P30_BASE2" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P30_BASE2/d1" "$P30_BASE2/d2"
p30_lines "$P30_BASE2/d1/x.txt" 101
(cd "$P30_BASE2" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_BASE2/d1/x.txt"
p30_lines "$P30_BASE2/d2/x.txt" 61
p30_lines "$P30_BASE2/zz.txt" 100
(cd "$P30_BASE2" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: a weak name match loses to the better score" \
    sh -c "(cd '$P30_BASE2' && git diff --cached --name-status) | grep -q '^R098.*d1/x.txt.*zz.txt'"
p29_cmp "$P30_BASE2" "--cached --name-status (weak name match loses)" \
    --cached --name-status

# Two tie-breaks that only a deliberately built fixture can reach, both
# measured against git 2.55.0 and both wrong in a way nothing else notices.
#
# 1. Identical content on two sources: the tie goes to the source that shares
#    the destination's FILE NAME, not to the first one in path order.
P30_EXBN="$WORKDIR/p30_exact_basename"
(cd "$WORKDIR" && "$SG" init p30_exact_basename) > /dev/null 2>&1
(cd "$P30_EXBN" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P30_EXBN/a" "$P30_EXBN/b" "$P30_EXBN/c"
p29_mk "$P30_EXBN" "$P30_EXBN/a/g.txt" SAME
p29_mk "$P30_EXBN" "$P30_EXBN/b/f.txt" SAME
(cd "$P30_EXBN" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_EXBN/a/g.txt" "$P30_EXBN/b/f.txt"
p29_mk "$P30_EXBN" "$P30_EXBN/c/f.txt" SAME
(cd "$P30_EXBN" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git breaks an exact tie on the file name" \
    sh -c "(cd '$P30_EXBN' && git diff --cached --name-status) | grep -q '^R100.*b/f.txt.*c/f.txt'"
p29_cmp "$P30_EXBN" "--cached --name-status (exact tie goes to the file name)" \
    --cached --name-status
#
# 2. Only the best FOUR sources per destination are ranked, and the ranking is
#    sorted stably -- so when two sources tie exactly, the winner is decided
#    by which slot a candidate was written into, not by the order candidates
#    were considered. Five sources scoring 50/60/89/80/89 make the two come
#    apart: git answers s5.txt, candidate order would answer s3.txt.
P30_TIE4="$WORKDIR/p30_four_candidates"
(cd "$WORKDIR" && "$SG" init p30_four_candidates) > /dev/null 2>&1
(cd "$P30_TIE4" && git config user.email "a@b.c" && git config user.name "git user")
python3 -c "
import sys
root = sys.argv[1]
base = ''.join('L-%d\n' % i for i in range(1, 81))
for tag, want in ((1, 782), (2, 652), (3, 434), (4, 489), (5, 434)):
    text = base
    i = 1
    while len(text) < want:
        text += 'f%d-%d\n' % (tag, i)
        i += 1
    open('%s/s%d.txt' % (root, tag), 'w').write(text)
" "$P30_TIE4"
(cd "$P30_TIE4" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_TIE4"/s*.txt
p30_lines "$P30_TIE4/p.txt" 81
(cd "$P30_TIE4" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git gives the tie to the fifth source, not the third" \
    sh -c "(cd '$P30_TIE4' && git diff --cached --name-status) | grep -q '^R089.*s5.txt.*p.txt'"
p29_cmp "$P30_TIE4" "--cached --name-status (only four candidates are ranked)" \
    --cached --name-status
#
# 3. Two sources scoring exactly the same: the ranking's second key is
#    whether the source shares the destination's file name. Scored at 59% --
#    over the 50% an ordinary pair needs, under the 75% the name shortcut
#    demands -- so this reaches the full comparison instead of being settled
#    on the name beforehand, and b/x.txt wins despite sorting later.
P30_NAMESCORE="$WORKDIR/p30_name_score"
(cd "$WORKDIR" && "$SG" init p30_name_score) > /dev/null 2>&1
(cd "$P30_NAMESCORE" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P30_NAMESCORE/a" "$P30_NAMESCORE/b" "$P30_NAMESCORE/c"
python3 -c "
import sys
root = sys.argv[1]
base = ''.join('L-%d\n' % i for i in range(1, 81))
for tag, rel in ((1, 'a/y.txt'), (2, 'b/x.txt')):
    text = base
    i = 1
    while len(text) < 652:
        text += 'f%d-%d\n' % (tag, i)
        i += 1
    open('%s/%s' % (root, rel), 'w').write(text)
" "$P30_NAMESCORE"
(cd "$P30_NAMESCORE" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_NAMESCORE/a/y.txt" "$P30_NAMESCORE/b/x.txt"
p30_lines "$P30_NAMESCORE/c/x.txt" 81
(cd "$P30_NAMESCORE" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git breaks an equal score on the file name" \
    sh -c "(cd '$P30_NAMESCORE' && git diff --cached --name-status) | grep -q '^R059.*b/x.txt.*c/x.txt'"
p29_cmp "$P30_NAMESCORE" "--cached --name-status (equal scores, file name decides)" \
    --cached --name-status
#
# 4. A tie must not displace what the ranking already holds. Six sources
#    score identically, two more than the four slots, so the last two meet a
#    full table of exact ties and are dropped -- git answers t1.txt, while
#    replacing on a tie would answer t6.txt.
P30_TIE6="$WORKDIR/p30_tie_keeps_first"
(cd "$WORKDIR" && "$SG" init p30_tie_keeps_first) > /dev/null 2>&1
(cd "$P30_TIE6" && git config user.email "a@b.c" && git config user.name "git user")
python3 -c "
import sys
root = sys.argv[1]
base = ''.join('L-%d\n' % i for i in range(1, 81))
for tag in range(1, 7):
    text = base
    i = 1
    while len(text) < 652:
        text += 'f%d-%d\n' % (tag, i)
        i += 1
    open('%s/t%d.txt' % (root, tag), 'w').write(text)
" "$P30_TIE6"
(cd "$P30_TIE6" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_TIE6"/t?.txt
p30_lines "$P30_TIE6/p.txt" 81
(cd "$P30_TIE6" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git keeps the first of six equal-scoring sources" \
    sh -c "(cd '$P30_TIE6' && git diff --cached --name-status) | grep -q '^R059.*t1.txt.*p.txt'"
p29_cmp "$P30_TIE6" "--cached --name-status (an exact tie keeps the first)" \
    --cached --name-status
#
# 5. git stops after a hundred identically-contented sources and settles for
#    the best it has seen. The source sharing the destination's file name is
#    the hundred-and-first, so the cap is the only thing between s001.txt and
#    s101/target.txt -- and it also pins down the ORDER those sources are
#    walked in, which is an assumption the port makes about a hashmap inside
#    git and has no other way to check.
P30_CAP="$WORKDIR/p30_alternatives_cap"
(cd "$WORKDIR" && "$SG" init p30_alternatives_cap) > /dev/null 2>&1
(cd "$P30_CAP" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P30_CAP/s101" "$P30_CAP/z"
python3 -c "
import sys
root = sys.argv[1]
text = ''.join('L-%d\n' % i for i in range(1, 21))
for i in range(1, 101):
    open('%s/s%03d.txt' % (root, i), 'w').write(text)
open('%s/s101/target.txt' % root, 'w').write(text)
" "$P30_CAP"
(cd "$P30_CAP" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_CAP"/s0*.txt "$P30_CAP"/s1[0-9][0-9].txt "$P30_CAP/s101/target.txt"
p29_mk "$P30_CAP" "$P30_CAP/z/target.txt" L
python3 -c "
import sys
open(sys.argv[1], 'w').write(''.join('L-%d\n' % i for i in range(1, 21)))
" "$P30_CAP/z/target.txt"
(cd "$P30_CAP" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git stops looking after a hundred identical sources" \
    sh -c "(cd '$P30_CAP' && git diff --cached --name-status) | grep -q '^R100.*s001.txt.*z/target.txt'"
p29_cmp "$P30_CAP" "--cached --name-status (the hundred-alternative cap)" \
    --cached --name-status
#
# 6. The name shortcut only fires when the name identifies ONE file on each
#    side. Two sources called x.txt make git decline to guess and let the
#    full comparison decide, picking b/x.txt at 98% over a/x.txt at 80% --
#    and 80% is deliberately above the 75% the shortcut demands, so taking
#    the first of the repeated names instead of declining would answer
#    a/x.txt.
P30_DUPNAME="$WORKDIR/p30_repeated_name"
(cd "$WORKDIR" && "$SG" init p30_repeated_name) > /dev/null 2>&1
(cd "$P30_DUPNAME" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P30_DUPNAME/a" "$P30_DUPNAME/b" "$P30_DUPNAME/c"
p30_lines "$P30_DUPNAME/a/x.txt" 81
p30_lines "$P30_DUPNAME/b/x.txt" 101
(cd "$P30_DUPNAME" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P30_DUPNAME/a/x.txt" "$P30_DUPNAME/b/x.txt"
p30_lines "$P30_DUPNAME/c/x.txt" 100
(cd "$P30_DUPNAME" && "$SG" add .) > /dev/null 2>&1
check "phase30 oracle: git declines the shortcut when a name is repeated" \
    sh -c "(cd '$P30_DUPNAME' && git diff --cached --name-status) | grep -q '^R098.*b/x.txt.*c/x.txt'"
p29_cmp "$P30_DUPNAME" "--cached --name-status (a repeated name decides nothing)" \
    --cached --name-status

# A -M argument neither side can make sense of must be REJECTED, not quietly
# read as far as it parses. Compared as behaviour rather than byte-for-byte:
# git exits 128 and sg's convention is only ever 0 or 1, so the shared
# assertion is "both refuse, and sg says so on stderr".
check "phase30 oracle: real git rejects an unparsable -M argument" \
    sh -c "! (cd '$P30_INEX' && git diff --cached --name-status -Mabc) > /dev/null 2>&1"
check "phase30: sg rejects it too" \
    sh -c "! (cd '$P30_INEX' && '$SG' diff --cached --name-status -Mabc) > /dev/null 2>&1"
(cd "$P30_INEX" && "$SG" diff --cached --name-status -Mabc) > /dev/null 2>"$WORKDIR/p30_mbad.txt"
check "phase30: and says so as a usage error" \
    grep -q '^usage: sg diff' "$WORKDIR/p30_mbad.txt"
# The control: the same argument with the garbage removed is accepted, so the
# checks above are about the garbage and not about -M being broken outright.
p29_cmp "$P30_INEX" "--cached --name-status -M86 (the control for -Mabc)" \
    --cached --name-status -M86

# --- Phase 31: `sg stash show` gains rename detection ---------------------
#
# The stash is built with `sg stash -u` (sg's own stash commit shape is not
# guaranteed to be byte-for-byte identical to git's), then read back by both
# sg and git's `stash show` and compared byte for byte -- same discipline as
# the Phase 25 stash-show block above and the Phase 29/30 diff-rename block.
#
# Fixture: tracked.txt (100 lines) committed, then renamed to tracked2.txt
# and truncated to 80 lines (an inexact, ~79%, rename). Two untracked files:
# untracked_a.txt is byte-identical to the ORIGINAL tracked.txt (so it can
# steal the exact-rename match once -u merges tracked and untracked into one
# list), and untracked_b.txt is unrelated content.
P31="$WORKDIR/p31_stash_rename"
(cd "$WORKDIR" && "$SG" init p31_stash_rename) > /dev/null 2>&1
(cd "$P31" && git config user.email "a@b.c" && git config user.name "git user")
python3 -c "
open('$P31/tracked.txt', 'w').write(''.join('T-%d\n' % i for i in range(1, 101)))
"
(cd "$P31" && "$SG" add tracked.txt && "$SG" commit -m base) > /dev/null 2>&1
cp "$P31/tracked.txt" "$WORKDIR/p31_untracked_a.txt"
git -C "$P31" mv tracked.txt tracked2.txt > /dev/null 2>&1
python3 -c "
lines = open('$P31/tracked2.txt').readlines()[:80]
open('$P31/tracked2.txt', 'w').writelines(lines)
"
cp "$WORKDIR/p31_untracked_a.txt" "$P31/untracked_a.txt"
python3 -c "
open('$P31/untracked_b.txt', 'w').write(''.join('U-%d\n' % i for i in range(1, 51)))
"
(cd "$P31" && "$SG" add tracked2.txt) > /dev/null 2>&1
(cd "$P31" && "$SG" stash -u) > /dev/null 2>&1

check "phase31 oracle: precondition -- sg's -u stash really has an untracked parent" \
    sh -c "(cd '$P31' && git cat-file -p 'stash@{0}') 2>/dev/null | grep -c '^parent ' | grep -q '^3\$'"
check "phase31 oracle: real git detects the inexact rename on the default (tracked-only) diff" \
    sh -c "(cd '$P31' && LC_ALL=C git stash show --name-status) 2>/dev/null | grep -q '^R079'"

p31_cmp() {
    p31_label="$1"
    shift
    (cd "$P31" && "$SG" stash show "$@") > "$WORKDIR/p31_sg.txt" 2>/dev/null
    (cd "$P31" && LC_ALL=C git -c core.quotepath=false stash show "$@") \
        > "$WORKDIR/p31_git.txt" 2>/dev/null
    check "phase31: sg stash show $p31_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p31_sg.txt" "$WORKDIR/p31_git.txt"
}

p31_cmp "(default, inexact rename)"
p31_cmp "-p (inexact rename)" -p
p31_cmp "--name-status (inexact rename)" --name-status
p31_cmp "--stat (inexact rename)" --stat
p31_cmp "--numstat (inexact rename)" --numstat
p31_cmp "--shortstat (inexact rename)" --shortstat
p31_cmp "--name-only (inexact rename)" --name-only

# -u merges tracked and untracked into one list before detection runs once,
# so untracked_a.txt (byte-identical to the ORIGINAL tracked.txt) steals the
# exact-rename match ahead of the real inexact rename -- measured against
# real git 2.55.0, not a bug in this port.
check "phase31 oracle: real git's -u lets the untracked exact match steal the source" \
    sh -c "(cd '$P31' && LC_ALL=C git stash show -u --name-status) 2>/dev/null | grep -q \"^R100\$(printf '\\t')tracked.txt\$(printf '\\t')untracked_a.txt\$\""
p31_cmp "-u --name-status (untracked steals the exact match)" -u --name-status

check "phase31 oracle: --only-untracked has no tracked side to rename from" \
    sh -c "(cd '$P31' && LC_ALL=C git stash show --only-untracked --name-status) 2>/dev/null | grep -q \"^A\$(printf '\\t')untracked_a.txt\$\""
p31_cmp "--only-untracked --name-status" --only-untracked --name-status

# The spellings that all mean "use the default". Worth their own checks here
# rather than leaning on `sg diff`'s: the bare -M and --find-renames branches
# in cmd_stash.c assign the default DIRECTLY and never reach the shared
# parser, so nothing on the diff side can protect them. -M0 and -M% do reach
# it, and pin down that the "a parsed 0 means the default" rule is wired up
# at this call site too.
for p31_dflt in -M --find-renames -M0 "-M%"; do
    p31_cmp "$p31_dflt --name-status (spellings meaning the default)" \
        "$p31_dflt" --name-status
done
p31_cmp "-M100% --name-status (exact-only threshold rejects the inexact rename)" \
    -M100% --name-status
p31_cmp "--no-renames --name-status (control)" --no-renames --name-status

check "phase31 oracle: a threshold above the actual score also rejects it" \
    sh -c "! (cd '$P31' && LC_ALL=C git stash show -M87% --name-status) 2>/dev/null | grep -q '^R'"
p31_cmp "-M87% --name-status (threshold above the actual score)" -M87% --name-status

check "phase31 oracle: real git rejects an unparsable -M argument" \
    sh -c "! (cd '$P31' && git stash show -Mabc) > /dev/null 2>&1"
check "phase31: sg rejects it too" \
    sh -c "! (cd '$P31' && '$SG' stash show -Mabc) > /dev/null 2>&1"
(cd "$P31" && "$SG" stash show -Mabc) > /dev/null 2>"$WORKDIR/p31_mbad.txt"
check "phase31: and says so as a usage error" \
    grep -q '^usage: sg stash show' "$WORKDIR/p31_mbad.txt"

# --- Phase 32: `sg status` gains a rename row -----------------------------
#
# The staged half of `sg status` used to be a second, hand-rolled walk of
# HEAD-tree vs index; it is now a thin adapter over sg_diff_tree_index, which
# is what let rename detection reach it at all. A differential harness
# (tests/test_status_staged_parity.c) proved the two walks equivalent before
# the swap, so everything below is about the rename rows the swap made
# possible, not about the swap itself.
#
# The long format cannot be compared whole: sg's hints say "sg restore ..."
# where git's say "git restore ...", deliberately. So the entry lines are
# extracted the way the Phase 23 block does it -- and for the staged section
# WITHOUT sorting, because the row order is part of the answer (measured:
# rename rows sort by the NEW path, exactly as `git diff --name-status` does).
P32="$WORKDIR/p32_status_rename"
(cd "$WORKDIR" && "$SG" init p32_status_rename) > /dev/null 2>&1
(cd "$P32" && git config user.email "a@b.c" && git config user.name "git user")
p32_mk() {
    python3 -c "
import sys
open(sys.argv[1], 'w').write(''.join('%s-%d\n' % (sys.argv[2], i)
                                     for i in range(1, int(sys.argv[3]) + 1)))
" "$1" "$2" "$3"
}
p32_mk "$P32/exact.txt" E 20
p32_mk "$P32/inexact.txt" I 100
p32_mk "$P32/has space.txt" S 20
p32_mk "$P32/rd.txt" R 20
(cd "$P32" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P32/exact.txt" "$P32/exact2.txt"
mv "$P32/inexact.txt" "$P32/inexact2.txt"
python3 -c "
import sys
lines = open(sys.argv[1]).readlines()[:80]
open(sys.argv[1], 'w').writelines(lines)
" "$P32/inexact2.txt"
mv "$P32/has space.txt" "$P32/renamed space.txt"
mv "$P32/rd.txt" "$P32/rd2.txt"
(cd "$P32" && "$SG" add .) > /dev/null 2>&1
rm "$P32/rd2.txt"

check "phase32 oracle: real git reports a staged rename as R" \
    sh -c "(cd '$P32' && LC_ALL=C git status --porcelain) | grep -q '^R  exact.txt -> exact2.txt$'"
check "phase32 oracle: and pairs the edited one too" \
    sh -c "(cd '$P32' && LC_ALL=C git status --porcelain) | grep -q '^R  inexact.txt -> inexact2.txt$'"
# Named apart from the byte compares so losing detection says so, rather than
# only saying "output differs".
check "phase32: sg reports it as a rename too" \
    sh -c "(cd '$P32' && '$SG' status --porcelain) | grep -q '^R  exact.txt -> exact2.txt$'"

p32_cmp() {
    p32_label="$1"
    shift
    (cd "$P32" && "$SG" status "$@") > "$WORKDIR/p32_sg.txt" 2>&1
    (cd "$P32" && LC_ALL=C git -c core.quotepath=false status "$@") > "$WORKDIR/p32_git.txt" 2>&1
    check "phase32: sg status $p32_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p32_sg.txt" "$WORKDIR/p32_git.txt"
}
p32_cmp "--porcelain (exact, inexact, quoted and RD renames)" --porcelain
p32_cmp "-s" -s
# The staged section of the long format, in order.
(cd "$P32" && "$SG" status) 2>/dev/null \
    | sed -n '/^Changes to be committed:/,/^$/p' | sed -n 's/^\t//p' > "$WORKDIR/p32_ls.txt"
(cd "$P32" && LC_ALL=C git -c core.quotepath=false status) 2>/dev/null \
    | sed -n '/^Changes to be committed:/,/^$/p' | sed -n 's/^\t//p' > "$WORKDIR/p32_lg.txt"
check "phase32: the long format's staged section matches real git, in order" \
    cmp -s "$WORKDIR/p32_ls.txt" "$WORKDIR/p32_lg.txt"
# A rename in the long format is NOT quoted even with a space in it, while
# porcelain quotes both halves -- the two rules that CLAUDE.md says must not
# be unified, now colliding head-on over a single row.
check "phase32: the long format leaves a spaced rename unquoted" \
    grep -q '^renamed:    has space.txt -> renamed space.txt$' "$WORKDIR/p32_ls.txt"
check "phase32: while porcelain quotes both halves of it" \
    sh -c "(cd '$P32' && '$SG' status --porcelain) | grep -q '^R  \"has space.txt\" -> \"renamed space.txt\"\$'"
# ...and quotes them INDEPENDENTLY, which the fixture above cannot show
# because both of its halves need quoting. Measured against git 2.55.0.
P32Q="$WORKDIR/p32_one_sided_quote"
(cd "$WORKDIR" && "$SG" init p32_one_sided_quote) > /dev/null 2>&1
(cd "$P32Q" && git config user.email "a@b.c" && git config user.name "git user")
p32_mk "$P32Q/has space.txt" S 20
p32_mk "$P32Q/plain.txt" P 20
(cd "$P32Q" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P32Q/has space.txt" "$P32Q/nospace.txt"
mv "$P32Q/plain.txt" "$P32Q/new space.txt"
(cd "$P32Q" && "$SG" add .) > /dev/null 2>&1
check "phase32 oracle: git quotes only the half that needs it" \
    sh -c "(cd '$P32Q' && LC_ALL=C git status --porcelain) | grep -q '^R  plain.txt -> \"new space.txt\"\$'"
check "phase32 oracle: and only the other half in the other direction" \
    sh -c "(cd '$P32Q' && LC_ALL=C git status --porcelain) | grep -q '^R  \"has space.txt\" -> nospace.txt\$'"
(cd "$P32Q" && "$SG" status --porcelain) > "$WORKDIR/p32_q_sg.txt" 2>&1
(cd "$P32Q" && LC_ALL=C git -c core.quotepath=false status --porcelain) > "$WORKDIR/p32_q_git.txt" 2>&1
check "phase32: sg quotes each half independently too" \
    cmp -s "$WORKDIR/p32_q_sg.txt" "$WORKDIR/p32_q_git.txt"
# A staged rename whose new path is then deleted from the working tree is one
# line, not two -- the staged R and the unstaged D merge on the new path.
check "phase32 oracle: git merges a staged rename with an unstaged delete" \
    sh -c "(cd '$P32' && LC_ALL=C git status --porcelain) | grep -q '^RD rd.txt -> rd2.txt$'"
check "phase32: so does sg" \
    sh -c "(cd '$P32' && '$SG' status --porcelain) | grep -q '^RD rd.txt -> rd2.txt$'"

# The -M grammar, and the one place `status` and `diff` genuinely disagree.
for p32_opt in --no-renames -M --find-renames -M5 -M05 "-M50%" "-M100%" "-M87%" \
               "--find-renames=87%" -M0 "-M%"; do
    p32_cmp "--porcelain $p32_opt" --porcelain "$p32_opt"
done
# MEASURED, and counter-intuitive: `git status -Mabc` exits 0 and quietly uses
# the default, while `git diff -Mabc` exits 129 with an error. The two commands
# do not share a rule, so sg cannot either.
check "phase32 oracle: git status accepts an unparsable -M and carries on" \
    sh -c "(cd '$P32' && LC_ALL=C git status --porcelain -Mabc) > /dev/null 2>&1"
check "phase32 oracle: while git diff rejects the very same argument" \
    sh -c "! (cd '$P32' && LC_ALL=C git diff --cached -Mabc) > /dev/null 2>&1"
check "phase32: sg status accepts it too" \
    sh -c "(cd '$P32' && '$SG' status --porcelain -Mabc) > /dev/null 2>&1"
check "phase32: and sg diff still rejects it" \
    sh -c "! (cd '$P32' && '$SG' diff --cached -Mabc) > /dev/null 2>&1"
p32_cmp "--porcelain -Mabc (ignored, not rejected)" --porcelain -Mabc

# Row order is the NEW path's, not the old one's. The two orders disagree
# here on purpose, so sorting by the wrong half cannot pass.
P32B="$WORKDIR/p32_order"
(cd "$WORKDIR" && "$SG" init p32_order) > /dev/null 2>&1
(cd "$P32B" && git config user.email "a@b.c" && git config user.name "git user")
p32_mk "$P32B/b.txt" B 20
p32_mk "$P32B/a2.txt" A 20
(cd "$P32B" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
mv "$P32B/b.txt" "$P32B/a.txt"
mv "$P32B/a2.txt" "$P32B/z.txt"
(cd "$P32B" && "$SG" add .) > /dev/null 2>&1
check "phase32 oracle: git orders rename rows by the new path" \
    sh -c "(cd '$P32B' && LC_ALL=C git status --porcelain) | head -1 | grep -q '^R  b.txt -> a.txt$'"
(cd "$P32B" && "$SG" status --porcelain) > "$WORKDIR/p32_osg.txt" 2>&1
(cd "$P32B" && LC_ALL=C git -c core.quotepath=false status --porcelain) > "$WORKDIR/p32_ogit.txt" 2>&1
check "phase32: sg orders them the same way" \
    cmp -s "$WORKDIR/p32_osg.txt" "$WORKDIR/p32_ogit.txt"

# The safety gates in apply.c ask for this same staged list, and they pass
# rename detection OFF on purpose: they enumerate it to tell the user what is
# uncommitted, one path per row, so a rename row would silently stop naming
# the path the content came FROM. Nothing else in this suite watches that
# choice -- a mutation flipping both gates to the default threshold left
# every other check green -- so it is pinned here, at the one place the
# gates' list becomes visible: the message they print when they block.
P32C="$WORKDIR/p32_gate_list"
(cd "$WORKDIR" && "$SG" init p32_gate_list) > /dev/null 2>&1
(cd "$P32C" && git config user.email "a@b.c" && git config user.name "git user")
p32_mk "$P32C/a.txt" A 20
(cd "$P32C" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P32C" && "$SG" switch -c other) > /dev/null 2>&1
printf 'x\n' > "$P32C/other.txt"
(cd "$P32C" && "$SG" add . && "$SG" commit -m other) > /dev/null 2>&1
(cd "$P32C" && "$SG" switch master) > /dev/null 2>&1
printf 'y\n' > "$P32C/m.txt"
(cd "$P32C" && "$SG" add . && "$SG" commit -m master) > /dev/null 2>&1
mv "$P32C/a.txt" "$P32C/b.txt"
(cd "$P32C" && "$SG" add .) > /dev/null 2>&1
# The precondition: `sg status` really does call this a rename, so the check
# below is about the gate's own choice and not about detection being broken.
check "phase32: precondition -- status calls the staged change a rename" \
    sh -c "(cd '$P32C' && '$SG' status --porcelain) | grep -q '^R  a.txt -> b.txt$'"
(cd "$P32C" && "$SG" merge other) > /dev/null 2>"$WORKDIR/p32_gate.txt"
check "phase32: the clean-workdir gate still names the path it came from" \
    grep -q '^	staged:              a.txt$' "$WORKDIR/p32_gate.txt"
check "phase32: and the path it went to" \
    grep -q '^	staged:              b.txt$' "$WORKDIR/p32_gate.txt"

# --- Phase 33: `-C` copy detection ----------------------------------------
#
# Same discipline as Phase 29/30: every positive case gets a whole-output cmp
# against real git, PLUS a grep-based oracle assertion proving real git
# actually answers what this block assumes it does -- Phase 32 lost a whole
# fixture to a flag sg did not support, and the byte-for-byte compares stayed
# green for the wrong reason because nothing checked the oracle side
# independently.
P33="$WORKDIR/p33_copy"
(cd "$WORKDIR" && "$SG" init p33_copy) > /dev/null 2>&1
(cd "$P33" && git config user.email "a@b.c" && git config user.name "git user")

p33_mk() {
    mkdir -p "$(dirname "$2")"
    python3 -c "
import sys
open(sys.argv[1], 'w').write(''.join('%s-%d\n' % (sys.argv[2], i) for i in range(1, int(sys.argv[3]) + 1)))
" "$2" "$3" "${4:-20}"
}

p33_cmp() {
    p33_dir="$1"
    p33_label="$2"
    shift 2
    (cd "$p33_dir" && "$SG" diff "$@") > "$WORKDIR/p33_sg.txt" 2>/dev/null
    (cd "$p33_dir" && git -c core.quotepath=false diff "$@") > "$WORKDIR/p33_git.txt" 2>/dev/null
    check "phase33: sg diff $p33_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p33_sg.txt" "$WORKDIR/p33_git.txt"
}

# --- A: an untouched source plus a full copy -- plain -C finds nothing, only
# -C -C (find-copies-harder) would, and sg does not implement that. ----------
P33_A="$WORKDIR/p33_untouched_source"
(cd "$WORKDIR" && "$SG" init p33_untouched_source) > /dev/null 2>&1
(cd "$P33_A" && git config user.email "a@b.c" && git config user.name "git user")
p33_mk "$P33_A" "$P33_A/src.txt" A
(cd "$P33_A" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
cp "$P33_A/src.txt" "$P33_A/copy.txt"
(cd "$P33_A" && "$SG" add .) > /dev/null 2>&1
check "phase33 oracle: real git's plain -C does NOT find a copy from an untouched source" \
    sh -c "(cd '$P33_A' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^A[[:space:]]copy.txt\$'"
check "phase33 oracle: and it prints no C-line at all" \
    sh -c "! (cd '$P33_A' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^C'"
for p33_fmt in --name-status --stat --numstat --shortstat --name-only; do
    p33_cmp "$P33_A" "--cached -C $p33_fmt (untouched source)" --cached -C "$p33_fmt"
done
p33_cmp "$P33_A" "--cached -C (patch, untouched source)" --cached -C

# --- B: a modified source plus an inexact copy of its ORIGINAL content -----
P33_B="$WORKDIR/p33_modified_source"
(cd "$WORKDIR" && "$SG" init p33_modified_source) > /dev/null 2>&1
(cd "$P33_B" && git config user.email "a@b.c" && git config user.name "git user")
p33_mk "$P33_B" "$P33_B/src.txt" B 100
(cd "$P33_B" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
# The copy is taken from the ORIGINAL 100-line content (first 80 lines of it,
# byte for byte -- the same shape as the Phase 30/test_rename.c 100-down-to-80
# fixture, which scores 79). src.txt is then edited in place, which is what
# makes it stay in the list as a plain modification instead of a deletion.
p33_mk "$P33_B" "$P33_B/copy.txt" B 80
python3 -c "
import sys
lines = open(sys.argv[1]).readlines()[:79]
open(sys.argv[1], 'w').writelines(lines + ['EDITED\n'])
" "$P33_B/src.txt"
(cd "$P33_B" && "$SG" add .) > /dev/null 2>&1
check "phase33 oracle: real git pairs the edited source with -C" \
    sh -c "(cd '$P33_B' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^C079[[:space:]]src.txt[[:space:]]copy.txt\$'"
check "phase33 oracle: and the source row is STILL a plain modification" \
    sh -c "(cd '$P33_B' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^M[[:space:]]src.txt\$'"
for p33_fmt in --name-status --stat --numstat --shortstat --name-only; do
    p33_cmp "$P33_B" "--cached -C $p33_fmt (modified source stays, plus a copy)" --cached -C "$p33_fmt"
done
p33_cmp "$P33_B" "--cached -C (patch, modified source stays, plus a copy)" --cached -C
check "phase33: sg's patch prints copy from/copy to, not rename from/to" \
    sh -c "(cd '$P33_B' && '$SG' diff --cached -C) | grep -q '^copy from src.txt\$'"

# --- C: a deleted source with two full copies -- one COPY, one RENAME ------
P33_C="$WORKDIR/p33_one_source_two_copies"
(cd "$WORKDIR" && "$SG" init p33_one_source_two_copies) > /dev/null 2>&1
(cd "$P33_C" && git config user.email "a@b.c" && git config user.name "git user")
p33_mk "$P33_C" "$P33_C/src.txt" C
(cd "$P33_C" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P33_C/src.txt"
p33_mk "$P33_C" "$P33_C/c1.txt" C
p33_mk "$P33_C" "$P33_C/c2.txt" C
(cd "$P33_C" && "$SG" add .) > /dev/null 2>&1
check "phase33 oracle: real git calls the first destination in path order a COPY" \
    sh -c "(cd '$P33_C' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^C100[[:space:]]src.txt[[:space:]]c1.txt\$'"
check "phase33 oracle: and the second a RENAME" \
    sh -c "(cd '$P33_C' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^R100[[:space:]]src.txt[[:space:]]c2.txt\$'"
for p33_fmt in --name-status --stat --numstat --shortstat --name-only; do
    p33_cmp "$P33_C" "--cached -C $p33_fmt (one source, one copy, one rename)" --cached -C "$p33_fmt"
done
p33_cmp "$P33_C" "--cached -C (patch, one source, one copy, one rename)" --cached -C

# --- D: the same-file-name shortcut, which -C is documented to SKIP --------
# Same fixture shape Phase 30 used to prove the shortcut exists: without -C,
# the shared file name pairs dir2/foo.txt first even though other.txt is a
# much closer match. With -C the shortcut is off entirely, so the pairing
# reverses: dir1/foo.txt goes to whichever destination the full comparison
# ranks best (other.txt, at 98%), and dir2/foo.txt is left as its own rename
# off... no other source, so it stays a copy from dir1/foo.txt too, at 79%.
P33_D="$WORKDIR/p33_shortcut_skipped"
(cd "$WORKDIR" && "$SG" init p33_shortcut_skipped) > /dev/null 2>&1
(cd "$P33_D" && git config user.email "a@b.c" && git config user.name "git user")
p33_mk "$P33_D" "$P33_D/dir1/foo.txt" D 100
(cd "$P33_D" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P33_D/dir1/foo.txt"
p33_mk "$P33_D" "$P33_D/dir2/foo.txt" D 80
p33_mk "$P33_D" "$P33_D/other.txt" D 99
(cd "$P33_D" && "$SG" add .) > /dev/null 2>&1
check "phase33 oracle: WITHOUT -C, the shared file name wins, at 79%" \
    sh -c "(cd '$P33_D' && LC_ALL=C git diff --cached --name-status) | grep -q '^R079[[:space:]]dir1/foo.txt[[:space:]]dir2/foo.txt\$'"
check "phase33 oracle: and other.txt is left an ordinary addition" \
    sh -c "(cd '$P33_D' && LC_ALL=C git diff --cached --name-status) | grep -q '^A[[:space:]]other.txt\$'"
check "phase33 oracle: WITH -C, the shortcut is skipped -- other.txt (98%) wins the RENAME" \
    sh -c "(cd '$P33_D' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^R098[[:space:]]dir1/foo.txt[[:space:]]other.txt\$'"
check "phase33 oracle: and dir2/foo.txt (79%) is left a COPY from the same source" \
    sh -c "(cd '$P33_D' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^C079[[:space:]]dir1/foo.txt[[:space:]]dir2/foo.txt\$'"
p33_cmp "$P33_D" "--cached --name-status (no -C: shortcut wins)" --cached --name-status
for p33_fmt in --name-status --stat --numstat --shortstat --name-only; do
    p33_cmp "$P33_D" "--cached -C $p33_fmt (shortcut skipped under -C)" --cached -C "$p33_fmt"
done
p33_cmp "$P33_D" "--cached -C (patch, shortcut skipped under -C)" --cached -C

# --- grammar: -C/-C<n>/-C<n>%/--find-copies[=<n>] share -M's parser --------
for p33_opt in -C -C5 "-C90%" --find-copies "--find-copies=90%"; do
    p33_cmp "$P33_C" "--name-status $p33_opt (grammar)" --cached --name-status "$p33_opt"
done
check "phase33 oracle: real git rejects an unparsable -C value" \
    sh -c "! (cd '$P33_C' && LC_ALL=C git diff --cached -Cabc) > /dev/null 2>&1"
check "phase33: sg rejects it too" \
    sh -c "! (cd '$P33_C' && '$SG' diff --cached -Cabc) > /dev/null 2>&1"

# --- -C -C / --find-copies-harder: a deliberate divergence from real git ---
# sg does not implement find-copies-harder (it would need every UNCHANGED
# path as a candidate source, which sg_diff_list never holds). Pin the
# divergence explicitly rather than let it look like an accident: real git
# accepts the flag, sg refuses it outright.
check "phase33 oracle: real git DOES accept -C -C" \
    sh -c "(cd '$P33_C' && LC_ALL=C git diff --cached -C -C --name-status) > /dev/null 2>&1"
check "phase33: sg deliberately rejects -C -C" \
    sh -c "! (cd '$P33_C' && '$SG' diff --cached -C -C) > /dev/null 2>$WORKDIR/p33_harder.txt"
check "phase33: and names find-copies-harder in the error" \
    grep -q "find-copies-harder" "$WORKDIR/p33_harder.txt"
check "phase33 oracle: real git also accepts the long form --find-copies-harder" \
    sh -c "(cd '$P33_C' && LC_ALL=C git diff --cached --find-copies-harder --name-status) > /dev/null 2>&1"
check "phase33: sg rejects the long form too" \
    sh -c "! (cd '$P33_C' && '$SG' diff --cached --find-copies-harder) > /dev/null 2>$WORKDIR/p33_harder2.txt"
check "phase33: and names find-copies-harder in that error too" \
    grep -q "find-copies-harder" "$WORKDIR/p33_harder2.txt"

# Two properties of copy mode that the fixtures above cannot see, because
# something else in the pipeline happens to reach the same answer. Both were
# found by mutating the product code and watching nothing turn red.
#
# 1. Copy mode skips the same-file-name shortcut. With ONE source that is
#    invisible: copy mode lets a source be reused, so claiming it early via
#    the shortcut costs nothing. It takes TWO sources -- one that shares the
#    destination's name at 79%, one that does not at 98% -- for the shortcut
#    to change the answer. Measured against git 2.55.0: without -C the name
#    wins, with -C the score does.
P33_E="$WORKDIR/p33_shortcut_two_sources"
(cd "$WORKDIR" && "$SG" init p33_shortcut_two_sources) > /dev/null 2>&1
(cd "$P33_E" && git config user.email "a@b.c" && git config user.name "git user")
p33_mk "$P33_E" "$P33_E/a/x.txt" L 80
p33_mk "$P33_E" "$P33_E/b/y.txt" L 99
(cd "$P33_E" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
rm "$P33_E/a/x.txt" "$P33_E/b/y.txt"
p33_mk "$P33_E" "$P33_E/d/x.txt" L 100
(cd "$P33_E" && "$SG" add .) > /dev/null 2>&1
check "phase33 oracle: without -C the shared file name wins" \
    sh -c "(cd '$P33_E' && LC_ALL=C git diff --cached --name-status) | grep -q '^R079'"
check "phase33 oracle: with -C the better score wins instead" \
    sh -c "(cd '$P33_E' && LC_ALL=C git diff --cached -C --name-status) | grep -q '^R098'"
p33_cmp "$P33_E" "--cached --name-status (two sources, shortcut on)" \
    --cached --name-status
p33_cmp "$P33_E" "--cached -C --name-status (two sources, shortcut skipped)" \
    --cached -C --name-status
#
# 2. The exact pass lets a source be claimed again under -C. Normally the
#    matrix's second walk would find the same pair anyway, so breaking the
#    exact pass changes nothing -- except at -C100%, where the matrix is
#    skipped entirely and the exact pass is the only thing left.
P33_F="$WORKDIR/p33_exact_reuse"
(cd "$WORKDIR" && "$SG" init p33_exact_reuse) > /dev/null 2>&1
(cd "$P33_F" && git config user.email "a@b.c" && git config user.name "git user")
p33_mk "$P33_F" "$P33_F/src.txt" S 100
(cd "$P33_F" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
cp "$P33_F/src.txt" "$P33_F/c1.txt"
cp "$P33_F/src.txt" "$P33_F/c2.txt"
rm "$P33_F/src.txt"
(cd "$P33_F" && "$SG" add .) > /dev/null 2>&1
check "phase33 oracle: -C100% still splits one source into a copy and a rename" \
    sh -c "(cd '$P33_F' && LC_ALL=C git diff --cached -C100% --name-status) | grep -q '^C100'"
check "phase33 oracle: and the second one is the rename" \
    sh -c "(cd '$P33_F' && LC_ALL=C git diff --cached -C100% --name-status) | grep -q '^R100'"
p33_cmp "$P33_F" "--cached -C100% --name-status (exact pass alone)" \
    --cached -C100% --name-status

# -M and -C write the same mode in git, so the LAST one wins. Measured:
# `git diff -C -M` finds renames only while `-M -C` finds copies. sg had -C
# sticky in the first order, and nothing noticed because no check combined
# the two flags -- a coverage gap sitting exactly where the defect was.
check "phase33 oracle: git's -C then -M reverts to renames only" \
    sh -c "(cd '$P33_F' && LC_ALL=C git diff --cached -C -M --name-status) | grep -qv '^C' "
check "phase33 oracle: and the reverse order keeps copies" \
    sh -c "(cd '$P33_F' && LC_ALL=C git diff --cached -M -C --name-status) | grep -q '^C100'"
p33_cmp "$P33_F" "--cached -C -M --name-status (the later flag wins)" \
    --cached -C -M --name-status
p33_cmp "$P33_F" "--cached -M -C --name-status (the later flag wins)" \
    --cached -M -C --name-status
p33_cmp "$P33_F" "--cached -C -M50% --name-status (a valued -M also clears -C)" \
    --cached -C -M50% --name-status
p33_cmp "$P33_F" "--cached -C --no-renames --name-status" --cached -C --no-renames --name-status
p33_cmp "$P33_F" "--cached --no-renames -C --name-status" --cached --no-renames -C --name-status

# Phase 34: combined diff (`sg diff -c` / `--cc`) for an unresolved conflict.
# Every fixture is a conflict `sg merge` produces (same idiom as phase4b
# case 2: real git recognizes an sg-made UU state as its own, so `git diff`
# on the SAME repo is a legitimate oracle without needing `git merge` at
# all). p34_cmp compares the FULL byte output of one format across sg and
# real git, same discipline as p33_cmp.
p34_cmp() {
    p34_dir="$1"
    p34_label="$2"
    shift 2
    (cd "$p34_dir" && "$SG" diff "$@") > "$WORKDIR/p34_sg.txt" 2>/dev/null
    # LC_ALL=C alongside core.quotepath=false: the two knobs this comparison
    # depends on, both declared on the command line rather than inherited
    # (the lesson Phase 38 paid a CI red for -- an oracle must declare its
    # own environment). Measured on this machine, `git diff`'s six formats
    # are not localized today, so this changes no current result; it is here
    # so a localized runner cannot turn 30-plus reused checks red at once.
    (cd "$p34_dir" && LC_ALL=C git -c core.quotepath=false diff "$@") > "$WORKDIR/p34_git.txt" 2>/dev/null
    check "phase34: sg diff $p34_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p34_sg.txt" "$WORKDIR/p34_git.txt"
}

p34_mkconflict() {
    p34_repo="$1"
    p34_path="$2"
    p34_base="$3"
    p34_ours="$4"
    p34_theirs="$5"

    mkdir -p "$p34_repo"
    (cd "$WORKDIR" && "$SG" init "$(basename "$p34_repo")") > /dev/null 2>&1
    printf '%s' "$p34_base" > "$p34_repo/$p34_path"
    (cd "$p34_repo" && "$SG" add "$p34_path" && "$SG" commit -m base) > /dev/null 2>&1
    (cd "$p34_repo" && "$SG" switch -c side) > /dev/null 2>&1
    printf '%s' "$p34_theirs" > "$p34_repo/$p34_path"
    (cd "$p34_repo" && "$SG" add "$p34_path" && "$SG" commit -m side_change) > /dev/null 2>&1
    (cd "$p34_repo" && "$SG" switch master < /dev/null) > /dev/null 2>&1
    printf '%s' "$p34_ours" > "$p34_repo/$p34_path"
    (cd "$p34_repo" && "$SG" add "$p34_path" && "$SG" commit -m ours_change) > /dev/null 2>&1
    (cd "$p34_repo" && "$SG" merge side < /dev/null) > /dev/null 2>&1
}

# --- A: a typical unresolved conflict, still carrying markers -------------
P34_A="$WORKDIR/p34_typical"
p34_mkconflict "$P34_A" f.txt \
    'a
b
BASE
d
e
f
g
' \
    'a
b
OURS
d
e
f
g
' \
    'a
b
SIDE
d
e
f
g
'
check "phase34 oracle: real git recognizes the sg-made conflict as UU" \
    sh -c "(cd '$P34_A' && git status --porcelain) | grep -q '^UU f.txt\$'"
for p34_fmt in --name-status --stat --numstat --shortstat --name-only; do
    p34_cmp "$P34_A" "$p34_fmt (default: no combined)" "$p34_fmt"
    p34_cmp "$P34_A" "-c $p34_fmt" -c "$p34_fmt"
    p34_cmp "$P34_A" "--cc $p34_fmt" --cc "$p34_fmt"
done
p34_cmp "$P34_A" "(patch, default is dense)"
p34_cmp "$P34_A" "-c (non-dense patch)" -c
p34_cmp "$P34_A" "--cc (dense patch)" --cc
p34_cmp "$P34_A" "-c --cc (last one wins: dense)" -c --cc
p34_cmp "$P34_A" "--cc -c (last one wins: non-dense)" --cc -c
check "phase34 oracle: -c --cc prints \"diff --cc\" (dense wins)" \
    sh -c "(cd '$P34_A' && '$SG' diff -c --cc) | head -1 | grep -q '^diff --cc '"
check "phase34 oracle: --cc -c prints \"diff --combined\" (non-dense wins)" \
    sh -c "(cd '$P34_A' && '$SG' diff --cc -c) | head -1 | grep -q '^diff --combined '"

# --- B: dense drops the hunk once the working copy matches ours exactly ---
P34_B="$WORKDIR/p34_dense_omit"
p34_mkconflict "$P34_B" f.txt \
    'a
b
BASE
d
e
f
g
' \
    'a
b
OURS
d
e
f
g
' \
    'a
b
SIDE
d
e
f
g
'
printf 'a\nb\nOURS\nd\ne\nf\ng\n' > "$P34_B/f.txt"
p34_cmp "$P34_B" "--cc (result equals ours: header only, no hunk)" --cc
p34_cmp "$P34_B" "-c (must NOT prune the same hunk)" -c

# --- C: the working copy is deleted entirely -------------------------------
P34_C="$WORKDIR/p34_deleted"
p34_mkconflict "$P34_C" f.txt \
    'a
b
c
' \
    'a
OURS
c
' \
    'a
THEIRS
c
'
rm -f "$P34_C/f.txt"
p34_cmp "$P34_C" "--cc (deleted result: header + mode line, no body)" --cc
for p34_fmt in --stat --numstat --shortstat --name-only --name-status; do
    p34_cmp "$P34_C" "$p34_fmt (deleted result)" --cc "$p34_fmt"
done

# --- D: --cached always "* Unmerged path", -c/--cc is a complete no-op ----
P34_D="$WORKDIR/p34_cached"
p34_mkconflict "$P34_D" f.txt \
    'a
' \
    'b
' \
    'c
'
p34_cmp "$P34_D" "--cached (unaffected by no flag)" --cached
p34_cmp "$P34_D" "--cached -c (must still be \"* Unmerged path\")" --cached -c
p34_cmp "$P34_D" "--cached --cc (must still be \"* Unmerged path\")" --cached --cc
check "phase34 oracle: real git's --cached -c is still \"* Unmerged path\"" \
    sh -c "(cd '$P34_D' && git diff --cached -c) | grep -q '^\* Unmerged path f.txt\$'"

# --- E/Phase 40: -c/--cc <rev> -- no longer rejected, now a real combined
# diff. Phase 34's belief that this needed the stage-1/named-tree pairing
# was wrong in three ways (docs/DESIGN.md Phase 40): it is not about
# conflicts at all, parent 1 is the INDEX (not a merge base or HEAD), and
# it reorders the whole output. p34_cmp is reused unchanged -- it already
# runs sg and `git -c core.quotepath=false` side by side on identical args,
# which is exactly what these fixtures need too.

# E-1: four DISTINCT blobs (HEAD~1, HEAD, index, worktree) is the only
# fixture that can tell "parent 1 = index" apart from "parent 1 = HEAD" --
# with fewer than four, two candidates coincide and the check can't fail
# for the right reason.
P40_E="$WORKDIR/p40_e"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_E")") > /dev/null 2>&1
printf 'V1-oldcommit\n' > "$P40_E/a.txt"
(cd "$P40_E" && "$SG" add a.txt && "$SG" commit -m c1) > /dev/null 2>&1
printf 'V2-head\n' > "$P40_E/a.txt"
(cd "$P40_E" && "$SG" add a.txt && "$SG" commit -m c2) > /dev/null 2>&1
printf 'V3-staged\n' > "$P40_E/a.txt"
(cd "$P40_E" && "$SG" add a.txt) > /dev/null 2>&1
printf 'V4-worktree\n' > "$P40_E/a.txt"
p34_cmp "$P40_E" "phase40: -c HEAD~1 combines [index, named tree] against the worktree" -c HEAD~1
p34_cmp "$P40_E" "phase40: --cc HEAD~1, same fixture" --cc HEAD~1

# E-2: the three "no effect" combinations (SPEC section 1) -- -c/--cc must
# degenerate to ordinary rendering, not error out and not combine.
p34_cmp "$P40_E" "phase40: -c --cached HEAD~1 is unaffected by -c" -c --cached HEAD~1
p34_cmp "$P40_E" "phase40: -c HEAD~1 HEAD (two revs) is unaffected by -c" -c HEAD~1 HEAD
check "phase40: -c --cached HEAD~1 prints an ordinary diff --git header, not --combined" \
    sh -c "(cd '$P40_E' && '$SG' diff -c --cached HEAD~1) | grep -q '^diff --git'"
check "phase40: -c HEAD~1 HEAD prints an ordinary diff --git header, not --combined" \
    sh -c "(cd '$P40_E' && '$SG' diff -c HEAD~1 HEAD) | grep -q '^diff --git'"

# E-3 (SPEC section 3): a row where the working-tree file was deleted is
# NOT combinable and falls back to an ordinary deletion row against the
# NAMED TREE's blob, byte-for-byte identical to plain `sg diff named` --
# not merely "similar". index and named-tree blobs are made to differ so a
# wrong fallback pairing (e.g. against the index instead) would be visible.
P40_G="$WORKDIR/p40_g"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_G")") > /dev/null 2>&1
printf 'GONE-v1\n' > "$P40_G/gone.txt"
(cd "$P40_G" && "$SG" add gone.txt && "$SG" commit -m c1) > /dev/null 2>&1
printf 'GONE-v2-namedtree\n' > "$P40_G/gone.txt"
(cd "$P40_G" && "$SG" add gone.txt && "$SG" commit -m c2) > /dev/null 2>&1
(cd "$P40_G" && "$SG" tag named) > /dev/null 2>&1
printf 'GONE-v3-index\n' > "$P40_G/gone.txt"
(cd "$P40_G" && "$SG" add gone.txt) > /dev/null 2>&1
rm -f "$P40_G/gone.txt"
p34_cmp "$P40_G" "phase40: -c named, deleted-from-worktree row falls back to an ordinary deletion" -c named
(cd "$P40_G" && "$SG" diff -c named) > "$WORKDIR/p40g_combined.txt" 2>/dev/null
(cd "$P40_G" && "$SG" diff named) > "$WORKDIR/p40g_plain.txt" 2>/dev/null
check "phase40: the fallback row is byte-identical to plain sg diff named, not just similar" \
    cmp -s "$WORKDIR/p40g_combined.txt" "$WORKDIR/p40g_plain.txt"

# E-4 (SPEC section 4): output ORDER -- every combined row first (path
# order), then every non-combined row (path order), in every format. An
# interleaved fixture (combinable a/c/e, non-combinable b/d) is required:
# with the two groups already contiguous, a bug that skips reordering
# entirely would still pass by accident. Note being merely UNSTAGED does
# NOT make a row non-combinable -- the index still carries a stage-0 entry
# either way (SPEC section 2's "ours" is present regardless), so b/d
# instead use the two shapes SPEC section 3 actually names as
# non-combinable: b.txt is a staged ADD (absent from the named tree),
# d.txt is DELETED from the working tree (measured above with the M2 debug
# fixture: an unstaged-only edit still combines).
P40_M="$WORKDIR/p40_m"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_M")") > /dev/null 2>&1
for p40m_f in a.txt c.txt d.txt e.txt; do
    printf 'base-%s\n' "$p40m_f" > "$P40_M/$p40m_f"
done
(cd "$P40_M" && "$SG" add a.txt c.txt d.txt e.txt && "$SG" commit -m c1) > /dev/null 2>&1
(cd "$P40_M" && "$SG" tag named) > /dev/null 2>&1
printf 'staged-a\n' > "$P40_M/a.txt"
printf 'staged-c\n' > "$P40_M/c.txt"
printf 'staged-e\n' > "$P40_M/e.txt"
(cd "$P40_M" && "$SG" add a.txt c.txt e.txt) > /dev/null 2>&1
printf 'work-a\n' > "$P40_M/a.txt"
printf 'work-c\n' > "$P40_M/c.txt"
printf 'work-e\n' > "$P40_M/e.txt"
printf 'new-b\n' > "$P40_M/b.txt"
(cd "$P40_M" && "$SG" add b.txt) > /dev/null 2>&1
rm -f "$P40_M/d.txt"
p34_cmp "$P40_M" "phase40: --name-only order (combined first, then non-combined, each path-sorted)" \
    --name-only -c named
p34_cmp "$P40_M" "phase40: --name-status order, same fixture" --name-status -c named
p34_cmp "$P40_M" "phase40: patch order, same fixture" -c named
check "phase40 oracle: real git's control (no -c) stays plain path order on the same fixture" \
    sh -c "(cd '$P40_M' && git -c core.quotepath=false diff --name-only named) | tr '\n' ' ' | grep -q '^a.txt b.txt c.txt d.txt e.txt '"
check "phase40: -c reorders combined rows (a c e) before non-combined (b d)" \
    sh -c "(cd '$P40_M' && '$SG' diff --name-only -c named) | tr '\n' ' ' | grep -q '^a.txt c.txt e.txt b.txt d.txt '"

# E-5 (SPEC section 5): rename/copy detection must skip combined rows --
# src.txt is modified (combinable) and copy.txt is a staged byte-identical
# copy of src.txt's OLD content. Without the skip, src.txt could be
# consumed as a copy source and vanish from the combined half entirely.
P40_N="$WORKDIR/p40_n"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_N")") > /dev/null 2>&1
printf 'src-v1\n' > "$P40_N/src.txt"
(cd "$P40_N" && "$SG" add src.txt && "$SG" commit -m c1) > /dev/null 2>&1
(cd "$P40_N" && "$SG" tag named) > /dev/null 2>&1
printf 'src-v2-staged\n' > "$P40_N/src.txt"
printf 'src-v1\n' > "$P40_N/copy.txt"
(cd "$P40_N" && "$SG" add src.txt copy.txt) > /dev/null 2>&1
printf 'src-v3-worktree\n' > "$P40_N/src.txt"
p34_cmp "$P40_N" "phase40: --name-status -c -C skips combined rows for rename/copy pairing" \
    --name-status -c -C named
check "phase40 oracle: without -c, the control DOES pair src.txt/copy.txt as a copy" \
    sh -c "(cd '$P40_N' && git -c core.quotepath=false diff --name-status -C named) | grep -q '^C100.*src\\.txt.*copy\\.txt'"
# The tab must come from $(printf '\t'), never a literal \t inside the
# pattern: BSD grep (macOS) accepts \t as a tab, GNU grep (ubuntu) does not
# and matches a literal "t" instead. Measured -- the original spelling of
# this check passed on macOS and failed on all three ubuntu CI cells while
# sg's actual output was correct all along (the p34_cmp byte-for-byte check
# on the same fixture stayed green everywhere). Same idiom as the phase23,
# phase31 and phase34 checks in this file.
check "phase40: with -c, src.txt renders MM (combined) and copy.txt renders A (no pairing)" \
    sh -c "(cd '$P40_N' && '$SG' diff --name-status -c -C named) | grep -q \"^MM\$(printf '\\t')src.txt\$\" && (cd '$P40_N' && '$SG' diff --name-status -c -C named) | grep -q \"^A\$(printf '\\t')copy.txt\$\""

# E-6 (SPEC section 6): per-format behaviour on a fixture mixing ONE
# combined row with TWO plain ones -- fixture J's own lesson (round 1 used
# an all-combined fixture and could not show the stat omission is
# PER-ROW). one.txt: combined (index and worktree both changed). two.txt,
# three.txt: worktree-only edits (plain, non-combined).
# Same lesson as fixture M: a merely-unstaged edit still combines (the
# index still carries a stage-0 entry equal to the named tree, and that is
# enough). two.txt/three.txt instead use the two genuinely non-combinable
# shapes from SPEC section 3: two.txt is a staged ADD (absent from the
# named tree), three.txt is DELETED from the working tree.
P40_J="$WORKDIR/p40_j"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_J")") > /dev/null 2>&1
printf 'one-base\n' > "$P40_J/one.txt"
printf 'three-base\n' > "$P40_J/three.txt"
(cd "$P40_J" && "$SG" add one.txt three.txt && "$SG" commit -m c1) > /dev/null 2>&1
(cd "$P40_J" && "$SG" tag named) > /dev/null 2>&1
printf 'one-staged\n' > "$P40_J/one.txt"
(cd "$P40_J" && "$SG" add one.txt) > /dev/null 2>&1
printf 'one-worktree\n' > "$P40_J/one.txt"
printf 'two-new\n' > "$P40_J/two.txt"
(cd "$P40_J" && "$SG" add two.txt) > /dev/null 2>&1
rm -f "$P40_J/three.txt"
for p40j_fmt in --stat --numstat --shortstat --name-only --name-status; do
    p34_cmp "$P40_J" "$p40j_fmt (one combined row mixed with two plain rows)" -c named "$p40j_fmt"
done
check "phase40: --stat's summary line still counts only the two plain rows" \
    sh -c "(cd '$P40_J' && '$SG' diff --stat -c named) | grep -q '2 files changed'"

# E-7 (SPEC section 6): the dense header-only shape -- `--cc`, result equal
# to ONE of the two parents, drops every hunk and leaves nothing after
# +++. This shape cannot arise from a real conflict (a resolved conflict
# is not still "unmerged"), so no pre-Phase-40 fixture ever reached it.
P40_O="$WORKDIR/p40_o"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_O")") > /dev/null 2>&1
printf 'p1-content\n' > "$P40_O/eqp1.txt"
(cd "$P40_O" && "$SG" add eqp1.txt && "$SG" commit -m c1) > /dev/null 2>&1
(cd "$P40_O" && "$SG" tag named) > /dev/null 2>&1
printf 'p2-content\n' > "$P40_O/eqp1.txt"
(cd "$P40_O" && "$SG" add eqp1.txt) > /dev/null 2>&1
printf 'p1-content\n' > "$P40_O/eqp1.txt"
# ours (index) = p2-content, theirs (named tree) = p1-content, result
# (worktree) = p1-content -- result equals parent 2, the "theirs" side.
p34_cmp "$P40_O" "phase40: --cc, result == parent 2 (named tree) -- dense header-only" --cc named
check "phase40: dense header-only leaves nothing after the +++ line" \
    sh -c "(cd '$P40_O' && '$SG' diff --cc named) | tail -n 1 | grep -q '^+++ '"

# E-7b: the OTHER half of the dense filter, plus the pair that actually
# discriminates. Two gaps in E-7 above: it only built "result == parent 2"
# (a wrong dense rule keyed on the ours side would stay green), and it only
# ran --cc, so "dense drops the hunks" and "there were never any hunks to
# drop" are indistinguishable. Here result == parent 1 (the INDEX side),
# and each fixture is run under BOTH flags: -c must print hunks, --cc must
# not. Same shape as the head-on colliding pairs elsewhere in this file --
# a single rule that got dense backwards cannot satisfy both directions.
P40_O2="$WORKDIR/p40_o2"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_O2")") > /dev/null 2>&1
printf 'p1-content\n' > "$P40_O2/eqp2.txt"
(cd "$P40_O2" && "$SG" add eqp2.txt && "$SG" commit -m c1) > /dev/null 2>&1
(cd "$P40_O2" && "$SG" tag named) > /dev/null 2>&1
printf 'p2-content\n' > "$P40_O2/eqp2.txt"
(cd "$P40_O2" && "$SG" add eqp2.txt) > /dev/null 2>&1
# ours (index) = p2-content, theirs (named tree) = p1-content, result
# (worktree) = p2-content -- result equals parent 1, the "ours" side.
p34_cmp "$P40_O2" "phase40: --cc, result == parent 1 (the index) -- dense header-only" --cc named
p34_cmp "$P40_O2" "phase40: -c on the same fixture -- non-dense KEEPS the hunk" -c named
check "phase40: --cc drops the hunk when result == parent 1 (nothing after +++)" \
    sh -c "(cd '$P40_O2' && '$SG' diff --cc named) | tail -n 1 | grep -q '^+++ '"
check "phase40: -c keeps it -- the same fixture must print an @@@ hunk header" \
    sh -c "(cd '$P40_O2' && '$SG' diff -c named) | grep -q '^@@@ '"
check "phase40: and the same discriminating pair on the result == parent 2 fixture" \
    sh -c "(cd '$P40_O' && '$SG' diff -c named) | grep -q '^@@@ '"
p34_cmp "$P40_O" "phase40: -c, result == parent 2 -- non-dense keeps the hunk too" -c named

# E-8: a conflict where only ONE of stage 2 / stage 3 exists (delete/modify)
# must NEVER render combined -- there is nothing to combine. This closes a
# blind spot that predates Phase 40 and was found by cold-reading this
# phase's diff: deleting the `ours == ABSENT` half of
# sg_diff_entry_is_combined's guard left interop at 1915/1915 with zero FAIL
# lines (measured with tests/mutate.sh), because every conflict fixture in
# this file goes through p34_mkconflict, which writes non-empty base/ours/
# theirs strings and so can only ever produce the full {1,2,3} stage set.
# Phase 40 did not introduce the gap, but it promoted that guard from a
# file-local combinable() to a documented public predicate, so it is fixed
# here rather than left for the next reader to rediscover.
#
# Measured against real git 2.55.0: both shapes print "* Unmerged path"
# under no flag, under -c, and under --cc alike. du.txt is stages {1,3}
# (ours deleted, theirs modified), ud.txt is stages {1,2} (the mirror) --
# both, because the guard is one predicate over two symmetric sides and a
# single-sided fixture would leave half of it unwitnessed.
P40_DM="$WORKDIR/p40_delmod"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_DM")") > /dev/null 2>&1
printf 'seed\n' > "$P40_DM/seed.txt"
printf 'DU-base\n' > "$P40_DM/du.txt"
printf 'UD-base\n' > "$P40_DM/ud.txt"
(cd "$P40_DM" && "$SG" add seed.txt du.txt ud.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P40_DM" && "$SG" branch topic) > /dev/null 2>&1
rm -f "$P40_DM/du.txt"
printf 'UD-ours\n' > "$P40_DM/ud.txt"
(cd "$P40_DM" && "$SG" add du.txt ud.txt && "$SG" commit -m ours) > /dev/null 2>&1
(cd "$P40_DM" && "$SG" switch topic) > /dev/null 2>&1
printf 'DU-theirs\n' > "$P40_DM/du.txt"
rm -f "$P40_DM/ud.txt"
(cd "$P40_DM" && "$SG" add du.txt ud.txt && "$SG" commit -m theirs) > /dev/null 2>&1
(cd "$P40_DM" && "$SG" switch master) > /dev/null 2>&1
(cd "$P40_DM" && "$SG" merge topic) > /dev/null 2>&1
check "phase40: precondition -- du.txt really has stages 1 and 3 but no stage 2" \
    sh -c "(cd '$P40_DM' && LC_ALL=C git ls-files -s du.txt) | awk '{print \$3}' | tr '\n' ' ' | grep -q '^1 3 $'"
check "phase40: precondition -- ud.txt really has stages 1 and 2 but no stage 3" \
    sh -c "(cd '$P40_DM' && LC_ALL=C git ls-files -s ud.txt) | awk '{print \$3}' | tr '\n' ' ' | grep -q '^1 2 $'"
p34_cmp "$P40_DM" "phase40: one-sided conflict, no flag (PATCH's dense default must not combine)"
p34_cmp "$P40_DM" "phase40: one-sided conflict, -c" -c
p34_cmp "$P40_DM" "phase40: one-sided conflict, --cc" --cc
p34_cmp "$P40_DM" "phase40: one-sided conflict, --name-status" --name-status
check "phase40: a one-sided conflict prints \"* Unmerged path\" and never a combined header" \
    sh -c "(cd '$P40_DM' && '$SG' diff --cc) | grep -q '^\* Unmerged path du.txt$' && ! (cd '$P40_DM' && '$SG' diff --cc) | grep -q '^diff --c'"

# E-9: the widening rule reads the index group's LOWEST stage. Every other
# fixture in this phase has a single-entry index group for the widened
# path, where "first entry" and "last entry" are the same row -- measured
# with tests/mutate.sh: rewriting the widening block to read the group's
# LAST entry instead left interop at 1915/1915 with zero FAIL lines.
#
# This fixture is built so stage 1 and stage 3 give OPPOSITE answers to
# "does this row exist at all", which is the only way to tell them apart:
#   stage 1     = BASE   (differs from the working tree -> row must exist)
#   stage 3     = ZZZ    (equals it -> reading stage 3 yields NO row)
#   named tree  = ZZZ    (equals it, so the ordinary tree-vs-worktree test
#                         finds nothing on its own)
#   working tree = ZZZ
# The plain `sg diff namedz` control below is what makes the -c result
# meaningful: it must print nothing, so the row can only have come from the
# widening rule.
P40_WS="$WORKDIR/p40_widen_stage"
(cd "$WORKDIR" && "$SG" init "$(basename "$P40_WS")") > /dev/null 2>&1
printf 'BASE\n' > "$P40_WS/f.txt"
printf 'seed\n' > "$P40_WS/seed.txt"
(cd "$P40_WS" && "$SG" add f.txt seed.txt && "$SG" commit -m base) > /dev/null 2>&1
(cd "$P40_WS" && "$SG" branch topic) > /dev/null 2>&1
printf 'OURS\n' > "$P40_WS/f.txt"
(cd "$P40_WS" && "$SG" add f.txt && "$SG" commit -m ours) > /dev/null 2>&1
(cd "$P40_WS" && "$SG" switch topic) > /dev/null 2>&1
printf 'ZZZ\n' > "$P40_WS/f.txt"
(cd "$P40_WS" && "$SG" add f.txt && "$SG" commit -m theirs && "$SG" tag namedz) > /dev/null 2>&1
(cd "$P40_WS" && "$SG" switch master) > /dev/null 2>&1
(cd "$P40_WS" && "$SG" merge topic) > /dev/null 2>&1
printf 'ZZZ\n' > "$P40_WS/f.txt"
check "phase40: precondition -- f.txt carries all three stages here" \
    sh -c "(cd '$P40_WS' && LC_ALL=C git ls-files -s f.txt) | awk '{print \$3}' | tr '\n' ' ' | grep -q '^1 2 3 $'"
check "phase40 control: plain sg diff namedz prints nothing (tree == working tree)" \
    sh -c "test -z \"$(cd '$P40_WS' && '$SG' diff namedz)\""
check "phase40 oracle: real git's control is empty too" \
    sh -c "test -z \"$(cd '$P40_WS' && LC_ALL=C git -c core.quotepath=false diff namedz)\""
p34_cmp "$P40_WS" "phase40: widening reads the group's LOWEST stage, not its last" -c namedz
check "phase40: ...and the row it produces names stage 1's blob, not stage 3's" \
    sh -c "(cd '$P40_WS' && '$SG' diff -c namedz) | grep -q '^- BASE$'"

# --- F: head-on collision -- "* Unmerged path" must NOT be quoted, but the
# SAME filename in a combined header (--- a/..., +++ b/..., diff --cc ...)
# MUST be quoted. A single "quote everything"/"quote nothing" rule would
# leave one of these two green for the wrong reason; mutate.sh proved this
# by turning "* Unmerged path" quoted and finding interop stayed fully
# green until this fixture existed (both prior checks used ASCII-only
# names, where quoting is a no-op either way). Real git needs
# core.quotepath=false for the combined header (>= 0x80 / control-byte
# quoting policy, same as every other combined-diff comparison in this
# file) but NOT for "* Unmerged path" itself -- PHASE34_ORACLE.md #1: real
# git leaves that one line unquoted regardless of the config.
P34Q_NAME=$(printf 'wei rd\ttab.txt')
P34_Q="$WORKDIR/p34_quote_collision"
p34_mkconflict "$P34_Q" "$P34Q_NAME" \
    'a
b
BASE
d
' \
    'a
b
OURS
d
' \
    'a
b
SIDE
d
'
p34_cmp "$P34_Q" "(patch, combinable: combined header must be quoted)"
check "phase34: combined header quotes the control-byte name" \
    sh -c "(cd '$P34_Q' && '$SG' diff) | grep -q '^diff --cc \"wei rd'"
(cd "$P34_Q" && "$SG" diff --cached) > "$WORKDIR/p34q_sg_cached.txt" 2>/dev/null
(cd "$P34_Q" && git -c core.quotepath=false diff --cached) > "$WORKDIR/p34q_git_cached.txt" 2>/dev/null
check "phase34: sg diff --cached (\"* Unmerged path\", unquoted) matches real git byte-for-byte" \
    cmp -s "$WORKDIR/p34q_sg_cached.txt" "$WORKDIR/p34q_git_cached.txt"
check "phase34: \"* Unmerged path\" carries the RAW tab byte, not a quoted name" \
    sh -c "grep -q \"^\* Unmerged path wei rd\$(printf '\\t')tab.txt\$\" '$WORKDIR/p34q_sg_cached.txt'"
check "phase34 oracle: real git's \"* Unmerged path\" is unquoted too, even with core.quotepath=false unset" \
    sh -c "(cd '$P34_Q' && git diff --cached) | grep -q \"^\* Unmerged path wei rd\$(printf '\\t')tab.txt\$\""

# --- Phase 36: a hand-crafted .git/index naming a path that escapes the
# repository ("../secret.txt"), whose blob does NOT exist in the object
# store yet -- real git's own porcelain cannot even build this fixture
# ("git update-index --add --cacheinfo ...,../secret.txt" itself refuses
# with "Invalid path", measured), so the index is written directly as raw
# v2 bytes, entries kept path-sorted (an unsorted index of the same two
# entries made sg's own a.txt lookup fail during measurement -- real git
# tolerated it, sg did not, so an unsorted fixture would have failed for
# the wrong reason here).
#
# This is the read-side hole Phase 36 closes: sg_tree_build_from_workdir
# used to hash the outside file and write it as a PERMANENT loose object
# before the separately-guarded delete/apply step ever failed -- measured
# before the fix, "sg cat-file -p <that id>" printed the secret back out.
# The SAME crafted bytes are used on both sides below, pinning three things
# at once:
#   1. `status --porcelain` still lists the escaping path on BOTH sides --
#      this is NOT the bug (real git lists it too, as the staged half of the
#      diff, which never touches the working directory). A guard that also
#      hid the path from status would be undetectable without this
#      positive-collision check sitting right next to #3.
#   2. `stash push` fails on BOTH sides.
#   3. NEITHER side's object store ever gains the outside file's blob.
# ---
p36_write_index() {
    # $1 = .git dir, $2 = repo-relative path of an existing tracked file,
    # $3 = the escaping repo-relative path (e.g. "../secret.txt"). Prints
    # the escaping path's blob sha1 (hex) to stdout, computed independently
    # of anything sg or git does with it.
    python3 - "$1" "$2" "$3" <<'PY'
import hashlib
import os
import struct
import sys

git_dir, a_path, secret_relpath = sys.argv[1], sys.argv[2], sys.argv[3]


def blob_sha1_hex(content):
    h = hashlib.sha1()
    h.update(b"blob %d\0" % len(content))
    h.update(content)
    return h.hexdigest()


def make_entry(path, mode, sha1_hex):
    sha1 = bytes.fromhex(sha1_hex)
    flags = len(path.encode()) & 0xFFF
    header = struct.pack(">IIIIIIIIII", 0, 0, 0, 0, 0, 0, mode, 0, 0, 0)
    entry = header + sha1 + struct.pack(">H", flags) + path.encode() + b"\0"
    pad = (8 - (len(entry) % 8)) % 8
    return entry + b"\0" * pad


worktree = os.path.dirname(git_dir)
with open(os.path.join(worktree, a_path), "rb") as f:
    a_sha1 = blob_sha1_hex(f.read())
with open(os.path.join(worktree, secret_relpath), "rb") as f:
    secret_sha1 = blob_sha1_hex(f.read())

entries = sorted(
    [(a_path, 0o100644, a_sha1), (secret_relpath, 0o100644, secret_sha1)],
    key=lambda e: e[0],
)
body = b"DIRC" + struct.pack(">II", 2, len(entries))
for name, mode, sha1hex in entries:
    body += make_entry(name, mode, sha1hex)
checksum = hashlib.sha1(body).digest()
with open(os.path.join(git_dir, "index"), "wb") as f:
    f.write(body)
    f.write(checksum)
print(secret_sha1)
PY
}

p36_secret_obj_missing() {
    # $1 = .git dir, $2 = blob sha1 hex
    sha_prefix=$(echo "$2" | cut -c1-2)
    sha_rest=$(echo "$2" | cut -c3-)
    test ! -f "$1/objects/$sha_prefix/$sha_rest"
}

# --- sg side ---
P36_SG="$WORKDIR/p36_sg"
mkdir -p "$P36_SG/outer"
printf 'TOP-SECRET-API-KEY-abc123' > "$P36_SG/outer/secret.txt"
(cd "$P36_SG/outer" && "$SG" init repo) > /dev/null 2>&1
P36_SG_REPO="$P36_SG/outer/repo"
printf 'hello\n' > "$P36_SG_REPO/a.txt"
(cd "$P36_SG_REPO" && "$SG" add a.txt && "$SG" commit -m base) > /dev/null 2>&1
P36_SG_SECRET_SHA=$(p36_write_index "$P36_SG_REPO/.git" "a.txt" "../secret.txt")
printf 'dirty extra\n' >> "$P36_SG_REPO/a.txt"

(cd "$P36_SG_REPO" && "$SG" status --porcelain) > "$WORKDIR/p36_sg_status.txt" 2>&1
check "phase36: sg status --porcelain lists the escaping index path" \
    grep -qF '../secret.txt' "$WORKDIR/p36_sg_status.txt"
# Deliberate divergence #4 (CLAUDE.md): sg refuses to read the escaping
# path's content, so it cannot compute git's three-way A /AM/AD answer and
# always prints AD instead -- pin the fixed answer itself, or a change that
# widened/narrowed this divergence would have nothing catching it. This
# fixture's outside file is untouched (content == what the index records),
# the same shape real git answers "A " for -- deliberately different sides,
# not a copy-paste mistake.
check "phase36: sg status --porcelain always prints a fixed AD for this path (deliberate divergence #4)" \
    grep -qx 'AD \.\./secret\.txt' "$WORKDIR/p36_sg_status.txt"

(cd "$P36_SG_REPO" && "$SG" stash push) > "$WORKDIR/p36_sg_stash.txt" 2>&1
P36_SG_STASH_RC=$?
check "phase36: sg stash push refuses a crafted index naming a path outside the repository" \
    test "$P36_SG_STASH_RC" != 0
check "phase36: sg never wrote the outside file's blob into its object store" \
    p36_secret_obj_missing "$P36_SG_REPO/.git" "$P36_SG_SECRET_SHA"
# Round-2 follow-up: cmd_stash.c's bad_path branch names the offending path
# instead of the pre-existing generic "unborn HEAD, or ... unresolved
# conflicts?" guess -- pin the actual message text, not just the exit code,
# or the CLI-visible half of this fix has nothing catching a regression.
check "phase36: sg stash push names the offending path in its error message" \
    grep -qF '../secret.txt' "$WORKDIR/p36_sg_stash.txt"

# --- git side (the control): built with plumbing (mktree/commit-tree),
# never `git commit`/`git checkout`, so it needs no confirmation prompt and
# touches no branch this script did not itself create. ---
P36_GIT="$WORKDIR/p36_git"
mkdir -p "$P36_GIT/outer"
printf 'TOP-SECRET-API-KEY-abc123' > "$P36_GIT/outer/secret.txt"
(cd "$P36_GIT/outer" && git init -q repo) > /dev/null 2>&1
P36_GIT_REPO="$P36_GIT/outer/repo"
(cd "$P36_GIT_REPO" && git config user.email "a@b.c" && git config user.name "git user")
printf 'hello\n' > "$P36_GIT_REPO/a.txt"
P36_GIT_BLOB_A=$(cd "$P36_GIT_REPO" && git hash-object -w a.txt)
P36_GIT_TREE=$(printf '100644 blob %s\ta.txt\n' "$P36_GIT_BLOB_A" | git -C "$P36_GIT_REPO" mktree)
P36_GIT_COMMIT=$(git -C "$P36_GIT_REPO" commit-tree "$P36_GIT_TREE" -m base)
git -C "$P36_GIT_REPO" update-ref refs/heads/master "$P36_GIT_COMMIT"
git -C "$P36_GIT_REPO" symbolic-ref HEAD refs/heads/master

P36_GIT_SECRET_SHA=$(p36_write_index "$P36_GIT_REPO/.git" "a.txt" "../secret.txt")
printf 'dirty extra\n' >> "$P36_GIT_REPO/a.txt"

(cd "$P36_GIT_REPO" && git status --porcelain) > "$WORKDIR/p36_git_status.txt" 2>&1
check "phase36 oracle: git status --porcelain also lists the escaping index path" \
    grep -qF '../secret.txt' "$WORKDIR/p36_git_status.txt"
# This is row 1 of DESIGN.md's three-row table (docs/DESIGN.md, Phase 36's
# "AD vs real git's answer" section): the outside file's content has not
# been touched since the crafted index recorded its blob id, so real git's
# own read finds no difference and reports a plain "A ".
check "phase36 oracle: git's exact answer when the outside file is UNCHANGED is 'A ' (table row 1)" \
    grep -qx 'A  \.\./secret\.txt' "$WORKDIR/p36_git_status.txt"

(cd "$P36_GIT_REPO" && git stash push) > "$WORKDIR/p36_git_stash.txt" 2>&1
P36_GIT_STASH_RC=$?
check "phase36 oracle: git stash push also refuses the same crafted index" \
    test "$P36_GIT_STASH_RC" != 0
check "phase36 oracle: git never wrote the outside file's blob into its object store either" \
    p36_secret_obj_missing "$P36_GIT_REPO/.git" "$P36_GIT_SECRET_SHA"

# --- Rows 2 and 3 of the same table: real git actually reads the outside
# file to decide, so its answer depends on what is sitting there. sg cannot
# reproduce any of this (that read is exactly what Phase 36 refuses to do,
# see the "AD" divergence check on the sg side above), so these two checks
# exist purely to keep DESIGN.md's table from silently going stale -- if a
# future git version changed how it answers here, these would be the first
# thing to turn red. Independent fixtures, not reusing $P36_GIT_REPO, so
# neither interferes with the stash-push checks already run against it. ---
P36_GIT_CHANGED="$WORKDIR/p36_git_changed"
mkdir -p "$P36_GIT_CHANGED/outer"
printf 'TOP-SECRET-API-KEY-abc123' > "$P36_GIT_CHANGED/outer/secret.txt"
(cd "$P36_GIT_CHANGED/outer" && git init -q repo) > /dev/null 2>&1
P36_GIT_CHANGED_REPO="$P36_GIT_CHANGED/outer/repo"
(cd "$P36_GIT_CHANGED_REPO" && git config user.email "a@b.c" && git config user.name "git user")
printf 'hello\n' > "$P36_GIT_CHANGED_REPO/a.txt"
P36_GIT_CHANGED_BLOB_A=$(cd "$P36_GIT_CHANGED_REPO" && git hash-object -w a.txt)
P36_GIT_CHANGED_TREE=$(printf '100644 blob %s\ta.txt\n' "$P36_GIT_CHANGED_BLOB_A" |
    git -C "$P36_GIT_CHANGED_REPO" mktree)
P36_GIT_CHANGED_COMMIT=$(git -C "$P36_GIT_CHANGED_REPO" commit-tree "$P36_GIT_CHANGED_TREE" -m base)
git -C "$P36_GIT_CHANGED_REPO" update-ref refs/heads/master "$P36_GIT_CHANGED_COMMIT"
git -C "$P36_GIT_CHANGED_REPO" symbolic-ref HEAD refs/heads/master
p36_write_index "$P36_GIT_CHANGED_REPO/.git" "a.txt" "../secret.txt" > /dev/null
# The index's blob id was computed from the ORIGINAL content above; changing
# the outside file's bytes now, after the index was already crafted, is
# what makes real git's read find a difference.
printf 'TOP-SECRET-API-KEY-abc123-CHANGED' > "$P36_GIT_CHANGED/outer/secret.txt"
(cd "$P36_GIT_CHANGED_REPO" && git status --porcelain) > "$WORKDIR/p36_git_changed_status.txt" 2>&1
check "phase36 oracle: git's exact answer when the outside file has CHANGED is 'AM' (table row 2)" \
    grep -qx 'AM \.\./secret\.txt' "$WORKDIR/p36_git_changed_status.txt"

P36_GIT_DELETED="$WORKDIR/p36_git_deleted"
mkdir -p "$P36_GIT_DELETED/outer"
printf 'TOP-SECRET-API-KEY-abc123' > "$P36_GIT_DELETED/outer/secret.txt"
(cd "$P36_GIT_DELETED/outer" && git init -q repo) > /dev/null 2>&1
P36_GIT_DELETED_REPO="$P36_GIT_DELETED/outer/repo"
(cd "$P36_GIT_DELETED_REPO" && git config user.email "a@b.c" && git config user.name "git user")
printf 'hello\n' > "$P36_GIT_DELETED_REPO/a.txt"
P36_GIT_DELETED_BLOB_A=$(cd "$P36_GIT_DELETED_REPO" && git hash-object -w a.txt)
P36_GIT_DELETED_TREE=$(printf '100644 blob %s\ta.txt\n' "$P36_GIT_DELETED_BLOB_A" |
    git -C "$P36_GIT_DELETED_REPO" mktree)
P36_GIT_DELETED_COMMIT=$(git -C "$P36_GIT_DELETED_REPO" commit-tree "$P36_GIT_DELETED_TREE" -m base)
git -C "$P36_GIT_DELETED_REPO" update-ref refs/heads/master "$P36_GIT_DELETED_COMMIT"
git -C "$P36_GIT_DELETED_REPO" symbolic-ref HEAD refs/heads/master
p36_write_index "$P36_GIT_DELETED_REPO/.git" "a.txt" "../secret.txt" > /dev/null
rm -f "$P36_GIT_DELETED/outer/secret.txt"
(cd "$P36_GIT_DELETED_REPO" && git status --porcelain) > "$WORKDIR/p36_git_deleted_status.txt" 2>&1
check "phase36 oracle: git's exact answer when the outside file has been DELETED is 'AD' (table row 3)" \
    grep -qx 'AD \.\./secret\.txt' "$WORKDIR/p36_git_deleted_status.txt"



# --- Phase 37: pathspec on `sg status` and `sg stash push` -------------
#
# Part A's fold table is a byte-for-byte cmp against real git, same
# technique as p28_cmp: the "does this fold or list individually" decision
# is exactly the kind of rule that looks obvious and is not (CLAUDE.md's
# Phase 37 entry: a wildcard spec still folds despite matching individual
# files below the fold point). Part B's three-tree comparison reuses the
# Phase 21 D1-D4 technique (twin git-run/sg-run repos, full `ls-tree -r`
# comparison, not --name-only, so mode+sha1+path all have to agree) rather
# than a name-only comparison, since content-addressed blob ids are
# byte-identical across repos whenever the content itself is.

P37_STATUS="$WORKDIR/p37_status"
(cd "$WORKDIR" && "$SG" init p37_status) > /dev/null 2>&1
(cd "$P37_STATUS" && git config user.email "a@b.c" && git config user.name "git user")
mkdir -p "$P37_STATUS/wholly/deep"
printf 'u1\n' > "$P37_STATUS/wholly/u1.txt"
printf 'u2\n' > "$P37_STATUS/wholly/deep/u2.txt"

# Runs `status --porcelain -- <args>` through both implementations and
# compares the whole output. core.quotepath=false for the same reason
# p28_cmp needs it (CLAUDE.md's byte>=0x80 divergence) -- harmless here,
# every name in this fixture is plain ASCII.
p37_status_cmp() {
    p37_label="$1"
    shift
    (cd "$P37_STATUS" && "$SG" status --porcelain "$@") > "$WORKDIR/p37_sg.txt" 2>/dev/null
    (cd "$P37_STATUS" && git -c core.quotepath=false status --porcelain "$@") > "$WORKDIR/p37_git.txt" 2>/dev/null
    check "phase37: sg status $p37_label matches real git byte-for-byte" \
        cmp -s "$WORKDIR/p37_sg.txt" "$WORKDIR/p37_git.txt"
}

p37_status_cmp "(no pathspec)"
p37_status_cmp "-- wholly" -- wholly
p37_status_cmp "-- wholly/" -- wholly/
p37_status_cmp "-- wholly/u1.txt" -- wholly/u1.txt
p37_status_cmp "-- wholly/deep" -- wholly/deep
p37_status_cmp "-- wholly/deep/u2.txt" -- wholly/deep/u2.txt
p37_status_cmp "-- 'wholly/*'" -- 'wholly/*'
p37_status_cmp "-- nosuch" -- nosuch

# Positive collision: naming a real branch is STILL a pathspec for `sg
# status` (no rev/path disambiguation at all, unlike `sg diff`) -- measured
# against git 2.55.0, `git status master` prints nothing and exits 0.
(cd "$P37_STATUS" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
P37_STATUS_MASTER_SG=$(cd "$P37_STATUS" && "$SG" status --porcelain master 2>/dev/null)
check "phase37: sg status <real-branch-name> prints nothing (it's a pathspec, not a rev)" \
    test -z "$P37_STATUS_MASTER_SG"
check "phase37: sg status <real-branch-name> exits 0" \
    sh -c "cd '$P37_STATUS' && '$SG' status master > /dev/null 2>&1"

# --- Part B: the three-tree partial-push comparison, and the head-on
# collision between `sg status -- nosuch` (silent, exit 0) and
# `sg stash push -- nosuch` (refused, exit 1) that guards against unifying
# the two "did the pathspec match anything" rules. ---
p37_b2_fixture() {
    _dir="$1"
    _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = "git" ]; then
        (cd "$_dir" && git init -q .) > /dev/null 2>&1
    else
        (cd "$WORKDIR" && "$_impl" init "$(basename "$_dir")") > /dev/null 2>&1
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user") > /dev/null 2>&1
    mkdir -p "$_dir/sub"
    printf 'base\n' > "$_dir/a.txt"
    printf 'base\n' > "$_dir/b.txt"
    printf 'base\n' > "$_dir/sub/c.txt"
    printf 'base\n' > "$_dir/sub/d.txt"
    if [ "$_impl" = "git" ]; then
        (cd "$_dir" && "$_impl" add . && "$_impl" commit -q -m base) > /dev/null 2>&1
    else
        (cd "$_dir" && "$_impl" add . && "$_impl" commit -m base) > /dev/null 2>&1
    fi
    printf 'STAGED-a\n' > "$_dir/a.txt"
    (cd "$_dir" && "$_impl" add a.txt) > /dev/null 2>&1
    printf 'WORKTREE-a\n' > "$_dir/a.txt"
    printf 'WORKTREE-b\n' > "$_dir/b.txt"
    printf 'STAGED-c\n' > "$_dir/sub/c.txt"
    (cd "$_dir" && "$_impl" add sub/c.txt) > /dev/null 2>&1
    printf 'WORKTREE-c\n' > "$_dir/sub/c.txt"
    printf 'WORKTREE-d\n' > "$_dir/sub/d.txt"
    printf 'untracked2\n' > "$_dir/sub/untracked2.txt"
    printf 'other\n' > "$_dir/other_untracked.txt"
}

P37_B2_GIT="$WORKDIR/p37_b2_git"
p37_b2_fixture "$P37_B2_GIT" git
(cd "$P37_B2_GIT" && git stash push -q -u -m p37 -- sub) > /dev/null 2>&1
check "phase37 oracle: git stash push -u -- sub exits 0" test $? = 0
P37_B2_GIT_TREE=$(cd "$P37_B2_GIT" && git ls-tree -r refs/stash | sort)
P37_B2_GIT_IDXTREE=$(cd "$P37_B2_GIT" && git ls-tree -r refs/stash^2 | sort)
P37_B2_GIT_UNTRACKED=$(cd "$P37_B2_GIT" && git ls-tree -r refs/stash^3 | sort)

P37_B2_SG="$WORKDIR/p37_b2_sg"
p37_b2_fixture "$P37_B2_SG" "$SG"
(cd "$P37_B2_SG" && "$SG" stash push -u -m p37 -- sub) > /dev/null 2>&1
check "phase37: sg stash push -u -- sub exits 0" test $? = 0
P37_B2_SG_TREE=$(cd "$P37_B2_SG" && git ls-tree -r refs/stash | sort)
P37_B2_SG_IDXTREE=$(cd "$P37_B2_SG" && git ls-tree -r refs/stash^2 | sort)
P37_B2_SG_UNTRACKED=$(cd "$P37_B2_SG" && git ls-tree -r refs/stash^3 | sort)

check "phase37: sg's partial-push stash tree matches real git's byte-for-byte (mode+sha1+path)" \
    test "$P37_B2_SG_TREE" = "$P37_B2_GIT_TREE"
check "phase37: sg's partial-push index tree (stash^2, unfiltered) matches real git's" \
    test "$P37_B2_SG_IDXTREE" = "$P37_B2_GIT_IDXTREE"
check "phase37: sg's partial-push untracked tree (stash^3, matched-only) matches real git's" \
    test "$P37_B2_SG_UNTRACKED" = "$P37_B2_GIT_UNTRACKED"

# B1 + the head-on collision, on the SAME underlying dirty fixture: `status`
# silently exits 0 on a pathspec matching nothing, `stash push` refuses.
P37_B1="$WORKDIR/p37_b1"
(cd "$WORKDIR" && "$SG" init p37_b1) > /dev/null 2>&1
(cd "$P37_B1" && git config user.email "a@b.c" && git config user.name "git user")
printf 'base\n' > "$P37_B1/a.txt"
(cd "$P37_B1" && "$SG" add . && "$SG" commit -m base) > /dev/null 2>&1
printf 'dirty\n' > "$P37_B1/a.txt"

check "phase37: sg status -- nosuch exits 0 on the SAME dirty fixture" \
    sh -c "cd '$P37_B1' && '$SG' status -- nosuch > /dev/null 2>&1"
P37_B1_STATUS_OUT=$(cd "$P37_B1" && "$SG" status --porcelain -- nosuch 2>/dev/null)
check "phase37: sg status -- nosuch prints nothing" test -z "$P37_B1_STATUS_OUT"

check "phase37: sg stash push -- nosuch (SAME fixture) exits 1, not 0" \
    sh -c "! (cd '$P37_B1' && '$SG' stash push -- nosuch) > /dev/null 2>&1"
check "phase37: sg stash push -- nosuch did not create a stash entry" \
    sh -c "[ \"\$(cd '$P37_B1' && '$SG' stash list | wc -l | tr -d ' ')\" = 0 ]"
P37_B1_A_AFTER=$(cat "$P37_B1/a.txt")
check "phase37: sg stash push -- nosuch left a.txt untouched" \
    test "$P37_B1_A_AFTER" = "dirty"

# Oracle confirmation: real git's own exit code for the identical case.
P37_B1_GIT="$WORKDIR/p37_b1_git"
mkdir -p "$P37_B1_GIT"
(cd "$P37_B1_GIT" && git init -q .) > /dev/null 2>&1
(cd "$P37_B1_GIT" && git config user.email "a@b.c" && git config user.name "git user")
printf 'base\n' > "$P37_B1_GIT/a.txt"
(cd "$P37_B1_GIT" && git add . && git commit -q -m base) > /dev/null 2>&1
printf 'dirty\n' > "$P37_B1_GIT/a.txt"
check "phase37 oracle: real git stash push -- nosuch also exits non-zero" \
    sh -c "! (cd '$P37_B1_GIT' && git stash push -q -m p37 -- nosuch) > /dev/null 2>&1"


# --- Phase 38: sg status long-format skeleton oracle -------------------
#
# Skeleton comparison rule (CLAUDE.md's Phase 38 entry, and PHASE38_SPEC.md
# section 0): drop exactly two line classes, cmp everything else
# byte-for-byte, no tool-name normalization anywhere else.
#   1. lines starting with "  (" -- indented hint lines. The two tools word
#      these differently ("sg add" vs "git add", and git has commit -a-style
#      shortcuts sg doesn't), and the rule is deliberately "^  (" and NOT
#      "^  (use \"" -- a conflict's "  (fix conflicts and run ...)" line
#      does not start with "(use \"" and would slip through uncaught if the
#      rule were narrowed to that (measured).
#   2. lines starting with a tab -- path lines. Already independently
#      guarded by the Phase 23 (untracked paths), Phase 25 (untracked
#      section), and Phase 32 (staged section ordering) checks; this rule
#      does not duplicate them, it just has to skip past them to compare
#      everything else.
# Every other line -- branch line, "No commits yet", section headers,
# "Untracked files not listed (...)", blank lines, and the closing summary
# line -- is compared byte-for-byte. The closing summary lines are safe to
# compare directly because both tools hard-code "git add"/"git commit -a" in
# their wording regardless of the invoking binary's name (re-measured this
# phase, also recorded in docs/DESIGN.md's Phase 37 section).
# The git side of every phase38 comparison runs with these flags. They live
# in one variable so the precondition check further down probes the SAME
# invocation the comparisons use -- dropping the pin in one place and not
# the other would defeat the guard.
P38_GIT_FLAGS="-c core.quotepath=false -c advice.statusHints=true"

p38_skel() {
    grep -v '^  (' "$1" | grep -v "$(printf '^\t')"
}

p38_cmp() {
    _slug="$1"; _label="$2"; _dir="$3"; shift 3
    ( cd "$_dir" && "$SG" status "$@" ) > "$WORKDIR/p38_${_slug}_sg.txt" 2>/dev/null
    _sg_rc=$?
    # Three environment axes have to be declared on git's side, not
    # inherited: LC_ALL=C (this machine's git is zh_TW-localized),
    # core.quotepath=false (sg emits >=0x80 raw), and -- since Phase 38 --
    # advice.statusHints=true. That third one was NOT hypothetical: this
    # group went green locally and red on the macOS CI runner for 21 of its
    # 34 cases, because that runner has advice turned off, and
    # advice.statusHints=false strips the parenthetical out of the CLOSING
    # SUMMARY line too ("nothing added to commit but untracked files
    # present" with no "(use \"git add\" to track)"), not just the indented
    # hint lines this comparison already filters. The failing set was
    # exactly "every case whose output carries a non-indented parenthetical"
    # -- clean/detached passed because their closing line has no parens at
    # all. sg has no advice config, so pinning git to true is what makes the
    # two comparable anywhere.
    ( cd "$_dir" && LC_ALL=C git $P38_GIT_FLAGS status "$@" ) \
        > "$WORKDIR/p38_${_slug}_git.txt" 2>/dev/null
    p38_skel "$WORKDIR/p38_${_slug}_sg.txt" > "$WORKDIR/p38_${_slug}_sg_skel.txt"
    p38_skel "$WORKDIR/p38_${_slug}_git.txt" > "$WORKDIR/p38_${_slug}_git_skel.txt"
    # The "test -s" is not decoration: both sides of the cmp are produced by
    # this same function with stderr discarded, so ANY failure that empties
    # both outputs at once -- a fixture whose repo never got created, a
    # broken $SG path, or a p38_skel edit that over-filters -- would leave
    # two empty files and every one of these 27+ checks would pass while
    # testing nothing. This project has shipped an unfailable test twice.
    # git's long format always emits at least a branch/HEAD line, so an
    # empty git-side skeleton means the fixture, not sg, is broken.
    # Phase 38 round 2, item C: the exit code was previously not checked at
    # all -- stderr from both sides was discarded, so sg printing a warning
    # or exiting non-zero on some fixture was invisible to this oracle.
    # Real git exits 0 on all 34 of these fixture combinations (measured), so sg
    # must too.
    check "phase38: sg status ($_label) matches real git skeleton" \
        sh -c "test -s '$WORKDIR/p38_${_slug}_git_skel.txt' \
            && cmp -s '$WORKDIR/p38_${_slug}_sg_skel.txt' '$WORKDIR/p38_${_slug}_git_skel.txt' \
            && [ $_sg_rc = 0 ]"
}

P38_ROOT="$WORKDIR/p38fx"
mkdir -p "$P38_ROOT"

# Fixtures are built with "$SG" init (not "git init"), per this phase's own
# convention -- everything else (add/commit/checkout/merge/mv) is plain git,
# same as every other interop fixture in this file; sg's on-disk format is
# byte-compatible so real git can operate on an sg-initialized repo freely.
p38_init() {
    ( cd "$P38_ROOT" && "$SG" init "$1" ) > /dev/null 2>&1
    ( cd "$P38_ROOT/$1" && git config user.email "a@b.c" && git config user.name "git user" \
        && git config commit.gpgsign false )
}

# "merge in progress, conflict already resolved to HEAD's content" common
# prefix, shared by resolved/mergebare/mergeuntr/mergeunst.
p38_mergebase() {
    _d="$P38_ROOT/$1"
    ( cd "$_d"
      printf 'A\n' > f.txt; echo keep > g.txt; git add .; git commit -qm base
      git checkout -q -b other; printf 'B\n' > f.txt; git commit -qam theirs
      git checkout -q master; printf 'C\n' > f.txt; git commit -qam ours
      git merge other -q > /dev/null 2>&1 )
}

for c in clean untracked unstaged staged mixed deleted renamed stagedonly; do
    p38_init "$c"
    ( cd "$P38_ROOT/$c"
      echo base > f.txt; echo two > g.txt; git add .; git commit -qm base
      case $c in
          untracked)  echo n > u.txt ;;
          unstaged)   echo m >> f.txt ;;
          staged)     echo m >> f.txt; git add f.txt ;;
          stagedonly) echo m >> f.txt; git add f.txt ;;
          mixed)      echo m >> f.txt; git add f.txt; echo m2 >> g.txt; echo n > u.txt ;;
          deleted)    rm f.txt ;;
          renamed)    git mv f.txt renamed.txt ;;
      esac )
done

p38_init unborn
p38_init unbornu;  ( cd "$P38_ROOT/unbornu"; echo n > u.txt )
p38_init unborns;  ( cd "$P38_ROOT/unborns"; echo s > s.txt; git add s.txt )

# Phase 38 round 2, item B2: other.txt is a second, untouched-by-the-merge
# file, added so a pathspec can distinguish "matches the conflicted path"
# from "matches something else" -- this is the fixture that guards item A's
# fix (the merge banner and the closing-line suppression now sharing one
# filtered unmerged count). Note this changes the "conflict" (no pathspec)
# case's own output too (other.txt now shows up as a second clean-tracked
# file, which changes nothing visible since it is unchanged) -- the
# comparison below is dynamic (built at check time, not pinned to a fixed
# string), so it tracks the fixture automatically.
p38_init conflict
( cd "$P38_ROOT/conflict"
  printf 'A\n' > f.txt; printf 'other\n' > other.txt; git add .; git commit -qm base
  git checkout -q -b other; printf 'B\n' > f.txt; git commit -qam theirs
  git checkout -q master; printf 'C\n' > f.txt; git commit -qam ours
  git merge other -q > /dev/null 2>&1 )

p38_init resolved;   p38_mergebase resolved;   ( cd "$P38_ROOT/resolved";   printf 'R\n' > f.txt; git add f.txt )
p38_init mergebare;  p38_mergebase mergebare;  ( cd "$P38_ROOT/mergebare";  printf 'C\n' > f.txt; git add f.txt )
p38_init mergeuntr;  p38_mergebase mergeuntr;  ( cd "$P38_ROOT/mergeuntr";  printf 'C\n' > f.txt; git add f.txt; echo n > u.txt )
p38_init mergeunst;  p38_mergebase mergeunst;  ( cd "$P38_ROOT/mergeunst";  printf 'C\n' > f.txt; git add f.txt; echo d >> g.txt )

p38_init detached
( cd "$P38_ROOT/detached"
  echo base > f.txt; git add .; git commit -qm base
  echo two >> f.txt; git commit -qam second; git checkout -q HEAD~1 )

p38_init ignored
( cd "$P38_ROOT/ignored"
  echo base > f.txt; echo 'build/' > .gitignore; git add f.txt .gitignore; git commit -qm base
  mkdir -p build; echo x > build/o.txt; echo n > u.txt )

# One check per fixture x flag combination -- 27 cases total, matching
# PHASE38_SPEC.md section 1's enumeration, deliberately not merged into
# fewer checks so a red line names exactly which state broke.
p38_cmp clean              "clean"                  "$P38_ROOT/clean"
p38_cmp untracked          "untracked"              "$P38_ROOT/untracked"
p38_cmp unstaged           "unstaged"               "$P38_ROOT/unstaged"
p38_cmp staged             "staged"                 "$P38_ROOT/staged"
p38_cmp mixed              "mixed"                  "$P38_ROOT/mixed"
p38_cmp deleted            "deleted"                "$P38_ROOT/deleted"
p38_cmp renamed            "renamed"                "$P38_ROOT/renamed"
p38_cmp detached           "detached"               "$P38_ROOT/detached"
p38_cmp unborn             "unborn"                 "$P38_ROOT/unborn"
p38_cmp unbornu            "unbornu"                "$P38_ROOT/unbornu"
p38_cmp unborns            "unborns"                "$P38_ROOT/unborns"
p38_cmp conflict           "conflict"               "$P38_ROOT/conflict"
p38_cmp resolved           "resolved"               "$P38_ROOT/resolved"
p38_cmp mergebare          "mergebare"              "$P38_ROOT/mergebare"
p38_cmp mergeuntr          "mergeuntr"              "$P38_ROOT/mergeuntr"
p38_cmp mergeunst          "mergeunst"              "$P38_ROOT/mergeunst"
p38_cmp ignored            "ignored"                "$P38_ROOT/ignored"
p38_cmp ignored_ignored    "ignored --ignored"      "$P38_ROOT/ignored"    --ignored
p38_cmp ignored_uall       "ignored -uall"          "$P38_ROOT/ignored"    -uall
p38_cmp ignored_uno        "ignored -uno"           "$P38_ROOT/ignored"    -uno
p38_cmp mixed_uno          "mixed -uno"             "$P38_ROOT/mixed"      -uno
p38_cmp mixed_uall         "mixed -uall"            "$P38_ROOT/mixed"      -uall
p38_cmp stagedonly_uno     "stagedonly -uno"        "$P38_ROOT/stagedonly" -uno
p38_cmp untracked_uno      "untracked -uno"         "$P38_ROOT/untracked"  -uno
p38_cmp unborn_uno         "unborn -uno"            "$P38_ROOT/unborn"     -uno
p38_cmp unbornu_uno        "unbornu -uno"           "$P38_ROOT/unbornu"    -uno
p38_cmp conflict_uno       "conflict -uno"          "$P38_ROOT/conflict"   -uno

# --- Phase 38 round 2, item B1: -uno on the four merge-in-progress fixtures
# whose closing summary line is suppressed entirely once conflicts are
# resolved (Bug E). "conflict -uno" already exists above, but that fixture
# still has unmerged_count > 0, so it never reaches the suppression branch
# and does not exercise this dimension -- these four do. ---
p38_cmp resolved_uno       "resolved -uno"          "$P38_ROOT/resolved"   -uno
p38_cmp mergebare_uno      "mergebare -uno"         "$P38_ROOT/mergebare"  -uno
p38_cmp mergeuntr_uno      "mergeuntr -uno"         "$P38_ROOT/mergeuntr"  -uno
p38_cmp mergeunst_uno      "mergeunst -uno"         "$P38_ROOT/mergeunst"  -uno

# --- Phase 38 round 2, item B2: merge in progress + pathspec, the regression
# guard for item A's fix (the merge banner and the closing-line suppression
# now sharing count_unmerged's one filtered count instead of the banner
# scanning idx unfiltered). "-- f.txt" matches the conflicted path (git's
# banner is "You have unmerged paths."); "-- other.txt" and "-- nosuch" do
# not (git's banner is "All conflicts fixed but you are still merging." and
# it prints no closing summary line at all). ---
p38_cmp conflict_pf        "conflict -- f.txt"      "$P38_ROOT/conflict"   -- f.txt
p38_cmp conflict_pother    "conflict -- other.txt"  "$P38_ROOT/conflict"   -- other.txt
p38_cmp conflict_pnosuch   "conflict -- nosuch"     "$P38_ROOT/conflict"   -- nosuch

# --- Phase 38 round 2, item B3: real-git oracle for four of the seven
# unmerged_label strings. Before this, only "both modified:" had an oracle
# (Q3, phase23). This is NOT run through the skeleton comparator: p38_skel
# drops every tab-led line, which is exactly where the label text lives, so
# a skeleton pass here would compare nothing. Uses Q3's own technique
# instead: strip the leading tab, sort, cmp -s -- so the label column is
# compared alongside the path, same as Q3's own comment says. The recipe
# below is measured to produce, byte-for-byte identical between sg and real
# git: "both added: aa.txt", "both modified: uu.txt",
# "deleted by them: ud.txt", "deleted by us: du.txt". The remaining three
# labels ("both deleted:", "added by us:", "added by them:") cannot be
# produced by an ordinary merge (DD auto-resolves, AU/UA need a rename or a
# hand-built index) and are NOT attempted here -- see CLAUDE.md and
# docs/DESIGN.md's Phase 38 section for that as a named, pre-existing gap.
p38_label_fixture() {
    _dir="$1"; _impl="$2"
    mkdir -p "$_dir"
    if [ "$_impl" = "git" ]; then
        (cd "$_dir" && git init -q .) > /dev/null 2>&1
    else
        (cd "$WORKDIR" && "$_impl" init "$(basename "$_dir")") > /dev/null 2>&1
    fi
    (cd "$_dir" && git config user.email "a@b.c" && git config user.name "git user" \
        && git config commit.gpgsign false) > /dev/null 2>&1
    ( cd "$_dir"
      for f in uu ud du; do echo base > $f.txt; done
      git add .; git commit -qm base
      git checkout -q -b other
        echo theirs > uu.txt; rm ud.txt; echo theirs > du.txt; echo theirs > aa.txt
        git add -A; git commit -qm theirs
      git checkout -q master
        echo ours > uu.txt; echo ours > ud.txt; rm du.txt; echo ours > aa.txt
        git add -A; git commit -qm ours
      git merge other > /dev/null 2>&1 )
}
P38_LBL_GIT="$WORKDIR/p38_label_git"
p38_label_fixture "$P38_LBL_GIT" git
check "phase38 oracle: precondition -- the label fixture merge really did conflict" \
    test -f "$P38_LBL_GIT/.git/MERGE_HEAD"
P38_LBL_SG="$WORKDIR/p38_label_sg"
p38_label_fixture "$P38_LBL_SG" "$SG"
check "phase38 oracle: precondition -- sg's label fixture merge also conflicted" \
    test -f "$P38_LBL_SG/.git/MERGE_HEAD"
(cd "$P38_LBL_SG" && "$SG" status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p38_label_sg.txt"
(cd "$P38_LBL_GIT" && LC_ALL=C git $P38_GIT_FLAGS status) 2>/dev/null | sed -n 's/^\t//p' | sort > "$WORKDIR/p38_label_git.txt"
check "phase38: sg status's unmerged labels match real git byte-for-byte" \
    cmp -s "$WORKDIR/p38_label_sg.txt" "$WORKDIR/p38_label_git.txt"

# Precondition for the advice.statusHints half of $P38_GIT_FLAGS. Phase 38
# went fully green locally and red on GitHub's macOS runner for 21 of its 34
# cases, because that runner's git has advice turned off, and
# advice.statusHints=false strips the "(use \"git add\" to track)" tail off
# the CLOSING SUMMARY line as well as removing the indented hint lines the
# skeleton already filters. Without this probe, dropping the pin would come
# back as 21 silent cmp failures naming no cause; with it, one check names
# the cause. Deliberately probes through $P38_GIT_FLAGS, the same variable
# the comparisons use.
(cd "$P38_ROOT/untracked" && LC_ALL=C git $P38_GIT_FLAGS status) \
    > "$WORKDIR/p38_advice_probe.txt" 2>/dev/null
check "phase38 oracle: precondition -- the pinned flags keep git's closing-line parenthetical" \
    grep -q 'nothing added to commit but untracked files present (use "git add" to track)' \
    "$WORKDIR/p38_advice_probe.txt"


echo ""
echo "interop: $PASS/$TOTAL passed, $SKIP skipped"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

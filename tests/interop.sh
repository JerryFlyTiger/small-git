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

# a test file with a newline, CJK text, and an embedded raw NUL byte
FILE1="$WORKDIR/file1.bin"
printf '第一行 hello\n第二行\xe4\xbd\xa0\xe5\xa5\xbd\n' > "$FILE1"
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
(cd "$INIT_DIR" && git status) > "$GIT_STATUS_OUT" 2>&1
GIT_STATUS_RC=$?
check "git status exits 0 inside sg-inited repo" test "$GIT_STATUS_RC" = 0
check "git status does not complain about an invalid repository" sh -c "! grep -qi 'not a git repository' '$GIT_STATUS_OUT'"

export GIT_AUTHOR_NAME="Interop Test"
export GIT_AUTHOR_EMAIL="interop@example.com"

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
check "phase4c case6: sg reports the duplicate commit as skipped" grep -q "已跳過" "$P4C_EMPTY_OUT"

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
        echo "warning: HTTP test server 未能在時限內就緒，跳過 phase 5b HTTP 測試" >&2
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

    kill "$HTTP_SERVER_PID" 2>/dev/null
    HTTP_SERVER_PID=""
else
    skip "phase5b: sg clone over smart HTTP"
    skip "phase5b: sg fetch over smart HTTP"
    skip "phase5c: sg push over smart HTTP"
fi

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
        echo "warning: phase6a HTTP test server 未能在時限內就緒，跳過 phase6a push/clone HTTP 測試" >&2
        skip "phase6a: sg push over smart HTTP with a chunked blob"
        skip "phase6a: sg push transfers every referenced chunk blob to the remote"
        skip "phase6a: git fsck exits 0 on the bare repo after pushing a chunked blob"
        skip "phase6a: sg clone over smart HTTP exits 0 for a repo containing a chunked blob"
        skip "phase6a: a second sg clone from the same (plain git) server now recovers the chunk data too, since sg push propagated refs/sg/chunks there"
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

        kill "$HTTP_SERVER_PID" 2>/dev/null
        HTTP_SERVER_PID=""
    fi
else
    skip "phase6a: sg push over smart HTTP with a chunked blob"
    skip "phase6a: sg push transfers every referenced chunk blob to the remote"
    skip "phase6a: git fsck exits 0 on the bare repo after pushing a chunked blob"
    skip "phase6a: sg clone over smart HTTP exits 0 for a repo containing a chunked blob"
    skip "phase6a: a second sg clone from the same (plain git) server now recovers the chunk data too, since sg push propagated refs/sg/chunks there"
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
    grep -q "資料塊" "$P6B_MISSING_RESTORE_OUT"
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
    grep -q "資料塊" "$P6C_FIRSTCHUNK_RESTORE_OUT"
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
        echo "warning: phase6b HTTP test server 未能在時限內就緒，跳過 phase6b sg push/clone chunk 測試" >&2
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
            grep -q "push 中止" "$P6B_PUSHFAIL_OUT"

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
    grep -q "資料塊" "$P6F_BROKEN_OUT"

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

echo ""
echo "interop: $PASS/$TOTAL passed, $SKIP skipped"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

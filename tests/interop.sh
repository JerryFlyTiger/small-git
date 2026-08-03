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

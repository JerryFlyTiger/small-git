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
for bad in 'a..b' 'a b' 'a.lock' 'HEAD' 'a/' '@{x}'; do
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
for bad in 'a..b' 'a b' 'a.lock' 'HEAD' 'a/' '@{x}'; do
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
(cd "$P9I_REPO" && "$SG" status) > "$P9I_SG_OUT" 2>&1
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
(cd "$P9ST_REPO" && "$SG" status) > "$P9ST_OUT" 2>&1
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
    grep -q "無法切換分支" "$P14_SWITCH_ERR"

P14_SWITCH_FORCE_ERR="$WORKDIR/p14_switch_force_err.txt"
(cd "$P14_SWITCH" && "$SG" switch --force master < /dev/null) > "$P14_SWITCH_FORCE_ERR" 2>&1
check "phase14: sg switch --force during a paused rebase is still rejected" test $? != 0
check "phase14: --force rejection left sg-rebase/ in place" test -d "$P14_SWITCH/.git/sg-rebase"
check "phase14: --force rejection is due to the rebase gate, not skipped by --force" \
    grep -q "無法切換分支" "$P14_SWITCH_FORCE_ERR"

P14_SWITCH_C_ERR="$WORKDIR/p14_switch_c_err.txt"
(cd "$P14_SWITCH" && "$SG" switch -c newbranch < /dev/null) > "$P14_SWITCH_C_ERR" 2>&1
check "phase14: sg switch -c during a paused rebase is rejected" test $? != 0
check "phase14: -c rejection left sg-rebase/ in place" test -d "$P14_SWITCH/.git/sg-rebase"
check "phase14: switch -c rejection did NOT create the new branch (matches real git)" \
    sh -c "! (cd '$P14_SWITCH' && git rev-parse --verify refs/heads/newbranch) > /dev/null 2>&1"
check "phase14: -c rejection is due to the rebase gate, not the dirty-workdir prompt" \
    grep -q "無法切換分支" "$P14_SWITCH_C_ERR"

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
    grep -q "undo 會放棄" "$P14_MSG_ERR"
check "phase14: the shared dirty prompt does not promise the rebase survives" \
    sh -c "! grep -q 'rebase 本身會保留' '$P14_MSG_ERR'"
check "phase14: the shared dirty prompt says what it actually does" \
    grep -q "覆蓋工作目錄裡的衝突解決內容" "$P14_MSG_ERR"
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
# ident/timestamp preservation for the surviving entries (§4.1).
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

# --- extra: multi-line -m normalization (§4.1's copy_reflog_msg) -- targets
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
# --short=7 for the same commit (Risks §9.1). ---
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
# list never consults refs/stash (measured, §4.2's sg_stash_list_read
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

# --- row 9: popping a `-u` stash -- "silently drop the untracked half and
# pop the rest" would also exit non-zero on some unrelated path in a sloppy
# implementation, or worse, exit 0 having lost the untracked file; assert
# the stash SURVIVES and the untracked file was never created. ---
P15_POPU="$WORKDIR/p15_popu"
p15_base_repo "$P15_POPU"
printf 'tracked change\n' > "$P15_POPU/a.txt"
printf 'untracked content\n' > "$P15_POPU/u.txt"
(cd "$P15_POPU" && git add a.txt && git stash push -q -u -m "has untracked") > /dev/null 2>&1
check "phase15 row9 (pop -u stash): precondition -- u.txt is gone (git -u stashed it)" \
    test ! -e "$P15_POPU/u.txt"
P15_POPU_ERR="$WORKDIR/p15_popu_err.txt"
(cd "$P15_POPU" && "$SG" stash pop) > /dev/null 2> "$P15_POPU_ERR"
check "phase15 row9 (pop -u stash): sg refuses (exit 1)" test $? != 0
check "phase15 row9 (pop -u stash): the stash is still listed" \
    sh -c "[ -n \"\$(cd '$P15_POPU' && git stash list)\" ]"
check "phase15 row9 (pop -u stash): the untracked file was not created" \
    test ! -e "$P15_POPU/u.txt"
# The three assertions above cannot tell the -u guard from any other
# failure: the parent-count check exists at both the CLI and the library
# layer, and disabling either one leaves the other returning the same exit
# 1, the same surviving stash and the same absent file. Measured -- with
# cmd_stash.c's `parent_count > 2` branch removed, all 798 checks stayed
# green. Only the message names which guard actually spoke.
check "phase15 row9 (pop -u stash): the refusal names the untracked half, not a generic failure" \
    grep -q '未追蹤檔案' "$P15_POPU_ERR"

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
# zh_TW machine prints "捨棄了 refs/stash@{0}（...）", full-width parentheses
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
    grep -q '尚有未解決的衝突' "$P15_UNMERGED_ERR"
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
# "無法切換分支" wording, so grepping for that alone would pass even if the
# merge gate did not exist. Every rejection below is pinned to the
# merge-specific "進行中的合併" instead, which no other refusal emits.

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
    grep -q "進行中的合併" "$P16_SWITCH_ERR"

P16_SWITCH_FORCE_ERR="$WORKDIR/p16_switch_force_err.txt"
(cd "$P16_SWITCH" && "$SG" switch --force other < /dev/null) > "$P16_SWITCH_FORCE_ERR" 2>&1
check "phase16: sg switch --force during an in-progress merge is still rejected" test $? != 0
check "phase16: --force rejection left MERGE_HEAD in place" test -f "$P16_SWITCH/.git/MERGE_HEAD"
check "phase16: --force rejection left HEAD unchanged" \
    sh -c "test \"\$(cd '$P16_SWITCH' && git rev-parse HEAD)\" = '$P16_HEAD_BEFORE'"
check "phase16: --force rejection is due to the merge gate, not skipped by --force" \
    grep -q "進行中的合併" "$P16_SWITCH_FORCE_ERR"

P16_SWITCH_C_ERR="$WORKDIR/p16_switch_c_err.txt"
(cd "$P16_SWITCH" && "$SG" switch -c newbranch < /dev/null) > "$P16_SWITCH_C_ERR" 2>&1
check "phase16: sg switch -c during an in-progress merge is rejected" test $? != 0
check "phase16: -c rejection left MERGE_HEAD in place" test -f "$P16_SWITCH/.git/MERGE_HEAD"
check "phase16: switch -c rejection did NOT create the new branch (matches real git)" \
    sh -c "! (cd '$P16_SWITCH' && git rev-parse --verify refs/heads/newbranch) > /dev/null 2>&1"
check "phase16: -c rejection is due to the merge gate" \
    grep -q "進行中的合併" "$P16_SWITCH_C_ERR"

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
    grep -q "進行中的合併" "$P16_RESOLVED_ERR"
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
    grep -q "進行中的合併" "$P16_CORRUPT_ERR"
: > "$P16_CORRUPT/.git/MERGE_HEAD"
P16_EMPTY_ERR="$WORKDIR/p16_empty_err.txt"
(cd "$P16_CORRUPT" && "$SG" switch --force other < /dev/null) > "$P16_EMPTY_ERR" 2>&1
check "phase16 corrupt: sg switch --force is rejected on an empty MERGE_HEAD" test $? != 0
check "phase16 corrupt: empty-MERGE_HEAD rejection is due to the merge gate" \
    grep -q "進行中的合併" "$P16_EMPTY_ERR"

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
    grep -q "進行中的合併" "$P16_DIRMH_ERR"

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
    sh -c "grep -q '進行中的 rebase' '$P16_REBASE_ERR' && ! grep -q '進行中的合併' '$P16_REBASE_ERR'"

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
    grep -q "無法執行 soft reset" "$P16_SOFT_ERR"

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
    grep -q "損壞的 MERGE_HEAD" "$P16_CCOMMIT_ERR"
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
    grep -q "尚未完成的合併" "$P16_CSECOND_ERR"
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
    grep -q "尚未完成的合併" "$P16_CREBASE_ERR"

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
        echo "warning: phase17d HTTP test server 未能在時限內就緒，跳過 phase17d 網路 reflog 測試" >&2
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
        test "${P18E_ERR#sg: 目前是 detached HEAD}" = "$P18E_ERR"
    check "phase18e: sg $p18e_cmd names the corrupt HEAD instead (got '$P18E_ERR')" \
        test "${P18E_ERR#sg: 無法讀取 HEAD}" != "$P18E_ERR"
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
(cd "$P19F_S" && "$SG" rebase --abort) > /dev/null 2>&1
check "phase19f: sg rebase --abort works from a detached start" test $? = 0
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
(cd "$P19G2_S" && "$SG" rebase master) > /dev/null 2>&1
check "phase19g: a fast-forward rebase from a detached HEAD succeeds" test $? = 0
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
        test "${P19H_ERR#sg: 無法讀取 HEAD}" != "$P19H_ERR"
    check "phase19h: sg $p19h_cmd leaves the corrupt HEAD as evidence" \
        test "$(cat "$P19H/.git/HEAD")" = "neither a ref nor a sha"
done

echo ""
echo "interop: $PASS/$TOTAL passed, $SKIP skipped"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

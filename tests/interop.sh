#!/bin/sh
# Interop test: proves small_git's object format is bit-compatible with real git.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
SG="$PROJECT_ROOT/build/sg"

PASS=0
FAIL=0
TOTAL=0

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

if [ ! -x "$SG" ]; then
    echo "error: $SG not found, run 'make' first" >&2
    exit 1
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

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

echo ""
echo "interop: $PASS/$TOTAL passed"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

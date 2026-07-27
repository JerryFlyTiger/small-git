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

echo ""
echo "interop: $PASS/$TOTAL passed"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0

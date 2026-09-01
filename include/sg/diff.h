#ifndef SG_DIFF_H
#define SG_DIFF_H

#include <stddef.h>

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/pathspec.h"
#include "sg/similarity.h"
#include "sg/tree_build.h"

/* Building a list of changed paths, decoupled from where the two sides come
   from. Before this existed `sg diff` could only ever compare the index to the
   working tree, because the loop that found the changed paths and the loop that
   printed them were the same loop. Everything here is about *which paths
   changed and what is on each side*; rendering lives in sg/diff_out.h. */

typedef enum {
    SG_DIFF_SIDE_ABSENT = 0, /* the path does not exist on this side */
    SG_DIFF_SIDE_BLOB,       /* content is the object named by `id` */
    SG_DIFF_SIDE_WORKDIR     /* content is the file at <repo_root>/<path> */
} sg_diff_side_kind;

typedef struct {
    sg_diff_side_kind kind;
    /* The tree/index entry mode, normalized to 100644/100755 for WORKDIR sides
       too (Phase 26): read off the file's permission bits with lstat, exec-bit
       only, never 120000 -- symlinks are out of scope for every tracked-file
       walk in this codebase (status/apply/tree_build all exclude them), and
       this field does not change that; a path that IS a symlink on disk is
       reported conservatively as 100644, same as "no exec bit". 0 still means
       "unknown" and mode comparison is skipped whenever either side is 0 (kept
       for ABSENT sides, which never carry a mode). */
    unsigned int mode;
    /* Valid iff kind != SG_DIFF_SIDE_ABSENT.
       For BLOB: the raw id as stored in the tree/index entry -- i.e. a chunk
       pointer's OWN id when the entry is chunked storage, not the pointed-to
       content's id. This is deliberate: sg_diff_side_read hands it straight to
       sg_chunk_read_blob, which needs the pointer's id to resolve it. Printing
       a git-compatible "index <old>..<new>" line needs the *effective*
       (content) id instead, which the renderer computes separately via
       sg_chunk_effective_id -- storing the effective id here instead would
       break sg_diff_side_read, since no object exists under the effective id
       when the entry is actually chunked.
       For WORKDIR (Phase 26): the working file's own content hash
       (sg_hash_file_blob's result, reused rather than recomputed) -- already
       the "effective" id, since a working-tree file is never itself a chunk
       pointer object. */
    unsigned char id[SG_SHA1_RAW_LEN];
} sg_diff_side;

typedef struct {
    /* The path this row is displayed under: the NEW path for an addition, a
       modification or a rename, and the only path for a deletion. The list
       stays sorted by this field, which is what `git diff --name-only`
       prints and the order it prints them in. */
    char *path; /* owned, repo-relative, '/' separated */
    /* Non-NULL only for a rename: where the content came from. `path` is
       still the destination, so every renderer that does not care about
       renames keeps working unchanged, and only the ones that do go looking
       here. Owned. */
    char *old_path;
    /* Similarity, 0-100, meaningful only when old_path != NULL. git prints
       it as a three-digit zero-padded suffix ("R100", "R093") and as
       "similarity index 93%". */
    int score;
    /* 1 when this row is a COPY rather than a rename -- the source is still
       there. Only meaningful when old_path != NULL, and only ever set when
       the caller asked for copy detection.

       git decides this at the very end rather than while pairing, and the
       rule is worth knowing because it looks arbitrary: a source is used
       some number of times, each paired destination consumes one use IN PATH
       ORDER, and a destination is a copy exactly while uses remain after
       its own. So with one source and two destinations the FIRST is the copy
       and the second is the rename, however much better the second matched.
       Measured against git 2.55.0; see docs/DESIGN.md Phase 33. */
    int is_copy;
    sg_diff_side old_side;
    sg_diff_side new_side;
    /* 1 when this row stands for an unresolved conflict rather than a content
       change. Both sides are ABSENT: there is no single "before" blob to show.
       Renderers print git's "U"/"Unmerged" for it and, measured against git
       2.55.0, leave it OUT of the "N files changed" total -- a diff whose only
       row is unmerged prints " 0 files changed".

       Without this flag the merge-join would see a path that is in the tree but
       has no stage-0 index entry and call it a deletion, inventing a deletion
       that never happened. That is worse than the old behaviour of skipping
       unmerged paths outright, which is why the flag exists rather than a
       `continue`. */
    int unmerged;
    /* These three are what the combined-diff renderer (Phase 34, extended
       Phase 40) needs and old_side/new_side cannot express. There are
       exactly TWO producers, and they fill them with different meanings:

       1. sg_diff_index_workdir, when unmerged != 0 (a real conflict):
            ours   = index stage 2 (the "-1" side in git's parlance),
            theirs = index stage 3,
            result = the working-tree file (ABSENT when it has been
                     deleted -- this does NOT disqualify the row, see
                     sg_diff_entry_is_combined below).
          old_side / new_side stay ABSENT for this row regardless.

       2. sg_diff_fill_combined_from_index (Phase 40), a post-build pass over
          sg_diff_tree_workdir's output (`sg diff -c/--cc <rev>`, no
          `--cached`, no second rev), with unmerged left at 0:
            ours   = the LOWEST-stage index entry for the path (stage 0 for
                     an ordinary tracked file; measured against git 2.55.0,
                     an add/add conflict with no stage 1 uses stage 2, not
                     "stage 1 or nothing"),
            theirs = a copy of old_side (the named tree's blob),
            result = a copy of new_side (the working-tree file).
          Here result absence (a working-tree deletion) DOES disqualify the
          row -- see sg_diff_entry_is_combined.

       Every other builder leaves all three ABSENT, which is what routes
       sg_diff_tree_index (--cached) through the "* Unmerged path" rendering
       unconditionally regardless of -c/--cc. */
    sg_diff_side ours;
    sg_diff_side theirs;
    sg_diff_side result;
    /* Phase 51: 1 for a row that exists ONLY to be offered to
       sg_diff_detect_renames as a `-C -C` copy source -- the path is
       unchanged (old_side and new_side carry identical content and mode).
       Such a row is never printed and never reaches any output format:
       sg_diff_detect_renames strips every row still carrying this flag
       before it returns, on every returning path (see its own header
       comment). `kind` is meaningless on a row like this -- it is not part
       of any sg_diff_kind-shaped enum in this struct, this note exists so a
       future field added in that shape does not assume every row means
       something by it; every existing `switch` stays exhaustive only
       because these rows never escape. Only sg_diff_trees, sg_diff_tree_index
       and sg_diff_tree_workdir ever set this (via their own
       `include_unchanged` parameter); sg_diff_index_workdir never does, see
       its own function comment for why. */
    int unchanged;
} sg_diff_entry;

/* Whether a row should render as a combined diff instead of an ordinary
   2-way one. This is the shared predicate for both producers above -- do
   not add a third boolean field, it is derivable from what is already
   there:

     ours/theirs both non-ABSENT, AND (unmerged OR result non-ABSENT).

   The asymmetry on `result` is deliberate and measured on both sides: a
   real conflict (producer 1) still renders combined with a deleted working
   tree file (interop pins the header-only "deleted result" shape), but the
   Phase 40 rev-mode pass (producer 2) does NOT -- git falls back to an
   ordinary "deleted file mode" row instead (SPEC section 3, fixture D).
   Collapsing the two into one unconditional rule breaks whichever side you
   didn't measure last. */
int sg_diff_entry_is_combined(const sg_diff_entry *e);

typedef struct {
    sg_diff_entry *entries; /* sorted by path, byte-wise */
    size_t count;
    size_t cap;
} sg_diff_list;

/* Frees every owned path and the array itself, and zeroes the list. */
void sg_diff_list_free(sg_diff_list *list);

/* The four builders below all produce a list holding only paths that actually
   differ: a path whose two sides carry the same content (and the same mode,
   when both modes are known) is left out entirely.

   `old_tree`/`new_tree` may be NULL, which means "the empty tree". That is what
   an unborn HEAD looks like, and taking NULL here saves every caller from
   having to write an empty tree object just to diff against it.

   Return 0 on success, -1 on failure, and -2 when a tree entry name fails
   sg_path_component_is_safe -- the -2 and the `bad_path` buffer (SG_PATH_MAX,
   may be NULL) are sg_tree_flatten's contract, propagated rather than
   flattened into -1 so the CLI can name the offending path. */

/* `include_unchanged` (Phase 51, mandatory, no default -- same idiom as
   sg_workdir_missing/rename_score/sg_diff_algorithm): when set, a path that
   exists unchanged on both sides is appended anyway, with `unchanged = 1`,
   at its natural position in the merge loop (the list stays sorted by path).
   This is `-C -C`'s data-layer half: such a row is fuel for
   sg_diff_detect_renames's copy-source pool and nothing else, and that
   function strips it back out before returning. Passing 0 reproduces the
   pre-Phase-51 behaviour exactly. An unmerged path is never unchanged. */

/* tree vs tree -- `sg diff <rev> <rev>`. */
int sg_diff_trees(const char *git_dir, const unsigned char *old_tree,
                  const unsigned char *new_tree, sg_diff_list *out, char *bad_path,
                  int include_unchanged);

/* Phase 52: sg_diff_trees' union-walk body, taking two already-flattened
   sg_flat_list (see sg/tree_build.h) instead of two tree ids. Both lists are
   BORROWED (this never frees them, the caller must keep them alive until it
   returns) and NULL means "the empty tree", the same convention old_tree/
   new_tree == NULL carries above. Exists so a caller that already flattened
   both trees for another reason (sg_merge_trees, when rename detection is
   on) does not pay for a second flatten just to diff them. Unlike
   sg_diff_trees, bad_path is never this function's to fill -- a -2 from
   flattening is the caller's own to report, since the caller did the
   flattening itself. Returns 0 or -1, never -2. */
int sg_diff_from_flat_lists(const sg_flat_list *old_flat, const sg_flat_list *new_flat,
                            sg_diff_list *out, int include_unchanged);

/* tree vs index -- `sg diff --cached`. A path carrying stage 1/2/3 entries has
   no single staged blob to diff against and yields exactly one row, with
   `unmerged` set. Measured: `git diff --cached --name-status` prints "U". */
int sg_diff_tree_index(const char *git_dir, const unsigned char *old_tree,
                       const sg_index *idx, sg_diff_list *out, char *bad_path,
                       int include_unchanged);

/* index vs working tree -- plain `sg diff`. Never fails on a missing working
   tree file: that is a deletion, not an error.

   A blob that cannot be read at all -- a deleted object, or a chunk pointer
   whose data is gone -- must NOT fail the whole call either. The builder
   cannot answer "did this path change", so it records the path as changed and
   leaves the complaining to the renderer, which is holding the path and can
   name it. Aborting the list instead loses two things at once: every other
   path's diff, and the actionable message itself, since nothing downstream
   ever gets far enough to know which file was broken.

   A working-tree file that EXISTS (stat succeeds) but cannot be read (e.g.
   permission denied) is a different failure from the two above, and is
   deliberately handled differently: it is folded into SG_DIFF_SIDE_ABSENT,
   the same as if the file were not there at all -- NOT reported as a WORKDIR
   side carrying a placeholder id. This matches sg_status_diff_unstaged
   (src/workdir/status.c), which reports the identical failure as
   SG_STATUS_DELETED. A placeholder id (all-zero, since sg_hash_file_blob
   never legitimately produces one) would instead be indistinguishable, at
   render time, from a real content id that happens to be all-zero, and would
   print git's "0000000" placeholder in shapes real git never produces (e.g.
   with a mode suffix on an ordinary modification row, where 0000000 only
   ever appears -- unsuffixed -- on an add/delete row). Every builder below
   that reads a working-tree file's bytes follows this same rule.

   An unmerged path produces up to TWO rows, in this order -- measured against
   git 2.55.0, which prints "U conflict.txt" and "M conflict.txt" for the same
   path:
     1. the `unmerged` row;
     2. a normal row comparing the path's *stage 2* (ours) blob against the
        working tree, emitted only when a stage-2 entry exists and its content
        differs from the file on disk. Writing the working tree back to exactly
        the stage-2 bytes makes this second row disappear, which is how the
        stage was identified -- stage 1 and stage 3 both leave it in place.

   Phase 51: deliberately given NO `include_unchanged` parameter. Every path
   in this comparison comes from the index, so a path is modified, unchanged,
   or deleted -- never added -- and a copy needs an addition as its
   destination. Measured against git 2.55.0: `git diff -C -C` on a
   staged-then-edited file, and on an unchanged file that a modified file is
   a 94% match of, both print only "M", never a "C" row. Giving this builder
   a parameter it cannot use would put a fourth call site on the hook for
   this feature's risk for no gain. */
int sg_diff_index_workdir(const char *git_dir, const char *repo_root,
                          const sg_index *idx, sg_diff_list *out);

/* tree vs working tree -- `sg diff <rev>`.

   The *index* decides which paths take part, not the working tree. Measured
   against git 2.55.0: after `git rm --cached f` the file is still on disk, and
   `git diff HEAD` reports it as deleted -- so a path in the tree but not in the
   index is a deletion even though the bytes are right there, and an untracked
   file is not reported at all. Getting this from the working tree instead would
   turn every untracked file into an addition.

   An unmerged path is NOT special here: the index only decides membership, and
   the content still comes from the working tree, so such a path takes part as
   an ordinary modification against the tree's blob. Measured: `git diff HEAD
   --name-status` prints "M" for a conflicted path, not "U".

   `combined` (Phase 40, pass nonzero only for `sg diff -c/--cc <rev>`)
   widens the row-inclusion rule from "differs from the tree" to "differs
   from the tree OR differs from the index's lowest-stage entry" -- needed
   because a combined row's inclusion test is "differs from ANY parent", and
   the index is a second parent this comparison otherwise never looks at.
   Measured against git 2.55.0: a path staged with one edit and then
   reverted back to the named tree's exact bytes in the working tree still
   appears in `git diff -c <rev>` (SPEC section 6, fixture O's p3) even
   though plain `git diff <rev>` on the same state prints nothing for it.
   Passing 0 reproduces the exact pre-Phase-40 behaviour; the caller still
   has to run sg_diff_fill_combined_from_index afterward to actually
   populate ours/theirs/result on the rows this widening added.

   `include_unchanged` (Phase 51): an unchanged row is only emitted when the
   path is not already being appended for some other reason -- in
   particular, an unmerged path or a path whose working-tree file is missing
   or unreadable is never unchanged, whatever `include_unchanged` says. */
int sg_diff_tree_workdir(const char *git_dir, const char *repo_root,
                         const unsigned char *old_tree, const sg_index *idx,
                         sg_diff_list *out, char *bad_path, int combined,
                         int include_unchanged);

/* Phase 40: a post-build pass over an sg_diff_tree_workdir list (`sg diff
   -c/--cc <rev>`, exactly one rev, no `--cached`) that fills in the three
   combined-diff sides described on sg_diff_entry, so sg_diff_entry_is_combined
   can recognize these rows the same way it recognizes an sg_diff_index_workdir
   conflict row. Run AFTER sg_diff_list_filter (same ordering rule as
   sg_diff_detect_renames, and for the identical reason: a pathspec should
   decide membership before this pass ever looks at a row) and BEFORE
   sg_diff_detect_renames (a combined row must never be offered to rename/copy
   detection as a source or destination -- SPEC section 5).

   Every entry in `list` is visited EXCEPT a Phase 51 `unchanged` row, which
   is skipped outright and keeps all three sides ABSENT. Filling one would
   make sg_diff_entry_is_combined answer yes for it, and rename.c's three
   source predicates all refuse a combined row -- the row would be dropped
   from the copy-source pool and the copy silently never found. Measured:
   `sg diff -c -C -C <rev>` printed `A copy.txt` where real git prints
   `C094 src.txt copy.txt`, while plain `-C -C` and plain `-c` were each
   correct on their own. Do not "simplify" the skip away.

   Otherwise, a path with no index entry at all leaves
   `ours` ABSENT, which is exactly what makes sg_diff_entry_is_combined false
   and the row fall back to plain sg_diff_tree_workdir rendering. Cannot fail
   -- it only reads already-loaded index data, no I/O. */
void sg_diff_fill_combined_from_index(const sg_index *idx, sg_diff_list *list);

/* The content id a side stands for, resolving a chunk pointer to what it
   points at. Returns 0 when that id is trustworthy, and -1 when it is the
   side's raw id copied through unverified (a genuine chunk pointer that
   could not be resolved). Callers must not treat two unverified ids that
   happen to be equal as proof the content matches. */
int sg_diff_side_effective_id(const char *git_dir, const sg_diff_side *side,
                              unsigned char out[SG_SHA1_RAW_LEN]);

/* Pairs deletions with additions and rewrites each pair as a single rename
   row: the destination entry keeps its `path`, gains the source's `old_path`
   and `old_side`, and the source entry is dropped.

   Both exact and inexact renames are found (Phase 30), in git's three
   passes and in git's order -- identical content first, settled by effective
   object id and always scoring 100; then same-file-name pairs at a raised
   threshold; then everything else scored against everything else. The order
   is part of the answer, not an optimization: a name match can settle on a
   pair the full comparison would have rejected in favour of a better one.
   See src/workdir/rename.c.

   The score is git's, to the point: it is printed in machine-readable form
   ("R093", "similarity index 93%"), so being one point off is a wrong
   answer, not a near miss. src/util/similarity.c is a deliberate port of
   git's diffcore-delta.c for that reason.

   Each source is used at most once, and ties are settled the way git settles
   them, which is NOT simply path order: among sources holding identical
   content the one sharing the destination's file name wins, and only failing
   that does the first in path order. Measured against git 2.55.0, as is the
   rest: two identical sources and two identical destinations pair up in
   order, and one source with two identical destinations claims the first
   destination and leaves the second an ordinary addition.

   MUST run after sg_diff_list_filter, never before. Measured: `git diff
   --cached --name-status -- b1.txt` on a renamed pair prints "A b1.txt", not
   the rename -- git filters by pathspec first, so a spec naming only one
   half of a rename leaves nothing to pair with. Running detection first
   would instead report a rename and then filter it, printing "R100" where
   git prints "A".

   The list stays sorted by `path` because only source entries are removed
   and destinations keep the path they already had.

   `min_score` is on git's 0..SG_SIMILARITY_MAX scale (see sg/similarity.h),
   NOT a percentage: the -M grammar can ask for a threshold finer than one
   percent, so a percentage could not hold one without rounding it. Passing 0
   means "do not detect renames at all", which is git's --no-renames;
   SG_SIMILARITY_MAX means exact renames only, which is git's -M100%.

   `repo_root` is needed because scoring reads both sides' bytes, and a
   WORKDIR side lives on disk rather than in the object store.

   `detect_copies` is git's -C. It changes three things, all measured:
   a path that exists on BOTH sides becomes eligible as a source (so a copy
   can be found from a file that was merely edited); a source may be paired
   more than once; and the same-file-name shortcut is skipped entirely.

   Phase 51 (`-C -C` / `--find-copies-harder`): a row with `unchanged` set
   (see sg_diff_entry) is registered as a source exactly like a modification
   when `detect_copies` is on -- including the `uses = 1` that lets it be
   claimed more than once -- because it already satisfies the same
   "present on both sides" test a modification does. It is never a
   destination (an unchanged row's old_side is never ABSENT) and never a
   rename source (rename detection requires detect_copies off, at which
   point an unchanged row is not registered as anything at all). Whether or
   not any of this fires, THIS FUNCTION OWNS STRIPPING: every row still
   carrying `unchanged` is removed before returning, on every returning path
   that reports success -- including the `min_score <= 0` and
   `nsrc == 0 || ndst == 0` early-outs -- so no caller needs to know the flag
   exists. A caller that never passed `include_unchanged` to a builder never
   sees a row with it set, so this is a no-op for every pre-Phase-51 call
   site.

   A side whose content cannot be read is never paired -- the same failure
   direction as an unverified id: no rename, never an invented one.

   Returns 0, or -1 on allocation failure with the list left untouched
   (including any `unchanged` rows still in it -- stripping only happens on
   the success path, matching this function's existing all-or-nothing
   failure contract). */
int sg_diff_detect_renames(const char *git_dir, const char *repo_root,
                           sg_diff_list *list, int min_score, int detect_copies);

/* Drops every entry whose path the pathspec does not cover, freeing it. An
   empty pathspec matches everything, so an unfiltered `sg diff` and a
   filtered one run the same code.

   Filtering happens here, after the four builders above have already done
   their work, rather than inside them: one filter cannot disagree with
   itself, whereas four pathspec checks -- one per builder, each with its own
   idea of when a path takes part -- is exactly the shape of the bug Phase 27
   spent a milestone removing. The cost is that a filtered `sg diff` still
   hashes every working-tree file before throwing most of them away; that is
   a speed bill, not a wrong answer.

   An unmerged path can occupy two adjacent entries (see
   sg_diff_index_workdir); both carry the same path, so both survive or both
   go, and the pair can never be split. */
void sg_diff_list_filter(sg_diff_list *list, const sg_pathspec *ps);

/* Phase 40: a stable partition, combined rows first (sg_diff_entry_is_combined),
   then non-combined rows -- each half keeping its own existing path order.
   Used only by `sg diff -c/--cc <rev>` (SPEC section 4); every other
   diff-printing path leaves the list's builder/filter/rename order alone.
   Returns 0, or -1 on allocation failure with the list left untouched. */
int sg_diff_reorder_combined_first(sg_diff_list *list);

/* Loads one side's bytes. An ABSENT side yields (NULL, 0), which is what the
   renderers want for an addition or a deletion. BLOB sides go through
   sg_chunk_read_blob, so a chunk pointer is resolved to the real content.

   Returns 0, -1 (unreadable), or -2 for a genuine chunk pointer whose data is
   missing or corrupt, filling *missing (may be NULL). -2 must not be flattened
   into -1: diffing the pointer's own raw text against the other side produces
   meaningless hunks instead of an error. */
int sg_diff_side_read(const char *git_dir, const char *repo_root, const char *path,
                      const sg_diff_side *side, unsigned char **data, size_t *len,
                      sg_chunk_missing_info *missing);

#endif /* SG_DIFF_H */

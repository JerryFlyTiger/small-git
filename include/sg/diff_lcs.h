#ifndef SG_DIFF_LCS_H
#define SG_DIFF_LCS_H

#include <stddef.h>

/* A line viewed into an existing buffer -- ptr/len are borrowed, never
   owned. len excludes the trailing '\n'; has_nl records whether one
   actually followed in the source (false only for a final line that isn't
   newline-terminated). Shared by `sg diff`'s unified-diff printer and
   by src/workdir/merge.c's three-way merge, which have used the SAME aligner
   (sg_diff_build_script, git's Myers algorithm) since Phase 41. Merge was the
   last caller of the O(na*nb) LCS table that used to be declared here, and
   the table went with it -- not tidiness: at 6000 lines a side that table
   allocated 602 MB and took 0.37s, where Myers takes 10.8 MB and under 0.01s
   for the same merge result (measured). */
typedef struct {
    const char *ptr;
    size_t len;
    int has_nl;
} sg_diff_line;

/* Splits data into lines. *count_out is always set; the returned array is
   NULL only on allocation failure (with *count_out left at 0). Caller frees
   the returned array (it never owns the underlying bytes). */
sg_diff_line *sg_diff_split_lines(const unsigned char *data, size_t len, size_t *count_out);

int sg_diff_lines_equal(sg_diff_line a, sg_diff_line b);

/* Like sg_diff_lines_equal, but ALSO requires has_nl to match. Real git
   treats "same text, different newline-termination" as a different line
   (Phase 26, measured: a final line missing its newline prints as its own
   -/+ row plus a following "\ No newline at end of file", never silently
   merged with an identically-spelled earlier line).

   The three-way merge needs it too, since Phase 41. It used to compare
   has_nl-blind on purpose, and that one choice was the dominant source of
   merge mismatch against real git: when ours' ONLY edit was removing a
   trailing newline, the merge read it as "ours changed nothing", took
   theirs, and reported success -- discarding a user's edit silently. The
   has_nl-blind sg_diff_lines_equal survives because src/cli/diff_out.c's
   combined diff still wants it. */
int sg_diff_lines_equal_exact(sg_diff_line a, sg_diff_line b);

/* ---- minimal edit script (patch-body intermediate representation) ---- */

/* One maximal run of change at a given synchronized gap: a[a_off,
   a_off+a_len) is removed, b[b_off, b_off+b_len) is added. Either a_len or
   b_len (but not both) may be 0 for a pure insertion/deletion; both
   non-zero is a "replace", meaning the two files each independently have a
   changed run at this same gap index -- NOT a distinguished case the
   alignment step pins in place. Internally (src/util/diff_lcs.c) each side is
   tracked as its own per-file "changed" bitmap and slid independently
   (mirroring git's xdiff/xdiffi.c: two xdfile_t structures, each compacted
   on its own, synchronized only by an end_matching_other tie-break so an
   edit that really is a single replace doesn't get split into a
   free-floating delete and a free-floating add); this struct is just the
   final, re-paired output of that process. Between two consecutive groups
   (or before the first / after the last), a[..]==b[..] line for line. */
typedef struct {
    size_t a_off, a_len;
    size_t b_off, b_len;
} sg_diff_group;

typedef struct {
    sg_diff_group *groups;
    size_t count;
} sg_diff_script;

/* Which alignment algorithm sg_diff_build_script runs. There is no default
   and the parameter is mandatory, the same idiom as sg_workdir_missing:
   the two are not interchangeable and picking one silently is exactly the
   bug this spells out. Measured against git 2.55.0, and the split is not
   the one an outsider would guess -- `git diff` defaults to MYERS while
   `git merge` defaults to HISTOGRAM (and honours diff.algorithm when set,
   which is how the default was measured). sg mirrors that split:
   src/cli/diff_out.c passes MYERS unless the user asked otherwise,
   src/workdir/merge.c always passes HISTOGRAM. */
typedef enum {
    SG_DIFF_ALGO_MYERS = 0,
    SG_DIFF_ALGO_HISTOGRAM = 1
} sg_diff_algorithm;

/* Builds the minimal edit script between a and b (has_nl-aware), then:
     1. group compaction -- a pure-insert or pure-delete group bordered by
        identical lines on either side can be slid up or down within its
        reachable range without changing what the diff represents (the
        classic "which of these two adjacent identical lines is the
        context line" ambiguity). Default position (indent_heuristic == 0)
        is the BOTTOM of that range -- measured against real git with
        diff.indentHeuristic off.
     2. if indent_heuristic is set, scores every reachable position using a
        reconstruction of git's indentation heuristic (diff.indentHeuristic,
        on by default since git 2.14) and picks the lowest-penalty one,
        breaking ties toward the bottom. The scoring constants
        (INDENT_WEIGHT and friends in diff_lcs.c) are a best-effort
        reconstruction from memory, NOT independently verified against
        git's source -- tests/fuzz_diff.py, which diffs sg's actual output
        against real git's, is the only judge of whether they are right.
     3. HISTOGRAM ONLY (Phase 52): when step 1/2 actually moved a group and
        the opposite file's matching group is non-empty, that pair of groups
        is re-diffed with Myers and the newly-surfaced identical lines are
        marked unchanged again. This is git's own step, in xdiffi.c's
        xdl_change_compact, and NOT part of the histogram algorithm -- which
        is exactly why Phase 42 could not find the ~1% divergence it causes
        while searching inside the xhistogram.c port. Myers is excluded
        because it already produces minimal edits, so a slid group cannot
        yield a smaller diff; git's own comment concedes that is only true
        "most of the time" without XDF_NEED_MINIMAL, which is why the
        exclusion is kept even though no measurement can currently see it
        (see docs/DESIGN.md Phase 52 item B section 7).
   Returns NULL only on allocation failure -- step 3 adds a second, new way
   for that to happen, since the re-diff allocates. */
sg_diff_script *sg_diff_build_script(const sg_diff_line *a, size_t na, const sg_diff_line *b, size_t nb,
                                    int indent_heuristic, sg_diff_algorithm algo);
void sg_diff_script_free(sg_diff_script *script);

#endif

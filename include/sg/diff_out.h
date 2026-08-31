#ifndef SG_DIFF_OUT_H
#define SG_DIFF_OUT_H

#include "sg/diff.h"
#include "sg/diff_lcs.h"

/* Rendering a change list (sg/diff.h) to stdout. Split from the list building
   so that `sg diff` and `sg stash show` render the same six ways without
   either one owning the printers. */

typedef enum {
    SG_DIFF_FORMAT_PATCH = 0, /* the default: diff --git / ---/+++ / hunks */
    SG_DIFF_FORMAT_STAT,      /* --stat */
    SG_DIFF_FORMAT_NUMSTAT,   /* --numstat */
    SG_DIFF_FORMAT_SHORTSTAT, /* --shortstat */
    SG_DIFF_FORMAT_NAME_ONLY, /* --name-only */
    SG_DIFF_FORMAT_NAME_STATUS/* --name-status */
} sg_diff_format;

typedef struct {
    sg_diff_format format;
    /* --stat=<width>[,<name-width>]. 0 means "work it out": measured against
       git 2.55.0, the total defaults to 80 when COLUMNS is unset, and git
       honours COLUMNS even when stdout is not a tty. */
    int stat_width;
    int stat_name_width;
    /* Combined-diff mode for an unmerged (both stage 2 and stage 3 present)
       row, Phase 34: 0 = not given on the command line, 1 = dense (--cc),
       2 = non-dense (-c). A single field carries both "which flag" and
       "was one given at all" -- deliberately, because the PATCH format and
       the other five formats read it differently (measured against git
       2.55.0, see CLAUDE.md's combined-diff note for the oracle):
         - PATCH treats 0 the same as 1 (dense IS the PATCH default even
           with no flag at all -- `git diff` on a conflict prints
           "diff --cc" unprompted); only 2 turns off dense.
         - The other five formats leave a combinable row rendered exactly as
           today UNLESS this is nonzero -- 0 must not be treated as "dense"
           there, or an unflagged `sg diff --name-status` on a conflict would
           silently start printing "MM" instead of "U".
       Never set by a builder, only by the CLI layer parsing -c/--cc. */
    int combined;
    /* Which alignment algorithm the patch body / --stat counts / combined
       diff run (Phase 42). Set by the CLI layer from --histogram or
       --diff-algorithm=<name>; SG_DIFF_ALGO_MYERS is git's own default for
       `git diff` and so is sg's here. NOTE the merge path deliberately does
       NOT read this: `git merge` defaults to histogram regardless of what
       `git diff` is doing (measured), so src/workdir/merge.c hard-codes
       SG_DIFF_ALGO_HISTOGRAM instead of taking it from an option. */
    sg_diff_algorithm algorithm;
    /* git's --summary block, printed AFTER the chosen format's own output
       (Phase 50). The one caller is `sg merge`'s fast-forward report, which
       is `git merge`'s own composition: measured against git 2.55.0, a
       fast-forward prints exactly `git diff --stat --summary <old> <new>`
       under its Updating/Fast-forward header. Four line shapes, all
       measured:

         create mode 100644 <path>
         delete mode 100644 <path>
         rename <compressed pair> (NN%)
         mode change 100644 => 100755 <path>

       The last one DROPS its path when it follows a rename line for the
       same entry -- git has already named it one line up. `copy` replaces
       `rename` when the row is a copy, though nothing in sg sets summary
       and copy detection at once today.

       Not a format: it composes with one, and only --stat is exercised. */
    int summary;
} sg_diff_out_opts;

/* Every format except PATCH quotes paths with sg_quote_path -- measured
   against git 2.55.0: --stat, --numstat, --name-only and --name-status all
   leave a space bare and quote only control bytes, i.e. exactly the long
   status format's rule and NOT the porcelain one. They also sort by the raw
   path bytes, not by the quoted form. (`sg diff --porcelain` does not exist;
   the field-separated quoting rule belongs to `sg status`, not here.)

   Returns 0, or -1 if any entry could not be read -- a message has already
   gone to stderr and every other entry has still been rendered, which is what
   `sg diff` does today: one unreadable blob must not silence the rest of the
   diff. The caller turns -1 into exit status 1. */
int sg_diff_print(const char *git_dir, const char *repo_root, const sg_diff_list *list,
                  const sg_diff_out_opts *opts);

/* Parses the "=<width>[,<name-width>]" suffix of --stat (the part after the
   "="; pass "" for a bare --stat with no suffix at all -- that is handled
   by the caller before this is even called, since a bare --stat needs no
   parsing). Malformed input (non-digits, non-positive, trailing garbage, or
   either field past SG_DIFF_STAT_ARG_MAX) is rejected rather than guessed
   at; both `sg diff --stat=...` and `sg stash show --stat=...` treat that
   the same as an unrecognized flag. Shared here (rather than kept a static
   helper in cmd_diff.c) so `sg stash show`, which forwards diff options the
   same way real git does, does not need a second copy. Returns 0 and fills
   *width_out / *name_width_out, or -1. */
#define SG_DIFF_STAT_ARG_MAX 1000000
int sg_diff_parse_stat_arg(const char *arg, int *width_out, int *name_width_out);

#endif /* SG_DIFF_OUT_H */

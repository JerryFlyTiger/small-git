#ifndef SG_DIFF_OUT_H
#define SG_DIFF_OUT_H

#include "sg/diff.h"

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

#endif /* SG_DIFF_OUT_H */

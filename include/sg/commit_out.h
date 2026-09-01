#ifndef SG_COMMIT_OUT_H
#define SG_COMMIT_OUT_H

#include "sg/hash.h"
#include "sg/object.h"

/* git abbreviates to core.abbrev, whose default is `auto` and scales with the
   repository's object count. sg pins 7, which is what auto yields for a small
   repository; interop declares the same on git's side rather than pretending
   the two policies agree. */
#define SG_COMMIT_OUT_ABBREV 7

typedef struct {
    int oneline; /* "<abbrev> <subject>" header instead of the full block */
    int patch;   /* -p */
    int stat;    /* --stat */
} sg_commit_out_opts;

/* Prints one commit entry exactly as `git log` / `git show` render it:
   either the "<abbrev> <subject>" oneline header, or the full
   "commit <sha>" / (optional "Merge: ...") / "Author: ..." / "Date: ..."
   block followed by the indented message -- and then, if opts->patch or
   opts->stat is set, that commit's own diff against its first parent (the
   empty tree for a root commit).

   `id` is the commit's own object id (already computed by the caller,
   which needs it anyway to walk to the next entry / print the header).
   Returns 0, or -1 having printed a partial entry (the caller decides how
   to report that -- see cmd_log.c's and cmd_show.c's own error messages). */
int sg_commit_out_entry(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                        const sg_commit *commit, const sg_commit_out_opts *opts);

#endif

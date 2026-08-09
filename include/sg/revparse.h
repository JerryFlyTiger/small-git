#ifndef SG_REVPARSE_H
#define SG_REVPARSE_H

#include "sg/hash.h"

/* Resolves a revision expression to a commit id.

   Supported grammar (deliberately a small subset of git-rev-parse, not the
   whole thing):

     <base> ::= "HEAD"
              | <branch name>          (looked up under refs/heads/)
              | <tag name>             (looked up under refs/tags/)
              | <40-char hex sha1>
     <rev>  ::= <base> ( "~" [N] | "^" [N] )*

   Base resolution order is full hex, then HEAD, then tag, then branch --
   real git's own gitrevisions disambiguation order (full SHA-1 object
   name first, then refs/<name> -> refs/tags/<name> -> refs/heads/<name>
   -> ...). Measured against real git for both halves of that order: a
   branch and a tag sharing a name resolve to the TAG's target (git prints
   a "refname is ambiguous" warning); and a branch literally NAMED like a
   full 40-hex sha1 that points somewhere else is still shadowed by the
   literal object id -- `git rev-parse <hex>` returns `<hex>` itself, not
   the branch's target, even though the branch exists. See resolve_base's
   comment in revparse.c for the exact commands this was checked against.
   "~" and "^"
   suffixes chain left to right and may repeat/mix freely (e.g.
   "HEAD~2^2~1"); a bare "~"/"^" means N=1. "~N" walks N generations via
   first parents; "^N" takes the Nth parent (1-based) of the current commit.

   If the base resolves to an annotated tag object, it is peeled (following
   sg_tag's `object` field, which may itself point at another tag) until a
   non-tag object is reached, with a bounded number of hops so a
   self-referential or cyclic chain of tag objects fails cleanly instead of
   looping forever. The final object -- after peeling and after any ~/^
   suffixes are applied -- must be a commit; a rev naming a blob or tree is
   an error, not a silent partial success.

   Deliberately NOT supported: abbreviated (prefix) object ids -- only a
   full 40-hex sha1 is accepted as a literal object id. Adding prefix
   matching later needs its own disambiguation policy (what happens on a
   short-hash collision), so it is left out here rather than guessed at.

   Returns 0 on success with commit_id_out filled in, -1 if rev is
   malformed, names nothing, or resolves to a non-commit object. Prints
   nothing to stderr; the caller (CLI layer) is responsible for any
   diagnostic. */
int sg_rev_parse_commit(const char *git_dir, const char *rev,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN]);

#endif

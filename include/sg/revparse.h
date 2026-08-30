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
              | "refs/" <rest>         (a fully-qualified ref path, used
                                         as-is if it exists -- see
                                         sg_rev_parse_ref_path's header
                                         comment)
     <rev>  ::= ( <base> | "@" ) [ "@{" N "}" ] ( "~" [N] | "^" [N] )*

   "@{N}" is git's reflog notation: it must immediately follow <base> (not
   after any "~"/"^" suffix -- "topic~1@{1}" is rejected, "topic@{1}~1" is
   accepted) and resolves to the NEW oid of logs/<ref>'s Nth-from-the-end
   entry (N=0 is the most recent), i.e. sg_reflog_at(log, N)->new_id. Any
   "~"/"^" suffixes that follow apply to that commit. The braces' content
   must be purely decimal digits (leading zeros allowed, "01" == "1") --
   deliberately NOT supported: git's "@{u}"/"@{upstream}" and date-ish
   selectors like "@{now}"/"@{2.days.ago}" (no upstream-tracking or reflog
   date index in this project). A tag has no reflog, so "<tag>@{N}" always
   fails (an empty/missing logs/refs/tags/<tag> reads as zero entries, which
   is out of range for any N).

   The two bare "@" spellings ARE supported as of Phase 48, and the first is
   the one worth knowing: a bare "@{N}" reads the CURRENT BRANCH's log, which
   is measurably NOT the same value as "HEAD@{N}" (a checkout away and back
   adds lines to logs/HEAD and none to the branch's, and git's own
   out-of-range message names the branch). On a detached HEAD there is no
   branch and it falls back to logs/HEAD; an unborn HEAD is rejected, and so
   is a CORRUPT one -- the two are separated by sg_ref_head_is_detached's
   tri-state, never by a NULL test on sg_ref_current_branch (Phase 18's
   rule). A bare "@" on its own is HEAD, suffixes included ("@~1" is
   "HEAD~1"), and means HEAD even if a branch literally named "@" exists.
   Both are implemented by rewriting <base> before anything else runs, so
   they inherit the "@{N}" lookup and the suffix loop unchanged.

   One measured case is deliberately NOT reproduced: when the current branch
   has no reflog file at all, real git lets a bare "@{0}" fall back to the
   branch's own tip while still rejecting the spelled-out "<branch>@{0}".
   sg rejects both. Reaching this at all takes deleting a log file by hand
   (sg and git both create one for every refs/heads/ update), and inventing
   an asymmetry between the two spellings is a worse answer than a uniform
   rejection.

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

/* Resolves a short <base> name (see sg_rev_parse_commit's grammar --
   "HEAD", a branch name, or a tag name; deliberately NOT a 40-hex object id,
   since an object id has no reflog) to the full ref path under git_dir that
   a reflog reader (sg_reflog_read, or sg_ref_read_path for anything other
   than "HEAD") would need: "HEAD" itself, "refs/tags/<name>",
   "refs/heads/<name>", or -- if name already starts with "refs/" -- name
   unchanged, PROVIDED that path actually exists. Tried in that order (same
   disambiguation order as resolve_base in revparse.c, minus the 40-hex
   case); the first that resolves wins. Returns 0 with out filled in
   (truncation, i.e. out_size too small, is a failure, not a silent cut), -1
   if name matches nothing. */
int sg_rev_parse_ref_path(const char *git_dir, const char *name, char *out, size_t out_size);

#endif

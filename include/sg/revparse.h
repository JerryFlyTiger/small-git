#ifndef SG_REVPARSE_H
#define SG_REVPARSE_H

#include <stddef.h>

#include "sg/hash.h"
#include "sg/object.h"

/* Phase 68b/68b-review: which reading an abbreviated (4..39 hex) object id
   prefix uses when it matches more than one object. STRICT is git's own
   default (`get_oid`) -- an ambiguous prefix is always an error. COMMITTISH
   is git's opt-in `get_oid_committish`: it resolves when EXACTLY ONE of the
   candidates is a commit or an (peeled) annotated tag; two commits, or a
   commit plus a tag, both still refuse with the full candidate list.
   TREEISH is `get_oid_treeish` -- the SAME idea widened to also count a bare
   TREE: exactly one candidate that is a commit, tag, or tree resolves.
   Measured (review round, real git 2.55.0, one 4-way tag+commit+tree+blob
   collision): `cat-file -t <amb>` (STRICT) lists all 4; `cat-file -t
   <amb>~1` (COMMITTISH, trigger 2 below) lists 2 (tag, commit); `cat-file -p
   <amb>:f.txt` (TREEISH) lists 3 (tag, commit, tree) -- three genuinely
   different filter widths, not two. A commit or tag counts toward BOTH
   COMMITTISH and TREEISH; only a bare tree is TREEISH-only.

   There are THREE ways a base can end up resolved under something other
   than the caller's own literal request, and they compose with a fixed
   priority -- see sg_rev_effective_disambig, the SINGLE function that
   decides this (do not re-derive this rule at a second call site: Phase 68b
   originally had a second, independent copy of trigger 2 inside
   sg_cli_report_ambiguous_oid, found and converged in the same review round
   that added TREEISH -- see that function's own history):
     1. (highest priority) a "~"/"^"/"@{" suffix anywhere in the rev part
        forces COMMITTISH, regardless of anything else -- `rev-parse <amb>`
        refuses while `rev-parse <amb>~1` resolves, under the SAME command,
        and `<amb>~1:f.txt` is COMMITTISH too, not TREEISH, because you
        cannot take a "generation" step from a bare tree.
     2. failing that, a "<rev>:<path>" colon form forces TREEISH -- this is
        NOT a per-command choice, `sg cat-file`/`sg show`'s <rev>:<path>
        argument is TREEISH even though both commands are otherwise STRICT
        for a bare prefix.
     3. failing both, the CALLER's own top-level request applies (measured
        per-command: `sg log`, `sg reset` and -- since Phase 69 --
        `sg rebase`'s <upstream> request COMMITTISH;
        CLAUDE.md's `sg log` -pathspec- entry documents the 15-command truth
        table trigger 1 was measured against; nothing requests TREEISH at
        the top level, it only ever arises from trigger 2). */
typedef enum {
    SG_REV_STRICT = 0,
    SG_REV_COMMITTISH,
    SG_REV_TREEISH
} sg_rev_disambig;

/* The single place that decides which of the three sg_rev_disambig values
   is actually IN EFFECT for a given rev string, per the three-way priority
   in sg_rev_disambig's own comment. `rev` may or may not contain a ':' --
   both shapes are handled: sg_rev_parse_commit_ex's own `rev` parameter
   never contains one (colon-splitting happens one layer up, in
   sg_rev_parse_object/resolve_rev_path), while sg_cli_report_ambiguous_oid
   in cli_args.h is handed the ORIGINAL, possibly colon-containing argument
   text and needs to detect the colon itself. Both callers get the correct
   answer from the same scan. `disambig` is the fallback used when neither
   trigger fires (priority 3). */
sg_rev_disambig sg_rev_effective_disambig(const char *rev, sg_rev_disambig disambig);

/* Whether the object `id` names counts as a candidate under `mode`'s
   filter -- by its PEELED type, not its own raw type (review round 3,
   measured against real git 2.55.0): a tag pointing at a blob does NOT
   count toward COMMITTISH, even though it IS a tag object -- decisive
   fixture: a tag->blob colliding with a real commit on one prefix; if tags
   counted unconditionally that would be two commit-ish candidates and
   `git log -1 <amb>` would refuse, but it resolves to the commit, so git
   is filtering by what a tag ultimately points at. A bare commit or tree
   counts directly (peeling them is a no-op); a tag is followed through its
   whole chain (which may itself be multiple tags deep) to whatever
   non-tag object it ultimately names. A tag whose peel chain fails
   (missing/corrupt target) does NOT match -- excluded from the count
   entirely, not treated as an error (see resolve_ambiguous_prefix's own
   comment on this failure direction).

   A commit or (peeled) tag-to-commit counts under COMMITTISH; those two
   plus a bare tree or (peeled) tag-to-tree count under TREEISH; STRICT
   never matches anything (STRICT has no filter -- an ambiguous prefix
   under STRICT is -4 outright, without narrowing). This is the SINGLE
   definition of "which candidates this mode accepts", shared by
   resolve_ambiguous_prefix's own disambiguation (revparse.c) and
   sg_cli_report_ambiguous_oid's candidate-list narrowing (cli_args.c) --
   converged in the Phase 68b review round for the same reason
   sg_rev_effective_disambig was: two independently-maintained copies of a
   three-way rule are one drift away from disagreeing. */
int sg_rev_object_matches_disambig(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                                   sg_rev_disambig mode);

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

   Abbreviated (prefix) object ids are supported as of Phase 68b: 4..39 hex
   characters (case-insensitive), tried in the base position (the same place
   a literal 40-hex sha1 already sits), always AFTER the ref lookup -- a ref
   literally named with a valid hex prefix wins (git warns
   "refname ... is ambiguous"; measured, and the same order a literal
   40-hex already used). See sg_rev_parse_commit_ex for the disambiguation
   policy when a prefix matches more than one object.

   Returns 0 on success with commit_id_out filled in, -1 if rev is
   malformed, names nothing, or resolves to a non-commit object, -4 if an
   abbreviated prefix in `rev` matches more than one object and the
   disambiguation policy in effect does not resolve it (see
   sg_rev_parse_commit_ex). Prints nothing to stderr; the caller (CLI layer)
   is responsible for any diagnostic (sg_cli_report_ambiguous_oid in
   cli_args.h prints the -4 case). */
int sg_rev_parse_commit(const char *git_dir, const char *rev,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN]);

/* Same as sg_rev_parse_commit, but lets the caller opt into the
   commit-ish disambiguation policy (sg_rev_disambig) for an abbreviated
   prefix in the BASE position. `sg_rev_parse_commit` itself is always
   SG_REV_STRICT -- git's own default is `get_oid`, not the opt-in
   `get_oid_committish`, and the measured table is strict for every caller
   but three: `sg log`, `sg reset` and (Phase 69) `sg rebase`'s <upstream>
   pass SG_REV_COMMITTISH here.

   WARNING: this comment said "`sg rebase` never reaches this function at
   all" until Phase 69, and that sentence was TRUE when written -- rebase
   resolved its <upstream> with sg_ref_read_branch and took no revision
   grammar at all. It went stale the moment rebase grew one, which is this
   project's recurring "one rule copied N times, one copy wrong" shape
   showing up in a brand-new copy rather than an old one: CLAUDE.md's own
   caller list was updated in the same commit and this one was not (caught
   by a cold review, not by any gate -- a stale comment compiles). It also
   pointed at "revparse.c's Phase 68 note", which never existed; the real
   write-up is in docs/DESIGN.md's Phase 68a section. If a fourth caller
   ever opts in, both copies have to move together.

   The actual mode used for the BASE is computed by
   sg_rev_effective_disambig(rev, disambig) -- the suffix-priority-1 trigger
   can override `disambig` regardless of what it says (measured: under
   `rev-parse`, which is STRICT, the bare ambiguous prefix refuses while the
   same prefix with "~1"/"^{commit}" appended resolves). `rev` here never
   contains a ':' (see sg_rev_parse_object's own comment), so
   sg_rev_effective_disambig's colon-triggered TREEISH branch cannot fire
   from this call site -- it is resolve_rev_path, one layer up, that passes
   SG_REV_TREEISH explicitly for the <rev> half of a <rev>:<path> form. */
int sg_rev_parse_commit_ex(const char *git_dir, const char *rev, sg_rev_disambig disambig,
                           unsigned char commit_id_out[SG_SHA1_RAW_LEN]);

/* Resolves a short <base> name (see sg_rev_parse_commit's grammar --
   "HEAD", a branch name, or a tag name; deliberately NOT a 40-hex object id,
   since an object id has no reflog) to the full ref path under git_dir that
   a reflog reader (sg_reflog_read, or sg_ref_read_path_resolved for
   anything other than "HEAD") would need.

   "HEAD" itself is returned first and unchanged, without requiring the ref
   to exist (see sg_ref_resolve_head's own indirection for why). Everything
   else goes through git's own gitrevisions lookup table
   (ref_rev_parse_rules), tried IN ORDER with NO early return -- a miss at
   any one rule falls through to the next, e.g. a "refs/foo" that is not
   itself a ref can still resolve via "refs/tags/refs/foo":

     "%s" (rule 1: name is itself a literal path under git_dir -- ANY file
       whose first 40 bytes are hex, not just a ref; this is what makes
       MERGE_HEAD/ORIG_HEAD/CHERRY_PICK_HEAD/REVERT_HEAD resolve),
     "refs/%s" (rule 2 -- sits BEFORE rules 3/4: a name colliding with a
       literal refs/<name> resolves there, not to a same-named tag/branch),
     "refs/tags/%s" (rule 3), "refs/heads/%s" (rule 4),
     "refs/remotes/%s" (rule 5), "refs/remotes/%s/HEAD" (rule 6 -- normally
       a symref, e.g. what `sg clone` creates via sg_ref_set_symref).

   name is gated first (a file-local predicate in revparse.c, not
   sg_ref_branch_name_is_safe) against an empty name, a leading/trailing
   '/', an empty path component, or a component that IS "." or "..";
   rejected before any rule is tried. Each candidate is probed with
   sg_ref_read_path_resolved (which follows a symref, needed for rules
   5/6), and the first one that resolves wins. Returns 0 with out filled
   in (truncation, i.e. out_size too small OR a candidate too long to fit
   its own scratch buffer, is a failure -- never a silent cut and never
   probed), -1 if name matches nothing or fails the gate.

   Callers that then read the id at the returned path must use
   sg_ref_read_path_resolved, not the plain sg_ref_read_path -- the
   returned path may itself be a symref (rules 5/6), and sg_ref_read_path
   does not follow one (resolve_base and sg_rev_parse_object both do this
   already; HEAD is the one exception, resolved via sg_ref_resolve_head
   instead). */
int sg_rev_parse_ref_path(const char *git_dir, const char *name, char *out, size_t out_size);

/* Resolves any object name -- a full 40-hex id, a ref (HEAD, branch, tag),
   anything sg_rev_parse_commit's grammar accepts, or <rev>:<path> -- to the
   object it names. An annotated tag is NOT peeled: unlike sg_rev_parse_commit,
   which exists specifically to resolve to a commit and therefore always peels,
   this function is for commands (`sg cat-file`, `sg show`) that need to
   report on whatever object the name literally denotes, tag included.
   `<rev>:<path>` resolves <rev> via sg_rev_parse_commit_ex under
   SG_REV_TREEISH (Phase 68b review round -- see sg_rev_disambig's own
   comment, priority 2: a colon form is TREEISH regardless of which command
   is asking, even though `cat-file`/`show` are otherwise STRICT for a bare
   prefix), which peels a resolved tag/commit but leaves a resolved BARE
   TREE unpeeled-and-rejected the same way it always has (sg_rev_parse_commit
   -- and _ex -- only ever yield a COMMIT; a rev that resolves to a bare
   tree fails there with -1, a PRE-EXISTING gap that predates Phase 68 and
   is not fixed by it -- measured: `sg cat-file -p <full-40-hex-tree>:f.txt`
   already failed before any abbreviation code existed, while real git
   succeeds; see CLAUDE.md/interop for the pinned divergence). There is no
   way to name an annotated tag's own tree/blob via this syntax any more
   than git offers one. Then walks the resolved tree component by component;
   an empty <path> means the commit's own tree.

   Resolution order for a name with no ':': full 40-hex id, then a ref path
   (HEAD/tag/branch, tried via sg_rev_parse_ref_path, unpeeled), then an
   abbreviated (4..39 hex) prefix -- ALWAYS SG_REV_STRICT here (measured:
   `cat-file`/`show` refuse an ambiguous prefix outright, unlike `log`) --
   then sg_rev_parse_commit's full grammar (~/^/@{N} suffixes, which does
   peel, only ever yields a commit, and inherits trigger 1's suffix-forces-
   committish rule from sg_rev_parse_commit_ex regardless of the STRICT
   choice made one step above).

   Peel syntax (`^{tree}`, `^{commit}`, `^{blob}`) is NOT supported anywhere
   in this project; an argument using it is rejected outright as "not a
   valid object name", the same as any other unrecognized rev.

   Returns 0 with *id_out and *type_out filled in; -1 if `arg` does not name any
   object; -2 if <rev> resolved but <path> does not exist inside its tree,
   with `bad_path` (a buffer of size `bad_path_size`) filled with the <path>
   half of `arg` (truncation is a hard failure, folded into -1 instead, same
   as -1's "malformed" case). Prints nothing to stderr -- the caller (CLI
   layer) is responsible for any diagnostic, matching sg_rev_parse_commit's
   own convention.
   Returns -3 when `arg` is a well-formed 40-hex id whose object cannot be
   read: the name is valid, the object is missing or corrupt, and saying
   "not a valid object name" for it would name the wrong problem.
   Returns -4 when an abbreviated prefix in `arg` matches more than one
   object (see sg_rev_parse_commit_ex's own comment for the disambiguation
   rules this inherits when falling through to sg_rev_parse_commit). */
int sg_rev_parse_object(const char *git_dir, const char *arg,
                        unsigned char id_out[SG_SHA1_RAW_LEN], sg_obj_type *type_out,
                        char *bad_path, size_t bad_path_size);

#endif

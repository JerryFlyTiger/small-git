#include "sg/revparse.h"

#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/refs.h"
#include "sg/reflog.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* An annotated tag can (maliciously or accidentally) point at another tag,
   including itself; this bounds how many "object" hops sg_rev_parse_commit
   will follow before giving up, so a cycle fails cleanly instead of
   spinning forever. No real repository nests tags anywhere close to this
   deep. */
#define SG_REVPARSE_MAX_TAG_HOPS 10

/* Follows tag objects (via sg_tag's `object` field) starting at id until a
   non-tag object is reached, overwriting id in place. *type_out receives
   the final object's type. Returns 0 on success, -1 if any object along
   the chain is unreadable/malformed or the chain is too deep. */
static int peel_to_non_tag(const char *git_dir, unsigned char id[SG_SHA1_RAW_LEN], sg_obj_type *type_out)
{
    int hops;

    for (hops = 0;; hops++) {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0)
            return -1;
        if (type != SG_OBJ_TAG) {
            free(content);
            *type_out = type;
            return 0;
        }
        if (hops >= SG_REVPARSE_MAX_TAG_HOPS) {
            free(content);
            return -1;
        }
        {
            sg_tag tag;

            if (sg_tag_parse(content, content_len, &tag) != 0) {
                free(content);
                return -1;
            }
            free(content);
            memcpy(id, tag.object, SG_SHA1_RAW_LEN);
            sg_tag_free(&tag);
        }
    }
}

/* Reads the commit at id and copies its Nth parent (1-based) into out.
   `out` may alias `id`. Returns -1 if id isn't a commit, or has fewer than
   N parents. */
static int commit_nth_parent(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                             unsigned long n, unsigned char out[SG_SHA1_RAW_LEN])
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;

    if (n == 0)
        return -1;
    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0)
        return -1;
    if (type != SG_OBJ_COMMIT) {
        free(content);
        return -1;
    }
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return -1;
    }
    free(content);
    if (n > (unsigned long)commit.parent_count) {
        sg_commit_free(&commit);
        return -1;
    }
    memcpy(out, commit.parents[n - 1], SG_SHA1_RAW_LEN);
    sg_commit_free(&commit);
    return 0;
}

/* Parses the (possibly empty) run of decimal digits [s, s+len) that follows
   a '~' or '^'. An empty run means the implicit N=1. Leading zeros are
   legal and don't count against the digit-count guard below -- measured
   against real git, "HEAD~0000000001" works and means the same as
   "HEAD~1" -- so the guard is applied to the number of SIGNIFICANT digits
   (after skipping leading zeros, always keeping at least one digit so
   "000" still parses as 0), not the raw character count. More than 9
   significant digits is rejected outright -- no real history is anywhere
   near that deep, and it keeps the accumulation below comfortably under
   overflow regardless of `unsigned long`'s width. */
static int parse_suffix_number(const char *s, size_t len, unsigned long *out)
{
    size_t i;
    size_t start;
    unsigned long v = 0;

    if (len == 0) {
        *out = 1;
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
    }
    start = 0;
    while (start < len - 1 && s[start] == '0')
        start++;
    if (len - start > 9)
        return -1;
    for (i = start; i < len; i++)
        v = v * 10 + (unsigned long)(s[i] - '0');
    *out = v;
    return 0;
}

/* True iff s[0..len) is entirely hex digits (upper or lower case),
   len >= SG_OID_MIN_ABBREV and < SG_SHA1_HEX_LEN -- exactly the shape
   sg_object_find_prefix accepts. A stricter length check than
   sg_object_find_prefix's own (which just rejects on a bad prefix) so
   callers here can tell "not even worth trying as a prefix" apart from
   "tried it and it happens not to exist" without relying on -1 meaning two
   different things. */
static int looks_like_oid_prefix(const char *s, size_t len)
{
    size_t i;

    if (len < SG_OID_MIN_ABBREV || len >= SG_SHA1_HEX_LEN)
        return 0;
    for (i = 0; i < len; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

sg_rev_disambig sg_rev_effective_disambig(const char *rev, sg_rev_disambig disambig)
{
    const char *colon = strchr(rev, ':');
    size_t limit = colon != NULL ? (size_t)(colon - rev) : strlen(rev);
    size_t i = 0;

    /* Priority 1: a "~"/"^"/"@{" suffix anywhere before the colon (or
       before the end, if there is none) forces COMMITTISH -- the exact
       same scan sg_rev_parse_commit_ex's own base_len loop performs, just
       bounded at `limit` so a colon's PATH half is never mistaken for
       containing one. */
    while (i < limit && rev[i] != '~' && rev[i] != '^' && !(rev[i] == '@' && rev[i + 1] == '{'))
        i++;
    if (i < limit)
        return SG_REV_COMMITTISH;

    /* Priority 2: no suffix, but a colon -- a "<rev>:<path>" form is
       TREEISH regardless of the caller's own request (resolve_rev_path
       passes SG_REV_TREEISH explicitly and never reaches this branch
       itself, since its own `rev` has already had the colon stripped off;
       this branch exists for sg_cli_report_ambiguous_oid, which is handed
       the ORIGINAL, still-colon-containing argument text). */
    if (colon != NULL)
        return SG_REV_TREEISH;

    /* Priority 3: neither trigger fired, use what the caller asked for. */
    return disambig;
}

int sg_rev_object_matches_disambig(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                                   sg_rev_disambig mode)
{
    unsigned char peeled[SG_SHA1_RAW_LEN];
    sg_obj_type type;

    /* STRICT has no filter -- an ambiguous prefix under STRICT is -4
       outright, without narrowing, so callers only ever need this
       predicate for the other two modes. */
    if (mode == SG_REV_STRICT)
        return 0;

    memcpy(peeled, id, SG_SHA1_RAW_LEN);
    /* Review round 3 (measured against real git 2.55.0): membership is
       decided by the PEELED type, not the candidate's own raw type -- a
       tag pointing at a blob does NOT count toward COMMITTISH, even though
       it IS a tag object. Decisive fixture: a tag->blob colliding with a
       real commit on the same 4-hex prefix. `cat-file -t <amb>` (STRICT,
       raw types) lists both the tag and the commit; if COMMITTISH counted
       by raw type that would be two commit-ish candidates and `log -1
       <amb>` would refuse -- it does not, it resolves to the commit, so
       git is counting the tag's PEELED type (a blob) and excluding it.
       Reusing peel_to_non_tag here (the SAME peeling sg_rev_parse_commit_ex
       itself performs on whatever this function eventually picks) keeps
       there being exactly one peeling implementation in this file. A
       candidate whose peel chain fails (missing/corrupt tag target) does
       NOT match -- it is EXCLUDED from the count, not treated as a
       resolution failure; see resolve_ambiguous_prefix's own comment on
       why "exclude" rather than "fail loudly" is the accepted tradeoff
       here. */
    if (peel_to_non_tag(git_dir, peeled, &type) != 0)
        return 0;

    if (mode == SG_REV_COMMITTISH)
        return type == SG_OBJ_COMMIT;
    return type == SG_OBJ_COMMIT || type == SG_OBJ_TREE; /* SG_REV_TREEISH */
}

/* Resolves an abbreviated hex prefix (4..39 characters, already validated by
   looks_like_oid_prefix) to a single raw object id, per `disambig`:

     - 0 matches: -1 (not found -- matches git's plain "ambiguous argument"
       wording for a well-formed-but-absent prefix, not "short object ID is
       ambiguous").
     - 1 match: that object's id, 0.
     - >1 matches, SG_REV_STRICT: -4.
     - >1 matches, SG_REV_COMMITTISH or SG_REV_TREEISH: if EXACTLY ONE
       candidate matches sg_rev_object_matches_disambig for that mode
       (by PEELED type, not raw type -- see that function's own comment),
       that candidate's id, 0; otherwise -4, same as STRICT -- e.g. two
       commits, or a commit plus a tag pointing at ANOTHER commit, both
       still refuse under COMMITTISH (measured, section 4 of the Phase 68
       spec); a commit+tag(->commit)+tree collision still refuses under
       TREEISH (three matches, not one -- measured in the Phase 68b review
       round).

   The id itself, not its type, is returned -- this function does not know
   or care whether the ultimate caller wants a commit; sg_rev_parse_object's
   own prefix branch reads the type back out separately for exactly that
   reason. */
static int resolve_ambiguous_prefix(const char *git_dir, const char *prefix, sg_rev_disambig disambig,
                                    unsigned char id_out[SG_SHA1_RAW_LEN])
{
    sg_oid_list list;
    int rc = -1;

    if (sg_object_find_prefix(git_dir, prefix, &list) != 0)
        return -1;

    if (list.count == 0) {
        rc = -1;
    } else if (list.count == 1) {
        memcpy(id_out, list.ids[0], SG_SHA1_RAW_LEN);
        rc = 0;
    } else if (disambig == SG_REV_STRICT) {
        rc = -4;
    } else {
        size_t i;
        size_t match_count = 0;
        size_t match_idx = 0;

        for (i = 0; i < list.count; i++) {
            /* An UNREADABLE candidate (sg_rev_object_matches_disambig
               returns 0 for it, same as a genuine non-match) is simply
               excluded from the count, never counted as a match. If it
               happened to be the only real commit-ish/tree-ish candidate,
               this makes the whole call refuse (-4) rather than either
               silently resolving to the WRONG object or crashing --
               deliberately the "refuse" failure direction, not "guess". */
            if (sg_rev_object_matches_disambig(git_dir, list.ids[i], disambig)) {
                match_count++;
                match_idx = i;
            }
        }
        if (match_count == 1) {
            memcpy(id_out, list.ids[match_idx], SG_SHA1_RAW_LEN);
            rc = 0;
        } else {
            rc = -4;
        }
    }

    sg_oid_list_free(&list);
    return rc;
}

/* Resolves just the <base> part of the grammar (see revparse.h) to a raw
   object id, trying a literal 40-hex sha1, then HEAD, then tag, then
   branch, then (Phase 68b) an abbreviated hex prefix, in that order -- the
   first that matches wins. This is real git's own gitrevisions
   disambiguation order (full SHA-1 object name, then refs/<name> ->
   refs/tags/<name> -> refs/heads/<name> -> ..., then an abbreviated
   prefix); measured against real git for all three of the interesting
   orderings:

     - a branch and a tag both named "foo" -- `git rev-parse foo` resolves
       to the TAG's target (with a "refname is ambiguous" warning), not
       the branch's; tag beats branch.
     - a branch literally NAMED like a full hex sha1 (e.g. `git branch
       "$C1_HEX" "$C2"`, so refs/heads/<C1_hex> points at C2) --
       `git rev-parse "$C1_HEX"` resolves to C1_HEX ITSELF (the literal
       object id), not to C2, even though a same-named branch exists. Full
       hex wins over everything, including a branch whose name happens to
       collide with it, which is the counter-intuitive case worth calling
       out here.
     - the MIRROR IMAGE at prefix length: a branch literally named with a
       valid 6-hex prefix (also a valid abbreviation) WINS over the prefix
       interpretation (git warns "refname '...' is ambiguous"), the exact
       opposite direction from the 40-hex case above. sg's ref lookup
       already runs before the new prefix branch, so both directions fall
       out of the existing order for free -- do not "unify" the two,
       CLAUDE.md's `sg_rev_parse_commit` entry pins them as a head-on pair.

   Consistent with the 40-hex case: once base looks like a full 40-hex sha1
   (right length, all hex digits), that IS the answer -- if the object
   doesn't exist, this returns failure without falling back to a same-named
   ref, matching `git rev-parse --verify` on a well-formed-but-absent
   sha1. An abbreviated prefix has no such shortcut: it is only tried after
   the ref lookup has already failed.

   Does not peel tags or apply ~/^ suffixes. Returns 0 on success, -1 if
   nothing matches, -4 if an abbreviated prefix is ambiguous under
   `disambig` (see resolve_ambiguous_prefix). */
static int resolve_base(const char *git_dir, const char *base, sg_rev_disambig disambig,
                        unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char ref_path[SG_PATH_MAX];
    size_t len = strlen(base);

    if (len == SG_SHA1_HEX_LEN)
        return sg_hex_to_sha1(base, id_out);

    if (sg_rev_parse_ref_path(git_dir, base, ref_path, sizeof(ref_path)) == 0) {
        /* "HEAD" is a symref ("ref: refs/heads/<branch>\n"), not a raw oid
           file, so it needs sg_ref_resolve_head's indirection rather than
           sg_ref_read_path_resolved (which would follow it exactly the
           same way, but HEAD's own indirection has its own detached-HEAD
           handling that sg_ref_resolve_head owns). Every other path
           sg_rev_parse_ref_path can return may ITSELF be a symref as of
           Phase 70 (rules 5/6 -- refs/remotes/<name>[/HEAD] -- ordinarily
           point through refs/remotes/<name>/HEAD), so this must follow
           through it with sg_ref_read_path_resolved, not the
           non-following sg_ref_read_path: for every other, ordinary oid
           ref path the two read identically, so this is not a behavior
           change for anything but a symref result. */
        if (strcmp(ref_path, "HEAD") == 0)
            return sg_ref_resolve_head(git_dir, id_out);
        return sg_ref_read_path_resolved(git_dir, ref_path, id_out);
    }

    if (looks_like_oid_prefix(base, len))
        return resolve_ambiguous_prefix(git_dir, base, disambig, id_out);

    return -1;
}

int sg_rev_parse_ref_path(const char *git_dir, const char *name, char *out, size_t out_size)
{
    unsigned char tmp[SG_SHA1_RAW_LEN];
    char candidate[SG_PATH_MAX];
    size_t i;

    /* "HEAD" is a symref ("ref: refs/heads/<branch>\n"), not a raw oid
       file -- returned without requiring the ref to exist, since @{N} on
       an unborn or detached HEAD depends on that (resolve_base handles the
       indirection separately). This stays first and unchanged. */
    if (strcmp(name, "HEAD") == 0) {
        if (out_size < 5)
            return -1;
        memcpy(out, "HEAD", 5);
        return 0;
    }

    /* This gate (Phase 70, section 3.1 of the spec) blocks a <base> string
       from becoming a hostile ref path BEFORE any of the six gitrevisions
       patterns below are tried against it -- deliberately NOT a tightening
       of sg_ref_branch_name_is_safe (refs.c), which has many other callers
       (ref writes, transport, branch reads) this project has a recorded
       lesson about not converging blindly. sg_ref_path_components_are_safe
       (refs.h) is the shared, stricter predicate used here AND (Phase 70b)
       by sg_ref_read_path_resolved's own symref-hop-target check -- see
       its header comment for why the two call sites are both needed and
       neither is redundant with the other. */
    if (!sg_ref_path_components_are_safe(name))
        return -1;

    /* git's own gitrevisions lookup order (`ref_rev_parse_rules`), tried in
       order, first hit wins. No early return between rules: a miss at any
       one of them falls through to the next (measured, section 2.4 -- e.g.
       a "refs/foo" that does not exist as a literal ref must still be
       tried as "refs/refs/foo", "refs/tags/refs/foo", etc). Each candidate
       is probed with sg_ref_read_path_resolved, which follows symrefs (rules
       5/6 need this: refs/remotes/<name>/HEAD is ordinarily a symref, and
       sg clone itself creates exactly this shape via sg_ref_set_symref). */
    static const char *const patterns[] = {
        "%s",
        "refs/%s",
        "refs/tags/%s",
        "refs/heads/%s",
        "refs/remotes/%s",
        "refs/remotes/%s/HEAD",
    };

    for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        int len = snprintf(candidate, sizeof(candidate), patterns[i], name);

        /* A truncated candidate must count as "this rule missed", never as
           "found something else" -- the project's standing sg_path_join
           rule -- so it must not even be probed. */
        if (len < 0 || (size_t)len >= sizeof(candidate))
            continue;
        if (sg_ref_read_path_resolved(git_dir, candidate, tmp) != 0)
            continue;
        if (strlen(candidate) >= out_size)
            return -1;
        strcpy(out, candidate);
        return 0;
    }

    return -1;
}

int sg_rev_parse_commit_ex(const char *git_dir, const char *rev, sg_rev_disambig disambig,
                           unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    char base[SG_PATH_MAX];
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    size_t base_len;
    size_t pos;
    sg_rev_disambig base_disambig;
    int rc;

    if (rev == NULL || rev[0] == '\0')
        return -1;

    /* The base ends at '~', '^', or the start of an "@{N}" reflog suffix
       ("@" followed immediately by "{" -- an '@' anywhere else, e.g. inside
       an email-shaped ref name, stays part of the base). */
    base_len = 0;
    while (rev[base_len] != '\0' && rev[base_len] != '~' && rev[base_len] != '^' &&
          !(rev[base_len] == '@' && rev[base_len + 1] == '{'))
        base_len++;

    /* The single shared decision (sg_rev_effective_disambig) -- `rev` never
       contains a ':' at this call site (see sg_rev_parse_object's comment),
       so only priority 1 (a suffix left over after the base -- a "~"/"^"
       run below, or an "@{N}" handled in its own branch further down) can
       override `disambig` here; priority 2 (a colon) is resolve_rev_path's
       own job, one layer up. Measured: under `rev-parse` (STRICT), the bare
       ambiguous prefix refuses while the same prefix with "~1" appended
       resolves -- an implementation keyed only on `disambig` gets every
       suffixed form wrong. */
    base_disambig = sg_rev_effective_disambig(rev, disambig);

    if (base_len >= sizeof(base))
        return -1;
    memcpy(base, rev, base_len);
    base[base_len] = '\0';

    /* Two bare "@" spellings, both measured against git 2.55.0 (Phase 48).
       They are resolved here, by rewriting base, so that everything below --
       the @{N} lookup, resolve_base, and the ~/^ suffix loop -- keeps working
       on an ordinary name and needs no second code path. "@{1}~1" and "@~1"
       both parse for free because of it. */
    if (base_len == 0) {
        /* An empty base is only legal as the "@{N}" shorthand. "~1" or "^"
           with nothing in front of it is still a parse error. */
        if (rev[0] != '@' || rev[1] != '{')
            return -1;

        /* Measured: `git rev-parse @{1}` reads the CURRENT BRANCH's log, not
           HEAD's -- with a checkout between them the two logs give different
           answers, and git's own out-of-range message names "master", not
           "HEAD". On a detached HEAD there is no branch and it reads HEAD's
           log instead. An unborn HEAD resolves to neither and is rejected,
           which is also what git does (it prints the argument back and
           fails). A CORRUPT HEAD must not be mistaken for detached, hence
           sg_ref_head_is_detached's tri-state rather than a NULL test on
           sg_ref_current_branch (Phase 18's rule).

           Note the two predicates do not partition every conceivable HEAD:
           sg_ref_head_is_detached answers 0 for any "ref: ..." line, while
           sg_ref_current_branch returns NULL unless the target is under
           refs/heads/. A HEAD symlinked somewhere else entirely would land
           in the else branch and be rejected by the NULL test -- the safe
           direction, and unreachable anyway since nothing in this codebase
           writes such a HEAD. */
        {
            int detached = sg_ref_head_is_detached(git_dir);
            char *branch;

            if (detached < 0)
                return -1;
            if (detached == 1) {
                memcpy(base, "HEAD", 5);
            } else {
                branch = sg_ref_current_branch(git_dir);
                if (branch == NULL)
                    return -1;
                if (strlen(branch) >= sizeof(base)) {
                    free(branch);
                    return -1;
                }
                strcpy(base, branch);
                free(branch);
            }
        }
    } else if (strcmp(base, "@") == 0) {
        /* Measured: "@" on its own is HEAD, suffixes and all ("@~1" is
           HEAD~1). Rewriting it here rather than teaching resolve_base about
           it keeps the rule in one place, and deliberately makes "@" mean
           HEAD even if a branch literally named "@" exists -- which is git's
           documented rule too. */
        memcpy(base, "HEAD", 5);
    }

    pos = base_len;

    if (rev[pos] == '@') {
        /* rev[pos + 1] == '{', guaranteed by the scan loop above. "@{N}"
           must immediately follow the base -- "topic~1@{1}" stops the base
           scan at '~', so "@{1}" is left for the ~/^ suffix loop below,
           where it fails to parse as a number and is rejected. That is the
           whole enforcement of "@{N} must be adjacent to the ref name"; no
           separate check is needed here.

           NOTE: `base_disambig`, computed above, is NOT used anywhere in
           this branch. An "@{N}" base names a REF (resolved via
           sg_rev_parse_ref_path just below, then read out of its reflog),
           never an abbreviated object id -- an id has no reflog to index
           into -- so there is no ambiguous-prefix question here for
           `base_disambig` to answer. It is still computed unconditionally
           above (rather than only on the other branch) because the suffix
           scan that feeds it is shared with base_len's own computation;
           do not read its presence here as this branch load-bearing on
           disambiguation, and do not "clean up" by deleting it -- the
           other branch (resolve_base, below) needs it. */
        char ref_path[SG_PATH_MAX];
        sg_reflog log;
        const sg_reflog_entry *entry;
        size_t start;
        size_t digits;
        unsigned long idx;

        pos += 2;
        start = pos;
        while (rev[pos] != '\0' && rev[pos] != '}')
            pos++;
        if (rev[pos] != '}')
            return -1;
        digits = pos - start;
        /* Reject "@{}" outright -- parse_suffix_number would otherwise
           treat an empty run as the implicit N=1, which is correct for a
           bare "~"/"^" but wrong here: real git rejects "topic@{}". Content
           that isn't purely digits (e.g. "@{u}", "@{now}", "@{-1}") is
           caught by parse_suffix_number's own digit scan -- sg does not
           support git's upstream/date reflog selectors, so those must fail
           to parse rather than be silently misread as an index. */
        if (digits == 0)
            return -1;
        if (parse_suffix_number(rev + start, digits, &idx) != 0)
            return -1;
        pos++; /* past '}' */

        if (sg_rev_parse_ref_path(git_dir, base, ref_path, sizeof(ref_path)) != 0)
            return -1;
        if (sg_reflog_read(git_dir, ref_path, &log) != 0)
            return -1;
        /* @{N} for N>=1 names the NEW oid of that log entry -- the value the
           ref was moved TO, not the value it had before (see sg_reflog_at's
           header comment; @{1}'s old_id is not "the previous commit", it
           can be all-zeros for a ref's very first entry). The log entry at
           idx must exist either way -- this is what makes @{0} still refuse
           when the reflog has been deleted, exactly like every other
           index. */
        entry = sg_reflog_at(&log, (size_t)idx);
        if (entry == NULL) {
            sg_reflog_free(&log);
            return -1;
        }
        if (idx == 0) {
            /* @{0} means the ref's CURRENT value, NOT the log's own last
               new_id -- these differ whenever the ref file was moved
               without a matching reflog append (measured against real git
               2.55.0: a hand-edited ref file, or a moved symref target,
               both leave the reflog stale while the ref itself points
               somewhere new). The existence check above is still required
               (a ref whose log has been deleted entirely must still
               refuse, matching git) -- only the SOURCE of the oid changes
               here, never the existence gate.

               Every rule sg_rev_parse_ref_path can return may itself be a
               symref as of Phase 70 (rules 5/6), so this reads the current
               value through the same functions every other symref-aware
               reader in this file uses: sg_ref_resolve_head for "HEAD"
               (its own detached-vs-symbolic indirection), and
               sg_ref_read_path_resolved (which follows a symref) for
               everything else -- never sg_ref_read_path, which does not
               follow one. */
            int rc2;

            if (strcmp(ref_path, "HEAD") == 0)
                rc2 = sg_ref_resolve_head(git_dir, id);
            else
                rc2 = sg_ref_read_path_resolved(git_dir, ref_path, id);
            sg_reflog_free(&log);
            if (rc2 != 0)
                return -1;
        } else {
            memcpy(id, entry->new_id, SG_SHA1_RAW_LEN);
            sg_reflog_free(&log);
        }
    } else {
        rc = resolve_base(git_dir, base, base_disambig, id);
        if (rc != 0)
            return rc;
    }

    if (peel_to_non_tag(git_dir, id, &type) != 0)
        return -1;
    if (type != SG_OBJ_COMMIT)
        return -1;

    while (rev[pos] != '\0') {
        char op = rev[pos];
        size_t start;
        unsigned long n;

        /* op must genuinely be '~' or '^' -- without this check, any other
           character reaching this loop (e.g. a trailing garbage byte left
           over right after an "@{N}" that this function's caller assumed
           was fully consumed) would silently fall into the '^' branch
           below, since the code only special-cases '~' and treats
           "anything else" as '^'. That let "master@{0}x" parse as
           "master@{0}^1" instead of being rejected -- measured against real
           git, which rejects it outright ("ambiguous argument"). */
        if (op != '~' && op != '^')
            return -1;

        pos++;
        start = pos;
        while (rev[pos] != '\0' && rev[pos] != '~' && rev[pos] != '^')
            pos++;
        if (parse_suffix_number(rev + start, pos - start, &n) != 0)
            return -1;

        if (op == '~') {
            unsigned long k;

            for (k = 0; k < n; k++) {
                if (commit_nth_parent(git_dir, id, 1, id) != 0)
                    return -1;
            }
        } else { /* '^' */
            if (n != 0 && commit_nth_parent(git_dir, id, n, id) != 0)
                return -1;
        }
    }

    memcpy(commit_id_out, id, SG_SHA1_RAW_LEN);
    return 0;
}

int sg_rev_parse_commit(const char *git_dir, const char *rev,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    return sg_rev_parse_commit_ex(git_dir, rev, SG_REV_STRICT, commit_id_out);
}

/* Splits `arg` at `colon` and resolves the left side via
   sg_rev_parse_commit_ex under SG_REV_TREEISH (peeling a resolved
   commit/tag), then walks its tree component by component to find the
   entry named by the right side (an empty right side means the commit's
   own tree). Returns 0 with *id_out and *type_out filled in; -1 if `rev`
   itself does not resolve, is too long, or resolves to a BARE TREE --
   sg_rev_parse_commit_ex only ever yields a commit, so a rev that resolves
   directly to a tree object (ambiguous or not, abbreviated or a full
   40-hex) fails here exactly as it always has. This is a PRE-EXISTING gap
   that predates Phase 68 entirely (measured: a full 40-hex tree id in
   `<rev>:<path>` already failed before any abbreviation code existed) and
   is deliberately not fixed by this phase -- pinned as a named divergence
   in interop rather than silently reproduced. -2 if `rev` resolved but
   `path` does not exist inside its tree, with bad_path filled with `path`
   (truncation of `path` into bad_path is folded into -1, not -2, matching
   the header's "truncation is a hard failure" note). Prints nothing. */
static int resolve_rev_path(const char *git_dir, const char *arg, const char *colon,
                            unsigned char id_out[SG_SHA1_RAW_LEN], sg_obj_type *type_out,
                            char *bad_path, size_t bad_path_size)
{
    char rev[SG_PATH_MAX];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    const char *path;
    size_t rev_len = (size_t)(colon - arg);

    if (rev_len >= sizeof(rev))
        return -1;
    memcpy(rev, arg, rev_len);
    rev[rev_len] = '\0';
    path = colon + 1;

    {
        /* Phase 68b review: TREEISH, not STRICT -- section 2/priority 2 of
           sg_rev_disambig's own comment. A resolved bare TREE still fails
           here (sg_rev_parse_commit_ex only ever yields a commit), which is
           the pre-existing "<tree>:<path> doesn't work" gap, deliberately
           left as-is -- see this function's own header comment. */
        int prc = sg_rev_parse_commit_ex(git_dir, rev, SG_REV_TREEISH, commit_id);

        if (prc != 0)
            return prc; /* propagates -4 (ambiguous prefix) as well as -1 */
    }
    if (sg_commit_tree_of(git_dir, commit_id, tree_id) != 0)
        return -1;

    if (path[0] == '\0') {
        memcpy(id_out, tree_id, SG_SHA1_RAW_LEN);
        *type_out = SG_OBJ_TREE;
        return 0;
    }

    if (strlen(path) >= bad_path_size)
        return -1;
    strcpy(bad_path, path);

    {
        char pathbuf[SG_PATH_MAX];
        char *saveptr = NULL;
        char *comp;
        unsigned char cur_id[SG_SHA1_RAW_LEN];

        if (strlen(path) >= sizeof(pathbuf))
            return -2;
        strcpy(pathbuf, path);
        memcpy(cur_id, tree_id, SG_SHA1_RAW_LEN);

        for (comp = strtok_r(pathbuf, "/", &saveptr); comp != NULL;
            comp = strtok_r(NULL, "/", &saveptr)) {
            unsigned char *content;
            size_t content_len;
            sg_obj_type type;
            sg_tree tree;
            size_t i;
            int found = 0;

            if (sg_object_read(git_dir, cur_id, &type, &content, &content_len) != 0)
                return -2;
            /* A SUCCESSFUL read of a non-tree still owns `content`: reached
               whenever a middle component names a file rather than a
               directory (`HEAD:f.txt/x`, an ordinary typo), so folding this
               into the condition above leaked the whole decompressed blob. */
            if (type != SG_OBJ_TREE) {
                free(content);
                return -2;
            }
            if (sg_tree_parse(content, content_len, &tree) != 0) {
                free(content);
                return -2;
            }
            free(content);

            for (i = 0; i < tree.count; i++) {
                if (strcmp(tree.entries[i].name, comp) == 0) {
                    memcpy(cur_id, tree.entries[i].sha1, SG_SHA1_RAW_LEN);
                    found = 1;
                    break;
                }
            }
            sg_tree_free(&tree);

            if (!found)
                return -2;
        }

        {
            unsigned char *content;
            size_t content_len;

            if (sg_object_read(git_dir, cur_id, type_out, &content, &content_len) != 0)
                return -2;
            free(content);
        }
        memcpy(id_out, cur_id, SG_SHA1_RAW_LEN);
        return 0;
    }
}

int sg_rev_parse_object(const char *git_dir, const char *arg,
                        unsigned char id_out[SG_SHA1_RAW_LEN], sg_obj_type *type_out,
                        char *bad_path, size_t bad_path_size)
{
    const char *colon = strchr(arg, ':');
    char ref_path[SG_PATH_MAX];

    if (colon != NULL)
        return resolve_rev_path(git_dir, arg, colon, id_out, type_out, bad_path, bad_path_size);

    if (strlen(arg) == SG_SHA1_HEX_LEN && sg_hex_to_sha1(arg, id_out) == 0) {
        unsigned char *content;
        size_t content_len;

        /* -3, not -1: the argument IS a well-formed object id, so calling it
           "not a valid object name" would name the wrong problem -- the
           object is missing or unreadable (a packed REF_DELTA whose base is
           gone, say). interop pins that distinction. */
        if (sg_object_read(git_dir, id_out, type_out, &content, &content_len) != 0)
            return -3;
        free(content);
        return 0;
    }

    /* A tag must not be peeled: resolve to a ref path and read the id it
       names directly (whatever object that turns out to be), only falling
       back to sg_rev_parse_commit (which does peel, and understands
       ~/^/@{N} suffixes) when the name isn't a ref at all. */
    if (sg_rev_parse_ref_path(git_dir, arg, ref_path, sizeof(ref_path)) == 0) {
        int rc;

        /* HEAD is a symref, not a raw-oid file -- same special case
           sg_rev_parse_commit's own caller makes above, for the same
           reason: sg_ref_read_path_resolved would follow it fine but
           HEAD's own detached-HEAD handling belongs to sg_ref_resolve_head.
           Every other ref_path may itself be a symref as of Phase 70
           (rules 5/6), so this must follow through it rather than use the
           non-following sg_ref_read_path -- see resolve_base's identical
           comment above. */
        if (strcmp(ref_path, "HEAD") == 0)
            rc = sg_ref_resolve_head(git_dir, id_out);
        else
            rc = sg_ref_read_path_resolved(git_dir, ref_path, id_out);

        if (rc == 0) {
            unsigned char *content;
            size_t content_len;

            if (sg_object_read(git_dir, id_out, type_out, &content, &content_len) != 0)
                return -1;
            free(content);
            return 0;
        }
    }

    /* Phase 68b: an abbreviated hex prefix (4..39 characters), tried after
       the ref lookup above (section 2 of the spec: a ref wins) and ALWAYS
       SG_REV_STRICT here -- measured, `cat-file`/`show` refuse an ambiguous
       prefix outright rather than dwimming to a commit the way `log` does.
       This must resolve to whatever object the prefix names, any type, not
       just a commit -- unlike sg_rev_parse_commit's own prefix handling in
       resolve_base, which only ever sits inside a commit-shaped grammar. A
       count of 0 (not found as any object) falls through to the general
       grammar below, same as any other name that is not a bare prefix.
       Routed through sg_rev_effective_disambig for the mode decision too
       (rather than hardcoding SG_REV_STRICT inline) purely so there is
       still only ONE place that decides a mode -- `arg` here is guaranteed
       pure hex by looks_like_oid_prefix, so it can contain neither a ':'
       nor a "~"/"^"/"@{", and the call is a no-op that always returns
       SG_REV_STRICT unchanged. */
    if (looks_like_oid_prefix(arg, strlen(arg))) {
        int prc = resolve_ambiguous_prefix(git_dir, arg, sg_rev_effective_disambig(arg, SG_REV_STRICT), id_out);

        if (prc == 0) {
            unsigned char *content;
            size_t content_len;

            if (sg_object_read(git_dir, id_out, type_out, &content, &content_len) != 0)
                return -1;
            free(content);
            return 0;
        }
        if (prc == -4)
            return -4;
    }

    {
        int prc = sg_rev_parse_commit(git_dir, arg, id_out);

        if (prc == 0) {
            *type_out = SG_OBJ_COMMIT;
            return 0;
        }
        if (prc == -4)
            return -4;
    }

    return -1;
}

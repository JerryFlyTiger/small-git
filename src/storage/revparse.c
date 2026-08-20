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

/* Resolves just the <base> part of the grammar (see revparse.h) to a raw
   object id, trying a literal 40-hex sha1, then HEAD, then tag, then
   branch, in that order -- the first that matches wins. This is real
   git's own gitrevisions disambiguation order (full SHA-1 object name,
   then refs/<name> -> refs/tags/<name> -> refs/heads/<name> -> ...);
   measured against real git for both halves of this order:

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

   Consistent with that: once base looks like a full 40-hex sha1 (right
   length, all hex digits), that IS the answer -- if the object doesn't
   exist, this returns failure without falling back to a same-named ref,
   matching `git rev-parse --verify` on a well-formed-but-absent sha1.

   Does not peel tags or apply ~/^ suffixes. Returns 0 on success. */
static int resolve_base(const char *git_dir, const char *base, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char ref_path[SG_PATH_MAX];

    if (strlen(base) == SG_SHA1_HEX_LEN)
        return sg_hex_to_sha1(base, id_out);

    if (sg_rev_parse_ref_path(git_dir, base, ref_path, sizeof(ref_path)) != 0)
        return -1;

    /* "HEAD" is a symref ("ref: refs/heads/<branch>\n"), not a raw oid file,
       so it needs sg_ref_resolve_head's indirection rather than
       sg_ref_read_path (which would try to hex-decode the "ref: ..." line
       and fail). Every other path sg_rev_parse_ref_path can return
       (refs/tags/<n>, refs/heads/<n>, or an already-"refs/..."  name) is an
       ordinary oid file. */
    if (strcmp(ref_path, "HEAD") == 0)
        return sg_ref_resolve_head(git_dir, id_out);

    return sg_ref_read_path(git_dir, ref_path, id_out);
}

int sg_rev_parse_ref_path(const char *git_dir, const char *name, char *out, size_t out_size)
{
    unsigned char tmp[SG_SHA1_RAW_LEN];
    char candidate[SG_PATH_MAX];

    if (strcmp(name, "HEAD") == 0) {
        if (out_size < 5)
            return -1;
        memcpy(out, "HEAD", 5);
        return 0;
    }

    /* Already a full ref path (e.g. "refs/heads/topic" passed straight
       through by a caller that already qualified it): used as-is, but only
       if it actually exists -- otherwise this would let a bogus
       "refs/nonsense/foo" through unchallenged. */
    if (strncmp(name, "refs/", 5) == 0) {
        if (sg_ref_read_path(git_dir, name, tmp) != 0)
            return -1;
        if (strlen(name) >= out_size)
            return -1;
        strcpy(out, name);
        return 0;
    }

    if (snprintf(candidate, sizeof(candidate), "refs/tags/%s", name) < (int)sizeof(candidate) &&
       sg_ref_read_path(git_dir, candidate, tmp) == 0) {
        if (strlen(candidate) >= out_size)
            return -1;
        strcpy(out, candidate);
        return 0;
    }

    if (sg_ref_read_branch(git_dir, name, tmp) == 0) {
        if (snprintf(candidate, sizeof(candidate), "refs/heads/%s", name) >= (int)sizeof(candidate) ||
           strlen(candidate) >= out_size)
            return -1;
        strcpy(out, candidate);
        return 0;
    }

    return -1;
}

int sg_rev_parse_commit(const char *git_dir, const char *rev,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    char base[SG_PATH_MAX];
    unsigned char id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    size_t base_len;
    size_t pos;

    if (rev == NULL || rev[0] == '\0')
        return -1;

    /* The base ends at '~', '^', or the start of an "@{N}" reflog suffix
       ("@" followed immediately by "{" -- an '@' anywhere else, e.g. inside
       an email-shaped ref name, stays part of the base). */
    base_len = 0;
    while (rev[base_len] != '\0' && rev[base_len] != '~' && rev[base_len] != '^' &&
          !(rev[base_len] == '@' && rev[base_len + 1] == '{'))
        base_len++;
    if (base_len == 0 || base_len >= sizeof(base))
        return -1;
    memcpy(base, rev, base_len);
    base[base_len] = '\0';

    pos = base_len;

    if (rev[pos] == '@') {
        /* rev[pos + 1] == '{', guaranteed by the scan loop above. "@{N}"
           must immediately follow the base -- "topic~1@{1}" stops the base
           scan at '~', so "@{1}" is left for the ~/^ suffix loop below,
           where it fails to parse as a number and is rejected. That is the
           whole enforcement of "@{N} must be adjacent to the ref name"; no
           separate check is needed here. */
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
        /* @{N} names the NEW oid of that log entry -- the value the ref was
           moved TO, not the value it had before (see sg_reflog_at's header
           comment; @{0}'s old_id is not "the previous commit", it can be
           all-zeros for a ref's very first entry). */
        entry = sg_reflog_at(&log, (size_t)idx);
        if (entry == NULL) {
            sg_reflog_free(&log);
            return -1;
        }
        memcpy(id, entry->new_id, SG_SHA1_RAW_LEN);
        sg_reflog_free(&log);
    } else {
        if (resolve_base(git_dir, base, id) != 0)
            return -1;
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

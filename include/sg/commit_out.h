#ifndef SG_COMMIT_OUT_H
#define SG_COMMIT_OUT_H

#include "sg/hash.h"
#include "sg/object.h"

/* git abbreviates to core.abbrev, whose default is `auto` and scales with the
   repository's object count. sg pins 7, which is what auto yields for a small
   repository; interop declares the same on git's side rather than pretending
   the two policies agree. */
#define SG_COMMIT_OUT_ABBREV 7

/* Phase 60: --pretty=<name>/format:<str>/tformat:<str> and --format=<...>.
   The five-grammar-rule ordering (case-insensitive builtin lookup,
   case-SENSITIVE "format:"/"tformat:" prefixes, "contains a %" -> tformat,
   otherwise an error) lives in sg_pretty_parse, CLAUDE.md's `sg log` entry
   has the full table. SG_PRETTY_FORMAT/_TFORMAT are recognized by the
   grammar as of Phase 60a; as of Phase 60b a user_format's placeholders
   (section 5.1's table: ids, author/committer name/email/local-part/six
   date renderings, %s/%f/%b/%B, %n/%%/%xNN) are fully expanded --
   sg_pretty_validate_format rejects anything outside that table up front,
   once per invocation, before any commit is ever rendered (a deliberate
   divergence from real git, which prints an unrecognized placeholder
   literally instead of refusing). */
typedef enum {
    SG_PRETTY_LEGACY = 0, /* opts->oneline decides oneline vs medium, unchanged */
    SG_PRETTY_ONELINE,    /* builtin `oneline`: full 40-hex, no dates */
    SG_PRETTY_SHORT,
    SG_PRETTY_MEDIUM,
    SG_PRETTY_FULL,
    SG_PRETTY_FULLER,
    SG_PRETTY_RAW,
    SG_PRETTY_REFERENCE,
    SG_PRETTY_FORMAT,  /* "format:<str>" -- no terminator, no separator beyond
                          the ordinary blank-line/`---` rule (see commit_out.c) */
    SG_PRETTY_TFORMAT  /* "tformat:<str>", or any string containing `%` given
                          bare (grammar rule 4) -- terminates with `\n` */
} sg_pretty_kind;

typedef struct {
    sg_pretty_kind kind;
    /* Borrowed pointer into the original argv string, past the "format:"/
       "tformat:" prefix (or the whole string for rule 4). NULL for every
       other kind. */
    const char *user_format;
} sg_pretty_format;

/* Parses the text after `--pretty=`/`--format=` (or the literal "medium"
   for a bare `--pretty` with no `=`, per CLAUDE.md's documented asymmetry
   with bare `--format`, which is rejected by the CLI layer before this is
   ever called). Implements the five ordered grammar rules; returns 0 with
   *out filled in, or -1 having printed nothing (the caller reports its own
   "invalid --pretty format" usage error, matching its own USAGE string). */
int sg_pretty_parse(const char *arg, sg_pretty_format *out);

/* Phase 60b: validates every `%`-sequence in a FORMAT/TFORMAT user_format
   against the placeholder table in CLAUDE.md's `sg log` Phase 60 entry
   (section 5.1 of the spec) -- ids, author/committer name/email/local-part/
   six date renderings, %s/%f/%b/%B, %n/%%/%xNN. This is a deliberate
   divergence from real git (section 5.3): git prints an unrecognized
   placeholder literally or renders a recognized-but-contextless one as
   empty, sg refuses the whole invocation instead. Returns 0 if every
   sequence is recognized, or -1 with *bad set to a borrowed pointer into
   fmt at the start of the offending sequence (including its leading `%`)
   and *bad_len to its length -- NOT NUL-terminated on its own, the caller
   must print it as "%.*s". A fmt with no `%` at all trivially returns 0.
   bad/bad_len may be NULL if the caller does not want to report it. */
int sg_pretty_validate_format(const char *fmt, const char **bad, size_t *bad_len);

typedef struct {
    int oneline; /* "<abbrev> <subject>" header instead of the full block --
                    this is the PRE-Phase-60 `--oneline` flag, independent of
                    `pretty` below (see CLAUDE.md's `sg log` --oneline note:
                    it abbreviates to 7 hex, `--pretty=oneline` does not). */
    int patch;   /* -p */
    int stat;    /* --stat */
    /* Phase 59: --name-only / --name-status. name_only and name_status are
       themselves mutually exclusive (cmd_show.c's argv loop rejects both
       given together, see CLAUDE.md's `sg show` bullet for the exact flag
       model), but patch/stat are NOT necessarily zero when one of these is
       set -- the renderer (commit_out.c's print_commit_diff, cmd_show.c's
       render_merge_diff) checks name_only/name_status FIRST and dispatches
       the name format regardless of patch/stat's value, so a caller may
       leave patch/stat at whatever the format flags themselves resolved to.
       Do NOT reintroduce a resolver-side zeroing of patch/stat "to be
       safe" -- that used to exist and was removed after a mutation showed
       it was a redundant guard the renderer already enforces one layer
       down. */
    int name_only;
    int name_status;
    /* Phase 60: NULL means legacy (oneline bool decides oneline vs medium,
       byte-identical to pre-Phase-60 behavior). Non-NULL selects one of the
       seven named formats or a user format (FORMAT/TFORMAT). Borrowed --
       the pointee's lifetime is the caller's, same convention as every
       other borrowed pointer in this project. Every construction site of
       this struct MUST set this field explicitly (memset or an explicit
       assignment) -- see CLAUDE.md's Phase 29 shared-struct warning, this
       is exactly the kind of field a partial field-by-field assignment
       leaves as stack garbage. */
    const sg_pretty_format *pretty;
} sg_commit_out_opts;

/* Prints one commit entry exactly as `git log` / `git show` render it:
   either the "<abbrev> <subject>" oneline header, or the full
   "commit <sha>" / (optional "Merge: ...") / "Author: ..." / "Date: ..."
   block followed by the indented message -- or, if opts->pretty is
   non-NULL, whichever of the seven builtins or the user format it selects
   -- and then, if opts->patch, opts->stat, opts->name_only or
   opts->name_status is set, that commit's own diff against its first
   parent (the empty tree for a root commit).

   `id` is the commit's own object id (already computed by the caller,
   which needs it anyway to walk to the next entry / print the header).
   Returns 0, or -1 having printed a partial entry (the caller decides how
   to report that -- see cmd_log.c's and cmd_show.c's own error messages).

   PRECONDITION: if opts->pretty selects SG_PRETTY_FORMAT/_TFORMAT, every
   `%`-sequence in its user_format must already be one sg_pretty_validate_
   format accepts. The CLI layer validates it up front, once per
   invocation, before ever calling this function (see cmd_log.c's/
   cmd_show.c's own resolve_pretty_arg) -- this function does not re-check,
   so a caller bypassing that gate gets an unrecognized `%`-sequence printed
   literally instead of rejected. */
int sg_commit_out_entry(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                        const sg_commit *commit, const sg_commit_out_opts *opts);

#endif

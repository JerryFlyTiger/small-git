#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/commit_out.h"
#include "sg/date.h"
#include "sg/hash.h"
#include "sg/log_graph.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/pathspec.h"
#include "sg/quote.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char USAGE[] =
    "usage: sg log [-n <count>|-<count>|--max-count=<count>] [--oneline] "
    "[--pretty[=<fmt>]|--format=<fmt>] [--date=<fmt>] [-p|--patch] [--stat] [--graph] [<rev>] [--] [<path>...]\n";

/* Phase 63: captures one entry's stdout output (sg_commit_out_entry AND
   whatever sg_diff_print underneath it prints for -p/--stat) into a shared
   tmpfile-backed fd, and returns the raw captured bytes -- it does NOT
   feed them through the prefixer itself. This is done by redirecting fd 1
   itself, not by threading a FILE * or prefix parameter into
   commit_out.c/diff_out.c -- see docs/DESIGN.md's Phase 63 section for
   why: capturing fd 1 also catches any future write that bypasses printf
   and writes the fd directly, where a threaded-parameter approach would
   silently miss it.

   The prefixing is deliberately NOT done here: per section 0.1a, whether
   an entry's continuation lines get "| " or "  " cannot be decided until
   it is known whether a FOLLOWING entry exists and, if not, whether the
   walk ended naturally -- see cmd_log.c's one-entry lookahead, which is
   why this function only hands back a buffer.

   *out_buf and *out_len are always filled (NULL/0 for an entry that produced
   no output at all, or on any failure). Returns 0 if sg_commit_out_entry
   itself succeeded, 1 if it reported failure (the returned bytes are
   whatever partial output it had already written before failing -- the
   caller prints its own "cannot render this commit's diff", matching the
   non-graph path's wording), or -1 if a mechanical step of the capture
   itself failed (this function has already printed its own diagnostic to
   stderr; the caller must not print a second, misleading message). fd 1
   is restored to saved_fd on every path where the restoring dup2 itself
   succeeds, including the entry_rc != 0 path -- this is NOT an
   unconditional guarantee, and the one exception is deliberate rather
   than an oversight: if THAT dup2 call itself fails (the "cannot restore
   stdout after --graph capture" branch below), fd 1 is left pointing at
   the tmpfile and there is no further recovery available -- the process
   is about to exit 1 regardless, and there is no third fd to fall back
   to. Never degrades silently to "return the bytes anyway without
   reporting the mechanical failure": every failure here is a hard error
   reported via the return value. */
static int graph_capture_raw(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                              sg_commit *commit, sg_commit_out_opts *o, int tmpfd, int saved_fd,
                              char **out_buf, size_t *out_len)
{
    int entry_rc;
    long len;
    char *content = NULL;

    *out_buf = NULL;
    *out_len = 0;

    fflush(stdout);
    /* dup2 makes fd 1 and tmpfd share one file description, offset
       included -- rewind(tmp) would not reset that shared offset, so the
       tmpfile must be truncated at the fd level before every entry. */
    if (lseek(tmpfd, 0, SEEK_SET) < 0 || ftruncate(tmpfd, 0) != 0) {
        fprintf(stderr, "sg: cannot reset temporary buffer for --graph\n");
        return -1;
    }
    if (dup2(tmpfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "sg: cannot redirect output for --graph\n");
        return -1;
    }

    entry_rc = sg_commit_out_entry(git_dir, id, commit, o);

    /* Flush and restore fd 1 BEFORE doing anything else, regardless of
       entry_rc -- an error message printed while fd 1 still points at the
       tmpfile would vanish. */
    fflush(stdout);
    if (dup2(saved_fd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "sg: cannot restore stdout after --graph capture\n");
        return -1;
    }

    len = lseek(tmpfd, 0, SEEK_END);
    if (len < 0 || lseek(tmpfd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "sg: cannot read captured output for --graph\n");
        return -1;
    }
    if (len > 0) {
        long total = 0;

        content = malloc((size_t)len);
        if (content == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            return -1;
        }
        while (total < len) {
            ssize_t n = read(tmpfd, content + total, (size_t)(len - total));

            if (n <= 0) {
                fprintf(stderr, "sg: cannot read captured output for --graph\n");
                free(content);
                return -1;
            }
            total += n;
        }
    }

    *out_buf = content;
    *out_len = (size_t)len;
    return entry_rc != 0 ? 1 : 0;
}

/* Phase 60: parses the value of --pretty/--format (raw is the text after
   `=`, or the literal "medium" for a bare --pretty). As of Phase 60b, a
   FORMAT/TFORMAT user_format's placeholders are validated against
   sg_pretty_validate_format's table up front -- section 5.3 of the Phase
   60 spec, a deliberate divergence from real git, which prints an
   unrecognized placeholder literally instead of refusing. Returns 0 with
   *out filled, or -1 having already printed a diagnostic and exit code
   decided by the caller. */
static int resolve_pretty_arg(const char *raw, sg_pretty_format *out)
{
    if (sg_pretty_parse(raw, out) != 0) {
        fprintf(stderr, "sg: invalid --pretty format: %s\n", raw);
        return -1;
    }
    if (out->kind == SG_PRETTY_FORMAT || out->kind == SG_PRETTY_TFORMAT) {
        const char *bad = NULL;
        size_t bad_len = 0;

        if (sg_pretty_validate_format(out->user_format, &bad, &bad_len) != 0) {
            fprintf(stderr, "sg: unsupported --pretty placeholder '%.*s'\n", (int)bad_len, bad);
            return -1;
        }
    }
    return 0;
}

/* Phase 64: parses the value of --date=/--date (CLAUDE.md's --date= entry
   has the full grammar). Mirrors resolve_pretty_arg's own shape -- prints
   its own diagnostic and returns -1 on an unknown/empty name, including
   the deliberately-unimplemented `relative`/`human`/`auto:` families. */
static int resolve_date_arg(const char *raw, sg_date_mode *out)
{
    if (sg_date_parse_mode(raw, out) != 0) {
        fprintf(stderr, "sg: unknown date format %s\n", raw);
        return -1;
    }
    return 0;
}

/* "-n <count>", "--max-count=<count>" and the bare "-<count>" all mean the
   same thing; 0 is legal and prints nothing (measured: git exits 0). */
static int parse_count(const char *s, long *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return -1;
    v = strtol(s, &end, 10);
    if (*end != '\0' || v < 0)
        return -1;
    *out = v;
    return 0;
}

int sg_cmd_log(int argc, char **argv)
{
    char *git_dir;
    char *repo_root = NULL;
    unsigned char id[SG_SHA1_RAW_LEN];
    const char *rev = NULL;
    sg_commit_out_opts o;
    sg_pretty_format pretty_storage;
    sg_date_mode date_mode_storage;
    sg_pathspec pathspec;
    long max_count = -1;
    int rc = 0;
    int graph = 0;
    FILE *graph_tmp = NULL;
    int graph_tmpfd = -1;
    int graph_saved_fd = -1;
    sg_log_graph_prefixer graph_pfx;
    /* Phase 63 section 0.1a/0.1b: exactly one entry is held back so that,
       once it is finally written, it is known whether a following entry
       exists (giving it "| " continuation lines) or the walk is ending --
       and if so, whether that ending was NATURAL (parent_count == 0 with
       no error: "  " continuation lines) or a cutoff (-n, or a mechanical
       capture failure: "| "). Bounded to exactly one entry's bytes, never
       the whole run's output. */
    char *graph_pending_buf = NULL;
    size_t graph_pending_len = 0;
    int graph_has_pending = 0;
    int graph_natural_end = 0;
    int printed_any = 0;
    int suppress_join;
    long shown = 0;
    int i;
    char **pos;
    int n_pos = 0;
    int dashdash = -1; /* index into pos[] where "--" split the line */
    int rev_count;

    memset(&pathspec, 0, sizeof(pathspec));

    o.oneline = 0;
    o.patch = 0;
    o.stat = 0;
    /* Phase 59 added name_only/name_status to this shared struct
       (sg_commit_out_opts) -- `sg log` does not implement either flag, but
       print_commit_diff now checks them FIRST, before patch/stat, so
       leaving these as uninitialized stack garbage would route every `sg
       log -p`/`--stat` call down the wrong branch whenever the garbage bit
       happened to be nonzero (CLAUDE.md's Phase 29 shared-struct warning:
       search every construction site that assigns fields one at a time
       rather than through a single factory). */
    o.name_only = 0;
    o.name_status = 0;
    /* Phase 60 added `pretty` to the same shared struct -- same warning
       applies, a NULL here means "legacy medium/oneline via o.oneline". */
    o.pretty = NULL;
    /* Phase 62 added `pathspec` -- see commit_out.h's own warning, this
       must be set on every construction site regardless of whether a
       pathspec was actually given (NULL below means "no path limiting",
       and is overwritten once the pathspec is actually built, further
       down, only when it is non-empty). */
    o.pathspec = NULL;
    /* Phase 64 added `date_mode` -- see commit_out.h's own warning, same
       Phase 29 shared-struct rule. NULL means every reach point keeps its
       pre-Phase-64 default; overwritten below only when --date= is given
       (last one wins, see resolve_date_mode's own caller). */
    o.date_mode = NULL;

    pos = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(*pos));
    if (pos == NULL) {
        fprintf(stderr, "sg: out of memory\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        /* Past "--" nothing is an option any more, same convention as
           cmd_diff.c/cmd_status.c/cmd_stash.c: `sg log -- --oneline` asks
           about a path literally named "--oneline" (measured 0.6: git
           treats it as a pathspec, matching nothing, exit 0). */
        if (dashdash >= 0) {
            pos[n_pos++] = argv[i];
            continue;
        }
        if (strcmp(a, "--") == 0) {
            dashdash = n_pos;
        } else if (strcmp(a, "--oneline") == 0) {
            o.oneline = 1;
        } else if (strcmp(a, "--graph") == 0) {
            graph = 1;
        } else if (strcmp(a, "--pretty") == 0) {
            /* Bare --pretty (no '=') is legal and means medium -- measured
               asymmetry with bare --format below, CLAUDE.md's `sg log`
               grammar section has the full table. */
            if (resolve_pretty_arg("medium", &pretty_storage) != 0) {
                free(pos);
                return 1;
            }
            o.pretty = &pretty_storage;
        } else if (strncmp(a, "--pretty=", 9) == 0) {
            if (resolve_pretty_arg(a + 9, &pretty_storage) != 0) {
                free(pos);
                return 1;
            }
            o.pretty = &pretty_storage;
        } else if (strncmp(a, "--format=", 9) == 0) {
            if (resolve_pretty_arg(a + 9, &pretty_storage) != 0) {
                free(pos);
                return 1;
            }
            o.pretty = &pretty_storage;
        } else if (strncmp(a, "--date=", 7) == 0) {
            if (resolve_date_arg(a + 7, &date_mode_storage) != 0) {
                free(pos);
                return 1;
            }
            o.date_mode = &date_mode_storage;
        } else if (strcmp(a, "--date") == 0) {
            /* Separate-argument form, "--date <value>" -- measured accepted
               by real git on both `log` and `show`. */
            if (i + 1 >= argc) {
                fprintf(stderr, "%s", USAGE);
                free(pos);
                return 1;
            }
            i++;
            if (resolve_date_arg(argv[i], &date_mode_storage) != 0) {
                free(pos);
                return 1;
            }
            o.date_mode = &date_mode_storage;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--patch") == 0) {
            o.patch = 1;
        } else if (strcmp(a, "--stat") == 0) {
            o.stat = 1;
        } else if (strcmp(a, "-n") == 0) {
            if (i + 1 >= argc || parse_count(argv[i + 1], &max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                free(pos);
                return 1;
            }
            i++;
        } else if (strncmp(a, "--max-count=", 12) == 0) {
            if (parse_count(a + 12, &max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                free(pos);
                return 1;
            }
        } else if (a[0] == '-' && a[1] >= '0' && a[1] <= '9') {
            if (parse_count(a + 1, &max_count) != 0) {
                fprintf(stderr, "%s", USAGE);
                free(pos);
                return 1;
            }
        } else if (a[0] == '-' && a[1] != '\0') {
            /* Everything not implemented lands here and is refused, never
               silently ignored -- --follow, --all, --reverse, --author=,
               --grep=, -c, --cc, --full-history, --simplify-merges among
               them (Phase 62 section 1's rejection list; CLAUDE.md's
               `sg log` entry names the reasons). --graph is implemented as
               of Phase 63, --date=<name> as of Phase 64 (a deterministic
               name only -- `relative`/`human`/`auto:` still land here and
               are refused, see resolve_date_arg / sg_date_parse_mode),
               see the dedicated parse branches above. */
            fprintf(stderr, "%s", USAGE);
            free(pos);
            return 1;
        } else {
            pos[n_pos++] = argv[i];
        }
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL) {
        free(pos);
        return 1;
    }
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(pos);
        free(git_dir);
        return 1;
    }

    /* An explicit "--" settles the split with no guessing at all, same
       shape as cmd_diff.c. */
    rev_count = dashdash >= 0 ? dashdash : sg_cli_split_revs_and_paths(git_dir, pos, n_pos, "log");
    if (rev_count < 0) {
        free(pos);
        free(repo_root);
        free(git_dir);
        return 1;
    }
    /* `sg log` only ever accepted a single <rev> (Phase 2 scope, unchanged
       by Phase 62) -- git accepts several and simplifies its history walk
       across all of them, which is out of scope here. */
    if (rev_count > 1) {
        fputs(USAGE, stderr);
        free(pos);
        free(repo_root);
        free(git_dir);
        return 1;
    }
    rev = rev_count > 0 ? pos[0] : NULL;

    for (i = rev_count; i < n_pos; i++) {
        sg_pathspec_error perr;

        if (sg_pathspec_add(&pathspec, repo_root, pos[i], &perr) != 0) {
            sg_cli_report_pathspec_error(perr, pos[i], repo_root);
            sg_pathspec_free(&pathspec);
            free(pos);
            free(repo_root);
            free(git_dir);
            return 1;
        }
    }
    free(pos);
    free(repo_root);
    if (pathspec.count > 0)
        o.pathspec = &pathspec;

    if (rev != NULL) {
        if (sg_rev_parse_commit(git_dir, rev, id) != 0) {
            fprintf(stderr, "sg: not a valid revision '%s'\n", rev);
            sg_pathspec_free(&pathspec);
            free(git_dir);
            return 1;
        }
    } else if (sg_ref_resolve_head(git_dir, id) != 0) {
        fprintf(stderr, "fatal: your current branch does not have any commits yet\n");
        sg_pathspec_free(&pathspec);
        free(git_dir);
        return 1;
    }

    /* Phase 63: --graph's capture buffer, one tmpfile shared by every
       entry (not one per entry) -- see graph_capture_raw's own comment
       and docs/DESIGN.md's Phase 63 section. Set up once here, torn down
       once at the shared cleanup below; every loop exit (break) reaches
       that cleanup, so there is exactly one teardown site. When --graph is
       off this whole block is skipped, so the non-graph path allocates
       nothing extra and costs nothing extra (same reasoning as Phase 62's
       ps == NULL shortcut). */
    if (graph) {
        graph_tmp = tmpfile();
        if (graph_tmp == NULL) {
            fprintf(stderr, "sg: cannot create temporary file for --graph\n");
            sg_pathspec_free(&pathspec);
            free(git_dir);
            return 1;
        }
        graph_tmpfd = fileno(graph_tmp);
        graph_saved_fd = dup(STDOUT_FILENO);
        if (graph_saved_fd < 0) {
            fprintf(stderr, "sg: cannot capture output for --graph\n");
            fclose(graph_tmp);
            sg_pathspec_free(&pathspec);
            free(git_dir);
            return 1;
        }
        sg_log_graph_init(&graph_pfx);
    }

    /* first-parent only: merge commits' other parents are not walked (Phase 2 scope) */
    for (;;) {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;
        int touches;
        int touch_rc;
        char bad_path[SG_PATH_MAX];

        if (max_count >= 0 && shown >= max_count)
            break;

        if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
            fprintf(stderr, "sg: corrupt commit object\n");
            rc = 1;
            break;
        }
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            fprintf(stderr, "sg: malformed commit object\n");
            free(content);
            rc = 1;
            break;
        }
        free(content);

        /* Phase 62: the SAME judgment print_commit_diff's own diff below
           will independently re-derive -- see commit_out.h's warning on
           why this is a deliberate double-flatten rather than a shared
           cache. Comparing against its first parent (empty tree for a
           root commit) is exactly what -- and only what -- a first-parent
           walk can mean by "did this commit change the pathspec": measured
           against `git log --first-parent -- <path>` (Phase 62 section
           0.1). o.pathspec == NULL answers 1 without reading a tree. */
        bad_path[0] = '\0';
        touch_rc = sg_commit_out_touches_pathspec(git_dir, &commit, o.pathspec, &touches, bad_path);
        if (touch_rc != 0) {
            /* -2 and -1 are different failures and must not share a message.
               Only -2 fills bad_path, so reporting a path on -1 would print
               an EMPTY one and blame a path for what is really a missing or
               corrupt object. The path itself goes through
               sg_quote_path_delimited for the same reason cmd_diff.c's
               report_bad_tree_path does: an entry name unsafe enough to
               land here is exactly the kind that carries a raw ESC byte. */
            if (touch_rc == -2)
                fprintf(stderr, "sg: path %s is invalid, refusing to expand this tree into file paths\n",
                       sg_quote_path_delimited(bad_path));
            else
                fprintf(stderr, "sg: cannot read this commit's tree\n");
            rc = 1;
            sg_commit_free(&commit);
            break;
        }

        if (touches) {
            /* shown++ counts PRINTED commits, not walked ones -- `-n 2 --
               a.txt` must stop after the second commit that actually
               touches a.txt, not the second commit visited (Phase 62
               section 0.6, measured). */
            shown++;

            /* One blank line BETWEEN entries and none after the last, so
               the separator belongs before every entry but the first --
               and, since Phase 62, "the first" means the first one
               actually PRINTED, not the first one the walk reached; a
               commit the pathspec filtered out must not leave a blank
               line behind. Measured: an entry whose message is empty
               still gets it, which is why this cannot be folded into the
               message block inside the shared renderer. Keeping it here
               also leaves the first line of `sg log` a bare `commit
               <sha>`, which interop already parses with `head -1`.
               --oneline gets no separator at all (measured).

               Phase 60: builtin `oneline` joins that exemption, and so
               does TFORMAT -- a tformat: entry already terminates itself
               with its own trailing '\n' (commit_out.c's SG_PRETTY_TFORMAT
               branch), so back-to-back entries need nothing extra
               (measured: "plain\nplain\n" for two entries, not a blank
               line between them). FORMAT is the opposite of TFORMAT here
               even though neither terminates itself the same way builtins
               do: measured, it DOES get this same "\n" printed before each
               entry but the first ("plain\nplain", joined by exactly one
               newline) -- see CLAUDE.md's `sg log` Phase 60 entry for the
               byte-level derivation of why the two land on different sides
               of this same boolean.

               REFERENCE joins the no-separator set too (Phase 60a oracle
               round 2, measured directly): `git log -N --pretty=reference`
               prints entries back to back with no blank line, even though
               the SAME entry's own diff separator (an ordinary commit's
               `-p`) still gets the usual blank line -- this is a
               between-ENTRIES-only rule, not a blanket "reference behaves
               like oneline" one, so it belongs only here and not in
               commit_out.c's print_commit_diff. */
            suppress_join = o.oneline ||
                           (o.pretty != NULL &&
                            (o.pretty->kind == SG_PRETTY_ONELINE || o.pretty->kind == SG_PRETTY_TFORMAT ||
                             o.pretty->kind == SG_PRETTY_REFERENCE));

            /* Phase 63 section 0.1b: under --graph, the PREVIOUS pending
               entry (if any) is flushed the moment it is known a following
               entry exists -- flushed with "| " continuation lines,
               because by definition something comes after it -- and the
               inter-entry separator "\n" (when not suppressed) is fed
               through the prefixer right after it, for the same reason
               (attributed to "after the previous entry, before this one,
               and a following entry is now certain"). The CURRENT entry's
               own bytes are only captured here, never flushed yet -- its
               continuation prefix cannot be decided until it is known
               whether IT has a follower and, if not, whether the walk
               ended naturally. See graph_capture_raw's and
               sg_log_graph_write's own header comments. The non-graph path
               below is untouched, byte for byte, from before this phase. */
            if (graph) {
                char *buf = NULL;
                size_t buflen = 0;
                int cap_rc;

                if (graph_has_pending) {
                    /* sg_log_graph_write_entry -- not a separate
                       begin_entry()+write() pair -- is the ONLY place
                       that combines the two, specifically so this cannot
                       drift out of sync with the len == 0 fix documented
                       on that function: an entry whose captured bytes are
                       empty (e.g. --pretty=format:%b on a body-less
                       commit) still needs its own "* " marker emitted,
                       which the byte-loop half of the old two-call
                       pattern could not do on its own. */
                    sg_log_graph_write_entry(&graph_pfx, graph_pending_buf, graph_pending_len, "| ",
                                              stdout);
                    free(graph_pending_buf);
                    graph_pending_buf = NULL;
                    graph_has_pending = 0;
                    if (!suppress_join)
                        sg_log_graph_write(&graph_pfx, "\n", 1, "| ", stdout);
                }
                /* NOTE: no `printed_any = 1;` here -- unlike the non-graph
                   branch below, graph mode never reads `printed_any`
                   (whether a separator is due is tracked by
                   graph_has_pending instead), so setting it here would be
                   a dead write. Confirmed by grep: printed_any's only read
                   in this function is inside the non-graph `else` branch. */

                cap_rc = graph_capture_raw(git_dir, id, &commit, &o, graph_tmpfd, graph_saved_fd, &buf,
                                            &buflen);
                if (cap_rc < 0) {
                    /* graph_capture_raw already printed its own
                       diagnostic; printing "cannot render this commit's
                       diff" too would misattribute a capture-mechanism
                       failure to the renderer. Nothing became pending for
                       this commit, so the shared cleanup below still only
                       has to flush whatever was ALREADY pending before
                       this iteration -- but that was already flushed
                       above (a following entry -- this one's capture
                       attempt -- is exactly what was just proven to
                       exist), so there is nothing left to flush at all. */
                    rc = 1;
                    sg_commit_free(&commit);
                    break;
                }
                graph_pending_buf = buf;
                graph_pending_len = buflen;
                graph_has_pending = 1;
                if (cap_rc > 0) {
                    fprintf(stderr, "sg: cannot render this commit's diff\n");
                    rc = 1;
                    sg_commit_free(&commit);
                    break;
                }
            } else {
                if (printed_any && !suppress_join)
                    printf("\n");
                printed_any = 1;

                if (sg_commit_out_entry(git_dir, id, &commit, &o) != 0) {
                    fprintf(stderr, "sg: cannot render this commit's diff\n");
                    rc = 1;
                    sg_commit_free(&commit);
                    break;
                }
            }
        }

        if (commit.parent_count == 0) {
            /* The walk ended NATURALLY (reached the true root, no -n
               cutoff, no error) -- section 0.1a's decisive fixture is a
               pathspec that filters out everything between the last
               MATCHING commit and here: that commit still gets flagged
               natural, even though it is not itself the root and even
               though it may not be the commit currently pending (it may
               have been flushed several non-matching commits ago is
               impossible -- it IS whatever is still pending, since
               nothing after it matched and became a new pending entry).
               A `-n` cutoff instead breaks at the TOP of this loop
               (`shown >= max_count`), never reaching this branch, so
               graph_natural_end correctly stays 0 for that case. */
            if (graph)
                graph_natural_end = 1;
            sg_commit_free(&commit);
            break;
        }
        memcpy(id, commit.parents[0], SG_SHA1_RAW_LEN);
        sg_commit_free(&commit);
    }

    /* Every break above reaches this single teardown site -- fd 1 itself
       is never left redirected across a break (graph_capture_raw always
       restores it before returning), so besides flushing the one entry
       still held back, this only ever needs to close the saved fd and the
       tmpfile. graph_natural_end being 1 implies rc == 0 (every error path
       above sets rc and breaks before ever reaching the parent_count == 0
       check), so testing it alone is sufficient -- but see its own comment
       for why a `-n` cutoff or a mechanical capture failure both leave it
       0, giving "| " as section 0.1a's rule requires. */
    if (graph) {
        if (graph_has_pending) {
            sg_log_graph_write_entry(&graph_pfx, graph_pending_buf, graph_pending_len,
                                      graph_natural_end ? "  " : "| ", stdout);
            free(graph_pending_buf);
        }
        if (graph_saved_fd >= 0)
            close(graph_saved_fd);
        if (graph_tmp != NULL)
            fclose(graph_tmp);
    }

    sg_pathspec_free(&pathspec);
    free(git_dir);
    return rc;
}

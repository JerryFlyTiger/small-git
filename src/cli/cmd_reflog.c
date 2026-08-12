#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/reflog.h"
#include "sg/repo.h"
#include "sg/revparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_PATH_MAX 4096

/* Supports only the read-only subset real git's `git reflog` offers most
   often (Phase 17 scope): `sg reflog`/`sg reflog show` with an optional
   <ref> (defaulting to HEAD) and an optional `-n <count>` cap, in either
   order. No expire/delete/--date/--format/--all. */
int sg_cmd_reflog(int argc, char **argv)
{
    static const char usage[] = "usage: sg reflog [show] [<ref>] [-n <count>]\n";
    const char *ref_arg = NULL;
    long limit = -1; /* -1 == unlimited */
    int i0 = 1;
    int i;
    char *git_dir;
    char ref_path[SG_PATH_MAX];
    sg_reflog log;
    size_t shown;

    if (argc >= 2 && strcmp(argv[1], "show") == 0)
        i0 = 2;

    for (i = i0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            char *end;
            long v;

            if (i + 1 >= argc) {
                fputs(usage, stderr);
                return 1;
            }
            v = strtol(argv[i + 1], &end, 10);
            if (*end != '\0' || v < 0) {
                fputs(usage, stderr);
                return 1;
            }
            limit = v;
            i++;
        } else if (ref_arg == NULL) {
            ref_arg = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }
    if (ref_arg == NULL)
        ref_arg = "HEAD";

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    /* Ref doesn't exist at all -> error (exit 1). Ref exists but has never
       been logged -> sg_reflog_read below succeeds with count 0, printing
       nothing and exiting 0 (measured against real git 2.55.0: `git reflog
       show <ref-with-no-log>` prints nothing and exits 0). */
    if (sg_rev_parse_ref_path(git_dir, ref_arg, ref_path, sizeof(ref_path)) != 0) {
        fprintf(stderr, "sg: %s: no such ref\n", ref_arg);
        free(git_dir);
        return 1;
    }

    if (sg_reflog_read(git_dir, ref_path, &log) != 0) {
        fprintf(stderr, "sg: failed to read reflog for '%s'（檔案損壞？）\n", ref_arg);
        free(git_dir);
        return 1;
    }

    /* @{N} notation: N=0 is the newest entry, counting up as entries get
       older -- entries[] itself is oldest-first, so sg_reflog_at flips the
       index (see its header comment). The name is echoed back exactly as
       the user typed it, not normalized to the ref path used to find the
       file (measured: `git reflog show heads/topic` prints
       "heads/topic@{0}", not "refs/heads/topic@{0}"). abbrev is a fixed 7
       hex digits -- real git's core.abbrev auto-lengthens for large repos,
       sg does not. */
    for (shown = 0; shown < log.count; shown++) {
        const sg_reflog_entry *entry = sg_reflog_at(&log, shown);
        char hex[SG_SHA1_HEX_LEN + 1];

        if (limit >= 0 && (long)shown >= limit)
            break;

        sg_sha1_to_hex(entry->new_id, hex);
        printf("%.7s %s@{%zu}: %s\n", hex, ref_arg, shown, entry->message);
    }

    sg_reflog_free(&log);
    free(git_dir);
    return 0;
}

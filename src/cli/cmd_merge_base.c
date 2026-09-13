#include "sg/cli.h"

#include "sg/cli_args.h"
#include "sg/hash.h"
#include "sg/merge.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>

int sg_cmd_merge_base(int argc, char **argv)
{
    char *git_dir;
    unsigned char a[SG_SHA1_RAW_LEN];
    unsigned char b[SG_SHA1_RAW_LEN];
    unsigned char out[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: sg merge-base <commit-a> <commit-b>\n");
        return 1;
    }
    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    {
        int prc = sg_rev_parse_commit(git_dir, argv[1], a);

        if (prc != 0) {
            char bad_path[SG_PATH_MAX];

            if (prc == -4) {
                sg_cli_report_ambiguous_oid(git_dir, argv[1], SG_REV_STRICT);
                sg_cli_report_rev_error("merge-base", SG_REV_ERR_NOT_A_REV, argv[1], NULL, 0);
            } else {
                sg_cli_report_rev_error("merge-base",
                                        sg_cli_classify_rev_error(git_dir, argv[1], bad_path, sizeof(bad_path)),
                                        argv[1], bad_path, 0);
            }
            free(git_dir);
            return 1;
        }
    }
    {
        int prc = sg_rev_parse_commit(git_dir, argv[2], b);

        if (prc != 0) {
            char bad_path[SG_PATH_MAX];

            if (prc == -4) {
                sg_cli_report_ambiguous_oid(git_dir, argv[2], SG_REV_STRICT);
                sg_cli_report_rev_error("merge-base", SG_REV_ERR_NOT_A_REV, argv[2], NULL, 0);
            } else {
                sg_cli_report_rev_error("merge-base",
                                        sg_cli_classify_rev_error(git_dir, argv[2], bad_path, sizeof(bad_path)),
                                        argv[2], bad_path, 0);
            }
            free(git_dir);
            return 1;
        }
    }

    rc = sg_merge_base(git_dir, a, b, out);
    free(git_dir);

    if (rc == -1) {
        fprintf(stderr, "sg: no common ancestor found\n");
        return 1;
    }
    if (rc == -2) {
        fprintf(stderr, "sg: multiple independent merge bases found (criss-cross history)\n");
        return 1;
    }

    sg_sha1_to_hex(out, hex);
    printf("%s\n", hex);
    return 0;
}

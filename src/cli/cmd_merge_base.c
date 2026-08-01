#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/merge.h"
#include "sg/repo.h"

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
    if (sg_hex_to_sha1(argv[1], a) != 0) {
        fprintf(stderr, "sg: '%s' is not a valid object id\n", argv[1]);
        return 1;
    }
    if (sg_hex_to_sha1(argv[2], b) != 0) {
        fprintf(stderr, "sg: '%s' is not a valid object id\n", argv[2]);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

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

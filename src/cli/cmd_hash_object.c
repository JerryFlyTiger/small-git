#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/repo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_all(FILE *f, unsigned char **out, size_t *out_len)
{
    size_t cap = 65536;
    unsigned char *buf = malloc(cap);
    size_t used = 0;

    if (buf == NULL)
        return -1;

    for (;;) {
        size_t n;

        if (used == cap) {
            size_t new_cap = cap * 2;
            unsigned char *grown = realloc(buf, new_cap);

            if (grown == NULL) {
                free(buf);
                return -1;
            }
            buf = grown;
            cap = new_cap;
        }

        n = fread(buf + used, 1, cap - used, f);
        used += n;
        if (n == 0)
            break;
    }

    if (ferror(f)) {
        free(buf);
        return -1;
    }

    *out = buf;
    *out_len = used;
    return 0;
}

int sg_cmd_hash_object(int argc, char **argv)
{
    static const char usage[] = "usage: sg hash-object [-w] [-t <type>] [--stdin] <file>\n";
    int write_flag = 0;
    int use_stdin = 0;
    sg_obj_type type = SG_OBJ_BLOB;
    const char *file = NULL;
    unsigned char *content;
    size_t content_len;
    unsigned char id[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) {
            write_flag = 1;
        } else if (strcmp(argv[i], "--stdin") == 0) {
            use_stdin = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "sg: -t requires an argument\n");
                return 1;
            }
            i++;
            if (sg_obj_type_from_name(argv[i], &type) != 0) {
                fprintf(stderr, "sg: invalid object type '%s'\n", argv[i]);
                return 1;
            }
        } else if (file == NULL) {
            file = argv[i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }

    if (!use_stdin && file == NULL) {
        fputs(usage, stderr);
        return 1;
    }

    if (use_stdin) {
        if (read_all(stdin, &content, &content_len) != 0) {
            fprintf(stderr, "sg: failed to read stdin\n");
            return 1;
        }
    } else {
        FILE *f = fopen(file, "rb");

        if (f == NULL) {
            fprintf(stderr, "sg: cannot open %s: %s\n", sg_quote_path_delimited(file), strerror(errno));
            return 1;
        }
        if (read_all(f, &content, &content_len) != 0) {
            fprintf(stderr, "sg: failed to read %s\n", sg_quote_path_delimited(file));
            fclose(f);
            return 1;
        }
        fclose(f);
    }

    if (write_flag) {
        char *git_dir = sg_require_git_dir();

        if (git_dir == NULL) {
            free(content);
            return 1;
        }
        if (sg_loose_write(git_dir, type, content, content_len, id) != 0) {
            fprintf(stderr, "sg: failed to write object\n");
            free(git_dir);
            free(content);
            return 1;
        }
        free(git_dir);
    } else {
        sg_object_hash(type, content, content_len, id);
    }

    free(content);
    sg_sha1_to_hex(id, hex);
    printf("%s\n", hex);
    return 0;
}

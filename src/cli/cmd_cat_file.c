#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tree entry modes are git's own fixed vocabulary, not arbitrary unix modes:
   040000 (tree), 160000 (commit/gitlink submodule), and blob variants
   (100644/100755/120000) -- 160000's bit pattern happens to look like
   S_IFDIR|S_IFLNK, so POSIX S_ISxxx macros can't be used to classify it */
static const char *entry_type_name(unsigned int mode)
{
    if (mode == 040000)
        return "tree";
    if (mode == 0160000)
        return "commit";
    return "blob";
}

static int print_tree(const unsigned char *content, size_t content_len)
{
    sg_tree tree;
    size_t i;

    if (sg_tree_parse(content, content_len, &tree) != 0) {
        fprintf(stderr, "sg: malformed tree object\n");
        return -1;
    }

    for (i = 0; i < tree.count; i++) {
        const sg_tree_entry *e = &tree.entries[i];
        char hex[SG_SHA1_HEX_LEN + 1];
        const char *type_name = entry_type_name(e->mode);

        sg_sha1_to_hex(e->sha1, hex);
        /* mode is zero-padded to 6 digits for display only; on-disk it never
           has a leading zero */
        printf("%06o %s %s\t%s\n", e->mode, type_name, hex, sg_quote_path(e->name));
    }

    sg_tree_free(&tree);
    return 0;
}

int sg_cmd_cat_file(int argc, char **argv)
{
    static const char usage[] = "usage: sg cat-file (-t | -s | -p) <object>\n";
    const char *mode;
    const char *hex_arg;
    unsigned char id[SG_SHA1_RAW_LEN];
    char *git_dir;
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    int rc = 0;

    if (argc != 3) {
        fputs(usage, stderr);
        return 1;
    }
    mode = argv[1];
    hex_arg = argv[2];

    if (strcmp(mode, "-t") != 0 && strcmp(mode, "-s") != 0 && strcmp(mode, "-p") != 0) {
        fputs(usage, stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    {
        char bad_path[SG_PATH_MAX];
        int resolve_rc;

        bad_path[0] = '\0';
        resolve_rc = sg_rev_parse_object(git_dir, hex_arg, id, &type, bad_path, sizeof(bad_path));
        /* A well-formed id whose object cannot be read is missing or
           corrupt, NOT an invalid name -- interop pins the wording,
           because naming the wrong problem sends the reader to the
           wrong place (a packed REF_DELTA with a vanished base). */
        if (resolve_rc == -3) {
            fprintf(stderr, "sg: object '%s' not found or corrupt\n", hex_arg);
            free(git_dir);
            return 1;
        }
        if (resolve_rc == -2) {
            const char *colon = strchr(hex_arg, ':');
            char rev[SG_PATH_MAX];
            size_t rev_len = colon != NULL ? (size_t)(colon - hex_arg) : 0;

            if (rev_len >= sizeof(rev))
                rev_len = sizeof(rev) - 1;
            memcpy(rev, hex_arg, rev_len);
            rev[rev_len] = '\0';
            fprintf(stderr, "sg: path '%s' does not exist in '%s'\n", bad_path, rev);
            free(git_dir);
            return 1;
        }
        if (resolve_rc != 0) {
            fprintf(stderr, "sg: not a valid object name '%s'\n", hex_arg);
            free(git_dir);
            return 1;
        }
    }

    /* sg_rev_parse_object only resolves the id/type, it does not hand back
       the object's decompressed bytes -- read those separately, the same
       object it just proved exists. */
    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0) {
        fprintf(stderr, "sg: object '%s' not found or corrupt\n", hex_arg);
        free(git_dir);
        return 1;
    }
    free(git_dir);

    /* Plumbing: -p/-t/-s always operate on the object's own stored bytes
       (the pointer text itself, never reassembled) -- but if it happens to
       be a chunked-storage pointer, a stderr note helps explain why the
       content/size looks the way it does, without touching stdout's
       object-content output. */
    if (type == SG_OBJ_BLOB) {
        sg_chunk_pointer ptr;

        if (sg_chunk_pointer_parse(content, content_len, &ptr)) {
            fprintf(stderr,
                   "sg: this is a chunked-storage pointer (original size: %zu bytes, %zu chunks)\n",
                   ptr.original_size, ptr.chunk_count);
            sg_chunk_pointer_free(&ptr);
        }
    }

    if (strcmp(mode, "-t") == 0) {
        printf("%s\n", sg_obj_type_name(type));
    } else if (strcmp(mode, "-s") == 0) {
        printf("%zu\n", content_len);
    } else if (type == SG_OBJ_TREE) {
        rc = print_tree(content, content_len);
    } else {
        fwrite(content, 1, content_len, stdout);
    }

    free(content);
    return rc == 0 ? 0 : 1;
}

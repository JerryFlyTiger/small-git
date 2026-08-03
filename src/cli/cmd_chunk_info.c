#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/index.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char (*ids)[SG_SHA1_RAW_LEN]; /* malloc'd */
    size_t count;
    size_t cap;
} id_list;

static void id_list_push(id_list *l, const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (l->count == l->cap) {
        size_t new_cap = l->cap == 0 ? 64 : l->cap * 2;
        void *grown = realloc(l->ids, new_cap * sizeof(*l->ids));

        if (grown == NULL)
            return; /* best effort: shared-chunk stats may end up incomplete */
        l->ids = grown;
        l->cap = new_cap;
    }
    memcpy(l->ids[l->count], id, SG_SHA1_RAW_LEN);
    l->count++;
}

static int id_cmp(const void *a, const void *b)
{
    return memcmp(a, b, SG_SHA1_RAW_LEN);
}

/* Best-effort, diagnostic-only scan of every loose object under
   git_dir/objects: any blob whose content merely *parses* as a well-formed
   chunk pointer (format only -- no hash/reassembly verification, this is
   just a rough usage tally, not a correctness check like sg_chunk_read_blob)
   contributes each of its declared chunk ids to `out`. Used only to report
   how many of *this* file's chunks are also referenced by some other chunked
   blob; a stray non-pointer or unrelated object never contributes anything. */
static void collect_chunk_refs(const char *git_dir, id_list *out)
{
    char objects_path[4096];
    DIR *d;
    struct dirent *entry;

    snprintf(objects_path, sizeof(objects_path), "%s/objects", git_dir);
    d = opendir(objects_path);
    if (d == NULL)
        return;

    while ((entry = readdir(d)) != NULL) {
        char subdir_path[4096];
        DIR *sd;
        struct dirent *file_entry;

        if (strlen(entry->d_name) != 2 || !isxdigit((unsigned char)entry->d_name[0]) ||
           !isxdigit((unsigned char)entry->d_name[1]))
            continue; /* skip "pack", "info", ".", ".." etc. */

        snprintf(subdir_path, sizeof(subdir_path), "%s/%s", objects_path, entry->d_name);
        sd = opendir(subdir_path);
        if (sd == NULL)
            continue;

        while ((file_entry = readdir(sd)) != NULL) {
            char hex[SG_SHA1_HEX_LEN + 1];
            unsigned char id[SG_SHA1_RAW_LEN];
            sg_obj_type type;
            unsigned char *content;
            size_t content_len;

            if (strlen(file_entry->d_name) != SG_SHA1_HEX_LEN - 2)
                continue;
            snprintf(hex, sizeof(hex), "%s%s", entry->d_name, file_entry->d_name);
            if (sg_hex_to_sha1(hex, id) != 0)
                continue;

            if (sg_object_read(git_dir, id, &type, &content, &content_len) == 0) {
                sg_chunk_pointer ptr;

                if (type == SG_OBJ_BLOB && sg_chunk_pointer_parse(content, content_len, &ptr)) {
                    size_t i;

                    for (i = 0; i < ptr.chunk_count; i++)
                        id_list_push(out, ptr.chunk_ids[i]);
                    sg_chunk_pointer_free(&ptr);
                }
                free(content);
            }
        }
        closedir(sd);
    }
    closedir(d);
}

static size_t count_occurrences(const id_list *sorted, const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t lo = 0;
    size_t hi = sorted->count;
    size_t n = 0;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (memcmp(sorted->ids[mid], id, SG_SHA1_RAW_LEN) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    while (lo < sorted->count && memcmp(sorted->ids[lo], id, SG_SHA1_RAW_LEN) == 0) {
        n++;
        lo++;
    }
    return n;
}

static void print_shared_chunks(const char *git_dir, const sg_chunk_pointer *ptr)
{
    id_list all;
    size_t shared = 0;
    size_t i;

    memset(&all, 0, sizeof(all));
    collect_chunk_refs(git_dir, &all);
    if (all.count > 0)
        qsort(all.ids, all.count, sizeof(*all.ids), id_cmp);

    for (i = 0; i < ptr->chunk_count; i++) {
        if (count_occurrences(&all, ptr->chunk_ids[i]) > 1)
            shared++;
    }

    printf("shared chunks: %zu/%zu（也被其他分塊物件引用）\n", shared, ptr->chunk_count);
    free(all.ids);
}

/* Looks up arg as a repo-relative working-directory path and resolves it to
   the blob id currently recorded for it in the index (stage 0) -- not a
   fresh hash of the working-directory file, since the point of this command
   is to inspect what's actually stored in the repo (a chunk pointer or an
   ordinary blob), which only the index entry reflects. */
static int resolve_blob_id(const char *git_dir, const char *arg, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char *repo_root;
    sg_index idx;
    char *rel;
    int pos;
    int rc = -1;

    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        return -1;
    }
    rel = sg_resolve_repo_path(repo_root, arg);
    if (rel == NULL) {
        fprintf(stderr, "sg: '%s' is outside the repository\n", arg);
        free(repo_root);
        return -1;
    }
    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(rel);
        free(repo_root);
        return -1;
    }

    pos = sg_index_find(&idx, rel);
    if (pos < 0) {
        fprintf(stderr, "sg: '%s' is not tracked in the index\n", arg);
    } else {
        memcpy(id_out, idx.entries[pos].sha1, SG_SHA1_RAW_LEN);
        rc = 0;
    }

    sg_index_free(&idx);
    free(rel);
    free(repo_root);
    return rc;
}

int sg_cmd_chunk_info(int argc, char **argv)
{
    static const char usage[] = "usage: sg chunk-info <path-or-blob-hex>\n";
    const char *arg;
    unsigned char id[SG_SHA1_RAW_LEN];
    char *git_dir;
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    unsigned char effective_id[SG_SHA1_RAW_LEN];

    if (argc != 2) {
        fputs(usage, stderr);
        return 1;
    }
    arg = argv[1];

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (strlen(arg) != SG_SHA1_HEX_LEN || sg_hex_to_sha1(arg, id) != 0) {
        /* not a bare blob id: treat as a working-directory path instead */
        if (resolve_blob_id(git_dir, arg, id) != 0) {
            free(git_dir);
            return 1;
        }
    }

    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0) {
        fprintf(stderr, "sg: object '%s' not found or corrupt\n", arg);
        free(git_dir);
        return 1;
    }

    if (type != SG_OBJ_BLOB) {
        fprintf(stderr, "sg: '%s' is not a blob (type: %s)\n", arg, sg_obj_type_name(type));
        free(content);
        free(git_dir);
        return 1;
    }

    /* sg_chunk_effective_id only returns an id different from the one passed
       in when the blob's content is a *hash-verified* chunk pointer -- format
       alone (sg_chunk_pointer_parse succeeding) is not enough, which is
       exactly the distinction this command needs to make. */
    if (sg_chunk_effective_id(git_dir, id, effective_id) != 0) {
        fprintf(stderr, "sg: failed to read object '%s'\n", arg);
        free(content);
        free(git_dir);
        return 1;
    }

    if (memcmp(id, effective_id, SG_SHA1_RAW_LEN) != 0) {
        sg_chunk_pointer ptr;

        if (sg_chunk_pointer_parse(content, content_len, &ptr)) {
            printf("chunked: yes\n");
            printf("original size: %zu bytes\n", ptr.original_size);
            printf("chunk count: %zu\n", ptr.chunk_count);
            print_shared_chunks(git_dir, &ptr);
            sg_chunk_pointer_free(&ptr);
        } else {
            /* Can't happen in practice: sg_chunk_effective_id already parsed
               this exact content successfully to compute effective_id. Still
               handled defensively rather than assumed. */
            printf("chunked: yes\n");
            printf("original size: %zu bytes\n", content_len);
        }
    } else {
        printf("chunked: no\n");
        printf("size: %zu bytes\n", content_len);
    }

    free(content);
    free(git_dir);
    return 0;
}

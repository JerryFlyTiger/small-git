#include "sg/tree_build.h"

#include "sg/chunk.h"
#include "sg/loose.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SG_TREE_DIR_MODE 040000
#define SG_TREE_BUILD_PATH_MAX 4096

static int build_level(const char *git_dir, const sg_flat_entry *entries, size_t count,
                       unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_tree_entry *level = NULL;
    size_t level_count = 0;
    size_t level_cap = 0;
    size_t i = 0;
    unsigned char *serialized;
    size_t serialized_len;
    int rc = -1;

    while (i < count) {
        const char *path = entries[i].path;
        const char *slash = strchr(path, '/');
        sg_tree_entry *slot;

        if (level_count == level_cap) {
            size_t new_cap = level_cap == 0 ? 8 : level_cap * 2;
            sg_tree_entry *grown = realloc(level, new_cap * sizeof(*grown));

            if (grown == NULL)
                goto done;
            level = grown;
            level_cap = new_cap;
        }
        slot = &level[level_count];

        if (slash == NULL) {
            slot->name = strdup(path);
            if (slot->name == NULL)
                goto done;
            slot->mode = entries[i].mode;
            memcpy(slot->sha1, entries[i].sha1, SG_SHA1_RAW_LEN);
            level_count++;
            i++;
        } else {
            size_t comp_len = (size_t)(slash - path);
            size_t j = i + 1;
            sg_flat_entry *sub;
            size_t sub_count;
            size_t k;

            while (j < count) {
                const char *p2 = entries[j].path;

                if (strncmp(p2, path, comp_len) != 0 || p2[comp_len] != '/')
                    break;
                j++;
            }
            sub_count = j - i;
            sub = malloc(sub_count * sizeof(*sub));
            if (sub == NULL)
                goto done;
            for (k = 0; k < sub_count; k++) {
                sub[k].path = entries[i + k].path + comp_len + 1; /* not owned, transient */
                sub[k].mode = entries[i + k].mode;
                memcpy(sub[k].sha1, entries[i + k].sha1, SG_SHA1_RAW_LEN);
            }

            slot->name = malloc(comp_len + 1);
            if (slot->name == NULL) {
                free(sub);
                goto done;
            }
            memcpy(slot->name, path, comp_len);
            slot->name[comp_len] = '\0';
            slot->mode = SG_TREE_DIR_MODE;

            if (build_level(git_dir, sub, sub_count, slot->sha1) != 0) {
                free(sub);
                free(slot->name);
                goto done;
            }
            free(sub);

            level_count++;
            i = j;
        }
    }

    /* Because entries arrive sorted by full path, a name collision between a
       leaf ("foo") and a directory group ("foo/bar") always lands on
       adjacent slots here -- catch it before it turns into a tree object
       with two entries sharing the same name, which git itself rejects. */
    for (i = 1; i < level_count; i++) {
        if (strcmp(level[i - 1].name, level[i].name) == 0)
            goto done;
    }

    if (sg_tree_serialize(level, level_count, &serialized, &serialized_len) != 0)
        goto done;
    rc = sg_loose_write(git_dir, SG_OBJ_TREE, serialized, serialized_len, tree_id_out);
    free(serialized);

done:
    {
        size_t n;

        for (n = 0; n < level_count; n++)
            free(level[n].name);
        free(level);
    }
    return rc;
}

int sg_tree_build(const char *git_dir, const sg_flat_entry *entries, size_t count,
                  unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    return build_level(git_dir, entries, count, tree_id_out);
}

static int flatten_append(sg_flat_list *out, size_t *cap, const char *path, unsigned int mode,
                          const unsigned char sha1[SG_SHA1_RAW_LEN])
{
    if (out->count == *cap) {
        size_t new_cap = *cap == 0 ? 8 : *cap * 2;
        sg_flat_entry *grown = realloc(out->entries, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        out->entries = grown;
        *cap = new_cap;
    }
    out->entries[out->count].path = strdup(path);
    if (out->entries[out->count].path == NULL)
        return -1;
    out->entries[out->count].mode = mode;
    memcpy(out->entries[out->count].sha1, sha1, SG_SHA1_RAW_LEN);
    out->count++;
    return 0;
}

/* A tree object's entry names come straight from object content, which may
   originate from a crafted/foreign commit (not just sg's own tree builder).
   Without this check, an entry named e.g. "../../../tmp/evil" would let
   `sg switch`/`sg restore` write outside the repository via the full_path
   built below. */
static int entry_name_is_safe(const char *name)
{
    if (name[0] == '\0')
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    if (strchr(name, '/') != NULL)
        return 0;
    return 1;
}

static int flatten_into(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN],
                        const char *prefix, sg_flat_list *out, size_t *cap)
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_tree tree;
    size_t i;
    int rc = 0;

    if (sg_object_read(git_dir, tree_id, &type, &content, &content_len) != 0 || type != SG_OBJ_TREE)
        return -1;

    if (sg_tree_parse(content, content_len, &tree) != 0) {
        free(content);
        return -1;
    }
    free(content);

    for (i = 0; i < tree.count && rc == 0; i++) {
        const sg_tree_entry *e = &tree.entries[i];
        char *full_path;
        size_t prefix_len = strlen(prefix);
        size_t name_len = strlen(e->name);

        if (!entry_name_is_safe(e->name)) {
            rc = -1;
            break;
        }

        full_path = malloc(prefix_len + name_len + 2);
        if (full_path == NULL) {
            rc = -1;
            break;
        }
        if (prefix_len > 0) {
            memcpy(full_path, prefix, prefix_len);
            full_path[prefix_len] = '/';
            memcpy(full_path + prefix_len + 1, e->name, name_len + 1);
        } else {
            memcpy(full_path, e->name, name_len + 1);
        }

        if (e->mode == SG_TREE_DIR_MODE)
            rc = flatten_into(git_dir, e->sha1, full_path, out, cap);
        else
            rc = flatten_append(out, cap, full_path, e->mode, e->sha1);

        free(full_path);
    }

    sg_tree_free(&tree);
    return rc;
}

int sg_tree_flatten(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN], sg_flat_list *out)
{
    size_t cap = 0;

    out->entries = NULL;
    out->count = 0;
    if (flatten_into(git_dir, tree_id, "", out, &cap) != 0) {
        sg_flat_list_free(out);
        return -1;
    }
    return 0;
}

void sg_flat_list_free(sg_flat_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->entries[i].path);
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
}

int sg_tree_build_from_index(const char *git_dir, const sg_index *idx,
                             unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *flat = NULL;
    size_t i;
    int rc;

    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage != 0)
            return -1;
    }

    if (idx->count > 0) {
        flat = malloc(idx->count * sizeof(*flat));
        if (flat == NULL)
            return -1;
        for (i = 0; i < idx->count; i++) {
            flat[i].path = idx->entries[i].path; /* not owned, transient view */
            flat[i].mode = idx->entries[i].mode;
            memcpy(flat[i].sha1, idx->entries[i].sha1, SG_SHA1_RAW_LEN);
        }
    }

    rc = sg_tree_build(git_dir, flat, idx->count, tree_id_out);
    free(flat);
    return rc;
}

int sg_tree_build_from_workdir(const char *git_dir, const char *repo_root, const sg_index *idx,
                               unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    sg_flat_entry *entries = NULL;
    size_t entry_count = 0;
    size_t i;
    int rc = -1;
    int chunk_enabled = 0;
    size_t chunk_threshold = SG_CHUNK_DEFAULT_THRESHOLD;

    if (idx->count > 0) {
        entries = malloc(idx->count * sizeof(*entries));
        if (entries == NULL)
            return -1;
    }

    sg_repo_read_chunk_config(git_dir, &chunk_enabled, &chunk_threshold);

    for (i = 0; i < idx->count; i++) {
        char abspath[SG_TREE_BUILD_PATH_MAX];
        unsigned char *content = NULL;
        size_t content_len = 0;
        unsigned char blob_id[SG_SHA1_RAW_LEN];

        /* idx may hold several stage 1/2/3 entries for the same path while a
           conflict is unresolved (there is no separate stage-0 entry then).
           Entries are sorted by (path, stage), so duplicates are contiguous
           and the first one seen is stage 0 if one exists, otherwise the
           lowest of whatever conflict stages are present -- either way,
           exactly one representative per path is emitted, using whatever
           content currently sits in the working tree (e.g. the
           conflict-marked content), never producing a tree with two entries
           sharing a name. */
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, idx->entries[i].path);
        if (sg_read_file(abspath, &content, &content_len) == 0) {
            int write_ok;

            if (chunk_enabled) {
                int chunked;

                write_ok = sg_chunk_store_blob(git_dir, content, content_len, chunk_threshold,
                                              blob_id, &chunked) == 0;
            } else {
                write_ok = sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, blob_id) == 0;
            }

            free(content);
            if (!write_ok)
                goto out_free_entries;
        } else {
            /* working-tree file gone or unreadable: fall back to the blob the
               index already recorded, so this entry still resolves */
            memcpy(blob_id, idx->entries[i].sha1, SG_SHA1_RAW_LEN);
        }

        entries[entry_count].path = idx->entries[i].path; /* transient view, not owned */
        entries[entry_count].mode = idx->entries[i].mode;
        memcpy(entries[entry_count].sha1, blob_id, SG_SHA1_RAW_LEN);
        entry_count++;
    }

    rc = sg_tree_build(git_dir, entries, entry_count, tree_id_out);

out_free_entries:
    free(entries);
    return rc;
}

int sg_tree_build_from_untracked(const char *git_dir, const char *repo_root, const sg_index *idx,
                                 int include_ignored,
                                 unsigned char tree_id_out[SG_SHA1_RAW_LEN],
                                 size_t *file_count_out)
{
    char **paths = NULL;
    size_t count = 0;
    sg_flat_entry *entries = NULL;
    size_t i;
    int rc = -1;
    int chunk_enabled = 0;
    size_t chunk_threshold = SG_CHUNK_DEFAULT_THRESHOLD;

    if (sg_status_list_untracked(git_dir, repo_root, idx, include_ignored, &paths, &count) != 0)
        return -1;

    if (file_count_out != NULL)
        *file_count_out = count;

    if (count > 0) {
        entries = malloc(count * sizeof(*entries));
        if (entries == NULL)
            goto out_free_paths;
    }

    sg_repo_read_chunk_config(git_dir, &chunk_enabled, &chunk_threshold);

    for (i = 0; i < count; i++) {
        char abspath[SG_TREE_BUILD_PATH_MAX];
        unsigned char *content = NULL;
        size_t content_len = 0;
        unsigned char blob_id[SG_SHA1_RAW_LEN];
        struct stat st;
        unsigned int mode = 0100644;

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, paths[i]);
        if (stat(abspath, &st) == 0 && (st.st_mode & 0111))
            mode = 0100755;

        if (sg_read_file(abspath, &content, &content_len) != 0)
            goto out_free_entries;

        if (chunk_enabled) {
            int chunked;

            if (sg_chunk_store_blob(git_dir, content, content_len, chunk_threshold, blob_id,
                                    &chunked) != 0) {
                free(content);
                goto out_free_entries;
            }
        } else {
            if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, blob_id) != 0) {
                free(content);
                goto out_free_entries;
            }
        }
        free(content);

        entries[i].path = paths[i]; /* transient view, not owned here */
        entries[i].mode = mode;
        memcpy(entries[i].sha1, blob_id, SG_SHA1_RAW_LEN);
    }

    rc = sg_tree_build(git_dir, entries, count, tree_id_out);

out_free_entries:
    free(entries);
out_free_paths:
    for (i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
    return rc;
}

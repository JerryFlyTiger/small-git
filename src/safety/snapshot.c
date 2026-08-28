#include "sg/snapshot.h"

#include "sg/loose.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SG_SNAPSHOT_SLUG_MAX 40

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static void slugify(const char *label, char *out, size_t out_cap)
{
    size_t n = 0;
    size_t i;

    for (i = 0; label[i] != '\0' && n < out_cap - 1; i++) {
        unsigned char c = (unsigned char)label[i];
        int keep = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');

        out[n++] = keep ? (char)c : '-';
    }
    out[n] = '\0';
}

static int ref_exists(const char *undo_dir, const char *name)
{
    char path[SG_PATH_MAX];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", undo_dir, name);
    return stat(path, &st) == 0;
}

/* Tries "<ts>-<slug>", then "<ts>-<slug>-2", "-3", ... until a name that
   isn't already taken is found -- a same-second collision must never clobber
   an existing snapshot. */
static int pick_unique_ref_name(const char *undo_dir, long long ts, const char *slug, char *out,
                                size_t out_size)
{
    int suffix;

    if ((size_t)snprintf(out, out_size, "%lld-%s", ts, slug) >= out_size)
        return -1;
    if (!ref_exists(undo_dir, out))
        return 0;

    for (suffix = 2; suffix < 10000; suffix++) {
        if ((size_t)snprintf(out, out_size, "%lld-%s-%d", ts, slug, suffix) >= out_size)
            return -1;
        if (!ref_exists(undo_dir, out))
            return 0;
    }
    return -1;
}

int sg_snapshot_create(const char *git_dir, const char *repo_root, const sg_index *idx,
                       const char *label, unsigned char commit_id_out[SG_SHA1_RAW_LEN],
                       char *bad_path)
{
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char parent_id[SG_SHA1_RAW_LEN];
    int has_parent;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char *cleaned_message = NULL;
    const char *name;
    const char *email;
    char undo_dir[SG_PATH_MAX];
    char slug[SG_SNAPSHOT_SLUG_MAX + 1];
    char ref_name[SG_SNAPSHOT_SLUG_MAX + 64];
    char ref_path[SG_PATH_MAX];
    long long ts;
    int rc = -1;

    memset(&commit, 0, sizeof(commit));

    if (sg_tree_build_from_workdir(git_dir, repo_root, idx, SG_WORKDIR_MISSING_KEEP_INDEX_BLOB, tree_id,
                                   bad_path) != 0)
        return -1;

    has_parent = (sg_ref_resolve_head(git_dir, parent_id) == 0);

    name = env_or("GIT_AUTHOR_NAME", "small_git");
    email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (has_parent) {
        commit.parents = malloc(sizeof(*commit.parents));
        if (commit.parents == NULL)
            return -1;
        memcpy(commit.parents[0], parent_id, SG_SHA1_RAW_LEN);
        commit.parent_count = 1;
    }
    commit.author_name = (char *)name;
    commit.author_email = (char *)email;
    commit.author_time = (long long)time(NULL);
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)name;
    commit.committer_email = (char *)email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");

    if (sg_message_cleanup(label, &cleaned_message) != 0) {
        free(commit.parents);
        return -1;
    }
    commit.message = cleaned_message;

    if (sg_commit_serialize(&commit, &serialized, &serialized_len) != 0) {
        free(commit.parents);
        free(cleaned_message);
        cleaned_message = NULL;
        return -1;
    }
    free(commit.parents);
    commit.parents = NULL;
    free(cleaned_message);
    cleaned_message = NULL;

    if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) != 0) {
        free(serialized);
        return -1;
    }
    free(serialized);

    snprintf(undo_dir, sizeof(undo_dir), "%s/refs/small-git/undo", git_dir);
    slugify(label, slug, sizeof(slug));
    ts = (long long)time(NULL);
    if (pick_unique_ref_name(undo_dir, ts, slug, ref_name, sizeof(ref_name)) != 0)
        return -1;

    snprintf(ref_path, sizeof(ref_path), "refs/small-git/undo/%s", ref_name);
    if (sg_ref_update(git_dir, ref_path, commit_id, NULL) != 0)
        return -1;

    if (commit_id_out != NULL)
        memcpy(commit_id_out, commit_id, SG_SHA1_RAW_LEN);
    rc = 0;

    return rc;
}

/* Pairs an entry with the ref file's mtime (nanosecond precision) purely to
   break ties when two snapshots land in the same committer_time second --
   commit timestamps only have second resolution, but the filesystem can tell
   which of two same-second refs was actually written later. */
typedef struct {
    sg_snapshot_entry entry;
    long long mtime_key;
} sg_snapshot_sort_item;

static int cmp_snapshot_item_desc(const void *a, const void *b)
{
    const sg_snapshot_sort_item *ia = a;
    const sg_snapshot_sort_item *ib = b;

    if (ia->entry.timestamp != ib->entry.timestamp)
        return ia->entry.timestamp > ib->entry.timestamp ? -1 : 1;
    if (ia->mtime_key != ib->mtime_key)
        return ia->mtime_key > ib->mtime_key ? -1 : 1;
    return strcmp(ib->entry.ref_name, ia->entry.ref_name);
}

int sg_snapshot_list_read(const char *git_dir, sg_snapshot_list *out)
{
    char undo_dir[SG_PATH_MAX];
    DIR *d;
    struct dirent *de;
    sg_snapshot_sort_item *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    memset(out, 0, sizeof(*out));

    snprintf(undo_dir, sizeof(undo_dir), "%s/refs/small-git/undo", git_dir);
    d = opendir(undo_dir);
    if (d == NULL)
        return 0; /* no snapshots taken yet: an empty list, not an error */

    while ((de = readdir(d)) != NULL) {
        char ref_path[SG_PATH_MAX];
        struct stat st;
        char hexbuf[SG_SHA1_HEX_LEN + 2];
        FILE *f;
        char *nl;
        unsigned char commit_id[SG_SHA1_RAW_LEN];
        unsigned char *content;
        size_t content_len;
        sg_obj_type type;
        sg_commit commit;
        char *ref_name_dup;
        char *message_dup;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        snprintf(ref_path, sizeof(ref_path), "%s/%s", undo_dir, de->d_name);
        if (stat(ref_path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        f = fopen(ref_path, "rb");
        if (f == NULL)
            continue;
        if (fgets(hexbuf, sizeof(hexbuf), f) == NULL) {
            fclose(f);
            continue;
        }
        fclose(f);
        nl = strchr(hexbuf, '\n');
        if (nl != NULL)
            *nl = '\0';
        if (sg_hex_to_sha1(hexbuf, commit_id) != 0)
            continue;

        if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 ||
           type != SG_OBJ_COMMIT)
            continue;
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            free(content);
            continue;
        }
        free(content);

        ref_name_dup = strdup(de->d_name);
        message_dup = strdup(commit.message);
        if (ref_name_dup == NULL || message_dup == NULL) {
            free(ref_name_dup);
            free(message_dup);
            sg_commit_free(&commit);
            continue; /* skip rather than abort the whole listing on OOM */
        }

        if (count == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            sg_snapshot_sort_item *grown = realloc(items, new_cap * sizeof(*grown));

            if (grown == NULL) {
                size_t j;

                free(ref_name_dup);
                free(message_dup);
                sg_commit_free(&commit);
                /* items[0..count-1] each own a strdup'd ref_name/message that
                   would otherwise leak once `items` itself is freed below. */
                for (j = 0; j < count; j++) {
                    free(items[j].entry.ref_name);
                    free(items[j].entry.message);
                }
                free(items);
                closedir(d);
                memset(out, 0, sizeof(*out));
                return -1;
            }
            items = grown;
            cap = new_cap;
        }

        items[count].entry.ref_name = ref_name_dup;
        memcpy(items[count].entry.commit_id, commit_id, SG_SHA1_RAW_LEN);
        items[count].entry.timestamp = commit.committer_time;
        items[count].entry.message = message_dup;
#if defined(__APPLE__)
        items[count].mtime_key = (long long)st.st_mtimespec.tv_sec * 1000000000LL +
            st.st_mtimespec.tv_nsec;
#else
        items[count].mtime_key = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
        count++;

        sg_commit_free(&commit);
    }
    closedir(d);

    qsort(items, count, sizeof(*items), cmp_snapshot_item_desc);

    if (count > 0) {
        sg_snapshot_entry *entries = malloc(count * sizeof(*entries));
        size_t i;

        if (entries == NULL) {
            for (i = 0; i < count; i++) {
                free(items[i].entry.ref_name);
                free(items[i].entry.message);
            }
            free(items);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        for (i = 0; i < count; i++)
            entries[i] = items[i].entry;
        out->entries = entries;
    }
    out->count = count;
    free(items);
    return 0;
}

void sg_snapshot_list_free(sg_snapshot_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        free(list->entries[i].ref_name);
        free(list->entries[i].message);
    }
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
}

int sg_snapshot_get_tree(const char *git_dir, const sg_snapshot_list *list, size_t index,
                         unsigned char tree_id_out[SG_SHA1_RAW_LEN])
{
    if (index >= list->count)
        return -1;

    return sg_commit_tree_of(git_dir, list->entries[index].commit_id, tree_id_out);
}

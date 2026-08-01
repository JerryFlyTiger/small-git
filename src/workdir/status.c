#include "sg/status.h"

#include "sg/hash.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int status_list_add(sg_status_list *list, const char *path, sg_status_kind kind)
{
    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        sg_status_entry *grown = realloc(list->entries, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        list->entries = grown;
        list->cap = new_cap;
    }
    list->entries[list->count].path = strdup(path);
    if (list->entries[list->count].path == NULL)
        return -1;
    list->entries[list->count].kind = kind;
    list->count++;
    return 0;
}

void sg_status_list_free(sg_status_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->entries[i].path);
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->cap = 0;
}

/* True if idx has any stage 1/2/3 entry at path -- i.e. path is an
   unresolved conflict, already surfaced by `sg status`'s "Unmerged paths"
   section rather than here. */
static int path_has_unmerged_stage(const sg_index *idx, const char *path)
{
    return sg_index_find_stage(idx, path, 1) >= 0 || sg_index_find_stage(idx, path, 2) >= 0 ||
        sg_index_find_stage(idx, path, 3) >= 0;
}

/* Both loops below only ever look at stage-0 entries: a path with an
   unresolved conflict has no stage-0 entry (only 1/2/3), so it is simply
   absent from the staged/unstaged comparison here -- `sg status`'s
   "Unmerged paths" section is what reports those separately, and treating
   stage 1/2/3 rows as ordinary entries here would both misreport them and
   break this loop's assumption of at most one idx entry per path. A HEAD
   path with an unresolved conflict (no stage-0 counterpart) must likewise
   not be reported as "deleted" here -- path_has_unmerged_stage catches that,
   since otherwise it would look exactly like index really did drop it. */
int sg_status_diff_staged(const sg_flat_list *head_flat, const sg_index *idx, sg_status_list *out)
{
    size_t hi = 0;
    size_t ii = 0;

    memset(out, 0, sizeof(*out));

    while (hi < head_flat->count || ii < idx->count) {
        int cmp;

        if (ii < idx->count && idx->entries[ii].stage != 0) {
            ii++;
            continue;
        }

        if (hi >= head_flat->count)
            cmp = 1;
        else if (ii >= idx->count)
            cmp = -1;
        else
            cmp = strcmp(head_flat->entries[hi].path, idx->entries[ii].path);

        if (cmp == 0) {
            if (memcmp(head_flat->entries[hi].sha1, idx->entries[ii].sha1, SG_SHA1_RAW_LEN) != 0 ||
               head_flat->entries[hi].mode != idx->entries[ii].mode) {
                if (status_list_add(out, idx->entries[ii].path, SG_STATUS_MODIFIED) != 0)
                    return -1;
            }
            hi++;
            ii++;
        } else if (cmp < 0) {
            if (!path_has_unmerged_stage(idx, head_flat->entries[hi].path)) {
                if (status_list_add(out, head_flat->entries[hi].path, SG_STATUS_DELETED) != 0)
                    return -1;
            }
            hi++;
        } else {
            if (status_list_add(out, idx->entries[ii].path, SG_STATUS_NEW) != 0)
                return -1;
            ii++;
        }
    }
    return 0;
}

int sg_status_diff_unstaged(const char *repo_root, const sg_index *idx, sg_status_list *out)
{
    size_t i;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < idx->count; i++) {
        char abspath[4096];
        unsigned char wd_sha1[SG_SHA1_RAW_LEN];
        struct stat st;

        if (idx->entries[i].stage != 0)
            continue;

        snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, idx->entries[i].path);
        if (stat(abspath, &st) != 0) {
            if (status_list_add(out, idx->entries[i].path, SG_STATUS_DELETED) != 0)
                return -1;
            continue;
        }
        if (sg_hash_file_blob(abspath, wd_sha1) != 0) {
            if (status_list_add(out, idx->entries[i].path, SG_STATUS_DELETED) != 0)
                return -1;
            continue;
        }
        if (memcmp(wd_sha1, idx->entries[i].sha1, SG_SHA1_RAW_LEN) != 0) {
            if (status_list_add(out, idx->entries[i].path, SG_STATUS_MODIFIED) != 0)
                return -1;
        }
    }
    return 0;
}

#include "sg/diff.h"

#include <stdlib.h>
#include <string.h>

/* Rename detection, as a pass over an already-built change list rather than
   something the four builders each do for themselves -- the same reasoning
   as sg_diff_list_filter: one place that decides cannot disagree with
   itself. See include/sg/diff.h for the ordering rule against filtering,
   which is measured, not assumed. */

static int is_deletion(const sg_diff_entry *e)
{
    return !e->unmerged && e->old_side.kind != SG_DIFF_SIDE_ABSENT &&
           e->new_side.kind == SG_DIFF_SIDE_ABSENT;
}

static int is_addition(const sg_diff_entry *e)
{
    return !e->unmerged && e->old_side.kind == SG_DIFF_SIDE_ABSENT &&
           e->new_side.kind != SG_DIFF_SIDE_ABSENT;
}

int sg_diff_detect_renames(const char *git_dir, sg_diff_list *list, int min_score)
{
    unsigned char *ids;   /* effective id per entry, valid iff usable[i] */
    char *usable;         /* 1 when this entry has an id worth comparing */
    char *taken;          /* 1 when this source has already been claimed */
    size_t i;
    size_t write;
    int paired = 0;

    if (list == NULL || list->count == 0 || min_score <= 0)
        return 0;

    ids = malloc(list->count * SG_SHA1_RAW_LEN);
    usable = calloc(list->count, 1);
    taken = calloc(list->count, 1);
    if (ids == NULL || usable == NULL || taken == NULL) {
        free(ids);
        free(usable);
        free(taken);
        return -1;
    }

    for (i = 0; i < list->count; i++) {
        const sg_diff_entry *e = &list->entries[i];
        const sg_diff_side *side = is_deletion(e) ? &e->old_side
                                 : is_addition(e) ? &e->new_side
                                 : NULL;

        /* -1 means the id could not be verified. Such a side is never
           paired: two unverified ids that happen to be equal are not proof
           the content matches (sg_diff_side_effective_id's contract), and
           the failure direction here must be "no rename", never a rename
           that did not happen. */
        if (side != NULL &&
            sg_diff_side_effective_id(git_dir, side, ids + i * SG_SHA1_RAW_LEN) == 0)
            usable[i] = 1;
    }

    /* Destinations in path order, each claiming the first unclaimed source
       with identical content -- measured against git 2.55.0, that is how two
       identical sources and two identical destinations pair up, and how one
       source with two identical destinations leaves the second destination
       an ordinary addition. */
    for (i = 0; i < list->count; i++) {
        size_t j;

        if (!usable[i] || !is_addition(&list->entries[i]))
            continue;

        for (j = 0; j < list->count; j++) {
            char *stolen;

            if (!usable[j] || taken[j] || !is_deletion(&list->entries[j]))
                continue;
            if (memcmp(ids + i * SG_SHA1_RAW_LEN, ids + j * SG_SHA1_RAW_LEN,
                      SG_SHA1_RAW_LEN) != 0)
                continue;

            /* The destination becomes the rename row; the source's path is
               handed over rather than copied, so this cannot fail partway
               and leave the list half-rewritten. */
            stolen = list->entries[j].path;
            list->entries[j].path = NULL;
            list->entries[i].old_path = stolen;
            list->entries[i].old_side = list->entries[j].old_side;
            list->entries[i].score = 100;
            taken[j] = 1;
            paired = 1;
            break;
        }
    }

    if (paired) {
        /* Drop the claimed sources. Destinations keep the path they already
           had, so what is left is still sorted by path. */
        write = 0;
        for (i = 0; i < list->count; i++) {
            if (taken[i]) {
                free(list->entries[i].old_path);
                continue;
            }
            if (write != i)
                list->entries[write] = list->entries[i];
            write++;
        }
        list->count = write;
    }

    free(ids);
    free(usable);
    free(taken);
    return 0;
}

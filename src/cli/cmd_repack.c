#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/pack.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Walks git_dir/objects/<xx>/<38 remaining hex chars>, skipping the "pack"
   and "info" entries that live alongside the two-hex-char loose object
   directories, and collects every loose object's id plus the on-disk path
   to its file (so the caller can delete it once it's safely packed). */
static int collect_loose_objects(const char *git_dir, unsigned char (**ids_out)[SG_SHA1_RAW_LEN],
                                 char ***paths_out, size_t *count_out)
{
    char objects_dir[SG_PATH_MAX];
    DIR *d;
    struct dirent *sub;
    unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
    char **paths = NULL;
    size_t count = 0, cap = 0;

    *ids_out = NULL;
    *paths_out = NULL;
    *count_out = 0;

    snprintf(objects_dir, sizeof(objects_dir), "%s/objects", git_dir);
    d = opendir(objects_dir);
    if (d == NULL)
        return 0; /* no objects/ dir at all: nothing to repack, not an error */

    while ((sub = readdir(d)) != NULL) {
        char subdir_path[SG_PATH_MAX];
        DIR *sd;
        struct dirent *file;

        if (strcmp(sub->d_name, ".") == 0 || strcmp(sub->d_name, "..") == 0)
            continue;
        if (strcmp(sub->d_name, "pack") == 0 || strcmp(sub->d_name, "info") == 0)
            continue;
        if (strlen(sub->d_name) != 2)
            continue;

        snprintf(subdir_path, sizeof(subdir_path), "%s/%s", objects_dir, sub->d_name);
        sd = opendir(subdir_path);
        if (sd == NULL)
            continue;

        while ((file = readdir(sd)) != NULL) {
            char hex[SG_SHA1_HEX_LEN + 1];
            unsigned char id[SG_SHA1_RAW_LEN];
            char full_path[SG_PATH_MAX];
            char *dup_path;

            if (strlen(file->d_name) != SG_SHA1_HEX_LEN - 2)
                continue;

            snprintf(hex, sizeof(hex), "%s%s", sub->d_name, file->d_name);
            if (sg_hex_to_sha1(hex, id) != 0)
                continue;

            snprintf(full_path, sizeof(full_path), "%s/%s", subdir_path, file->d_name);
            dup_path = strdup(full_path);
            if (dup_path == NULL) {
                closedir(sd);
                closedir(d);
                goto fail;
            }

            if (count == cap) {
                size_t new_cap = cap == 0 ? 16 : cap * 2;
                unsigned char(*grown_ids)[SG_SHA1_RAW_LEN] =
                    realloc(ids, new_cap * sizeof(*ids));
                char **grown_paths;

                if (grown_ids == NULL) {
                    free(dup_path);
                    closedir(sd);
                    closedir(d);
                    goto fail;
                }
                ids = grown_ids;

                grown_paths = realloc(paths, new_cap * sizeof(*paths));
                if (grown_paths == NULL) {
                    free(dup_path);
                    closedir(sd);
                    closedir(d);
                    goto fail;
                }
                paths = grown_paths;
                cap = new_cap;
            }

            memcpy(ids[count], id, SG_SHA1_RAW_LEN);
            paths[count] = dup_path;
            count++;
        }
        closedir(sd);
    }
    closedir(d);

    *ids_out = ids;
    *paths_out = paths;
    *count_out = count;
    return 0;

fail:
    {
        size_t i;

        for (i = 0; i < count; i++)
            free(paths[i]);
        free(paths);
        free(ids);
    }
    return -1;
}

int sg_cmd_repack(int argc, char **argv)
{
    char *git_dir;
    unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
    char **paths = NULL;
    size_t count = 0;
    size_t i;
    int rc = 0;

    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: sg repack\n");
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (collect_loose_objects(git_dir, &ids, &paths, &count) != 0) {
        fprintf(stderr, "sg: failed to scan loose objects\n");
        free(git_dir);
        return 1;
    }

    if (count == 0) {
        printf("Nothing to repack\n");
        free(git_dir);
        return 0;
    }

    if (sg_pack_write(git_dir, ids, count) != 0) {
        fprintf(stderr, "sg: failed to write pack\n");
        rc = 1;
        goto done;
    }

    /* the objects now live in the pack; drop the now-redundant loose copies,
       mirroring `git repack`'s behavior */
    for (i = 0; i < count; i++)
        unlink(paths[i]);

    printf("Repacked %zu object(s) into a new pack\n", count);

done:
    for (i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
    free(ids);
    free(git_dir);
    return rc;
}

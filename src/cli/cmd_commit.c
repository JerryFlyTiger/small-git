#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/tree_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

int sg_cmd_commit(int argc, char **argv)
{
    static const char usage[] = "usage: sg commit -m <message>\n";
    const char *message = NULL;
    char *git_dir;
    sg_index idx;
    sg_flat_entry *flat = NULL;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char parent_id[SG_SHA1_RAW_LEN];
    int has_parent;
    char *branch;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char commit_hex[SG_SHA1_HEX_LEN + 1];
    const char *name;
    const char *email;
    size_t i;
    int rc = 0;

    for (i = 1; (int)i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            if ((int)i + 1 >= argc) {
                fputs(usage, stderr);
                return 1;
            }
            message = argv[++i];
        } else {
            fputs(usage, stderr);
            return 1;
        }
    }
    if (message == NULL) {
        fputs(usage, stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        return 1;
    }

    if (idx.count > 0) {
        flat = malloc(idx.count * sizeof(*flat));
        if (flat == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            sg_index_free(&idx);
            free(git_dir);
            return 1;
        }
        for (i = 0; i < idx.count; i++) {
            flat[i].path = idx.entries[i].path; /* not owned, transient view */
            flat[i].mode = idx.entries[i].mode;
            memcpy(flat[i].sha1, idx.entries[i].sha1, SG_SHA1_RAW_LEN);
        }
    }

    if (sg_tree_build(git_dir, flat, idx.count, tree_id) != 0) {
        fprintf(stderr, "sg: failed to build tree from index\n");
        free(flat);
        sg_index_free(&idx);
        free(git_dir);
        return 1;
    }
    free(flat);
    sg_index_free(&idx);

    has_parent = (sg_ref_resolve_head(git_dir, parent_id) == 0);

    branch = sg_ref_current_branch(git_dir);
    if (branch == NULL) {
        fprintf(stderr, "sg: failed to determine current branch\n");
        free(git_dir);
        return 1;
    }

    name = env_or("GIT_AUTHOR_NAME", "small_git");
    email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (has_parent) {
        commit.parents = malloc(sizeof(*commit.parents));
        if (commit.parents == NULL) {
            fprintf(stderr, "sg: out of memory\n");
            free(branch);
            free(git_dir);
            return 1;
        }
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
    commit.message = (char *)message;

    if (sg_commit_serialize(&commit, &serialized, &serialized_len) != 0) {
        fprintf(stderr, "sg: failed to serialize commit\n");
        free(commit.parents);
        free(branch);
        free(git_dir);
        return 1;
    }
    free(commit.parents);

    if (sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) != 0) {
        fprintf(stderr, "sg: failed to write commit object\n");
        free(serialized);
        free(branch);
        free(git_dir);
        return 1;
    }
    free(serialized);

    if (sg_ref_update_branch(git_dir, branch, commit_id) != 0) {
        fprintf(stderr, "sg: failed to update branch '%s'\n", branch);
        rc = 1;
    }

    sg_sha1_to_hex(commit_id, commit_hex);
    {
        char short_hex[8];
        const char *first_line_end = strchr(message, '\n');
        size_t first_line_len = first_line_end != NULL ? (size_t)(first_line_end - message)
                                                        : strlen(message);

        memcpy(short_hex, commit_hex, 7);
        short_hex[7] = '\0';
        printf("[%s%s %s] %.*s\n", branch, has_parent ? "" : " (root-commit)", short_hex,
              (int)first_line_len, message);
    }

    free(branch);
    free(git_dir);
    return rc;
}

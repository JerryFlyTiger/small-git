#include "sg/cli.h"

#include "sg/apply.h"
#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/pack.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/transport.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_PATH_MAX 4096

/* Takes the last non-empty path segment of the URL, stripping a trailing
   ".git" -- e.g. "http://host/a/b/repo.git" -> "repo", matching `git clone`
   with no explicit destination directory. */
static char *derive_target_dir(const char *url)
{
    size_t len = strlen(url);
    size_t end = len;
    size_t start;
    size_t seg_len;
    char *name;

    while (end > 0 && url[end - 1] == '/')
        end--;
    if (end == 0)
        return NULL;
    start = end;
    while (start > 0 && url[start - 1] != '/')
        start--;

    seg_len = end - start;
    if (seg_len > 4 && strcmp(url + start + seg_len - 4, ".git") == 0)
        seg_len -= 4;
    if (seg_len == 0)
        return NULL;

    name = malloc(seg_len + 1);
    if (name == NULL)
        return NULL;
    memcpy(name, url + start, seg_len);
    name[seg_len] = '\0';
    return name;
}

/* A missing directory is fine (will be created); an existing, non-empty one
   is refused, matching `git clone`'s own guard against clobbering. */
static int target_dir_is_usable(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int empty = 1;

    if (d == NULL)
        return 1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        empty = 0;
        break;
    }
    closedir(d);
    return empty;
}

/* refs/heads/<name> -> refs/remotes/<remote>/<name>, refs/tags/<name> ->
   refs/tags/<name> (unchanged), and SG_CHUNK_KEEPALIVE_REF -> the same path
   locally (it's
   shared keep-alive plumbing, not a branch/tag, so it isn't namespaced under
   refs/remotes/) -- see the phase 6b spec's clone/fetch section: this is
   what lets a fresh `sg clone` from another sg repo actually recover a
   chunked file's chunk blobs instead of just the pointer text, since
   without this ref the freshly-cloned chunks would be unreachable here too
   and gc'd right back out from under the pointer. */
static int write_remote_and_tag_refs(const char *git_dir, const sg_ref_adv *adv,
                                     const char *remote_name)
{
    size_t i;

    for (i = 0; i < adv->count; i++) {
        const char *name = adv->refs[i].name;
        char ref_path[SG_PATH_MAX];
        int is_keepalive = 0;

        if (strncmp(name, "refs/heads/", 11) == 0) {
            snprintf(ref_path, sizeof(ref_path), "refs/remotes/%s/%s", remote_name, name + 11);
        } else if (strncmp(name, "refs/tags/", 10) == 0) {
            snprintf(ref_path, sizeof(ref_path), "%s", name);
        } else if (strcmp(name, SG_CHUNK_KEEPALIVE_REF) == 0) {
            snprintf(ref_path, sizeof(ref_path), "%s", name);
            is_keepalive = 1;
        } else {
            continue;
        }
        if (sg_ref_update(git_dir, ref_path, adv->refs[i].id, NULL) != 0)
            return -1;

        /* This clone just received (and locally wrote) another sg repo's own
           SG_CHUNK_KEEPALIVE_REF -- record that fact in .git/config the same
           way sg_chunk_store_blob does for chunks produced locally (see
           sg_repo_mark_chunking_used's doc comment).

           Fatal, not a warning: without the marker, a repo that later loses
           this ref reads as "never chunked anything", which is exactly the
           case chunk_resolve treats as ordinary content -- it would hand back
           pointer text as file contents. sg_chunk_store_blob already refuses
           to chunk at all when it can't write the marker; a clone that keeps
           going here would be the one path left where the two disagree. */
        if (is_keepalive && sg_repo_mark_chunking_used(git_dir) != 0) {
            fprintf(stderr, "sg: 無法在 .git/config 標記本地端已使用過分塊儲存\n");
            return -1;
        }
    }
    return 0;
}

/* Deduplicated want list: the tip of every refs/heads/, refs/tags/, and (if
   advertised) SG_CHUNK_KEEPALIVE_REF entry the server advertised -- the
   latter so a chunked file's chunk blobs actually come down in the pack,
   not just the pointer blob that names them (see the phase 6b spec's
   clone/fetch section). A real, unmodified `git` server never advertises
   this custom ref outside of its own refs/heads/tags refspecs, so this is
   sg <-> sg specific; against a plain `git clone`/`git fetch` of an sg
   repo, this list is unaffected and the known limitation documented in
   interop.sh applies. */
static int build_want_ids(const sg_ref_adv *adv, unsigned char (**ids_out)[SG_SHA1_RAW_LEN],
                          size_t *count_out)
{
    unsigned char(*ids)[SG_SHA1_RAW_LEN];
    size_t count = 0;
    size_t i, j;

    ids = malloc(adv->count > 0 ? adv->count * sizeof(*ids) : 1);
    if (ids == NULL)
        return -1;

    for (i = 0; i < adv->count; i++) {
        int dup = 0;

        if (strncmp(adv->refs[i].name, "refs/heads/", 11) != 0 &&
           strncmp(adv->refs[i].name, "refs/tags/", 10) != 0 &&
           strcmp(adv->refs[i].name, SG_CHUNK_KEEPALIVE_REF) != 0)
            continue;
        for (j = 0; j < count; j++) {
            if (memcmp(ids[j], adv->refs[i].id, SG_SHA1_RAW_LEN) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            memcpy(ids[count], adv->refs[i].id, SG_SHA1_RAW_LEN);
            count++;
        }
    }

    *ids_out = ids;
    *count_out = count;
    return 0;
}

/* Chooses the default branch (bare name, no "refs/heads/" prefix): prefers
   the advertised symref=HEAD:refs/heads/X capability, then "main", then
   "master", then simply the first advertised head. Returns a malloc'd
   string, or NULL if the remote has no branches at all (e.g. tags only). */
static char *pick_default_branch(const sg_ref_adv *adv)
{
    size_t i;

    if (adv->head_symref != NULL && strncmp(adv->head_symref, "refs/heads/", 11) == 0) {
        for (i = 0; i < adv->count; i++) {
            if (strcmp(adv->refs[i].name, adv->head_symref) == 0)
                return strdup(adv->head_symref + 11);
        }
    }
    for (i = 0; i < adv->count; i++) {
        if (strcmp(adv->refs[i].name, "refs/heads/main") == 0)
            return strdup("main");
    }
    for (i = 0; i < adv->count; i++) {
        if (strcmp(adv->refs[i].name, "refs/heads/master") == 0)
            return strdup("master");
    }
    for (i = 0; i < adv->count; i++) {
        if (strncmp(adv->refs[i].name, "refs/heads/", 11) == 0)
            return strdup(adv->refs[i].name + 11);
    }
    return NULL;
}

static int fetch_and_store_pack(const char *git_dir, const char *url,
                                const unsigned char (*want_ids)[SG_SHA1_RAW_LEN],
                                size_t want_count)
{
    unsigned char *pack_data = NULL;
    size_t pack_len = 0;
    char *pack_path = NULL;
    int rc = -1;

    if (sg_transport_fetch_pack(url, want_ids, want_count, NULL, 0, &pack_data, &pack_len) != 0)
        return -1;

    if (sg_pack_store_raw(git_dir, pack_data, pack_len, &pack_path) != 0) {
        fprintf(stderr, "sg: failed to store the received pack\n");
        goto done;
    }
    if (sg_pack_index_existing(pack_path) != 0) {
        fprintf(stderr, "sg: failed to index the received pack\n");
        goto done;
    }
    rc = 0;

done:
    free(pack_path);
    free(pack_data);
    return rc;
}

/* The URL lands verbatim in .git/config, so an embedded newline would let a
   caller passing through an untrusted URL (a dependency manifest, say) append
   arbitrary config lines to the fresh repo. */
static int url_is_config_safe(const char *url)
{
    size_t i;

    for (i = 0; url[i] != '\0'; i++) {
        if ((unsigned char)url[i] < 0x20 || (unsigned char)url[i] == 0x7f)
            return 0;
    }
    return 1;
}

static int write_config_stanza(const char *git_dir, const char *url, const char *default_branch)
{
    char path[SG_PATH_MAX];
    FILE *f;

    if (!url_is_config_safe(url)) {
        fprintf(stderr, "sg: URL 含有控制字元，拒絕寫入 config\n");
        return -1;
    }

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "a");
    if (f == NULL)
        return -1;

    fprintf(f, "[remote \"origin\"]\n\turl = %s\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n",
           url);
    if (default_branch != NULL)
        fprintf(f, "[branch \"%s\"]\n\tremote = origin\n\tmerge = refs/heads/%s\n",
               default_branch, default_branch);

    return fclose(f) == 0 ? 0 : -1;
}

int sg_cmd_clone(int argc, char **argv)
{
    const char *url;
    char *target_dir = NULL;
    char git_dir[SG_PATH_MAX];
    char *repo_root = NULL;
    sg_ref_adv adv;
    int have_adv = 0;
    unsigned char(*want_ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t want_count = 0;
    char *default_branch = NULL;
    unsigned char target_commit_id[SG_SHA1_RAW_LEN];
    int initialized = 0;
    int rc = 1;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: sg clone <url> [<directory>]\n");
        return 1;
    }
    url = argv[1];

    if (argc == 3) {
        target_dir = strdup(argv[2]);
    } else {
        target_dir = derive_target_dir(url);
    }
    if (target_dir == NULL) {
        fprintf(stderr, "sg: cannot determine a destination directory from '%s'\n", url);
        return 1;
    }

    if (!target_dir_is_usable(target_dir)) {
        fprintf(stderr, "sg: destination path '%s' already exists and is not empty\n",
               target_dir);
        goto done;
    }

    printf("Cloning into '%s'...\n", target_dir);

    if (sg_repo_init(target_dir) != 0) {
        fprintf(stderr, "sg: failed to initialize '%s'\n", target_dir);
        goto done;
    }
    initialized = 1;
    snprintf(git_dir, sizeof(git_dir), "%s/.git", target_dir);
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        goto done;
    }

    if (sg_transport_ls_refs(url, &adv) != 0)
        goto done;
    have_adv = 1;

    if (adv.count == 0) {
        printf("warning: 遠端是空的 repository\n");
        if (write_config_stanza(git_dir, url, NULL) != 0) {
            fprintf(stderr, "sg: failed to write config\n");
            goto done;
        }
        rc = 0;
        goto done;
    }

    if (build_want_ids(&adv, &want_ids, &want_count) != 0 || want_count == 0) {
        fprintf(stderr, "sg: remote advertised no fetchable branches or tags\n");
        goto done;
    }

    if (fetch_and_store_pack(git_dir, url, want_ids, want_count) != 0)
        goto done;

    if (write_remote_and_tag_refs(git_dir, &adv, "origin") != 0) {
        fprintf(stderr, "sg: failed to write remote-tracking refs\n");
        goto done;
    }

    default_branch = pick_default_branch(&adv);
    if (default_branch == NULL) {
        printf("warning: 遠端沒有任何分支，只有 tag；跳過 checkout\n");
        if (write_config_stanza(git_dir, url, NULL) != 0) {
            fprintf(stderr, "sg: failed to write config\n");
            goto done;
        }
        rc = 0;
        goto done;
    }

    {
        char full_ref[SG_PATH_MAX];
        size_t i;
        int found = 0;

        snprintf(full_ref, sizeof(full_ref), "refs/heads/%s", default_branch);
        for (i = 0; i < adv.count; i++) {
            if (strcmp(adv.refs[i].name, full_ref) == 0) {
                memcpy(target_commit_id, adv.refs[i].id, SG_SHA1_RAW_LEN);
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "sg: internal error: default branch '%s' not found in advertisement\n",
                   default_branch);
            goto done;
        }
    }

    if (sg_ref_set_head(git_dir, default_branch, NULL) != 0) {
        fprintf(stderr, "sg: failed to write HEAD\n");
        goto done;
    }

    {
        char branch_ref_path[SG_PATH_MAX];

        snprintf(branch_ref_path, sizeof(branch_ref_path), "refs/heads/%s", default_branch);
        if (sg_ref_update(git_dir, branch_ref_path, target_commit_id, NULL) != 0) {
            fprintf(stderr, "sg: failed to create local branch '%s'\n", default_branch);
            goto done;
        }
    }

    if (write_config_stanza(git_dir, url, default_branch) != 0) {
        fprintf(stderr, "sg: failed to write config\n");
        goto done;
    }

    {
        unsigned char tree_id[SG_SHA1_RAW_LEN];

        if (sg_commit_tree_of(git_dir, target_commit_id, tree_id) != 0) {
            fprintf(stderr, "sg: corrupt commit for branch '%s'\n", default_branch);
            goto done;
        }
        if (sg_apply_tree_to_workdir(git_dir, repo_root, tree_id) != 0) {
            fprintf(stderr, "sg: failed to check out working tree\n");
            goto done;
        }
    }

    printf("Switched to branch '%s'\n", default_branch);
    rc = 0;

done:
    /* Deliberately not deleting target_dir here: auto-removing a directory
       tree is the most destructive thing this command could get wrong, and
       the user may have pointed us at a pre-existing empty directory. Say
       plainly that what's left is incomplete so it can't be mistaken for a
       working clone. */
    if (rc != 0 && initialized)
        fprintf(stderr,
               "sg: clone 未完成，'%s' 內容不完整，請自行刪除後再重試\n", target_dir);

    if (have_adv)
        sg_ref_adv_free(&adv);
    free(want_ids);
    free(default_branch);
    free(repo_root);
    free(target_dir);
    return rc;
}

#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/merge.h"
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
#include <sys/stat.h>

#define SG_FETCH_MAX_HAVES 256

/* Deduplicated want list: the tip of every refs/heads/, refs/tags/, and (if
   advertised) SG_CHUNK_KEEPALIVE_REF entry the server advertised -- the
   latter so a chunked file's chunk blobs actually come down in the pack,
   not just the pointer blob that names them (see the phase 6b spec's
   clone/fetch section). */
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

static int id_array_append(unsigned char (**arr)[SG_SHA1_RAW_LEN], size_t *len, size_t *cap,
                           const unsigned char id[SG_SHA1_RAW_LEN])
{
    if (*len == *cap) {
        size_t new_cap = (*cap == 0) ? 64 : *cap * 2;
        unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(*arr, new_cap * sizeof(**arr));

        if (grown == NULL)
            return -1;
        *arr = grown;
        *cap = new_cap;
    }
    memcpy((*arr)[*len], id, SG_SHA1_RAW_LEN);
    (*len)++;
    return 0;
}

static int id_array_contains(const unsigned char (*arr)[SG_SHA1_RAW_LEN], size_t len,
                             const unsigned char id[SG_SHA1_RAW_LEN])
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (memcmp(arr[i], id, SG_SHA1_RAW_LEN) == 0)
            return 1;
    }
    return 0;
}

/* Recursively walks dir_path (a refs/heads or refs/remotes subtree, which
   may nest arbitrarily for slash-containing branch names) collecting every
   regular file's leading 40 hex chars as a tip id. */
static int collect_dir_tips(const char *dir_path, unsigned char (**ids)[SG_SHA1_RAW_LEN],
                            size_t *count, size_t *cap)
{
    DIR *d = opendir(dir_path);
    struct dirent *de;

    if (d == NULL)
        return 0; /* missing subtree (e.g. no remotes yet) is fine */

    while ((de = readdir(d)) != NULL) {
        char full[SG_PATH_MAX];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
        if (stat(full, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (collect_dir_tips(full, ids, count, cap) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            unsigned char *content;
            size_t content_len;
            unsigned char id[SG_SHA1_RAW_LEN];

            if (sg_read_file(full, &content, &content_len) != 0)
                continue;
            if (content_len >= SG_SHA1_HEX_LEN) {
                char hex[SG_SHA1_HEX_LEN + 1];

                memcpy(hex, content, SG_SHA1_HEX_LEN);
                hex[SG_SHA1_HEX_LEN] = '\0';
                if (sg_hex_to_sha1(hex, id) == 0) {
                    if (id_array_append(ids, count, cap, id) != 0) {
                        free(content);
                        closedir(d);
                        return -1;
                    }
                }
            }
            free(content);
        }
    }
    closedir(d);
    return 0;
}

/* Simplified have-negotiation: BFS commit ancestry from every local branch
   and remote-tracking tip, newest-first-ish, capped at
   SG_FETCH_MAX_HAVES -- an intentional simplification (see phase 5b spec):
   an incomplete have list only costs some redundant data in the response,
   it can never make the result wrong. */
static int build_have_ids(const char *git_dir, unsigned char (**haves_out)[SG_SHA1_RAW_LEN],
                          size_t *have_count_out)
{
    char heads_dir[SG_PATH_MAX];
    char remotes_dir[SG_PATH_MAX];
    unsigned char(*tips)[SG_SHA1_RAW_LEN] = NULL;
    size_t tip_count = 0, tip_cap = 0;
    unsigned char(*visited)[SG_SHA1_RAW_LEN] = NULL;
    size_t visited_count = 0, visited_cap = 0;
    unsigned char(*queue)[SG_SHA1_RAW_LEN] = NULL;
    size_t queue_len = 0, queue_cap = 0, queue_head = 0;
    unsigned char(*haves)[SG_SHA1_RAW_LEN] = NULL;
    size_t have_count = 0;
    size_t i;
    int rc = -1;

    snprintf(heads_dir, sizeof(heads_dir), "%s/refs/heads", git_dir);
    snprintf(remotes_dir, sizeof(remotes_dir), "%s/refs/remotes", git_dir);
    if (collect_dir_tips(heads_dir, &tips, &tip_count, &tip_cap) != 0)
        goto done;
    if (collect_dir_tips(remotes_dir, &tips, &tip_count, &tip_cap) != 0)
        goto done;

    haves = malloc(SG_FETCH_MAX_HAVES * sizeof(*haves));
    if (haves == NULL)
        goto done;

    for (i = 0; i < tip_count; i++) {
        if (!id_array_contains(visited, visited_count, tips[i])) {
            if (id_array_append(&visited, &visited_count, &visited_cap, tips[i]) != 0)
                goto done;
            if (id_array_append(&queue, &queue_len, &queue_cap, tips[i]) != 0)
                goto done;
        }
    }

    while (queue_head < queue_len && have_count < SG_FETCH_MAX_HAVES) {
        unsigned char id[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;

        memcpy(id, queue[queue_head], SG_SHA1_RAW_LEN);
        queue_head++;

        memcpy(haves[have_count], id, SG_SHA1_RAW_LEN);
        have_count++;

        if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0 ||
           type != SG_OBJ_COMMIT)
            continue;
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            free(content);
            continue;
        }
        free(content);

        for (i = 0; i < commit.parent_count && have_count < SG_FETCH_MAX_HAVES; i++) {
            if (!id_array_contains(visited, visited_count, commit.parents[i])) {
                if (id_array_append(&visited, &visited_count, &visited_cap, commit.parents[i]) !=
                       0 ||
                   id_array_append(&queue, &queue_len, &queue_cap, commit.parents[i]) != 0) {
                    sg_commit_free(&commit);
                    goto done;
                }
            }
        }
        sg_commit_free(&commit);
    }

    *haves_out = haves;
    *have_count_out = have_count;
    haves = NULL;
    rc = 0;

done:
    free(tips);
    free(visited);
    free(queue);
    free(haves);
    return rc;
}

/* Fast-forward means old_id is an ancestor of new_id, i.e. their merge base
   is old_id itself -- the exact same test cmd_push.c's check_fast_forward
   already applies on the push side (sg_merge_base is the one shared source
   of truth for reachability in this codebase); -1 (unrelated history) and -2
   (criss-cross) both count as "not fast-forward" here, same as any other
   non-ancestor base. */
static int fetch_is_fast_forward(const char *git_dir, const unsigned char old_id[SG_SHA1_RAW_LEN],
                                 const unsigned char new_id[SG_SHA1_RAW_LEN])
{
    unsigned char base[SG_SHA1_RAW_LEN];

    return sg_merge_base(git_dir, old_id, new_id, base) == 0 &&
          memcmp(base, old_id, SG_SHA1_RAW_LEN) == 0;
}

static int read_ref_file(const char *path, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    unsigned char *content;
    size_t content_len;
    char hex[SG_SHA1_HEX_LEN + 1];
    int rc;

    if (sg_read_file(path, &content, &content_len) != 0)
        return -1;
    if (content_len < SG_SHA1_HEX_LEN) {
        free(content);
        return -1;
    }
    memcpy(hex, content, SG_SHA1_HEX_LEN);
    hex[SG_SHA1_HEX_LEN] = '\0';
    rc = sg_hex_to_sha1(hex, id_out);
    free(content);
    return rc;
}

int sg_cmd_fetch(int argc, char **argv)
{
    const char *remote = "origin";
    char *git_dir = NULL;
    char *url = NULL;
    sg_ref_adv adv;
    int have_adv = 0;
    unsigned char(*want_ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t want_count = 0;
    unsigned char(*have_ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t have_count = 0;
    int any_updated = 0;
    int rc = 1;
    size_t i;

    if (argc > 2) {
        fprintf(stderr, "usage: sg fetch [<remote>]\n");
        return 1;
    }
    if (argc == 2)
        remote = argv[1];

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    url = sg_repo_read_remote_url(git_dir, remote);
    if (url == NULL) {
        fprintf(stderr, "sg: remote '%s' is not configured (no [remote \"%s\"] url found in .git/config)\n",
               remote, remote);
        goto done;
    }

    if (sg_transport_ls_refs(url, &adv) != 0)
        goto done;
    have_adv = 1;

    if (adv.count == 0) {
        printf("warning: remote is an empty repository\n");
        rc = 0;
        goto done;
    }

    if (build_want_ids(&adv, &want_ids, &want_count) != 0 || want_count == 0) {
        fprintf(stderr, "sg: remote advertised no fetchable branches or tags\n");
        goto done;
    }

    if (build_have_ids(git_dir, &have_ids, &have_count) != 0) {
        fprintf(stderr, "sg: failed to build have list\n");
        goto done;
    }

    {
        unsigned char *pack_data = NULL;
        size_t pack_len = 0;
        char *pack_path = NULL;
        int fetch_rc;

        fetch_rc = sg_transport_fetch_pack(url, want_ids, want_count, have_ids, have_count,
                                           &pack_data, &pack_len);
        if (fetch_rc == 0) {
            if (sg_pack_store_raw(git_dir, pack_data, pack_len, &pack_path) != 0) {
                fprintf(stderr, "sg: failed to store the received pack\n");
                fetch_rc = -1;
            } else if (sg_pack_index_existing(pack_path) != 0) {
                fprintf(stderr, "sg: failed to index the received pack\n");
                fetch_rc = -1;
            }
        }
        free(pack_path);
        free(pack_data);
        if (fetch_rc != 0)
            goto done;
    }

    /* Merge the remote's SG_CHUNK_KEEPALIVE_REF into this repo's own (if it
       advertised one) so the chunk blobs just fetched into the pack above
       stay reachable here too -- otherwise a local `git gc` would
       immediately collect them right back out from under whatever pointer
       blob refers to them. A merge (via sg_chunk_keepalive_merge_commit),
       not an overwrite: this repo may already have its own
       SG_CHUNK_KEEPALIVE_REF protecting chunks from files committed locally
       but not yet pushed anywhere, and clobbering it with the remote's
       would strip those local chunks of their only keep-alive protection --
       exactly the durability bug this phase exists to fix, just replayed on
       the fetch path. keepalive_commit_id and everything its tree
       references are guaranteed present locally at this point: this ref's
       tip was included in want_ids above (build_want_ids), so its objects
       came down in the same pack already stored/indexed. A merge failure is
       only a warning, not a fatal error: the remote's chunk objects are
       already safely on disk from the pack either way, this only affects
       whether they're *also* protected from a local gc, so failing the
       whole fetch over it would be disproportionate. Silent on success (no
       printed line, doesn't affect "Already up to date."): this is internal
       plumbing, not a branch/tag the user asked to fetch. */
    for (i = 0; i < adv.count; i++) {
        if (strcmp(adv.refs[i].name, SG_CHUNK_KEEPALIVE_REF) == 0) {
            if (sg_chunk_keepalive_merge_commit(git_dir, adv.refs[i].id) != 0) {
                fprintf(stderr, "sg: warning: failed to merge remote's %s\n", SG_CHUNK_KEEPALIVE_REF);
            } else if (sg_repo_mark_chunking_used(git_dir) != 0) {
                /* Fatal for the same reason as in cmd_clone.c: a repo holding
                   chunk pointers but missing this marker reads as "never
                   chunked" the moment the keep-alive ref goes away, and
                   chunk_resolve then hands back pointer text as file
                   contents. sg_chunk_store_blob refuses to chunk when it
                   can't write the marker; fetch must not be laxer. */
                fprintf(stderr, "sg: cannot mark chunk storage as used locally in .git/config\n");
                goto done;
            }
            break;
        }
    }

    for (i = 0; i < adv.count; i++) {
        const char *name = adv.refs[i].name;
        char ref_path[SG_PATH_MAX];
        char full_path[SG_PATH_MAX];
        unsigned char old_id[SG_SHA1_RAW_LEN];
        int had_old;
        int is_tag;

        if (strncmp(name, "refs/heads/", 11) == 0) {
            is_tag = 0;
            snprintf(ref_path, sizeof(ref_path), "refs/remotes/%s/%s", remote, name + 11);
        } else if (strncmp(name, "refs/tags/", 10) == 0) {
            /* tags are followed the same way real `git fetch` does by
               default: written straight to the local refs/tags/<tag>, not
               under refs/remotes/ -- otherwise the tag's object (already
               downloaded into the pack, since build_want_ids wants every
               advertised tag tip too) would be orphaned data nothing
               references. This is not "touching a local branch or HEAD",
               which the spec says fetch must leave alone -- tags are
               neither. */
            is_tag = 1;
            snprintf(ref_path, sizeof(ref_path), "%s", name);
        } else {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, ref_path);
        had_old = (read_ref_file(full_path, old_id) == 0);

        if (had_old && memcmp(old_id, adv.refs[i].id, SG_SHA1_RAW_LEN) == 0)
            continue; /* unchanged */

        if (is_tag) {
            /* refs/tags/... is never logged (ref_path_reflog_allowed), so
               passing a message here would just make sg_ref_update fail the
               whole tag update -- keep this NULL, matching every other tag
               write in this codebase. */
            if (sg_ref_update(git_dir, ref_path, adv.refs[i].id, NULL) != 0) {
                fprintf(stderr, "sg: failed to update %s\n", ref_path);
                goto done;
            }
        } else {
            /* Real git's fetch reflog message is the full argv after
               "fetch" (e.g. "fetch -q origin" -> "fetch -q origin: ..."),
               but sg's flags don't match git's, so echoing them back would
               be meaningless -- sg deliberately embeds just the remote name,
               which is enough to make `sg fetch origin` and `git fetch
               origin` produce byte-identical messages (measured against git
               2.55.0). */
            char msg[256];
            const char *result;

            if (!had_old)
                result = "storing head";
            else if (fetch_is_fast_forward(git_dir, old_id, adv.refs[i].id))
                result = "fast-forward";
            else
                result = "forced-update";
            snprintf(msg, sizeof(msg), "fetch %s: %s", remote, result);

            if (sg_ref_update(git_dir, ref_path, adv.refs[i].id, msg) != 0) {
                fprintf(stderr, "sg: failed to update %s\n", ref_path);
                goto done;
            }
        }
        any_updated = 1;

        if (is_tag) {
            const char *tag_name = name + 10;

            if (had_old) {
                char old_hex[SG_SHA1_HEX_LEN + 1];
                char new_hex[SG_SHA1_HEX_LEN + 1];

                sg_sha1_to_hex(old_id, old_hex);
                sg_sha1_to_hex(adv.refs[i].id, new_hex);
                printf("   %.7s..%.7s  %s -> %s\n", old_hex, new_hex, tag_name, tag_name);
            } else {
                printf(" * [new tag]         %s -> %s\n", tag_name, tag_name);
            }
        } else {
            const char *branch_name = name + 11;

            if (had_old) {
                char old_hex[SG_SHA1_HEX_LEN + 1];
                char new_hex[SG_SHA1_HEX_LEN + 1];

                sg_sha1_to_hex(old_id, old_hex);
                sg_sha1_to_hex(adv.refs[i].id, new_hex);
                printf("   %.7s..%.7s  %s -> %s/%s\n", old_hex, new_hex, branch_name, remote,
                      branch_name);
            } else {
                printf(" * [new branch]      %s -> %s/%s\n", branch_name, remote, branch_name);
            }
        }
    }

    if (!any_updated)
        printf("Already up to date.\n");

    rc = 0;

done:
    if (have_adv)
        sg_ref_adv_free(&adv);
    free(want_ids);
    free(have_ids);
    free(url);
    free(git_dir);
    return rc;
}

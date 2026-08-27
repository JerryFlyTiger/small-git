#include "sg/stash.h"

#include "sg/apply.h"
#include "sg/chunk.h"
#include "sg/ignore.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/merge.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/reflog.h"
#include "sg/refs.h"
#include "sg/snapshot.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* Reads commit_id's commit object and fills short_hex_out (7 hex chars +
   NUL) and *subject_out (malloc'd, the first line of the commit message,
   without the newline). Returns 0, -1 on failure. */
static int get_short_and_subject(const char *git_dir, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                                 char short_hex_out[8], char **subject_out)
{
    sg_obj_type type;
    unsigned char *content;
    size_t content_len;
    sg_commit commit;
    char hex[SG_SHA1_HEX_LEN + 1];
    const char *nl;
    size_t subject_len;

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT)
        return -1;

    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return -1;
    }
    free(content);

    sg_sha1_to_hex(commit_id, hex);
    memcpy(short_hex_out, hex, 7);
    short_hex_out[7] = '\0';

    nl = strchr(commit.message, '\n');
    subject_len = (nl != NULL) ? (size_t)(nl - commit.message) : strlen(commit.message);
    *subject_out = malloc(subject_len + 1);
    if (*subject_out == NULL) {
        sg_commit_free(&commit);
        return -1;
    }
    memcpy(*subject_out, commit.message, subject_len);
    (*subject_out)[subject_len] = '\0';

    sg_commit_free(&commit);
    return 0;
}

static int build_and_write_commit(const char *git_dir, const unsigned char tree[SG_SHA1_RAW_LEN],
                                  const unsigned char parents[][SG_SHA1_RAW_LEN], size_t parent_count,
                                  const char *cleaned_message, const char *name, const char *email,
                                  long long when, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    int rc;

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree, SG_SHA1_RAW_LEN);
    commit.parents = NULL;
    if (parent_count > 0) {
        commit.parents = malloc(parent_count * sizeof(*commit.parents));
        if (commit.parents == NULL)
            return -1;
        memcpy(commit.parents, parents, parent_count * sizeof(*commit.parents));
    }
    commit.parent_count = parent_count;
    commit.author_name = (char *)name;
    commit.author_email = (char *)email;
    commit.author_time = when;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)name;
    commit.committer_email = (char *)email;
    commit.committer_time = when;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)cleaned_message;

    if (sg_commit_serialize(&commit, &serialized, &serialized_len) != 0) {
        free(commit.parents);
        return -1;
    }
    free(commit.parents);

    rc = sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, id_out);
    free(serialized);
    return rc;
}

/* Unlinks every path in paths[0..count) under repo_root -- the files that
   just went into the third parent's tree. ENOENT is not an error (the file
   may already be gone for some unrelated reason); anything else is, and
   aborts immediately rather than silently leaving some files behind while
   reporting success. Returns 0, -1 on the first real failure. */
static int remove_untracked_files(const char *repo_root, char **paths, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        char abspath[SG_PATH_MAX];

        if (sg_path_join(abspath, sizeof(abspath), repo_root, paths[i]) != 0)
            return -1;
        if (unlink(abspath) != 0 && errno != ENOENT)
            return -1;
    }
    return 0;
}

/* Post-order: recurses into reldir's subdirectories first (skipping ones
   sg_ignore prunes -- an ignored directory is never entered, matching
   sg_status_list_untracked's own walk, unless include_ignored says to sweep
   it too), then, once every child has had its chance to disappear, removes
   reldir itself if it is now physically empty. reldir == "" (repo_root) is
   never removed. Best-effort: an unreadable directory is left alone rather
   than treated as a hard failure, same convention as
   sg_status_list_untracked's own directory walk. A path that would truncate
   is skipped the same way -- never acted on. This is NOT the same
   convention as status.c's collect_untracked (:202-204,233-239), which
   prints a warning when it has to skip an entry -- collect_untracked's
   silence would under-report real, user-visible untracked files, but here
   the only thing left behind by a skip is an empty directory, which is
   invisible to `sg status` and to real git alike, so there is nothing
   worth warning about. */
static void prune_empty_untracked_dirs(const char *repo_root, const char *reldir, sg_ignore *ig,
                                       int include_ignored)
{
    char absdir[SG_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    int empty;
    int n;

    n = snprintf(absdir, sizeof(absdir), "%s%s%s", repo_root, reldir[0] != '\0' ? "/" : "", reldir);
    if (n < 0 || (size_t)n >= sizeof(absdir))
        return; /* truncated -- never act on a truncated path, same as path_join in status.c */

    d = opendir(absdir);
    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL) {
        char relpath[SG_PATH_MAX];
        char abspath[SG_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strcmp(ent->d_name, ".git") == 0)
            continue;
        n = snprintf(relpath, sizeof(relpath), "%s%s%s", reldir, reldir[0] != '\0' ? "/" : "", ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(relpath))
            continue; /* truncated -- skip this entry entirely, never act on it */
        n = snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, relpath);
        if (n < 0 || (size_t)n >= sizeof(abspath))
            continue; /* truncated -- skip this entry entirely, never act on it */
        if (lstat(abspath, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (!include_ignored && sg_ignore_is_ignored(ig, relpath, 1))
            continue; /* left alone entirely, same as the untracked-file walk */
        if (sg_ignore_push_dir(ig, relpath) != 0)
            continue;
        prune_empty_untracked_dirs(repo_root, relpath, ig, include_ignored);
        sg_ignore_pop_dir(ig);
    }
    closedir(d);

    /* Re-open and recount: children removed above may have made reldir
       itself empty now. */
    d = opendir(absdir);
    if (d == NULL)
        return;
    empty = 1;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        empty = 0;
        break;
    }
    closedir(d);

    if (empty && reldir[0] != '\0')
        rmdir(absdir);
}

/* ---- list ---------------------------------------------------------------- */

int sg_stash_list_read(const char *git_dir, sg_stash_list *out)
{
    sg_reflog log;
    size_t i;

    out->entries = NULL;
    out->count = 0;

    if (sg_reflog_read(git_dir, "refs/stash", &log) != 0)
        return -1;

    if (log.count == 0) {
        sg_reflog_free(&log);
        return 0;
    }

    out->entries = malloc(log.count * sizeof(*out->entries));
    if (out->entries == NULL) {
        sg_reflog_free(&log);
        return -1;
    }

    for (i = 0; i < log.count; i++) {
        const sg_reflog_entry *src = sg_reflog_at(&log, i);

        memcpy(out->entries[i].commit_id, src->new_id, SG_SHA1_RAW_LEN);
        out->entries[i].message = strdup(src->message);
        if (out->entries[i].message == NULL) {
            size_t k;

            for (k = 0; k < i; k++)
                free(out->entries[k].message);
            free(out->entries);
            out->entries = NULL;
            sg_reflog_free(&log);
            return -1;
        }
        out->count = i + 1;
    }

    sg_reflog_free(&log);
    return 0;
}

void sg_stash_list_free(sg_stash_list *list)
{
    size_t i;

    if (list == NULL)
        return;
    for (i = 0; i < list->count; i++)
        free(list->entries[i].message);
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
}

/* ---- spec parsing ---------------------------------------------------------- */

int sg_stash_parse_spec(const char *spec, size_t *index_out)
{
    const char *digits;
    size_t digits_len;
    char buf[32];
    char *endptr;
    unsigned long long value;
    size_t i;

    if (spec == NULL || spec[0] == '\0') {
        *index_out = 0;
        return 0;
    }

    if (strncmp(spec, "stash@{", 7) == 0) {
        size_t len = strlen(spec);

        if (len < 8 || spec[len - 1] != '}')
            return -1;
        digits = spec + 7;
        digits_len = len - 1 - 7;
    } else {
        digits = spec;
        digits_len = strlen(spec);
    }

    if (digits_len == 0 || digits_len >= sizeof(buf))
        return -1;

    for (i = 0; i < digits_len; i++) {
        if (!isdigit((unsigned char)digits[i]))
            return -1;
    }

    memcpy(buf, digits, digits_len);
    buf[digits_len] = '\0';

    errno = 0;
    value = strtoull(buf, &endptr, 10);
    if (errno == ERANGE || *endptr != '\0')
        return -1;
    if (value > (unsigned long long)SIZE_MAX)
        return -1;

    *index_out = (size_t)value;
    return 0;
}

/* ---- push ------------------------------------------------------------------ */

int sg_stash_push(const char *git_dir, const char *repo_root, const sg_stash_push_opts *opts,
                  unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    static const sg_stash_push_opts default_opts = {NULL, 0, 0, 0};
    const char *message;
    int untracked_flag;
    unsigned char head_commit[SG_SHA1_RAW_LEN];
    unsigned char head_tree[SG_SHA1_RAW_LEN];
    sg_index idx;
    unsigned char worktree_tree[SG_SHA1_RAW_LEN];
    unsigned char index_tree[SG_SHA1_RAW_LEN];
    char **untracked_paths = NULL;
    size_t untracked_path_count = 0;
    char *branch = NULL;
    const char *branch_display;
    char short_hex[8];
    char *head_subject = NULL;
    char subj_buf[512];
    char *cleaned_index_msg = NULL;
    char *cleaned_untracked_msg = NULL;
    char *cleaned_subject = NULL;
    unsigned char index_commit_id[SG_SHA1_RAW_LEN];
    unsigned char untracked_commit_id[SG_SHA1_RAW_LEN];
    unsigned char stash_commit_id[SG_SHA1_RAW_LEN];
    unsigned char index_parents[1][SG_SHA1_RAW_LEN];
    unsigned char stash_parents[3][SG_SHA1_RAW_LEN];
    size_t stash_parent_count;
    unsigned char old_stash_id[SG_SHA1_RAW_LEN];
    long long appended_at = 0;
    long long when;
    const char *name;
    const char *email;
    size_t i;
    int rc = -1;

    if (opts == NULL)
        opts = &default_opts;
    message = opts->message;
    untracked_flag = opts->include_untracked || opts->include_ignored;

    if (sg_ref_resolve_head(git_dir, head_commit) != 0)
        return -1; /* unborn HEAD */

    if (sg_commit_tree_of(git_dir, head_commit, head_tree) != 0)
        return -1;

    if (sg_index_read(git_dir, &idx) != 0)
        return -1;

    if (sg_index_has_unmerged(&idx)) {
        sg_index_free(&idx);
        return -1;
    }

    if (sg_tree_build_from_workdir(git_dir, repo_root, &idx, SG_WORKDIR_MISSING_RECORD_DELETION,
                                   worktree_tree) != 0) {
        sg_index_free(&idx);
        return -1;
    }
    if (sg_tree_build_from_index(git_dir, &idx, index_tree) != 0) {
        sg_index_free(&idx);
        return -1;
    }

    if (untracked_flag) {
        if (sg_status_list_untracked(git_dir, repo_root, &idx, opts->include_ignored,
                                     SG_STATUS_UNTRACKED_LIST_FILES, &untracked_paths,
                                     &untracked_path_count) != 0) {
            sg_index_free(&idx);
            return -1;
        }
    }

    if (memcmp(worktree_tree, head_tree, SG_SHA1_RAW_LEN) == 0 &&
       memcmp(index_tree, head_tree, SG_SHA1_RAW_LEN) == 0 &&
       (!untracked_flag || untracked_path_count == 0)) {
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        sg_index_free(&idx);
        return 1; /* nothing to save */
    }

    branch = sg_ref_current_branch(git_dir);
    branch_display = (branch != NULL) ? branch : "(no branch)";

    if (get_short_and_subject(git_dir, head_commit, short_hex, &head_subject) != 0) {
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }

    name = env_or("GIT_AUTHOR_NAME", "small_git");
    email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");
    when = (long long)time(NULL);

    /* index commit (parent 2): always the "index on" form, independent of
       the user-supplied message. */
    snprintf(subj_buf, sizeof(subj_buf), "index on %s: %s %s", branch_display, short_hex, head_subject);
    if (sg_message_cleanup(subj_buf, &cleaned_index_msg) != 0) {
        free(head_subject);
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }

    memcpy(index_parents[0], head_commit, SG_SHA1_RAW_LEN);
    if (build_and_write_commit(git_dir, index_tree, index_parents, 1, cleaned_index_msg, name, email, when,
                               index_commit_id) != 0) {
        free(cleaned_index_msg);
        free(head_subject);
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }
    free(cleaned_index_msg);

    /* untracked commit (parent 3): root commit (no parent of its own), tree
       built from the untracked file list -- unconditionally, even when that
       list is empty (spec sec 1.6), and its subject is ALWAYS the "untracked
       files on" default form, never influenced by -m (spec sec 1.2), so it
       is built here, before head_subject is freed below, rather than
       alongside the stash's own (possibly -m-driven) subject. */
    if (untracked_flag) {
        unsigned char untracked_tree[SG_SHA1_RAW_LEN];

        if (sg_tree_build_from_untracked(git_dir, repo_root, &idx, opts->include_ignored, untracked_tree,
                                         NULL) != 0) {
            free(head_subject);
            for (i = 0; i < untracked_path_count; i++)
                free(untracked_paths[i]);
            free(untracked_paths);
            free(branch);
            sg_index_free(&idx);
            return -1;
        }
        snprintf(subj_buf, sizeof(subj_buf), "untracked files on %s: %s %s", branch_display, short_hex,
                head_subject);
        if (sg_message_cleanup(subj_buf, &cleaned_untracked_msg) != 0) {
            free(head_subject);
            for (i = 0; i < untracked_path_count; i++)
                free(untracked_paths[i]);
            free(untracked_paths);
            free(branch);
            sg_index_free(&idx);
            return -1;
        }
        if (build_and_write_commit(git_dir, untracked_tree, NULL, 0, cleaned_untracked_msg, name, email,
                                   when, untracked_commit_id) != 0) {
            free(cleaned_untracked_msg);
            free(head_subject);
            for (i = 0; i < untracked_path_count; i++)
                free(untracked_paths[i]);
            free(untracked_paths);
            free(branch);
            sg_index_free(&idx);
            return -1;
        }
        free(cleaned_untracked_msg);
    }

    /* stash commit's own subject -- also the reflog message. */
    if (message != NULL)
        snprintf(subj_buf, sizeof(subj_buf), "On %s: %s", branch_display, message);
    else
        snprintf(subj_buf, sizeof(subj_buf), "WIP on %s: %s %s", branch_display, short_hex, head_subject);
    free(head_subject);
    head_subject = NULL;

    if (sg_message_cleanup(subj_buf, &cleaned_subject) != 0) {
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }

    memcpy(stash_parents[0], head_commit, SG_SHA1_RAW_LEN);
    memcpy(stash_parents[1], index_commit_id, SG_SHA1_RAW_LEN);
    stash_parent_count = 2;
    if (untracked_flag) {
        memcpy(stash_parents[2], untracked_commit_id, SG_SHA1_RAW_LEN);
        stash_parent_count = 3;
    }
    if (build_and_write_commit(git_dir, worktree_tree, stash_parents, stash_parent_count, cleaned_subject,
                               name, email, when, stash_commit_id) != 0) {
        free(cleaned_subject);
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }
    free(cleaned_subject);

    if (sg_ref_read_path(git_dir, "refs/stash", old_stash_id) != 0)
        memset(old_stash_id, 0, SG_SHA1_RAW_LEN);

    if (sg_reflog_append(git_dir, "refs/stash", old_stash_id, stash_commit_id, subj_buf, &appended_at) !=
       0) {
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }

    if (sg_ref_write_path(git_dir, "refs/stash", stash_commit_id) != 0) {
        sg_reflog_truncate(git_dir, "refs/stash", appended_at);
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -1;
    }

    /* Destructive from here on: the stash object + ref are durable, so a
       failure past this point leaves the user's work safely recoverable via
       `sg stash apply` even if the working tree reset itself fails. That is
       exactly why these two return -2 instead of -1: the caller must not
       claim "nothing was created" once the stash entry is already listed in
       `sg stash list`. */
    if (sg_snapshot_create(git_dir, repo_root, &idx, "stash push", NULL) != 0) {
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        sg_index_free(&idx);
        return -2;
    }
    sg_index_free(&idx);

    if (sg_apply_tree_to_workdir(git_dir, repo_root, head_tree) != 0) {
        for (i = 0; i < untracked_path_count; i++)
            free(untracked_paths[i]);
        free(untracked_paths);
        free(branch);
        return -2;
    }

    /* --keep-index (measured against real git 2.55.0): after the reset to
       HEAD above, re-apply the index tree on top so staged changes land
       back in both the working tree and the index. This is deliberately
       TWO calls to sg_apply_tree_to_workdir rather than one call straight to
       index_tree: sg_apply_tree_to_workdir only removes a working-tree file
       when it is tracked by the index it reads AT CALL TIME and absent from
       the target tree. A single call with index_tree as the target, using
       the pre-push (staged) index as that baseline, would never delete a
       path staged for removal (e.g. `git rm --cached f` leaves f on disk,
       untracked) -- absent from BOTH the staged index and index_tree, so it
       reads as "not this call's business" and is left behind. Chaining
       through head_tree first makes the second call's baseline HEAD's
       tree (which DOES still list that path), so it gets deleted exactly
       where the index tree also lacks it -- matching real git's own
       "reset to HEAD, then re-apply the staged diff" implementation, and
       the staged-delete row of the measured --keep-index table (Phase 20
       spec). */
    if (opts->keep_index) {
        if (sg_apply_tree_to_workdir(git_dir, repo_root, index_tree) != 0) {
            for (i = 0; i < untracked_path_count; i++)
                free(untracked_paths[i]);
            free(untracked_paths);
            free(branch);
            return -2;
        }
    }

    /* Only after the working tree has settled into its final shape (HEAD's
       tree, or the index tree too under --keep-index) do we take away the
       untracked files that went into the third parent, and prune whatever
       directories that leaves physically empty -- see sg_stash_push_opts's
       header comment for why this does not re-consult ignore status for
       FILES (it does still need it for deciding whether an already-empty
       directory is itself ignored -- see prune_empty_untracked_dirs). The
       ignore engine is opened BEFORE remove_untracked_files runs, not
       after: .gitignore itself is frequently untracked (never committed),
       so it is one of the paths remove_untracked_files can delete under -u.
       Opening the engine first means it still reads the real .gitignore
       content; opening it after would silently see an empty rule set (or a
       stale/partial one) once .gitignore itself is gone, misjudging every
       ignored-and-now-empty directory as not ignored -- exactly what real
       git does NOT do (measured: real git still spares an ignored, empty
       directory under -u even when .gitignore itself was untracked and got
       stashed away by the same push). */
    if (untracked_flag) {
        sg_ignore *ig = NULL;
        int ig_opened = (sg_ignore_open(&ig, git_dir, repo_root) == 0);

        if (remove_untracked_files(repo_root, untracked_paths, untracked_path_count) != 0) {
            if (ig_opened)
                sg_ignore_free(ig);
            for (i = 0; i < untracked_path_count; i++)
                free(untracked_paths[i]);
            free(untracked_paths);
            free(branch);
            return -2;
        }
        if (ig_opened) {
            prune_empty_untracked_dirs(repo_root, "", ig, opts->include_ignored);
            sg_ignore_free(ig);
        }
    }

    for (i = 0; i < untracked_path_count; i++)
        free(untracked_paths[i]);
    free(untracked_paths);

    memcpy(commit_id_out, stash_commit_id, SG_SHA1_RAW_LEN);
    free(branch);
    rc = 0;
    return rc;
}

/* ---- apply ------------------------------------------------------------------ */

static int add_stage_entry(sg_index *idx, const char *path, unsigned int stage, unsigned int mode,
                           const unsigned char sha1[SG_SHA1_RAW_LEN])
{
    sg_index_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.mode = mode;
    entry.stage = stage;
    memcpy(entry.sha1, sha1, SG_SHA1_RAW_LEN);
    entry.path = (char *)path;
    return sg_index_upsert(idx, &entry);
}

static const sg_flat_entry *flat_find(const sg_flat_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return &list->entries[i];
    }
    return NULL;
}

static int path_is_conflict(const sg_merge_result *result, const char *path)
{
    size_t i;

    for (i = 0; i < result->count; i++) {
        if (strcmp(result->entries[i].path, path) == 0)
            return result->entries[i].conflict;
    }
    return 0;
}

/* Writes every entry in flat to disk under repo_root, chunk-aware, exactly
   the way sg_apply_tree_to_workdir writes a tracked tree -- but these paths
   are never staged (see sg_stash_apply's header comment: the untracked half
   of a -u/-a stash is restored as untracked files, not index entries).
   Returns 0, -1 on the first unreadable blob or write failure -- leaves
   whatever was already written on disk, same no-rollback convention as
   sg_apply_tree_to_workdir. The pre-flight collision check the caller runs
   first means a failure here is a genuine I/O error, not a pre-existing
   file at the target path. */
static int restore_untracked_flat(const char *git_dir, const char *repo_root, const sg_flat_list *flat)
{
    size_t i;

    for (i = 0; i < flat->count; i++) {
        char abspath[SG_PATH_MAX];
        unsigned char *blob_content;
        size_t blob_len;
        sg_chunk_missing_info missing;
        int read_rc;

        if (sg_path_join(abspath, sizeof(abspath), repo_root, flat->entries[i].path) != 0)
            return -1;
        read_rc = sg_chunk_read_blob(git_dir, flat->entries[i].sha1, &blob_content, &blob_len, &missing);
        if (read_rc == -2) {
            sg_chunk_print_missing_error(flat->entries[i].path, &missing);
            return -1;
        }
        if (read_rc != 0) {
            fprintf(stderr, "sg: missing blob for %s\n", sg_quote_path_delimited(flat->entries[i].path));
            return -1;
        }
        if (sg_write_file_mkdirs(abspath, blob_content, blob_len, (int)(flat->entries[i].mode & 0777)) !=
           0) {
            fprintf(stderr, "sg: failed to write %s\n", sg_quote_path_delimited(flat->entries[i].path));
            free(blob_content);
            return -1;
        }
        free(blob_content);
    }
    return 0;
}

/* sg_stash_trees / sg_stash_load_trees are declared in sg/stash.h -- shared
   by sg_stash_apply, sg_stash_apply_check_dirty and `sg stash show` so the
   three never drift on what counts as a valid stash-shaped commit (parent
   count 2 or 3). */
int sg_stash_load_trees(const char *git_dir, size_t index, sg_stash_trees *out)
{
    sg_stash_list list;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;
    sg_commit stash_commit;

    /* has_untracked == 0 leaves untracked_tree untouched below (it is only
       ever written for a 3-parent stash) -- zero it up front so a caller
       that someday forgets to check has_untracked first reads 20 zero
       bytes, not uninitialized stack garbage. There is no sanitizer for
       this project that would ever catch the alternative: ASan does not
       flag a pure read of an uninitialized value the way it flags a
       use-after-free or an out-of-bounds access, and this project has no
       MemorySanitizer build. */
    memset(out, 0, sizeof(*out));

    if (sg_stash_list_read(git_dir, &list) != 0)
        return -1;
    if (index >= list.count) {
        sg_stash_list_free(&list);
        return -1;
    }
    memcpy(commit_id, list.entries[index].commit_id, SG_SHA1_RAW_LEN);
    sg_stash_list_free(&list);

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
        free(content);
        return -1;
    }
    if (sg_commit_parse(content, content_len, &stash_commit) != 0) {
        free(content);
        return -1;
    }
    free(content);

    if (stash_commit.parent_count != 2 && stash_commit.parent_count != 3) {
        sg_commit_free(&stash_commit);
        return -1;
    }

    if (sg_commit_tree_of(git_dir, stash_commit.parents[0], out->base_tree) != 0) {
        sg_commit_free(&stash_commit);
        return -1;
    }
    if (sg_commit_tree_of(git_dir, stash_commit.parents[1], out->index_tree) != 0) {
        sg_commit_free(&stash_commit);
        return -1;
    }
    memcpy(out->theirs_tree, stash_commit.tree, SG_SHA1_RAW_LEN);
    out->has_untracked = (stash_commit.parent_count == 3);
    if (out->has_untracked && sg_commit_tree_of(git_dir, stash_commit.parents[2], out->untracked_tree) != 0) {
        sg_commit_free(&stash_commit);
        return -1;
    }
    sg_commit_free(&stash_commit);
    return 0;
}

/* Finds path's entry in result, or NULL. */
static const sg_merge_result_entry *result_find(const sg_merge_result *result, const char *path)
{
    size_t i;

    for (i = 0; i < result->count; i++) {
        if (strcmp(result->entries[i].path, path) == 0)
            return &result->entries[i];
    }
    return NULL;
}

/* True if the merge's outcome at path actually TOUCHES it -- i.e. differs
   from what ours (HEAD's tree) already has there -- as opposed to carrying
   it through unchanged (base/ours/theirs all agree). A thin wrapper around
   sg_merge_entry_touches_ours (merge.h): as of Phase 20,
   sg_merge_result_apply itself skips rewriting a path whose outcome equals
   ours, so this now answers exactly the same question that function asks
   internally. Shared by sg_stash_apply_check_dirty's dirty-workdir gate
   (spec sec 4.3) and sg_stash_apply's own index-restaging rule (spec sec
   4.4): the two must agree on what "touched" means, or the gate could let a
   path through that the restage step then clobbers, or refuse a path the
   restage step would have left alone anyway. Returns 0 (not touched) for a
   path absent from result -- sg_merge_trees only omits a path when none of
   base/ours/theirs has it, which can't happen for anything in head_flat. */
static int path_is_touched(const sg_merge_result *result, const char *path)
{
    const sg_merge_result_entry *re = result_find(result, path);

    if (re == NULL)
        return 0;
    return sg_merge_entry_touches_ours(re);
}

/* Checks whether apply/pop of stash entry `index` would be forced to
   overwrite an uncommitted change (Phase 20 spec sec 4.2/4.3): the
   replacement for the old blanket sg_require_clean_workdir gate. Only paths
   the merge actually TOUCHES (path_is_touched) are examined, and only two of
   the three ways a touched path can collide are checked here:

     - the working tree still has the path and its content differs from HEAD
       (spec rows 2/3/4 -- row 3, content that happens to already equal what
       the stash would write, is included: the rule looks at whether the
       path differs from HEAD, not whether it differs from the stash).
     - the index already differs from HEAD at that path (row 8) -- sg's
       three-way merge uses HEAD as "ours", not the index the way real git
       does, so letting a staged change through here would silently overwrite
       it. This is a DELIBERATE DIVERGENCE from real git (which 3-way-merges
       against the index and can produce a conflict instead); see
       sg_stash_apply's header comment in sg/stash.h.

   Row 5 (the working-tree file is simply gone) is deliberately NOT flagged:
   nothing is there to overwrite. Row 6 (an untracked file blocking a path
   the stash would newly CREATE, i.e. hf == NULL here) is intentionally left
   to sg_stash_apply's own pre-flight collision check rather than duplicated
   here -- that check already exists, has its own message, and covers both
   the tracked and the -u/-a untracked half.

   On success, *dirty_paths_out is a malloc'd, path-sorted array of malloc'd
   path strings (the same order sg_merge_trees produced), *dirty_count_out
   its length; both are set to NULL/0 when nothing collided. Returns 0 when
   it is safe to proceed, 1 when at least one path collided, -1 on error
   (same causes as sg_stash_apply: bad index, out-of-range index, corrupt
   stash commit, unreadable tree/object, allocation failure). */
int sg_stash_apply_check_dirty(const char *git_dir, const char *repo_root, size_t index,
                               char ***dirty_paths_out, size_t *dirty_count_out)
{
    sg_stash_trees trees;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    sg_merge_result result;
    sg_flat_list head_flat;
    sg_index idx;
    char **dirty = NULL;
    size_t dirty_count = 0;
    size_t i;
    int rc = 0;

    *dirty_paths_out = NULL;
    *dirty_count_out = 0;

    if (sg_stash_load_trees(git_dir, index, &trees) != 0)
        return -1;

    if (sg_ref_resolve_head(git_dir, head_commit_id) != 0)
        return -1;
    if (sg_commit_tree_of(git_dir, head_commit_id, ours_tree) != 0)
        return -1;

    if (sg_merge_trees(git_dir, trees.base_tree, ours_tree, trees.theirs_tree, "Updated upstream",
                       "Stashed changes", &result) != 0)
        return -1;

    {
        char bad_path[SG_PATH_MAX];
        int flatten_rc = sg_tree_flatten(git_dir, ours_tree, &head_flat, bad_path);

        if (flatten_rc == -2)
            fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into "
                            "file paths\n",
                   sg_quote_path_delimited(bad_path));
        if (flatten_rc != 0) {
            sg_merge_result_free(&result);
            return -1;
        }
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        sg_flat_list_free(&head_flat);
        sg_merge_result_free(&result);
        return -1;
    }

    for (i = 0; i < result.count && rc == 0; i++) {
        const char *path = result.entries[i].path;
        const sg_flat_entry *hf = NULL;
        size_t hi;
        int dirty_here = 0;

        if (!path_is_touched(&result, path))
            continue;

        for (hi = 0; hi < head_flat.count; hi++) {
            if (strcmp(head_flat.entries[hi].path, path) == 0) {
                hf = &head_flat.entries[hi];
                break;
            }
        }

        /* Row 8. */
        {
            int idxpos = sg_index_find_stage(&idx, path, 0);

            if (idxpos < 0) {
                if (hf != NULL)
                    dirty_here = 1; /* staged delete of a HEAD path the stash touches */
            } else if (hf == NULL || memcmp(idx.entries[idxpos].sha1, hf->sha1, SG_SHA1_RAW_LEN) != 0 ||
                      idx.entries[idxpos].mode != hf->mode) {
                dirty_here = 1;
            }
        }

        /* Rows 2/3/4. */
        if (!dirty_here && hf != NULL) {
            char abspath[SG_PATH_MAX];
            struct stat st;

            /* A truncated path can't be verified clean, and this is a gate:
               the conservative answer is dirty, never clean. */
            if (sg_path_join(abspath, sizeof(abspath), repo_root, path) != 0) {
                dirty_here = 1;
            } else if (lstat(abspath, &st) == 0) {
                unsigned char wd_sha1[SG_SHA1_RAW_LEN];

                if (sg_hash_file_blob(abspath, wd_sha1) != 0 ||
                   memcmp(wd_sha1, hf->sha1, SG_SHA1_RAW_LEN) != 0)
                    dirty_here = 1;
            }
        }

        if (dirty_here) {
            char **grown = realloc(dirty, (dirty_count + 1) * sizeof(*grown));

            if (grown == NULL) {
                rc = -1;
                break;
            }
            dirty = grown;
            dirty[dirty_count] = strdup(path);
            if (dirty[dirty_count] == NULL) {
                rc = -1;
                break;
            }
            dirty_count++;
        }
    }

    sg_index_free(&idx);
    sg_flat_list_free(&head_flat);
    sg_merge_result_free(&result);

    if (rc != 0) {
        for (i = 0; i < dirty_count; i++)
            free(dirty[i]);
        free(dirty);
        return -1;
    }

    *dirty_paths_out = dirty;
    *dirty_count_out = dirty_count;
    return dirty_count > 0 ? 1 : 0;
}

int sg_stash_apply(const char *git_dir, const char *repo_root, size_t index, int restore_index)
{
    sg_stash_trees trees;
    unsigned char base_tree[SG_SHA1_RAW_LEN];
    unsigned char ours_tree[SG_SHA1_RAW_LEN];
    unsigned char theirs_tree[SG_SHA1_RAW_LEN];
    unsigned char untracked_tree[SG_SHA1_RAW_LEN];
    int has_untracked = 0;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    sg_merge_result result;
    sg_flat_list head_flat;
    sg_flat_list untracked_flat;
    sg_index orig_idx;
    sg_index new_idx;
    char **conflict_paths = NULL;
    size_t conflict_count = 0;
    size_t i;
    int untracked_collision = 0;

    memset(&untracked_flat, 0, sizeof(untracked_flat));

    if (sg_stash_load_trees(git_dir, index, &trees) != 0)
        return -1;
    memcpy(base_tree, trees.base_tree, SG_SHA1_RAW_LEN);
    memcpy(theirs_tree, trees.theirs_tree, SG_SHA1_RAW_LEN);
    has_untracked = trees.has_untracked;
    if (has_untracked)
        memcpy(untracked_tree, trees.untracked_tree, SG_SHA1_RAW_LEN);

    if (sg_ref_resolve_head(git_dir, head_commit_id) != 0)
        return -1;
    if (sg_commit_tree_of(git_dir, head_commit_id, ours_tree) != 0)
        return -1;

    if (sg_merge_trees(git_dir, base_tree, ours_tree, theirs_tree, "Updated upstream", "Stashed changes",
                       &result) != 0)
        return -1;

    {
        char bad_path[SG_PATH_MAX];
        int flatten_rc = sg_tree_flatten(git_dir, ours_tree, &head_flat, bad_path);

        if (flatten_rc == -2)
            fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into "
                            "file paths\n",
                   sg_quote_path_delimited(bad_path));
        if (flatten_rc != 0) {
            sg_merge_result_free(&result);
            return -1;
        }
    }

    if (has_untracked) {
        char bad_path[SG_PATH_MAX];
        int flatten_rc = sg_tree_flatten(git_dir, untracked_tree, &untracked_flat, bad_path);

        if (flatten_rc == -2)
            fprintf(stderr, "sg: path %s is invalid, refusing to flatten this tree into "
                            "file paths\n",
                   sg_quote_path_delimited(bad_path));
        if (flatten_rc != 0) {
            sg_flat_list_free(&head_flat);
            sg_merge_result_free(&result);
            return -1;
        }
    }

    /* Pre-flight: a merge-result path missing from HEAD must not already
       exist, untracked, in the working directory -- sg_require_clean_workdir
       (the caller's earlier gate) only looks at tracked paths. */
    for (i = 0; i < result.count && !untracked_collision; i++) {
        sg_merge_result_entry *e = &result.entries[i];

        if (e->conflict || e->deleted)
            continue;
        if (flat_find(&head_flat, e->path) == NULL) {
            char abspath[SG_PATH_MAX];
            struct stat st;

            /* A truncated path can't be verified collision-free -- this is
               a pre-flight gate, so the conservative answer is "collides",
               never "clear". */
            if (sg_path_join(abspath, sizeof(abspath), repo_root, e->path) != 0)
                untracked_collision = 1;
            else if (lstat(abspath, &st) == 0)
                untracked_collision = 1;
        }
    }
    /* Same pre-flight, extended to the untracked half (see sg_stash_apply's
       header comment for why sg deliberately refuses the WHOLE apply here
       instead of real git's partial-apply behavior). */
    for (i = 0; i < untracked_flat.count && !untracked_collision; i++) {
        char abspath[SG_PATH_MAX];
        struct stat st;

        if (sg_path_join(abspath, sizeof(abspath), repo_root, untracked_flat.entries[i].path) != 0) {
            untracked_collision = 1;
            continue;
        }
        if (lstat(abspath, &st) == 0)
            untracked_collision = 1;
    }
    if (untracked_collision) {
        sg_flat_list_free(&untracked_flat);
        sg_flat_list_free(&head_flat);
        sg_merge_result_free(&result);
        return -1;
    }

    /* Materializes the merge result into the working tree and a fresh index.
       As of Phase 20, sg_merge_result_apply itself skips rewriting any path
       whose outcome equals ours (HEAD's tree) -- see its header comment in
       merge.h -- so a dirty-but-untouched working-tree path is left alone
       here without this caller needing to snapshot/restore anything around
       the call. A -1 here means either a chunked blob's data was
       unrecoverable or the index could not be built completely. */
    if (sg_merge_result_apply(git_dir, repo_root, &result, &new_idx, &conflict_paths, &conflict_count) != 0) {
        sg_flat_list_free(&untracked_flat);
        sg_flat_list_free(&head_flat);
        sg_merge_result_free(&result);
        return -1;
    }

    /* Snapshot of the on-disk index exactly as it stood before this apply/
       pop touched anything -- needed by the re-stage loop below (Phase 20
       spec sec 4.4) to tell "untouched by the merge" paths apart from paths
       the merge actually changed. Reading it here (nothing above this point
       writes to the index) is equivalent to reading it at function entry. */
    if (sg_index_read(git_dir, &orig_idx) != 0) {
        size_t j;

        sg_flat_list_free(&untracked_flat);
        sg_flat_list_free(&head_flat);
        sg_index_free(&new_idx);
        sg_merge_result_free(&result);
        for (j = 0; j < conflict_count; j++)
            free(conflict_paths[j]);
        free(conflict_paths);
        return -1;
    }

    /* Default pop index rule (measured): for every HEAD path the merge
       actually TOUCHES (path_is_touched -- spec sec 4.3/4.4), the index ends
       up equal to HEAD's own version there, except that a path the merge
       result introduced but HEAD never had stays staged. path_is_conflict
       consults the merge result (not the index), so re-staging HEAD's
       version is correct even for a path sg_merge_result_apply left with no
       stage-0 entry at all (HEAD had it, the stash cleanly deleted it:
       re-staging it here is exactly the "unstaged deletion" real git leaves
       behind). A conflicted path must carry ONLY the stage 1/2/3 entries
       sg_merge_result_apply already wrote, never a stage-0 one alongside
       them, so those are skipped entirely.

       For a HEAD path the merge does NOT touch, sg_merge_result_apply above
       still wrote a stage-0 entry for it (built purely from the tree merge,
       which agrees with HEAD when nothing changed) -- but that ignores
       whatever was ALREADY staged there before this apply/pop ran. Once the
       dirty-workdir gate stopped requiring a perfectly clean index (spec sec
       4.2 row 7), that staged change must survive: restore orig_idx's own
       stage-0 entry for the path instead of HEAD's, or drop the path
       entirely if orig_idx did not have it either (a pre-existing staged
       delete on an untouched path stays deleted, not resurrected). Under the
       old clean-workdir precondition orig_idx's stage-0 entry always equaled
       HEAD's own, so this is a no-op there -- not a special case. */
    for (i = 0; i < head_flat.count; i++) {
        const char *path = head_flat.entries[i].path;

        if (path_is_conflict(&result, path))
            continue;

        if (path_is_touched(&result, path)) {
            if (add_stage_entry(&new_idx, path, 0, head_flat.entries[i].mode, head_flat.entries[i].sha1) !=
               0) {
                size_t j;

                sg_flat_list_free(&untracked_flat);
                sg_flat_list_free(&head_flat);
                sg_index_free(&orig_idx);
                sg_index_free(&new_idx);
                sg_merge_result_free(&result);
                for (j = 0; j < conflict_count; j++)
                    free(conflict_paths[j]);
                free(conflict_paths);
                return -1;
            }
        } else {
            int oi = sg_index_find_stage(&orig_idx, path, 0);

            if (oi >= 0) {
                if (add_stage_entry(&new_idx, path, 0, orig_idx.entries[oi].mode,
                                    orig_idx.entries[oi].sha1) != 0) {
                    size_t j;

                    sg_flat_list_free(&untracked_flat);
                    sg_flat_list_free(&head_flat);
                    sg_index_free(&orig_idx);
                    sg_index_free(&new_idx);
                    sg_merge_result_free(&result);
                    for (j = 0; j < conflict_count; j++)
                        free(conflict_paths[j]);
                    free(conflict_paths);
                    return -1;
                }
            } else {
                sg_index_remove(&new_idx, path);
            }
        }
    }

    /* Second half of the same "untouched things survive" rule (Phase 20
       spec sec 4.4, error 2): a stage-0 orig_idx entry for a path HEAD never
       had at all is invisible to the loop above -- head_flat only lists
       HEAD's own paths -- and, when the stash's own change never touches
       that path either, sg_merge_trees never emits an entry for it in
       result at all (base/ours/theirs all agree there is nothing there), so
       sg_merge_result_apply never staged it in new_idx either. Without this
       pass a staged-new file whose path the stash never touches would
       silently lose its "A " status. Skip any path new_idx already carries
       at stage 0 (already handled above) and any path the merge left
       conflicted (already carries stage 1/2/3, never stage 0 alongside
       them). */
    for (i = 0; i < orig_idx.count; i++) {
        const sg_index_entry *oe = &orig_idx.entries[i];

        if (oe->stage != 0)
            continue;
        if (sg_index_find_stage(&new_idx, oe->path, 0) >= 0)
            continue;
        if (path_is_conflict(&result, oe->path))
            continue;
        if (add_stage_entry(&new_idx, oe->path, 0, oe->mode, oe->sha1) != 0) {
            size_t j;

            sg_index_free(&orig_idx);
            sg_flat_list_free(&head_flat);
            sg_flat_list_free(&untracked_flat);
            sg_index_free(&new_idx);
            sg_merge_result_free(&result);
            for (j = 0; j < conflict_count; j++)
                free(conflict_paths[j]);
            free(conflict_paths);
            return -1;
        }
    }
    sg_index_free(&orig_idx);
    sg_flat_list_free(&head_flat);

    /* Untracked half: written to disk, never staged (see sg_stash_apply's
       header comment). The pre-flight collision check above already ruled
       out a pre-existing file at any of these paths. */
    if (has_untracked && restore_untracked_flat(git_dir, repo_root, &untracked_flat) != 0) {
        sg_flat_list_free(&untracked_flat);
        sg_index_free(&new_idx);
        sg_merge_result_free(&result);
        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
        return -1;
    }
    sg_flat_list_free(&untracked_flat);

    if (sg_index_write(git_dir, &new_idx) != 0) {
        sg_index_free(&new_idx);
        sg_merge_result_free(&result);
        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);
        return -1;
    }
    sg_index_free(&new_idx);
    sg_merge_result_free(&result);

    {
        int has_conflict = conflict_count > 0;

        for (i = 0; i < conflict_count; i++)
            free(conflict_paths[i]);
        free(conflict_paths);

        /* --index (spec sec 3): restores the index to the stash's own index
           tree (parents[1]), entirely replacing whatever the rules above
           just staged. Only on a clean merge -- real git (measured) skips
           this step outright on conflict and leaves the index exactly as
           the no-"--index" rules above already left it; the caller is
           responsible for telling the user (see sg/stash.h). No no-staged-
           changes special case is needed: when trees.index_tree equals
           trees.base_tree, resetting to it is simply a no-op alongside
           whatever the rules above already produced from the same base. */
        if (restore_index && !has_conflict) {
            if (sg_index_reset_to_tree(git_dir, trees.index_tree) != 0)
                return -1;
        }

        return has_conflict ? 1 : 0;
    }
}

/* ---- drop / clear ------------------------------------------------------------ */

int sg_stash_drop(const char *git_dir, size_t index)
{
    sg_reflog log;
    sg_reflog_entry *new_entries;
    size_t new_count;
    size_t remove_pos;
    size_t i, j;
    int rc;

    if (sg_reflog_read(git_dir, "refs/stash", &log) != 0)
        return -1;
    if (index >= log.count) {
        sg_reflog_free(&log);
        return -1;
    }

    remove_pos = log.count - 1 - index; /* stash@{index} -> file-order position */
    new_count = log.count - 1;

    if (new_count == 0) {
        if (sg_reflog_rewrite(git_dir, "refs/stash", NULL, 0) != 0) {
            sg_reflog_free(&log);
            return -1;
        }
        if (sg_ref_delete_under(git_dir, "refs/", "stash") == -1) {
            /* Best effort: put the reflog we just wiped back so refs/stash
               (still pointing at the pre-drop tip) doesn't end up without
               the reflog line backing it -- the "log for ref refs/stash
               unexpectedly ended" state the header warns about. */
            sg_reflog_rewrite(git_dir, "refs/stash", log.entries, log.count);
            sg_reflog_free(&log);
            return -1;
        }
        sg_reflog_free(&log);
        return 0;
    }

    new_entries = malloc(new_count * sizeof(*new_entries));
    if (new_entries == NULL) {
        sg_reflog_free(&log);
        return -1;
    }
    for (i = 0, j = 0; i < log.count; i++) {
        if (i == remove_pos)
            continue;
        new_entries[j++] = log.entries[i]; /* borrows ident/message from log */
    }

    rc = sg_reflog_rewrite(git_dir, "refs/stash", new_entries, new_count);
    if (rc == 0) {
        rc = sg_ref_write_path(git_dir, "refs/stash", new_entries[new_count - 1].new_id);
        if (rc != 0) {
            /* refs/stash still points at the pre-drop tip; restore the
               original reflog (still in `log`) so the tip invariant holds
               instead of leaving a rewritten reflog the ref disagrees with. */
            sg_reflog_rewrite(git_dir, "refs/stash", log.entries, log.count);
        }
    }

    free(new_entries);
    sg_reflog_free(&log);
    return rc;
}

int sg_stash_clear(const char *git_dir)
{
    sg_reflog log;
    int have_log = (sg_reflog_read(git_dir, "refs/stash", &log) == 0);

    if (sg_reflog_rewrite(git_dir, "refs/stash", NULL, 0) != 0) {
        if (have_log)
            sg_reflog_free(&log);
        return -1;
    }
    if (sg_ref_delete_under(git_dir, "refs/", "stash") == -1) {
        /* Best effort: put the reflog we just wiped back so refs/stash
           (still pointing at the pre-clear tip) isn't left without the
           reflog line backing it. Skipped when we could not read the
           original reflog in the first place (have_log == 0) -- there is
           nothing to restore. */
        if (have_log && log.count > 0)
            sg_reflog_rewrite(git_dir, "refs/stash", log.entries, log.count);
        if (have_log)
            sg_reflog_free(&log);
        return -1;
    }
    if (have_log)
        sg_reflog_free(&log);
    return 0;
}

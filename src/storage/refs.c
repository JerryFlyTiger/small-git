#include "sg/refs.h"

#include "sg/reflog.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SG_PATH_MAX 4096
#define HEAD_PREFIX "ref: "
#define BRANCH_PREFIX "refs/heads/"

int sg_ref_branch_name_is_safe(const char *name)
{
    if (name[0] == '\0' || name[0] == '/')
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    return 1;
}

int sg_ref_name_valid_for_create(const char *name)
{
    size_t len = strlen(name);
    size_t i;

    if (len == 0)
        return 0;
    if (name[0] == '-' || name[0] == '/')
        return 0;
    if (name[len - 1] == '/' || name[len - 1] == '.')
        return 0;
    if (strcmp(name, "HEAD") == 0)
        return 0;
    if (len >= 5 && strcmp(name + len - 5, ".lock") == 0)
        return 0;
    if (strstr(name, "//") != NULL || strstr(name, "..") != NULL ||
       strstr(name, "@{") != NULL)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == 0x7f || c == ' ' || c == '\\' || c == '~' ||
           c == '^' || c == ':' || c == '?' || c == '*' || c == '[')
            return 0;
    }
    return 1;
}

static char *read_small_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t cap = 256;
    size_t used = 0;

    if (f == NULL)
        return NULL;

    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    for (;;) {
        size_t n;

        if (used == cap) {
            size_t new_cap = cap * 2;
            char *grown = realloc(buf, new_cap);

            if (grown == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
            cap = new_cap;
        }
        n = fread(buf + used, 1, cap - used, f);
        used += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    buf[used] = '\0'; /* cap always leaves room: used < cap right after growth check, or grows */
    if (used == cap) {
        char *grown = realloc(buf, cap + 1);

        if (grown == NULL) {
            free(buf);
            return NULL;
        }
        buf = grown;
        buf[used] = '\0';
    }
    return buf;
}

char *sg_ref_current_branch(const char *git_dir)
{
    char path[SG_PATH_MAX];
    char *content;
    char *nl;
    char *branch;

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    content = read_small_file(path);
    if (content == NULL)
        return NULL;

    if (strncmp(content, HEAD_PREFIX, strlen(HEAD_PREFIX)) != 0) {
        free(content);
        return NULL;
    }

    nl = strchr(content, '\n');
    if (nl != NULL)
        *nl = '\0';

    {
        const char *ref = content + strlen(HEAD_PREFIX);

        if (strncmp(ref, BRANCH_PREFIX, strlen(BRANCH_PREFIX)) != 0) {
            free(content);
            return NULL;
        }
        branch = strdup(ref + strlen(BRANCH_PREFIX));
    }
    free(content);
    return branch;
}

/* defined below, next to sg_ref_read_path -- `git gc` packs refs away
   from their loose files, and a lookup that misses that concludes the
   branch has no commits, which turns the next commit into a root commit
   and orphans the whole history. */
static int read_packed_ref(const char *git_dir, const char *ref_path,
                           unsigned char id_out[SG_SHA1_RAW_LEN]);

int sg_ref_read_branch(const char *git_dir, const char *branch, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char ref_name[SG_PATH_MAX];
    char *content;
    char *nl;
    int rc;

    if (!sg_ref_branch_name_is_safe(branch))
        return -1;

    /* branch comes from argv (or, since resolve_base's phase12 addition, a
       user-supplied rev-parse base): a name long enough to truncate either
       path below would make this look up some OTHER branch/ref, so treat
       truncation the same as "not found" rather than silently reading the
       wrong one. */
    if (snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch) >= (int)sizeof(path))
        return -1;
    content = read_small_file(path);
    if (content == NULL) {
        if (snprintf(ref_name, sizeof(ref_name), "refs/heads/%s", branch) >= (int)sizeof(ref_name))
            return -1;
        return read_packed_ref(git_dir, ref_name, id_out);
    }

    nl = strchr(content, '\n');
    if (nl != NULL)
        *nl = '\0';

    rc = sg_hex_to_sha1(content, id_out);
    free(content);
    return rc;
}

int sg_ref_head_is_detached(const char *git_dir)
{
    char path[SG_PATH_MAX];
    char *content;
    char *nl;
    unsigned char id[SG_SHA1_RAW_LEN];
    int rc;

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    content = read_small_file(path);
    if (content == NULL)
        return -1;

    if (strncmp(content, HEAD_PREFIX, strlen(HEAD_PREFIX)) == 0) {
        free(content);
        return 0;
    }

    nl = strchr(content, '\n');
    if (nl != NULL)
        *nl = '\0';

    /* Anything that is neither "ref: ..." nor a well-formed raw object id is
       a corrupt HEAD, and must NOT be reported as detached: callers use a
       detached answer to decide they may overwrite HEAD with a raw id, which
       would quietly launder the corruption into a valid-looking state. */
    rc = sg_hex_to_sha1(content, id) == 0 ? 1 : -1;
    free(content);
    return rc;
}

int sg_ref_resolve_head(const char *git_dir, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char *branch = sg_ref_current_branch(git_dir);
    int rc;

    /* No branch name means either a detached HEAD (a raw object id, which
       sg_ref_read_path parses directly) or a HEAD we cannot make sense of.
       Before Phase 18 both collapsed into -1 here, which every caller then
       read as "this repo has no commits yet" -- so on a detached checkout
       `sg commit` believed it was making a root commit, `sg status` printed
       "No commits yet", and sg_safe_apply_tree compared the work tree
       against an empty tree. Resolving the raw id keeps -1 meaning ONLY
       "unborn HEAD", which is what the header promises callers. */
    if (branch == NULL)
        return sg_ref_read_path(git_dir, "HEAD", id_out);
    rc = sg_ref_read_branch(git_dir, branch, id_out);
    free(branch);
    return rc;
}

int sg_ref_update_branch(const char *git_dir, const char *branch, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char ref_path[SG_PATH_MAX];

    if (!sg_ref_branch_name_is_safe(branch))
        return -1;
    if (snprintf(ref_path, sizeof(ref_path), "%s%s", BRANCH_PREFIX, branch) >= (int)sizeof(ref_path))
        return -1;

    return sg_ref_update(git_dir, ref_path, id, NULL);
}

int sg_ref_branch_exists(const char *git_dir, const char *branch)
{
    char path[SG_PATH_MAX];
    struct stat st;
    unsigned char id[SG_SHA1_RAW_LEN];

    if (!sg_ref_branch_name_is_safe(branch))
        return 0;

    snprintf(path, sizeof(path), "%s/refs/heads/%s", git_dir, branch);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return 1;

    /* a packed branch is still a branch -- see read_packed_ref */
    return sg_ref_read_branch(git_dir, branch, id) == 0;
}

/* The actual ref-file writer, shared by sg_ref_write_path and sg_ref_update
   (which both, ultimately, only ever write a ref through here) -- named
   separately so sg_ref_update's NULL-message fast path can call it directly
   instead of going through sg_ref_write_path, which would recurse right
   back into sg_ref_update. */
static int write_ref_path_raw(const char *git_dir, const char *ref_path,
                              const unsigned char id[SG_SHA1_RAW_LEN])
{
    char full_path[SG_PATH_MAX];
    char content[SG_SHA1_HEX_LEN + 2];

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, ref_path);
    sg_sha1_to_hex(id, content);
    content[SG_SHA1_HEX_LEN] = '\n';
    content[SG_SHA1_HEX_LEN + 1] = '\0';
    return sg_write_file_mkdirs(full_path, (const unsigned char *)content, SG_SHA1_HEX_LEN + 1, 0644);
}

int sg_ref_write_path(const char *git_dir, const char *ref_path, const unsigned char id[SG_SHA1_RAW_LEN])
{
    return sg_ref_update(git_dir, ref_path, id, NULL);
}

/* Parses git's packed-refs file (a plain "<40-hex-sha1> <ref-name>\n" list,
   with an optional leading '#'-comment header line and, for annotated tags
   only, '^'-prefixed peeled-object lines following the ref they annotate)
   looking for exactly ref_path. `git gc` (via the `git pack-refs` step it
   runs) routinely moves refs out of their loose per-ref file and into this
   single file instead -- including SG_CHUNK_KEEPALIVE_REF, since by design
   it's an ordinary ref from git's own perspective (see that ref's doc
   comment in chunk.h) -- so a reader that only ever checks the loose path
   would start reporting "ref doesn't exist" right after a real `git gc`,
   even though the ref is still there. Returns 0 with id_out filled in if
   found, -1 if packed-refs is missing/unreadable or doesn't list ref_path. */
static int read_packed_ref(const char *git_dir, const char *ref_path,
                           unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    char *content;
    char *saveptr = NULL;
    char *line;
    int rc = -1;

    snprintf(path, sizeof(path), "%s/packed-refs", git_dir);
    content = read_small_file(path);
    if (content == NULL)
        return -1;

    for (line = strtok_r(content, "\n", &saveptr); line != NULL;
        line = strtok_r(NULL, "\n", &saveptr)) {
        char *space;

        if (line[0] == '#' || line[0] == '^')
            continue;
        space = strchr(line, ' ');
        if (space == NULL || (size_t)(space - line) != SG_SHA1_HEX_LEN)
            continue;
        if (strcmp(space + 1, ref_path) == 0) {
            char hex[SG_SHA1_HEX_LEN + 1];

            memcpy(hex, line, SG_SHA1_HEX_LEN);
            hex[SG_SHA1_HEX_LEN] = '\0';
            rc = sg_hex_to_sha1(hex, id_out);
            break;
        }
    }

    free(content);
    return rc;
}

int sg_ref_read_path(const char *git_dir, const char *ref_path, unsigned char id_out[SG_SHA1_RAW_LEN])
{
    char full_path[SG_PATH_MAX];
    unsigned char *content;
    size_t content_len;
    char hex[SG_SHA1_HEX_LEN + 1];
    int rc;

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    snprintf(full_path, sizeof(full_path), "%s/%s", git_dir, ref_path);
    if (sg_read_file(full_path, &content, &content_len) != 0)
        return read_packed_ref(git_dir, ref_path, id_out); /* loose ref file absent: maybe packed by `git gc` */
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

/* ---- branch enumeration and deletion -------------------------------------
   Both must treat loose refs/heads files and packed-refs as ONE store:
   listing that only walks the directory goes blind right after `git
   pack-refs` (same failure mode read_packed_ref exists for), and a deletion
   that only unlinks the loose file leaves any stale packed line behind --
   the branch then "resurrects" at whatever old commit packed-refs still
   records, which is silent data corruption, not a cosmetic bug. */

typedef struct {
    char **names;
    size_t count;
    size_t cap;
} name_list;

static int name_list_push(name_list *list, const char *name)
{
    char *copy;

    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0 ? 16 : list->cap * 2;
        char **grown = realloc(list->names, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        list->names = grown;
        list->cap = new_cap;
    }
    copy = strdup(name);
    if (copy == NULL)
        return -1;
    list->names[list->count++] = copy;
    return 0;
}

static void name_list_free(name_list *list)
{
    size_t i;

    for (i = 0; i < list->count; i++)
        free(list->names[i]);
    free(list->names);
}

/* Recursively collects every regular file under dir_path as a branch name
   relative to refs/heads (subdirectories become slash-separated name
   segments, e.g. refs/heads/feature/x -> "feature/x"). A missing directory
   is an empty result, not an error. */
static int list_loose_branches(const char *dir_path, const char *rel_prefix, name_list *acc)
{
    DIR *dir;
    struct dirent *ent;
    int rc = 0;

    dir = opendir(dir_path);
    if (dir == NULL)
        return errno == ENOENT ? 0 : -1;

    while (rc == 0 && (ent = readdir(dir)) != NULL) {
        char child_path[SG_PATH_MAX];
        char child_rel[SG_PATH_MAX];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        /* A path this deep can't be a branch sg (or git) can address within
           these limits; skip it rather than stat/report a truncated name. */
        if (snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, ent->d_name) >=
               (int)sizeof(child_path) ||
           snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel_prefix,
                    rel_prefix[0] != '\0' ? "/" : "", ent->d_name) >= (int)sizeof(child_rel))
            continue;
        if (stat(child_path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            rc = list_loose_branches(child_path, child_rel, acc);
        else if (S_ISREG(st.st_mode))
            rc = name_list_push(acc, child_rel);
    }
    closedir(dir);
    return rc;
}

/* The enumerating sibling of read_packed_ref: same line grammar ('#' header
   comments, '^' peel lines, "<40-hex> <refname>"), but collecting every
   refname under prefix instead of looking one up. prefix must include the
   trailing '/' (e.g. "refs/heads/"). */
static int list_packed_under(const char *git_dir, const char *prefix, name_list *acc)
{
    char path[SG_PATH_MAX];
    char *content;
    char *saveptr = NULL;
    char *line;
    size_t prefix_len = strlen(prefix);
    int rc = 0;

    snprintf(path, sizeof(path), "%s/packed-refs", git_dir);
    content = read_small_file(path);
    if (content == NULL) {
        /* Missing packed-refs just means nothing is packed; a packed-refs
           that exists but cannot be read means the listing would silently
           omit every packed ref under prefix, so that is an error instead. */
        return access(path, F_OK) != 0 ? 0 : -1;
    }

    for (line = strtok_r(content, "\n", &saveptr); line != NULL && rc == 0;
        line = strtok_r(NULL, "\n", &saveptr)) {
        char *space;

        if (line[0] == '#' || line[0] == '^')
            continue;
        space = strchr(line, ' ');
        if (space == NULL || (size_t)(space - line) != SG_SHA1_HEX_LEN)
            continue;
        if (strncmp(space + 1, prefix, prefix_len) == 0 && space[1 + prefix_len] != '\0')
            rc = name_list_push(acc, space + 1 + prefix_len);
    }
    free(content);
    return rc;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int sg_ref_list_under(const char *git_dir, const char *prefix, char ***names_out, size_t *count_out)
{
    char dir_path[SG_PATH_MAX];
    name_list acc = {NULL, 0, 0};
    size_t prefix_len = strlen(prefix);
    size_t i;
    size_t out_n;

    *names_out = NULL;
    *count_out = 0;

    /* prefix ends in '/'; strip it to get the on-disk directory to walk. */
    if (prefix_len == 0 || prefix[prefix_len - 1] != '/')
        return -1;
    if (snprintf(dir_path, sizeof(dir_path), "%s/%.*s", git_dir, (int)(prefix_len - 1), prefix) >=
       (int)sizeof(dir_path))
        return -1;

    if (list_loose_branches(dir_path, "", &acc) != 0 ||
       list_packed_under(git_dir, prefix, &acc) != 0) {
        name_list_free(&acc);
        return -1;
    }

    if (acc.count == 0) {
        free(acc.names);
        return 0;
    }

    qsort(acc.names, acc.count, sizeof(char *), name_cmp);

    /* A ref present both loose and packed appears twice under the same
       name; only the name is reported, so deduping adjacent entries after
       the sort is exactly the "loose wins" reader precedence. */
    out_n = 0;
    for (i = 0; i < acc.count; i++) {
        if (out_n > 0 && strcmp(acc.names[out_n - 1], acc.names[i]) == 0)
            free(acc.names[i]);
        else
            acc.names[out_n++] = acc.names[i];
    }

    *names_out = acc.names;
    *count_out = out_n;
    return 0;
}

int sg_ref_list_branches(const char *git_dir, char ***names_out, size_t *count_out)
{
    return sg_ref_list_under(git_dir, BRANCH_PREFIX, names_out, count_out);
}

/* Rewrites packed-refs without prefix<name> (e.g. "refs/heads/"+"foo", and
   without any peel lines immediately following that entry). The rewrite is
   atomic -- filtered content goes to a temp file in git_dir which is then
   rename()d over packed-refs -- so a crash mid-delete can never leave a torn
   packed-refs. Missing packed-refs, or one that doesn't list the ref, is
   success with nothing to do. Returns 0 on success, -1 on I/O error. */
static int packed_refs_remove_under(const char *git_dir, const char *prefix, const char *name)
{
    char path[SG_PATH_MAX];
    char tmp_path[SG_PATH_MAX];
    char refname[SG_PATH_MAX];
    char *content;
    char *saveptr = NULL;
    char *line;
    FILE *f;
    int fd;
    int removed = 0;
    int skipping_peels = 0;

    snprintf(path, sizeof(path), "%s/packed-refs", git_dir);
    content = read_small_file(path);
    if (content == NULL) {
        /* Missing packed-refs: nothing to purge. Present but unreadable:
           MUST fail -- "success" here would leave a stale packed line
           behind, which is exactly the resurrection bug this function
           exists to prevent. */
        return access(path, F_OK) != 0 ? 0 : -1;
    }

    /* name comes from argv: a name long enough to truncate here would
       make the filter below match (and drop) the WRONG packed line. */
    if (snprintf(refname, sizeof(refname), "%s%s", prefix, name) >= (int)sizeof(refname)) {
        free(content);
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s/packed-refs.sg-XXXXXX", git_dir);
    fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(content);
        return -1;
    }
    f = fdopen(fd, "wb");
    if (f == NULL) {
        close(fd);
        unlink(tmp_path);
        free(content);
        return -1;
    }

    for (line = strtok_r(content, "\n", &saveptr); line != NULL;
        line = strtok_r(NULL, "\n", &saveptr)) {
        if (line[0] == '^') {
            if (skipping_peels)
                continue; /* peel line of the entry being removed */
        } else {
            skipping_peels = 0;
            if (line[0] != '#') {
                char *space = strchr(line, ' ');

                if (space != NULL && (size_t)(space - line) == SG_SHA1_HEX_LEN &&
                   strcmp(space + 1, refname) == 0) {
                    removed = 1;
                    skipping_peels = 1;
                    continue;
                }
            }
        }
        if (fprintf(f, "%s\n", line) < 0) {
            fclose(f);
            unlink(tmp_path);
            free(content);
            return -1;
        }
    }
    free(content);
    if (fclose(f) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if (!removed) {
        unlink(tmp_path);
        return 0; /* ref wasn't packed; leave packed-refs untouched */
    }
    /* mkstemp creates the file 0600; packed-refs is world-readable in git
       repos, so restore the conventional mode before swapping it in. */
    if (chmod(tmp_path, 0644) != 0 || rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int sg_ref_delete_under(const char *git_dir, const char *prefix, const char *name)
{
    char path[SG_PATH_MAX];
    char ref_path[SG_PATH_MAX];
    unsigned char id[SG_SHA1_RAW_LEN];
    struct stat st;
    size_t prefix_len = strlen(prefix);

    /* name comes straight from argv -- same traversal concern as every
       other function here. */
    if (!sg_ref_branch_name_is_safe(name) || prefix_len == 0 || prefix[prefix_len - 1] != '/')
        return -1;

    if (snprintf(ref_path, sizeof(ref_path), "%s%s", prefix, name) >= (int)sizeof(ref_path))
        return -1;
    if (snprintf(path, sizeof(path), "%s/%s", git_dir, ref_path) >= (int)sizeof(path))
        return -1;

    /* Existence: a present loose file counts even before validating its
       content (matching the previous branch-only sg_ref_branch_exists),
       otherwise fall back to the packed store via sg_ref_read_path. */
    if (!(stat(path, &st) == 0 && S_ISREG(st.st_mode)) &&
       sg_ref_read_path(git_dir, ref_path, id) != 0)
        return 1;

    /* Purge BOTH stores. Unlinking only the loose file would resurrect the
       ref from any stale packed-refs line (measured against real git:
       loose e6215c5 shadowing packed 72566fa came back at 72566fa); a
       packed-only ref (post `git pack-refs`) has no loose file at all
       and the rewrite below is the entire deletion. A name long enough to
       truncate the path would aim unlink() at some OTHER file, so refuse
       instead of truncating (the name comes from argv).

       Order matters: packed-refs FIRST, loose file second. Deleting the
       loose ref first and then failing the rewrite (ENOSPC, EROFS, a
       failed rename) would leave the stale packed entry exposed -- the
       ref silently resurrects at an older commit on a path that reports
       failure to the caller, which is the very bug this function exists to
       prevent. In this order a failed rewrite leaves the loose ref in
       place, and since loose shadows packed, the repository still reads
       exactly as it did before: the failure is inert. */
    if (packed_refs_remove_under(git_dir, prefix, name) != 0)
        return -1;

    if (unlink(path) != 0 && errno != ENOENT)
        return -1;

    /* Real git also removes the ref's own reflog file when the ref itself
       goes away (measured: `git branch -D` deletes logs/refs/heads/<branch>
       too, not just the ref). This function also serves tag deletion
       (cmd_tag.c's `sg tag -d`), and tags never had a reflog to begin with
       (ref_path_reflog_allowed excludes refs/tags/...), so logs/<ref_path>
       is routinely absent here.

       Best-effort on purpose, and this is the one step in this function that
       cannot report failure. Everything above is ordered so that a failure
       is inert -- the repository still reads exactly as it did before. By
       the time we get here the ref is already gone irreversibly, so
       returning -1 would tell the caller "delete failed" about a delete that
       succeeded, and `sg branch -d` would print an error for a branch that
       is no longer there. A leftover log file is cosmetic; a lying exit code
       is not. Note the path also needs 5 bytes more than the ref path did
       ("/logs/" vs "/"), so truncation is reachable for a name right at the
       limit even though the ref write itself fit. */
    {
        char log_path[SG_PATH_MAX];

        if (snprintf(log_path, sizeof(log_path), "%s/logs/%s", git_dir, ref_path) <
           (int)sizeof(log_path))
            (void)unlink(log_path);
    }

    return 0;
}

int sg_ref_delete_branch(const char *git_dir, const char *branch)
{
    return sg_ref_delete_under(git_dir, BRANCH_PREFIX, branch);
}

/* Only these namespaces get a reflog, matching real git (measured against
   2.55.0): HEAD itself, any branch, any remote-tracking ref, and refs/stash.
   refs/tags/... and sg's own internal refs (refs/sg/chunks,
   refs/small-git/undo/...) are deliberately excluded. */
static int ref_path_reflog_allowed(const char *ref_path)
{
    static const char remote_prefix[] = "refs/remotes/";

    if (strcmp(ref_path, "HEAD") == 0)
        return 1;
    if (strncmp(ref_path, BRANCH_PREFIX, strlen(BRANCH_PREFIX)) == 0)
        return 1;
    if (strncmp(ref_path, remote_prefix, strlen(remote_prefix)) == 0)
        return 1;
    if (strcmp(ref_path, "refs/stash") == 0)
        return 1;
    return 0;
}

int sg_ref_update(const char *git_dir, const char *ref_path, const unsigned char new_id[SG_SHA1_RAW_LEN],
                  const char *reflog_msg)
{
    unsigned char old_id[SG_SHA1_RAW_LEN];
    long long branch_offset = 0;
    long long head_offset = 0;
    int wrote_branch_log = 0;
    int wrote_head_log = 0;
    int is_current_branch = 0;

    if (reflog_msg == NULL)
        return write_ref_path_raw(git_dir, ref_path, new_id);

    if (!ref_path_reflog_allowed(ref_path))
        return -1;

    if (sg_ref_read_path(git_dir, ref_path, old_id) != 0)
        memset(old_id, 0, SG_SHA1_RAW_LEN); /* no such ref yet (or unreadable): treat as "created" */

    if (strncmp(ref_path, BRANCH_PREFIX, strlen(BRANCH_PREFIX)) == 0) {
        char *cur = sg_ref_current_branch(git_dir);

        if (cur != NULL) {
            if (strcmp(cur, ref_path + strlen(BRANCH_PREFIX)) == 0)
                is_current_branch = 1;
            free(cur);
        }
    }

    /* Rule 1 (measured, asymmetric): a ref's own log suppresses a no-op
       (old == new) update; logs/HEAD does not. */
    if (memcmp(old_id, new_id, SG_SHA1_RAW_LEN) != 0) {
        if (sg_reflog_append(git_dir, ref_path, old_id, new_id, reflog_msg, &branch_offset) != 0)
            return -1;
        wrote_branch_log = 1;
    }

    /* Rule 2 (measured): updating the branch HEAD currently points to also
       logs to HEAD, with the identical old/new/message, regardless of
       whether that update was itself a no-op. */
    if (is_current_branch) {
        if (sg_reflog_append(git_dir, "HEAD", old_id, new_id, reflog_msg, &head_offset) != 0) {
            if (wrote_branch_log)
                sg_reflog_truncate(git_dir, ref_path, branch_offset);
            return -1;
        }
        wrote_head_log = 1;
    }

    if (write_ref_path_raw(git_dir, ref_path, new_id) != 0) {
        if (wrote_head_log)
            sg_reflog_truncate(git_dir, "HEAD", head_offset);
        if (wrote_branch_log)
            sg_reflog_truncate(git_dir, ref_path, branch_offset);
        return -1;
    }

    return 0;
}

int sg_ref_set_head(const char *git_dir, const char *branch, const char *reflog_msg)
{
    char path[SG_PATH_MAX];
    FILE *f;
    long long offset = 0;
    int wrote_log = 0;
    int rc;

    if (reflog_msg != NULL) {
        unsigned char old_id[SG_SHA1_RAW_LEN];
        unsigned char new_id[SG_SHA1_RAW_LEN];

        if (sg_ref_resolve_head(git_dir, old_id) != 0)
            memset(old_id, 0, SG_SHA1_RAW_LEN);
        if (sg_ref_read_branch(git_dir, branch, new_id) != 0)
            memset(new_id, 0, SG_SHA1_RAW_LEN); /* target branch has no commits yet (e.g. fresh sg init) */

        if (sg_reflog_append(git_dir, "HEAD", old_id, new_id, reflog_msg, &offset) != 0)
            return -1;
        wrote_log = 1;
    }

    snprintf(path, sizeof(path), "%s/HEAD", git_dir);
    f = fopen(path, "wb");
    if (f == NULL) {
        if (wrote_log)
            sg_reflog_truncate(git_dir, "HEAD", offset);
        return -1;
    }
    if (fprintf(f, "ref: refs/heads/%s\n", branch) < 0) {
        fclose(f);
        if (wrote_log)
            sg_reflog_truncate(git_dir, "HEAD", offset);
        return -1;
    }
    rc = fclose(f) == 0 ? 0 : -1;
    if (rc != 0 && wrote_log)
        sg_reflog_truncate(git_dir, "HEAD", offset);
    return rc;
}

int sg_ref_set_head_detached(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                             const char *reflog_msg)
{
    unsigned char old_id[SG_SHA1_RAW_LEN];
    long long offset = 0;
    int wrote_log = 0;

    if (reflog_msg != NULL) {
        /* The old value MUST come from sg_ref_resolve_head, not from
           sg_ref_read_path(git_dir, "HEAD", ...): while HEAD is still
           symbolic its file holds "ref: refs/heads/<b>", whose hex parse
           necessarily fails, and the all-zeros fallback would record
           "detached from <commit>" as if the commit had been created from
           nothing. Real git writes the outgoing commit id there (measured,
           git 2.55.0). Resolving through the branch is the only way to get
           it, and is also why this cannot simply call sg_ref_update with a
           ref_path of "HEAD" even though that would produce the right FILE. */
        if (sg_ref_resolve_head(git_dir, old_id) != 0)
            memset(old_id, 0, SG_SHA1_RAW_LEN); /* unborn HEAD: nothing to come from */
        if (sg_reflog_append(git_dir, "HEAD", old_id, id, reflog_msg, &offset) != 0)
            return -1;
        wrote_log = 1;
    }

    if (write_ref_path_raw(git_dir, "HEAD", id) != 0) {
        if (wrote_log)
            sg_reflog_truncate(git_dir, "HEAD", offset);
        return -1;
    }
    return 0;
}

#include "sg/cli.h"

#include "sg/chunk.h"
#include "sg/hash.h"
#include "sg/ignore.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Non-zero if path carries any index entry at all (stage 0, or 1/2/3 while a
   merge conflict at it is unresolved). Tracked paths are staged even when an
   ignore rule matches them -- tracked wins over ignore. */
static int tracked_any_stage(const sg_index *idx, const char *path)
{
    unsigned int stage;

    for (stage = 0; stage <= 3; stage++) {
        if (sg_index_find_stage(idx, path, stage) >= 0)
            return 1;
    }
    return 0;
}

/* Non-zero if any index entry lives under dir/ (at any depth).
   An ignored directory is normally pruned outright, which is correct for
   untracked content: nothing under an excluded directory can be re-included.
   But tracked content is never subject to ignore rules, and pruning is the
   only thing that decides whether a tracked file is ever visited and
   re-hashed -- so pruning a directory that still holds tracked files made
   `sg add .` silently leave their modifications unstaged, exiting 0 while
   `git add .` staged them (git avoids this because its tracked-file updates
   are driven from the index rather than from the ignore-filtered walk).
   Entries are sorted byte-wise by path, so everything sharing the "dir/"
   prefix is contiguous and a scan can stop as soon as it passes it. */
static int tracked_under_dir(const sg_index *idx, const char *dir)
{
    size_t dir_len = strlen(dir);
    size_t i;

    if (dir_len == 0)
        return idx->count > 0;

    for (i = 0; i < idx->count; i++) {
        const char *p = idx->entries[i].path;
        int cmp = strncmp(p, dir, dir_len);

        if (cmp > 0)
            break; /* sorted: past every possible "dir/..." entry */
        if (cmp == 0 && p[dir_len] == '/')
            return 1;
    }
    return 0;
}

/* Stages the regular file at rel_path (repo-root-relative). display is the
   name used in messages: the user's own spelling for explicit arguments, the
   repo-relative path during recursion. Symlinks warn and are skipped;
   any other non-regular type (FIFO, socket, device) is rejected before any
   read, so an explicit FIFO argument errors instead of hanging in
   sg_read_file. */
static int stage_file(const char *git_dir, const char *repo_root, sg_index *idx,
                      const char *rel_path, const char *display)
{
    char abs_path[SG_PATH_MAX];
    struct stat st;
    unsigned char *content;
    size_t content_len;
    sg_index_entry entry;
    unsigned int mode;

    if (sg_path_join(abs_path, sizeof(abs_path), repo_root, rel_path) != 0) {
        fprintf(stderr, "sg: 路徑過長,無法處理 '%s'\n", display);
        return -1;
    }

    if (sg_is_symlink(abs_path)) {
        fprintf(stderr, "sg: warning: '%s' is a symlink, skipping (unsupported in phase 2)\n",
               display);
        return 0;
    }

    if (stat(abs_path, &st) != 0) {
        fprintf(stderr, "sg: cannot stat '%s': no such file\n", display);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "sg: '%s' 是不支援的檔案類型（不是一般檔案）\n", display);
        return -1;
    }

    if (sg_read_file(abs_path, &content, &content_len) != 0) {
        fprintf(stderr, "sg: failed to read '%s'\n", display);
        return -1;
    }

    mode = (st.st_mode & 0111) ? 0100755 : 0100644;

    memset(&entry, 0, sizeof(entry));
    entry.ctime_sec = (unsigned int)st.st_ctime;
#if defined(__APPLE__)
    entry.ctime_nsec = (unsigned int)st.st_ctimespec.tv_nsec;
    entry.mtime_nsec = (unsigned int)st.st_mtimespec.tv_nsec;
#else
    entry.ctime_nsec = (unsigned int)st.st_ctim.tv_nsec;
    entry.mtime_nsec = (unsigned int)st.st_mtim.tv_nsec;
#endif
    entry.mtime_sec = (unsigned int)st.st_mtime;
    entry.dev = (unsigned int)st.st_dev;
    entry.ino = (unsigned int)st.st_ino;
    entry.mode = mode;
    entry.uid = (unsigned int)st.st_uid;
    entry.gid = (unsigned int)st.st_gid;
    entry.file_size = (unsigned int)content_len;
    entry.path = (char *)rel_path; /* sg_index_upsert copies it */

    {
        int enabled = 0;
        size_t threshold = SG_CHUNK_DEFAULT_THRESHOLD;
        int chunked;
        int write_ok;

        sg_repo_read_chunk_config(git_dir, &enabled, &threshold);

        if (enabled)
            write_ok = sg_chunk_store_blob(git_dir, content, content_len, threshold, entry.sha1,
                                           &chunked) == 0;
        else
            write_ok = sg_loose_write(git_dir, SG_OBJ_BLOB, content, content_len, entry.sha1) == 0;

        if (!write_ok) {
            fprintf(stderr, "sg: failed to write blob for '%s'\n", display);
            free(content);
            return -1;
        }
    }
    free(content);

    /* If rel_path currently carries stage 1/2/3 entries (an unresolved
       merge conflict), staging it now means the user has resolved it:
       clear those before writing the ordinary stage-0 entry below. */
    sg_index_remove_all_stages(idx, rel_path);

    if (sg_index_upsert(idx, &entry) != 0) {
        fprintf(stderr, "sg: failed to stage '%s'\n", display);
        return -1;
    }

    return 0;
}

static int str_ptr_cmp(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;

    return strcmp(sa, sb);
}

/* Recursive directory walk for `sg add <dir>`. reldir is repo-root-relative
   ("" = the repo root itself); the caller has already pushed ignore frames
   for reldir and every ancestor. Entries are sorted before processing so
   staging order -- and therefore which error is reported first -- is
   deterministic across filesystems. Returns 0 on success, -1 on the first
   error; the index is only written when the whole command succeeds, so any
   error leaves the on-disk index untouched. */
static int add_walk(const char *git_dir, const char *repo_root, sg_index *idx, sg_ignore *ig,
                    const char *reldir, int force)
{
    char absdir[SG_PATH_MAX];
    DIR *d;
    struct dirent *ent;
    char **names = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;
    int rc = 0;

    if (sg_path_join(absdir, sizeof(absdir), repo_root, reldir) != 0) {
        fprintf(stderr, "sg: 路徑過長,無法處理目錄 '%s'\n", reldir);
        return -1;
    }

    d = opendir(absdir);
    if (d == NULL) {
        fprintf(stderr, "sg: 無法讀取目錄 '%s'\n", reldir[0] != '\0' ? reldir : ".");
        return -1;
    }

    while ((ent = readdir(d)) != NULL) {
        char *copy;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        /* never walk into a git dir, at any depth (no submodule support) */
        if (strcmp(ent->d_name, ".git") == 0)
            continue;
        if (count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            char **grown = realloc(names, new_cap * sizeof(*grown));

            if (grown == NULL) {
                rc = -1;
                break;
            }
            names = grown;
            cap = new_cap;
        }
        copy = strdup(ent->d_name);
        if (copy == NULL) {
            rc = -1;
            break;
        }
        names[count++] = copy;
    }
    closedir(d);
    if (rc != 0)
        fprintf(stderr, "sg: 記憶體不足，無法列出目錄 '%s'\n", reldir[0] != '\0' ? reldir : ".");

    if (rc == 0 && count > 1)
        qsort(names, count, sizeof(*names), str_ptr_cmp);

    for (i = 0; rc == 0 && i < count; i++) {
        char relpath[SG_PATH_MAX];
        char abspath[SG_PATH_MAX];
        struct stat st;

        /* Truncation here is NOT a cosmetic problem: a path cut off at the
           buffer boundary is usually still a real directory further up the
           tree, so the lstat below would SUCCEED against the wrong entry and
           the walk would silently operate on it. That is how this failed on
           Linux while passing on macOS -- Linux's PATH_MAX (4096) equals
           these buffers, so truncation happens before the kernel ever gets a
           chance to reject the path, whereas macOS's 1024 limit means the
           kernel rejects it first. Refuse instead of guessing. */
        if (sg_path_join(relpath, sizeof(relpath), reldir, names[i]) != 0 ||
           sg_path_join(abspath, sizeof(abspath), repo_root, relpath) != 0) {
            fprintf(stderr, "sg: 路徑過長,無法處理 '%s/%s'\n",
                    reldir[0] != '\0' ? reldir : ".", names[i]);
            rc = -1;
            break;
        }

        if (lstat(abspath, &st) != 0) {
            /* ENOENT genuinely means the entry disappeared between readdir
               and here, which is benign. Every other errno does NOT mean
               "gone" -- ENAMETOOLONG (this walk builds absolute paths, so a
               deep enough tree exceeds the platform's PATH_MAX), EACCES,
               ELOOP. Skipping those silently made `sg add .` stage nothing
               and still exit 0 while `git add .` staged the file: a commit
               quietly missing content, which is strictly worse than refusing
               to run. */
            if (errno == ENOENT)
                continue;
            fprintf(stderr, "sg: 無法讀取 '%s': %s\n", relpath, strerror(errno));
            rc = -1;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            /* An ignored directory is pruned outright: nothing under it can
               be re-included by any pattern (verified git behavior) -- unless
               it still holds tracked files, which ignore rules never apply to
               and which are only re-staged by being walked (see
               tracked_under_dir). */
            if (!force && sg_ignore_is_ignored(ig, relpath, 1) &&
               !tracked_under_dir(idx, relpath))
                continue;
            if (sg_ignore_push_dir(ig, relpath) != 0) {
                fprintf(stderr, "sg: 記憶體不足，無法載入 .gitignore 規則\n");
                rc = -1;
                break;
            }
            rc = add_walk(git_dir, repo_root, idx, ig, relpath, force);
            sg_ignore_pop_dir(ig);
        } else if (S_ISLNK(st.st_mode)) {
            rc = stage_file(git_dir, repo_root, idx, relpath, relpath); /* warns + skips */
        } else if (S_ISREG(st.st_mode)) {
            /* Tracked files are staged even when ignored (tracked wins);
               untracked ignored files are skipped silently. */
            if (!force && !tracked_any_stage(idx, relpath) &&
                sg_ignore_is_ignored(ig, relpath, 0))
                continue;
            rc = stage_file(git_dir, repo_root, idx, relpath, relpath);
        }
        /* other types (FIFO/socket/device) are skipped silently during
           recursion; only explicitly named ones error */
    }

    for (i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return rc;
}

/* After walking an added directory, stages deletions the way `git add <dir>`
   does: every stage-0 index entry under prefix ("" = the whole repo) whose
   working-tree file is gone (lstat fails with ENOENT) is removed from the
   index. Entries whose path now names a symlink or another non-regular type
   are left alone: sg treats those as unsupported, and dropping the entry on
   a file->symlink swap would lose the last tracked content. Paths are
   collected first and removed afterwards so removal never invalidates the
   iteration. Returns 0 on success, -1 on allocation failure. */
static int stage_deletions_under(const char *repo_root, sg_index *idx, const char *prefix)
{
    size_t plen = strlen(prefix);
    char **gone = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;
    int rc = 0;
    int oom = 0;

    for (i = 0; i < idx->count; i++) {
        const sg_index_entry *e = &idx->entries[i];
        char abspath[SG_PATH_MAX];
        struct stat st;
        char *copy;

        if (e->stage != 0)
            continue;
        if (plen > 0 && (strncmp(e->path, prefix, plen) != 0 || e->path[plen] != '/'))
            continue;
        /* A truncated path could lstat successfully against some shorter
           real path and make this conclude the file still exists -- which
           here would mean silently NOT staging a deletion. Refuse instead. */
        if (sg_path_join(abspath, sizeof(abspath), repo_root, e->path) != 0) {
            fprintf(stderr, "sg: 路徑過長,無法檢查 '%s' 是否已刪除\n", e->path);
            rc = -1;
            break;
        }
        if (lstat(abspath, &st) == 0 || errno != ENOENT)
            continue;
        if (count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            char **grown = realloc(gone, new_cap * sizeof(*grown));

            if (grown == NULL) {
                oom = 1;
                rc = -1;
                break;
            }
            gone = grown;
            cap = new_cap;
        }
        copy = strdup(e->path);
        if (copy == NULL) {
            oom = 1;
            rc = -1;
            break;
        }
        gone[count++] = copy;
    }

    if (rc == 0) {
        for (i = 0; i < count; i++)
            sg_index_remove(idx, gone[i]);
    } else if (oom) {
        /* Only the allocation failures get this message. The path-too-long
           bail above has already said what went wrong, and following it with
           "out of memory" would contradict it. */
        fprintf(stderr, "sg: 記憶體不足，無法暫存刪除的檔案\n");
    }

    for (i = 0; i < count; i++)
        free(gone[i]);
    free(gone);
    return rc;
}

/* Pushes an ignore frame for every directory component of rel ("a/b/c"
   pushes "a", then "a/b", then -- when include_leaf -- "a/b/c" itself), so
   the .gitignore files between the repo root and an explicitly named
   argument all take part in ignore decisions. Returns the number of frames
   pushed, or -1 on allocation failure (anything already pushed is popped
   again). */
static int push_parents(sg_ignore *ig, const char *rel, int include_leaf)
{
    size_t len = strlen(rel);
    size_t i;
    int pushed = 0;
    char *buf;

    if (len == 0)
        return 0;
    buf = malloc(len + 1);
    if (buf == NULL)
        return -1;

    for (i = 1; i < len; i++) {
        if (rel[i] != '/')
            continue;
        memcpy(buf, rel, i);
        buf[i] = '\0';
        if (sg_ignore_push_dir(ig, buf) != 0)
            goto fail;
        pushed++;
    }
    if (include_leaf) {
        memcpy(buf, rel, len + 1);
        if (sg_ignore_push_dir(ig, buf) != 0)
            goto fail;
        pushed++;
    }
    free(buf);
    return pushed;

fail:
    while (pushed-- > 0)
        sg_ignore_pop_dir(ig);
    free(buf);
    return -1;
}

static void pop_n(sg_ignore *ig, int n)
{
    while (n-- > 0)
        sg_ignore_pop_dir(ig);
}

/* Handles one explicit command-line path argument: file, directory
   (recursive), deleted-but-tracked path, or unsupported type. Returns 0 on
   success, -1 on error. */
static int add_one_arg(const char *git_dir, const char *repo_root, sg_index *idx, sg_ignore *ig,
                       const char *arg, int force)
{
    char *rel;
    char abs_path[SG_PATH_MAX];
    struct stat st;
    int rc = 0;

    rel = sg_resolve_repo_path_allow_root(repo_root, arg);
    if (rel == NULL) {
        fprintf(stderr, "sg: '%s' is outside the repository\n", arg);
        return -1;
    }

    if (sg_path_join(abs_path, sizeof(abs_path), repo_root, rel) != 0) {
        fprintf(stderr, "sg: 路徑過長,無法處理 '%s'\n", arg);
        free(rel);
        return -1;
    }

    if (lstat(abs_path, &st) != 0) {
        /* A tracked path that no longer exists on disk: stage the deletion,
           matching `git add <deleted-file>`. */
        if (rel[0] != '\0' && sg_index_find(idx, rel) >= 0) {
            sg_index_remove(idx, rel);
            free(rel);
            return 0;
        }
        fprintf(stderr, "sg: cannot stat '%s': no such file\n", arg);
        free(rel);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        int pushed = push_parents(ig, rel, 1);

        if (pushed < 0) {
            fprintf(stderr, "sg: 記憶體不足，無法載入 .gitignore 規則\n");
            free(rel);
            return -1;
        }
        if (!force && rel[0] != '\0' && sg_ignore_is_ignored(ig, rel, 1)) {
            fprintf(stderr, "sg: '%s' 被 .gitignore 規則忽略（使用 -f 強制加入）\n", arg);
            rc = -1;
        } else {
            rc = add_walk(git_dir, repo_root, idx, ig, rel, force);
            if (rc == 0)
                rc = stage_deletions_under(repo_root, idx, rel);
        }
        pop_n(ig, pushed);
    } else if (S_ISLNK(st.st_mode)) {
        rc = stage_file(git_dir, repo_root, idx, rel, arg); /* warning + skip */
    } else if (S_ISREG(st.st_mode)) {
        int pushed = push_parents(ig, rel, 0);

        if (pushed < 0) {
            fprintf(stderr, "sg: 記憶體不足，無法載入 .gitignore 規則\n");
            free(rel);
            return -1;
        }
        /* An explicitly named ignored file errors (git: exit 1 + advice)
           unless forced or already tracked -- tracked wins over ignore. */
        if (!force && !tracked_any_stage(idx, rel) && sg_ignore_is_ignored(ig, rel, 0)) {
            fprintf(stderr, "sg: '%s' 被 .gitignore 規則忽略（使用 -f 強制加入）\n", arg);
            rc = -1;
        } else {
            rc = stage_file(git_dir, repo_root, idx, rel, arg);
        }
        pop_n(ig, pushed);
    } else {
        fprintf(stderr, "sg: '%s' 是不支援的檔案類型（不是一般檔案）\n", arg);
        rc = -1;
    }

    free(rel);
    return rc;
}

int sg_cmd_add(int argc, char **argv)
{
    static const char usage[] = "usage: sg add [-f | --force] [--] <path>...\n";
    char *git_dir;
    char *repo_root;
    sg_index idx;
    sg_ignore *ig = NULL;
    int i;
    int rc = 0;
    int force = 0;
    int npaths = 0;
    int no_more_flags = 0;

    for (i = 1; i < argc; i++) {
        if (!no_more_flags && strcmp(argv[i], "--") == 0) {
            no_more_flags = 1;
            continue;
        }
        if (!no_more_flags && (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)) {
            force = 1;
            continue;
        }
        npaths++;
    }
    if (npaths == 0) {
        fputs(usage, stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;
    repo_root = sg_repo_root(git_dir);
    if (repo_root == NULL) {
        fprintf(stderr, "sg: failed to determine repository root\n");
        free(git_dir);
        return 1;
    }

    if (sg_index_read(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to read index (corrupt?)\n");
        free(git_dir);
        free(repo_root);
        return 1;
    }

    if (sg_ignore_open(&ig, git_dir, repo_root) != 0) {
        fprintf(stderr, "sg: 記憶體不足，無法載入 .gitignore 規則\n");
        sg_index_free(&idx);
        free(git_dir);
        free(repo_root);
        return 1;
    }

    /* All-or-nothing: the rewritten index is only written back when every
       argument succeeded, so an error on ANY explicit argument leaves the
       on-disk index untouched and the command exits 1. */
    no_more_flags = 0;
    for (i = 1; i < argc; i++) {
        if (!no_more_flags && strcmp(argv[i], "--") == 0) {
            no_more_flags = 1;
            continue;
        }
        if (!no_more_flags && (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0))
            continue;
        if (add_one_arg(git_dir, repo_root, &idx, ig, argv[i], force) != 0)
            rc = 1;
    }

    if (rc == 0 && sg_index_write(git_dir, &idx) != 0) {
        fprintf(stderr, "sg: failed to write index\n");
        rc = 1;
    }

    sg_ignore_free(ig);
    sg_index_free(&idx);
    free(git_dir);
    free(repo_root);
    return rc;
}

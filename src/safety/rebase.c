#include "sg/rebase.h"

#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/refs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SG_PATH_MAX 4096

/* What orig-branch holds when the rebase started on a detached HEAD, read
   back as orig_branch == NULL. One definition, because two copies of a
   string a mutation has to hit is one copy the mutation silently misses --
   the reader and the writer would still agree with each other while both
   drifted away from the format the tests pin. */
static const char DETACHED_SENTINEL[] = "detached HEAD";

static int state_dir_path(const char *git_dir, char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size, "%s/sg-rebase", git_dir) < out_size ? 0 : -1;
}

static int state_file_path(const char *git_dir, const char *name, char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size, "%s/sg-rebase/%s", git_dir, name) < out_size ? 0 : -1;
}

int sg_rebase_state_exists(const char *git_dir)
{
    char path[SG_PATH_MAX];
    struct stat st;

    if (state_dir_path(git_dir, path, sizeof(path)) != 0)
        return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Reads one line (trailing '\n' stripped) from a whole small file, malloc'd.
   Returns NULL if the file can't be read or is empty. */
static char *read_line_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t cap = 256, used = 0;
    char *nl;

    if (f == NULL)
        return NULL;
    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        size_t n;

        if (used + 1 >= cap) {
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
        n = fread(buf + used, 1, cap - used - 1, f);
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
    buf[used] = '\0';

    if (used == 0) {
        free(buf);
        return NULL;
    }

    nl = strchr(buf, '\n');
    if (nl != NULL)
        *nl = '\0';
    return buf;
}

static int write_line_file(const char *path, const char *line)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return -1;
    if (fprintf(f, "%s\n", line) < 0) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int read_hex_file(const char *path, unsigned char out[SG_SHA1_RAW_LEN])
{
    char *line = read_line_file(path);
    int rc;

    if (line == NULL)
        return -1;
    if (strlen(line) != SG_SHA1_HEX_LEN) {
        free(line);
        return -1;
    }
    rc = sg_hex_to_sha1(line, out);
    free(line);
    return rc;
}

static int write_hex_file(const char *path, const unsigned char id[SG_SHA1_RAW_LEN])
{
    char hex[SG_SHA1_HEX_LEN + 1];

    sg_sha1_to_hex(id, hex);
    return write_line_file(path, hex);
}

/* todo file: one 40-hex-char line per pending commit, oldest-to-newest. Every
   line must be exactly SG_SHA1_HEX_LEN chars -- anything else (short line,
   trailing garbage) means the sequencer state is corrupt. */
static int read_todo_file(const char *path, unsigned char (**out)[SG_SHA1_RAW_LEN],
                          size_t *out_count)
{
    FILE *f = fopen(path, "rb");
    unsigned char(*todo)[SG_SHA1_RAW_LEN] = NULL;
    size_t count = 0, cap = 0;
    char line[SG_SHA1_HEX_LEN + 2];

    *out = NULL;
    *out_count = 0;

    if (f == NULL)
        return -1;

    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);

        if (len == 0 || line[len - 1] != '\n') {
            fclose(f);
            free(todo);
            return -1;
        }
        line[len - 1] = '\0';
        if (strlen(line) != SG_SHA1_HEX_LEN) {
            fclose(f);
            free(todo);
            return -1;
        }
        if (count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(todo, new_cap * sizeof(*grown));

            if (grown == NULL) {
                fclose(f);
                free(todo);
                return -1;
            }
            todo = grown;
            cap = new_cap;
        }
        if (sg_hex_to_sha1(line, todo[count]) != 0) {
            fclose(f);
            free(todo);
            return -1;
        }
        count++;
    }
    if (ferror(f)) {
        fclose(f);
        free(todo);
        return -1;
    }
    fclose(f);

    *out = todo;
    *out_count = count;
    return 0;
}

static int write_todo_file(const char *path, const unsigned char (*todo)[SG_SHA1_RAW_LEN],
                           size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (f == NULL)
        return -1;
    for (i = 0; i < count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(todo[i], hex);
        if (fprintf(f, "%s\n", hex) < 0) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

int sg_rebase_state_read(const char *git_dir, sg_rebase_state *out)
{
    char dir_path[SG_PATH_MAX];
    char path[SG_PATH_MAX];
    char *branch;

    memset(out, 0, sizeof(*out));

    if (!sg_rebase_state_exists(git_dir))
        return -1;

    if (state_dir_path(git_dir, dir_path, sizeof(dir_path)) != 0)
        return -1;

    if (state_file_path(git_dir, "onto", path, sizeof(path)) != 0 ||
       read_hex_file(path, out->onto) != 0)
        return -1;

    if (state_file_path(git_dir, "orig-head", path, sizeof(path)) != 0 ||
       read_hex_file(path, out->orig_head) != 0)
        return -1;

    if (state_file_path(git_dir, "orig-branch", path, sizeof(path)) != 0)
        return -1;
    branch = read_line_file(path);
    if (branch == NULL) {
        /* Absence is corruption, not "detached at start" -- a rebase that
           got this far always wrote SOME orig-branch file (a real branch
           name or the "detached HEAD" sentinel below), so a missing file
           means something else destroyed it. Treating a missing file as
           "detached" would launder that loss into a legitimate state. */
        return -1;
    }
    if (strcmp(branch, DETACHED_SENTINEL) == 0) {
        /* Sentinel for "rebase started on a detached HEAD" (mirrors real
           git's .git/rebase-merge/head-name, oracle-measured: git 2.55.0
           writes the literal string "detached HEAD" there too). It can
           never collide with a real branch name: sg_ref_name_valid_for_create
           rejects names containing a space at creation time, and real git's
           check-ref-format does the same, so no branch called "detached
           HEAD" can ever exist to be confused with this. */
        free(branch);
        out->orig_branch = NULL;
    } else if (!sg_ref_branch_name_is_safe(branch)) {
        free(branch);
        return -1;
    } else {
        out->orig_branch = branch;
    }

    if (state_file_path(git_dir, "todo", path, sizeof(path)) != 0 ||
       read_todo_file(path, &out->todo, &out->todo_count) != 0) {
        free(out->orig_branch);
        out->orig_branch = NULL;
        return -1;
    }

    if (state_file_path(git_dir, "current", path, sizeof(path)) != 0) {
        sg_rebase_state_free(out);
        return -1;
    }
    if (read_hex_file(path, out->current) == 0) {
        out->has_current = 1;
    } else {
        struct stat st;

        /* "current" is optional (absent between the initial write and the
           first conflict), but if the file exists and is merely malformed,
           that IS corruption -- don't silently treat it as "no conflict". */
        if (stat(path, &st) == 0) {
            sg_rebase_state_free(out);
            return -1;
        }
        out->has_current = 0;
    }

    return 0;
}

int sg_rebase_state_write(const char *git_dir, const sg_rebase_state *st)
{
    char dir_path[SG_PATH_MAX];
    char path[SG_PATH_MAX];

    if (state_dir_path(git_dir, dir_path, sizeof(dir_path)) != 0)
        return -1;
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST)
        return -1;

    if (state_file_path(git_dir, "onto", path, sizeof(path)) != 0 ||
       write_hex_file(path, st->onto) != 0)
        return -1;
    if (state_file_path(git_dir, "orig-head", path, sizeof(path)) != 0 ||
       write_hex_file(path, st->orig_head) != 0)
        return -1;
    if (state_file_path(git_dir, "orig-branch", path, sizeof(path)) != 0 ||
       write_line_file(path, st->orig_branch != NULL ? st->orig_branch : DETACHED_SENTINEL) != 0)
        return -1;
    if (state_file_path(git_dir, "todo", path, sizeof(path)) != 0 ||
       write_todo_file(path, st->todo, st->todo_count) != 0)
        return -1;

    if (state_file_path(git_dir, "current", path, sizeof(path)) != 0)
        return -1;
    if (st->has_current) {
        if (write_hex_file(path, st->current) != 0)
            return -1;
    } else {
        if (remove(path) != 0 && errno != ENOENT)
            return -1;
    }

    return 0;
}

/* Sweeps any leftover entries in dir_path (e.g. an editor swap file dropped
   there by hand) so a stray file can't leave sg-rebase/ un-rmdir-able and
   every later command mistaking that for an in-progress rebase forever. */
static void sweep_stray_entries(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    struct dirent *ent;
    char path[SG_PATH_MAX];

    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) >= sizeof(path))
            continue;
        remove(path);
    }
    closedir(d);
}

int sg_rebase_state_remove(const char *git_dir)
{
    char dir_path[SG_PATH_MAX];
    char path[SG_PATH_MAX];
    static const char *names[] = {"onto", "orig-head", "orig-branch", "todo", "current"};
    size_t i;
    int rc = 0;

    if (state_dir_path(git_dir, dir_path, sizeof(dir_path)) != 0)
        return -1;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (state_file_path(git_dir, names[i], path, sizeof(path)) != 0) {
            rc = -1;
            continue;
        }
        if (remove(path) != 0 && errno != ENOENT)
            rc = -1;
    }
    if (rmdir(dir_path) != 0 && errno != ENOENT) {
        if (errno != ENOTEMPTY)
            return -1;
        sweep_stray_entries(dir_path);
        if (rmdir(dir_path) != 0 && errno != ENOENT)
            rc = -1;
    }
    return rc;
}

void sg_rebase_state_free(sg_rebase_state *st)
{
    free(st->orig_branch);
    st->orig_branch = NULL;
    free(st->todo);
    st->todo = NULL;
    st->todo_count = 0;
}

int sg_rebase_compute_todo(const char *git_dir, const unsigned char head_commit[SG_SHA1_RAW_LEN],
                          const unsigned char base_commit[SG_SHA1_RAW_LEN],
                          unsigned char (**out)[SG_SHA1_RAW_LEN], size_t *out_count,
                          unsigned char merge_commit_out[SG_SHA1_RAW_LEN], int *found_merge_commit_out)
{
    unsigned char(*todo)[SG_SHA1_RAW_LEN] = NULL;
    size_t count = 0, cap = 0;
    unsigned char cur[SG_SHA1_RAW_LEN];

    *out = NULL;
    *out_count = 0;
    *found_merge_commit_out = 0;

    memcpy(cur, head_commit, SG_SHA1_RAW_LEN);
    while (memcmp(cur, base_commit, SG_SHA1_RAW_LEN) != 0) {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;

        if (sg_object_read(git_dir, cur, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT) {
            free(todo);
            return -1;
        }
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            free(content);
            free(todo);
            return -1;
        }
        free(content);

        if (commit.parent_count > 1) {
            memcpy(merge_commit_out, cur, SG_SHA1_RAW_LEN);
            *found_merge_commit_out = 1;
            sg_commit_free(&commit);
            free(todo);
            return 0;
        }
        if (commit.parent_count == 0) {
            /* Reached a root commit without ever meeting base_commit: base
               is not actually an ancestor of head via first-parent (or the
               history is otherwise unrelated). The caller already validated
               base via sg_merge_base, so this indicates a corrupt/unexpected
               graph rather than a normal outcome. */
            sg_commit_free(&commit);
            free(todo);
            return -1;
        }

        if (count == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(todo, new_cap * sizeof(*grown));

            if (grown == NULL) {
                sg_commit_free(&commit);
                free(todo);
                return -1;
            }
            todo = grown;
            cap = new_cap;
        }
        /* appended newest-first for now; reversed below into oldest-first */
        memcpy(todo[count], cur, SG_SHA1_RAW_LEN);
        count++;

        memcpy(cur, commit.parents[0], SG_SHA1_RAW_LEN);
        sg_commit_free(&commit);
    }

    {
        size_t lo = 0, hi = count > 0 ? count - 1 : 0;

        while (lo < hi) {
            unsigned char tmp[SG_SHA1_RAW_LEN];

            memcpy(tmp, todo[lo], SG_SHA1_RAW_LEN);
            memcpy(todo[lo], todo[hi], SG_SHA1_RAW_LEN);
            memcpy(todo[hi], tmp, SG_SHA1_RAW_LEN);
            lo++;
            hi--;
        }
    }

    *out = todo;
    *out_count = count;
    return 0;
}

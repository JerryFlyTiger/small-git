#include "sg/sequencer.h"

#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/workdir.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *head_file_name(sg_seq_kind kind)
{
    return kind == SG_SEQ_CHERRY_PICK ? "CHERRY_PICK_HEAD" : "REVERT_HEAD";
}

static const char *todo_verb(sg_seq_kind kind)
{
    return kind == SG_SEQ_CHERRY_PICK ? "pick" : "revert";
}

static int top_path(const char *git_dir, const char *name, char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size, "%s/%s", git_dir, name) < out_size ? 0 : -1;
}

static int seq_dir_path(const char *git_dir, char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size, "%s/sequencer", git_dir) < out_size ? 0 : -1;
}

static int seq_file_path(const char *git_dir, const char *name, char *out, size_t out_size)
{
    return (size_t)snprintf(out, out_size, "%s/sequencer/%s", git_dir, name) < out_size ? 0 : -1;
}

/* Reads one whole small file, malloc'd, trailing '\n' NOT stripped (callers
   that want a single stripped line use read_line_file below). Returns NULL
   if the file can't be read or is empty. */
static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t cap = 256, used = 0;

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
    return buf;
}

static char *read_line_file(const char *path)
{
    char *buf = read_whole_file(path);
    char *nl;

    if (buf == NULL)
        return NULL;
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

sg_seq_kind sg_sequencer_kind_in_progress(const char *git_dir)
{
    char path[SG_PATH_MAX];
    struct stat st;

    if (top_path(git_dir, head_file_name(SG_SEQ_CHERRY_PICK), path, sizeof(path)) == 0 &&
       lstat(path, &st) == 0)
        return SG_SEQ_CHERRY_PICK;
    if (top_path(git_dir, head_file_name(SG_SEQ_REVERT), path, sizeof(path)) == 0 &&
       lstat(path, &st) == 0)
        return SG_SEQ_REVERT;
    return 0;
}

static int seq_dir_exists(const char *git_dir)
{
    char path[SG_PATH_MAX];
    struct stat st;

    if (seq_dir_path(git_dir, path, sizeof(path)) != 0)
        return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Parses one todo line: "<verb> <40hex> <subject>\n". Subject is not
   validated (any bytes up to the newline); only verb and hex are checked.
   Returns 0 and fills id on success, -1 on any malformed field. */
static int parse_todo_line(const char *line, sg_seq_kind kind, unsigned char id[SG_SHA1_RAW_LEN])
{
    const char *verb = todo_verb(kind);
    size_t verb_len = strlen(verb);
    char hex[SG_SHA1_HEX_LEN + 1];

    if (strncmp(line, verb, verb_len) != 0 || line[verb_len] != ' ')
        return -1;
    line += verb_len + 1;
    if (strlen(line) < SG_SHA1_HEX_LEN || line[SG_SHA1_HEX_LEN] != ' ')
        return -1;
    memcpy(hex, line, SG_SHA1_HEX_LEN);
    hex[SG_SHA1_HEX_LEN] = '\0';
    return sg_hex_to_sha1(hex, id);
}

static int read_todo_file(const char *path, sg_seq_kind kind, unsigned char (**out)[SG_SHA1_RAW_LEN],
                          size_t *out_count)
{
    FILE *f = fopen(path, "rb");
    unsigned char(*todo)[SG_SHA1_RAW_LEN] = NULL;
    size_t count = 0, cap = 0;
    char *line = NULL;
    size_t line_cap = 0;

    *out = NULL;
    *out_count = 0;
    if (f == NULL)
        return -1;

    for (;;) {
        ssize_t n = getline(&line, &line_cap, f);

        if (n < 0)
            break;
        if (n == 0 || line[n - 1] != '\n') {
            free(line);
            fclose(f);
            free(todo);
            return -1;
        }
        line[n - 1] = '\0';

        if (count == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            unsigned char(*grown)[SG_SHA1_RAW_LEN] = realloc(todo, new_cap * sizeof(*grown));

            if (grown == NULL) {
                free(line);
                fclose(f);
                free(todo);
                return -1;
            }
            todo = grown;
            cap = new_cap;
        }
        if (parse_todo_line(line, kind, todo[count]) != 0) {
            free(line);
            fclose(f);
            free(todo);
            return -1;
        }
        count++;
    }
    free(line);
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

static int write_todo_file(const char *git_dir, const char *path, sg_seq_kind kind,
                           const unsigned char (*todo)[SG_SHA1_RAW_LEN], size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (f == NULL)
        return -1;
    for (i = 0; i < count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;
        sg_commit commit;
        size_t subject_len;

        sg_sha1_to_hex(todo[i], hex);

        if (sg_object_read(git_dir, todo[i], &type, &content, &content_len) != 0 ||
           type != SG_OBJ_COMMIT) {
            fclose(f);
            return -1;
        }
        if (sg_commit_parse(content, content_len, &commit) != 0) {
            free(content);
            fclose(f);
            return -1;
        }
        free(content);
        subject_len = strcspn(commit.message, "\n");

        if (fprintf(f, "%s %s %.*s\n", todo_verb(kind), hex, (int)subject_len,
                   commit.message) < 0) {
            sg_commit_free(&commit);
            fclose(f);
            return -1;
        }
        sg_commit_free(&commit);
    }
    return fclose(f) == 0 ? 0 : -1;
}

int sg_sequencer_state_read(const char *git_dir, sg_sequencer_state *out)
{
    char path[SG_PATH_MAX];
    sg_seq_kind kind;

    memset(out, 0, sizeof(*out));

    kind = sg_sequencer_kind_in_progress(git_dir);
    if (kind == 0)
        return -1;
    out->kind = kind;

    if (top_path(git_dir, head_file_name(kind), path, sizeof(path)) != 0 ||
       read_hex_file(path, out->current) != 0)
        return -1;

    if (!seq_dir_exists(git_dir)) {
        out->has_sequence = 0;
        return 0;
    }
    out->has_sequence = 1;

    if (seq_file_path(git_dir, "head", path, sizeof(path)) != 0 ||
       read_hex_file(path, out->orig_head) != 0)
        return -1;
    if (seq_file_path(git_dir, "abort-safety", path, sizeof(path)) != 0 ||
       read_hex_file(path, out->abort_safety) != 0)
        return -1;
    if (seq_file_path(git_dir, "todo", path, sizeof(path)) != 0 ||
       read_todo_file(path, kind, &out->todo, &out->todo_count) != 0)
        return -1;
    if (out->todo_count == 0 || memcmp(out->todo[0], out->current, SG_SHA1_RAW_LEN) != 0) {
        free(out->todo);
        out->todo = NULL;
        out->todo_count = 0;
        return -1;
    }

    return 0;
}

int sg_sequencer_abort_target(const char *git_dir, sg_seq_kind *kind_out, int *has_sequence_out,
                              unsigned char orig_head_out[SG_SHA1_RAW_LEN],
                              unsigned char abort_safety_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    sg_seq_kind kind = sg_sequencer_kind_in_progress(git_dir);

    if (kind == 0)
        return -1;
    *kind_out = kind;

    if (!seq_dir_exists(git_dir)) {
        *has_sequence_out = 0;
        return 0;
    }
    *has_sequence_out = 1;

    /* Deliberately does NOT touch sequencer/todo -- see this function's
       header comment. head/abort-safety are each a single 40-hex line,
       exactly as simple as CHERRY_PICK_HEAD's own format, and written
       identically by git and sg. */
    if (seq_file_path(git_dir, "head", path, sizeof(path)) != 0 ||
       read_hex_file(path, orig_head_out) != 0)
        return -1;
    if (seq_file_path(git_dir, "abort-safety", path, sizeof(path)) != 0 ||
       read_hex_file(path, abort_safety_out) != 0)
        return -1;
    return 0;
}

int sg_sequencer_current_commit(const char *git_dir, sg_seq_kind *kind_out,
                                unsigned char current_out[SG_SHA1_RAW_LEN])
{
    char path[SG_PATH_MAX];
    sg_seq_kind kind = sg_sequencer_kind_in_progress(git_dir);

    if (kind == 0)
        return -1;
    *kind_out = kind;

    /* Deliberately independent of sequencer/'s existence or parseability --
       CHERRY_PICK_HEAD/REVERT_HEAD's own value is a single 40-hex line,
       readable regardless of whatever state sequencer/todo is in. */
    if (top_path(git_dir, head_file_name(kind), path, sizeof(path)) != 0 ||
       read_hex_file(path, current_out) != 0)
        return -1;
    return 0;
}

int sg_sequencer_state_write(const char *git_dir, const sg_sequencer_state *st)
{
    char path[SG_PATH_MAX];

    if (top_path(git_dir, head_file_name(st->kind), path, sizeof(path)) != 0 ||
       write_hex_file(path, st->current) != 0)
        return -1;

    if (!st->has_sequence)
        return 0;

    {
        char dir_path[SG_PATH_MAX];

        if (seq_dir_path(git_dir, dir_path, sizeof(dir_path)) != 0)
            return -1;
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST)
            return -1;
    }

    if (seq_file_path(git_dir, "head", path, sizeof(path)) != 0 ||
       write_hex_file(path, st->orig_head) != 0)
        return -1;
    if (seq_file_path(git_dir, "abort-safety", path, sizeof(path)) != 0 ||
       write_hex_file(path, st->abort_safety) != 0)
        return -1;
    if (seq_file_path(git_dir, "todo", path, sizeof(path)) != 0 ||
       write_todo_file(git_dir, path, st->kind, st->todo, st->todo_count) != 0)
        return -1;

    return 0;
}

int sg_sequencer_write_merge_msg(const char *git_dir, const char *message,
                                 char **conflict_paths, size_t conflict_count)
{
    char path[SG_PATH_MAX];
    FILE *f;
    size_t i;

    if (top_path(git_dir, "MERGE_MSG", path, sizeof(path)) != 0)
        return -1;

    f = fopen(path, "wb");
    if (f == NULL)
        return -1;
    if (fputs(message, f) < 0) {
        fclose(f);
        return -1;
    }
    if (conflict_count > 0) {
        if (fprintf(f, "\n# Conflicts:\n") < 0) {
            fclose(f);
            return -1;
        }
        for (i = 0; i < conflict_count; i++) {
            if (fprintf(f, "#\t%s\n", conflict_paths[i]) < 0) {
                fclose(f);
                return -1;
            }
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Sweeps any leftover entries in dir_path so a stray file can't leave
   sequencer/ un-rmdir-able forever, same convention as sg_rebase_state_remove. */
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

int sg_sequencer_state_remove(const char *git_dir)
{
    char path[SG_PATH_MAX];
    char dir_path[SG_PATH_MAX];
    static const char *seq_names[] = {"head", "abort-safety", "todo"};
    size_t i;
    int rc = 0;

    if (top_path(git_dir, "CHERRY_PICK_HEAD", path, sizeof(path)) != 0)
        rc = -1;
    else if (remove(path) != 0 && errno != ENOENT)
        rc = -1;

    if (top_path(git_dir, "REVERT_HEAD", path, sizeof(path)) != 0)
        rc = -1;
    else if (remove(path) != 0 && errno != ENOENT)
        rc = -1;

    if (top_path(git_dir, "MERGE_MSG", path, sizeof(path)) != 0)
        rc = -1;
    else if (remove(path) != 0 && errno != ENOENT)
        rc = -1;

    if (seq_dir_path(git_dir, dir_path, sizeof(dir_path)) != 0)
        return -1;
    for (i = 0; i < sizeof(seq_names) / sizeof(seq_names[0]); i++) {
        if (seq_file_path(git_dir, seq_names[i], path, sizeof(path)) != 0) {
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

void sg_sequencer_state_free(sg_sequencer_state *st)
{
    free(st->todo);
    st->todo = NULL;
    st->todo_count = 0;
}

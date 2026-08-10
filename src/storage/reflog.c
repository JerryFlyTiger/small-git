#include "sg/reflog.h"

#include "sg/refs.h"
#include "sg/workdir.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SG_PATH_MAX 4096

/* "<40-hex old> <40-hex new> " is 82 bytes (0..81 inclusive) before the ident
   even starts; anything shorter than that -- including the "shorter than 81
   bytes" floor named in the header comment -- cannot possibly hold both
   oid fields, so it is rejected as malformed. */
#define SG_REFLOG_MIN_LINE 82

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static void free_entries(sg_reflog_entry *entries, size_t count)
{
    size_t i;

    if (entries == NULL)
        return;
    for (i = 0; i < count; i++) {
        free(entries[i].ident);
        free(entries[i].message);
    }
    free(entries);
}

/* Parses one line (no trailing newline) into *entry. line_len is the length
   without the newline. Returns 0 on success, -1 if malformed. */
static int parse_line(const char *line, size_t line_len, sg_reflog_entry *entry)
{
    const char *tab;
    size_t ident_len;
    size_t message_len;

    if (line_len < SG_REFLOG_MIN_LINE)
        return -1;
    if (line[40] != ' ' || line[81] != ' ')
        return -1;

    /* the two literal offsets above only make sense once we know both hex
       fields are actually 40 hex digits -- otherwise "line[81]" is reading
       past a short malformed line into the middle of the ident/message. */
    {
        char hex[SG_SHA1_HEX_LEN + 1];

        memcpy(hex, line, SG_SHA1_HEX_LEN);
        hex[SG_SHA1_HEX_LEN] = '\0';
        if (sg_hex_to_sha1(hex, entry->old_id) != 0)
            return -1;

        memcpy(hex, line + 41, SG_SHA1_HEX_LEN);
        hex[SG_SHA1_HEX_LEN] = '\0';
        if (sg_hex_to_sha1(hex, entry->new_id) != 0)
            return -1;
    }

    tab = memchr(line + 82, '\t', line_len - 82);
    if (tab != NULL) {
        ident_len = (size_t)(tab - (line + 82));
        message_len = line_len - 82 - ident_len - 1;
    } else {
        ident_len = line_len - 82;
        message_len = 0;
    }

    entry->ident = malloc(ident_len + 1);
    if (entry->ident == NULL)
        return -1;
    memcpy(entry->ident, line + 82, ident_len);
    entry->ident[ident_len] = '\0';

    entry->message = malloc(message_len + 1);
    if (entry->message == NULL) {
        free(entry->ident);
        entry->ident = NULL;
        return -1;
    }
    if (tab != NULL)
        memcpy(entry->message, tab + 1, message_len);
    entry->message[message_len] = '\0';

    return 0;
}

int sg_reflog_read(const char *git_dir, const char *ref_path, sg_reflog *out)
{
    char full_path[SG_PATH_MAX];
    unsigned char *content;
    size_t content_len;
    sg_reflog_entry *entries = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t pos = 0;

    out->entries = NULL;
    out->count = 0;

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    if (snprintf(full_path, sizeof(full_path), "%s/logs/%s", git_dir, ref_path) >= (int)sizeof(full_path))
        return -1;

    if (sg_read_file(full_path, &content, &content_len) != 0)
        return 0; /* missing file: no reflog yet, not an error */

    while (pos < content_len) {
        const char *nl = memchr((const char *)content + pos, '\n', content_len - pos);
        size_t line_len = (nl != NULL) ? (size_t)((const unsigned char *)nl - (content + pos))
                                       : (content_len - pos);
        sg_reflog_entry entry;

        if (line_len == 0 && nl == NULL)
            break; /* trailing blank at true EOF with no data: ignore */

        if (parse_line((const char *)content + pos, line_len, &entry) != 0) {
            free_entries(entries, count);
            free(content);
            out->entries = NULL;
            out->count = 0;
            return -1;
        }

        if (count == cap) {
            size_t new_cap = (cap == 0) ? 8 : cap * 2;
            sg_reflog_entry *grown = realloc(entries, new_cap * sizeof(*entries));

            if (grown == NULL) {
                free(entry.ident);
                free(entry.message);
                free_entries(entries, count);
                free(content);
                return -1;
            }
            entries = grown;
            cap = new_cap;
        }
        entries[count++] = entry;

        pos += line_len;
        if (nl != NULL)
            pos += 1;
        else
            break; /* last line had no trailing newline: tolerated */
    }

    free(content);
    out->entries = entries;
    out->count = count;
    return 0;
}

void sg_reflog_free(sg_reflog *log)
{
    if (log == NULL)
        return;
    free_entries(log->entries, log->count);
    log->entries = NULL;
    log->count = 0;
}

/* Collapses whitespace the way real git's copy_reflog_msg does: leading
   whitespace dropped, every run of whitespace (including '\n' and '\t')
   becomes a single ' ', trailing whitespace trimmed. Result is malloc'd. */
static char *normalize_message(const char *message)
{
    size_t len = strlen(message);
    char *out = malloc(len + 1);
    size_t i = 0;
    size_t o = 0;
    int pending_space = 0;

    if (out == NULL)
        return NULL;

    while (i < len && isspace((unsigned char)message[i]))
        i++;

    for (; i < len; i++) {
        unsigned char c = (unsigned char)message[i];

        if (isspace(c)) {
            pending_space = 1;
            continue;
        }
        if (pending_space) {
            out[o++] = ' ';
            pending_space = 0;
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return out;
}

int sg_reflog_append(const char *git_dir, const char *ref_path, const unsigned char old_id[SG_SHA1_RAW_LEN],
                     const unsigned char new_id[SG_SHA1_RAW_LEN], const char *message,
                     long long *appended_at_out)
{
    char full_path[SG_PATH_MAX];
    char old_hex[SG_SHA1_HEX_LEN + 1];
    char new_hex[SG_SHA1_HEX_LEN + 1];
    const char *name;
    const char *email;
    char *normalized;
    char *line;
    int line_len;
    FILE *f;
    long before;
    int rc;

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    if (snprintf(full_path, sizeof(full_path), "%s/logs/%s", git_dir, ref_path) >= (int)sizeof(full_path))
        return -1;

    if (sg_mkdir_parents(full_path) != 0)
        return -1;

    normalized = normalize_message(message);
    if (normalized == NULL)
        return -1;

    name = env_or("GIT_AUTHOR_NAME", "small_git");
    email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

    sg_sha1_to_hex(old_id, old_hex);
    sg_sha1_to_hex(new_id, new_hex);

    line_len = snprintf(NULL, 0, "%s %s %s <%s> %lld +0000\t%s\n", old_hex, new_hex, name, email,
                        (long long)time(NULL), normalized);
    if (line_len < 0) {
        free(normalized);
        return -1;
    }
    line = malloc((size_t)line_len + 1);
    if (line == NULL) {
        free(normalized);
        return -1;
    }
    snprintf(line, (size_t)line_len + 1, "%s %s %s <%s> %lld +0000\t%s\n", old_hex, new_hex, name, email,
             (long long)time(NULL), normalized);
    free(normalized);

    f = fopen(full_path, "ab");
    if (f == NULL) {
        free(line);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        free(line);
        return -1;
    }
    before = ftell(f);
    if (before < 0) {
        fclose(f);
        free(line);
        return -1;
    }

    rc = (fwrite(line, 1, (size_t)line_len, f) == (size_t)line_len) ? 0 : -1;
    free(line);
    if (fclose(f) != 0)
        rc = -1;
    if (rc != 0)
        return -1;

    if (appended_at_out != NULL)
        *appended_at_out = (long long)before;
    return 0;
}

int sg_reflog_rewrite(const char *git_dir, const char *ref_path, const sg_reflog_entry *entries,
                      size_t count)
{
    char full_path[SG_PATH_MAX];
    char tmp_path[SG_PATH_MAX];
    unsigned char zero_id[SG_SHA1_RAW_LEN];
    FILE *f;
    size_t i;

    if (!sg_ref_branch_name_is_safe(ref_path))
        return -1;

    if (snprintf(full_path, sizeof(full_path), "%s/logs/%s", git_dir, ref_path) >= (int)sizeof(full_path))
        return -1;

    if (count == 0) {
        remove(full_path); /* not-found is fine: nothing to remove */
        return 0;
    }

    if (sg_mkdir_parents(full_path) != 0)
        return -1;

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", full_path) >= (int)sizeof(tmp_path))
        return -1;

    f = fopen(tmp_path, "wb");
    if (f == NULL)
        return -1;

    memset(zero_id, 0, sizeof(zero_id));

    for (i = 0; i < count; i++) {
        char old_hex[SG_SHA1_HEX_LEN + 1];
        char new_hex[SG_SHA1_HEX_LEN + 1];
        const unsigned char *old_id = (i == 0) ? zero_id : entries[i - 1].new_id;

        sg_sha1_to_hex(old_id, old_hex);
        sg_sha1_to_hex(entries[i].new_id, new_hex);

        if (fprintf(f, "%s %s %s\t%s\n", old_hex, new_hex, entries[i].ident, entries[i].message) < 0) {
            fclose(f);
            remove(tmp_path);
            return -1;
        }
    }

    if (fclose(f) != 0) {
        remove(tmp_path);
        return -1;
    }

    if (rename(tmp_path, full_path) != 0) {
        remove(tmp_path);
        return -1;
    }

    return 0;
}

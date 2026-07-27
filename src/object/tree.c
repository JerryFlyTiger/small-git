#include "sg/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* git orders tree entries as if a directory's name had a trailing '/', so that
   e.g. "foo" (a file) sorts before "foo.c" but "foo/" (a dir) sorts after it */
static int tree_name_cmp(const char *name1, size_t len1, unsigned int mode1, const char *name2,
                         size_t len2, unsigned int mode2)
{
    size_t len = len1 < len2 ? len1 : len2;
    int cmp = memcmp(name1, name2, len);
    unsigned char c1, c2;

    if (cmp != 0)
        return cmp;

    c1 = (unsigned char)name1[len];
    c2 = (unsigned char)name2[len];
    if (c1 == '\0' && (mode1 & S_IFMT) == S_IFDIR)
        c1 = '/';
    if (c2 == '\0' && (mode2 & S_IFMT) == S_IFDIR)
        c2 = '/';
    return (c1 < c2) ? -1 : (c1 > c2) ? 1 : 0;
}

static int tree_entry_cmp(const void *a, const void *b)
{
    const sg_tree_entry *ea = a;
    const sg_tree_entry *eb = b;

    return tree_name_cmp(ea->name, strlen(ea->name), ea->mode, eb->name, strlen(eb->name),
                        eb->mode);
}

int sg_tree_serialize(const sg_tree_entry *entries, size_t count, unsigned char **out,
                      size_t *out_len)
{
    sg_tree_entry *sorted;
    size_t total = 0;
    size_t i;
    unsigned char *buf;
    unsigned char *p;

    sorted = malloc(count * sizeof(*sorted));
    if (sorted == NULL && count > 0)
        return -1;
    if (count > 0) {
        memcpy(sorted, entries, count * sizeof(*sorted));
        qsort(sorted, count, sizeof(*sorted), tree_entry_cmp);
    }

    for (i = 0; i < count; i++) {
        char mode_buf[16];
        int mode_len = snprintf(mode_buf, sizeof(mode_buf), "%o", sorted[i].mode);

        total += (size_t)mode_len + 1 + strlen(sorted[i].name) + 1 + SG_SHA1_RAW_LEN;
    }

    buf = malloc(total > 0 ? total : 1);
    if (buf == NULL) {
        free(sorted);
        return -1;
    }

    p = buf;
    for (i = 0; i < count; i++) {
        char mode_buf[16];
        int mode_len = snprintf(mode_buf, sizeof(mode_buf), "%o", sorted[i].mode);
        size_t name_len = strlen(sorted[i].name);

        memcpy(p, mode_buf, (size_t)mode_len);
        p += mode_len;
        *p++ = ' ';
        memcpy(p, sorted[i].name, name_len);
        p += name_len;
        *p++ = '\0';
        memcpy(p, sorted[i].sha1, SG_SHA1_RAW_LEN);
        p += SG_SHA1_RAW_LEN;
    }

    free(sorted);
    *out = buf;
    *out_len = total;
    return 0;
}

int sg_tree_parse(const unsigned char *content, size_t content_len, sg_tree *tree_out)
{
    size_t cap = 8;
    size_t count = 0;
    sg_tree_entry *entries = malloc(cap * sizeof(*entries));
    const unsigned char *p = content;
    const unsigned char *end = content + content_len;

    if (entries == NULL)
        return -1;

    while (p < end) {
        const unsigned char *space = memchr(p, ' ', (size_t)(end - p));
        const unsigned char *nul;
        unsigned int mode;
        char *mode_end;
        size_t name_len;
        char *name;

        if (space == NULL)
            goto fail;

        mode = (unsigned int)strtoul((const char *)p, &mode_end, 8);
        if (mode_end != (const char *)space)
            goto fail;

        nul = memchr(space + 1, '\0', (size_t)(end - space - 1));
        if (nul == NULL)
            goto fail;

        name_len = (size_t)(nul - space - 1);
        if ((size_t)(end - (nul + 1)) < SG_SHA1_RAW_LEN)
            goto fail;

        name = malloc(name_len + 1);
        if (name == NULL)
            goto fail;
        memcpy(name, space + 1, name_len);
        name[name_len] = '\0';

        if (count == cap) {
            sg_tree_entry *grown;

            cap *= 2;
            grown = realloc(entries, cap * sizeof(*entries));
            if (grown == NULL) {
                free(name);
                goto fail;
            }
            entries = grown;
        }

        entries[count].mode = mode;
        entries[count].name = name;
        memcpy(entries[count].sha1, nul + 1, SG_SHA1_RAW_LEN);
        count++;

        p = nul + 1 + SG_SHA1_RAW_LEN;
    }

    tree_out->entries = entries;
    tree_out->count = count;
    return 0;

fail:
    {
        size_t i;

        for (i = 0; i < count; i++)
            free(entries[i].name);
        free(entries);
    }
    return -1;
}

void sg_tree_free(sg_tree *tree)
{
    size_t i;

    for (i = 0; i < tree->count; i++)
        free(tree->entries[i].name);
    free(tree->entries);
    tree->entries = NULL;
    tree->count = 0;
}

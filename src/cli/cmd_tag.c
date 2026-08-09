#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/revparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char USAGE[] =
    "usage: sg tag [-a] [-m <msg>] [-f|--force] [--] <name> [<rev>]\n"
    "       sg tag -d <name>\n"
    "       sg tag\n";

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

static int list_tags(const char *git_dir)
{
    char **names;
    size_t count;
    size_t i;

    if (sg_ref_list_under(git_dir, "refs/tags/", &names, &count) != 0) {
        fprintf(stderr, "sg: 無法列出標籤\n");
        return 1;
    }

    for (i = 0; i < count; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

/* True if refs/tags/<name> already exists (loose or packed). */
static int tag_exists(const char *git_dir, const char *name)
{
    unsigned char id[SG_SHA1_RAW_LEN];
    char ref_path[4096];

    if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name) >= (int)sizeof(ref_path))
        return 0;
    return sg_ref_read_path(git_dir, ref_path, id) == 0;
}

static int create_tag(const char *git_dir, const char *name, const char *rev, int annotated,
                      const char *message, int force)
{
    unsigned char target_id[SG_SHA1_RAW_LEN];
    char ref_path[4096];

    if (!sg_ref_name_valid_for_create(name)) {
        fprintf(stderr, "sg: '%s' 不是有效的標籤名稱\n", name);
        return 1;
    }
    if (!force && tag_exists(git_dir, name)) {
        fprintf(stderr, "sg: 標籤 '%s' 已經存在\n", name);
        return 1;
    }
    if (sg_rev_parse_commit(git_dir, rev != NULL ? rev : "HEAD", target_id) != 0) {
        fprintf(stderr, "sg: 無法解析 '%s'\n", rev != NULL ? rev : "HEAD");
        return 1;
    }

    if (snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", name) >= (int)sizeof(ref_path)) {
        fprintf(stderr, "sg: 標籤名稱太長\n");
        return 1;
    }

    if (annotated) {
        sg_tag tag;
        unsigned char *serialized;
        size_t serialized_len;
        unsigned char tag_id[SG_SHA1_RAW_LEN];
        const char *tagger_name = env_or("GIT_AUTHOR_NAME", "small_git");
        const char *tagger_email = env_or("GIT_AUTHOR_EMAIL", "sg@localhost");

        memset(&tag, 0, sizeof(tag));
        memcpy(tag.object, target_id, SG_SHA1_RAW_LEN);
        tag.object_type = SG_OBJ_COMMIT;
        tag.tag_name = (char *)name;
        tag.tagger_name = (char *)tagger_name;
        tag.tagger_email = (char *)tagger_email;
        tag.tagger_time = (long long)time(NULL);
        strcpy(tag.tagger_tz, "+0000");
        tag.message = (char *)message;

        if (sg_tag_serialize(&tag, &serialized, &serialized_len) != 0) {
            fprintf(stderr, "sg: 無法序列化標籤物件\n");
            return 1;
        }
        if (sg_loose_write(git_dir, SG_OBJ_TAG, serialized, serialized_len, tag_id) != 0) {
            fprintf(stderr, "sg: 無法寫入標籤物件\n");
            free(serialized);
            return 1;
        }
        free(serialized);

        if (sg_ref_write_path(git_dir, ref_path, tag_id) != 0) {
            fprintf(stderr, "sg: 無法建立標籤 '%s'\n", name);
            return 1;
        }
    } else {
        if (sg_ref_write_path(git_dir, ref_path, target_id) != 0) {
            fprintf(stderr, "sg: 無法建立標籤 '%s'\n", name);
            return 1;
        }
    }

    return 0;
}

static int delete_tag(const char *git_dir, const char *name)
{
    int rc = sg_ref_delete_under(git_dir, "refs/tags/", name);

    if (rc == 1) {
        fprintf(stderr, "sg: 找不到標籤 '%s'\n", name);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: 刪除標籤 '%s' 失敗\n", name);
        return 1;
    }
    printf("已刪除標籤 '%s'\n", name);
    return 0;
}

int sg_cmd_tag(int argc, char **argv)
{
    int del = 0;
    int force = 0;
    int annotated = 0;
    int opts_done = 0;
    const char *message = NULL;
    const char *name = NULL;
    const char *rev = NULL;
    char *git_dir;
    int rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (!opts_done && strcmp(argv[i], "--") == 0) {
            opts_done = 1;
        } else if (!opts_done && (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--delete") == 0)) {
            del = 1;
        } else if (!opts_done && (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)) {
            force = 1;
        } else if (!opts_done && strcmp(argv[i], "-a") == 0) {
            annotated = 1;
        } else if (!opts_done && strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                fputs(USAGE, stderr);
                return 1;
            }
            message = argv[++i];
        } else if (!opts_done && argv[i][0] == '-') {
            fputs(USAGE, stderr);
            return 1;
        } else if (name == NULL) {
            name = argv[i];
        } else if (rev == NULL) {
            rev = argv[i];
        } else {
            fputs(USAGE, stderr);
            return 1;
        }
    }

    if (del && name == NULL) {
        fputs(USAGE, stderr);
        return 1;
    }
    if (annotated && message == NULL) {
        fputs(USAGE, stderr);
        return 1;
    }
    /* -m without -a implicitly means an annotated tag, matching real git. */
    if (message != NULL)
        annotated = 1;

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (del)
        rc = delete_tag(git_dir, name);
    else if (name != NULL)
        rc = create_tag(git_dir, name, rev, annotated, message, force);
    else
        rc = list_tags(git_dir);

    free(git_dir);
    return rc;
}

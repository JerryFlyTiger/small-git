#include "sg/cli.h"

#include "sg/confirm.h"
#include "sg/hash.h"
#include "sg/merge.h"
#include "sg/refs.h"
#include "sg/repo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char USAGE[] = "usage: sg branch [-d|--delete] [--force|-f] [--] [<name>]\n";

/* Stricter than sg_ref_branch_name_is_safe, and applied only when CREATING a
   branch: existing refs, however they were named, must still list and delete.
   This is the subset of git-check-ref-format rules measured against real git
   (leading '-', "a..b", "a b", "a.lock", "HEAD", "a/", "@{x}" all rejected
   there). */
static int branch_name_valid_for_create(const char *name)
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

static int list_branches(const char *git_dir)
{
    char **names;
    size_t count;
    size_t i;
    char *current;

    if (sg_ref_list_branches(git_dir, &names, &count) != 0) {
        fprintf(stderr, "sg: 無法列出分支\n");
        return 1;
    }

    current = sg_ref_current_branch(git_dir);
    for (i = 0; i < count; i++) {
        int is_current = current != NULL && strcmp(names[i], current) == 0;

        printf("%s %s\n", is_current ? "*" : " ", names[i]);
        free(names[i]);
    }
    free(names);
    free(current);
    return 0;
}

static int create_branch(const char *git_dir, const char *name)
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    char ref_path[4096];

    if (!branch_name_valid_for_create(name)) {
        fprintf(stderr, "sg: '%s' 不是有效的分支名稱\n", name);
        return 1;
    }
    if (sg_ref_branch_exists(git_dir, name)) {
        fprintf(stderr, "sg: 分支 '%s' 已經存在\n", name);
        return 1;
    }
    if (sg_ref_resolve_head(git_dir, head_id) != 0) {
        fprintf(stderr, "sg: 無法建立分支 '%s'：目前的分支還沒有任何 commit\n", name);
        return 1;
    }
    /* sg_ref_write_path (not sg_ref_update_branch) so that slash-containing
       names like feature/x get their parent directories created. The name
       comes from argv: refusing an over-long one beats silently truncating
       it into a DIFFERENT (possibly valid) branch name. */
    if (snprintf(ref_path, sizeof(ref_path), "refs/heads/%s", name) >= (int)sizeof(ref_path)) {
        fprintf(stderr, "sg: 分支名稱太長\n");
        return 1;
    }
    if (sg_ref_write_path(git_dir, ref_path, head_id) != 0) {
        fprintf(stderr, "sg: 無法建立分支 '%s'\n", name);
        return 1;
    }
    return 0;
}

/* Composes the unmerged-delete warning on the heap: name is argv-controlled
   and may be arbitrarily long, so no fixed-size truncation here. */
static char *compose_unmerged_msg(const char *name, const char *tip_hex)
{
    static const char fmt[] =
        "sg branch: 分支 '%s' 尚未合併到 HEAD（目前指向 %s），刪除後將失去其獨有的 commit。\n";
    int need = snprintf(NULL, 0, fmt, name, tip_hex);
    char *msg;

    if (need < 0)
        return NULL;
    msg = malloc((size_t)need + 1);
    if (msg == NULL)
        return NULL;
    snprintf(msg, (size_t)need + 1, fmt, name, tip_hex);
    return msg;
}

/* Branch X is provably merged iff merge_base(HEAD, X) == tip(X) (X is an
   ancestor of HEAD, or HEAD itself). Everything unprovable -- unrelated
   history, criss-cross bases, unresolvable HEAD -- counts as NOT merged and
   goes through the confirmation. Returns 1 merged, 0 not provably merged,
   -1 on a real error. */
static int branch_is_merged(const char *git_dir, const unsigned char tip[SG_SHA1_RAW_LEN])
{
    unsigned char head_id[SG_SHA1_RAW_LEN];
    unsigned char base[SG_SHA1_RAW_LEN];
    int rc;

    if (sg_ref_resolve_head(git_dir, head_id) != 0)
        return 0;
    if (memcmp(head_id, tip, SG_SHA1_RAW_LEN) == 0)
        return 1;
    rc = sg_merge_base(git_dir, head_id, tip, base);
    if (rc == 0)
        return memcmp(base, tip, SG_SHA1_RAW_LEN) == 0;
    if (rc == -1 || rc == -2)
        return 0;
    return -1;
}

static int delete_branch(const char *git_dir, const char *name, int force)
{
    unsigned char tip[SG_SHA1_RAW_LEN];
    char tip_hex[SG_SHA1_HEX_LEN + 1];
    char *current;
    int merged;
    int rc;

    /* Refusing to delete the checked-out branch is correctness, not
       caution: HEAD would be left pointing at nothing. --force does not
       override it. */
    current = sg_ref_current_branch(git_dir);
    if (current != NULL && strcmp(current, name) == 0) {
        fprintf(stderr, "sg: 無法刪除目前所在的分支 '%s'\n", name);
        free(current);
        return 1;
    }
    free(current);

    if (!sg_ref_branch_exists(git_dir, name)) {
        fprintf(stderr, "sg: 找不到分支 '%s'\n", name);
        return 1;
    }
    if (sg_ref_read_branch(git_dir, name, tip) != 0) {
        fprintf(stderr, "sg: 無法讀取分支 '%s'\n", name);
        return 1;
    }
    sg_sha1_to_hex(tip, tip_hex);

    merged = branch_is_merged(git_dir, tip);
    if (merged < 0) {
        fprintf(stderr, "sg: 無法判斷分支 '%s' 是否已合併\n", name);
        return 1;
    }
    if (!merged) {
        char *msg = compose_unmerged_msg(name, tip_hex);
        int ok;

        if (msg == NULL) {
            fprintf(stderr, "sg: 記憶體不足\n");
            return 1;
        }
        ok = sg_confirm_dangerous(msg, force);
        free(msg);
        if (!ok) {
            fprintf(stderr, "sg: 已取消刪除分支\n");
            return 1;
        }
    }

    rc = sg_ref_delete_branch(git_dir, name);
    if (rc == 1) {
        fprintf(stderr, "sg: 找不到分支 '%s'\n", name);
        return 1;
    }
    if (rc != 0) {
        fprintf(stderr, "sg: 刪除分支 '%s' 失敗\n", name);
        return 1;
    }

    /* The full old tip is the ONLY recovery path -- snapshots cover the
       index and worktree, never refs -- so print all 40 hex digits. */
    printf("已刪除分支 '%s'（原指向 %s）\n", name, tip_hex);
    return 0;
}

int sg_cmd_branch(int argc, char **argv)
{
    int del = 0;
    int force = 0;
    int opts_done = 0;
    const char *name = NULL;
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
        } else if (!opts_done && argv[i][0] == '-') {
            fputs(USAGE, stderr);
            return 1;
        } else if (name == NULL) {
            name = argv[i];
        } else {
            fputs(USAGE, stderr);
            return 1;
        }
    }
    if (del && name == NULL) {
        fputs(USAGE, stderr);
        return 1;
    }

    git_dir = sg_require_git_dir();
    if (git_dir == NULL)
        return 1;

    if (del)
        rc = delete_branch(git_dir, name, force);
    else if (name != NULL)
        rc = create_branch(git_dir, name);
    else
        rc = list_branches(git_dir);

    free(git_dir);
    return rc;
}

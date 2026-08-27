#include "sg/cli.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/merge.h"
#include "sg/objstore.h"
#include "sg/object.h"
#include "sg/quote.h"
#include "sg/rebase.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/status.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char USAGE[] =
    "usage: sg status [-s|--short|--porcelain] [-b|--branch] [--ignored] "
    "[-u<mode>|--untracked-files=<mode>]\n";

/* -u<mode>/--untracked-files=<mode>. Measured against git 2.55.0: bare "-u"
   (no attached mode) behaves like "-uall", NOT like the flagless default --
   see parse_untracked_mode's caller for why that has to be handled before
   ever looking at an attached suffix. A *separate* argv token "no" after a
   bare "-u" is NOT the same as "-uno" (git treats it as a pathspec instead);
   sg has no pathspec support, so that shape simply falls through to the
   usage error below, which is the correct way to avoid silently reinterpreting
   it as a mode string. STATUS_U_NORMAL is 0 so status_opts's memset-to-zero
   below already selects it as the default. */
typedef enum {
    STATUS_U_NORMAL, /* fold a wholly-untracked dir into "dir/" -- the default */
    STATUS_U_ALL,     /* never fold: one line per untracked file */
    STATUS_U_NO,      /* list no untracked (or ignored) paths at all */
} status_u_mode;

typedef struct {
    int porcelain;
    int branch;
    int ignored;
    status_u_mode u_mode;
} status_opts;

/* Parses the mode suffix of -u<mode>/--untracked-files=<mode> (everything
   after the "-u" or "--untracked-files=" prefix has already been stripped
   by the caller). Returns 0 and sets *out on a recognized mode ("all",
   "normal", "no"); -1 (leaving *out untouched) on anything else, which the
   caller turns into the usual usage error. */
static int parse_untracked_mode(const char *s, status_u_mode *out)
{
    if (strcmp(s, "all") == 0) {
        *out = STATUS_U_ALL;
        return 0;
    }
    if (strcmp(s, "normal") == 0) {
        *out = STATUS_U_NORMAL;
        return 0;
    }
    if (strcmp(s, "no") == 0) {
        *out = STATUS_U_NO;
        return 0;
    }
    return -1;
}

static const char *kind_label(sg_status_kind kind)
{
    switch (kind) {
    case SG_STATUS_NEW:
        return "new file:   ";
    case SG_STATUS_MODIFIED:
        return "modified:   ";
    case SG_STATUS_DELETED:
        return "deleted:    ";
    }
    return "";
}

static char kind_char(sg_status_kind kind)
{
    switch (kind) {
    case SG_STATUS_NEW:
        return 'A';
    case SG_STATUS_MODIFIED:
        return 'M';
    case SG_STATUS_DELETED:
        return 'D';
    }
    return '?';
}

/* Single source of truth for "which stages are present at this unmerged
   path -> which two-letter porcelain code / which long-format label",
   shared by the long-format and porcelain printers below so the two
   formats cannot silently drift apart. Every unresolved-conflict path has
   at least one of stage1/stage2/stage3 set (that is what "unresolved
   conflict" means), so the fallback branch is unreachable in practice --
   it exists only so the function always initializes its outputs. */
static void unmerged_label(int stage1, int stage2, int stage3, char code[3], const char **label)
{
    if (stage1 && !stage2 && !stage3) {
        code[0] = 'D';
        code[1] = 'D';
        *label = "both deleted:";
    } else if (stage1 && stage2 && !stage3) {
        code[0] = 'U';
        code[1] = 'D';
        *label = "deleted by them:";
    } else if (stage1 && stage2 && stage3) {
        code[0] = 'U';
        code[1] = 'U';
        *label = "both modified:";
    } else if (stage1 && !stage2 && stage3) {
        code[0] = 'D';
        code[1] = 'U';
        *label = "deleted by us:";
    } else if (!stage1 && stage2 && !stage3) {
        code[0] = 'A';
        code[1] = 'U';
        *label = "added by us:";
    } else if (!stage1 && stage2 && stage3) {
        code[0] = 'A';
        code[1] = 'A';
        *label = "both added:";
    } else if (!stage1 && !stage2 && stage3) {
        code[0] = 'U';
        code[1] = 'A';
        *label = "added by them:";
    } else {
        code[0] = '?';
        code[1] = '?';
        *label = "unmerged:";
    }
    code[2] = '\0';
}

static void print_section(const char *title, const char **hints, size_t hint_count,
                          const sg_status_list *list)
{
    size_t i;

    if (list->count == 0)
        return;
    printf("%s\n", title);
    for (i = 0; i < hint_count; i++)
        printf("  (%s)\n", hints[i]);
    for (i = 0; i < list->count; i++)
        printf("\t%s%s\n", kind_label(list->entries[i].kind),
              sg_quote_path(list->entries[i].path));
    printf("\n");
}

/* Prints every distinct path carrying a stage 1/2/3 entry (idx is sorted by
   (path, stage), so duplicates for a path are contiguous), each with the
   long-format label unmerged_label derives from which stages are present.
   Returns the number of distinct unmerged paths found. */
static size_t print_unmerged(const sg_index *idx, int rebase_in_progress)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < idx->count; i++) {
        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        count++;
    }
    if (count == 0)
        return 0;

    printf("Unmerged paths:\n");
    printf("  (use \"sg add <file>...\" to mark resolution)\n");
    if (rebase_in_progress)
        printf("  (use \"sg rebase --abort\" to check out the original branch)\n");
    else
        printf("  (use \"sg merge --abort\" to abort the merge)\n");
    for (i = 0; i < idx->count; i++) {
        char code[3];
        const char *label;

        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        unmerged_label(sg_index_find_stage(idx, idx->entries[i].path, 1) >= 0,
                      sg_index_find_stage(idx, idx->entries[i].path, 2) >= 0,
                      sg_index_find_stage(idx, idx->entries[i].path, 3) >= 0, code, &label);
        printf("\t%-17s%s\n", label, sg_quote_path(idx->entries[i].path));
    }
    printf("\n");
    return count;
}

/* One porcelain line's worth of X/Y status. path is a borrowed pointer into
   one of the source lists (staged, unstaged, or an idx entry's path) --
   never freed here. */
typedef struct {
    const char *path;
    char x;
    char y;
} prow;

static int prow_cmp(const void *a, const void *b)
{
    return strcmp(((const prow *)a)->path, ((const prow *)b)->path);
}

static int prow_append(prow **rows, size_t *count, size_t *cap, const char *path, char x, char y)
{
    if (*count == *cap) {
        size_t new_cap = *cap == 0 ? 16 : *cap * 2;
        prow *grown = realloc(*rows, new_cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        *rows = grown;
        *cap = new_cap;
    }
    (*rows)[*count].path = path;
    (*rows)[*count].x = x;
    (*rows)[*count].y = y;
    (*count)++;
    return 0;
}

/* Prints the tracked (staged/unstaged/unmerged) portion of `status
   --short`/`--porcelain`: one "XY path" line per distinct path, sorted by
   path, X = HEAD-vs-index and Y = index-vs-worktree ('D'/'U' pairs from
   unmerged_label for an unresolved conflict, or 'A'/'M'/'D'/' ' otherwise).
   Untracked and ignored paths are printed separately by the caller, in
   their own path-sorted batches, never interleaved with this section (see
   status.h's fold documentation and the task's measured ordering).
   Returns 0 on success, -1 on allocation failure (nothing has been printed
   in that case beyond whatever fprintf itself managed). */
static int print_porcelain_tracked(const sg_index *idx, const sg_status_list *staged,
                                   const sg_status_list *unstaged)
{
    prow *rows = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i;
    int rc = 0;

    for (i = 0; i < idx->count; i++) {
        char code[3];
        const char *label;

        if (idx->entries[i].stage == 0)
            continue;
        if (i > 0 && strcmp(idx->entries[i].path, idx->entries[i - 1].path) == 0)
            continue;
        unmerged_label(sg_index_find_stage(idx, idx->entries[i].path, 1) >= 0,
                      sg_index_find_stage(idx, idx->entries[i].path, 2) >= 0,
                      sg_index_find_stage(idx, idx->entries[i].path, 3) >= 0, code, &label);
        (void)label; /* porcelain output only needs the two-letter code */
        if (prow_append(&rows, &count, &cap, idx->entries[i].path, code[0], code[1]) != 0) {
            rc = -1;
            goto out;
        }
    }
    for (i = 0; i < staged->count; i++) {
        if (prow_append(&rows, &count, &cap, staged->entries[i].path,
                        kind_char(staged->entries[i].kind), ' ') != 0) {
            rc = -1;
            goto out;
        }
    }
    for (i = 0; i < unstaged->count; i++) {
        if (prow_append(&rows, &count, &cap, unstaged->entries[i].path, ' ',
                        kind_char(unstaged->entries[i].kind)) != 0) {
            rc = -1;
            goto out;
        }
    }

    if (count > 0)
        qsort(rows, count, sizeof(*rows), prow_cmp);

    /* Compact adjacent same-path rows (a path can have both a staged and an
       unstaged row): keep the first non-space x and the first non-space y
       seen across the group. Unmerged rows never collide with staged/
       unstaged rows here -- the former only ever come from stage 1/2/3
       entries, the latter only from stage 0 -- so a group is always either
       one unmerged row or up to two staged/unstaged rows. */
    for (i = 0; i < count;) {
        size_t j = i + 1;
        char x = rows[i].x;
        char y = rows[i].y;

        while (j < count && strcmp(rows[j].path, rows[i].path) == 0) {
            if (rows[j].x != ' ')
                x = rows[j].x;
            if (rows[j].y != ' ')
                y = rows[j].y;
            j++;
        }
        printf("%c%c %s\n", x, y, sg_quote_path_porcelain(rows[i].path));
        i = j;
    }

out:
    free(rows);
    return rc;
}

static void print_porcelain_paths(const char *prefix, char **paths, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        printf("%s%s\n", prefix, sg_quote_path_porcelain(paths[i]));
}

/* Prints the "## ..." branch header used only by `status -b/--branch`'s
   porcelain form -- deliberately not shared with the long format's "On
   branch"/"Not currently on any branch" wording (cmd_status.c's main body
   keeps printing that unconditionally, matching git, where -b only ever
   affects porcelain output). detached is judged with
   sg_ref_head_is_detached rather than "sg_ref_resolve_head failed", per
   CLAUDE.md: that failure means unborn HEAD now, not detached. */
static void print_porcelain_branch_header(const char *git_dir, const char *branch)
{
    if (branch != NULL) {
        unsigned char head_id[SG_SHA1_RAW_LEN];

        if (sg_ref_resolve_head(git_dir, head_id) == 0)
            printf("## %s\n", branch);
        else
            printf("## No commits yet on %s\n", branch);
        return;
    }
    if (sg_ref_head_is_detached(git_dir) == 1) {
        printf("## HEAD (no branch)\n");
        return;
    }
    /* Corrupt HEAD: no measured porcelain wording for this state, and the
       long format's own fallback line already covers it outside -b. */
    printf("## HEAD (no branch)\n");
}

/* diff_out receives every path in b_paths that is not also in a_paths
   (both inputs sorted by path, as sg_status_list_untracked guarantees) --
   i.e. exactly the ignored paths that a folded FOLD_DIRS untracked walk
   with include_ignored=1 adds on top of the include_ignored=0 walk. This
   is the only way cmd_status.c tells "?? " apart from "!! " without
   sg_status_list_untracked itself gaining a per-entry ignored flag (out of
   scope for this change -- see status.h). Returns 0 on success, -1 on
   allocation failure; *diff_out and *diff_count are left at NULL/0 on failure. */
static int list_diff_sorted(char **a_paths, size_t a_count, char **b_paths, size_t b_count,
                            char ***diff_out, size_t *diff_count)
{
    char **out = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t ai = 0, bi = 0;

    *diff_out = NULL;
    *diff_count = 0;

    while (bi < b_count) {
        int cmp;

        if (ai >= a_count)
            cmp = 1;
        else
            cmp = strcmp(a_paths[ai], b_paths[bi]);

        if (cmp == 0) {
            ai++;
            bi++;
        } else if (cmp < 0) {
            ai++;
        } else {
            if (count == cap) {
                size_t new_cap = cap == 0 ? 16 : cap * 2;
                char **grown = realloc(out, new_cap * sizeof(*grown));

                if (grown == NULL)
                    goto fail;
                out = grown;
                cap = new_cap;
            }
            out[count] = strdup(b_paths[bi]);
            if (out[count] == NULL)
                goto fail;
            count++;
            bi++;
        }
    }

    *diff_out = out;
    *diff_count = count;
    return 0;

fail: {
    size_t i;

    for (i = 0; i < count; i++)
        free(out[i]);
    free(out);
    return -1;
}
}

int sg_cmd_status(int argc, char **argv)
{
    status_opts opts;
    char *git_dir;
    char *repo_root;
    char *branch;
    sg_index idx;
    unsigned char head_commit_id[SG_SHA1_RAW_LEN];
    int has_head;
    sg_flat_list head_flat;
    sg_status_list staged;
    sg_status_list unstaged;
    char **untracked = NULL;
    size_t untracked_count = 0;
    char **ignored = NULL;
    size_t ignored_count = 0;
    size_t unmerged_count;
    size_t i;
    int arg_i;
    static const char *staged_hints[] = {
        "use \"sg restore --staged <file>...\" to unstage",
    };
    static const char *unstaged_hints[] = {
        "use \"sg add <file>...\" to update what will be committed",
        "use \"sg restore <file>...\" to discard changes in working directory",
    };

    memset(&opts, 0, sizeof(opts));
    for (arg_i = 1; arg_i < argc; arg_i++) {
        if (strcmp(argv[arg_i], "-s") == 0 || strcmp(argv[arg_i], "--short") == 0 ||
           strcmp(argv[arg_i], "--porcelain") == 0) {
            opts.porcelain = 1;
        } else if (strcmp(argv[arg_i], "-b") == 0 || strcmp(argv[arg_i], "--branch") == 0) {
            opts.branch = 1;
        } else if (strcmp(argv[arg_i], "--ignored") == 0) {
            opts.ignored = 1;
        } else if (strcmp(argv[arg_i], "-u") == 0) {
            opts.u_mode = STATUS_U_ALL; /* bare -u == -uall, measured (git 2.55.0) */
        } else if (strncmp(argv[arg_i], "-u", 2) == 0 && argv[arg_i][2] != '\0') {
            if (parse_untracked_mode(argv[arg_i] + 2, &opts.u_mode) != 0) {
                fputs(USAGE, stderr);
                return 1;
            }
        } else if (strncmp(argv[arg_i], "--untracked-files=", strlen("--untracked-files=")) == 0) {
            if (parse_untracked_mode(argv[arg_i] + strlen("--untracked-files="), &opts.u_mode) !=
               0) {
                fputs(USAGE, stderr);
                return 1;
            }
        } else {
            fputs(USAGE, stderr);
            return 1;
        }
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
    branch = sg_ref_current_branch(git_dir);

    if (opts.porcelain) {
        if (opts.branch)
            print_porcelain_branch_header(git_dir, branch);
    } else if (branch != NULL) {
        printf("On branch %s\n", branch);
    } else {
        char detached[4160]; /* fits any ref path sg can build (SG_PATH_MAX) plus the wording */

        /* Only a real detached HEAD gets git's detached wording; a HEAD that
           is neither a branch symref nor a raw id is corrupt, and claiming
           it is detached would describe a broken repository as a normal
           state. Both of the remaining cases land on git's own fallback
           line, which it also uses for a detached HEAD whose reflog records
           no checkout (measured, 2.55.0). */
        if (sg_ref_head_is_detached(git_dir) == 1 &&
           sg_ref_detach_description(git_dir, detached, sizeof(detached)) == 0)
            printf("%s\n", detached);
        else
            printf("Not currently on any branch.\n");
    }

    if (!opts.porcelain) {
        sg_rebase_state rstate;

        if (sg_rebase_state_exists(git_dir) && sg_rebase_state_read(git_dir, &rstate) == 0) {
            char onto_hex[SG_SHA1_HEX_LEN + 1];
            char onto_short[8];
            size_t remaining = rstate.todo_count + (rstate.has_current ? 1 : 0);

            sg_sha1_to_hex(rstate.onto, onto_hex);
            memcpy(onto_short, onto_hex, 7);
            onto_short[7] = '\0';
            if (rstate.orig_branch != NULL)
                printf("You are currently rebasing branch '%s' onto %s.\n", rstate.orig_branch,
                      onto_short);
            else
                printf("You are currently rebasing.\n");
            printf("(%zu commits left to process)\n", remaining);
            if (rstate.has_current) {
                printf("  (fix conflicts and run \"sg rebase --continue\")\n"
                      "  (use \"sg rebase --skip\" to skip this patch)\n"
                      "  (use \"sg rebase --abort\" to check out the original branch)\n");
            }
            printf("\n");
            sg_rebase_state_free(&rstate);
        }

        /* Existence, not parseability: real git reports a corrupt MERGE_HEAD
           as an ongoing merge in `status` just like a well-formed one
           (measured, 2.55.0), and status must agree with the gates in
           switch/commit about whether a merge is in flight. */
        if (sg_merge_head_exists(git_dir)) {
            if (sg_index_has_unmerged(&idx))
                printf("You have unmerged paths.\n");
            else
                printf("All conflicts fixed but you are still merging.\n");
        }
    }
    free(branch);

    has_head = (sg_ref_resolve_head(git_dir, head_commit_id) == 0);
    memset(&head_flat, 0, sizeof(head_flat));
    if (has_head) {
        sg_obj_type type;
        unsigned char *content = NULL;
        size_t content_len;
        sg_commit commit;

        if (sg_object_read(git_dir, head_commit_id, &type, &content, &content_len) == 0 &&
           type == SG_OBJ_COMMIT) {
            if (sg_commit_parse(content, content_len, &commit) == 0) {
                sg_tree_flatten(git_dir, commit.tree, &head_flat, NULL);
                sg_commit_free(&commit);
            }
            free(content);
        }
    } else if (!opts.porcelain) {
        printf("\nNo commits yet\n");
    }

    if (sg_status_diff_staged(&head_flat, &idx, &staged) != 0)
        fprintf(stderr, "sg: warning: out of memory computing staged changes\n");
    sg_flat_list_free(&head_flat);

    if (sg_status_diff_unstaged(git_dir, repo_root, &idx, &unstaged) != 0)
        fprintf(stderr,
               "sg: warning: failed to compute unstaged changes (out of memory, or a path too "
               "long)\n");

    if (opts.u_mode != STATUS_U_NO) {
        sg_status_untracked_fold fold =
            opts.u_mode == STATUS_U_ALL ? SG_STATUS_UNTRACKED_LIST_FILES : SG_STATUS_UNTRACKED_FOLD_DIRS;

        if (sg_status_list_untracked(git_dir, repo_root, &idx, 0, fold, &untracked,
                                     &untracked_count) != 0) {
            fprintf(stderr, "sg: out of memory, cannot scan untracked files\n");
            sg_status_list_free(&staged);
            sg_status_list_free(&unstaged);
            sg_index_free(&idx);
            free(repo_root);
            free(git_dir);
            return 1;
        }

        if (opts.ignored) {
            char **untracked_and_ignored = NULL;
            size_t untracked_and_ignored_count = 0;

            if (sg_status_list_untracked(git_dir, repo_root, &idx, 1, fold,
                                         &untracked_and_ignored, &untracked_and_ignored_count) !=
                   0 ||
               list_diff_sorted(untracked, untracked_count, untracked_and_ignored,
                                untracked_and_ignored_count, &ignored, &ignored_count) != 0) {
                fprintf(stderr, "sg: out of memory, cannot scan ignored files\n");
                for (i = 0; i < untracked_and_ignored_count; i++)
                    free(untracked_and_ignored[i]);
                free(untracked_and_ignored);
                sg_status_list_free(&staged);
                sg_status_list_free(&unstaged);
                for (i = 0; i < untracked_count; i++)
                    free(untracked[i]);
                free(untracked);
                sg_index_free(&idx);
                free(repo_root);
                free(git_dir);
                return 1;
            }
            for (i = 0; i < untracked_and_ignored_count; i++)
                free(untracked_and_ignored[i]);
            free(untracked_and_ignored);
        }
    }
    /* opts.u_mode == STATUS_U_NO: untracked/ignored are left at NULL/0 --
       "-uno" means neither is ever scanned at all (not scanned-then-hidden),
       which is also why the fuzzer's own -uall invocation keeps its
       discriminating power: folding only ever hides individual-file detail
       behind a "dir/" line, it never skips the ignore engine the way -uno
       does. */

    if (opts.porcelain) {
        if (print_porcelain_tracked(&idx, &staged, &unstaged) != 0)
            fprintf(stderr, "sg: warning: out of memory printing porcelain output\n");
        print_porcelain_paths("?? ", untracked, untracked_count);
        if (opts.ignored)
            print_porcelain_paths("!! ", ignored, ignored_count);
    } else {
        unmerged_count = print_unmerged(&idx, sg_rebase_state_exists(git_dir));

        print_section("Changes to be committed:", staged_hints, 1, &staged);
        print_section("Changes not staged for commit:", unstaged_hints, 2, &unstaged);

        if (untracked_count > 0) {
            printf("Untracked files:\n");
            printf("  (use \"sg add <file>...\" to include in what will be committed)\n");
            for (i = 0; i < untracked_count; i++)
                printf("\t%s\n", sg_quote_path(untracked[i]));
            printf("\n");
        } else if (opts.u_mode == STATUS_U_NO && staged.count > 0) {
            /* Real git prints this exactly where "Untracked files:" would
               have gone, only when something is already staged -- with
               nothing staged, the single-line summary below covers it
               instead (measured, git 2.55.0). */
            printf("Untracked files not listed (use -u option to show untracked files)\n\n");
        }

        if (opts.ignored && ignored_count > 0) {
            printf("Ignored files:\n");
            printf("  (use \"sg add\" to track, though .gitignore excludes it)\n");
            for (i = 0; i < ignored_count; i++)
                printf("\t%s\n", sg_quote_path(ignored[i]));
            printf("\n");
        }

        if (opts.u_mode == STATUS_U_NO) {
            /* -uno never learns whether untracked files exist, so its
               closing line can never claim the tree is fully clean the way
               the default mode's "working tree clean" does -- it always
               hedges with "(use -u to show untracked files)" instead
               (measured, git 2.55.0). staged.count > 0 gets no closing line
               at all, same as every other mode: the sections already
               printed above speak for themselves. */
            if (staged.count == 0) {
                if (unstaged.count > 0 || unmerged_count > 0)
                    printf("no changes added to commit (use \"git add\" and/or \"git commit "
                          "-a\")\n");
                else
                    printf("nothing to commit (use -u to show untracked files)\n");
            }
        } else if (staged.count == 0 && unstaged.count == 0 && untracked_count == 0 &&
                  unmerged_count == 0) {
            printf("nothing to commit, working tree clean\n");
        }
    }

    for (i = 0; i < untracked_count; i++)
        free(untracked[i]);
    free(untracked);
    for (i = 0; i < ignored_count; i++)
        free(ignored[i]);
    free(ignored);
    sg_status_list_free(&staged);
    sg_status_list_free(&unstaged);
    sg_index_free(&idx);
    free(repo_root);
    free(git_dir);
    return 0;
}

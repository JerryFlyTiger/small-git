/* Phase 80: F1 (sg_write_file_worktree, never write through a symlink) and
   F2 (sg_worktree_clear_write_path, replace what git replaces / fail closed
   on everything else). */

#include "sg/apply.h"

#include "sg/ignore.h"
#include "sg/index.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                         \
    } while (0)

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_worktree_write_test_XXXXXX";
    char *path = strdup(template);
    char git_dir[4096];

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
    free(path);
    return strdup(git_dir);
}

static void write_workdir_file(const char *repo_root, const char *rel, const char *content)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)content, strlen(content), 0644) == 0,
         "failed to write workdir file %s", rel);
}

static void mkdir_workdir(const char *repo_root, const char *rel)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(mkdir(abspath, 0755) == 0, "failed to mkdir %s", rel);
}

static char *read_workdir_file(const char *repo_root, const char *rel)
{
    char abspath[4096];
    unsigned char *buf;
    size_t len;
    char *s;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    if (sg_read_file(abspath, &buf, &len) != 0)
        return NULL;
    s = malloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    free(buf);
    return s;
}

static int path_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return lstat(abspath, &st) == 0;
}

static int path_is_symlink(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return lstat(abspath, &st) == 0 && S_ISLNK(st.st_mode);
}

/* ---- F1: ordinary write into fresh, missing parent directories ---- */

static void test_write_creates_missing_dirs(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char *content;

    CHECK(sg_write_file_worktree(repo_root, "a/b/c.txt", (const unsigned char *)"hello\n", 6, 0644) ==
             0,
         "write into missing directories failed");
    content = read_workdir_file(repo_root, "a/b/c.txt");
    CHECK(content != NULL && strcmp(content, "hello\n") == 0, "unexpected content: %s",
         content != NULL ? content : "(null)");
    free(content);

    free(repo_root);
    free(git_dir);
}

/* ---- F1: repo root reached through a symlinked ancestor must keep
   working -- components AT OR ABOVE repo_root are never checked. ---- */

static void test_repo_root_through_symlink(void)
{
    char *git_dir = make_tmp_repo();
    char *real_root = sg_repo_root(git_dir);
    char link_root[4096];
    char *content;

    snprintf(link_root, sizeof(link_root), "%s_link", real_root);
    CHECK(symlink(real_root, link_root) == 0, "failed to create symlinked repo root");

    CHECK(sg_write_file_worktree(link_root, "sub/file.txt", (const unsigned char *)"via-link\n", 9,
                                 0644) == 0,
         "write through a symlinked repo root failed");
    content = read_workdir_file(real_root, "sub/file.txt");
    CHECK(content != NULL && strcmp(content, "via-link\n") == 0,
         "write through the symlinked root did not land on the real tree: %s",
         content != NULL ? content : "(null)");
    free(content);

    unlink(link_root);
    free(real_root);
    free(git_dir);
}

/* ---- F1: a symlink sitting BELOW repo_root as an ancestor component
   refuses the write outright (F1 never traverses through it; only F2's
   own ignore-aware clearing may remove such a blocker first). ---- */

static void test_ancestor_symlink_refuses(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char elsewhere[4096];
    char link_path[4096];

    snprintf(elsewhere, sizeof(elsewhere), "%s/elsewhere", repo_root);
    CHECK(mkdir(elsewhere, 0755) == 0, "failed to mkdir elsewhere");
    snprintf(link_path, sizeof(link_path), "%s/a", repo_root);
    CHECK(symlink(elsewhere, link_path) == 0, "failed to symlink a -> elsewhere");

    CHECK(sg_write_file_worktree(repo_root, "a/b/c.txt", (const unsigned char *)"x\n", 2, 0644) == -1,
         "expected the write to fail through the ancestor symlink 'a'");
    CHECK(!path_exists(repo_root, "elsewhere/b"),
         "the write must never have traversed into the symlink's target");
    CHECK(path_is_symlink(repo_root, "a"), "the ancestor symlink itself must be untouched");

    free(repo_root);
    free(git_dir);
}

/* ---- F1: the FINAL component being a symlink is unlinked and replaced by
   a plain file -- the symlink's TARGET must never be touched. ---- */

static void test_final_symlink_replaced_target_untouched(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char outside_dir[4096];
    char outside_file[4096];
    char link_path[4096];
    char *outside_content;
    char *written_content;

    snprintf(outside_dir, sizeof(outside_dir), "%s_outside", repo_root);
    CHECK(mkdir(outside_dir, 0755) == 0, "failed to mkdir outside dir");
    snprintf(outside_file, sizeof(outside_file), "%s/keep.txt", outside_dir);
    CHECK(sg_write_file_mkdirs(outside_file, (const unsigned char *)"outside-untouched\n", 18, 0644) ==
             0,
         "failed to seed outside file");
    snprintf(link_path, sizeof(link_path), "%s/new.txt", repo_root);
    CHECK(symlink(outside_file, link_path) == 0, "failed to symlink new.txt -> outside file");

    CHECK(sg_write_file_worktree(repo_root, "new.txt", (const unsigned char *)"topic-content\n", 14,
                                 0644) == 0,
         "expected the final symlink to be replaced");
    CHECK(!path_is_symlink(repo_root, "new.txt"), "new.txt must no longer be a symlink");
    written_content = read_workdir_file(repo_root, "new.txt");
    CHECK(written_content != NULL && strcmp(written_content, "topic-content\n") == 0,
         "new.txt does not hold the new content: %s",
         written_content != NULL ? written_content : "(null)");
    free(written_content);

    outside_content = read_workdir_file(outside_dir, "keep.txt");
    CHECK(outside_content != NULL && strcmp(outside_content, "outside-untouched\n") == 0,
         "the symlink's OWN TARGET must never be written through, got: %s",
         outside_content != NULL ? outside_content : "(null)");
    free(outside_content);

    free(repo_root);
    free(git_dir);
}

/* ---- F2: an ignored SYMLINK inside a removed directory must be unlinked
   itself, never traversed into -- its target survives untouched. ---- */

static void test_removed_dir_ignored_symlink_target_untouched(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char outside_dir[4096];
    char outside_file[4096];
    char link_path[4096];
    sg_ignore *ig;
    char *outside_content;
    char *written_content;

    snprintf(outside_dir, sizeof(outside_dir), "%s_outside2", repo_root);
    CHECK(mkdir(outside_dir, 0755) == 0, "failed to mkdir outside dir");
    snprintf(outside_file, sizeof(outside_file), "%s/keep.txt", outside_dir);
    CHECK(sg_write_file_mkdirs(outside_file, (const unsigned char *)"outside-untouched\n", 18, 0644) ==
             0,
         "failed to seed outside file");

    write_workdir_file(repo_root, ".gitignore", "*.log\n");
    mkdir_workdir(repo_root, "new.txt");
    snprintf(link_path, sizeof(link_path), "%s/new.txt/l.log", repo_root);
    CHECK(symlink(outside_dir, link_path) == 0, "failed to symlink new.txt/l.log -> outside dir");

    CHECK(sg_ignore_open(&ig, git_dir, repo_root) == 0, "sg_ignore_open failed");
    CHECK(sg_worktree_clear_write_path(ig, repo_root, "new.txt", NULL) == 0,
         "expected the all-ignored directory to be cleared");
    sg_ignore_free(ig);

    CHECK(sg_write_file_worktree(repo_root, "new.txt", (const unsigned char *)"topic-content\n", 14,
                                 0644) == 0,
         "expected the write to succeed after clearing");
    written_content = read_workdir_file(repo_root, "new.txt");
    CHECK(written_content != NULL && strcmp(written_content, "topic-content\n") == 0,
         "unexpected content: %s", written_content != NULL ? written_content : "(null)");
    free(written_content);

    outside_content = read_workdir_file(outside_dir, "keep.txt");
    CHECK(outside_content != NULL && strcmp(outside_content, "outside-untouched\n") == 0,
         "the ignored symlink's TARGET DIRECTORY must never be traversed into, got: %s",
         outside_content != NULL ? outside_content : "(null)");
    free(outside_content);

    free(repo_root);
    free(git_dir);
}

/* ---- F2: a removal failure (read-only directory) leaves the tree
   intact and fails closed. ---- */

static void test_readonly_removal_failure_leaves_tree(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char blocker_dir[4096];
    sg_ignore *ig;
    int rc;

    if (geteuid() == 0) {
        /* root ignores directory permissions -- this fixture cannot work */
        free(repo_root);
        free(git_dir);
        return;
    }

    write_workdir_file(repo_root, ".gitignore", "*.log\n");
    mkdir_workdir(repo_root, "new.txt");
    write_workdir_file(repo_root, "new.txt/x.log", "noise\n");
    snprintf(blocker_dir, sizeof(blocker_dir), "%s/new.txt", repo_root);
    CHECK(chmod(blocker_dir, 0555) == 0, "chmod new.txt read-only failed");

    CHECK(sg_ignore_open(&ig, git_dir, repo_root) == 0, "sg_ignore_open failed");
    rc = sg_worktree_clear_write_path(ig, repo_root, "new.txt", NULL);
    sg_ignore_free(ig);

    CHECK(rc == -1, "expected the removal to fail closed under a read-only directory, got %d", rc);

    chmod(blocker_dir, 0755); /* restore so cleanup can remove it */
    CHECK(path_exists(repo_root, "new.txt/x.log"),
         "a failed removal must leave whatever could not be removed in place");

    free(repo_root);
    free(git_dir);
}

/* ---- F2: an untracked EMPTY directory tree sitting at the write target
   is entirely expendable. ---- */

static void test_empty_dir_tree_cleared(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_ignore *ig;
    char *content;

    mkdir_workdir(repo_root, "new.txt");
    mkdir_workdir(repo_root, "new.txt/empty_sub");
    mkdir_workdir(repo_root, "new.txt/empty_sub/deeper");

    CHECK(sg_ignore_open(&ig, git_dir, repo_root) == 0, "sg_ignore_open failed");
    CHECK(sg_worktree_clear_write_path(ig, repo_root, "new.txt", NULL) == 0,
         "expected the empty directory tree to be cleared");
    sg_ignore_free(ig);

    CHECK(sg_write_file_worktree(repo_root, "new.txt", (const unsigned char *)"topic-content\n", 14,
                                 0644) == 0,
         "expected the write to succeed after clearing");
    content = read_workdir_file(repo_root, "new.txt");
    CHECK(content != NULL && strcmp(content, "topic-content\n") == 0, "unexpected content: %s",
         content != NULL ? content : "(null)");
    free(content);

    free(repo_root);
    free(git_dir);
}

/* ---- F2: a directory that is ITSELF ignored (I4) is wholly expendable
   regardless of its contents -- no recursive "all-ignored" scan is even
   needed, matching the pre-flight's own ignore-beats-scan ordering. ---- */

static void test_ignored_dir_at_path_cleared(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_ignore *ig;
    char *content;

    write_workdir_file(repo_root, ".gitignore", "new.txt/\n");
    mkdir_workdir(repo_root, "new.txt");
    /* deliberately NOT ignored on its own -- only the containing directory
       "new.txt/" is ignored, so this proves the whole directory is cleared
       without needing its contents to individually qualify */
    write_workdir_file(repo_root, "new.txt/plain.txt", "not ignored by itself\n");

    CHECK(sg_ignore_open(&ig, git_dir, repo_root) == 0, "sg_ignore_open failed");
    CHECK(sg_worktree_clear_write_path(ig, repo_root, "new.txt", NULL) == 0,
         "expected the self-ignored directory to be cleared regardless of its contents");
    sg_ignore_free(ig);

    CHECK(sg_write_file_worktree(repo_root, "new.txt", (const unsigned char *)"topic-content\n", 14,
                                 0644) == 0,
         "expected the write to succeed after clearing");
    content = read_workdir_file(repo_root, "new.txt");
    CHECK(content != NULL && strcmp(content, "topic-content\n") == 0, "unexpected content: %s",
         content != NULL ? content : "(null)");
    free(content);

    free(repo_root);
    free(git_dir);
}

/* ---- F2's own NULL-index write-time convention vs the pre-flight's idx
   exemption: a directory holding a path that IS tracked in the index (and
   still physically present on disk, as if the caller's own delete-first
   pass had not yet run) is NOT expendable at write time -- F2 always
   passes NULL, deliberately with no tracked-path exemption, because that
   exemption exists for the PRE-FLIGHT (before anything is deleted), not
   for the write path (which Phase 80's F4 fix always runs after
   deletions). This directly exercises the "NULL index" contract
   sg_worktree_clear_write_path's own header comment describes -- contrast
   with test_untracked_overwrite.c's test_directory_scan_tracked_exemption,
   which proves the OPPOSITE answer for the pre-flight side of the same
   scan with a real idx passed in. ---- */

static void test_write_time_no_tracked_exemption(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    sg_ignore *ig;
    int rc;

    mkdir_workdir(repo_root, "d");
    write_workdir_file(repo_root, "d/x.txt", "tracked content still on disk\n");

    CHECK(sg_ignore_open(&ig, git_dir, repo_root) == 0, "sg_ignore_open failed");
    /* NULL index, same as every real F1 call site -- even though d/x.txt
       would be "tracked" from some caller's point of view, this function
       has no way to know that and must not guess "expendable". */
    rc = sg_worktree_clear_write_path(ig, repo_root, "d", NULL);
    sg_ignore_free(ig);

    CHECK(rc == -1,
         "expected write-time clearing to fail closed on real, un-deleted content, got %d", rc);
    CHECK(path_exists(repo_root, "d/x.txt"), "the file must survive a failed clear");

    free(repo_root);
    free(git_dir);
}

/* ---- Phase 80 fix round (finding 1, CRITICAL): sg_remove_file_worktree
   must never traverse a symlinked ancestor either -- the delete-side twin
   of F1's write-side rule. ---- */

static void write_outside_file(const char *outside_dir, const char *rel, const char *content)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", outside_dir, rel);
    CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)content, strlen(content), 0644) == 0,
         "failed to write outside file %s", rel);
}

static void test_remove_blocked_by_ancestor_symlink_leaves_outside_intact(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char outside_dir[4096];
    char link_path[4096];
    char *outside_content;

    snprintf(outside_dir, sizeof(outside_dir), "%s_outside", repo_root);
    CHECK(mkdir(outside_dir, 0755) == 0, "failed to mkdir outside dir");
    write_outside_file(outside_dir, "b/tracked.txt", "TT\n");
    snprintf(link_path, sizeof(link_path), "%s/a", repo_root);
    CHECK(symlink(outside_dir, link_path) == 0, "failed to symlink a -> outside dir");

    CHECK(sg_remove_file_worktree(repo_root, "a/b/tracked.txt") == -1,
         "expected the delete to fail closed through the ancestor symlink 'a'");
    CHECK(path_is_symlink(repo_root, "a"), "the ancestor symlink itself must be untouched");

    outside_content = read_workdir_file(outside_dir, "b/tracked.txt");
    CHECK(outside_content != NULL && strcmp(outside_content, "TT\n") == 0,
         "the outside file must never be deleted or modified through the symlink, got: %s",
         outside_content != NULL ? outside_content : "(null, meaning it was actually deleted!)");
    free(outside_content);

    free(repo_root);
    free(git_dir);
}

static void test_remove_ordinary_file_succeeds(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);

    write_workdir_file(repo_root, "a/b/tracked.txt", "TT\n");
    CHECK(sg_remove_file_worktree(repo_root, "a/b/tracked.txt") == 0, "ordinary removal should succeed");
    CHECK(!path_exists(repo_root, "a/b/tracked.txt"), "the file must actually be gone");

    free(repo_root);
    free(git_dir);
}

static void test_remove_missing_ancestor_is_success(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);

    /* "a" was never created at all -- nothing anywhere along this path
       exists, which must be treated as "already gone", not a failure. */
    CHECK(sg_remove_file_worktree(repo_root, "a/b/tracked.txt") == 0,
         "a missing ancestor must be treated as already-gone, not a failure");

    free(repo_root);
    free(git_dir);
}

static void test_remove_missing_final_component_is_success(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);

    /* Every ancestor is a real directory, but the final file itself was
       never created (or was already removed by something else) -- this
       must ALSO be "already gone", matching every one of this function's
       callers' pre-existing tolerance of a delete racing something else
       (measured necessary: an ordinary `sg reset --hard` onto a commit
       that restores a path the working tree had already deleted,
       unstaged, goes through exactly this path). */
    mkdir_workdir(repo_root, "a");
    CHECK(sg_remove_file_worktree(repo_root, "a/tracked.txt") == 0,
         "a missing final component must be treated as already-gone, not a failure");

    free(repo_root);
    free(git_dir);
}

/* ---- Phase 80 fix round (finding 1): sg_prune_empty_parents must not
   rmdir through a symlinked ancestor either. ---- */

static void test_prune_empty_parents_blocked_by_ancestor_symlink(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);
    char outside_dir[4096];
    char link_path[4096];
    char abspath[4096];

    snprintf(outside_dir, sizeof(outside_dir), "%s_outside2", repo_root);
    CHECK(mkdir(outside_dir, 0755) == 0, "failed to mkdir outside dir");
    /* An EMPTY directory "b" directly under the outside dir -- if the
       guard is missing, sg_prune_empty_parents("a/b/tracked.txt") would
       rmdir "outside_dir/b" (an empty, real directory reached only by
       resolving through the symlink), removing something outside the
       repository entirely. */
    snprintf(abspath, sizeof(abspath), "%s/b", outside_dir);
    CHECK(mkdir(abspath, 0755) == 0, "failed to mkdir outside/b");
    snprintf(link_path, sizeof(link_path), "%s/a", repo_root);
    CHECK(symlink(outside_dir, link_path) == 0, "failed to symlink a -> outside dir");

    sg_prune_empty_parents(repo_root, "a/b/tracked.txt");

    snprintf(abspath, sizeof(abspath), "%s/b", outside_dir);
    CHECK(access(abspath, F_OK) == 0,
         "the outside directory 'b' must survive -- sg_prune_empty_parents must never rmdir "
         "through a symlinked ancestor");
    CHECK(path_is_symlink(repo_root, "a"), "the ancestor symlink itself must be untouched");

    free(repo_root);
    free(git_dir);
}

/* ---- Phase 80 fix round (finding 3): an empty relpath must fail closed,
   not read past the end of the joined path. ---- */

static void test_empty_relpath_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = sg_repo_root(git_dir);

    CHECK(sg_write_file_worktree(repo_root, "", (const unsigned char *)"x", 1, 0644) == -1,
         "an empty relpath must be rejected outright by sg_write_file_worktree");
    CHECK(sg_remove_file_worktree(repo_root, "") == -1,
         "an empty relpath must be rejected outright by sg_remove_file_worktree");

    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_write_creates_missing_dirs();
    test_repo_root_through_symlink();
    test_ancestor_symlink_refuses();
    test_final_symlink_replaced_target_untouched();
    test_removed_dir_ignored_symlink_target_untouched();
    test_readonly_removal_failure_leaves_tree();
    test_empty_dir_tree_cleared();
    test_ignored_dir_at_path_cleared();
    test_write_time_no_tracked_exemption();
    test_remove_blocked_by_ancestor_symlink_leaves_outside_intact();
    test_remove_ordinary_file_succeeds();
    test_remove_missing_ancestor_is_success();
    test_remove_missing_final_component_is_success();
    test_prune_empty_parents_blocked_by_ancestor_symlink();
    test_empty_relpath_rejected();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all worktree_write tests passed\n");
    return 0;
}

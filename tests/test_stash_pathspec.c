/* Phase 37, Part B: `sg stash push -- <pathspec>...`. Pins PHASE37_SPEC.md's
   B1-B3, at the sg_stash_push library level (same level test_stash.c already
   tests at) rather than through the CLI, since the tree-content assertions
   need direct access to sha1s.

     - B1: a pathspec matching nothing at all returns 2 (a brand-new return
       code) and writes nothing durable -- refs/stash stays absent, not even
       a reflog line is appended.
     - B2: the three trees are built asymmetrically, not just filtered
       uniformly -- asserted by flattening each tree and comparing content,
       never just checking counts or exit codes.
     - B3: after push, an unmatched path's working-tree content AND index
       entry are both left completely alone (even a staged, otherwise-dirty
       one); a matched path is reset to HEAD (or, under --keep-index, the
       index tree) in both places. A working-tree DELETION of an unmatched
       path must never be recorded into the stash's own tree (that would be
       silently treating "not this round's business" as "this file is
       gone"). */
#include "sg/stash.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/pathspec.h"
#include "sg/reflog.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/snapshot.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

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
    static char template[] = "/tmp/sg_stash_pathspec_test_XXXXXX";
    char *path = strdup(template);
    char resolved[4096];
    char git_dir[4096];

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    /* realpath, not the literal mkdtemp path: on macOS /tmp is a symlink to
       /private/tmp, and sg_pathspec_add resolves a relative arg against
       getcwd() (always the resolved form) -- a repo_root built from the
       unresolved "/tmp/..." path would then fail to be a prefix of the
       resolved cwd, and every sg_pathspec_add call below would spuriously
       fail with SG_PATHSPEC_ERR_OUTSIDE. */
    if (realpath(path, resolved) == NULL) {
        fprintf(stderr, "realpath failed\n");
        exit(1);
    }
    if (sg_repo_init(resolved) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    snprintf(git_dir, sizeof(git_dir), "%s/.git", resolved);
    free(path);
    return strdup(git_dir);
}

static char *repo_root_of(const char *git_dir)
{
    char *root = strdup(git_dir);
    char *slash = strrchr(root, '/');

    CHECK(slash != NULL, "git_dir has no '/'");
    *slash = '\0';
    return root;
}

static void write_workdir_file(const char *repo_root, const char *rel, const char *content)
{
    char abspath[4096];

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    CHECK(sg_write_file_mkdirs(abspath, (const unsigned char *)content, strlen(content), 0644) == 0,
         "failed to write workdir file %s", rel);
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

static int workdir_file_exists(const char *repo_root, const char *rel)
{
    char abspath[4096];
    struct stat st;

    snprintf(abspath, sizeof(abspath), "%s/%s", repo_root, rel);
    return lstat(abspath, &st) == 0;
}

static char *read_blob(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN])
{
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len = 0;
    char *s;

    if (sg_object_read(git_dir, id, &type, &content, &content_len) != 0 || type != SG_OBJ_BLOB)
        return NULL;
    s = malloc(content_len + 1);
    memcpy(s, content, content_len);
    s[content_len] = '\0';
    free(content);
    return s;
}

static void read_commit(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN], sg_commit *out)
{
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len;

    CHECK(sg_object_read(git_dir, id, &type, &content, &content_len) == 0 && type == SG_OBJ_COMMIT,
         "failed to read commit object");
    CHECK(sg_commit_parse(content, content_len, out) == 0, "failed to parse commit object");
    free(content);
}

/* Builds a multi-file initial commit on "master": every file in files[] is
   written to the working tree, staged, and committed. Used as the shared
   HEAD every fixture below diffs/stashes against. */
typedef struct {
    const char *path;
    const char *content;
} fixture_file;

static void commit_files(const char *git_dir, const char *repo_root, const fixture_file *files,
                         size_t count, const char *message)
{
    sg_index idx;
    sg_flat_entry *entries;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    size_t i;

    memset(&idx, 0, sizeof(idx));
    entries = malloc(count * sizeof(*entries));

    for (i = 0; i < count; i++) {
        unsigned char blob[SG_SHA1_RAW_LEN];
        sg_index_entry e;

        write_workdir_file(repo_root, files[i].path, files[i].content);
        CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, files[i].content, strlen(files[i].content),
                             blob) == 0,
             "write blob for %s", files[i].path);

        memset(&e, 0, sizeof(e));
        e.mode = 0100644;
        memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
        e.path = (char *)files[i].path;
        CHECK(sg_index_upsert(&idx, &e) == 0, "upsert %s", files[i].path);

        entries[i].path = (char *)files[i].path;
        entries[i].mode = 0100644;
        memcpy(entries[i].sha1, blob, SG_SHA1_RAW_LEN);
    }
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
    sg_index_free(&idx);

    CHECK(sg_tree_build(git_dir, entries, count, tree_id) == 0, "build tree");
    free(entries);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.parent_count = 0;
    commit.author_name = (char *)"Test";
    commit.author_email = (char *)"test@example.com";
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)message;

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "serialize commit");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id) == 0,
         "write commit");
    free(serialized);

    CHECK(sg_ref_update_branch(git_dir, "master", commit_id) == 0, "update branch");
}

/* Re-stages path at newcontent (both on disk and in the index) -- what
   `sg add` after editing a file does, without going through the CLI. */
static void stage_file(const char *git_dir, const char *repo_root, const char *path,
                       const char *newcontent)
{
    sg_index idx;
    unsigned char blob[SG_SHA1_RAW_LEN];
    sg_index_entry e;

    write_workdir_file(repo_root, path, newcontent);
    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, newcontent, strlen(newcontent), blob) == 0,
         "write blob for %s", path);
    CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");

    memset(&e, 0, sizeof(e));
    e.mode = 0100644;
    memcpy(e.sha1, blob, SG_SHA1_RAW_LEN);
    e.path = (char *)path;
    CHECK(sg_index_upsert(&idx, &e) == 0, "restage %s", path);
    CHECK(sg_index_write(git_dir, &idx) == 0, "write index");
    sg_index_free(&idx);
}

static const sg_flat_entry *flat_find(const sg_flat_list *list, const char *path)
{
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].path, path) == 0)
            return &list->entries[i];
    }
    return NULL;
}

/* Asserts tree_id, flattened, has exactly the given (path, content) pairs
   (order-independent) -- both directions: every expected path must be
   present with the right content, AND the flattened count must match, so an
   extra, unexpected path is caught too. */
static void assert_tree_contents(const char *git_dir, const unsigned char tree_id[SG_SHA1_RAW_LEN],
                                 const char *label, const fixture_file *expected, size_t expected_count)
{
    sg_flat_list flat;
    size_t i;

    CHECK(sg_tree_flatten(git_dir, tree_id, &flat, NULL) == 0, "%s: flatten failed", label);
    CHECK(flat.count == expected_count, "%s: expected %zu entries, got %zu", label, expected_count,
         flat.count);

    for (i = 0; i < expected_count; i++) {
        const sg_flat_entry *e = flat_find(&flat, expected[i].path);
        char *content;

        CHECK(e != NULL, "%s: expected path %s missing", label, expected[i].path);
        if (e == NULL)
            continue;
        content = read_blob(git_dir, e->sha1);
        CHECK(content != NULL, "%s: could not read blob for %s", label, expected[i].path);
        if (content != NULL) {
            CHECK(strcmp(content, expected[i].content) == 0, "%s: path %s expected %s got %s",
                 label, expected[i].path, expected[i].content, content);
            free(content);
        }
    }
    sg_flat_list_free(&flat);
}

static sg_pathspec make_pathspec(const char *repo_root, const char *spec)
{
    sg_pathspec ps;
    sg_pathspec_error err;

    /* sg_pathspec_add resolves a relative arg against the CWD (mirroring
       how the CLI always runs from inside the repo) -- every fixture below
       builds its files via absolute repo_root-joined paths, so it never
       needed to chdir before this point. */
    CHECK(chdir(repo_root) == 0, "chdir(%s) failed", repo_root);
    memset(&ps, 0, sizeof(ps));
    CHECK(sg_pathspec_add(&ps, repo_root, spec, &err) == 0, "sg_pathspec_add(%s) failed", spec);
    return ps;
}

/* B2 (PHASE37_SPEC.md), the core fixture: a.txt/b.txt do NOT match "sub",
   sub/c.txt/sub/d.txt DO. Push `-u -- sub` and assert the exact content of
   all three trees. */
static void test_b2_three_trees(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    fixture_file base[] = {
        {"a.txt", "base\n"},
        {"b.txt", "base\n"},
        {"sub/c.txt", "base\n"},
        {"sub/d.txt", "base\n"},
    };
    sg_stash_push_opts opts;
    sg_pathspec ps;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_commit stash_commit;
    sg_commit index_parent;
    int rc;

    commit_files(git_dir, repo_root, base, 4, "base\n");

    /* a.txt: staged=STAGED-a, worktree=WORKTREE-a (further dirty on top). */
    stage_file(git_dir, repo_root, "a.txt", "STAGED-a\n");
    write_workdir_file(repo_root, "a.txt", "WORKTREE-a\n");
    /* b.txt: worktree-only change, index stays at base. */
    write_workdir_file(repo_root, "b.txt", "WORKTREE-b\n");
    /* sub/c.txt: staged=STAGED-c, worktree=WORKTREE-c. */
    stage_file(git_dir, repo_root, "sub/c.txt", "STAGED-c\n");
    write_workdir_file(repo_root, "sub/c.txt", "WORKTREE-c\n");
    /* sub/d.txt: worktree-only change, index stays at base. */
    write_workdir_file(repo_root, "sub/d.txt", "WORKTREE-d\n");
    /* untracked, matched by "sub". */
    write_workdir_file(repo_root, "sub/untracked2.txt", "untracked2\n");
    /* untracked, NOT matched by "sub". */
    write_workdir_file(repo_root, "other_untracked.txt", "other\n");

    ps = make_pathspec(repo_root, "sub");
    memset(&opts, 0, sizeof(opts));
    opts.message = "partial";
    opts.include_untracked = 1;
    opts.pathspec = &ps;

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, NULL);
    CHECK(rc == 0, "sg_stash_push (-u -- sub) failed, rc=%d", rc);
    if (rc != 0)
        goto out;

    read_commit(git_dir, commit_id, &stash_commit);
    CHECK(stash_commit.parent_count == 3, "expected a 3-parent stash commit, got %zu",
         stash_commit.parent_count);

    {
        fixture_file expect_stash[] = {
            {"a.txt", "STAGED-a\n"},   /* unmatched: INDEX content */
            {"b.txt", "base\n"},       /* unmatched: INDEX (== HEAD) content */
            {"sub/c.txt", "WORKTREE-c\n"}, /* matched: worktree content */
            {"sub/d.txt", "WORKTREE-d\n"}, /* matched: worktree content */
        };
        assert_tree_contents(git_dir, stash_commit.tree, "stash tree", expect_stash, 4);
    }

    read_commit(git_dir, stash_commit.parents[1], &index_parent);
    {
        fixture_file expect_index[] = {
            {"a.txt", "STAGED-a\n"},
            {"b.txt", "base\n"},
            {"sub/c.txt", "STAGED-c\n"}, /* NOT filtered -- complete index tree */
            {"sub/d.txt", "base\n"},
        };
        assert_tree_contents(git_dir, index_parent.tree, "stash^2 (index tree)", expect_index, 4);
    }
    sg_commit_free(&index_parent);

    {
        fixture_file expect_untracked[] = {
            {"sub/untracked2.txt", "untracked2\n"},
        };
        sg_commit untracked_parent;

        read_commit(git_dir, stash_commit.parents[2], &untracked_parent);
        assert_tree_contents(git_dir, untracked_parent.tree, "stash^3 (untracked tree)",
                             expect_untracked, 1);
        sg_commit_free(&untracked_parent);
    }

    /* B3: post-push state. Unmatched paths untouched (worktree AND index);
       matched paths reset to HEAD (no --keep-index here); the matched
       untracked file is swept away, the unmatched one is not. */
    {
        char *a_wd = read_workdir_file(repo_root, "a.txt");
        char *b_wd = read_workdir_file(repo_root, "b.txt");
        char *c_wd = read_workdir_file(repo_root, "sub/c.txt");
        char *d_wd = read_workdir_file(repo_root, "sub/d.txt");
        sg_index idx;

        CHECK(a_wd != NULL && strcmp(a_wd, "WORKTREE-a\n") == 0,
             "a.txt (unmatched) worktree content must be untouched, got %s", a_wd ? a_wd : "(gone)");
        CHECK(b_wd != NULL && strcmp(b_wd, "WORKTREE-b\n") == 0,
             "b.txt (unmatched) worktree content must be untouched, got %s", b_wd ? b_wd : "(gone)");
        CHECK(c_wd != NULL && strcmp(c_wd, "base\n") == 0,
             "sub/c.txt (matched) must be reset to HEAD, got %s", c_wd ? c_wd : "(gone)");
        CHECK(d_wd != NULL && strcmp(d_wd, "base\n") == 0,
             "sub/d.txt (matched) must be reset to HEAD, got %s", d_wd ? d_wd : "(gone)");
        free(a_wd);
        free(b_wd);
        free(c_wd);
        free(d_wd);

        CHECK(!workdir_file_exists(repo_root, "sub/untracked2.txt"),
             "matched untracked file must be swept away by -u");
        CHECK(workdir_file_exists(repo_root, "other_untracked.txt"),
             "unmatched untracked file must survive -u");

        CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");
        {
            int ai = sg_index_find_stage(&idx, "a.txt", 0);
            int ci = sg_index_find_stage(&idx, "sub/c.txt", 0);

            CHECK(ai >= 0, "a.txt must still be in the index");
            if (ai >= 0) {
                char *blob = read_blob(git_dir, idx.entries[ai].sha1);

                CHECK(blob != NULL && strcmp(blob, "STAGED-a\n") == 0,
                     "a.txt (unmatched) index entry must be untouched, got %s",
                     blob ? blob : "(missing)");
                free(blob);
            }
            CHECK(ci >= 0, "sub/c.txt must still be in the index");
            if (ci >= 0) {
                char *blob = read_blob(git_dir, idx.entries[ci].sha1);

                CHECK(blob != NULL && strcmp(blob, "base\n") == 0,
                     "sub/c.txt (matched) index entry must be reset to HEAD, got %s",
                     blob ? blob : "(missing)");
                free(blob);
            }
        }
        sg_index_free(&idx);
    }

    /* Coordinator follow-up: the automatic safety snapshot
       (sg_snapshot_create, called unconditionally by sg_stash_push before
       the destructive restore step) must NOT be narrowed by the pathspec --
       it exists to let the user recover the true pre-push working tree
       regardless of what was actually stashed, so it has to capture a.txt's
       real WORKTREE-a content even though a.txt itself does not match
       "sub" and the stash's own tree recorded only its INDEX content
       (STAGED-a). snapshot.c passes NULL unconditionally (structural, not
       parameterized), so this property has no mutation to hang a
       reverse-mutation round on -- this assertion exists so that a FUTURE
       change threading a pathspec into that NULL would at least turn this
       one red. */
    {
        sg_snapshot_list snaps;
        unsigned char snap_tree[SG_SHA1_RAW_LEN];

        CHECK(sg_snapshot_list_read(git_dir, &snaps) == 0, "snapshot list read failed");
        CHECK(snaps.count > 0, "sg_stash_push must have taken a safety snapshot");
        if (snaps.count > 0) {
            CHECK(sg_snapshot_get_tree(git_dir, &snaps, 0, snap_tree) == 0,
                 "snapshot tree lookup failed");
            {
                fixture_file expect_snapshot[] = {
                    {"a.txt", "WORKTREE-a\n"}, /* real worktree content, NOT STAGED-a */
                    {"b.txt", "WORKTREE-b\n"},
                    {"sub/c.txt", "WORKTREE-c\n"},
                    {"sub/d.txt", "WORKTREE-d\n"},
                };
                assert_tree_contents(git_dir, snap_tree, "safety snapshot (must be unfiltered)",
                                     expect_snapshot, 4);
            }
        }
        sg_snapshot_list_free(&snaps);
    }

    sg_commit_free(&stash_commit);
out:
    sg_pathspec_free(&ps);
    free(repo_root);
    free(git_dir);
}

/* B3, --keep-index: a matched path resets to the INDEX tree's content, not
   HEAD's. */
static void test_b3_keep_index(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    fixture_file base[] = {
        {"sub/c.txt", "base\n"},
        {"a.txt", "base\n"},
    };
    sg_stash_push_opts opts;
    sg_pathspec ps;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char *c_wd;
    char *a_wd;
    sg_index idx;
    int ci;
    int ai;
    int rc;

    commit_files(git_dir, repo_root, base, 2, "base\n");
    stage_file(git_dir, repo_root, "sub/c.txt", "STAGED-c\n");
    write_workdir_file(repo_root, "sub/c.txt", "WORKTREE-c\n");
    /* a.txt (unmatched by "sub") staged AND further dirtied -- this is what
       distinguishes restore_matched_paths from a whole-tree
       sg_apply_tree_to_workdir(index_tree): the latter would reset EVERY
       tracked path to the index tree's content (wiping out a.txt's own
       further-dirty worktree edit and its staged content alike), the
       former must leave it exactly alone. */
    stage_file(git_dir, repo_root, "a.txt", "STAGED-a\n");
    write_workdir_file(repo_root, "a.txt", "WORKTREE-a\n");

    ps = make_pathspec(repo_root, "sub");
    memset(&opts, 0, sizeof(opts));
    opts.message = "keepindex";
    opts.keep_index = 1;
    opts.pathspec = &ps;

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, NULL);
    CHECK(rc == 0, "sg_stash_push (--keep-index -- sub) failed, rc=%d", rc);
    if (rc != 0)
        goto out;

    c_wd = read_workdir_file(repo_root, "sub/c.txt");
    CHECK(c_wd != NULL && strcmp(c_wd, "STAGED-c\n") == 0,
         "sub/c.txt worktree content must reset to the INDEX tree under --keep-index, got %s",
         c_wd ? c_wd : "(gone)");
    free(c_wd);

    a_wd = read_workdir_file(repo_root, "a.txt");
    CHECK(a_wd != NULL && strcmp(a_wd, "WORKTREE-a\n") == 0,
         "a.txt (unmatched) worktree content must stay untouched under --keep-index, got %s",
         a_wd ? a_wd : "(gone)");
    free(a_wd);

    CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");
    ci = sg_index_find_stage(&idx, "sub/c.txt", 0);
    CHECK(ci >= 0, "sub/c.txt must still be in the index");
    if (ci >= 0) {
        char *blob = read_blob(git_dir, idx.entries[ci].sha1);

        CHECK(blob != NULL && strcmp(blob, "STAGED-c\n") == 0,
             "sub/c.txt index entry must stay at the staged content under --keep-index, got %s",
             blob ? blob : "(missing)");
        free(blob);
    }
    ai = sg_index_find_stage(&idx, "a.txt", 0);
    CHECK(ai >= 0, "a.txt must still be in the index");
    if (ai >= 0) {
        char *blob = read_blob(git_dir, idx.entries[ai].sha1);

        CHECK(blob != NULL && strcmp(blob, "STAGED-a\n") == 0,
             "a.txt (unmatched) index entry must stay untouched under --keep-index, got %s",
             blob ? blob : "(missing)");
        free(blob);
    }
    sg_index_free(&idx);

out:
    sg_pathspec_free(&ps);
    free(repo_root);
    free(git_dir);
}

/* B1: a pathspec matching nothing at all -- returns 2, writes nothing
   durable, even though the repo has OTHER, unrelated dirty content. */
static void test_b1_no_match(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    fixture_file base[] = {
        {"a.txt", "base\n"},
    };
    sg_stash_push_opts opts;
    sg_pathspec ps;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_reflog log;
    unsigned char ref_id[SG_SHA1_RAW_LEN];
    int rc;

    commit_files(git_dir, repo_root, base, 1, "base\n");
    write_workdir_file(repo_root, "a.txt", "dirty\n"); /* unrelated dirty change */

    ps = make_pathspec(repo_root, "nosuch");
    memset(&opts, 0, sizeof(opts));
    opts.pathspec = &ps;

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, NULL);
    CHECK(rc == 2, "expected rc==2 for a pathspec matching nothing, got %d", rc);

    CHECK(sg_ref_read_path(git_dir, "refs/stash", ref_id) != 0,
         "refs/stash must not exist after a no-match push");
    CHECK(sg_reflog_read(git_dir, "refs/stash", &log) == 0, "reflog read failed");
    CHECK(log.count == 0, "refs/stash reflog must have no entries, got %zu", log.count);
    sg_reflog_free(&log);

    /* The unrelated dirty change must survive untouched -- push refused
       outright, it must not have reset anything either. */
    {
        char *a_wd = read_workdir_file(repo_root, "a.txt");

        CHECK(a_wd != NULL && strcmp(a_wd, "dirty\n") == 0,
             "a.txt must be untouched after a refused push, got %s", a_wd ? a_wd : "(gone)");
        free(a_wd);
    }

    sg_pathspec_free(&ps);
    free(repo_root);
    free(git_dir);
}

/* B3's sharpest edge: an UNMATCHED path deleted from the working tree must
   NOT have that deletion recorded into the stash's own tree -- it must
   still carry the path's INDEX content, exactly as if the working tree had
   never been touched at all. */
static void test_b3_unmatched_deletion_not_recorded(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    fixture_file base[] = {
        {"a.txt", "base\n"},
        {"sub/c.txt", "base\n"},
    };
    char abspath[4096];
    sg_stash_push_opts opts;
    sg_pathspec ps;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_commit stash_commit;
    int rc;

    commit_files(git_dir, repo_root, base, 2, "base\n");

    /* a.txt (unmatched) DELETED from the working tree, not staged. */
    snprintf(abspath, sizeof(abspath), "%s/a.txt", repo_root);
    CHECK(remove(abspath) == 0, "failed to delete a.txt");
    /* sub/c.txt (matched) modified, so the push has something to save. */
    write_workdir_file(repo_root, "sub/c.txt", "WORKTREE-c\n");

    ps = make_pathspec(repo_root, "sub");
    memset(&opts, 0, sizeof(opts));
    opts.message = "unmatched-deletion";
    opts.pathspec = &ps;

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, NULL);
    CHECK(rc == 0, "sg_stash_push failed, rc=%d", rc);
    if (rc != 0)
        goto out;

    read_commit(git_dir, commit_id, &stash_commit);
    {
        fixture_file expect_stash[] = {
            {"a.txt", "base\n"}, /* NOT omitted, despite being gone on disk */
            {"sub/c.txt", "WORKTREE-c\n"},
        };
        assert_tree_contents(git_dir, stash_commit.tree, "stash tree", expect_stash, 2);
    }
    sg_commit_free(&stash_commit);

out:
    sg_pathspec_free(&ps);
    free(repo_root);
    free(git_dir);
}

/* B3's mirror case: a MATCHED path deleted from the working tree DOES record
   the deletion (RECORD_DELETION still applies on a matched path). */
static void test_b3_matched_deletion_recorded(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    fixture_file base[] = {
        {"sub/c.txt", "base\n"},
        {"sub/d.txt", "base\n"},
    };
    char abspath[4096];
    sg_stash_push_opts opts;
    sg_pathspec ps;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_commit stash_commit;
    int rc;

    commit_files(git_dir, repo_root, base, 2, "base\n");

    /* sub/c.txt (matched) DELETED from the working tree. */
    snprintf(abspath, sizeof(abspath), "%s/sub/c.txt", repo_root);
    CHECK(remove(abspath) == 0, "failed to delete sub/c.txt");

    ps = make_pathspec(repo_root, "sub");
    memset(&opts, 0, sizeof(opts));
    opts.message = "matched-deletion";
    opts.pathspec = &ps;

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, NULL);
    CHECK(rc == 0, "sg_stash_push failed, rc=%d", rc);
    if (rc != 0)
        goto out;

    read_commit(git_dir, commit_id, &stash_commit);
    {
        fixture_file expect_stash[] = {
            {"sub/d.txt", "base\n"},
        };
        /* sub/c.txt must be OMITTED entirely -- a deletion, matched. */
        assert_tree_contents(git_dir, stash_commit.tree, "stash tree", expect_stash, 1);
    }
    sg_commit_free(&stash_commit);

out:
    sg_pathspec_free(&ps);
    free(repo_root);
    free(git_dir);
}

/* B3's restore step deletes a matched-but-target-absent path (e.g. a
   working-tree deletion under a matched pathspec is restored back to
   nothing, since target_tree also lacks it). This test is the mirror case
   for the DELETION side specifically: an UNMATCHED path that is staged as a
   brand-new addition (present in idx, ABSENT from head_tree, since it was
   never committed) must survive the restore step untouched -- its absence
   from head_tree must never be misread as "this path should be deleted"
   just because head_tree lacks it. Unlike test_b3_unmatched_deletion_not_
   recorded (which pins the STASH TREE's own content), this one pins the
   restore step's effect on the actual working tree + index after push. */
static void test_b3_unmatched_new_staged_file_survives_restore(void)
{
    char *git_dir = make_tmp_repo();
    char *repo_root = repo_root_of(git_dir);
    fixture_file base[] = {
        {"sub/c.txt", "base\n"},
    };
    sg_stash_push_opts opts;
    sg_pathspec ps;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    sg_index idx;
    int ni;
    int rc;

    commit_files(git_dir, repo_root, base, 1, "base\n");
    /* sub/c.txt (matched) changed, so the push has something to save. */
    write_workdir_file(repo_root, "sub/c.txt", "WORKTREE-c\n");
    /* newfile.txt (unmatched by "sub"): staged as a brand-new addition,
       never committed -- absent from head_tree entirely. */
    stage_file(git_dir, repo_root, "newfile.txt", "NEW-STAGED\n");

    ps = make_pathspec(repo_root, "sub");
    memset(&opts, 0, sizeof(opts));
    opts.message = "unmatched-new-file";
    opts.pathspec = &ps;

    rc = sg_stash_push(git_dir, repo_root, &opts, commit_id, NULL);
    CHECK(rc == 0, "sg_stash_push failed, rc=%d", rc);
    if (rc != 0)
        goto out;

    CHECK(workdir_file_exists(repo_root, "newfile.txt"),
         "newfile.txt (unmatched, staged-new) must survive the restore step");
    CHECK(sg_index_read(git_dir, &idx) == 0, "index read failed");
    ni = sg_index_find_stage(&idx, "newfile.txt", 0);
    CHECK(ni >= 0, "newfile.txt (unmatched, staged-new) must still be in the index");
    if (ni >= 0) {
        char *blob = read_blob(git_dir, idx.entries[ni].sha1);

        CHECK(blob != NULL && strcmp(blob, "NEW-STAGED\n") == 0,
             "newfile.txt index entry must be untouched, got %s", blob ? blob : "(missing)");
        free(blob);
    }
    sg_index_free(&idx);

out:
    sg_pathspec_free(&ps);
    free(repo_root);
    free(git_dir);
}

int main(void)
{
    test_b2_three_trees();
    test_b3_keep_index();
    test_b1_no_match();
    test_b3_unmatched_deletion_not_recorded();
    test_b3_matched_deletion_recorded();
    test_b3_unmatched_new_staged_file_survives_restore();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all stash pathspec tests passed\n");
    return 0;
}

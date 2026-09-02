#include "sg/sequencer.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static long long time_seq = 2000000;

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
    static char template[] = "/tmp/sg_sequencer_state_test_XXXXXX";
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

/* Same idiom as test_rebase_state.c's make_commit: one blob per commit
   (content = message) so distinct commits never accidentally share a tree. */
static void make_commit(const char *git_dir, const char *message,
                        const unsigned char (*parents)[SG_SHA1_RAW_LEN], size_t parent_count,
                        unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, message, strlen(message), blob_id) == 0,
         "blob write failed for '%s'", message);

    entry.path = (char *)"file.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "tree build failed for '%s'", message);

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    if (parent_count > 0) {
        commit.parents = malloc(parent_count * sizeof(*commit.parents));
        CHECK(commit.parents != NULL, "oom");
        memcpy(commit.parents, parents, parent_count * SG_SHA1_RAW_LEN);
        commit.parent_count = parent_count;
    }
    commit.author_name = (char *)"tester";
    commit.author_email = (char *)"tester@example.com";
    commit.author_time = time_seq++;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)message;

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0,
         "serialize failed for '%s'", message);
    free(commit.parents);
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed for '%s'", message);
    free(serialized);
}

static void write_raw_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");

    CHECK(f != NULL, "failed to open '%s'", path);
    if (f == NULL)
        return;
    fputs(content, f);
    fclose(f);
}

/* ==================== single-commit shape: no sequencer/ dir ==================== */

static void test_single_commit_no_sequence(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN];
    sg_sequencer_state st, read_back;

    make_commit(git_dir, "c1", NULL, 0, c1);

    CHECK(sg_sequencer_kind_in_progress(git_dir) == 0, "nothing in progress yet");

    memset(&st, 0, sizeof(st));
    st.kind = SG_SEQ_CHERRY_PICK;
    memcpy(st.current, c1, SG_SHA1_RAW_LEN);
    st.has_sequence = 0;

    CHECK(sg_sequencer_state_write(git_dir, &st) == 0, "single-commit state write should succeed");
    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "CHERRY_PICK_HEAD alone should count as in progress");

    {
        char path[4096];
        struct stat sb;

        snprintf(path, sizeof(path), "%s/sequencer", git_dir);
        CHECK(stat(path, &sb) != 0,
             "a single-commit cherry-pick must create NO sequencer/ directory at all");
    }

    memset(&read_back, 0, sizeof(read_back));
    CHECK(sg_sequencer_state_read(git_dir, &read_back) == 0, "read should succeed");
    CHECK(read_back.kind == SG_SEQ_CHERRY_PICK, "kind round-trips");
    CHECK(memcmp(read_back.current, c1, SG_SHA1_RAW_LEN) == 0, "current round-trips");
    CHECK(read_back.has_sequence == 0, "has_sequence should read back 0");
    CHECK(read_back.todo == NULL && read_back.todo_count == 0,
         "todo should be empty when has_sequence is 0");
    sg_sequencer_state_free(&read_back);

    CHECK(sg_sequencer_state_remove(git_dir) == 0, "remove should succeed");
    CHECK(sg_sequencer_kind_in_progress(git_dir) == 0, "nothing should be in progress after remove");
    CHECK(sg_sequencer_state_read(git_dir, &read_back) == -1, "reading a removed state should fail");

    free(git_dir);
}

/* ==================== multi-commit shape: sequencer/ dir present, revert kind ==================== */

static void test_multi_commit_sequence_revert(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN], head[SG_SHA1_RAW_LEN];
    sg_sequencer_state st, read_back;

    make_commit(git_dir, "head commit", NULL, 0, head);
    make_commit(git_dir, "c1 first", NULL, 0, c1);
    make_commit(git_dir, "c2 second", NULL, 0, c2);

    memset(&st, 0, sizeof(st));
    st.kind = SG_SEQ_REVERT;
    memcpy(st.current, c1, SG_SHA1_RAW_LEN);
    memcpy(st.orig_head, head, SG_SHA1_RAW_LEN);
    memcpy(st.abort_safety, head, SG_SHA1_RAW_LEN);
    st.has_sequence = 1;
    st.todo = malloc(2 * sizeof(*st.todo));
    memcpy(st.todo[0], c1, SG_SHA1_RAW_LEN);
    memcpy(st.todo[1], c2, SG_SHA1_RAW_LEN);
    st.todo_count = 2;

    CHECK(sg_sequencer_state_write(git_dir, &st) == 0, "multi-commit state write should succeed");
    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_REVERT, "REVERT_HEAD should be seen");

    {
        char path[4096];
        struct stat sb;

        snprintf(path, sizeof(path), "%s/sequencer", git_dir);
        CHECK(stat(path, &sb) == 0 && S_ISDIR(sb.st_mode), "sequencer/ directory should exist");
        snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
        CHECK(stat(path, &sb) == 0, "sequencer/todo should exist");
    }

    /* Pin the on-disk todo format directly: "revert <40hex> <subject>". */
    {
        char path[4096];
        FILE *f;
        char line[512];
        char hex[SG_SHA1_HEX_LEN + 1];
        char expected[600];

        snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
        f = fopen(path, "rb");
        CHECK(f != NULL, "todo file should be readable");
        if (f != NULL) {
            CHECK(fgets(line, sizeof(line), f) != NULL, "todo should have a first line");
            sg_sha1_to_hex(c1, hex);
            snprintf(expected, sizeof(expected), "revert %s c1 first\n", hex);
            CHECK(strcmp(line, expected) == 0, "todo[0] line should be '%s', got '%s'", expected,
                 line);
            fclose(f);
        }
    }

    memset(&read_back, 0, sizeof(read_back));
    CHECK(sg_sequencer_state_read(git_dir, &read_back) == 0, "read should succeed");
    CHECK(read_back.kind == SG_SEQ_REVERT, "kind round-trips");
    CHECK(read_back.has_sequence == 1, "has_sequence round-trips");
    CHECK(memcmp(read_back.orig_head, head, SG_SHA1_RAW_LEN) == 0, "orig_head round-trips");
    CHECK(memcmp(read_back.abort_safety, head, SG_SHA1_RAW_LEN) == 0, "abort_safety round-trips");
    CHECK(read_back.todo_count == 2, "todo_count round-trips (got %zu)", read_back.todo_count);
    CHECK(read_back.todo != NULL && memcmp(read_back.todo[0], c1, SG_SHA1_RAW_LEN) == 0,
         "todo[0] round-trips (must equal current)");
    CHECK(read_back.todo != NULL && memcmp(read_back.todo[1], c2, SG_SHA1_RAW_LEN) == 0,
         "todo[1] round-trips");
    sg_sequencer_state_free(&read_back);

    CHECK(sg_sequencer_state_remove(git_dir) == 0, "remove should succeed");
    {
        char path[4096];
        struct stat sb;

        snprintf(path, sizeof(path), "%s/sequencer", git_dir);
        CHECK(stat(path, &sb) != 0, "sequencer/ directory should be gone after remove");
    }

    free(st.todo);
    free(git_dir);
}

/* ==================== MERGE_MSG format (spec section 2.2) ==================== */

static void test_merge_msg_with_conflicts(void)
{
    char *git_dir = make_tmp_repo();
    char *paths[2];
    char *content;
    char path[4096];
    unsigned char *data;
    size_t len;

    paths[0] = (char *)"b.txt";
    paths[1] = (char *)"g.txt";

    CHECK(sg_sequencer_write_merge_msg(git_dir, "topic B two conflicts\n", paths, 2) == 0,
         "write should succeed");

    snprintf(path, sizeof(path), "%s/MERGE_MSG", git_dir);
    CHECK(sg_read_file(path, &data, &len) == 0, "MERGE_MSG should be readable");
    if (data != NULL) {
        content = malloc(len + 1);
        memcpy(content, data, len);
        content[len] = '\0';
        CHECK(strcmp(content,
                     "topic B two conflicts\n\n# Conflicts:\n#\tb.txt\n#\tg.txt\n") == 0,
             "MERGE_MSG bytes should match the measured format exactly, got '%s'", content);
        free(content);
        free(data);
    }

    free(git_dir);
}

static void test_merge_msg_without_conflicts(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    unsigned char *data;
    size_t len;

    CHECK(sg_sequencer_write_merge_msg(git_dir, "plain message\n", NULL, 0) == 0,
         "write should succeed");

    snprintf(path, sizeof(path), "%s/MERGE_MSG", git_dir);
    CHECK(sg_read_file(path, &data, &len) == 0, "MERGE_MSG should be readable");
    CHECK(len == strlen("plain message\n") && memcmp(data, "plain message\n", len) == 0,
         "an empty-result stop must hold just the message, no # Conflicts: block");
    free(data);

    free(git_dir);
}

/* ==================== malformed-field rejections ==================== */

static void test_malformed_current_hex_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    sg_sequencer_state st;

    snprintf(path, sizeof(path), "%s/CHERRY_PICK_HEAD", git_dir);
    write_raw_file(path, "not-valid-hex\n");

    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "existence must be seen even though the content is corrupt");
    CHECK(sg_sequencer_state_read(git_dir, &st) == -1, "malformed current hex should be rejected");

    free(git_dir);
}

static void test_short_hex_in_todo_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    unsigned char c1[SG_SHA1_RAW_LEN], head[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_sequencer_state st;

    make_commit(git_dir, "head", NULL, 0, head);
    make_commit(git_dir, "c1", NULL, 0, c1);
    sg_sha1_to_hex(c1, hex);

    snprintf(path, sizeof(path), "%s/CHERRY_PICK_HEAD", git_dir);
    write_raw_file(path, hex);
    {
        FILE *f = fopen(path, "ab");
        CHECK(f != NULL, "reopen for append");
        if (f != NULL) {
            fputs("\n", f);
            fclose(f);
        }
    }

    snprintf(path, sizeof(path), "%s/sequencer", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sequencer/head", git_dir);
    write_raw_file(path, hex);
    {
        FILE *f = fopen(path, "ab");
        if (f != NULL) {
            fputs("\n", f);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/abort-safety", git_dir);
    write_raw_file(path, hex);
    {
        FILE *f = fopen(path, "ab");
        if (f != NULL) {
            fputs("\n", f);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
    write_raw_file(path, "pick deadbeef c1\n"); /* too short */

    CHECK(sg_sequencer_state_read(git_dir, &st) == -1, "a too-short hex in a todo line should be rejected");

    free(git_dir);
}

static void test_missing_newline_in_todo_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    unsigned char c1[SG_SHA1_RAW_LEN], head[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_sequencer_state st;

    make_commit(git_dir, "head", NULL, 0, head);
    make_commit(git_dir, "c1", NULL, 0, c1);
    sg_sha1_to_hex(c1, hex);

    snprintf(path, sizeof(path), "%s/REVERT_HEAD", git_dir);
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "open REVERT_HEAD");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }

    snprintf(path, sizeof(path), "%s/sequencer", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sequencer/head", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/abort-safety", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            /* No trailing newline. */
            fprintf(f, "revert %s c1", hex);
            fclose(f);
        }
    }

    CHECK(sg_sequencer_state_read(git_dir, &st) == -1, "a todo line missing its newline should be rejected");

    free(git_dir);
}

static void test_bad_verb_in_todo_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    unsigned char c1[SG_SHA1_RAW_LEN], head[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_sequencer_state st;

    make_commit(git_dir, "head", NULL, 0, head);
    make_commit(git_dir, "c1", NULL, 0, c1);
    sg_sha1_to_hex(c1, hex);

    snprintf(path, sizeof(path), "%s/CHERRY_PICK_HEAD", git_dir);
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "open CHERRY_PICK_HEAD");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }

    snprintf(path, sizeof(path), "%s/sequencer", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sequencer/head", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/abort-safety", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            /* CHERRY_PICK_HEAD requires "pick", not "revert". */
            fprintf(f, "revert %s c1\n", hex);
            fclose(f);
        }
    }

    CHECK(sg_sequencer_state_read(git_dir, &st) == -1,
         "a todo verb that doesn't match the kind should be rejected");

    free(git_dir);
}

static void test_missing_head_file_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    unsigned char c1[SG_SHA1_RAW_LEN], head[SG_SHA1_RAW_LEN];
    char hex[SG_SHA1_HEX_LEN + 1];
    sg_sequencer_state st;

    make_commit(git_dir, "head", NULL, 0, head);
    make_commit(git_dir, "c1", NULL, 0, c1);
    sg_sha1_to_hex(c1, hex);

    snprintf(path, sizeof(path), "%s/CHERRY_PICK_HEAD", git_dir);
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "open CHERRY_PICK_HEAD");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }

    snprintf(path, sizeof(path), "%s/sequencer", git_dir);
    mkdir(path, 0755);
    /* "head" (orig_head) file is deliberately never written. */
    snprintf(path, sizeof(path), "%s/sequencer/abort-safety", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "pick %s c1\n", hex);
            fclose(f);
        }
    }

    CHECK(sg_sequencer_state_read(git_dir, &st) == -1,
         "a missing sequencer/head file should be rejected as corruption");

    free(git_dir);
}

static void test_todo_current_mismatch_rejected(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];
    unsigned char c1[SG_SHA1_RAW_LEN], c2[SG_SHA1_RAW_LEN], head[SG_SHA1_RAW_LEN];
    char hex_current[SG_SHA1_HEX_LEN + 1];
    char hex_todo[SG_SHA1_HEX_LEN + 1];
    sg_sequencer_state st;

    make_commit(git_dir, "head", NULL, 0, head);
    make_commit(git_dir, "c1", NULL, 0, c1);
    make_commit(git_dir, "c2", NULL, 0, c2);
    sg_sha1_to_hex(c1, hex_current);
    sg_sha1_to_hex(c2, hex_todo);

    snprintf(path, sizeof(path), "%s/CHERRY_PICK_HEAD", git_dir);
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "open CHERRY_PICK_HEAD");
        if (f != NULL) {
            fprintf(f, "%s\n", hex_current);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer", git_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/sequencer/head", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex_current);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/abort-safety", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "%s\n", hex_current);
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/sequencer/todo", git_dir);
    {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
            /* todo[0] names c2, but CHERRY_PICK_HEAD (current) names c1. */
            fprintf(f, "pick %s c2\n", hex_todo);
            fclose(f);
        }
    }

    CHECK(sg_sequencer_state_read(git_dir, &st) == -1,
         "todo[0] disagreeing with CHERRY_PICK_HEAD's current must be rejected as corrupt");

    free(git_dir);
}

/* ==================== _remove idempotence ==================== */

static void test_remove_idempotent(void)
{
    char *git_dir = make_tmp_repo();

    CHECK(sg_sequencer_state_remove(git_dir) == 0,
         "removing when nothing exists at all should succeed");
    CHECK(sg_sequencer_state_remove(git_dir) == 0, "removing again should still succeed");

    free(git_dir);
}

/* Header comment says "if somehow BOTH exist, CHERRY_PICK_HEAD wins" --
   this is the one place that rule was only ever documented, never tested. */
static void test_both_head_files_present_cherry_pick_wins(void)
{
    char *git_dir = make_tmp_repo();
    char path[4096];

    snprintf(path, sizeof(path), "%s/CHERRY_PICK_HEAD", git_dir);
    write_raw_file(path, "0000000000000000000000000000000000000000\n");
    snprintf(path, sizeof(path), "%s/REVERT_HEAD", git_dir);
    write_raw_file(path, "1111111111111111111111111111111111111111\n");

    CHECK(sg_sequencer_kind_in_progress(git_dir) == SG_SEQ_CHERRY_PICK,
         "with both CHERRY_PICK_HEAD and REVERT_HEAD present, CHERRY_PICK_HEAD must win");

    free(git_dir);
}

int main(void)
{
    test_single_commit_no_sequence();
    test_multi_commit_sequence_revert();
    test_merge_msg_with_conflicts();
    test_merge_msg_without_conflicts();
    test_malformed_current_hex_rejected();
    test_short_hex_in_todo_rejected();
    test_missing_newline_in_todo_rejected();
    test_bad_verb_in_todo_rejected();
    test_missing_head_file_rejected();
    test_todo_current_mismatch_rejected();
    test_remove_idempotent();
    test_both_head_files_present_cherry_pick_wins();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all sequencer_state tests passed\n");
    return 0;
}

#include "sg/cli_args.h"

#include "sg/hash.h"
#include "sg/index.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"
#include "sg/revparse.h"
#include "sg/tree_build.h"
#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Phase 75: the "this revision does not resolve" message matrix -- one
   reporter (sg_cli_report_rev_error) shared by every command, keyed by
   (cmd, class). See docs/DESIGN.md's Phase 75 section for the measured
   oracle table this mirrors. */

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_cli_rev_err_test_XXXXXX";
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

/* Same technique tests/test_revparse_abbrev.c's capture_stderr_start/end
   use: redirect stderr to a temp file via dup2, since every function under
   test here only ever writes to stderr. */
static int g_saved_stderr = -1;
static char g_stderr_capture_path[] = "/tmp/sg_cli_rev_err_stderr_XXXXXX";

static void capture_stderr_start(void)
{
    char path[] = "/tmp/sg_cli_rev_err_stderr_XXXXXX";
    int fd;

    fflush(stderr);
    fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed\n");
        exit(1);
    }
    memcpy(g_stderr_capture_path, path, sizeof(g_stderr_capture_path));
    g_saved_stderr = dup(STDERR_FILENO);
    if (g_saved_stderr < 0) {
        fprintf(stderr, "dup(STDERR_FILENO) failed\n");
        exit(1);
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
}

static char *capture_stderr_end(void)
{
    FILE *f;
    long len;
    char *buf;

    fflush(stderr);
    dup2(g_saved_stderr, STDERR_FILENO);
    close(g_saved_stderr);
    g_saved_stderr = -1;

    f = fopen(g_stderr_capture_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "failed to reopen stderr capture file\n");
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fprintf(stderr, "OOM reading stderr capture file\n");
        exit(1);
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "short read on stderr capture file\n");
        exit(1);
    }
    buf[len] = '\0';
    fclose(f);
    unlink(g_stderr_capture_path);
    return buf;
}

/* --- one representative check per distinguishing shape in the table --- */

static void test_ambig_unknown_block(void)
{
    char *out;

    capture_stderr_start();
    sg_cli_report_rev_error("log", SG_REV_ERR_NOT_A_REV, "nosuch", NULL, 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: ambiguous argument 'nosuch': unknown revision or path not "
                     "in the working tree.\n"
                     "Use '--' to separate paths from revisions, like this:\n"
                     "'sg <command> [<revision>...] -- [<file>...]'\n") == 0,
         "log R class must be the three-line AMBIG-UNKNOWN block, got: %s", out);
    free(out);
}

static void test_ambig_both_block(void)
{
    char *out;

    capture_stderr_start();
    sg_cli_report_rev_error("show", SG_REV_ERR_BOTH, "dual", NULL, 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: ambiguous argument 'dual': both revision and filename\n"
                     "Use '--' to separate paths from revisions, like this:\n"
                     "'sg <command> [<revision>...] -- [<file>...]'\n") == 0,
         "show D class must be the AMBIG-BOTH block (no trailing period on the first "
         "line), got: %s", out);
    free(out);
}

static void test_cat_file_p_and_ts_diverge_on_class_o(void)
{
    char *out;

    capture_stderr_start();
    sg_cli_report_rev_error("cat-file-p", SG_REV_ERR_MISSING_OBJ, "deadbeef", NULL, 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: Not a valid object name deadbeef\n") == 0,
         "cat-file -p's class O must be identical to class R (no quotes around the "
         "hex, measured against real git), got: %s", out);
    free(out);

    capture_stderr_start();
    sg_cli_report_rev_error("cat-file-ts", SG_REV_ERR_MISSING_OBJ, "deadbeef", NULL, 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: sg cat-file: could not get object info\n") == 0,
         "cat-file -t/-s's class O is a literal line naming no argument at all "
         "(the doubled 'sg', deliberate and faithful), got: %s", out);
    free(out);
}

static void test_tag_class_o_takes_two_args_in_detail_arg_order(void)
{
    char *out;

    capture_stderr_start();
    sg_cli_report_rev_error("tag", SG_REV_ERR_MISSING_OBJ, "deadbeef", "mytag", 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: trying to write ref 'refs/tags/mytag' with nonexistent "
                     "object deadbeef\n") == 0,
         "tag's class O names the REF (from `detail`) before the object (from `arg`), "
         "got: %s", out);
    free(out);
}

static void test_reset_dashdash_collapses_path_into_revision_wording(void)
{
    char *out;

    /* Measured against real git 2.55.0: `git reset --hard HEAD:nosuchfile --`
       answers with the class-R (dashdash) wording, not a path message --
       once "--" is present, class P collapses into R using the WHOLE
       argument. */
    capture_stderr_start();
    sg_cli_report_rev_error("reset", SG_REV_ERR_MISSING_PATH, "HEAD:nosuchfile", "nosuchfile", 1);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: Failed to resolve 'HEAD:nosuchfile' as a valid revision.\n") == 0,
         "reset's class P must collapse into its dashdash-R wording when \"--\" is "
         "present, got: %s", out);
    free(out);
}

static void test_merge_base_class_p_uses_r_wording(void)
{
    char *out;

    /* merge-base's P column is byte-identical to its R column (git never
       gives it a two-part path message at all). */
    capture_stderr_start();
    sg_cli_report_rev_error("merge-base", SG_REV_ERR_MISSING_PATH, "HEAD:nosuchfile", "nosuchfile", 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: Not a valid object name HEAD:nosuchfile\n") == 0,
         "merge-base's class P must print via its R format with the WHOLE argument, "
         "got: %s", out);
    free(out);
}

/* --- the sanitizer must be reentrant: a message embedding two arguments
   must not let the second sanitize() call clobber the first's buffer
   (a single static buffer would be a bug here -- see cli_args.c's own
   comment on sanitize_rev_err_arg). "path 'p' does not exist in 'r'" is
   the one wording that embeds two independently-sanitized strings in a
   single fprintf, so it is the shape that actually exercises this. --- */
static void test_sanitizer_is_reentrant_across_two_embedded_args(void)
{
    char *out;

    /* Control bytes in BOTH halves, distinct from each other, so an
       aliasing bug (second sanitize() call overwriting the first's
       buffer) would make one half wrong while the other looks right. */
    capture_stderr_start();
    sg_cli_report_rev_error("log", SG_REV_ERR_MISSING_PATH, "HEAD\x01:bad\x02path", "bad\x02path", 0);
    out = capture_stderr_end();
    CHECK(strcmp(out, "sg: path 'bad?path' does not exist in 'HEAD?'\n") == 0,
         "both embedded arguments must be independently sanitized (0x01 and 0x02 "
         "both become '?'), got: %s", out);
    free(out);
}

/* --- control-byte sanitizer: every byte 0x01-0x08/0x0b-0x1f/0x7f becomes
   '?'; tab, newline, space, and every byte >= 0x80 pass through raw
   (measured byte-by-byte against `git cat-file -p`'s vreportf). --- */
static void test_sanitizer_byte_rules(void)
{
    char *out;
    /* One byte from each class the header comment claims: 0x01 (control,
       becomes '?'), 0x09 (tab, passes), 0x0a would break the single-line
       CHECK string so it is skipped here (still covered by the "both
       halves" test above using 0x02 instead), 0x20 (space, passes), 0x7f
       (DEL, becomes '?'), and 0xc3 (>= 0x80, passes raw). */
    char raw[] = { 'a', 0x01, '\t', ' ', 0x7f, (char)0xc3, 'b', '\0' };

    capture_stderr_start();
    sg_cli_report_rev_error("cherry-pick", SG_REV_ERR_NOT_A_REV, raw, NULL, 0);
    out = capture_stderr_end();
    {
        static const char prefix[] = "sg: bad revision '";
        size_t plen = strlen(prefix);

        CHECK(strncmp(out, prefix, plen) == 0, "unexpected prefix, got: %s", out);
        CHECK(strlen(out) == plen + 9, "unexpected length for %s", out);
        CHECK(out[plen + 0] == 'a', "byte 0 (plain 'a') must pass through, got: %s", out);
        CHECK((unsigned char)out[plen + 1] == '?', "0x01 must become '?', got: %s", out);
        CHECK(out[plen + 2] == '\t', "tab must pass through raw, got: %s", out);
        CHECK(out[plen + 3] == ' ', "space must pass through raw, got: %s", out);
        CHECK((unsigned char)out[plen + 4] == '?', "0x7f must become '?', got: %s", out);
        CHECK((unsigned char)out[plen + 5] == 0xc3, ">= 0x80 must pass through raw, got: %s", out);
        CHECK(out[plen + 6] == 'b', "trailing plain byte must pass through, got: %s", out);
        CHECK(strncmp(out + plen + 7, "'\n", 2) == 0, "unexpected suffix, got: %s", out);
    }
    free(out);
}

/* --- sg_cli_classify_rev_error against a real repo: R/O/P must be told
   apart correctly (this is what every call site's dispatch depends on). --- */
/* Same shape as tests/test_revparse.c's own make_commit -- a single-blob
   tree, no shared fixture helper exists project-wide (see CLAUDE.md's
   Testing conventions). */
static void make_commit(const char *git_dir, unsigned char commit_id_out[SG_SHA1_RAW_LEN])
{
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_flat_entry entry;
    sg_commit commit;
    unsigned char *serialized;
    size_t serialized_len;

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, "hi\n", 3, blob_id) == 0, "blob write failed");

    entry.path = (char *)"f.txt";
    entry.mode = 0100644;
    memcpy(entry.sha1, blob_id, SG_SHA1_RAW_LEN);
    CHECK(sg_tree_build(git_dir, &entry, 1, tree_id) == 0, "tree build failed");

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.author_name = (char *)"tester";
    commit.author_email = (char *)"tester@example.com";
    commit.author_time = 1700000000;
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = commit.author_name;
    commit.committer_email = commit.author_email;
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");
    commit.message = (char *)"init\n";

    CHECK(sg_commit_serialize(&commit, &serialized, &serialized_len) == 0, "commit serialize failed");
    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, serialized, serialized_len, commit_id_out) == 0,
         "commit write failed");
    free(serialized);
}

static void test_classify_against_real_repo(void)
{
    char *git_dir = make_tmp_repo();
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char bad_path[4096];
    sg_rev_err_kind kind;

    make_commit(git_dir, commit_id);

    /* R: not a revision, not a well-formed hex id at all. */
    kind = sg_cli_classify_rev_error(git_dir, "nosuch", bad_path, sizeof(bad_path));
    CHECK(kind == SG_REV_ERR_NOT_A_REV, "an unresolvable plain name must classify as R, got %d", kind);

    /* O: well-formed 40-hex, object absent. */
    kind = sg_cli_classify_rev_error(git_dir, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
                                     bad_path, sizeof(bad_path));
    CHECK(kind == SG_REV_ERR_MISSING_OBJ, "an absent well-formed 40-hex id must classify as O, got %d", kind);

    /* P: <rev>:<path>, rev resolves, path does not. */
    {
        char hex[SG_SHA1_HEX_LEN + 1];
        char arg[128];

        sg_sha1_to_hex(commit_id, hex);
        snprintf(arg, sizeof(arg), "%s:nosuchfile", hex);
        kind = sg_cli_classify_rev_error(git_dir, arg, bad_path, sizeof(bad_path));
        CHECK(kind == SG_REV_ERR_MISSING_PATH,
             "a resolvable <rev> with a missing <path> must classify as P, got %d", kind);
        CHECK(strcmp(bad_path, "nosuchfile") == 0,
             "bad_path must be filled with just the <path> half, got '%s'", bad_path);
    }

    free(git_dir);
}

int main(void)
{
    test_ambig_unknown_block();
    test_ambig_both_block();
    test_cat_file_p_and_ts_diverge_on_class_o();
    test_tag_class_o_takes_two_args_in_detail_arg_order();
    test_reset_dashdash_collapses_path_into_revision_wording();
    test_merge_base_class_p_uses_r_wording();
    test_sanitizer_is_reentrant_across_two_embedded_args();
    test_sanitizer_byte_rules();
    test_classify_against_real_repo();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all cli_rev_err tests passed\n");
    return 0;
}

#include "sg/pathspec.h"

#include "sg/diff.h"
#include "sg/wildmatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                            \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                  \
            fprintf(stderr, __VA_ARGS__);                                                         \
            fprintf(stderr, "\n");                                                                \
            failures++;                                                                           \
        }                                                                                          \
    } while (0)

/* Every expectation below was measured against git 2.55.0 with `git diff
   --name-only -- <spec>` in a worktree holding exactly these paths. The
   fixture is only a chdir target: sg_pathspec_add resolves lexically and
   never asks the filesystem whether a spec exists, so no repository and no
   real files are needed -- only a cwd inside the "repo root" it is given. */
static char *make_tmp_dir(char *out, size_t out_size)
{
    char tmpl[] = "/tmp/sg_pathspec_test_XXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "setup failed: mkdtemp\n");
        exit(1);
    }
    if (chdir(tmpl) != 0) {
        fprintf(stderr, "setup failed: chdir\n");
        exit(1);
    }
    /* The root must be spelled the way getcwd spells it: on macOS /tmp is a
       symlink to /private/tmp, and a root of "/tmp/..." against a cwd of
       "/private/tmp/..." would put every single spec outside the repo. */
    if (getcwd(out, out_size) == NULL) {
        fprintf(stderr, "setup failed: getcwd\n");
        exit(1);
    }
    return out;
}

/* Resolves one spec and reports whether it covers `path`, so each case below
   reads as the one line it is. Aborts the test binary on a resolution error:
   the error cases have their own checks and get there a different way. */
static int matches(const char *root, const char *spec, const char *path)
{
    sg_pathspec ps;
    int hit;

    memset(&ps, 0, sizeof(ps));
    if (sg_pathspec_add(&ps, root, spec, NULL) != 0) {
        fprintf(stderr, "FAIL: spec '%s' unexpectedly rejected\n", spec);
        failures++;
        return -1;
    }
    hit = sg_pathspec_matches(&ps, path);
    sg_pathspec_free(&ps);
    return hit;
}

static void test_literal_and_leading_directory(const char *root)
{
    CHECK(matches(root, "a.txt", "a.txt") == 1, "exact match");
    CHECK(matches(root, "a.txt", "a.txt.bak") == 0, "a prefix is not a match");
    CHECK(matches(root, "sub", "sub/b.txt") == 1, "leading directory");
    CHECK(matches(root, "sub", "sub/deep/c.txt") == 1, "leading directory, deeper");
    CHECK(matches(root, "sub", "subway.txt") == 0, "leading directory needs the slash");
    CHECK(matches(root, "sub/deep", "sub/deep/c.txt") == 1, "nested leading directory");
    CHECK(matches(root, "sub", "a.txt") == 0, "unrelated path");
}

/* `git diff -- sub/` lists sub's contents; `git diff -- a.txt/` matches
   nothing at all, because a trailing slash asks for entries UNDER the name
   and a regular file has none. The normalizer drops the slash, so this pair
   is what proves sg_pathspec_add puts it back. */
static void test_trailing_slash(const char *root)
{
    CHECK(matches(root, "sub/", "sub/b.txt") == 1, "trailing slash still covers contents");
    CHECK(matches(root, "a.txt/", "a.txt") == 0, "trailing slash refuses the file itself");
    CHECK(matches(root, "a.txt", "a.txt") == 1, "...and the same spec without it matches");
    CHECK(matches(root, "./", "a.txt") == 1, "a trailing slash on the root is not a dead spec");
}

/* The wildcard rules that are easiest to get backwards: '*' crosses '/',
   and a spec containing a wildcard does NOT also get the leading-directory
   treatment. Measured: `git diff -- 'o[tx]her'` prints nothing even though
   "other/" has changes under it, and so do 'su?' and 'sub/dee?'. */
static void test_wildcards(const char *root)
{
    CHECK(matches(root, "*.c", "e.c") == 1, "wildcard, same directory");
    CHECK(matches(root, "*.c", "other/d.c") == 1, "'*' crosses '/'");
    CHECK(matches(root, "sub/*", "sub/deep/c.txt") == 1, "'*' crosses '/' mid-spec");
    CHECK(matches(root, "sub*", "sub/b.txt") == 1, "'sub*' works via the star, not recursion");
    CHECK(matches(root, "o[tx]her", "other/d.c") == 0, "a wildcard spec gets no leading-dir rule");
    CHECK(matches(root, "su?", "sub/b.txt") == 0, "'su?' does not recurse into sub/");
    CHECK(matches(root, "s*b", "sub/b.txt") == 0, "'s*b' matches the name, not its contents");
    CHECK(matches(root, "sub/dee?", "sub/deep/c.txt") == 0, "nested wildcard does not recurse");
    CHECK(matches(root, "o[tx]her", "other") == 1, "the class still matches the name itself");
}

/* A file whose name contains a '*' is matched by the byte-compare rule, and
   an escaped '\*' reaches the wildmatch path instead -- measured: both
   `git diff -- 'lit*st'` and `git diff -- 'lit\*st'` report the file
   literally named "lit*st". */
static void test_literal_wildcard_characters(const char *root)
{
    CHECK(matches(root, "lit*st", "lit*st") == 1, "byte compare wins for a literal '*'");
    CHECK(matches(root, "lit*st", "litXXst") == 1, "...and the same spec still globs");
    CHECK(matches(root, "lit\\*st", "lit*st") == 1, "an escaped '*' matches the literal");
    CHECK(matches(root, "lit\\*st", "litXXst") == 0, "an escaped '*' does not glob");
}

/* Rules 1 and 2 are a plain byte compare, and they run even when the spec
   contains wildcard characters -- so a DIRECTORY whose real name contains
   '[' or '*' is recursed into by a spec spelling that name literally. That
   is not a loophole in "a wildcard spec gets no leading-directory rule"
   above; it is git's own order (ps_strncmp first, wildmatch second).

   Measured against git 2.55.0 in a worktree holding a directory literally
   named "o[tx]her" and one named "st*ar": `git diff -- 'o[tx]her'` reports
   o[tx]her/f.txt, and `git diff -- 'st*ar'` reports st*ar/g.txt.

   Without this case, gating rules 1 and 2 behind !has_wildcard(spec) -- the
   obvious "fix" for the negatives in test_wildcards -- reddens nothing at
   all, in this file or in interop.sh. Verified by mutation. */
static void test_literal_rules_run_for_wildcard_specs(const char *root)
{
    CHECK(matches(root, "o[tx]her", "o[tx]her/f.txt") == 1,
          "a directory literally named o[tx]her is recursed into");
    CHECK(matches(root, "st*ar", "st*ar/g.txt") == 1,
          "so is one literally named st*ar");
    CHECK(matches(root, "o[tx]her", "o[tx]her") == 1, "and the name itself matches exactly");
    CHECK(matches(root, "o[tx]her/", "o[tx]her/f.txt") == 1,
          "the trailing-slash form works on such a name too");
    /* The control: the same spec must still NOT recurse into a directory it
       only matches via wildmatch. Losing this pairing would turn the case
       above into a licence for the over-broad rule test_wildcards forbids. */
    CHECK(matches(root, "o[tx]her", "other/d.c") == 0,
          "...while the wildcard reading of it still gets no leading-dir rule");
}

static void test_match_everything(const char *root)
{
    sg_pathspec ps;

    memset(&ps, 0, sizeof(ps));
    CHECK(sg_pathspec_matches(&ps, "anything/at/all") == 1, "an empty pathspec matches everything");
    CHECK(sg_pathspec_matches(NULL, "anything") == 1, "so does no pathspec at all");
    sg_pathspec_free(&ps);

    CHECK(matches(root, ".", "sub/b.txt") == 1, "'.' at the root matches everything");
}

static void test_multiple_specs(const char *root)
{
    sg_pathspec ps;

    memset(&ps, 0, sizeof(ps));
    CHECK(sg_pathspec_add(&ps, root, "a.txt", NULL) == 0, "add a.txt");
    CHECK(sg_pathspec_add(&ps, root, "sub", NULL) == 0, "add sub");
    CHECK(ps.count == 2, "two specs held");
    CHECK(sg_pathspec_matches(&ps, "a.txt") == 1, "first spec");
    CHECK(sg_pathspec_matches(&ps, "sub/b.txt") == 1, "second spec");
    CHECK(sg_pathspec_matches(&ps, "other/d.c") == 0, "neither spec");
    sg_pathspec_free(&ps);
    CHECK(ps.specs == NULL && ps.count == 0, "free zeroes the pathspec");
}

static void test_errors(const char *root)
{
    sg_pathspec ps;
    sg_pathspec_error err;

    memset(&ps, 0, sizeof(ps));

    err = SG_PATHSPEC_ERR_NONE;
    CHECK(sg_pathspec_add(&ps, root, "", &err) == -1, "the empty string is rejected");
    CHECK(err == SG_PATHSPEC_ERR_EMPTY, "...as EMPTY, not as something else");

    err = SG_PATHSPEC_ERR_NONE;
    CHECK(sg_pathspec_add(&ps, root, "..", &err) == -1, "'..' escapes the worktree");
    CHECK(err == SG_PATHSPEC_ERR_OUTSIDE, "...and is reported as OUTSIDE");

    err = SG_PATHSPEC_ERR_NONE;
    CHECK(sg_pathspec_add(&ps, root, "/etc/passwd", &err) == -1, "an absolute path outside is rejected");
    CHECK(err == SG_PATHSPEC_ERR_OUTSIDE, "...as OUTSIDE");

    err = SG_PATHSPEC_ERR_NONE;
    CHECK(sg_pathspec_add(&ps, root, ":(icase)a.txt", &err) == -1, "pathspec magic is rejected");
    CHECK(err == SG_PATHSPEC_ERR_MAGIC, "...as MAGIC, so the CLI can say why");

    err = SG_PATHSPEC_ERR_NONE;
    CHECK(sg_pathspec_add(&ps, root, ":!sub", &err) == -1, "exclusion magic is rejected too");
    CHECK(err == SG_PATHSPEC_ERR_MAGIC, "...also as MAGIC");

    CHECK(ps.count == 0, "no rejected spec was stored");
    sg_pathspec_free(&ps);
}

/* A spec does not have to exist: `sg diff -- deleted.txt` is the only way to
   ask about a file that is gone, which is exactly when it cannot be stat'd. */
static void test_nonexistent_path_resolves(const char *root)
{
    CHECK(matches(root, "nosuch.txt", "nosuch.txt") == 1, "a missing path still resolves");
    CHECK(matches(root, "no/such/dir", "no/such/dir/f") == 1, "so does a missing directory");
}

static void test_absolute_inside_repo(const char *root)
{
    char abs[4096];

    snprintf(abs, sizeof(abs), "%s/sub/b.txt", root);
    CHECK(matches(root, abs, "sub/b.txt") == 1, "an absolute path inside the repo resolves");
}

/* Specs are relative to the current directory, not the repository root --
   `cd sub && git diff -- b.txt` reports sub/b.txt, and '..' walks back out
   as far as the root but no further. */
static void test_relative_to_cwd(const char *root)
{
    char sub[4096];

    snprintf(sub, sizeof(sub), "%s/sub", root);
    if (mkdir(sub, 0755) != 0 || chdir(sub) != 0) {
        fprintf(stderr, "setup failed: could not enter %s\n", sub);
        exit(1);
    }

    CHECK(matches(root, "b.txt", "sub/b.txt") == 1, "a bare name is relative to the cwd");
    CHECK(matches(root, "b.txt", "b.txt") == 0, "...and not to the repository root");
    CHECK(matches(root, "../a.txt", "a.txt") == 1, "'..' climbs back toward the root");
    CHECK(matches(root, ".", "sub/deep/c.txt") == 1, "'.' in a subdirectory means that subdirectory");
    CHECK(matches(root, ".", "a.txt") == 0, "...and nothing above it");
    CHECK(matches(root, "*.txt", "sub/deep/c.txt") == 1, "a wildcard is anchored at the cwd too");
    CHECK(matches(root, "*.txt", "a.txt") == 0, "...so it cannot reach above the cwd");

    if (chdir(root) != 0) {
        fprintf(stderr, "setup failed: could not return to %s\n", root);
        exit(1);
    }
}

static void test_looks_like_spec(void)
{
    CHECK(sg_pathspec_looks_like_spec("*.c") == 1, "'*' looks like a spec");
    CHECK(sg_pathspec_looks_like_spec("a?b") == 1, "'?' looks like a spec");
    CHECK(sg_pathspec_looks_like_spec("a[bc]") == 1, "'[' looks like a spec");
    CHECK(sg_pathspec_looks_like_spec("a\\b") == 1, "'\\' looks like a spec");
    CHECK(sg_pathspec_looks_like_spec(":(icase)x") == 1, "magic looks like a spec");
    CHECK(sg_pathspec_looks_like_spec("a.txt") == 0, "a plain name does not");
    CHECK(sg_pathspec_looks_like_spec("sub/deep") == 0, "nor does a plain path");
}

/* The property pathspec matching leans on, stated on its own because
   ignore.c's segment layer hides it: sg_wildmatch does not know '/' exists. */
static void test_wildmatch_does_not_know_about_slashes(void)
{
    CHECK(sg_wildmatch("*", 1, "a/b/c", 5) == 1, "'*' spans the whole path");
    CHECK(sg_wildmatch("a*c", 3, "a/b/c", 5) == 1, "'*' spans directory separators");
    CHECK(sg_wildmatch("?", 1, "/", 1) == 1, "'?' matches a '/' like any other byte");
    CHECK(sg_wildmatch("a*", 2, "b/a", 3) == 0, "matching is still anchored at both ends");
}

/* sg_diff_list_filter drops entries in place. The unmerged pair (two
   adjacent entries sharing one path, see sg_diff_index_workdir) must survive
   or go together -- a filter that split them would leave a "U" row with no
   content row, or the reverse. */
static void add_entry(sg_diff_list *list, const char *path, int unmerged)
{
    sg_diff_entry *grown = realloc(list->entries, (list->count + 1) * sizeof(*grown));

    if (grown == NULL) {
        fprintf(stderr, "setup failed: realloc\n");
        exit(1);
    }
    list->entries = grown;
    memset(&list->entries[list->count], 0, sizeof(list->entries[list->count]));
    list->entries[list->count].path = strdup(path);
    list->entries[list->count].unmerged = unmerged;
    if (list->entries[list->count].path == NULL) {
        fprintf(stderr, "setup failed: strdup\n");
        exit(1);
    }
    list->count++;
    list->cap = list->count;
}

static void test_diff_list_filter(const char *root)
{
    sg_diff_list list;
    sg_pathspec ps;

    memset(&list, 0, sizeof(list));
    add_entry(&list, "a.txt", 0);
    add_entry(&list, "conflict.txt", 1);
    add_entry(&list, "conflict.txt", 0);
    add_entry(&list, "sub/b.txt", 0);

    memset(&ps, 0, sizeof(ps));
    sg_diff_list_filter(&list, &ps);
    CHECK(list.count == 4, "an empty pathspec filters nothing");

    if (sg_pathspec_add(&ps, root, "conflict.txt", NULL) != 0) {
        fprintf(stderr, "setup failed: sg_pathspec_add\n");
        exit(1);
    }
    sg_diff_list_filter(&list, &ps);
    CHECK(list.count == 2, "only the two conflict rows survive");
    if (list.count == 2) {
        CHECK(strcmp(list.entries[0].path, "conflict.txt") == 0, "first survivor is the conflict");
        CHECK(list.entries[0].unmerged == 1, "the unmerged row kept its flag");
        CHECK(strcmp(list.entries[1].path, "conflict.txt") == 0, "second survivor is its pair");
        CHECK(list.entries[1].unmerged == 0, "the content row is still the second of the pair");
    }

    sg_pathspec_free(&ps);
    sg_diff_list_free(&list);

    memset(&ps, 0, sizeof(ps));
    memset(&list, 0, sizeof(list));
    add_entry(&list, "a.txt", 0);
    if (sg_pathspec_add(&ps, root, "nothing", NULL) != 0) {
        fprintf(stderr, "setup failed: sg_pathspec_add\n");
        exit(1);
    }
    sg_diff_list_filter(&list, &ps);
    CHECK(list.count == 0, "a pathspec matching nothing empties the list");
    sg_pathspec_free(&ps);
    sg_diff_list_free(&list);
}

int main(void)
{
    char root_buf[4096];
    const char *root = make_tmp_dir(root_buf, sizeof(root_buf));

    test_literal_and_leading_directory(root);
    test_trailing_slash(root);
    test_wildcards(root);
    test_literal_rules_run_for_wildcard_specs(root);
    test_literal_wildcard_characters(root);
    test_match_everything(root);
    test_multiple_specs(root);
    test_errors(root);
    test_nonexistent_path_resolves(root);
    test_absolute_inside_repo(root);
    test_relative_to_cwd(root);
    test_looks_like_spec();
    test_wildmatch_does_not_know_about_slashes();
    test_diff_list_filter(root);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_pathspec: all checks passed\n");
    return 0;
}

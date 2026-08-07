#include "sg/ignore.h"

#include "sg/workdir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

static char *make_tmp_root(void)
{
    static char template[] = "/tmp/sg_ignore_test_XXXXXX";
    char *path = strdup(template);

    if (path == NULL || mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    return path;
}

static void write_str(const char *root, const char *rel, const char *content, size_t len)
{
    char path[8192];

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    CHECK(sg_write_file_mkdirs(path, (const unsigned char *)content, len, 0644) == 0,
          "writing %s failed", rel);
}

/* One repo-level query: writes .git/info/exclude and the root .gitignore,
   opens a fresh matcher and checks a single is_ignored answer. */
typedef struct {
    const char *exclude;   /* .git/info/exclude content */
    const char *gitignore; /* root .gitignore content */
    const char *path;      /* repo-root-relative query */
    int is_dir;
    int expected;
} ignore_case;

static void run_case(const char *root, const ignore_case *c)
{
    char git_dir[8192];
    sg_ignore *ig = NULL;
    int got;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_str(root, ".git/info/exclude", c->exclude, strlen(c->exclude));
    write_str(root, ".gitignore", c->gitignore, strlen(c->gitignore));

    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed for rules '%s'", c->gitignore);
    if (ig == NULL)
        return;
    got = sg_ignore_is_ignored(ig, c->path, c->is_dir);
    CHECK(got == c->expected, "rules '%s' exclude '%s' path '%s' is_dir=%d: got %d, want %d",
          c->gitignore, c->exclude, c->path, c->is_dir, got, c->expected);
    sg_ignore_free(ig);
}

/* Every single-file behavior verified against git 2.55 in the phase 9 spec
   probes, as one table. */
static const ignore_case root_cases[] = {
    /* --- empty / blank lines / comments --- */
    {"", "", "anything", 0, 0},
    {"", "\nfoo\n\n", "foo", 0, 1},
    {"", "\nfoo\n\n", "bar", 0, 0},
    {"", "#lit\n", "#lit", 0, 0},   /* comment line has no effect */
    {"", "\\#lit\n", "#lit", 0, 1}, /* escaped literal leading hash */

    /* --- negation parsing --- */
    {"", "!bang\n", "bang", 0, 0},    /* negation with nothing to negate */
    {"", "\\!bang\n", "!bang", 0, 1}, /* escaped literal leading bang */

    /* --- trailing spaces --- */
    {"", "sp1 \n", "sp1", 0, 1},     /* unescaped trailing space stripped */
    {"", "sp1 \n", "sp1 ", 0, 0},
    {"", "sp2\\ \n", "sp2 ", 0, 1},  /* escaped trailing space kept */
    {"", "sp2\\ \n", "sp2", 0, 0},

    /* --- CRLF --- */
    {"", "crlf\r\n", "crlf", 0, 1},

    /* --- trailing '/': directories only --- */
    {"", "foo/\n", "foo", 1, 1},
    {"", "foo/\n", "foo", 0, 0},
    {"", "foo/\n", "a/foo", 1, 1},        /* dir-only basename works at depth */
    {"", "foo/\n", "a/foo", 0, 0},        /* the FILE a/foo is not matched */
    {"", "foo/\n", "a/foo/inside", 0, 1}, /* content buried under ignored dir */

    /* --- anchoring --- */
    {"", "foo\n", "foo", 0, 1}, /* no slash: basename at any depth */
    {"", "foo\n", "a/foo", 0, 1},
    {"", "foo\n", "a/b/foo", 0, 1},
    {"", "/foo\n", "foo", 0, 1}, /* leading slash: top level only */
    {"", "/foo\n", "a/foo", 0, 0},
    {"", "doc/frotz\n", "doc/frotz", 0, 1}, /* inner slash anchors too */
    {"", "doc/frotz\n", "a/doc/frotz", 0, 0},
    {"", "a/*.txt\n", "a/x.txt", 0, 1},
    {"", "a/*.txt\n", "a/b/c.txt", 0, 0}, /* '*' does not cross '/' when anchored */

    /* --- wildcards --- */
    {"", "*.log\n", "x.log", 0, 1},
    {"", "*.log\n", "a/b/x.log", 0, 1},
    {"", "*.log\n", "x.logx", 0, 0},
    {"", "f?o\n", "foo", 0, 1},
    {"", "f?o\n", "fo", 0, 0},        /* '?' is exactly one character */
    {"", "a/f?o\n", "a/f/o", 0, 0},   /* '?' never matches '/' */

    /* --- character classes --- */
    {"", "[a-c]x\n", "ax", 0, 1},
    {"", "[a-c]x\n", "bx", 0, 1},
    {"", "[a-c]x\n", "dx", 0, 0},
    {"", "[!d]y\n", "by", 0, 1},
    {"", "[!d]y\n", "dy", 0, 0},
    {"", "[^e]z\n", "fz", 0, 1}, /* '^' accepted as negation synonym */
    {"", "[^e]z\n", "ez", 0, 0},
    /* Unterminated '[': git 2.55 (probed on this machine) treats the whole
       rule as unmatchable -- wildmatch aborts -- so nothing is ignored. */
    {"", "a[b\n", "a[b", 0, 0},
    {"", "a[b\n", "ab", 0, 0},
    {"", "[]a]x\n", "]x", 0, 1}, /* ']' first in a class is a literal member */
    {"", "[]a]x\n", "ax", 0, 1},
    {"", "[]a]x\n", "bx", 0, 0},
    {"", "[a-]y\n", "-y", 0, 1}, /* trailing '-' is a literal member */
    {"", "[a-]y\n", "ay", 0, 1},

    /* --- '**' --- */
    {"", "**/foo\n", "foo", 0, 1}, /* leading: any depth including zero */
    {"", "**/foo\n", "a/foo", 0, 1},
    {"", "**/foo\n", "a/b/foo", 0, 1},
    {"", "ab/**\n", "ab/f", 0, 1}, /* trailing: everything inside */
    {"", "ab/**\n", "ab/c/g", 0, 1},
    {"", "ab/**\n", "ab", 1, 0},     /* ...but not the directory itself */
    {"", "x/**/y\n", "x/y", 0, 1},   /* middle: zero directories included */
    {"", "x/**/y\n", "x/m/n/y", 0, 1},
    {"", "x/**/y\n", "x/ay", 0, 0},  /* a "**" segment only eats whole dirs */
    {"", "a**b\n", "axxb", 0, 1},    /* '**' not alone in a segment: '*' */
    {"", "a**b\n", "ab", 0, 1},
    {"", "a**b\n", "a/qb", 0, 0},    /* ...and it does not cross '/' */

    /* --- backslash escapes --- */
    {"", "\\*lit\n", "*lit", 0, 1},
    {"", "\\*lit\n", "xlit", 0, 0},

    /* --- case sensitivity --- */
    {"", "Foo\n", "foo", 0, 0},
    {"", "Foo\n", "Foo", 0, 1},

    /* --- last matching pattern wins --- */
    {"", "*.log\n!keep.log\n", "keep.log", 0, 0},
    {"", "*.log\n!keep.log\n", "other.log", 0, 1},
    {"", "!keep.log\n*.log\n", "keep.log", 0, 1}, /* reversed order flips it */
    {"", "sub2/\n!sub2/\n", "sub2", 1, 0},
    {"", "sub2/\n!sub2/\n", "sub2/x", 0, 0},

    /* --- ignored directories bury their content --- */
    {"", "sub/\n!sub/file.txt\n", "sub/file.txt", 0, 1},
    {"", "sub/\n!sub/file.txt\n", "sub", 1, 1},

    /* --- .git/info/exclude: lowest precedence, behaves as at the root --- */
    {"exc\n", "", "exc", 0, 1},
    {"exc\n", "!exc\n", "exc", 0, 0}, /* root .gitignore overrides exclude */
    {"exc\n", "", "sub/exc", 0, 1},   /* basename semantics from the root */
    {"d/only\n", "", "d/only", 0, 1}, /* anchored as if at the repo root */
    {"d/only\n", "", "x/d/only", 0, 0},
};

static void test_root_table(void)
{
    char *root = make_tmp_root();
    size_t i;

    for (i = 0; i < sizeof(root_cases) / sizeof(root_cases[0]); i++)
        run_case(root, &root_cases[i]);
    free(root);
}

/* Deeper .gitignore beats shallower: root ignores 'secret', over/.gitignore
   un-ignores it -- but only while 'over' is pushed. */
static void test_deeper_gitignore_wins(void)
{
    char *root = make_tmp_root();
    char git_dir[8192];
    sg_ignore *ig = NULL;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_str(root, ".gitignore", "secret\n", 7);
    write_str(root, "over/.gitignore", "!secret\n", 8);

    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_is_ignored(ig, "secret", 0) == 1, "root secret should be ignored");
    CHECK(sg_ignore_is_ignored(ig, "over/secret", 0) == 1,
          "before pushing 'over' its .gitignore must not apply");

    CHECK(sg_ignore_push_dir(ig, "over") == 0, "push over failed");
    CHECK(sg_ignore_is_ignored(ig, "over/secret", 0) == 0,
          "deeper !secret must beat the root's secret");
    CHECK(sg_ignore_is_ignored(ig, "secret", 0) == 1,
          "root-level secret must stay ignored while inside over/");

    sg_ignore_pop_dir(ig);
    CHECK(sg_ignore_is_ignored(ig, "over/secret", 0) == 1,
          "after popping, the root rule applies again");

    sg_ignore_free(ig);
    free(root);
}

/* Once a directory is ignored, a deeper .gitignore cannot resurrect its
   content either. */
static void test_no_resurrection_across_frames(void)
{
    char *root = make_tmp_root();
    char git_dir[8192];
    sg_ignore *ig = NULL;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_str(root, ".gitignore", "sub/\n", 5);
    write_str(root, "sub/.gitignore", "!file.txt\n", 10);

    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_push_dir(ig, "sub") == 0, "push sub failed");
    CHECK(sg_ignore_is_ignored(ig, "sub/file.txt", 0) == 1,
          "!file.txt in sub/.gitignore must not resurrect content of ignored sub/");
    CHECK(sg_ignore_is_ignored(ig, "sub", 1) == 1, "sub itself stays ignored");
    sg_ignore_pop_dir(ig);

    sg_ignore_free(ig);
    free(root);
}

/* Nested pushes stack correctly, pops unwind one level at a time, and
   popping past the root frame is a silent no-op. */
static void test_push_pop_stack(void)
{
    char *root = make_tmp_root();
    char git_dir[8192];
    sg_ignore *ig = NULL;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    /* no root .gitignore at all: missing files are normal */
    write_str(root, "a/.gitignore", "*.o\n", 4);
    write_str(root, "a/b/.gitignore", "!keep.o\n", 8);

    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open without any root .gitignore failed");
    CHECK(sg_ignore_is_ignored(ig, "a/x.o", 0) == 0, "nothing pushed yet: not ignored");

    CHECK(sg_ignore_push_dir(ig, "a") == 0, "push a failed");
    CHECK(sg_ignore_is_ignored(ig, "a/x.o", 0) == 1, "a/.gitignore *.o applies");

    CHECK(sg_ignore_push_dir(ig, "a/b") == 0, "push a/b failed");
    CHECK(sg_ignore_is_ignored(ig, "a/b/keep.o", 0) == 0, "deeper !keep.o wins");
    CHECK(sg_ignore_is_ignored(ig, "a/b/x.o", 0) == 1, "other .o files stay ignored");

    sg_ignore_pop_dir(ig);
    CHECK(sg_ignore_is_ignored(ig, "a/b/keep.o", 0) == 1, "negation gone after pop");

    sg_ignore_pop_dir(ig);
    CHECK(sg_ignore_is_ignored(ig, "a/x.o", 0) == 0, "back to no rules at all");

    /* popping past the root frame must be ignored, and the matcher must
       still answer queries afterwards */
    sg_ignore_pop_dir(ig);
    sg_ignore_pop_dir(ig);
    CHECK(sg_ignore_is_ignored(ig, "a/x.o", 0) == 0, "matcher survives excess pops");
    CHECK(sg_ignore_push_dir(ig, "a") == 0, "push still works after excess pops");
    CHECK(sg_ignore_is_ignored(ig, "a/x.o", 0) == 1, "pushed frame applies again");
    sg_ignore_pop_dir(ig);

    sg_ignore_free(ig);
    free(root);
}

/* An unreadable .gitignore is treated exactly like a missing one. */
static void test_unreadable_treated_as_absent(void)
{
    char *root = make_tmp_root();
    char git_dir[8192];
    char gi_path[8192];
    sg_ignore *ig = NULL;

    if (geteuid() == 0) {
        free(root);
        return; /* root ignores file modes; the test would be meaningless */
    }

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", root);
    write_str(root, ".gitignore", "foo\n", 4);

    CHECK(chmod(gi_path, 0000) == 0, "chmod 000 failed");
    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open must succeed anyway");
    CHECK(sg_ignore_is_ignored(ig, "foo", 0) == 0, "unreadable .gitignore must be ignored");
    sg_ignore_free(ig);

    CHECK(chmod(gi_path, 0644) == 0, "chmod back failed");
    ig = NULL;
    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "reopen failed");
    CHECK(sg_ignore_is_ignored(ig, "foo", 0) == 1, "readable again: rule applies");
    sg_ignore_free(ig);
    free(root);
}

/* Lines longer than any plausible internal buffer must match exactly, never
   on a truncated prefix. */
static void test_long_pattern_no_truncation(void)
{
    enum { N = 8000 };
    char *root = make_tmp_root();
    char git_dir[8192];
    char *content = malloc(N + 2);
    char *path_full = malloc(N + 1);
    sg_ignore *ig = NULL;

    CHECK(content != NULL && path_full != NULL, "oom");
    if (content == NULL || path_full == NULL)
        exit(1);
    memset(content, 'a', N);
    content[N] = '\n';
    memset(path_full, 'a', N);
    path_full[N] = '\0';

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_str(root, ".gitignore", content, N + 1);

    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_is_ignored(ig, path_full, 0) == 1, "8000-char pattern must match exactly");
    path_full[N - 1] = '\0';
    CHECK(sg_ignore_is_ignored(ig, path_full, 0) == 0,
          "a 7999-char path must NOT match (would indicate prefix truncation)");

    sg_ignore_free(ig);
    free(content);
    free(path_full);
    free(root);
}

/* Hostile patterns must complete quickly and must not smash the stack:
   10,000 consecutive stars, the classic a*a*...a*b backtracking bomb, and a
   deep path against many '**' segments. */
static void test_pathological_patterns(void)
{
    char *root = make_tmp_root();
    char git_dir[8192];
    char *content;
    char *path;
    sg_ignore *ig = NULL;
    clock_t t0;
    double elapsed;
    size_t i;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);

    /* (1) 10,000 '*' characters against a 2,000-char path */
    content = malloc(10001);
    path = malloc(2001);
    CHECK(content != NULL && path != NULL, "oom");
    if (content == NULL || path == NULL)
        exit(1);
    memset(content, '*', 10000);
    content[10000] = '\n';
    memset(path, 'a', 2000);
    path[2000] = '\0';
    write_str(root, ".gitignore", content, 10001);
    free(content);

    t0 = clock();
    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_is_ignored(ig, path, 0) == 1, "all-stars pattern matches everything");
    sg_ignore_free(ig);
    ig = NULL;
    elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(elapsed < 5.0, "10,000-star pattern took %.2fs (must stay polynomial)", elapsed);

    /* (2) "a*a*a*...a*b" (2,500 repetitions) against 3,000 'a's: the naive
       recursive matcher is exponential here; two-pointer must stay fast */
    content = malloc(2 * 2500 + 2);
    CHECK(content != NULL, "oom");
    if (content == NULL)
        exit(1);
    for (i = 0; i < 2500; i++) {
        content[2 * i] = 'a';
        content[2 * i + 1] = '*';
    }
    content[2 * 2500] = 'b';
    content[2 * 2500 + 1] = '\n';
    /* reuse the 2,000-char all-'a' path from (1): no 'b', must NOT match */
    write_str(root, ".gitignore", content, 2 * 2500 + 2);
    free(content);

    t0 = clock();
    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_is_ignored(ig, path, 0) == 0, "a*a*...b bomb must simply not match");
    sg_ignore_free(ig);
    ig = NULL;
    elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(elapsed < 5.0, "a*...*b bomb took %.2fs (must stay polynomial)", elapsed);
    free(path);

    /* (3) many '**' segments against a 400-segment path: segment matching
       is iterative too, so this must neither recurse deeply nor blow up */
    content = malloc(3 * 200 + 2 + 1);
    path = malloc(2 * 400 + 2);
    CHECK(content != NULL && path != NULL, "oom");
    if (content == NULL || path == NULL)
        exit(1);
    for (i = 0; i < 200; i++)
        memcpy(content + 3 * i, "**/", 3);
    content[3 * 200] = 'x';
    content[3 * 200 + 1] = '\n';
    for (i = 0; i < 400; i++)
        memcpy(path + 2 * i, "a/", 2);
    path[2 * 400] = 'x';
    path[2 * 400 + 1] = '\0';
    write_str(root, ".gitignore", content, 3 * 200 + 2);
    free(content);

    t0 = clock();
    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_is_ignored(ig, path, 0) == 1, "200 '**/' segments then x should match");
    path[2 * 400] = 'y';
    CHECK(sg_ignore_is_ignored(ig, path, 0) == 0, "...and y at the end should not");
    sg_ignore_free(ig);
    elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(elapsed < 5.0, "deep '**' matching took %.2fs (must stay polynomial)", elapsed);
    free(path);
    free(root);
}

/* Degenerate arguments must not crash and must fail cleanly. */
static void test_argument_edges(void)
{
    char *root = make_tmp_root();
    char git_dir[8192];
    sg_ignore *ig = NULL;

    snprintf(git_dir, sizeof(git_dir), "%s/.git", root);
    write_str(root, ".gitignore", "foo\n", 4);

    CHECK(sg_ignore_open(&ig, git_dir, root) == 0, "open failed");
    CHECK(sg_ignore_is_ignored(ig, NULL, 0) == 0, "NULL path is never ignored");
    CHECK(sg_ignore_is_ignored(ig, "", 0) == 0, "empty path is never ignored");
    CHECK(sg_ignore_is_ignored(NULL, "foo", 0) == 0, "NULL matcher answers not-ignored");
    CHECK(sg_ignore_push_dir(ig, NULL) == -1, "pushing NULL must fail");
    CHECK(sg_ignore_push_dir(ig, "") == -1, "pushing the empty dir must fail");
    CHECK(sg_ignore_is_ignored(ig, "foo", 0) == 1, "matcher still works after bad pushes");
    sg_ignore_free(ig);
    sg_ignore_free(NULL); /* must be a no-op */

    CHECK(sg_ignore_open(NULL, git_dir, root) == -1, "open with NULL out must fail");
    free(root);
}

int main(void)
{
    test_root_table();
    test_deeper_gitignore_wins();
    test_no_resurrection_across_frames();
    test_push_pop_stack();
    test_unreadable_treated_as_absent();
    test_long_pattern_no_truncation();
    test_pathological_patterns();
    test_argument_edges();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all ignore tests passed\n");
    return 0;
}

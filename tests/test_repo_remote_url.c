#include "sg/repo.h"

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

static char *make_tmp_repo(void)
{
    static char template[] = "/tmp/sg_repo_remote_url_test_XXXXXX";
    char *path = strdup(template);

    if (mkdtemp(path) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    if (sg_repo_init(path) != 0) {
        fprintf(stderr, "sg_repo_init failed\n");
        exit(1);
    }
    return path;
}

static char *git_dir_of(const char *repo)
{
    char buf[4096];

    snprintf(buf, sizeof(buf), "%s/.git", repo);
    return strdup(buf);
}

static void append_remote(const char *git_dir, const char *name, const char *url)
{
    char path[4096];
    FILE *f;

    snprintf(path, sizeof(path), "%s/config", git_dir);
    f = fopen(path, "a");
    if (f == NULL) {
        fprintf(stderr, "failed to open config for append\n");
        exit(1);
    }
    fprintf(f, "[remote \"%s\"]\n\turl = %s\n", name, url);
    fclose(f);
}

/* The header[256] fixed buffer used to snprintf() the "[remote \"<name>\"]"
   section marker; a remote name long enough to overflow it made the
   generated header SHORTER than the section it needed to match, so the
   strcmp() comparing them could never succeed -- a well-formed section was
   silently reported as "remote not configured". Fail-CLOSED: safe, but
   still a wrong answer. This regression-tests the header[256] site. */
static void test_long_remote_name_is_found(void)
{
    char *repo = make_tmp_repo();
    char *git_dir = git_dir_of(repo);
    /* 400 'r's: comfortably longer than the section header the old fixed
       256-byte buffer could hold ("[remote \"" + name + "\"]"). */
    char long_name[401];
    char *url;

    memset(long_name, 'r', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    append_remote(git_dir, long_name, "https://example.com/repo.git");

    url = sg_repo_read_remote_url(git_dir, long_name);
    CHECK(url != NULL, "expected a url for a %zu-char remote name, got NULL",
          strlen(long_name));
    if (url != NULL)
        CHECK(strcmp(url, "https://example.com/repo.git") == 0,
              "wrong url for long remote name: got '%s'", url);

    free(url);
    free(git_dir);
    free(repo);
}

/* The line[1024] fixed buffer, read with fgets(), silently truncated any
   "url = ..." value once the whole line exceeded ~1024 bytes -- fgets()
   returns a partial line at the buffer boundary with no error at all, so a
   caller (e.g. sg fetch) went on to use a DIFFERENT, truncated URL with no
   warning. This is the fail-OPEN half: it does not refuse, it silently
   answers a wrong question. Regression-tests the line[1024]/fgets site. */
static void test_long_url_is_not_truncated(void)
{
    char *repo = make_tmp_repo();
    char *git_dir = git_dir_of(repo);
    /* 2029 'u' characters plus a fixed suffix, comfortably longer than the
       old 1024-byte line buffer (which also had to hold "\turl = " and the
       trailing newline). */
    char long_url[2029 + 32];
    char *url;
    size_t n;

    n = (size_t)snprintf(long_url, sizeof(long_url), "https://example.com/");
    memset(long_url + n, 'u', 2029);
    n += 2029;
    n += (size_t)snprintf(long_url + n, sizeof(long_url) - n, "/repo.git");

    append_remote(git_dir, "origin", long_url);

    url = sg_repo_read_remote_url(git_dir, "origin");
    CHECK(url != NULL, "expected a url for a %zu-char url, got NULL", strlen(long_url));
    if (url != NULL) {
        CHECK(strlen(url) == strlen(long_url), "url length mismatch: got %zu, want %zu",
              strlen(url), strlen(long_url));
        CHECK(strcmp(url, long_url) == 0, "long url was truncated or altered");
    }

    free(url);
    free(git_dir);
    free(repo);
}

int main(void)
{
    test_long_remote_name_is_found();
    test_long_url_is_not_truncated();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all repo remote url tests passed\n");
    return 0;
}

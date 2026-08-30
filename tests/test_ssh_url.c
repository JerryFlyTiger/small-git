/* SSH URL detection and splitting (Phase 47). Every expectation here was
   measured against real git 2.55.0 by pointing GIT_SSH_COMMAND at a script
   that logs its argv and running `git ls-remote <url>`; the interesting rows
   are the ones where the two URL forms DISAGREE about the leading slash. */

#include "sg/ssh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);                                 \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fprintf(stderr, "\n");                                                               \
            failures++;                                                                          \
        }                                                                                        \
    } while (0)

static void expect_ssh(const char *url, int want)
{
    int got = sg_ssh_is_ssh_url(url);

    CHECK(got == want, "'%s': expected is_ssh=%d, got %d", url, want, got);
}

static void test_detection(void)
{
    expect_ssh("ssh://host/srv/repo.git", 1);
    expect_ssh("ssh://user@host:2222/srv/repo.git", 1);
    expect_ssh("host:repo.git", 1);
    expect_ssh("user@host:srv/repo.git", 1);
    /* Not ours: a scheme's own authority also carries a colon, and the
       "://" test is what keeps the scp-like rule from having to know about
       schemes at all. */
    expect_ssh("http://host/repo.git", 0);
    expect_ssh("https://user@host/repo.git", 0);
    /* A local path is not a URL. The second one is the rule's whole point:
       the colon comes AFTER a slash, so it is a path with a colon in it,
       not host:path. */
    expect_ssh("/tmp/repo.git", 0);
    expect_ssh("sub/dir:weird", 0);
    expect_ssh("repo.git", 0);
    /* A leading colon has no host in front of it. */
    expect_ssh(":repo.git", 0);
}

static void expect_parse(const char *url, const char *want_host, const char *want_port,
                         const char *want_path)
{
    char *host = NULL, *port = NULL, *path = NULL;
    int rc = sg_ssh_parse_url(url, &host, &port, &path);

    if (want_host == NULL) {
        CHECK(rc != 0, "'%s': expected a parse failure, got host='%s'", url, host ? host : "");
        free(host);
        free(port);
        free(path);
        return;
    }
    CHECK(rc == 0, "'%s': expected success, got %d", url, rc);
    if (rc != 0)
        return;
    CHECK(host != NULL && strcmp(host, want_host) == 0, "'%s': host is '%s', want '%s'", url,
         host ? host : "(null)", want_host);
    if (want_port == NULL)
        CHECK(port == NULL, "'%s': port should be absent, got '%s'", url, port ? port : "");
    else
        CHECK(port != NULL && strcmp(port, want_port) == 0, "'%s': port is '%s', want '%s'", url,
             port ? port : "(null)", want_port);
    CHECK(path != NULL && strcmp(path, want_path) == 0, "'%s': path is '%s', want '%s'", url,
         path ? path : "(null)", want_path);
    free(host);
    free(port);
    free(path);
}

static void test_parse(void)
{
    /* ssh:// KEEPS the leading slash ... */
    expect_parse("ssh://host/srv/repo.git", "host", NULL, "/srv/repo.git");
    expect_parse("ssh://user@host/srv/repo.git", "user@host", NULL, "/srv/repo.git");
    expect_parse("ssh://user@host:2222/srv/repo.git", "user@host", "2222", "/srv/repo.git");
    /* ... except before a '~', where git drops it so the far side's shell
       can expand the home directory. Measured; it is not a typo. */
    expect_parse("ssh://host/~alice/repo.git", "host", NULL, "~alice/repo.git");
    /* The scp-like form has no slash to keep, and "host:22" is a PATH named
       22 -- there is no port in this syntax at all. Reading it as a port
       would silently ask the far side for the wrong repository. */
    expect_parse("host:repo.git", "host", NULL, "repo.git");
    expect_parse("user@host:srv/repo.git", "user@host", NULL, "srv/repo.git");
    expect_parse("host:22", "host", NULL, "22");
    /* Where the local error lives, measured: only a form with no path
       SEPARATOR at all fails here. An empty path after the separator is
       passed through and refused by the far side, which is what git does --
       "ssh://host/" asks for "/" and "host:" asks for "". Rejecting those
       locally would be a different, tidier-looking answer than git's. */
    expect_parse("ssh://host", NULL, NULL, NULL);
    expect_parse("ssh://host/", "host", NULL, "/");
    expect_parse("host:", "host", NULL, "");
}

int main(void)
{
    test_detection();
    test_parse();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all ssh_url tests passed\n");
    return 0;
}

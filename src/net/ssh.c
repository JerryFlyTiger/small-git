#include "sg/ssh.h"

#include "sg/pktline.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* This file is the project's FIRST subprocess: nothing else in src/ forks or
   execs anything. The shapes that follow (a fork/exec with two pipes, a
   poll loop that writes and reads at the same time, SIGPIPE ignored around
   the write, and a wait that turns the child's exit into our own error) are
   therefore not copied from anywhere in-tree -- they are here because each
   one has a specific failure it prevents, named at its own site. */

#define SSH_ARGV_MAX 16

/* ---- URL parsing ---- */

int sg_ssh_is_ssh_url(const char *url)
{
    const char *colon;

    if (strncmp(url, "ssh://", 6) == 0)
        return 1;
    /* Anything with a scheme is somebody else's: this must not swallow
       http:// or https://, whose authority also contains a colon. Checking
       for "://" first is what keeps the scp-like rule below from having to
       reason about schemes at all. */
    if (strstr(url, "://") != NULL)
        return 0;
    colon = strchr(url, ':');
    if (colon == NULL || colon == url)
        return 0;
    /* scp-like is "colon before any slash". A local path such as
       /tmp/repo.git has no colon at all; a relative one like
       sub/dir:weird has its slash first and stays a path, which is git's
       own rule and the reason a Windows-style "C:\..." would be read as a
       host here -- this project supports POSIX only (CLAUDE.md). */
    return memchr(url, '/', (size_t)(colon - url)) == NULL;
}

static char *dup_range(const char *start, size_t len)
{
    char *out = malloc(len + 1);

    if (out == NULL)
        return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

int sg_ssh_parse_url(const char *url, char **user_host_out, char **port_out, char **path_out)
{
    *user_host_out = NULL;
    *port_out = NULL;
    *path_out = NULL;

    if (strncmp(url, "ssh://", 6) == 0) {
        const char *rest = url + 6;
        const char *slash = strchr(rest, '/');
        const char *host_end;
        const char *port_start = NULL;
        const char *at;

        if (slash == NULL || slash == rest) {
            fprintf(stderr, "sg: ssh url '%s' has no repository path\n", url);
            return -1;
        }
        host_end = slash;
        /* A port is the LAST colon in the authority, and only after any
           userinfo -- searching the whole authority left to right would take
           the colon in a "user:pass@host" instead. sg never sends a password
           to ssh, but such a URL must still parse as host+port rather than
           silently becoming a hostname with a colon in it. */
        at = memchr(rest, '@', (size_t)(host_end - rest));
        {
            const char *scan = at != NULL ? at + 1 : rest;
            const char *c = memchr(scan, ':', (size_t)(host_end - scan));

            if (c != NULL)
                port_start = c;
        }
        if (port_start != NULL)
            host_end = port_start;
        *user_host_out = dup_range(rest, (size_t)(host_end - rest));
        if (port_start != NULL)
            *port_out = dup_range(port_start + 1, (size_t)(slash - port_start - 1));
        /* The leading slash is kept, EXCEPT before a '~': measured against
           git, ssh://host/~alice/repo asks for "~alice/repo" so the far
           side's shell expands it, while ssh://host/srv/repo asks for
           "/srv/repo". */
        if (slash[1] == '~')
            *path_out = strdup(slash + 1);
        else
            *path_out = strdup(slash);
    } else {
        const char *colon = strchr(url, ':');

        /* An EMPTY path after the colon is passed through, not rejected:
           measured, git sends git-upload-pack '' and lets the far side
           refuse it. Only a form with no path separator at all (the
           "ssh://host" branch above) fails locally. */
        if (colon == NULL) {
            fprintf(stderr, "sg: ssh url '%s' has no repository path\n", url);
            return -1;
        }
        *user_host_out = dup_range(url, (size_t)(colon - url));
        /* No port in the scp-like form -- "host:22" is a PATH named 22 in
           git, not a port, and inventing a port rule here would silently
           send the wrong request. */
        *path_out = strdup(colon + 1);
    }

    if (*user_host_out == NULL || *path_out == NULL || (*user_host_out)[0] == '\0') {
        fprintf(stderr, "sg: cannot parse ssh url '%s'\n", url);
        free(*user_host_out);
        free(*port_out);
        free(*path_out);
        *user_host_out = *port_out = *path_out = NULL;
        return -1;
    }
    return 0;
}

/* ---- spawning ---- */

/* Builds "<service> '<path>'" the way git does: ONE argument, with the path
   single-quoted because the far side runs it through a shell. An embedded
   single quote is rejected rather than escaped -- a path that needs escaping
   is a path this project has no measured behaviour for, and guessing at
   shell quoting on a string that reaches a remote shell is the kind of
   failure direction that does not get a second chance. */
static char *build_remote_command(const char *service, const char *path)
{
    size_t n;
    char *cmd;

    if (strchr(path, '\'') != NULL) {
        fprintf(stderr, "sg: ssh repository path may not contain a single quote: %s\n", path);
        return NULL;
    }
    n = strlen(service) + strlen(path) + 4; /* space, two quotes, NUL */
    cmd = malloc(n);
    if (cmd == NULL)
        return NULL;
    snprintf(cmd, n, "%s '%s'", service, path);
    return cmd;
}

/* Splits GIT_SSH_COMMAND on whitespace into argv, git-compatibly: measured,
   GIT_SSH_COMMAND="<prog> -vvv" reaches ssh as two argv entries. No quote
   handling -- git hands the string to a shell, sg splits it, and a command
   needing quotes is rejected below rather than mis-split. */
static int push_arg(char **argv, int *argc, char *value)
{
    if (*argc >= SSH_ARGV_MAX - 1)
        return -1;
    argv[(*argc)++] = value;
    return 0;
}

static int build_ssh_argv(char **argv, int *argc, char **owned, int *owned_count,
                          const char *user_host, const char *port, char *remote_cmd)
{
    const char *env = getenv("GIT_SSH_COMMAND");
    char *copy = NULL;

    *argc = 0;
    *owned_count = 0;
    if (env == NULL || env[0] == '\0')
        env = getenv("GIT_SSH");
    if (env != NULL && env[0] != '\0') {
        char *tok, *save = NULL;

        if (strchr(env, '\'') != NULL || strchr(env, '"') != NULL) {
            fprintf(stderr, "sg: quoting in GIT_SSH_COMMAND is not supported: %s\n", env);
            return -1;
        }
        copy = strdup(env);
        if (copy == NULL)
            return -1;
        owned[(*owned_count)++] = copy;
        for (tok = strtok_r(copy, " \t", &save); tok != NULL; tok = strtok_r(NULL, " \t", &save)) {
            if (push_arg(argv, argc, tok) != 0)
                goto too_many;
        }
        if (*argc == 0) {
            fprintf(stderr, "sg: GIT_SSH_COMMAND is empty\n");
            return -1;
        }
    } else if (push_arg(argv, argc, (char *)"ssh") != 0) {
        goto too_many;
    }

    if (port != NULL) {
        if (push_arg(argv, argc, (char *)"-p") != 0 || push_arg(argv, argc, (char *)port) != 0)
            goto too_many;
    }
    if (push_arg(argv, argc, (char *)user_host) != 0 || push_arg(argv, argc, remote_cmd) != 0)
        goto too_many;
    argv[*argc] = NULL;
    return 0;

too_many:
    fprintf(stderr, "sg: ssh command line is too long\n");
    return -1;
}

typedef struct {
    pid_t pid;
    int in_fd;  /* we write here -> child's stdin */
    int out_fd; /* we read here <- child's stdout */
} ssh_proc;

static int ssh_spawn(char **argv, ssh_proc *out)
{
    int to_child[2], from_child[2];
    pid_t pid;

    if (pipe(to_child) != 0)
        return -1;
    if (pipe(from_child) != 0) {
        close(to_child[0]);
        close(to_child[1]);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        fprintf(stderr, "sg: cannot fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* Child. stderr is deliberately left alone so ssh's own diagnostics
           (host key prompts, permission denied) reach the user unchanged --
           this transport has no way to authenticate on its own and the
           message from ssh is the only useful thing it can offer. */
        if (dup2(to_child[0], STDIN_FILENO) < 0 || dup2(from_child[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execvp(argv[0], argv);
        fprintf(stderr, "sg: cannot run '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    out->pid = pid;
    out->in_fd = to_child[1];
    out->out_fd = from_child[0];
    return 0;
}

/* Reaps the child and turns a non-zero exit into our own failure. Called on
   every path, success included: leaving a zombie behind would be invisible
   in a one-shot command and a leak in anything longer-lived. */
static int ssh_reap(ssh_proc *p, const char *what)
{
    int status = 0;

    if (p->in_fd >= 0)
        close(p->in_fd);
    if (p->out_fd >= 0)
        close(p->out_fd);
    p->in_fd = p->out_fd = -1;
    while (waitpid(p->pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "sg: ssh (%s) was killed by signal %d\n", what, WTERMSIG(status));
        return -1;
    }
    fprintf(stderr, "sg: ssh (%s) exited with status %d\n", what, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

/* Writes request while reading the reply, because doing either one to
   completion first can deadlock: the far side starts answering before it has
   consumed the whole request, so a write-everything-then-read loop stalls as
   soon as both pipe buffers fill. */
static int ssh_pump(ssh_proc *p, const void *request, size_t request_len, sg_buf *out)
{
    const unsigned char *wp = request;
    size_t written = 0;
    void (*old_pipe)(int);
    int rc = -1;

    /* A far side that dies mid-conversation makes our write raise SIGPIPE,
       whose default action is to kill sg outright -- with no message, which
       would read to the user as a crash rather than as a remote failure. */
    old_pipe = signal(SIGPIPE, SIG_IGN);

    if (request == NULL || request_len == 0) {
        close(p->in_fd);
        p->in_fd = -1;
        written = request_len;
    }

    for (;;) {
        struct pollfd fds[2];
        int nfds = 0;
        int ridx = -1, widx = -1;

        if (p->out_fd >= 0) {
            fds[nfds].fd = p->out_fd;
            fds[nfds].events = POLLIN;
            ridx = nfds++;
        }
        if (p->in_fd >= 0 && written < request_len) {
            fds[nfds].fd = p->in_fd;
            fds[nfds].events = POLLOUT;
            widx = nfds++;
        }
        if (nfds == 0)
            break;
        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "sg: ssh: poll failed: %s\n", strerror(errno));
            goto out;
        }
        if (widx >= 0 && (fds[widx].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            ssize_t n = write(p->in_fd, wp + written, request_len - written);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "sg: ssh: write failed: %s\n", strerror(errno));
                goto out;
            }
            written += (size_t)n;
            if (written == request_len) {
                /* Half-close: the far side reads to EOF to know the request
                   is complete. Keeping it open would hang both ends. */
                close(p->in_fd);
                p->in_fd = -1;
            }
        }
        if (ridx >= 0 && (fds[ridx].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            unsigned char buf[65536];
            ssize_t n = read(p->out_fd, buf, sizeof(buf));

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "sg: ssh: read failed: %s\n", strerror(errno));
                goto out;
            }
            if (n == 0) {
                close(p->out_fd);
                p->out_fd = -1;
                continue;
            }
            if (sg_buf_append(out, buf, (size_t)n) != 0) {
                fprintf(stderr, "sg: out of memory\n");
                goto out;
            }
        }
    }
    rc = 0;

out:
    signal(SIGPIPE, old_pipe);
    return rc;
}

static int ssh_start(const char *url, const char *service, ssh_proc *proc)
{
    char *user_host = NULL, *port = NULL, *path = NULL, *remote_cmd = NULL;
    char *argv[SSH_ARGV_MAX];
    char *owned[2];
    int argc = 0, owned_count = 0, i;
    int rc = -1;

    if (sg_ssh_parse_url(url, &user_host, &port, &path) != 0)
        return -1;
    remote_cmd = build_remote_command(service, path);
    if (remote_cmd == NULL)
        goto out;
    if (build_ssh_argv(argv, &argc, owned, &owned_count, user_host, port, remote_cmd) != 0)
        goto out;
    if (ssh_spawn(argv, proc) != 0)
        goto out;
    rc = 0;

out:
    for (i = 0; i < owned_count; i++)
        free(owned[i]);
    free(remote_cmd);
    free(user_host);
    free(port);
    free(path);
    return rc;
}

/* How many bytes of buf are a complete pkt-line advertisement, i.e. up to and
   including the first flush. 0 means "not yet". */
static size_t advertisement_len(const unsigned char *data, size_t len)
{
    size_t pos = 0;

    for (;;) {
        sg_pkt_type type;
        const unsigned char *payload;
        size_t payload_len;

        if (sg_pkt_read(data, len, &pos, &type, &payload, &payload_len) != 0)
            return 0; /* incomplete OR malformed -- the caller's parser is
                         the one that gets to decide which, on the whole
                         buffer, so nothing is reported here */
        if (type == SG_PKT_FLUSH)
            return pos;
    }
}

int sg_ssh_advertise(const char *url, const char *service, sg_buf *out)
{
    ssh_proc proc;
    static const char flush[] = "0000";

    memset(out, 0, sizeof(*out));
    if (ssh_start(url, service, &proc) != 0)
        return -1;
    /* A flush and nothing else is the client saying "I want nothing", which
       is how `git ls-remote` ends a connection. Without it upload-pack sees
       EOF where a request should be and reports a hung-up connection -- an
       error message for a successful operation. */
    if (ssh_pump(&proc, flush, sizeof(flush) - 1, out) != 0) {
        ssh_reap(&proc, service);
        sg_buf_free(out);
        return -1;
    }
    if (ssh_reap(&proc, service) != 0) {
        sg_buf_free(out);
        return -1;
    }
    /* Trailing bytes after the advertisement's flush are not ours to keep:
       the parser stops there anyway, and truncating makes that explicit. */
    {
        size_t adv = advertisement_len(out->data, out->len);

        if (adv > 0)
            out->len = adv;
    }
    return 0;
}

int sg_ssh_request(const char *url, const char *service, const void *request, size_t request_len,
                   sg_buf *out)
{
    ssh_proc proc;
    sg_buf raw;
    size_t adv;

    memset(out, 0, sizeof(*out));
    memset(&raw, 0, sizeof(raw));
    if (ssh_start(url, service, &proc) != 0)
        return -1;
    if (ssh_pump(&proc, request, request_len, &raw) != 0) {
        ssh_reap(&proc, service);
        sg_buf_free(&raw);
        return -1;
    }
    if (ssh_reap(&proc, service) != 0) {
        sg_buf_free(&raw);
        return -1;
    }
    /* Drop the advertisement this connection re-sent. Everything after it is
       the answer to `request`, in exactly the bytes the HTTP path would have
       received as a response body -- which is what lets both share every
       parser downstream. */
    adv = advertisement_len(raw.data, raw.len);
    if (adv == 0) {
        fprintf(stderr, "sg: ssh: %s did not send a ref advertisement\n", service);
        sg_buf_free(&raw);
        return -1;
    }
    if (sg_buf_append(out, raw.data + adv, raw.len - adv) != 0) {
        fprintf(stderr, "sg: out of memory\n");
        sg_buf_free(&raw);
        sg_buf_free(out);
        return -1;
    }
    sg_buf_free(&raw);
    return 0;
}

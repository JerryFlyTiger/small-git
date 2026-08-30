#include "sg/http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sg_buf_free(sg_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int sg_buf_append(sg_buf *b, const void *data, size_t len)
{
    if (b->len + len > b->cap) {
        size_t new_cap = (b->cap == 0) ? 4096 : b->cap;
        unsigned char *grown;

        while (new_cap < b->len + len)
            new_cap *= 2;
        grown = realloc(b->data, new_cap);
        if (grown == NULL)
            return -1;
        b->data = grown;
        b->cap = new_cap;
    }
    if (len > 0)
        memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static void ensure_curl_global_init(void)
{
    static int inited = 0;

    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        atexit(curl_global_cleanup);
        inited = 1;
    }
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    sg_buf *buf = userdata;
    size_t n = size * nmemb;

    if (sg_buf_append(buf, ptr, n) != 0)
        return 0; /* short write signals an error to libcurl, aborting the transfer */
    return n;
}

char *sg_url_redact(const char *url)
{
    const char *scheme_end = strstr(url, "://");
    const char *authority_start;
    const char *authority_end;
    const char *at = NULL;
    const char *p;
    size_t prefix_len;
    size_t suffix_len;
    char *out;

    if (scheme_end == NULL) {
        /* No scheme: the scp-like ssh shorthand "[user@]host:path" (Phase
           47), which has real userinfo and reaches the same error messages
           as any other remote. Returning it unchanged -- what this function
           did before ssh existed -- would print a user name that the URL
           form was the only reason to have. The authority here ends at the
           FIRST ':', because everything after it is the path. */
        const char *scp_colon = strchr(url, ':');
        const char *scp_at = NULL;
        const char *q;

        if (scp_colon == NULL)
            return strdup(url);
        for (q = url; q < scp_colon; q++) {
            if (*q == '@')
                scp_at = q;
        }
        if (scp_at == NULL)
            return strdup(url);
        {
            size_t tail = strlen(scp_at);
            char *scp_out = malloc(3 + tail + 1);

            if (scp_out == NULL)
                return NULL;
            memcpy(scp_out, "***", 3);
            memcpy(scp_out + 3, scp_at, tail + 1);
            return scp_out;
        }
    }
    authority_start = scheme_end + 3;
    authority_end = authority_start;
    while (*authority_end != '\0' && *authority_end != '/')
        authority_end++;

    for (p = authority_start; p < authority_end; p++) {
        if (*p == '@')
            at = p; /* the last '@' before the path is where userinfo ends */
    }
    if (at == NULL)
        return strdup(url);

    prefix_len = (size_t)(authority_start - url);
    suffix_len = strlen(at); /* "@host..." onward, '\0' included via the +1 below */

    out = malloc(prefix_len + 3 + suffix_len + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, url, prefix_len);
    memcpy(out + prefix_len, "***", 3);
    memcpy(out + prefix_len + 3, at, suffix_len + 1);
    return out;
}

static void print_error_body(const sg_buf *out)
{
    if (out->len > 0) {
        size_t show = out->len > 2000 ? 2000 : out->len;

        fprintf(stderr, "sg: server response: %.*s\n", (int)show, (const char *)out->data);
    }
}

static int perform_request(CURL *curl, const char *method, const char *url, sg_buf *out)
{
    CURLcode res;
    long http_code = 0;
    char *safe_url = sg_url_redact(url);
    /* Fail closed: if redaction couldn't allocate, printing the raw URL would
       leak any user:password embedded in it. A vaguer message is strictly
       better than a leaked credential. */
    const char *shown_url = safe_url != NULL ? safe_url : "(remote)";

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "sg: %s %s failed: %s\n", method, shown_url, curl_easy_strerror(res));
        free(safe_url);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        fprintf(stderr, "sg: %s %s failed: HTTP %ld\n", method, shown_url, http_code);
        if (http_code == 401 || http_code == 403) {
            fprintf(stderr, "sg: authentication failed. Check that ~/.netrc has credentials "
                            "for this host, or set SG_USERNAME / SG_PASSWORD and try again\n");
        }
        print_error_body(out);
        free(safe_url);
        return -1;
    }
    free(safe_url);
    return 0;
}

static CURL *make_curl(const char *url, sg_buf *out)
{
    CURL *curl = curl_easy_init();

    if (curl == NULL)
        return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "git/2.0 (small-git)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    /* Pin the allowed schemes explicitly rather than trusting whichever
       defaults the linked libcurl happens to ship: a malicious server must
       not be able to redirect us to file://, scp://, etc. */
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    /* leave CURLOPT_SSL_VERIFYPEER/VERIFYHOST at libcurl's secure defaults */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);

    /* CURL_NETRC_OPTIONAL: use ~/.netrc credentials for a matching host if
       present, but don't require one -- same as real git's own default. */
    curl_easy_setopt(curl, CURLOPT_NETRC, (long)CURL_NETRC_OPTIONAL);
    {
        const char *user = getenv("SG_USERNAME");
        const char *pass = getenv("SG_PASSWORD");

        if (user != NULL)
            curl_easy_setopt(curl, CURLOPT_USERNAME, user);
        if (pass != NULL)
            curl_easy_setopt(curl, CURLOPT_PASSWORD, pass);
    }

    return curl;
}

int sg_http_get(const char *url, const char *accept_header, sg_buf *out)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    char accept_line[256];
    int rc;

    ensure_curl_global_init();
    memset(out, 0, sizeof(*out));

    curl = make_curl(url, out);
    if (curl == NULL) {
        char *safe_url = sg_url_redact(url);

        fprintf(stderr, "sg: failed to initialize HTTP client for %s\n",
               safe_url != NULL ? safe_url : "(remote)");
        free(safe_url);
        return -1;
    }

    if (accept_header != NULL) {
        snprintf(accept_line, sizeof(accept_line), "Accept: %s", accept_header);
        headers = curl_slist_append(headers, accept_line);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    rc = perform_request(curl, "GET", url, out);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != 0)
        sg_buf_free(out);
    return rc;
}

int sg_http_post(const char *url, const char *content_type, const char *accept_header,
                 const void *body, size_t body_len, sg_buf *out)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    char content_type_line[256];
    char accept_line[256];
    int rc;

    ensure_curl_global_init();
    memset(out, 0, sizeof(*out));

    curl = make_curl(url, out);
    if (curl == NULL) {
        char *safe_url = sg_url_redact(url);

        fprintf(stderr, "sg: failed to initialize HTTP client for %s\n",
               safe_url != NULL ? safe_url : "(remote)");
        free(safe_url);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);

    if (content_type != NULL) {
        snprintf(content_type_line, sizeof(content_type_line), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, content_type_line);
    }
    if (accept_header != NULL) {
        snprintf(accept_line, sizeof(accept_line), "Accept: %s", accept_header);
        headers = curl_slist_append(headers, accept_line);
    }
    if (headers != NULL)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    rc = perform_request(curl, "POST", url, out);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != 0)
        sg_buf_free(out);
    return rc;
}

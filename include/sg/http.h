#ifndef SG_HTTP_H
#define SG_HTTP_H

#include <stddef.h>

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} sg_buf;

void sg_buf_free(sg_buf *b);

/* Appends data/len to b, growing b->data (realloc) as needed. Shared by the
   libcurl write callback here and by the sideband packfile accumulator in
   net/transport.c. Returns 0 on success, -1 on allocation failure. */
int sg_buf_append(sg_buf *b, const void *data, size_t len);

/* HTTP GET/POST via libcurl's easy interface. On success (HTTP 200) returns 0
   with the response body in *out. On any transport error or non-200 status,
   returns -1 after printing the URL, status code (if any), and a snippet of
   the server's response body to stderr. accept_header may be NULL to omit
   the Accept header. TLS certificate verification is never disabled. */
int sg_http_get(const char *url, const char *accept_header, sg_buf *out);
int sg_http_post(const char *url, const char *content_type, const char *accept_header,
                 const void *body, size_t body_len, sg_buf *out);

/* Both sg_http_get/sg_http_post read ~/.netrc (CURL_NETRC_OPTIONAL) and, if
   set, the SG_USERNAME / SG_PASSWORD environment variables for HTTP
   authentication -- the same mechanisms real git uses. Credentials are never
   read from the URL's own userinfo by this layer beyond what libcurl already
   does with it; see sg_url_redact below for keeping any that *are* embedded
   in a URL out of output. */

/* Returns a malloc'd copy of url with any "user:pass@" (or "user@") userinfo
   in its authority component replaced by "***@", so it's safe to print.
   Returns a plain copy, unchanged, for a URL with no such userinfo. A URL
   with no "scheme://" is treated as the scp-like ssh shorthand
   "[user@]host:path" and gets the same treatment up to its first ':'
   (Phase 47) -- before ssh existed this function returned such a string
   untouched, which would now print a user name. Caller frees. Every place that
   prints a URL (this file's own error messages included) must go through
   this first -- leaking credentials into stdout/stderr/logs is a real
   security issue, not just cosmetic. */
char *sg_url_redact(const char *url);

#endif

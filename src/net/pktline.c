#include "sg/pktline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SG_PKT_MAX_PAYLOAD 65516 /* 0xfff0 - 4 */

static int hex_nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') /* git only emits lowercase, but accept both on read */
        return c - 'A' + 10;
    return -1;
}

int sg_pkt_read(const unsigned char *buf, size_t len, size_t *pos, sg_pkt_type *type,
                const unsigned char **payload, size_t *payload_len)
{
    size_t p = *pos;
    unsigned val = 0;
    size_t i;

    if (p > len || len - p < 4)
        return -1;

    for (i = 0; i < 4; i++) {
        int nibble = hex_nibble(buf[p + i]);

        if (nibble < 0)
            return -1;
        val = (val << 4) | (unsigned)nibble;
    }

    if (val == 0) {
        *type = SG_PKT_FLUSH;
        *payload = NULL;
        *payload_len = 0;
        *pos = p + 4;
        return 0;
    }
    if (val == 1) {
        *type = SG_PKT_DELIM;
        *payload = NULL;
        *payload_len = 0;
        *pos = p + 4;
        return 0;
    }
    /* 0002/0003 have no valid meaning (payload would be negative-length);
       the format caps a single packet at 0xfff0 total bytes */
    if (val < 4 || val > 0xfff0)
        return -1;
    /* declared length is remote-controlled -- never trust it past what's
       actually left in the buffer */
    if (len - p < (size_t)val)
        return -1;

    *type = SG_PKT_DATA;
    *payload = buf + p + 4;
    *payload_len = (size_t)val - 4;
    *pos = p + (size_t)val;
    return 0;
}

static int grow(unsigned char **buf, size_t *cap, size_t need)
{
    size_t new_cap;
    unsigned char *grown;

    if (need <= *cap)
        return 0;
    new_cap = (*cap == 0) ? 256 : *cap;
    while (new_cap < need)
        new_cap *= 2;
    grown = realloc(*buf, new_cap);
    if (grown == NULL)
        return -1;
    *buf = grown;
    *cap = new_cap;
    return 0;
}

int sg_pkt_append(unsigned char **buf, size_t *len, size_t *cap, const void *payload,
                  size_t payload_len)
{
    char hdr[5];
    size_t total;

    if (payload_len > SG_PKT_MAX_PAYLOAD)
        return -1;
    total = payload_len + 4;
    if (grow(buf, cap, *len + total) != 0)
        return -1;

    snprintf(hdr, sizeof(hdr), "%04zx", total);
    memcpy(*buf + *len, hdr, 4);
    if (payload_len > 0)
        memcpy(*buf + *len + 4, payload, payload_len);
    *len += total;
    return 0;
}

int sg_pkt_append_str(unsigned char **buf, size_t *len, size_t *cap, const char *s)
{
    return sg_pkt_append(buf, len, cap, s, strlen(s));
}

int sg_pkt_append_flush(unsigned char **buf, size_t *len, size_t *cap)
{
    if (grow(buf, cap, *len + 4) != 0)
        return -1;
    memcpy(*buf + *len, "0000", 4);
    *len += 4;
    return 0;
}

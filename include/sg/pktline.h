#ifndef SG_PKTLINE_H
#define SG_PKTLINE_H

#include <stddef.h>

typedef enum {
    SG_PKT_DATA,
    SG_PKT_FLUSH,
    SG_PKT_DELIM,
} sg_pkt_type;

/* Reads one pkt-line starting at buf[*pos]. On success (0), *type is set,
   *pos advances past the packet, and for SG_PKT_DATA, *payload and
   *payload_len point into buf (no copy) -- buf must outlive any use of
   *payload. The
   4-hex-digit length prefix counts itself, so a DATA packet's payload is
   (declared length - 4) bytes; 0000/0001 are the flush/delim markers with no
   payload. Returns -1 (without advancing *pos) on any malformed input: a
   non-hex length digit, length 0002/0003, length > fff0, or a declared
   length that would read past buf+len -- callers must treat all of these as
   "this data is not trustworthy", since buf is typically straight off the
   network. */
int sg_pkt_read(const unsigned char *buf, size_t len, size_t *pos, sg_pkt_type *type,
                const unsigned char **payload, size_t *payload_len);

/* Appends one DATA pkt-line (4-hex-digit length prefix + payload) to the
   dynamically-grown buffer given by buf/len/cap (realloc'd as needed; *buf
   may start NULL/0/0). payload_len must not exceed 65516 (0xfff0 - 4), the
   largest payload a single pkt-line can carry. Returns 0 on success, -1 on
   allocation failure or an oversized payload. */
int sg_pkt_append(unsigned char **buf, size_t *len, size_t *cap, const void *payload,
                  size_t payload_len);

/* Same as sg_pkt_append, with payload = s, payload_len = strlen(s); s must
   itself include any trailing '\n' the caller wants in the packet. */
int sg_pkt_append_str(unsigned char **buf, size_t *len, size_t *cap, const char *s);

/* Appends a flush-pkt ("0000", no payload). */
int sg_pkt_append_flush(unsigned char **buf, size_t *len, size_t *cap);

#endif

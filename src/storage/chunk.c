#include "sg/chunk.h"

#include "sg/loose.h"
#include "sg/object.h"
#include "sg/objstore.h"
#include "sg/quote.h"
#include "sg/refs.h"
#include "sg/repo.h"
#include "sg/workdir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* Precomputed with a fixed seed (splitmix64(0x736D616C6C5F676974)) -- never
   generate this at runtime with rand()/a PRNG, since differing PRNGs across
   platforms/libc would make the same file chunk differently on different
   machines and break deduplication. */
static const uint64_t SG_GEAR[256] = {
    0x538D3183B9533123ULL, 0x5DDBC4A43A78C056ULL, 0x788BA9BFEB5737EAULL, 0x8127E72FB34F4DBFULL,
    0xF1BD6245C1402467ULL, 0x6C52C90CE8C20A63ULL, 0xE041769E6FE00FC7ULL, 0x8A6577B3697AF0E7ULL,
    0x6464418A20C90F84ULL, 0xBDB600B7B6B5A8D5ULL, 0x31921BAC0A11CC79ULL, 0xCBD4F07641348835ULL,
    0x9501ECFC4D133FAAULL, 0xD432AE2E7489B6D0ULL, 0xCF45E4928E3AEC0CULL, 0xBE94910200C0DAECULL,
    0x98BFF767CB91F455ULL, 0x2AD61CC09954F8B2ULL, 0xCF1FCA0F63D3A679ULL, 0x2EDD0703310CE397ULL,
    0x550247D2E33750DDULL, 0x6ABAA68CAE936D01ULL, 0x0FD5A4DBA06F75F2ULL, 0x4DD50F498721A3AEULL,
    0xABFB7421233ED4A2ULL, 0x13D249BFEFD4D071ULL, 0x85D79C79E144947EULL, 0xAF1EF9F48477F1FCULL,
    0x5FEA15FDCF39BADBULL, 0x7636D59687B9B75EULL, 0x1CF059C4F2526E11ULL, 0x2A40DD0DD278E87FULL,
    0xF81A5353BBBD76CCULL, 0xE2C9105663B4B3C2ULL, 0x50F90A815773F824ULL, 0xDC904A68333E58B2ULL,
    0xA643813637AC650CULL, 0x645B735C6551E550ULL, 0x605B6B51F787FC96ULL, 0xC2565EF2DA47F611ULL,
    0xC794ADC951C4A364ULL, 0x918F0292D2966FB9ULL, 0x5A8AF9C521FBC19FULL, 0xC137D057B3FB6C92ULL,
    0x6F02EACE20B609F2ULL, 0xBA657D4FCEA91C47ULL, 0xDAD203D0B1F96144ULL, 0x158F7B8AD98FA3D5ULL,
    0xA6E4C4BA21B447C2ULL, 0x10F14D5134E7EB5CULL, 0xC5C5956A50E21E06ULL, 0x345D3A66171FC69CULL,
    0x712D8FE73437B0A3ULL, 0x5E9B7707C498ED8AULL, 0x91549154A9C00BC2ULL, 0x3FC3C7839054F4A5ULL,
    0x385F31A63B305984ULL, 0x70379B3E9AD41A02ULL, 0x12268B02C4F94FFEULL, 0x20DDBB8381FE504AULL,
    0x51335A3C204262EAULL, 0x5A2E6F6267F9B443ULL, 0x63F2E5554A4F4496ULL, 0xBD3C338EC6C8CC7EULL,
    0x397D5B6B1D7E58B8ULL, 0x0AB0BF76E2B045CDULL, 0x1C839B338509DEB9ULL, 0x91124CA4308D0707ULL,
    0xB296084A26B5E799ULL, 0x32B35B32EC3CD81EULL, 0xB061AFCBBE1552C8ULL, 0x360A6D1245231531ULL,
    0x053E0336A1774959ULL, 0x7D0500D074098242ULL, 0x12A66063E352A2DAULL, 0x9AE2355404C483D2ULL,
    0x08CA4C90D94FCBE0ULL, 0x8B4C478233AAF10AULL, 0x08D7043383F4EF05ULL, 0x12D8DCB720162E78ULL,
    0x45860B1337CFC433ULL, 0xE9C9FFD3ECDC07CFULL, 0x1480C2E33BB21DA4ULL, 0x9669634146FF6740ULL,
    0xCB8894B54D2E39DBULL, 0xAD1EF15E67EEB566ULL, 0xD7AE699BF704D193ULL, 0xB5FB7D11289924E2ULL,
    0xAC5C1C41B292B1B3ULL, 0xE9B60BB344715866ULL, 0x25010E64F5DB20C5ULL, 0x57F1EEB9B273BA9CULL,
    0x6D3792A5452A2DECULL, 0x0D03C112F1895665ULL, 0x8022758A1913ECC8ULL, 0x2834C47E66F3522BULL,
    0xD7551C9486B028A7ULL, 0x012CD4506848493EULL, 0x3AF67C989C053B18ULL, 0xC8BC751BD8D39AEBULL,
    0x389FD8C2D2500B31ULL, 0x6B460B410E924897ULL, 0x44DF63E3D9B3C601ULL, 0xFDE999C6427B2616ULL,
    0xE220E94AC73509D3ULL, 0xA6A63D9FDA3C8A0FULL, 0xDD83CF15939CA0DEULL, 0x38C88EB614EF8E94ULL,
    0x1207E2F067152FDEULL, 0xA6E3A5D9B336C33EULL, 0xABB048F56B0CD105ULL, 0x687168C6484B0F30ULL,
    0x4C9603F3DDBF1923ULL, 0x15A6DFDEE1BD242CULL, 0x219E6EC7CC3996DDULL, 0xAB43C821ACC3E14DULL,
    0xA20C93EF0E56860FULL, 0xC800F3E53E3DE7EFULL, 0x08B4BFB8E98DB252ULL, 0xF3522BC02EFDD443ULL,
    0xA9E4BF61A8BB4AA1ULL, 0x22DC956A80A95E6CULL, 0xE9020D6102C7D9B8ULL, 0x07DA905262586DE4ULL,
    0xFFE3F0FDB63412E3ULL, 0x6F6A4F01C3697F51ULL, 0x6204CB4420BD0889ULL, 0x1C32CD1900DC043EULL,
    0xECBC94480A20FB45ULL, 0x43CA786706C5395FULL, 0xA06CBD718B2092DCULL, 0x32985BE33A520016ULL,
    0xBBCCF5A8FDDBA17BULL, 0x66E27BB4616D1749ULL, 0x98BFC43192133C89ULL, 0xDDD8B704F12B6B6BULL,
    0x71D9F2D324FED844ULL, 0x8F31230CCF5374FFULL, 0xF240A552DE775A5AULL, 0x4A90887DD5E3E570ULL,
    0xD611F941A0096AEFULL, 0x49BB4DF3D6C37420ULL, 0x98AEE61664367975ULL, 0x305443A07332C7EAULL,
    0xA406C0BA4BA8909CULL, 0x374FA0081B7B092FULL, 0xC1ED7A038F7E6BDEULL, 0xB172A906ACD53BB3ULL,
    0x1BCA9C63E720367EULL, 0xDDE61632C216B876ULL, 0x9F584A0BEED56E31ULL, 0x06FB1E2157961336ULL,
    0x66144E3B9E6F72CBULL, 0xB4B30C943E0F9F88ULL, 0x0DB844B1730EF6CFULL, 0x60C3D7CB07231F5BULL,
    0x15314659D64E6600ULL, 0xBFAA144ED336E5CFULL, 0x6FECC81634D7E613ULL, 0xDA0CF1D66D6D8B21ULL,
    0xF12BC7D26077A2A5ULL, 0x2AFEBDAF4772AA3EULL, 0x59240170368B8546ULL, 0x1AD515ADE390C647ULL,
    0xCDE2873C54DC7BF0ULL, 0xBE6522BFB701299BULL, 0x96EF564F61532C27ULL, 0xC2CE5D8DF5102210ULL,
    0x05E149754DEDBF2DULL, 0x3097BEA67458501DULL, 0xC41F9AEF46352C8FULL, 0xA7A44D6F9BDD0285ULL,
    0x027C833D28346DCEULL, 0xA2D8E36822E14199ULL, 0xC78B95D4B486422DULL, 0xD6327DB2AA86B145ULL,
    0x382BDA5AFFB99B4EULL, 0x11B38799301AA94CULL, 0x2886F378E7F739CEULL, 0x649EF0ED239DBCFFULL,
    0x74B4D351E31363E4ULL, 0xA30B21011C0C9490ULL, 0x9C503BEC0E074B94ULL, 0xA8A2863034436216ULL,
    0x807BE80CBC2C962BULL, 0x8C5BF6017A5AAA06ULL, 0xAA73FB6827E0B1E0ULL, 0xD1AB153BDB60C24AULL,
    0xC587965D0A75921FULL, 0xD3BE7B08FCBB3DA9ULL, 0x17C657921F51346AULL, 0x2E6A20F1697223AAULL,
    0x100375270C26A22BULL, 0x146D7060AC6024E9ULL, 0xDA0DAE36C1D2C617ULL, 0x37740825E5E42A6AULL,
    0xB7F62FD11E84CAE0ULL, 0x8F6E6DF424FCAAA0ULL, 0xED80210346F486C5ULL, 0xB9168210B77ECDD8ULL,
    0xCE6429FE4D44F96FULL, 0x002D7C2692FD1905ULL, 0xD24CCD7988B7B595ULL, 0xE7B6077C772E16EFULL,
    0x586C30D118237E14ULL, 0xE1C84051DA9FCB55ULL, 0x068844B4841D902EULL, 0x2935DCF9AC923475ULL,
    0x392CA9DF3F17B0FFULL, 0x9A151AD230EBC330ULL, 0xFCC37E84A5809DA7ULL, 0xD680917AA33AA3EEULL,
    0x4B0C29BF909A39FFULL, 0xA3414376A4DF1B16ULL, 0xD134BFFA08EFC084ULL, 0x08E97FF8B7367634ULL,
    0x2E1AF4B88110E82FULL, 0x6A1D2168BB7646C5ULL, 0x3A12B92E0B589FA6ULL, 0x21B9431D565C107EULL,
    0x43C92CBF590489EFULL, 0x2F6D3486398063C0ULL, 0xED2DCDC6989344D1ULL, 0x4AF5785628BB9045ULL,
    0x3AE9B355CCFE89DFULL, 0xD6D9D4F3FDECBEC7ULL, 0xDA65D951A3A6BE57ULL, 0xDF85B2F1A71F6787ULL,
    0x8476167BE070D990ULL, 0xB6DF7F6E453C2634ULL, 0xB396075A49522F6BULL, 0xBD40C58146D91C1DULL,
    0x95E636A5A284896DULL, 0x3F1F1C108F4899ACULL, 0xC5E9E417A8E6C0E1ULL, 0xBDADBADA733809C6ULL,
    0xA63ECF5081732392ULL, 0xECA61A25D28D4ADBULL, 0xC21FE1A4DCB7A3FDULL, 0x4E1D42E8D3F5F272ULL,
    0x5443DA5BA981348BULL, 0x034891B592431268ULL, 0x5CACAF473A206BEAULL, 0x2F1FC4CE77EEA703ULL,
    0x8BE12A50F28E52D5ULL, 0xF8835504A901D0AAULL, 0x4D1CFE65ACA2ABBBULL, 0x832032E2D1166891ULL,
    0x1F68157E10285E0FULL, 0xB3AB218434378B1EULL, 0xF8D17C0F20EFFBF8ULL, 0x4B50B79E00063318ULL,
    0x80D79D61EDFA6141ULL, 0x9D6EDD50DDDFAB13ULL, 0x74A60F647B8B3D0CULL, 0x66FC8C363AE62A28ULL,
};

static int push_chunk(size_t **offsets, size_t **lengths, size_t *cap, size_t *count,
                      size_t offset, size_t length)
{
    if (*count == *cap) {
        size_t new_cap = *cap * 2;
        size_t *new_offsets = realloc(*offsets, new_cap * sizeof(*new_offsets));
        size_t *new_lengths;

        if (new_offsets == NULL)
            return -1;
        *offsets = new_offsets;

        new_lengths = realloc(*lengths, new_cap * sizeof(*new_lengths));
        if (new_lengths == NULL)
            return -1;
        *lengths = new_lengths;

        *cap = new_cap;
    }

    (*offsets)[*count] = offset;
    (*lengths)[*count] = length;
    (*count)++;
    return 0;
}

int sg_chunk_split(const unsigned char *data, size_t len, size_t **offsets_out,
                   size_t **lengths_out, size_t *count_out)
{
    size_t cap = 16;
    size_t count = 0;
    size_t chunk_start = 0;
    size_t *offsets;
    size_t *lengths;
    uint64_t h = 0;
    size_t i;

    if (len == 0) {
        *offsets_out = NULL;
        *lengths_out = NULL;
        *count_out = 0;
        return 0;
    }

    offsets = malloc(cap * sizeof(*offsets));
    lengths = malloc(cap * sizeof(*lengths));
    if (offsets == NULL || lengths == NULL) {
        free(offsets);
        free(lengths);
        return -1;
    }

    for (i = 0; i < len; i++) {
        size_t chunk_len;

        h = (h << 1) + SG_GEAR[data[i]];
        chunk_len = i - chunk_start + 1;

        if (chunk_len >= SG_CHUNK_MIN_SIZE && (h & SG_CHUNK_MASK) == 0) {
            if (push_chunk(&offsets, &lengths, &cap, &count, chunk_start, chunk_len) != 0) {
                free(offsets);
                free(lengths);
                return -1;
            }
            chunk_start = i + 1;
        } else if (chunk_len >= SG_CHUNK_MAX_SIZE) {
            if (push_chunk(&offsets, &lengths, &cap, &count, chunk_start, chunk_len) != 0) {
                free(offsets);
                free(lengths);
                return -1;
            }
            chunk_start = i + 1;
        }
    }

    if (chunk_start < len) {
        if (push_chunk(&offsets, &lengths, &cap, &count, chunk_start, len - chunk_start) != 0) {
            free(offsets);
            free(lengths);
            return -1;
        }
    }

    *offsets_out = offsets;
    *lengths_out = lengths;
    *count_out = count;
    return 0;
}

static int expect_prefix(const unsigned char **pp, const unsigned char *end, const char *prefix)
{
    size_t prefix_len = strlen(prefix);

    if ((size_t)(end - *pp) < prefix_len || memcmp(*pp, prefix, prefix_len) != 0)
        return 0;
    *pp += prefix_len;
    return 1;
}

static int parse_decimal_line(const unsigned char **pp, const unsigned char *end, size_t *out_val)
{
    const unsigned char *p = *pp;
    size_t val = 0;
    int digits = 0;

    while (p < end && *p >= '0' && *p <= '9') {
        size_t d = (size_t)(*p - '0');

        if (val > (SIZE_MAX - d) / 10)
            return 0;
        val = val * 10 + d;
        digits++;
        p++;
    }
    if (digits == 0 || p >= end || *p != '\n')
        return 0;
    p++;

    *out_val = val;
    *pp = p;
    return 1;
}

static int parse_hex40_line(const unsigned char **pp, const unsigned char *end,
                            unsigned char out[SG_SHA1_RAW_LEN])
{
    const unsigned char *p = *pp;
    char hexbuf[SG_SHA1_HEX_LEN + 1];
    size_t i;

    if ((size_t)(end - p) < SG_SHA1_HEX_LEN + 1)
        return 0;
    for (i = 0; i < SG_SHA1_HEX_LEN; i++) {
        unsigned char c = p[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return 0;
    }
    if (p[SG_SHA1_HEX_LEN] != '\n')
        return 0;

    memcpy(hexbuf, p, SG_SHA1_HEX_LEN);
    hexbuf[SG_SHA1_HEX_LEN] = '\0';
    if (sg_hex_to_sha1(hexbuf, out) != 0)
        return 0;

    *pp = p + SG_SHA1_HEX_LEN + 1;
    return 1;
}

int sg_chunk_pointer_parse(const unsigned char *content, size_t len, sg_chunk_pointer *out)
{
    const unsigned char *p = content;
    const unsigned char *end = content + len;
    size_t magic_len = strlen(SG_CHUNK_MAGIC);
    size_t size_val;
    size_t chunks_val;
    unsigned char sha1_raw[SG_SHA1_RAW_LEN];
    unsigned char (*chunk_ids)[SG_SHA1_RAW_LEN];
    size_t i;

    if ((size_t)(end - p) < magic_len || memcmp(p, SG_CHUNK_MAGIC, magic_len) != 0)
        return 0;
    p += magic_len;

    if (!expect_prefix(&p, end, "size ") || !parse_decimal_line(&p, end, &size_val))
        return 0;
    if (!expect_prefix(&p, end, "sha1 ") || !parse_hex40_line(&p, end, sha1_raw))
        return 0;
    if (!expect_prefix(&p, end, "chunks ") || !parse_decimal_line(&p, end, &chunks_val))
        return 0;

    if (chunks_val > SIZE_MAX / sizeof(*chunk_ids))
        return 0;

    /* Guards against more than an integer-overflow-safe malloc size: a
       hostile/corrupt blob could still declare an astronomically large
       "chunks" value (e.g. "chunks 900000000000") while the buffer actually
       has only a handful of real hex lines left, which would pass the check
       above and still trigger one huge transient malloc before the per-line
       parse loop below fails partway through anyway. Each chunk line is
       exactly SG_SHA1_HEX_LEN hex chars plus a newline, so the remaining
       input can never hold more than (end - p) / (SG_SHA1_HEX_LEN + 1) of
       them -- reject up front if the declared count exceeds that. */
    if (chunks_val > (size_t)(end - p) / (SG_SHA1_HEX_LEN + 1))
        return 0;

    if (chunks_val == 0) {
        chunk_ids = NULL;
    } else {
        chunk_ids = malloc(chunks_val * sizeof(*chunk_ids));
        if (chunk_ids == NULL)
            return 0;
    }

    for (i = 0; i < chunks_val; i++) {
        if (!parse_hex40_line(&p, end, chunk_ids[i])) {
            free(chunk_ids);
            return 0;
        }
    }

    if (p != end) {
        free(chunk_ids);
        return 0;
    }

    out->original_size = size_val;
    memcpy(out->original_sha1, sha1_raw, SG_SHA1_RAW_LEN);
    out->chunk_ids = chunk_ids;
    out->chunk_count = chunks_val;
    return 1;
}

int sg_chunk_pointer_format(const sg_chunk_pointer *p, unsigned char **out_content,
                            size_t *out_len)
{
    char header[256];
    char sha1_hex[SG_SHA1_HEX_LEN + 1];
    int header_len;
    size_t total_len;
    unsigned char *buf;
    size_t offset;
    size_t i;

    sg_sha1_to_hex(p->original_sha1, sha1_hex);
    header_len = snprintf(header, sizeof(header), "%ssize %zu\nsha1 %s\nchunks %zu\n",
                          SG_CHUNK_MAGIC, p->original_size, sha1_hex, p->chunk_count);
    if (header_len < 0 || (size_t)header_len >= sizeof(header))
        return -1;

    if (p->chunk_count > (SIZE_MAX - (size_t)header_len) / (SG_SHA1_HEX_LEN + 1))
        return -1;

    total_len = (size_t)header_len + p->chunk_count * (SG_SHA1_HEX_LEN + 1);
    buf = malloc(total_len);
    if (buf == NULL)
        return -1;

    memcpy(buf, header, (size_t)header_len);
    offset = (size_t)header_len;
    for (i = 0; i < p->chunk_count; i++) {
        char hexline[SG_SHA1_HEX_LEN + 2];

        sg_sha1_to_hex(p->chunk_ids[i], hexline);
        hexline[SG_SHA1_HEX_LEN] = '\n';
        memcpy(buf + offset, hexline, SG_SHA1_HEX_LEN + 1);
        offset += SG_SHA1_HEX_LEN + 1;
    }

    *out_content = buf;
    *out_len = total_len;
    return 0;
}

void sg_chunk_pointer_free(sg_chunk_pointer *p)
{
    if (p == NULL)
        return;
    free(p->chunk_ids);
    p->chunk_ids = NULL;
    p->chunk_count = 0;
    p->original_size = 0;
}

/* Reads back every chunk just written and checks that they concatenate to
   exactly reproduce content/len -- this is the safety net for the write path
   itself (compression/decompression, loose object storage), not merely a
   check of sg_chunk_split's in-memory bookkeeping. Returns 1 if the chunks
   reproduce content exactly, 0 if a chunk couldn't be read or the
   reassembled bytes differ (never treated as a hard error by the caller). */
static int chunks_round_trip_ok(const char *git_dir, unsigned char (*chunk_ids)[SG_SHA1_RAW_LEN],
                                size_t count, const unsigned char *content, size_t len)
{
    unsigned char *rebuilt = NULL;
    size_t rebuilt_len = 0;
    int ok = 1;
    size_t i;

    if (len > 0) {
        rebuilt = malloc(len);
        if (rebuilt == NULL)
            return 0;
    }

    for (i = 0; ok && i < count; i++) {
        sg_obj_type type;
        unsigned char *buf = NULL;
        size_t buf_len = 0;

        if (sg_loose_read(git_dir, chunk_ids[i], &type, &buf, &buf_len) != 0) {
            ok = 0;
            break;
        }
        if (buf_len > len - rebuilt_len) {
            free(buf);
            ok = 0;
            break;
        }
        memcpy(rebuilt + rebuilt_len, buf, buf_len);
        rebuilt_len += buf_len;
        free(buf);
    }

    if (ok && (rebuilt_len != len || (len > 0 && memcmp(rebuilt, content, len) != 0)))
        ok = 0;

    free(rebuilt);
    return ok;
}

/* ---- refs/sg/chunks keep-alive tree: see SG_CHUNK_KEEPALIVE_REF's doc
   comment for why this exists. Every chunk id ever written by
   sg_chunk_store_blob ends up as an entry (mode 100644, name = the chunk's
   own 40-hex id) in the tree this ref's commit points at, so `git gc` always
   sees them as reachable. Rebuilt from scratch on every call -- O(total kept
   chunk count) -- rather than incrementally patched on disk; deliberately
   not optimized further in this phase (see the phase 6b spec's own note:
   correctness first, this would only matter at chunk counts in the tens of
   thousands). ---- */

/* Reads a keep-alive-shaped commit (commit -> tree, each tree entry's sha1
   one chunk id -- see keep_alive_add) and flattens its entries into a raw
   id array. Shared by keep_alive_read_existing (this repo's own current
   SG_CHUNK_KEEPALIVE_REF) and sg_chunk_keepalive_merge_commit (an arbitrary
   keep-alive commit, e.g. one just fetched from a remote). Returns -1 if
   commit_id's commit/tree can't be read or parsed. */
static int keep_alive_read_commit(const char *git_dir, const unsigned char commit_id[SG_SHA1_RAW_LEN],
                                  unsigned char (**ids_out)[SG_SHA1_RAW_LEN], size_t *count_out)
{
    sg_obj_type type;
    unsigned char *content = NULL;
    size_t content_len = 0;
    sg_commit commit;
    sg_tree tree;
    unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t i;

    *ids_out = NULL;
    *count_out = 0;

    if (sg_object_read(git_dir, commit_id, &type, &content, &content_len) != 0 || type != SG_OBJ_COMMIT)
        return -1;
    if (sg_commit_parse(content, content_len, &commit) != 0) {
        free(content);
        return -1;
    }
    free(content);
    content = NULL;

    if (sg_object_read(git_dir, commit.tree, &type, &content, &content_len) != 0 || type != SG_OBJ_TREE) {
        sg_commit_free(&commit);
        return -1;
    }
    sg_commit_free(&commit);

    if (sg_tree_parse(content, content_len, &tree) != 0) {
        free(content);
        return -1;
    }
    free(content);

    if (tree.count > 0) {
        ids = malloc(tree.count * sizeof(*ids));
        if (ids == NULL) {
            sg_tree_free(&tree);
            return -1;
        }
    }
    for (i = 0; i < tree.count; i++)
        memcpy(ids[i], tree.entries[i].sha1, SG_SHA1_RAW_LEN);

    /* sg_tree_free zeroes tree.count as part of freeing tree.entries, so the
       count must be captured before the free, not after -- reading it after
       would silently report zero existing chunks every time, defeating the
       whole point of keeping previously-added chunks alive. */
    *count_out = tree.count;
    sg_tree_free(&tree);

    *ids_out = ids;
    return 0;
}

/* Reads SG_CHUNK_KEEPALIVE_REF -> commit -> tree via keep_alive_read_commit.
   *count_out is set to 0 (not an error) if the ref doesn't exist yet -- the
   ordinary state before the first chunk has ever been written. Returns -1
   only if the ref exists but the commit/tree it names can't be read or
   parsed. */
static int keep_alive_read_existing(const char *git_dir, unsigned char (**ids_out)[SG_SHA1_RAW_LEN],
                                    size_t *count_out)
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];

    *ids_out = NULL;
    *count_out = 0;

    if (sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, commit_id) != 0)
        return 0; /* no keep-alive ref yet: nothing kept alive so far */

    return keep_alive_read_commit(git_dir, commit_id, ids_out, count_out);
}

/* ---- sg-chunks.lock: guards keep_alive_add's read-modify-write critical
   section (read the existing keep-alive tree, merge in new ids, write a new
   tree/commit, repoint the ref) against two concurrent callers racing. Two
   `sg add`/`sg commit` invocations on different large files (or one of those
   racing against `sg fetch`'s/the push-time merge's own call into
   sg_chunk_keepalive_merge_commit, which funnels through this same
   function) can both read the same old snapshot of the tree; whichever
   writes last simply clobbers the other's newly-added chunk ids out of the
   tree, silently dropping them from keep-alive protection -- a `git gc`
   later collects them right out from under their pointer blob, recreating
   the exact durability bug this ref exists to prevent, just via a race
   instead of a missing ref. A plain lock file (not flock()/fcntl() advisory
   locking) is deliberately used here: it needs no extra per-platform
   plumbing, and every acquire/release in this codebase is short-lived and
   funnels through these two helpers, so a stale lock is only ever left
   behind by a killed/crashed process, not by ordinary contention. ---- */

#define SG_CHUNK_LOCK_FILE "sg-chunks.lock"
/* 40 attempts * 100ms = ~4s total before giving up -- long enough to ride
   out ordinary contention between a couple of concurrent commands, short
   enough that a genuinely stuck/leaked lock (e.g. a killed `sg` process)
   fails loud in a few seconds rather than hanging every future
   chunk-storing command indefinitely. */
#define SG_CHUNK_LOCK_MAX_ATTEMPTS 40
#define SG_CHUNK_LOCK_RETRY_NSEC (100L * 1000L * 1000L) /* 100ms */

/* Acquires the lock at <git_dir>/sg-chunks.lock, writing the path used into
   lock_path (lock_path_size bytes) so the caller can release it later.
   O_CREAT|O_EXCL makes the create-if-absent check atomic across processes
   (not just threads), which is the actual mutual-exclusion primitive here --
   the file's contents are never read, only its existence matters. Retries a
   bounded number of times with a short sleep between attempts (see the
   attempt-count/retry-interval constants above) rather than blocking
   forever. Returns 0 on success (lock held, caller must eventually call
   chunk_lock_release), -1 if the lock couldn't be acquired within the retry
   budget or the lock file couldn't be created for any other reason (message
   already printed to stderr either way). */
static int chunk_lock_acquire(const char *git_dir, char *lock_path, size_t lock_path_size)
{
    int attempt;

    snprintf(lock_path, lock_path_size, "%s/%s", git_dir, SG_CHUNK_LOCK_FILE);

    for (attempt = 0; attempt < SG_CHUNK_LOCK_MAX_ATTEMPTS; attempt++) {
        int fd = open(lock_path, O_CREAT | O_EXCL | O_WRONLY, 0644);

        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (errno != EEXIST) {
            fprintf(stderr, "sg: cannot create chunk keep-alive lock %s: %s\n", lock_path,
                   strerror(errno));
            return -1;
        }

        {
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = SG_CHUNK_LOCK_RETRY_NSEC;
            nanosleep(&ts, NULL);
        }
    }

    fprintf(stderr,
           "sg: cannot acquire chunk keep-alive lock %s (another sg process may be writing, "
           "or this is a stale lock file that can be removed after manual verification)\n",
           lock_path);
    return -1;
}

/* Releases a lock acquired by chunk_lock_acquire. keep_alive_add calls this
   unconditionally on every exit path out of its critical section (success
   or failure alike), so a failure partway through never leaves the lock
   file behind and blocks every subsequent chunk-storing call indefinitely. */
static void chunk_lock_release(const char *lock_path)
{
    unlink(lock_path);
}

/* Merges new_ids (new_count of them, just written by sg_chunk_store_blob)
   into the SG_CHUNK_KEEPALIVE_REF tree, deduplicating against whatever chunk
   ids are already kept alive, and points the ref at a fresh no-parent commit
   wrapping the rebuilt tree. Returns 0 on success, -1 on failure -- the
   caller treats that as reason to fall back to an ordinary (unchunked) blob
   rather than hand back a pointer whose chunks aren't gc-safe. The entire
   read-existing-through-update-ref critical section below is protected by
   sg-chunks.lock (see chunk_lock_acquire's doc comment just above) -- every
   path out of this function past the lock-acquire funnels through `done:`,
   which is what releases it, so a failure partway through never leaves the
   lock held. */
static int keep_alive_add(const char *git_dir, unsigned char (*new_ids)[SG_SHA1_RAW_LEN],
                          size_t new_count)
{
    unsigned char (*existing)[SG_SHA1_RAW_LEN] = NULL;
    size_t existing_count = 0;
    sg_tree_entry *entries = NULL;
    size_t entry_count = 0;
    size_t cap;
    unsigned char *tree_content = NULL;
    size_t tree_len = 0;
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    sg_commit commit;
    unsigned char *commit_content = NULL;
    size_t commit_len = 0;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    char *cleaned_message = NULL;
    char lock_path[SG_PATH_MAX];
    int lock_held = 0;
    size_t i;
    int rc = -1;

    if (new_count == 0)
        return 0;

    if (chunk_lock_acquire(git_dir, lock_path, sizeof(lock_path)) != 0)
        return -1;
    lock_held = 1;

    if (keep_alive_read_existing(git_dir, &existing, &existing_count) != 0)
        goto done;

    cap = existing_count + new_count;
    entries = malloc(cap * sizeof(*entries));
    if (entries == NULL)
        goto done;

    /* sg_tree_free (used elsewhere on any sg_tree) always frees .name, so
       every entry below -- including ones surviving unchanged from the
       existing tree -- gets its own fresh strdup rather than sharing a
       buffer, so this array can be torn down the same way. */
    for (i = 0; i < existing_count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];

        sg_sha1_to_hex(existing[i], hex);
        entries[entry_count].name = strdup(hex);
        if (entries[entry_count].name == NULL)
            goto done;
        entries[entry_count].mode = 0100644;
        memcpy(entries[entry_count].sha1, existing[i], SG_SHA1_RAW_LEN);
        entry_count++;
    }

    for (i = 0; i < new_count; i++) {
        char hex[SG_SHA1_HEX_LEN + 1];
        size_t j;
        int dup = 0;

        for (j = 0; j < entry_count; j++) {
            if (memcmp(entries[j].sha1, new_ids[i], SG_SHA1_RAW_LEN) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        sg_sha1_to_hex(new_ids[i], hex);
        entries[entry_count].name = strdup(hex);
        if (entries[entry_count].name == NULL)
            goto done;
        entries[entry_count].mode = 0100644;
        memcpy(entries[entry_count].sha1, new_ids[i], SG_SHA1_RAW_LEN);
        entry_count++;
    }

    /* Nothing new got added: every id the caller passed was already in the
       tree. Stop here rather than rebuilding an identical tree under a fresh
       commit.

       The commit's timestamp comes from time(NULL), so rebuilding produces
       the SAME object id only when it happens within the same second as the
       previous one -- which made this an intermittent failure rather than an
       obvious one. Measured: the push path merges the remote's keep-alive
       set into the local one on every push where the two refs differ, and
       with a deliberate 1.2s delay inserted between `sg add` and `sg push`
       the keep-alive ref moved on 8 runs out of 8, versus 6 out of 60
       without it. Both commits had a byte-identical tree and timestamps one
       second apart.

       Beyond the flakiness this is simply wasteful: an unchanged keep-alive
       set would otherwise leave a redundant commit in the object store on
       every push, and churn a ref that nothing has actually changed. */
    if (entry_count == existing_count) {
        rc = 0;
        goto done;
    }

    if (sg_tree_serialize(entries, entry_count, &tree_content, &tree_len) != 0)
        goto done;
    if (sg_loose_write(git_dir, SG_OBJ_TREE, tree_content, tree_len, tree_id) != 0)
        goto done;

    memset(&commit, 0, sizeof(commit));
    memcpy(commit.tree, tree_id, SG_SHA1_RAW_LEN);
    commit.parents = NULL;
    commit.parent_count = 0;
    commit.author_name = (char *)env_or("GIT_AUTHOR_NAME", "small_git");
    commit.author_email = (char *)env_or("GIT_AUTHOR_EMAIL", "sg@localhost");
    commit.author_time = (long long)time(NULL);
    strcpy(commit.author_tz, "+0000");
    commit.committer_name = (char *)env_or("GIT_COMMITTER_NAME", commit.author_name);
    commit.committer_email = (char *)env_or("GIT_COMMITTER_EMAIL", commit.author_email);
    commit.committer_time = commit.author_time;
    strcpy(commit.committer_tz, "+0000");

    if (sg_message_cleanup("sg chunk keep-alive\n", &cleaned_message) != 0)
        goto done;
    commit.message = cleaned_message;

    if (sg_commit_serialize(&commit, &commit_content, &commit_len) != 0)
        goto done;
    if (sg_loose_write(git_dir, SG_OBJ_COMMIT, commit_content, commit_len, commit_id) != 0)
        goto done;
    if (sg_ref_write_path(git_dir, SG_CHUNK_KEEPALIVE_REF, commit_id) != 0)
        goto done;

    rc = 0;

done:
    free(tree_content);
    free(commit_content);
    free(cleaned_message);
    for (i = 0; i < entry_count; i++)
        free(entries[i].name);
    free(entries);
    free(existing);
    if (lock_held)
        chunk_lock_release(lock_path);
    return rc;
}

int sg_chunk_keepalive_merge_commit(const char *git_dir,
                                    const unsigned char keepalive_commit_id[SG_SHA1_RAW_LEN])
{
    unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
    size_t count = 0;
    int rc;

    if (keep_alive_read_commit(git_dir, keepalive_commit_id, &ids, &count) != 0)
        return -1;
    rc = keep_alive_add(git_dir, ids, count);
    free(ids);
    return rc;
}

/* ---- keep-alive membership check: the "is this a pointer of ours?"
   identity test used by chunk_resolve below. Deliberately answers a
   different question than "does this chunk's object file still exist" --
   see chunk_resolve's discriminator comment for why conflating the two was
   the bug this replaces. Membership is checked against the tree
   SG_CHUNK_KEEPALIVE_REF's commit points at (every chunk id this repo's own
   sg_chunk_store_blob has ever produced), never against the object store
   directly, so a chunk that's genuinely ours but whose loose object went
   missing (gc, incomplete clone/fetch) is still correctly recognized as
   "ours" -- it just also turns out to be broken, which is chunk_resolve's
   job to detect and report, not this function's. ---- */

static int chunk_id_cmp(const void *a, const void *b)
{
    return memcmp(a, b, SG_SHA1_RAW_LEN);
}

/* Process-lifetime cache of the flattened, sorted keep-alive tree, so
   repeatedly resolving many chunk pointers (e.g. walking a whole commit's
   tree during restore/switch/merge/rebase/push) doesn't re-read and
   re-parse the commit/tree from scratch for every single pointer. Keyed on
   git_dir (by value, not pointer identity -- callers pass their own copies)
   plus the ref's current commit id, so it's automatically invalidated both
   by a fresh keep_alive_add in this same process (the ref's commit id
   changes) and by a different repo reusing the same git_dir path. */
static struct {
    int valid;
    char *git_dir;
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char (*ids)[SG_SHA1_RAW_LEN];
    size_t count;
} g_keepalive_cache;

static void keepalive_cache_atexit(void)
{
    free(g_keepalive_cache.git_dir);
    free(g_keepalive_cache.ids);
}

static int keep_alive_is_member(const char *git_dir, const unsigned char chunk_id[SG_SHA1_RAW_LEN])
{
    unsigned char commit_id[SG_SHA1_RAW_LEN];

    if (sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, commit_id) != 0)
        return 0; /* no keep-alive ref (e.g. a plain `git clone`): nothing is a member */

    if (!g_keepalive_cache.valid || g_keepalive_cache.git_dir == NULL ||
       strcmp(g_keepalive_cache.git_dir, git_dir) != 0 ||
       memcmp(g_keepalive_cache.commit_id, commit_id, SG_SHA1_RAW_LEN) != 0) {
        unsigned char (*ids)[SG_SHA1_RAW_LEN] = NULL;
        size_t count = 0;
        char *git_dir_copy;

        if (keep_alive_read_commit(git_dir, commit_id, &ids, &count) != 0)
            return 0; /* ref names an unreadable commit/tree: treat as not a member */

        git_dir_copy = strdup(git_dir);
        if (git_dir_copy == NULL) {
            free(ids);
            return 0;
        }

        if (count > 1)
            qsort(ids, count, sizeof(*ids), chunk_id_cmp);

        if (!g_keepalive_cache.valid)
            atexit(keepalive_cache_atexit);
        free(g_keepalive_cache.git_dir);
        free(g_keepalive_cache.ids);
        g_keepalive_cache.git_dir = git_dir_copy;
        memcpy(g_keepalive_cache.commit_id, commit_id, SG_SHA1_RAW_LEN);
        g_keepalive_cache.ids = ids;
        g_keepalive_cache.count = count;
        g_keepalive_cache.valid = 1;
    }

    return bsearch(chunk_id, g_keepalive_cache.ids, g_keepalive_cache.count,
                   sizeof(*g_keepalive_cache.ids), chunk_id_cmp) != NULL;
}

int sg_chunk_store_blob(const char *git_dir, const unsigned char *content, size_t len,
                        size_t threshold, unsigned char id_out[SG_SHA1_RAW_LEN],
                        int *chunked_out)
{
    size_t *offsets = NULL;
    size_t *lengths = NULL;
    size_t count = 0;
    unsigned char (*chunk_ids)[SG_SHA1_RAW_LEN] = NULL;
    sg_chunk_pointer ptr;
    unsigned char *ptr_content = NULL;
    size_t ptr_len = 0;
    size_t i;

    if (len < threshold) {
        if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, len, id_out) != 0)
            return -1;
        *chunked_out = 0;
        return 0;
    }

    if (sg_chunk_split(content, len, &offsets, &lengths, &count) != 0)
        return -1;

    if (count > 0) {
        chunk_ids = malloc(count * sizeof(*chunk_ids));
        if (chunk_ids == NULL) {
            free(offsets);
            free(lengths);
            return -1;
        }
    }

    for (i = 0; i < count; i++) {
        if (sg_loose_write(git_dir, SG_OBJ_BLOB, content + offsets[i], lengths[i], chunk_ids[i]) !=
           0) {
            free(chunk_ids);
            free(offsets);
            free(lengths);
            return -1;
        }
    }
    free(offsets);
    free(lengths);

    if (!chunks_round_trip_ok(git_dir, chunk_ids, count, content, len)) {
        /* The chunk blobs already written are content-addressed and harmless
           to leave behind (never deleted elsewhere in this project either);
           just fall back to storing content as a single ordinary blob. */
        free(chunk_ids);
        fprintf(stderr,
               "sg: warning: chunk round-trip verification failed, falling back to a plain blob\n");
        if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, len, id_out) != 0)
            return -1;
        *chunked_out = 0;
        return 0;
    }

    /* The chunks reproduce content correctly, but they're still only
       reachable -- from git's own object-graph perspective -- as plain hex
       text inside the pointer blob about to be written below; a `git gc`
       (manual or gc.auto) would otherwise collect them as garbage right out
       from under the pointer. Protect them by merging into the
       SG_CHUNK_KEEPALIVE_REF tree *before* handing back a pointer that
       depends on them existing; if that fails, fall back to an ordinary
       blob rather than produce a pointer whose chunks aren't gc-safe yet,
       same fallback shape as the round-trip failure just above. */
    if (keep_alive_add(git_dir, chunk_ids, count) != 0) {
        free(chunk_ids);
        fprintf(stderr,
               "sg: warning: cannot add chunk to keep-alive tree %s, falling back to a plain "
               "blob\n",
               SG_CHUNK_KEEPALIVE_REF);
        if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, len, id_out) != 0)
            return -1;
        *chunked_out = 0;
        return 0;
    }

    /* Record locally (in .git/config, not a ref -- see sg_repo_mark_chunking_used's
       doc comment) that this repo has now genuinely produced a chunk
       pointer. This is the durability fix's second half: if
       SG_CHUNK_KEEPALIVE_REF itself is later deleted/lost, chunk_resolve's
       discriminator needs some other way to know "this repo used chunking
       for real, an absent keep-alive ref here means the safety net broke"
       rather than misreading the absence as "this repo never chunked
       anything, e.g. a plain `git clone`" and quietly handing back pointer
       text as if it were file content. Treated the same as the round-trip
       and keep-alive-tree failures just above: if the marker can't be
       written, don't hand back a pointer this repo can't actually protect
       against a later lost ref -- fall back to an ordinary blob instead. */
    if (sg_repo_mark_chunking_used(git_dir) != 0) {
        free(chunk_ids);
        fprintf(stderr,
               "sg: warning: cannot mark chunk storage as used in .git/config, falling back to "
               "a plain blob\n");
        if (sg_loose_write(git_dir, SG_OBJ_BLOB, content, len, id_out) != 0)
            return -1;
        *chunked_out = 0;
        return 0;
    }

    sg_object_hash(SG_OBJ_BLOB, content, len, ptr.original_sha1);
    ptr.original_size = len;
    ptr.chunk_ids = chunk_ids;
    ptr.chunk_count = count;

    if (sg_chunk_pointer_format(&ptr, &ptr_content, &ptr_len) != 0) {
        free(chunk_ids);
        return -1;
    }
    free(chunk_ids);

    if (sg_loose_write(git_dir, SG_OBJ_BLOB, ptr_content, ptr_len, id_out) != 0) {
        free(ptr_content);
        return -1;
    }
    free(ptr_content);

    *chunked_out = 1;
    return 0;
}

void sg_chunk_print_missing_error(const char *path, const sg_chunk_missing_info *info)
{
    if (info->keepalive_lost) {
        fprintf(stderr, "sg: %s is a chunk-stored file, but %s is gone\n",
               sg_quote_path_delimited(path), SG_CHUNK_KEEPALIVE_REF);
        fprintf(stderr,
               "sg: this repository's .git/config records that chunk storage was used before, "
               "but %s, which protects the chunk data from being collected by git gc, no "
               "longer exists (it may have been manually deleted, or never fetched/cloned)\n",
               SG_CHUNK_KEEPALIVE_REF);
        fprintf(stderr,
               "sg: this means every chunk-stored file in this repository may have been "
               "collected by git gc; the data is likely lost\n");
        fprintf(stderr, "sg: cannot restore this file's content\n");
        return;
    }
    if (info->missing_count > 0) {
        fprintf(stderr, "sg: %s is a chunk-stored file, but %zu/%zu chunks are missing from "
               "the object store\n",
               sg_quote_path_delimited(path), info->missing_count, info->chunk_count);
    } else {
        fprintf(stderr,
               "sg: %s is a chunk-stored file, but the reassembled content does not match the "
               "recorded hash (the object store may be corrupt)\n",
               sg_quote_path_delimited(path));
    }
    fprintf(stderr, "sg: this usually means the object store was cleaned up by git gc, or %s "
           "was not fetched during clone\n",
           SG_CHUNK_KEEPALIVE_REF);
    fprintf(stderr, "sg: cannot restore this file's content\n");
}

/* Shared core of sg_chunk_read_blob/sg_chunk_effective_id. Always fills
   raw_content/raw_len (the blob's own bytes, as read) on success. Beyond
   that there are three outcomes, tracked by is_pointer/is_broken (see the
   discriminator comment below for why "first chunk id exists" is the line
   between "not a pointer" and "a real pointer"):
     - neither set: not a pointer (or an unverifiable pointer-shaped file) --
       raw_content/raw_len is the final answer.
     - is_pointer: a genuine, hash-verified pointer -- rebuilt/rebuilt_len/
       original_sha1 are the reassembled content.
     - is_broken: a genuine pointer whose data is missing/corrupt -- missing
       is filled in, rebuilt is NULL. Callers must treat this as a hard
       error, never falling back to raw_content as if it were real content.
   Returns 0 on success, -1 only when the top-level sg_object_read of id
   itself fails. */
typedef struct {
    unsigned char *raw_content; /* malloc'd; always set when this returns 0 */
    size_t raw_len;
    int is_pointer;
    int is_broken;
    unsigned char *rebuilt; /* malloc'd; only set when is_pointer */
    size_t rebuilt_len;
    unsigned char original_sha1[SG_SHA1_RAW_LEN];
    sg_chunk_missing_info missing; /* only meaningful when is_broken */
} chunk_resolved;

static int chunk_resolve(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                        chunk_resolved *r)
{
    sg_obj_type type;
    sg_chunk_pointer ptr;
    int is_our_pointer;
    unsigned char keepalive_commit_id[SG_SHA1_RAW_LEN];
    int keepalive_ref_exists;
    int ok;
    size_t i;

    memset(r, 0, sizeof(*r));

    if (sg_object_read(git_dir, id, &type, &r->raw_content, &r->raw_len) != 0)
        return -1;

    if (!sg_chunk_pointer_parse(r->raw_content, r->raw_len, &ptr))
        return 0; /* not a pointer -- raw_content/raw_len is the final answer */

    keepalive_ref_exists = sg_ref_read_path(git_dir, SG_CHUNK_KEEPALIVE_REF, keepalive_commit_id) == 0;

    if (!keepalive_ref_exists && ptr.chunk_count > 0 && sg_repo_chunking_was_used(git_dir)) {
        /* The keep-alive ref -- the only thing that lets us tell "a pointer
           our own sg_chunk_store_blob really produced" apart from
           "coincidentally pointer-shaped ordinary content" (see the
           discriminator comment just below) -- is gone: deleted, never
           fetched, or otherwise unreadable. Ordinarily that would just mean
           "this repo never used chunking" (e.g. a plain `git clone`, which
           never asks for SG_CHUNK_KEEPALIVE_REF at all) and every
           pointer-shaped file would correctly fall through to "not a
           pointer" below. But .git/config says otherwise: this repo's own
           sg_chunk_store_blob (or a `sg clone`/`sg fetch` that merged in a
           remote's keep-alive ref) DID produce real chunk pointers here at
           some point (see sg_repo_mark_chunking_used). With the ref gone,
           there is no way left to verify membership at all -- not just for
           this one pointer, but for every chunked file this repo ever
           committed, since the tree that would prove "these chunk ids are
           ours" no longer exists. Silently falling through to "not a
           pointer" here would resurrect exactly the bug this whole
           discriminator exists to prevent, just triggered a different way:
           it would hand back this (and every other chunked file's) raw
           pointer text as if it were real content, across the WHOLE
           repository, not just one broken file. Treat every well-formed
           pointer as broken instead -- a hard failure that's impossible to
           miss, versus data that's already gone being handed out as if
           nothing were wrong. */
        sg_chunk_pointer_free(&ptr);
        r->is_broken = 1;
        r->missing.keepalive_lost = 1;
        return 0;
    }

    /* Discriminator between "coincidental pointer-shaped ordinary content"
       and "a real chunk pointer of ours, possibly with missing/corrupt
       data": format alone (sg_chunk_pointer_parse succeeding) can't tell
       them apart -- an ordinary file can coincidentally contain
       well-formed-looking magic/size/sha1/chunks text (interop exercises
       exactly this with a small hand-written file). This is a question of
       *identity* ("did our own sg_chunk_store_blob ever produce this
       pointer's chunks?"), which is answered by checking whether the first
       declared chunk id is a member of the SG_CHUNK_KEEPALIVE_REF tree --
       every chunk id we've ever written ends up there (see keep_alive_add),
       and the set travels with clone/fetch independently of which loose
       object files happen to still be present.

       This used to be answered by checking whether the first chunk id's
       object file exists in the object store, which conflated identity with
       integrity: if that specific chunk (the first one) was the one lost to
       gc/corruption/an incomplete clone, a genuine pointer of ours would be
       misdiagnosed as "not a pointer" and its raw pointer text handed back
       as if it were the file's content -- silent data corruption on exactly
       the failure this whole discriminator exists to catch. Checking tree
       membership instead answers "is this ours?" without depending on
       whether the data is still intact; intactness is then verified
       separately below (every declared chunk read back and the reassembly
       hash-verified), with a lost/corrupt chunk now failing hard via
       is_broken instead of falling through to "not a pointer".

       sg_chunk_store_blob never produces a pointer with zero chunks
       (chunking only ever triggers for len >= threshold > 0, which always
       yields at least one chunk), so chunk_count == 0 takes the same "not a
       pointer" path as a first chunk id absent from the keep-alive tree.
       This is a heuristic, not a proof -- a contrived file could in
       principle collide with a real chunk id -- but it turns "real pointer,
       data lost" from silent corruption into a hard error while still
       protecting ordinary files from being misdiagnosed as broken
       pointers. */
    is_our_pointer = ptr.chunk_count > 0 && keep_alive_is_member(git_dir, ptr.chunk_ids[0]);
    if (!is_our_pointer) {
        sg_chunk_pointer_free(&ptr);
        return 0; /* not a pointer we recognize as ours */
    }

    ok = 1;
    if (ptr.original_size > 0) {
        r->rebuilt = malloc(ptr.original_size);
        if (r->rebuilt == NULL)
            ok = 0;
    }

    r->missing.chunk_count = ptr.chunk_count;
    r->missing.missing_count = 0;

    /* Unlike a plain reassembly, this deliberately keeps scanning every
       chunk even after one is found missing or a copy would overflow --
       ok latches false and stops contributing to rebuilt, but the loop
       still needs to finish so missing_count reflects every absent chunk
       for sg_chunk_print_missing_error's message, not just the first one
       found. */
    for (i = 0; i < ptr.chunk_count; i++) {
        sg_obj_type chunk_type;
        unsigned char *chunk_content = NULL;
        size_t chunk_len = 0;

        if (sg_object_read(git_dir, ptr.chunk_ids[i], &chunk_type, &chunk_content, &chunk_len) != 0) {
            r->missing.missing_count++;
            ok = 0;
            continue;
        }
        if (!ok || chunk_len > ptr.original_size - r->rebuilt_len) {
            free(chunk_content);
            ok = 0;
            continue;
        }
        memcpy(r->rebuilt + r->rebuilt_len, chunk_content, chunk_len);
        r->rebuilt_len += chunk_len;
        free(chunk_content);
    }

    if (ok && r->rebuilt_len == ptr.original_size) {
        unsigned char computed_sha1[SG_SHA1_RAW_LEN];

        sg_object_hash(SG_OBJ_BLOB, r->rebuilt, ptr.original_size, computed_sha1);
        if (memcmp(computed_sha1, ptr.original_sha1, SG_SHA1_RAW_LEN) != 0)
            ok = 0;
    } else {
        ok = 0;
    }

    if (ok) {
        memcpy(r->original_sha1, ptr.original_sha1, SG_SHA1_RAW_LEN);
        r->is_pointer = 1;
    } else {
        /* A genuine chunk pointer (first chunk id resolved) whose data is
           missing or corrupt: this is a hard error for the caller, never a
           silent fallback to the pointer's own raw text. */
        free(r->rebuilt);
        r->rebuilt = NULL;
        r->rebuilt_len = 0;
        r->is_broken = 1;
    }

    sg_chunk_pointer_free(&ptr);
    return 0;
}

static void chunk_resolved_free(chunk_resolved *r)
{
    free(r->raw_content);
    free(r->rebuilt);
}

int sg_chunk_read_blob(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                       unsigned char **content_out, size_t *len_out,
                       sg_chunk_missing_info *missing_out)
{
    chunk_resolved r;

    if (chunk_resolve(git_dir, id, &r) != 0)
        return -1;

    if (r.is_broken) {
        if (missing_out != NULL)
            *missing_out = r.missing;
        chunk_resolved_free(&r);
        return -2;
    }

    if (r.is_pointer) {
        free(r.raw_content);
        *content_out = r.rebuilt;
        *len_out = r.rebuilt_len;
    } else {
        *content_out = r.raw_content;
        *len_out = r.raw_len;
    }
    return 0;
}

int sg_chunk_effective_id(const char *git_dir, const unsigned char id[SG_SHA1_RAW_LEN],
                          unsigned char out[SG_SHA1_RAW_LEN])
{
    chunk_resolved r;

    if (chunk_resolve(git_dir, id, &r) != 0)
        return -1;

    if (r.is_broken) {
        chunk_resolved_free(&r);
        return -2;
    }

    if (r.is_pointer)
        memcpy(out, r.original_sha1, SG_SHA1_RAW_LEN);
    else
        memcpy(out, id, SG_SHA1_RAW_LEN);

    chunk_resolved_free(&r);
    return 0;
}

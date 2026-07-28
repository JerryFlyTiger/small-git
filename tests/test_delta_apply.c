/* sg_pack_delta_apply is defined (non-static, but not part of sg/pack.h's
   public surface) in src/storage/pack.c; declared here via extern so the
   delta-reconstruction logic is unit-testable without exposing it as public
   API. Test vectors were built with a small Python helper that implements
   the same encoding the task spec describes, then hand-verified byte by
   byte against the pseudocode. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sg_pack_delta_apply(const unsigned char *base, size_t base_len,
                               const unsigned char *delta, size_t delta_len, unsigned char **out,
                               size_t *out_len);

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

/* base_size=13, target_size=13, one copy instruction: offset=0 (omitted),
   size=13 (one size byte) -- copies the whole base verbatim */
static void test_pure_copy(void)
{
    const unsigned char base[] = "Hello, World!";
    const unsigned char delta[] = {0x0d, 0x0d, 0x90, 0x0d};
    unsigned char *out;
    size_t out_len;

    CHECK(sg_pack_delta_apply(base, 13, delta, sizeof(delta), &out, &out_len) == 0,
         "delta apply failed");
    CHECK(out_len == 13, "expected out_len 13, got %zu", out_len);
    CHECK(memcmp(out, base, 13) == 0, "content mismatch");
    free(out);
}

/* base_size=0, target_size=6, one insert instruction of 6 literal bytes --
   an empty base entirely reconstructed from literal data */
static void test_pure_insert(void)
{
    const unsigned char delta[] = {0x00, 0x06, 0x06, 'a', 'b', 'c', 'd', 'e', 'f'};
    unsigned char *out;
    size_t out_len;

    CHECK(sg_pack_delta_apply(NULL, 0, delta, sizeof(delta), &out, &out_len) == 0,
         "delta apply failed");
    CHECK(out_len == 6, "expected out_len 6, got %zu", out_len);
    CHECK(memcmp(out, "abcdef", 6) == 0, "content mismatch");
    free(out);
}

/* base_size=43, target_size=50: copy "The quick brown " (offset=0, size=16),
   insert " ADDED " (7 literal bytes), copy "fox jumps over the lazy dog"
   (offset=16, size=27) -- exercises copy+insert mixed in one stream */
static void test_copy_and_insert_mixed(void)
{
    const unsigned char base[] = "The quick brown fox jumps over the lazy dog";
    const unsigned char delta[] = {0x2b, 0x32, 0x90, 0x10, 0x07, ' ', 'A', 'D', 'D',
                                   'E',  'D',  ' ',  0x91, 0x10, 0x1b};
    const char *expected = "The quick brown  ADDED fox jumps over the lazy dog";
    unsigned char *out;
    size_t out_len;

    CHECK(sizeof(base) - 1 == 43, "test setup: base should be 43 bytes, got %zu",
         sizeof(base) - 1);
    CHECK(sg_pack_delta_apply(base, sizeof(base) - 1, delta, sizeof(delta), &out, &out_len) == 0,
         "delta apply failed");
    CHECK(out_len == strlen(expected), "expected out_len %zu, got %zu", strlen(expected), out_len);
    CHECK(out_len == strlen(expected) && memcmp(out, expected, out_len) == 0, "content mismatch");
    free(out);
}

/* the copy instruction's size field being entirely absent (all three size
   bits clear in the opcode) means size 65536, not 0 -- git's one genuine
   special case in this format. base_size=70000, target_size=65536+4: copy
   offset=0 size=<omitted, i.e. 65536>, then insert 4 literal bytes. */
static void test_copy_size_zero_means_65536(void)
{
    unsigned char *base;
    unsigned char delta[] = {0xf0, 0xa2, 0x04, 0x84, 0x80, 0x04,
                             0x80, 0x04, 'T',  'A',  'I',  'L'};
    unsigned char *out;
    size_t out_len;
    size_t i;

    base = malloc(70000);
    if (base == NULL) {
        CHECK(0, "malloc failed");
        return;
    }
    for (i = 0; i < 70000; i++)
        base[i] = (unsigned char)(i & 0xFF);

    CHECK(sg_pack_delta_apply(base, 70000, delta, sizeof(delta), &out, &out_len) == 0,
         "delta apply failed");
    CHECK(out_len == 65536 + 4, "expected out_len %d, got %zu", 65536 + 4, out_len);
    if (out_len == 65536 + 4) {
        CHECK(memcmp(out, base, 65536) == 0, "copied region mismatch");
        CHECK(memcmp(out + 65536, "TAIL", 4) == 0, "inserted tail mismatch");
    }
    free(out);
    free(base);
}

/* a mismatched declared base size must be rejected rather than silently
   proceeding against the wrong base object */
static void test_base_size_mismatch_rejected(void)
{
    const unsigned char base[] = "short";
    const unsigned char delta[] = {0x63, 0x05, 0x91, 0x00, 0x05}; /* claims base_size=99 */
    unsigned char *out;
    size_t out_len;

    CHECK(sg_pack_delta_apply(base, 5, delta, sizeof(delta), &out, &out_len) != 0,
         "expected rejection of mismatched base size");
}

int main(void)
{
    test_pure_copy();
    test_pure_insert();
    test_copy_and_insert_mixed();
    test_copy_size_zero_means_65536();
    test_base_size_mismatch_rejected();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all delta apply tests passed\n");
    return 0;
}

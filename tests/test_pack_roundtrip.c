#include "sg/pack.h"

#include "sg/hash.h"
#include "sg/loose.h"
#include "sg/object.h"
#include "sg/repo.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
          (uint32_t)p[3];
}

static char *make_tmp_repo(void)
{
    char *dir = strdup("/tmp/sg_pack_test_XXXXXX");

    if (dir == NULL)
        return NULL;
    if (mkdtemp(dir) == NULL) {
        free(dir);
        return NULL;
    }
    if (sg_repo_init(dir) != 0) {
        free(dir);
        return NULL;
    }
    return dir;
}

static char *git_dir_of(const char *repo_dir)
{
    char buf[4096];

    snprintf(buf, sizeof(buf), "%s/.git", repo_dir);
    return strdup(buf);
}

static void rm_rf(const char *path)
{
    char cmd[4300];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best-effort cleanup */
    }
}

/* Writes a blob, a tree, and a commit as loose objects, packs all three with
   sg_pack_write, then reads each back via sg_pack_read and checks the type
   and content are byte-for-byte identical to what was written. Also
   validates the .idx file's fanout table, sha1 ordering, and trailer. */
static void test_roundtrip_blob_tree_commit(void)
{
    char *repo_dir;
    char *git_dir;
    unsigned char blob_id[SG_SHA1_RAW_LEN];
    unsigned char tree_id[SG_SHA1_RAW_LEN];
    unsigned char commit_id[SG_SHA1_RAW_LEN];
    unsigned char ids[3][SG_SHA1_RAW_LEN];
    const char *blob_content = "hello from a packed blob\n";
    unsigned char tree_content[64];
    size_t tree_content_len;
    const char *commit_content =
        "tree 0000000000000000000000000000000000000000\n"
        "author A <a@example.com> 1700000000 +0000\n"
        "committer A <a@example.com> 1700000000 +0000\n"
        "\n"
        "test commit\n";

    repo_dir = make_tmp_repo();
    CHECK(repo_dir != NULL, "failed to create tmp repo");
    if (repo_dir == NULL)
        return;
    git_dir = git_dir_of(repo_dir);
    CHECK(git_dir != NULL, "failed to build git_dir");

    CHECK(sg_loose_write(git_dir, SG_OBJ_BLOB, blob_content, strlen(blob_content), blob_id) == 0,
         "failed to write blob");

    /* a minimal, syntactically valid tree entry referencing the blob above */
    {
        unsigned char *p = tree_content;
        const char *mode_name = "100644 leaf.txt";

        memcpy(p, mode_name, strlen(mode_name));
        p += strlen(mode_name);
        *p++ = '\0';
        memcpy(p, blob_id, SG_SHA1_RAW_LEN);
        p += SG_SHA1_RAW_LEN;
        tree_content_len = (size_t)(p - tree_content);
    }
    CHECK(sg_loose_write(git_dir, SG_OBJ_TREE, tree_content, tree_content_len, tree_id) == 0,
         "failed to write tree");

    CHECK(sg_loose_write(git_dir, SG_OBJ_COMMIT, commit_content, strlen(commit_content),
                         commit_id) == 0,
         "failed to write commit");

    memcpy(ids[0], blob_id, SG_SHA1_RAW_LEN);
    memcpy(ids[1], tree_id, SG_SHA1_RAW_LEN);
    memcpy(ids[2], commit_id, SG_SHA1_RAW_LEN);

    CHECK(sg_pack_write(git_dir, ids, 3) == 0, "sg_pack_write failed");

    {
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        CHECK(sg_pack_read(git_dir, blob_id, &type, &content, &content_len) == 0,
             "sg_pack_read failed for blob");
        CHECK(type == SG_OBJ_BLOB, "blob type mismatch");
        CHECK(content_len == strlen(blob_content), "blob content_len mismatch");
        CHECK(content_len == strlen(blob_content) &&
                 memcmp(content, blob_content, content_len) == 0,
             "blob content mismatch");
        free(content);

        CHECK(sg_pack_read(git_dir, tree_id, &type, &content, &content_len) == 0,
             "sg_pack_read failed for tree");
        CHECK(type == SG_OBJ_TREE, "tree type mismatch");
        CHECK(content_len == tree_content_len, "tree content_len mismatch");
        CHECK(content_len == tree_content_len &&
                 memcmp(content, tree_content, content_len) == 0,
             "tree content mismatch");
        free(content);

        CHECK(sg_pack_read(git_dir, commit_id, &type, &content, &content_len) == 0,
             "sg_pack_read failed for commit");
        CHECK(type == SG_OBJ_COMMIT, "commit type mismatch");
        CHECK(content_len == strlen(commit_content), "commit content_len mismatch");
        CHECK(content_len == strlen(commit_content) &&
                 memcmp(content, commit_content, content_len) == 0,
             "commit content mismatch");
        free(content);
    }

    /* an id that was never packed must not be found */
    {
        unsigned char bogus_id[SG_SHA1_RAW_LEN];
        sg_obj_type type;
        unsigned char *content;
        size_t content_len;

        memset(bogus_id, 0xAB, sizeof(bogus_id));
        CHECK(sg_pack_read(git_dir, bogus_id, &type, &content, &content_len) != 0,
             "sg_pack_read unexpectedly found a non-existent object");
    }

    /* ---- validate the .idx file's own structure directly ---- */
    {
        char pack_dir[4096];
        DIR *d;
        struct dirent *de;
        char idx_path[4096];
        int found_idx = 0;
        FILE *f;
        struct stat st;
        unsigned char *idx_data;
        size_t idx_len;
        uint32_t count;
        const unsigned char *sha1_table;
        size_t i;

        snprintf(pack_dir, sizeof(pack_dir), "%s/objects/pack", git_dir);
        d = opendir(pack_dir);
        CHECK(d != NULL, "objects/pack dir missing");
        if (d != NULL) {
            while ((de = readdir(d)) != NULL) {
                size_t len = strlen(de->d_name);

                if (len > 4 && strcmp(de->d_name + len - 4, ".idx") == 0) {
                    snprintf(idx_path, sizeof(idx_path), "%s/%s", pack_dir, de->d_name);
                    found_idx = 1;
                    break;
                }
            }
            closedir(d);
        }
        CHECK(found_idx, "no .idx file found after sg_pack_write");

        if (found_idx) {
            f = fopen(idx_path, "rb");
            CHECK(f != NULL, "failed to open .idx file");
            if (f != NULL) {
                fstat(fileno(f), &st);
                idx_len = (size_t)st.st_size;
                idx_data = malloc(idx_len);
                CHECK(idx_data != NULL, "malloc failed for idx_data");
                if (idx_data != NULL) {
                    CHECK(fread(idx_data, 1, idx_len, f) == idx_len, "short read on idx file");

                    CHECK(idx_data[0] == 0xff && idx_data[1] == 0x74 && idx_data[2] == 0x4f &&
                             idx_data[3] == 0x63,
                         "idx magic mismatch");
                    CHECK(be32(idx_data + 4) == 2, "idx version should be 2");

                    count = be32(idx_data + 8 + 255 * 4);
                    CHECK(count == 3, "expected fanout[255] == 3, got %u", count);

                    /* fanout must be non-decreasing and end at count */
                    {
                        uint32_t prev = 0;
                        int monotonic = 1;

                        for (i = 0; i < 256; i++) {
                            uint32_t v = be32(idx_data + 8 + i * 4);

                            if (v < prev)
                                monotonic = 0;
                            prev = v;
                        }
                        CHECK(monotonic, "fanout table is not non-decreasing");
                    }

                    sha1_table = idx_data + 8 + 256 * 4;
                    {
                        int sorted = 1;

                        for (i = 0; i + 1 < count; i++) {
                            if (memcmp(sha1_table + i * SG_SHA1_RAW_LEN,
                                      sha1_table + (i + 1) * SG_SHA1_RAW_LEN,
                                      SG_SHA1_RAW_LEN) >= 0)
                                sorted = 0;
                        }
                        CHECK(sorted, "sha1 table is not strictly sorted ascending");
                    }

                    /* every id we packed must appear somewhere in the table */
                    {
                        int all_present = 1;
                        unsigned char(*want)[SG_SHA1_RAW_LEN] = ids;
                        size_t w;

                        for (w = 0; w < 3; w++) {
                            int present = 0;

                            for (i = 0; i < count; i++) {
                                if (memcmp(sha1_table + i * SG_SHA1_RAW_LEN, want[w],
                                          SG_SHA1_RAW_LEN) == 0)
                                    present = 1;
                            }
                            if (!present)
                                all_present = 0;
                        }
                        CHECK(all_present, "not every packed id was found in the idx sha1 table");
                    }

                    /* trailer: pack checksum (20 bytes) + idx's own sha1 (20 bytes) */
                    {
                        unsigned char idx_self_sha1[SG_SHA1_RAW_LEN];

                        CHECK(idx_len >= 2 * SG_SHA1_RAW_LEN, "idx too short for trailer");
                        sg_sha1(idx_data, idx_len - SG_SHA1_RAW_LEN, idx_self_sha1);
                        CHECK(memcmp(idx_self_sha1, idx_data + idx_len - SG_SHA1_RAW_LEN,
                                    SG_SHA1_RAW_LEN) == 0,
                             "idx trailer sha1 does not match a hash of the preceding bytes");
                    }

                    free(idx_data);
                }
                fclose(f);
            }
        }
    }

    free(git_dir);
    rm_rf(repo_dir);
    free(repo_dir);
}

int main(void)
{
    test_roundtrip_blob_tree_commit();

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("all pack roundtrip tests passed\n");
    return 0;
}

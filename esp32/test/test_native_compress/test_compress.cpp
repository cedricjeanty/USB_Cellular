#include <unity.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include "airbridge_compress.h"

static const char* TMP = "/tmp/test_compress";

void setUp(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TMP, TMP);
    int rc = system(cmd); (void)rc;
}
void tearDown(void) {}

// Fixture dir (esp32/test/fixtures/), mirrors test_native_dsu.
static std::string fixtureDir() {
    std::string p = __FILE__;
    auto pos = p.rfind('/'); if (pos != std::string::npos) p = p.substr(0, pos);
    pos = p.rfind('/');      if (pos != std::string::npos) p = p.substr(0, pos);
    return p + "/fixtures/";
}

// Write a file of `kb` KB of ~3x-compressible bytes (5-symbol alphabet, like the
// benchmark's realistic filler) so the unit test reflects real-log compressibility.
static void writeCompressible(const char* path, size_t kb, unsigned seed = 1) {
    FILE* f = fopen(path, "wb");
    const char alpha[5] = {'A','B','C','D','E'};
    std::vector<unsigned char> buf(1024);
    unsigned x = seed * 2654435761u + 1;
    for (size_t k = 0; k < kb; k++) {
        for (size_t i = 0; i < 1024; i++) { x = x * 1103515245u + 12345u; buf[i] = alpha[(x >> 16) % 5]; }
        fwrite(buf.data(), 1, 1024, f);
    }
    fclose(f);
}

static uint64_t fsize(const char* p) {
    FILE* f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); uint64_t s = (uint64_t)ftell(f); fclose(f); return s;
}

// ── ~3x ratio on compressible data ───────────────────────────────────────────
void test_gzip_ratio_compressible(void) {
    char src[256], dst[256];
    snprintf(src, sizeof(src), "%s/in.bin", TMP);
    snprintf(dst, sizeof(dst), "%s/in.bin.gz", TMP);
    writeCompressible(src, 512);  // 512 KB

    GzipResult r = gzipFileStdio(src, dst);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT64(512 * 1024, r.inBytes);
    TEST_ASSERT_EQUAL_UINT64(fsize(dst), r.outBytes);
    double ratio = (double)r.inBytes / (double)r.outBytes;
    printf("[test] compressible ratio %.2fx (%llu -> %llu)\n", ratio,
           (unsigned long long)r.inBytes, (unsigned long long)r.outBytes);
    TEST_ASSERT_TRUE_MESSAGE(ratio > 2.3, "5-symbol filler should compress > 2.3x");
}

// ── gunzip round-trips (valid .gz framing) ───────────────────────────────────
void test_gzip_roundtrip_identity(void) {
    char src[256], dst[256], ungz[256];
    snprintf(src, sizeof(src), "%s/r.bin", TMP);
    snprintf(dst, sizeof(dst), "%s/r.bin.gz", TMP);
    snprintf(ungz, sizeof(ungz), "%s/r.out", TMP);
    writeCompressible(src, 200, 7);

    TEST_ASSERT_TRUE(gzipFileStdio(src, dst).ok);

    // System gunzip must accept our framing and reproduce the bytes exactly.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "gunzip -c '%s' > '%s'", dst, ungz);
    TEST_ASSERT_EQUAL_INT(0, system(cmd));
    snprintf(cmd, sizeof(cmd), "cmp -s '%s' '%s'", src, ungz);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, system(cmd), "gunzip output must equal the original");
}

// ── empty input → valid (tiny) gzip that gunzips to empty ────────────────────
void test_gzip_empty(void) {
    char src[256], dst[256], ungz[256];
    snprintf(src, sizeof(src), "%s/e.bin", TMP);
    snprintf(dst, sizeof(dst), "%s/e.bin.gz", TMP);
    snprintf(ungz, sizeof(ungz), "%s/e.out", TMP);
    fclose(fopen(src, "wb"));  // 0 bytes

    GzipResult r = gzipFileStdio(src, dst);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT64(0, r.inBytes);
    TEST_ASSERT_TRUE(r.outBytes > 0);  // gzip header+trailer
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "gunzip -c '%s' > '%s'", dst, ungz);
    TEST_ASSERT_EQUAL_INT(0, system(cmd));
    TEST_ASSERT_EQUAL_UINT64(0, fsize(ungz));
}

// ── real .eaofh fixture compresses ~3x + round-trips (gated on availability) ──
void test_gzip_real_fixture(void) {
    std::string src = fixtureDir() + "EA500.000243_01077.eaofh";
    if (access(src.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }
    char dst[256], ungz[256];
    snprintf(dst, sizeof(dst), "%s/fx.gz", TMP);
    snprintf(ungz, sizeof(ungz), "%s/fx.out", TMP);

    GzipResult r = gzipFileStdio(src.c_str(), dst);
    TEST_ASSERT_TRUE(r.ok);
    double ratio = (double)r.inBytes / (double)r.outBytes;
    printf("[test] real .eaofh ratio %.2fx (%llu -> %llu)\n", ratio,
           (unsigned long long)r.inBytes, (unsigned long long)r.outBytes);
    TEST_ASSERT_TRUE_MESSAGE(ratio > 2.0, "real flight log should compress > 2x");

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "gunzip -c '%s' > '%s' && cmp -s '%s' '%s'",
             dst, ungz, src.c_str(), ungz);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, system(cmd), "real fixture must round-trip");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_gzip_ratio_compressible);
    RUN_TEST(test_gzip_roundtrip_identity);
    RUN_TEST(test_gzip_empty);
    RUN_TEST(test_gzip_real_fixture);
    return UNITY_END();
}

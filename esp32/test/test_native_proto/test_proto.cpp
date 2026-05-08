#include <unity.h>
#include <cstring>
#include <cstdio>
#include "airbridge_proto.h"

void setUp(void) {}
void tearDown(void) {}

// ── dechunk ─────────────────────────────────────────────────────────────────

void test_dechunk_single(void) {
    std::string raw = "5\r\nhello\r\n0\r\n";
    TEST_ASSERT_EQUAL_STRING("hello", dechunk(raw).c_str());
}
void test_dechunk_multiple(void) {
    std::string raw = "5\r\nhello\r\n6\r\n world\r\n0\r\n";
    TEST_ASSERT_EQUAL_STRING("hello world", dechunk(raw).c_str());
}
void test_dechunk_empty(void) {
    std::string raw = "0\r\n";
    TEST_ASSERT_EQUAL_STRING("", dechunk(raw).c_str());
}
void test_dechunk_hex_size(void) {
    // "a" hex = 10 decimal
    std::string data = "0123456789";
    std::string raw = "a\r\n" + data + "\r\n0\r\n";
    TEST_ASSERT_EQUAL_STRING("0123456789", dechunk(raw).c_str());
}
void test_dechunk_uppercase_hex(void) {
    std::string raw = "A\r\n0123456789\r\n0\r\n";
    TEST_ASSERT_EQUAL_STRING("0123456789", dechunk(raw).c_str());
}

// ── crc16_8005 ──────────────────────────────────────────────────────────────

void test_crc16_deterministic(void) {
    uint8_t data[] = {0xEA, 0x1E, 0x00, 78, 0xD1};
    uint16_t crc1 = crc16_8005(data, 5);
    uint16_t crc2 = crc16_8005(data, 5);
    TEST_ASSERT_EQUAL_UINT16(crc1, crc2);
    TEST_ASSERT_NOT_EQUAL(0, crc1);
}
void test_crc16_zero_length(void) {
    uint8_t dummy = 0;
    uint16_t crc = crc16_8005(&dummy, 0);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, crc);  // init value unchanged
}
void test_crc16_different_data(void) {
    uint8_t a[] = {0x01, 0x02, 0x03};
    uint8_t b[] = {0x03, 0x02, 0x01};
    TEST_ASSERT_NOT_EQUAL(crc16_8005(a, 3), crc16_8005(b, 3));
}

// ── buildDsuCookie ──────────────────────────────────────────────────────────

void test_buildDsuCookie_magic(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1218, cookie);
    TEST_ASSERT_EQUAL_UINT8(0xEA, cookie[0]);
    TEST_ASSERT_EQUAL_UINT8(0x1E, cookie[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, cookie[2]);
    TEST_ASSERT_EQUAL_UINT8(78,   cookie[3]);
    TEST_ASSERT_EQUAL_UINT8(0xD1, cookie[4]);
}
void test_buildDsuCookie_serial(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1218, cookie);
    TEST_ASSERT_EQUAL_STRING("EA500.000243", (const char*)&cookie[9]);
}
void test_buildDsuCookie_flight_be(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 0x01020304, cookie);
    TEST_ASSERT_EQUAL_UINT8(0x01, cookie[62]);
    TEST_ASSERT_EQUAL_UINT8(0x02, cookie[63]);
    TEST_ASSERT_EQUAL_UINT8(0x03, cookie[64]);
    TEST_ASSERT_EQUAL_UINT8(0x04, cookie[65]);
}
void test_buildDsuCookie_mode_flag(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1218, cookie);
    TEST_ASSERT_EQUAL_UINT8(0x01, cookie[60]);
}
void test_buildDsuCookie_crc_validates(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1218, cookie);
    uint16_t expected = crc16_8005(cookie, 76);
    uint16_t actual = ((uint16_t)cookie[76] << 8) | cookie[77];
    TEST_ASSERT_EQUAL_UINT16(expected, actual);
}
void test_buildDsuCookie_small_flight(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000001", 1, cookie);
    TEST_ASSERT_EQUAL_UINT8(0x00, cookie[62]);
    TEST_ASSERT_EQUAL_UINT8(0x00, cookie[63]);
    TEST_ASSERT_EQUAL_UINT8(0x00, cookie[64]);
    TEST_ASSERT_EQUAL_UINT8(0x01, cookie[65]);
    // CRC still validates
    uint16_t expected = crc16_8005(cookie, 76);
    uint16_t actual = ((uint16_t)cookie[76] << 8) | cookie[77];
    TEST_ASSERT_EQUAL_UINT16(expected, actual);
}
// Golden-file check — byte-for-byte match against a known-good cookie captured
// from a real DSU. Regression guard: any change to buildDsuCookie() that breaks
// aircraft compatibility will fail this test.
void test_buildDsuCookie_golden_1210(void) {
    uint8_t ours[78];
    buildDsuCookie("EA500.000243", 1210, ours);

    FILE* f = fopen("test/test_native_proto/fixtures/dsuCookie_1210.easdf", "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "fixture dsuCookie_1210.easdf missing");
    uint8_t ref[78] = {0};
    size_t n = fread(ref, 1, 78, f);
    fclose(f);
    TEST_ASSERT_EQUAL_size_t(78, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, ours, 78);
}

// ── buildDsuCookieDate (date-mode) ───────────────────────────────────────────
void test_reverse_bits_8(void) {
    TEST_ASSERT_EQUAL_UINT8(0x00, reverse_bits_8(0x00));
    TEST_ASSERT_EQUAL_UINT8(0xFF, reverse_bits_8(0xFF));
    TEST_ASSERT_EQUAL_UINT8(0x80, reverse_bits_8(0x01));
    TEST_ASSERT_EQUAL_UINT8(0x19, reverse_bits_8(0x98));
}
void test_to_bcd(void) {
    TEST_ASSERT_EQUAL_UINT8(0x00, to_bcd(0));
    TEST_ASSERT_EQUAL_UINT8(0x09, to_bcd(9));
    TEST_ASSERT_EQUAL_UINT8(0x26, to_bcd(26));
    TEST_ASSERT_EQUAL_UINT8(0x99, to_bcd(99));
}
// Verified by hand against EA500 reference encoder for 2026-04-06: 90 19 08 C1.
void test_encode_arinc_date_known(void) {
    uint8_t b2, b3, b4;
    encode_arinc_date(2026, 4, 6, &b2, &b3, &b4);
    TEST_ASSERT_EQUAL_UINT8(0x19, b2);
    TEST_ASSERT_EQUAL_UINT8(0x08, b3);
    TEST_ASSERT_EQUAL_UINT8(0xC1, b4);  // includes parity bit
}
// Parity must always be odd across {0x90, b2, b3, b4}.
void test_encode_arinc_date_parity_odd(void) {
    struct { uint16_t y; uint8_t m, d; } cases[] = {
        {2025,1,1}, {2025,3,1}, {2026,4,6}, {2026,12,31}, {2099,6,15}, {2030,7,4}
    };
    for (auto& c : cases) {
        uint8_t b2, b3, b4;
        encode_arinc_date(c.y, c.m, c.d, &b2, &b3, &b4);
        int ones = __builtin_popcount(0x90) + __builtin_popcount(b2)
                 + __builtin_popcount(b3) + __builtin_popcount(b4);
        TEST_ASSERT_EQUAL_INT(1, ones & 1);
    }
}
void test_buildDsuCookieDate_layout(void) {
    uint8_t cookie[78];
    buildDsuCookieDate("EA500.000243", 2026, 4, 6, cookie);
    // Header
    TEST_ASSERT_EQUAL_UINT8(0xEA, cookie[0]);
    TEST_ASSERT_EQUAL_UINT8(0x1E, cookie[1]);
    TEST_ASSERT_EQUAL_UINT8(0xD1, cookie[4]);
    TEST_ASSERT_EQUAL_UINT8(0x01, cookie[60]);
    // ARINC date word
    TEST_ASSERT_EQUAL_UINT8(0x90, cookie[52]);
    TEST_ASSERT_EQUAL_UINT8(0x19, cookie[53]);
    TEST_ASSERT_EQUAL_UINT8(0x08, cookie[54]);
    TEST_ASSERT_EQUAL_UINT8(0xC1, cookie[55]);
    // End sentinel = 0xFFFFFFFF (no upper bound) instead of flight number
    TEST_ASSERT_EQUAL_UINT8(0xFF, cookie[62]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, cookie[63]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, cookie[64]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, cookie[65]);
    // CRC validates
    uint16_t expected = crc16_8005(cookie, 76);
    uint16_t actual = ((uint16_t)cookie[76] << 8) | cookie[77];
    TEST_ASSERT_EQUAL_UINT16(expected, actual);
}
// Byte-for-byte match against EA500 reference encoder.
void test_buildDsuCookieDate_golden(void) {
    uint8_t ours[78];
    buildDsuCookieDate("EA500.000243", 2026, 4, 6, ours);

    FILE* f = fopen("test/test_native_proto/fixtures/dsuCookie_date_20260406.easdf", "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "fixture dsuCookie_date_20260406.easdf missing");
    uint8_t ref[78] = {0};
    size_t n = fread(ref, 1, 78, f);
    fclose(f);
    TEST_ASSERT_EQUAL_size_t(78, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, ours, 78);
}

// ── lastRecordFromLog (content-based parser) ────────────────────────────────

// Build a single record at the given offset within `out`. Returns total bytes.
// rtype: 0x0E or 0x4C. Pads with zeros up to rlen.
static size_t makeRec(uint8_t* out, uint8_t rtype, uint16_t rlen,
                     const char* serial, uint16_t flight) {
    memset(out, 0, rlen);
    out[0] = 0xEA;
    out[1] = rtype;
    out[2] = (rlen >> 8) & 0xFF;
    out[3] = rlen & 0xFF;
    size_t snLen = strnlen(serial, 12);
    memcpy(out + 4 + 5, serial, snLen);
    out[4 + 20] = (flight >> 8) & 0xFF;
    out[4 + 21] = flight & 0xFF;
    return rlen;
}
// Write `data` (len bytes) to a tmpfile and call lastRecordFromLog via stdio.
static bool runParser(const uint8_t* data, size_t len,
                     uint32_t* outFlight, char* outSerial, size_t serialSz) {
    FILE* f = tmpfile();
    if (!f) return false;
    fwrite(data, 1, len, f);
    fflush(f);
    bool ok = lastRecordFromLog(stdio_read_at, f, len,
                                outFlight, outSerial, serialSz);
    fclose(f);
    return ok;
}

void test_lastRec_single_0x4C(void) {
    uint8_t buf[64];
    size_t n = makeRec(buf, 0x4C, 28, "EA500.000243", 1077);
    char s[13]; uint32_t fnum = 0;
    TEST_ASSERT_TRUE(runParser(buf, n, &fnum, s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT32(1077, fnum);
    TEST_ASSERT_EQUAL_STRING("EA500.000243", s);
}
void test_lastRec_single_0x0E(void) {
    uint8_t buf[64];
    size_t n = makeRec(buf, 0x0E, 32, "EA500.000179", 2090);
    char s[13]; uint32_t fnum = 0;
    TEST_ASSERT_TRUE(runParser(buf, n, &fnum, s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT32(2090, fnum);
    TEST_ASSERT_EQUAL_STRING("EA500.000179", s);
}
void test_lastRec_picks_last_record(void) {
    // Two records back to back; backward scan must hit the second one.
    uint8_t buf[128] = {};
    size_t n1 = makeRec(buf, 0x0E, 32, "EA500.000243", 1000);
    size_t n2 = makeRec(buf + n1, 0x4C, 28, "EA500.000243", 1099);
    char s[13]; uint32_t fnum = 0;
    TEST_ASSERT_TRUE(runParser(buf, n1 + n2, &fnum, s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT32(1099, fnum);
}
void test_lastRec_truncated_tail_rejected(void) {
    // Two valid records followed by a partial record that runs past EOF —
    // parser must reject the partial and return the LAST complete record.
    uint8_t buf[128] = {};
    size_t n1 = makeRec(buf, 0x4C, 28, "EA500.000243", 500);
    size_t n2 = makeRec(buf + n1, 0x4C, 28, "EA500.000243", 501);
    // Tack on an "EA 4C" sync + length that overshoots EOF (truncated tail).
    size_t off = n1 + n2;
    buf[off++] = 0xEA;
    buf[off++] = 0x4C;
    buf[off++] = 0x10;  // rlen = 0x1000 = 4096, no trailing data
    buf[off++] = 0x00;
    char s[13]; uint32_t fnum = 0;
    TEST_ASSERT_TRUE(runParser(buf, off, &fnum, s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT32(501, fnum);  // truncated tail rejected, last valid wins
}
void test_lastRec_false_positive_in_body(void) {
    // Embed an "EA 4C" sequence inside an earlier record's body, past the
    // serial+flight fields. The framing check (next byte == 0xEA or EOF)
    // plus length range should reject the false positive.
    uint8_t buf[128] = {};
    size_t n = makeRec(buf, 0x0E, 64, "EA500.000099", 42);
    // Inject 0xEA 0x4C 0x00 0x18 deep in the body (offset 30 = body[26]).
    // Looks like a length-24 record but its trailing byte is inside the
    // outer record, not 0xEA.
    buf[30] = 0xEA;
    buf[31] = 0x4C;
    buf[32] = 0x00;
    buf[33] = 0x18;
    char s[13]; uint32_t fnum = 0;
    TEST_ASSERT_TRUE(runParser(buf, n, &fnum, s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT32(42, fnum);
    TEST_ASSERT_EQUAL_STRING("EA500.000099", s);
}
void test_lastRec_no_records(void) {
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));  // no 0xEA bytes, no records
    char s[13] = "junk"; uint32_t fnum = 0;
    TEST_ASSERT_FALSE(runParser(buf, sizeof(buf), &fnum, s, sizeof(s)));
}
void test_lastRec_chunk_boundary(void) {
    // Place a record so its sync byte sits exactly at a 1024-byte chunk
    // boundary. Backward scan must still find it (overlap handles this).
    uint8_t buf[2048] = {};
    // Random-ish fill in the first 1022 bytes, then record straddling 1023.
    for (int i = 0; i < 1022; i++) buf[i] = (uint8_t)(i ^ 0x5A);
    // Ensure no accidental 0xEA in fill that would mislead.
    for (int i = 0; i < 1022; i++) if (buf[i] == 0xEA) buf[i] = 0x55;
    size_t recOff = 1023;  // sync byte at offset 1023, prefix straddles 1024
    size_t n = makeRec(buf + recOff, 0x4C, 28, "EA500.000214", 1284);
    char s[13]; uint32_t fnum = 0;
    TEST_ASSERT_TRUE(runParser(buf, recOff + n, &fnum, s, sizeof(s)));
    TEST_ASSERT_EQUAL_UINT32(1284, fnum);
    TEST_ASSERT_EQUAL_STRING("EA500.000214", s);
}

// ── Real-file integration tests ─────────────────────────────────────────────
// Run against actual DSU log files captured from aircraft. Skipped (passed
// with warning) if files not present locally.
struct RealLogCase { const char* path; const char* expectSerial; uint32_t expectFlight; };
static const RealLogCase REAL_LOGS[] = {
    { "/home/cedric/Downloads/EA500.000243_01077_20250301.eaofh", "EA500.000243", 1077 },
    { "/home/cedric/EA500/EA500.000179_02090_20480101.eaofh",     "EA500.000179", 2090 },
    { "/home/cedric/EA500/EA500.000214_01284_20480000.eaofh",     "EA500.000214", 1284 },
};
static void runRealLogCase(const RealLogCase& c) {
    FILE* f = fopen(c.path, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "skip: %s not present", c.path);
        TEST_IGNORE_MESSAGE(msg);
        return;
    }
    fseek(f, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftell(f);
    char serial[13] = {0};
    uint32_t fnum = 0;
    bool ok = lastRecordFromLog(stdio_read_at, f, fsize, &fnum, serial, sizeof(serial));
    fclose(f);
    TEST_ASSERT_TRUE_MESSAGE(ok, c.path);
    TEST_ASSERT_EQUAL_STRING(c.expectSerial, serial);
    TEST_ASSERT_EQUAL_UINT32(c.expectFlight, fnum);
}
void test_realLog_EA500_000243_01077(void) { runRealLogCase(REAL_LOGS[0]); }
void test_realLog_EA500_000179_02090(void) { runRealLogCase(REAL_LOGS[1]); }
void test_realLog_EA500_000214_01284(void) { runRealLogCase(REAL_LOGS[2]); }

// ── flattenPath ─────────────────────────────────────────────────────────────

void test_flattenPath_with_prefix(void) {
    char out[128];
    flattenPath("logs", "data.bin", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("logs__data.bin", out);
}
void test_flattenPath_no_prefix(void) {
    char out[128];
    flattenPath("", "data.bin", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("data.bin", out);
}
void test_flattenPath_nested(void) {
    char out[128];
    flattenPath("dir1__dir2", "file.csv", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("dir1__dir2__file.csv", out);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // dechunk
    RUN_TEST(test_dechunk_single);
    RUN_TEST(test_dechunk_multiple);
    RUN_TEST(test_dechunk_empty);
    RUN_TEST(test_dechunk_hex_size);
    RUN_TEST(test_dechunk_uppercase_hex);

    // crc16_8005
    RUN_TEST(test_crc16_deterministic);
    RUN_TEST(test_crc16_zero_length);
    RUN_TEST(test_crc16_different_data);

    // buildDsuCookie
    RUN_TEST(test_buildDsuCookie_magic);
    RUN_TEST(test_buildDsuCookie_serial);
    RUN_TEST(test_buildDsuCookie_flight_be);
    RUN_TEST(test_buildDsuCookie_mode_flag);
    RUN_TEST(test_buildDsuCookie_crc_validates);
    RUN_TEST(test_buildDsuCookie_small_flight);
    RUN_TEST(test_buildDsuCookie_golden_1210);

    // buildDsuCookieDate (date-mode)
    RUN_TEST(test_reverse_bits_8);
    RUN_TEST(test_to_bcd);
    RUN_TEST(test_encode_arinc_date_known);
    RUN_TEST(test_encode_arinc_date_parity_odd);
    RUN_TEST(test_buildDsuCookieDate_layout);
    RUN_TEST(test_buildDsuCookieDate_golden);

    // lastRecordFromLog (synthetic)
    RUN_TEST(test_lastRec_single_0x4C);
    RUN_TEST(test_lastRec_single_0x0E);
    RUN_TEST(test_lastRec_picks_last_record);
    RUN_TEST(test_lastRec_truncated_tail_rejected);
    RUN_TEST(test_lastRec_false_positive_in_body);
    RUN_TEST(test_lastRec_no_records);
    RUN_TEST(test_lastRec_chunk_boundary);

    // lastRecordFromLog (real DSU files — skipped if not present)
    RUN_TEST(test_realLog_EA500_000243_01077);
    RUN_TEST(test_realLog_EA500_000179_02090);
    RUN_TEST(test_realLog_EA500_000214_01284);

    // flattenPath
    RUN_TEST(test_flattenPath_with_prefix);
    RUN_TEST(test_flattenPath_no_prefix);
    RUN_TEST(test_flattenPath_nested);

    return UNITY_END();
}

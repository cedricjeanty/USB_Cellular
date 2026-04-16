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

// ── parseDsuFilename ────────────────────────────────────────────────────────

void test_parseDsuFilename_standard(void) {
    char serial[44];
    uint32_t fnum = 0;
    TEST_ASSERT_TRUE(parseDsuFilename("EA500.000243_01218_20260406.eaofh", serial, sizeof(serial), &fnum));
    TEST_ASSERT_EQUAL_STRING("EA500.000243", serial);
    TEST_ASSERT_EQUAL_UINT32(1218, fnum);
}
void test_parseDsuFilename_with_prefix(void) {
    char serial[44];
    uint32_t fnum = 0;
    TEST_ASSERT_TRUE(parseDsuFilename("flightHistory__EA500.000243_01218_20260406.eaofh", serial, sizeof(serial), &fnum));
    TEST_ASSERT_EQUAL_STRING("EA500.000243", serial);
    TEST_ASSERT_EQUAL_UINT32(1218, fnum);
}
void test_parseDsuFilename_no_ea(void) {
    char serial[44];
    uint32_t fnum = 0;
    TEST_ASSERT_FALSE(parseDsuFilename("random_file.eaofh", serial, sizeof(serial), &fnum));
}
void test_parseDsuFilename_high_flight(void) {
    char serial[44];
    uint32_t fnum = 0;
    TEST_ASSERT_TRUE(parseDsuFilename("EA500.000099_99999_20260501.eaofh", serial, sizeof(serial), &fnum));
    TEST_ASSERT_EQUAL_STRING("EA500.000099", serial);
    TEST_ASSERT_EQUAL_UINT32(99999, fnum);
}

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

    // parseDsuFilename
    RUN_TEST(test_parseDsuFilename_standard);
    RUN_TEST(test_parseDsuFilename_with_prefix);
    RUN_TEST(test_parseDsuFilename_no_ea);
    RUN_TEST(test_parseDsuFilename_high_flight);

    // flattenPath
    RUN_TEST(test_flattenPath_with_prefix);
    RUN_TEST(test_flattenPath_no_prefix);
    RUN_TEST(test_flattenPath_nested);

    return UNITY_END();
}

#include <unity.h>
#include "airbridge_utils.h"

void setUp(void) {}
void tearDown(void) {}

// ── versionNewer ────────────────────────────────────────────────────────────

void test_versionNewer_major_bump(void) {
    TEST_ASSERT_TRUE(versionNewer("2.0.0", "1.0.0"));
}
void test_versionNewer_minor_bump(void) {
    TEST_ASSERT_TRUE(versionNewer("1.1.0", "1.0.0"));
}
void test_versionNewer_patch_bump(void) {
    TEST_ASSERT_TRUE(versionNewer("1.0.1", "1.0.0"));
}
void test_versionNewer_equal(void) {
    TEST_ASSERT_FALSE(versionNewer("1.0.0", "1.0.0"));
}
void test_versionNewer_older_major(void) {
    TEST_ASSERT_FALSE(versionNewer("1.0.0", "2.0.0"));
}
void test_versionNewer_older_minor(void) {
    TEST_ASSERT_FALSE(versionNewer("1.0.0", "1.1.0"));
}
void test_versionNewer_real_firmware(void) {
    TEST_ASSERT_TRUE(versionNewer("10.2002.0", "10.2001.7"));
    TEST_ASSERT_FALSE(versionNewer("10.2001.7", "10.2001.7"));
    TEST_ASSERT_FALSE(versionNewer("10.2001.6", "10.2001.7"));
}

// ── jsonStr ─────────────────────────────────────────────────────────────────

void test_jsonStr_simple(void) {
    TEST_ASSERT_EQUAL_STRING("bar", jsonStr("{\"foo\":\"bar\"}", "foo").c_str());
}
void test_jsonStr_missing_key(void) {
    TEST_ASSERT_EQUAL_STRING("", jsonStr("{\"foo\":\"bar\"}", "baz").c_str());
}
void test_jsonStr_null_value(void) {
    TEST_ASSERT_EQUAL_STRING("", jsonStr("{\"foo\":null}", "foo").c_str());
}
void test_jsonStr_with_spaces(void) {
    TEST_ASSERT_EQUAL_STRING("hello", jsonStr("{\"key\" : \"hello\"}", "key").c_str());
}
void test_jsonStr_nested(void) {
    std::string json = "{\"version\":\"10.3.0\",\"size\":1024000,\"url\":\"https://example.com/fw.bin\"}";
    TEST_ASSERT_EQUAL_STRING("10.3.0", jsonStr(json, "version").c_str());
    TEST_ASSERT_EQUAL_STRING("https://example.com/fw.bin", jsonStr(json, "url").c_str());
}

// ── jsonInt ─────────────────────────────────────────────────────────────────

void test_jsonInt_positive(void) {
    TEST_ASSERT_EQUAL_INT(42, jsonInt("{\"count\":42}", "count"));
}
void test_jsonInt_missing(void) {
    TEST_ASSERT_EQUAL_INT(-1, jsonInt("{\"count\":42}", "total"));
}
void test_jsonInt_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, jsonInt("{\"n\":0}", "n"));
}
void test_jsonInt_large(void) {
    TEST_ASSERT_EQUAL_INT(1024000, jsonInt("{\"size\":1024000}", "size"));
}

// ── urlEncode ───────────────────────────────────────────────────────────────

void test_urlEncode_passthrough(void) {
    TEST_ASSERT_EQUAL_STRING("hello", urlEncode("hello").c_str());
}
void test_urlEncode_space(void) {
    TEST_ASSERT_EQUAL_STRING("hello%20world", urlEncode("hello world").c_str());
}
void test_urlEncode_special(void) {
    TEST_ASSERT_EQUAL_STRING("a%26b%3Dc", urlEncode("a&b=c").c_str());
}
void test_urlEncode_tilde_preserved(void) {
    TEST_ASSERT_EQUAL_STRING("~user", urlEncode("~user").c_str());
}
void test_urlEncode_slash(void) {
    TEST_ASSERT_EQUAL_STRING("path%2Fto%2Ffile", urlEncode("path/to/file").c_str());
}
void test_urlEncode_empty(void) {
    TEST_ASSERT_EQUAL_STRING("", urlEncode("").c_str());
}

// ── url_decode ──────────────────────────────────────────────────────────────

void test_url_decode_percent(void) {
    TEST_ASSERT_EQUAL_STRING("a&b", url_decode("a%26b", 5).c_str());
}
void test_url_decode_plus_to_space(void) {
    TEST_ASSERT_EQUAL_STRING("hello world", url_decode("hello+world", 11).c_str());
}
void test_url_decode_passthrough(void) {
    TEST_ASSERT_EQUAL_STRING("abc", url_decode("abc", 3).c_str());
}
void test_url_decode_empty(void) {
    TEST_ASSERT_EQUAL_STRING("", url_decode("", 0).c_str());
}

// ── form_field ──────────────────────────────────────────────────────────────

void test_form_field_first(void) {
    TEST_ASSERT_EQUAL_STRING("mynet", form_field("ssid=mynet&pass=secret", "ssid").c_str());
}
void test_form_field_last(void) {
    TEST_ASSERT_EQUAL_STRING("secret", form_field("ssid=mynet&pass=secret", "pass").c_str());
}
void test_form_field_missing(void) {
    TEST_ASSERT_EQUAL_STRING("", form_field("ssid=mynet", "pass").c_str());
}
void test_form_field_encoded_value(void) {
    TEST_ASSERT_EQUAL_STRING("my net", form_field("ssid=my+net&pass=x", "ssid").c_str());
}
void test_form_field_percent_encoded(void) {
    TEST_ASSERT_EQUAL_STRING("a&b", form_field("val=a%26b", "val").c_str());
}

// ── parseUrl ────────────────────────────────────────────────────────────────

void test_parseUrl_full(void) {
    char host[64], path[128];
    TEST_ASSERT_TRUE(parseUrl("https://example.com/api/v1", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL_STRING("/api/v1", path);
}
void test_parseUrl_no_path(void) {
    char host[64], path[128];
    TEST_ASSERT_TRUE(parseUrl("https://example.com", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL_STRING("/", path);
}
void test_parseUrl_no_scheme(void) {
    char host[64], path[128];
    TEST_ASSERT_FALSE(parseUrl("example.com/foo", host, sizeof(host), path, sizeof(path)));
}
void test_parseUrl_deep_path(void) {
    char host[64], path[256];
    TEST_ASSERT_TRUE(parseUrl("https://api.example.com/prod/presign?action=upload", host, sizeof(host), path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("api.example.com", host);
    TEST_ASSERT_EQUAL_STRING("/prod/presign?action=upload", path);
}

// ── rssiToBars ──────────────────────────────────────────────────────────────

void test_rssiToBars_strong(void) {
    TEST_ASSERT_EQUAL_INT8(4, rssiToBars(-50));
    TEST_ASSERT_EQUAL_INT8(4, rssiToBars(-55));
}
void test_rssiToBars_good(void) {
    TEST_ASSERT_EQUAL_INT8(3, rssiToBars(-56));
    TEST_ASSERT_EQUAL_INT8(3, rssiToBars(-67));
}
void test_rssiToBars_fair(void) {
    TEST_ASSERT_EQUAL_INT8(2, rssiToBars(-68));
    TEST_ASSERT_EQUAL_INT8(2, rssiToBars(-80));
}
void test_rssiToBars_weak(void) {
    TEST_ASSERT_EQUAL_INT8(1, rssiToBars(-81));
    TEST_ASSERT_EQUAL_INT8(1, rssiToBars(-90));
}
void test_rssiToBars_none(void) {
    TEST_ASSERT_EQUAL_INT8(0, rssiToBars(-91));
    TEST_ASSERT_EQUAL_INT8(0, rssiToBars(-120));
}

// ── isSkipped ───────────────────────────────────────────────────────────────

void test_isSkipped_dot_prefix(void) {
    TEST_ASSERT_TRUE(isSkipped(".hidden"));
    TEST_ASSERT_TRUE(isSkipped(".Spotlight-V100"));
}
void test_isSkipped_tilde_prefix(void) {
    TEST_ASSERT_TRUE(isSkipped("~temp"));
}
void test_isSkipped_known_names(void) {
    TEST_ASSERT_TRUE(isSkipped("Thumbs.db"));
    TEST_ASSERT_TRUE(isSkipped("desktop.ini"));
    TEST_ASSERT_TRUE(isSkipped("System Volume Information"));
    TEST_ASSERT_TRUE(isSkipped("harvested"));
    TEST_ASSERT_TRUE(isSkipped("airbridge.log"));
}
void test_isSkipped_normal_files(void) {
    TEST_ASSERT_FALSE(isSkipped("data.csv"));
    TEST_ASSERT_FALSE(isSkipped("EA500.000243_01218_20260406.eaofh"));
    TEST_ASSERT_FALSE(isSkipped("flight_log.bin"));
}

// ── _fmtSize ────────────────────────────────────────────────────────────────

void test_fmtSize_gigabytes(void) {
    char buf[32];
    _fmtSize(buf, sizeof(buf), 2048.0f);
    TEST_ASSERT_EQUAL_STRING("2.0GB", buf);
}
void test_fmtSize_megabytes(void) {
    char buf[32];
    _fmtSize(buf, sizeof(buf), 45.6f);
    TEST_ASSERT_EQUAL_STRING("45.6MB", buf);
}
void test_fmtSize_kilobytes(void) {
    char buf[32];
    _fmtSize(buf, sizeof(buf), 0.05f);
    TEST_ASSERT_EQUAL_STRING("51.2KB", buf);
}
void test_fmtSize_boundary_gb(void) {
    char buf[32];
    // 1000.0f >= 1000.0f → formats as GB: 1000/1024 ≈ 0.977 → "1.0GB"
    _fmtSize(buf, sizeof(buf), 1000.0f);
    TEST_ASSERT_EQUAL_STRING("1.0GB", buf);
}
void test_fmtSize_boundary_mb(void) {
    char buf[32];
    _fmtSize(buf, sizeof(buf), 0.1f);
    TEST_ASSERT_EQUAL_STRING("0.1MB", buf);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // versionNewer
    RUN_TEST(test_versionNewer_major_bump);
    RUN_TEST(test_versionNewer_minor_bump);
    RUN_TEST(test_versionNewer_patch_bump);
    RUN_TEST(test_versionNewer_equal);
    RUN_TEST(test_versionNewer_older_major);
    RUN_TEST(test_versionNewer_older_minor);
    RUN_TEST(test_versionNewer_real_firmware);

    // jsonStr
    RUN_TEST(test_jsonStr_simple);
    RUN_TEST(test_jsonStr_missing_key);
    RUN_TEST(test_jsonStr_null_value);
    RUN_TEST(test_jsonStr_with_spaces);
    RUN_TEST(test_jsonStr_nested);

    // jsonInt
    RUN_TEST(test_jsonInt_positive);
    RUN_TEST(test_jsonInt_missing);
    RUN_TEST(test_jsonInt_zero);
    RUN_TEST(test_jsonInt_large);

    // urlEncode
    RUN_TEST(test_urlEncode_passthrough);
    RUN_TEST(test_urlEncode_space);
    RUN_TEST(test_urlEncode_special);
    RUN_TEST(test_urlEncode_tilde_preserved);
    RUN_TEST(test_urlEncode_slash);
    RUN_TEST(test_urlEncode_empty);

    // url_decode
    RUN_TEST(test_url_decode_percent);
    RUN_TEST(test_url_decode_plus_to_space);
    RUN_TEST(test_url_decode_passthrough);
    RUN_TEST(test_url_decode_empty);

    // form_field
    RUN_TEST(test_form_field_first);
    RUN_TEST(test_form_field_last);
    RUN_TEST(test_form_field_missing);
    RUN_TEST(test_form_field_encoded_value);
    RUN_TEST(test_form_field_percent_encoded);

    // parseUrl
    RUN_TEST(test_parseUrl_full);
    RUN_TEST(test_parseUrl_no_path);
    RUN_TEST(test_parseUrl_no_scheme);
    RUN_TEST(test_parseUrl_deep_path);

    // rssiToBars
    RUN_TEST(test_rssiToBars_strong);
    RUN_TEST(test_rssiToBars_good);
    RUN_TEST(test_rssiToBars_fair);
    RUN_TEST(test_rssiToBars_weak);
    RUN_TEST(test_rssiToBars_none);

    // isSkipped
    RUN_TEST(test_isSkipped_dot_prefix);
    RUN_TEST(test_isSkipped_tilde_prefix);
    RUN_TEST(test_isSkipped_known_names);
    RUN_TEST(test_isSkipped_normal_files);

    // _fmtSize
    RUN_TEST(test_fmtSize_gigabytes);
    RUN_TEST(test_fmtSize_megabytes);
    RUN_TEST(test_fmtSize_kilobytes);
    RUN_TEST(test_fmtSize_boundary_gb);
    RUN_TEST(test_fmtSize_boundary_mb);

    return UNITY_END();
}

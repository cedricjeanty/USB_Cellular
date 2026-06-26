// Tests for the RAM ring buffer that backs SD-independent log egress.
//
// When the SD card is corrupt/unmounted the firmware can't read the SD log
// file, so it POSTs the RAM ring buffer (airbridge_log.h) straight to S3 and
// then consumes exactly the bytes it sent. These tests pin that contract:
//   - snapshot reflects what was logged (no SD touched)
//   - consume(n) drops only the first n bytes, keeping newer lines
//   - a successful "upload" (snapshot → consume) egresses each line once
//   - a failed upload (snapshot, no consume) re-sends the same bytes next time
//   - the buffer keeps the NEWEST data when it overflows (oldest dropped)

#include <unity.h>
#include <cstring>
#include <string>
#include "airbridge_log.h"

static uint32_t s_fake_ms = 0;
static uint32_t fake_uptime() { return s_fake_ms; }
static void null_serial(const char*, int) {}

void setUp(void) {
    airbridge_log_init(null_serial, fake_uptime);
    airbridge_log_clear();
    s_fake_ms = 0;
}
void tearDown(void) {}

// Snapshot returns the logged bytes without any filesystem access.
void test_snapshot_reflects_logged_lines(void) {
    airbridge_log("hello %d", 1);
    airbridge_log("world");
    char buf[256] = {};
    int n = airbridge_log_snapshot(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "hello 1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "world"));
}

// consume(n) drops the first n bytes; later lines remain.
void test_consume_drops_front_keeps_rest(void) {
    airbridge_log("AAAA");           // becomes a line with a timestamp prefix
    char before[256] = {};
    int n1 = airbridge_log_snapshot(before, sizeof(before));
    // Find the end of the first line (inclusive of newline).
    char* nl = strchr(before, '\n');
    TEST_ASSERT_NOT_NULL(nl);
    int firstLen = (int)(nl - before) + 1;

    airbridge_log("BBBB");           // appended after AAAA's line

    airbridge_log_consume(firstLen); // drop only the AAAA line
    char after[256] = {};
    int n2 = airbridge_log_snapshot(after, sizeof(after));
    TEST_ASSERT_TRUE(n2 > 0 && n2 < n1 + 32);
    TEST_ASSERT_NULL_MESSAGE(strstr(after, "AAAA"), "consumed line must be gone");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(after, "BBBB"), "newer line must remain");
}

// A successful upload cycle (snapshot then consume snapLen) egresses each line
// exactly once: after consuming, a fresh snapshot of a quiet buffer is empty.
void test_successful_upload_egresses_once(void) {
    airbridge_log("line one");
    airbridge_log("line two");
    char snap[256] = {};
    int n = airbridge_log_snapshot(snap, sizeof(snap));
    TEST_ASSERT_GREATER_THAN(0, n);
    airbridge_log_consume(n);        // simulate POST success

    char again[256] = {};
    int n2 = airbridge_log_snapshot(again, sizeof(again));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n2, "nothing left to re-send after a full consume");
}

// A failed upload (snapshot but NO consume) leaves the bytes in place so the
// next cycle re-sends them — no data lost when the link is down.
void test_failed_upload_resends_same_bytes(void) {
    airbridge_log("important event");
    char snap1[256] = {};
    int n1 = airbridge_log_snapshot(snap1, sizeof(snap1));
    // (POST fails — do NOT consume)
    char snap2[256] = {};
    int n2 = airbridge_log_snapshot(snap2, sizeof(snap2));
    TEST_ASSERT_EQUAL_INT(n1, n2);
    TEST_ASSERT_EQUAL_STRING(snap1, snap2);
    TEST_ASSERT_NOT_NULL(strstr(snap2, "important event"));
}

// consume(0) and consume(>len) are safe (no-op / full clear).
void test_consume_bounds(void) {
    airbridge_log("data");
    char buf[128] = {};
    int n = airbridge_log_snapshot(buf, sizeof(buf));
    airbridge_log_consume(0);        // no-op
    char b2[128] = {};
    TEST_ASSERT_EQUAL_INT(n, airbridge_log_snapshot(b2, sizeof(b2)));
    airbridge_log_consume(100000);   // over-consume => clear
    char b3[128] = {};
    TEST_ASSERT_EQUAL_INT(0, airbridge_log_snapshot(b3, sizeof(b3)));
}

// On overflow the buffer keeps the NEWEST lines (oldest discarded), so a
// degraded unit still reports its most recent state, not stale history.
void test_overflow_keeps_newest(void) {
    for (int i = 0; i < 2000; i++) airbridge_log("msg number %d padding padding", i);
    char buf[AIRBRIDGE_LOG_BUF_SIZE + 16] = {};
    int n = airbridge_log_snapshot(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n <= AIRBRIDGE_LOG_BUF_SIZE);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "msg number 1999"), "newest line must be present");
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "msg number 0 "), "oldest line must be dropped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_snapshot_reflects_logged_lines);
    RUN_TEST(test_consume_drops_front_keeps_rest);
    RUN_TEST(test_successful_upload_egresses_once);
    RUN_TEST(test_failed_upload_resends_same_bytes);
    RUN_TEST(test_consume_bounds);
    RUN_TEST(test_overflow_keeps_newest);
    return UNITY_END();
}

#include <unity.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include "sim_dsu.h"
#include "airbridge_proto.h"

static const char* TEST_SD = "/tmp/test_dsu_sd";

void setUp(void) {
    // Clean test directory
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_SD, TEST_SD);
    system(cmd);
}

void tearDown(void) {}

// ── Cookie reading ──────────────────────────────────────────────────────────

void test_read_cookie_none(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(0, dsu.readCookie());
}

void test_read_cookie_valid(void) {
    // Write a valid cookie with flight 1218
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1218, cookie);
    char path[256];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(path, "wb");
    fwrite(cookie, 1, 78, f);
    fclose(f);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(1218, dsu.readCookie());
}

void test_read_cookie_bad_magic(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(path, "wb");
    uint8_t bad[78] = {};
    bad[0] = 0xFF;  // wrong magic
    fwrite(bad, 1, 78, f);
    fclose(f);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(0, dsu.readCookie());
}

// ── Session writing ─────────────────────────────────────────────────────────

void test_session_generates_files(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.nextFlight = 1050;

    SimDSU::SessionResult r = dsu.runSession(1.0f);  // 1MB
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT32(1050, r.flightNum);
    TEST_ASSERT_TRUE(r.bytesWritten > 0);

    // Check flight history file exists
    char fhPath[384];
    snprintf(fhPath, sizeof(fhPath), "%s/flightHistory/%s", TEST_SD, r.filename);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(fhPath, &st));
    TEST_ASSERT_EQUAL_UINT32(1048576, st.st_size);  // 1MB

    // Check filename format
    TEST_ASSERT_TRUE(strstr(r.filename, "EA500.000243") != nullptr);
    TEST_ASSERT_TRUE(strstr(r.filename, "_01050_") != nullptr);
    TEST_ASSERT_TRUE(strstr(r.filename, ".eaofh") != nullptr);
}

void test_session_writes_metrics(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.runSession(0.1f);

    char path[384];
    struct stat st;

    // dsuMetric.eacmf (empty)
    snprintf(path, sizeof(path), "%s/metrics/dsuMetric.eacmf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_EQUAL_INT(0, st.st_size);

    // dsuMetric.1.eacmf (~20KB)
    snprintf(path, sizeof(path), "%s/metrics/dsuMetric.1.eacmf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_TRUE(st.st_size > 0);

    // dsuUsage.eacuf
    snprintf(path, sizeof(path), "%s/metrics/dsuUsage.eacuf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_TRUE(st.st_size > 0);
}

void test_session_writes_report(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.runSession(0.1f);

    char path[384];
    snprintf(path, sizeof(path), "%s/downloadReport.txt", TEST_SD);
    FILE* f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(f);
    char buf[1024] = "";
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    TEST_ASSERT_TRUE(strstr(buf, "Gps date") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Download Report") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, ".eaofh") != nullptr);
}

// ── Cookie-based resumption ─────────────────────────────────────────────────

void test_session_respects_cookie(void) {
    // Write cookie pointing to flight 1200
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1200, cookie);
    char cookiePath[256];
    snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(cookiePath, "wb");
    fwrite(cookie, 1, 78, f);
    fclose(f);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.flightIncrement = 10;
    dsu.nextFlight = 999;  // should be overridden by cookie

    SimDSU::SessionResult r = dsu.runSession(0.1f);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT32(1210, r.flightNum);  // 1200 + 10
}

void test_multi_session_cycle(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.nextFlight = 1000;
    dsu.flightIncrement = 5;

    // Session 1
    SimDSU::SessionResult r1 = dsu.runSession(0.1f);
    TEST_ASSERT_TRUE(r1.success);
    TEST_ASSERT_EQUAL_UINT32(1000, r1.flightNum);

    // Simulate firmware writing cookie after harvest (like real device)
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", r1.flightNum, cookie);
    char cookiePath[256];
    snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(cookiePath, "wb");
    fwrite(cookie, 1, 78, f);
    fclose(f);

    // Session 2 — should read cookie and increment
    SimDSU dsu2;
    dsu2.sdRoot = TEST_SD;
    dsu2.flightIncrement = 5;
    SimDSU::SessionResult r2 = dsu2.runSession(0.1f);
    TEST_ASSERT_TRUE(r2.success);
    TEST_ASSERT_EQUAL_UINT32(1005, r2.flightNum);

    // Both flight files should exist
    char p1[384], p2[384];
    snprintf(p1, sizeof(p1), "%s/flightHistory/%s", TEST_SD, r1.filename);
    snprintf(p2, sizeof(p2), "%s/flightHistory/%s", TEST_SD, r2.filename);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(p1, &st));
    TEST_ASSERT_EQUAL_INT(0, stat(p2, &st));
}

void test_metrics_overwritten(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.nextFlight = 1000;

    dsu.runSession(0.1f);

    // Get metric file size
    char path[384];
    snprintf(path, sizeof(path), "%s/metrics/dsuMetric.1.eacmf", TEST_SD);
    struct stat st1;
    stat(path, &st1);

    // Run second session — metrics should be overwritten (same size)
    dsu.runSession(0.1f);
    struct stat st2;
    stat(path, &st2);

    // Size should be the same (overwritten, not appended)
    TEST_ASSERT_EQUAL(st1.st_size, st2.st_size);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Cookie
    RUN_TEST(test_read_cookie_none);
    RUN_TEST(test_read_cookie_valid);
    RUN_TEST(test_read_cookie_bad_magic);

    // Session
    RUN_TEST(test_session_generates_files);
    RUN_TEST(test_session_writes_metrics);
    RUN_TEST(test_session_writes_report);

    // Cookie cycle
    RUN_TEST(test_session_respects_cookie);
    RUN_TEST(test_multi_session_cycle);
    RUN_TEST(test_metrics_overwritten);

    return UNITY_END();
}

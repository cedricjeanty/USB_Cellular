#include <unity.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "sim_dsu.h"
#include "airbridge_proto.h"

static const char* TEST_SD = "/tmp/test_dsu_sd";

// Fixture path relative to this file: esp32/test/fixtures/
static std::string fixtureDir() {
    std::string p = __FILE__;
    auto pos = p.rfind('/');
    if (pos != std::string::npos) p = p.substr(0, pos);
    pos = p.rfind('/');
    if (pos != std::string::npos) p = p.substr(0, pos);
    return p + "/fixtures/";
}
static std::string shortFixture() {
    return fixtureDir() + "EA500.000243_01077.eaofh";
}
static std::string fullFixture() {
    return fixtureDir() + "EA500.000243_01071.eaofh";
}

void setUp(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", TEST_SD, TEST_SD);
    system(cmd);
}

void tearDown(void) {}

// ── Helper ──────────────────────────────────────────────────────────────────

static void writeCookie(uint32_t flight, const char* sn = "EA500.000243") {
    uint8_t cookie[78];
    buildDsuCookie(sn, flight, cookie);
    char path[256];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(path, "wb");
    fwrite(cookie, 1, 78, f);
    fclose(f);
}

// ── Cookie reading ──────────────────────────────────────────────────────────

void test_read_cookie_none(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(0, dsu.readCookie());
}

void test_read_cookie_valid(void) {
    writeCookie(1218);
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(1218, dsu.readCookie());
}

void test_read_cookie_bad_magic(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(path, "wb");
    uint8_t bad[78] = {};
    bad[0] = 0xFF;
    fwrite(bad, 1, 78, f);
    fclose(f);
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(0, dsu.readCookie());
}

void test_read_cookie_bad_crc(void) {
    uint8_t cookie[78];
    buildDsuCookie("EA500.000243", 1200, cookie);
    cookie[77] ^= 0xFF;  // corrupt CRC
    char path[256];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(path, "wb");
    fwrite(cookie, 1, 78, f);
    fclose(f);
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    TEST_ASSERT_EQUAL_UINT32(0, dsu.readCookie());
}

void test_read_cookie_date_mode(void) {
    uint8_t cookie[78];
    buildDsuCookieDate("EA500.000243", 2026, 4, 6, cookie);
    char path[256];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", TEST_SD);
    FILE* f = fopen(path, "wb");
    fwrite(cookie, 1, 78, f);
    fclose(f);
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    // Date-mode cookies are treated as "no cookie"
    TEST_ASSERT_EQUAL_UINT32(0, dsu.readCookie());
}

// ── Flight index ─────────────────────────────────────────────────────────────

void test_build_flight_index_empty_path(void) {
    auto idx = buildFlightIndex(nullptr);
    TEST_ASSERT_TRUE(idx.empty());
}

void test_build_flight_index_nonexistent(void) {
    auto idx = buildFlightIndex("/nonexistent/path.eaofh");
    TEST_ASSERT_TRUE(idx.empty());
}

void test_build_flight_index_short_fixture(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) {
        TEST_IGNORE_MESSAGE("short fixture not available");
        return;
    }
    auto idx = buildFlightIndex(path.c_str());
    TEST_ASSERT_TRUE_MESSAGE(!idx.empty(), "index should not be empty");

    // Entries must be in non-decreasing flight order (a flight can have multiple
    // 0x4C records — the DSU appends partial summaries as the flight progresses)
    for (size_t i = 1; i < idx.size(); i++) {
        TEST_ASSERT_TRUE(idx[i].flight >= idx[i-1].flight);
    }

    // blockStart of first entry is 0; each blockStart equals previous blockEnd
    TEST_ASSERT_EQUAL_UINT64(0, idx[0].blockStart);
    for (size_t i = 1; i < idx.size(); i++) {
        TEST_ASSERT_EQUAL_UINT64(idx[i-1].blockEnd, idx[i].blockStart);
    }

    // All serials should be non-empty
    for (auto& e : idx)
        TEST_ASSERT_TRUE(e.serial[0] != '\0');

    // Last block should end exactly at end of file
    struct stat st;
    stat(path.c_str(), &st);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)st.st_size, idx.back().blockEnd);

    printf("[test] Short fixture: %zu records, flights %u-%u\n",
           idx.size(), idx.front().flight, idx.back().flight);
}

// ── Session: no internal memory ──────────────────────────────────────────────

void test_session_no_internal_memory(void) {
    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    // internalMemoryPath is null
    SimDSU::SessionResult r = dsu.runSession();
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_EQUAL_UINT32(0, r.bytesWritten);
}

// ── Session: full download (no cookie) ──────────────────────────────────────

void test_session_no_cookie(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }

    auto idx = buildFlightIndex(path.c_str());
    if (idx.empty()) { TEST_IGNORE_MESSAGE("empty index"); return; }

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.writeSpeedKBps = 0;  // no throttle in tests
    dsu.internalMemoryPath = path.c_str();

    SimDSU::SessionResult r = dsu.runSession();
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_TRUE(r.bytesWritten > 0);
    TEST_ASSERT_EQUAL_UINT32(idx.front().flight, r.firstFlight);
    TEST_ASSERT_EQUAL_UINT32(idx.back().flight, r.flightNum);

    // Output file exists
    char fhPath[384];
    snprintf(fhPath, sizeof(fhPath), "%s/flightHistory/%s", TEST_SD, r.filename);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(fhPath, &st));

    // lastRecordFromLog agrees with the index on the last flight
    FILE* ff = fopen(fhPath, "rb");
    fseek(ff, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftell(ff);
    fseek(ff, 0, SEEK_SET);
    uint32_t lastFlight = 0;
    char serial[13];
    bool parsed = lastRecordFromLog(stdio_read_at, ff, fsize, &lastFlight, serial, sizeof(serial));
    fclose(ff);
    TEST_ASSERT_TRUE(parsed);
    TEST_ASSERT_EQUAL_UINT32(idx.back().flight, lastFlight);
}

// ── Session: partial download with cookie ────────────────────────────────────

void test_session_with_cookie(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }

    auto idx = buildFlightIndex(path.c_str());
    if (idx.size() < 2) { TEST_IGNORE_MESSAGE("fixture has < 2 flights"); return; }

    // Cookie points to first flight — session should download the rest
    writeCookie(idx.front().flight);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.writeSpeedKBps = 0;
    dsu.internalMemoryPath = path.c_str();

    SimDSU::SessionResult r = dsu.runSession();
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_TRUE(r.bytesWritten > 0);
    // firstFlight must be AFTER the cookie flight
    TEST_ASSERT_TRUE(r.firstFlight > idx.front().flight);
    TEST_ASSERT_EQUAL_UINT32(idx.back().flight, r.flightNum);

    // Verify output last record matches
    char fhPath[384];
    snprintf(fhPath, sizeof(fhPath), "%s/flightHistory/%s", TEST_SD, r.filename);
    FILE* ff = fopen(fhPath, "rb");
    fseek(ff, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftell(ff);
    fseek(ff, 0, SEEK_SET);
    uint32_t lastFlight = 0;
    char serial[13];
    lastRecordFromLog(stdio_read_at, ff, fsize, &lastFlight, serial, sizeof(serial));
    fclose(ff);
    TEST_ASSERT_EQUAL_UINT32(idx.back().flight, lastFlight);
}

// ── Session: maxFlight limits output ─────────────────────────────────────────

void test_session_with_max_flight(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }

    auto idx = buildFlightIndex(path.c_str());
    if (idx.size() < 2) { TEST_IGNORE_MESSAGE("fixture has < 2 flights"); return; }

    uint32_t midFlight = idx[idx.size() / 2].flight;

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.writeSpeedKBps = 0;
    dsu.internalMemoryPath = path.c_str();
    dsu.maxFlight = midFlight;

    SimDSU::SessionResult r = dsu.runSession();
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_TRUE(r.bytesWritten > 0);
    TEST_ASSERT_EQUAL_UINT32(midFlight, r.flightNum);

    // last record in output must be midFlight
    char fhPath[384];
    snprintf(fhPath, sizeof(fhPath), "%s/flightHistory/%s", TEST_SD, r.filename);
    FILE* ff = fopen(fhPath, "rb");
    fseek(ff, 0, SEEK_END);
    uint64_t fsize = (uint64_t)ftell(ff);
    fseek(ff, 0, SEEK_SET);
    uint32_t lastFlight = 0;
    char serial[13];
    lastRecordFromLog(stdio_read_at, ff, fsize, &lastFlight, serial, sizeof(serial));
    fclose(ff);
    TEST_ASSERT_EQUAL_UINT32(midFlight, lastFlight);
}

// ── Session: caught up (cookie == last flight) ───────────────────────────────

void test_session_caught_up(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }

    auto idx = buildFlightIndex(path.c_str());
    if (idx.empty()) { TEST_IGNORE_MESSAGE("empty index"); return; }

    writeCookie(idx.back().flight);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.writeSpeedKBps = 0;
    dsu.internalMemoryPath = path.c_str();

    SimDSU::SessionResult r = dsu.runSession();
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT32(0, r.bytesWritten);
    TEST_ASSERT_EQUAL_UINT32(0, r.flightNum);

    // No flight history file should exist
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/flightHistory", TEST_SD);
    struct stat st;
    // Either directory doesn't exist, or it's empty
    if (stat(dir, &st) == 0) {
        char pattern[384];
        snprintf(pattern, sizeof(pattern), "ls %s/*.eaofh 2>/dev/null | wc -l", dir);
        FILE* pipe = popen(pattern, "r");
        char result[16] = "0";
        fread(result, 1, sizeof(result) - 1, pipe);
        pclose(pipe);
        TEST_ASSERT_EQUAL_INT(0, atoi(result));
    }
}

// ── Session: incremental (two sessions cover all flights) ────────────────────

void test_session_incremental(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }

    auto idx = buildFlightIndex(path.c_str());
    if (idx.size() < 2) { TEST_IGNORE_MESSAGE("fixture has < 2 flights"); return; }

    uint32_t midFlight = idx[idx.size() / 2].flight;

    // Session 1: download up to midFlight
    SimDSU dsu1;
    dsu1.sdRoot = TEST_SD;
    dsu1.writeSpeedKBps = 0;
    dsu1.internalMemoryPath = path.c_str();
    dsu1.maxFlight = midFlight;
    SimDSU::SessionResult r1 = dsu1.runSession();
    TEST_ASSERT_TRUE(r1.success);
    TEST_ASSERT_EQUAL_UINT32(midFlight, r1.flightNum);

    // Simulate firmware writing cookie after harvest
    writeCookie(r1.flightNum, idx.back().serial);

    // Session 2: download remainder
    SimDSU dsu2;
    dsu2.sdRoot = TEST_SD;
    dsu2.writeSpeedKBps = 0;
    dsu2.internalMemoryPath = path.c_str();
    SimDSU::SessionResult r2 = dsu2.runSession();
    TEST_ASSERT_TRUE(r2.success);
    TEST_ASSERT_TRUE(r2.bytesWritten > 0);
    TEST_ASSERT_EQUAL_UINT32(idx.back().flight, r2.flightNum);
    TEST_ASSERT_TRUE(r2.firstFlight > midFlight);

    // Combined: sessions cover all flights
    TEST_ASSERT_EQUAL_UINT32(idx.front().flight, r1.firstFlight);
    TEST_ASSERT_EQUAL_UINT32(idx.back().flight, r2.flightNum);
}

// ── Session: metrics and report are written ──────────────────────────────────

void test_session_writes_metrics(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }
    std::string mdir = fixtureDir() + "metrics";

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.writeSpeedKBps = 0;
    dsu.internalMemoryPath = path.c_str();
    dsu.metricsSource = mdir.c_str();
    dsu.runSession();

    char p[384];
    struct stat st;

    // dsuMetric.eacmf (non-numbered, 20215 bytes)
    snprintf(p, sizeof(p), "%s/metrics/dsuMetric.eacmf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(20215, st.st_size);

    // dsuMetric.1 (20215 bytes)
    snprintf(p, sizeof(p), "%s/metrics/dsuMetric.1.eacmf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(20215, st.st_size);

    // dsuMetric.2 (0 bytes)
    snprintf(p, sizeof(p), "%s/metrics/dsuMetric.2.eacmf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(0, st.st_size);

    // dsuMetric.4 must NOT exist
    snprintf(p, sizeof(p), "%s/metrics/dsuMetric.4.eacmf", TEST_SD);
    TEST_ASSERT_NOT_EQUAL(0, stat(p, &st));

    // dsuMetric.7 (20215 bytes — real DSU has this)
    snprintf(p, sizeof(p), "%s/metrics/dsuMetric.7.eacmf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(20215, st.st_size);

    // dsuUsage.eacuf (640 bytes)
    snprintf(p, sizeof(p), "%s/metrics/dsuUsage.eacuf", TEST_SD);
    TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
    TEST_ASSERT_EQUAL_INT(640, st.st_size);
}

void test_session_writes_report(void) {
    std::string path = shortFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("fixture not available"); return; }

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.writeSpeedKBps = 0;
    dsu.internalMemoryPath = path.c_str();
    dsu.runSession();

    char p[384];
    snprintf(p, sizeof(p), "%s/downloadReport.txt", TEST_SD);
    FILE* f = fopen(p, "r");
    TEST_ASSERT_NOT_NULL(f);
    char buf[2048] = "";
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    // Matches real DSU report format
    TEST_ASSERT_TRUE(strstr(buf, "Gps date(yyyymmdd) and time(HHmmss)") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Downloading usage & metric to /mnt/memStick/metrics/") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Downloading FH to /mnt/memStick/flightHistory/") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Estimated download file size") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Download Report:") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Size :") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, "Duration :") != nullptr);
    TEST_ASSERT_TRUE(strstr(buf, ".eaofh") != nullptr);
}

// ── Full fixture tests (gated on DSU_FULL env var) ───────────────────────────

void test_build_flight_index_full_fixture(void) {
    if (!getenv("DSU_FULL")) { TEST_IGNORE_MESSAGE("set DSU_FULL=1 to run"); return; }
    std::string path = fullFixture();
    if (access(path.c_str(), R_OK) != 0) { TEST_IGNORE_MESSAGE("full fixture not available"); return; }

    auto idx = buildFlightIndex(path.c_str());
    TEST_ASSERT_TRUE(!idx.empty());
    for (size_t i = 1; i < idx.size(); i++)
        TEST_ASSERT_TRUE(idx[i].flight >= idx[i-1].flight);

    struct stat st;
    stat(path.c_str(), &st);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)st.st_size, idx.back().blockEnd);

    printf("[test] Full fixture: %zu records, flights %u-%u\n",
           idx.size(), idx.front().flight, idx.back().flight);
}

// ── Test runner ──────────────────────────────────────────────────────────────

// ── Synthetic backlog + per-flight transfer (flight_cycle_test model) ────────
// Mirror scripts/flight_cycle_test.sh append_flight: <size_kb> of zero filler
// (no stray 0xEA) then a 0x4C footer carrying the flight number. The block
// buildFlightIndex assigns to flight N = [prev footer end, this footer end), so
// the transferred slice ends at footer N (lastRecordFromLog → N).
static void appendSyntheticFlight(const char* path, uint32_t flight, uint32_t kb,
                                  const char* sn = "EA500.000243") {
    FILE* f = fopen(path, "ab");
    std::vector<uint8_t> fill(kb * 1024, 0);
    fwrite(fill.data(), 1, fill.size(), f);
    uint8_t rec[28] = {0xEA, 0x4C, 0x00, 0x1C};
    for (int i = 0; i < 12 && sn[i]; i++) rec[4 + 5 + i] = (uint8_t)sn[i];  // body[5:17]
    rec[4 + 20] = (flight >> 8) & 0xFF;                                     // body[20:22]
    rec[4 + 21] = flight & 0xFF;
    fwrite(rec, 1, 28, f);
    fclose(f);
}

void test_synthetic_memory_roundtrip(void) {
    char mem[256];
    snprintf(mem, sizeof(mem), "%s/internal.eaofh", TEST_SD);
    ::remove(mem);
    for (uint32_t i = 1; i <= 5; i++) appendSyntheticFlight(mem, i, 8);
    auto idx = buildFlightIndex(mem);
    TEST_ASSERT_EQUAL_UINT32(5, idx.size());
    for (uint32_t i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_UINT32(i + 1, idx[i].flight);
    // Each block ≈ 8KB filler + 28B footer, contiguous & covering the file.
    TEST_ASSERT_EQUAL_UINT64(0, idx.front().blockStart);
    TEST_ASSERT_GREATER_THAN_UINT64(8 * 1024, idx[0].blockEnd - idx[0].blockStart);
}

void test_transfer_flight_atomic_and_content(void) {
    char mem[256];
    snprintf(mem, sizeof(mem), "%s/internal.eaofh", TEST_SD);
    ::remove(mem);
    for (uint32_t i = 1; i <= 3; i++) appendSyntheticFlight(mem, i, 16);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.internalMemoryPath = mem;
    dsu.writeSpeedKBps = 0;  // no throttle in the unit test

    SimDSU::SessionResult r = dsu.transferFlight(2);
    TEST_ASSERT_TRUE(r.success);
    TEST_ASSERT_EQUAL_UINT32(2, r.flightNum);
    TEST_ASSERT_GREATER_THAN_UINT32(15 * 1024, r.bytesWritten);

    // Non-atomic DSU: the flight is written DIRECTLY to its .eaofh (no temp), so a
    // clean transfer leaves no .part — and neither would an interrupted one (it would
    // leave a TRUNCATED .eaofh instead; see test_transfer_flight_interrupted).
    char fhDir[300];
    snprintf(fhDir, sizeof(fhDir), "%s/flightHistory", TEST_SD);
    char findPart[400];
    snprintf(findPart, sizeof(findPart), "ls %s/.*.part >/dev/null 2>&1", fhDir);
    TEST_ASSERT_NOT_EQUAL(0, system(findPart));  // no .part temp at all in the non-atomic model
    TEST_ASSERT_FALSE(r.interrupted);            // a full, clean transfer

    // The transferred slice must contain flight 2 as its last record.
    char fpath[420];
    snprintf(fpath, sizeof(fpath), "%s/flightHistory/%s", TEST_SD, r.filename);
    FILE* f = fopen(fpath, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END); uint64_t sz = (uint64_t)ftell(f);
    uint32_t lastFlight = 0; char serial[13] = {0};
    bool ok = lastRecordFromLog(stdio_read_at, f, sz, &lastFlight, serial, sizeof(serial));
    fclose(f);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(2, lastFlight);
}

void test_transfer_flight_cursor_sequence(void) {
    char mem[256];
    snprintf(mem, sizeof(mem), "%s/internal.eaofh", TEST_SD);
    ::remove(mem);
    for (uint32_t i = 1; i <= 4; i++) appendSyntheticFlight(mem, i, 8);

    SimDSU dsu;
    dsu.sdRoot = TEST_SD;
    dsu.internalMemoryPath = mem;
    dsu.writeSpeedKBps = 0;

    // Cursor-driven: transfer flights past a resume point of 2 (as the emu thread does).
    const auto& idx = dsu.flightIndex();
    int sent = 0;
    for (const auto& e : idx)
        if (e.flight > 2 && dsu.transferFlight(e.flight).success) sent++;
    TEST_ASSERT_EQUAL_INT(2, sent);  // flights 3 and 4

    char chk[400];
    snprintf(chk, sizeof(chk), "ls %s/flightHistory/*_00003_*.eaofh >/dev/null 2>&1", TEST_SD);
    TEST_ASSERT_EQUAL(0, system(chk));
    snprintf(chk, sizeof(chk), "ls %s/flightHistory/*_00002_*.eaofh >/dev/null 2>&1", TEST_SD);
    TEST_ASSERT_NOT_EQUAL(0, system(chk));  // flight 2 (<= resume) NOT transferred

    // Unknown flight → no-op failure.
    TEST_ASSERT_FALSE(dsu.transferFlight(99).success);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_read_cookie_none);
    RUN_TEST(test_read_cookie_valid);
    RUN_TEST(test_read_cookie_bad_magic);
    RUN_TEST(test_read_cookie_bad_crc);
    RUN_TEST(test_read_cookie_date_mode);

    RUN_TEST(test_build_flight_index_empty_path);
    RUN_TEST(test_build_flight_index_nonexistent);
    RUN_TEST(test_build_flight_index_short_fixture);

    RUN_TEST(test_session_no_internal_memory);
    RUN_TEST(test_session_no_cookie);
    RUN_TEST(test_session_with_cookie);
    RUN_TEST(test_session_with_max_flight);
    RUN_TEST(test_session_caught_up);
    RUN_TEST(test_session_incremental);

    RUN_TEST(test_session_writes_metrics);
    RUN_TEST(test_session_writes_report);

    RUN_TEST(test_synthetic_memory_roundtrip);
    RUN_TEST(test_transfer_flight_atomic_and_content);
    RUN_TEST(test_transfer_flight_cursor_sequence);

    RUN_TEST(test_build_flight_index_full_fixture);

    return UNITY_END();
}

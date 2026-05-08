#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <string>
#include "hal/hal.h"
#include "airbridge_harvest.h"

// ── Test fixtures ──────────────────────────────────────────────────────────

static StubDisplay  s_display;
static StubClock    s_clock;
static StubNvs      s_nvs;
static MemFilesys   s_fs;
static HAL          s_hal = { &s_display, &s_clock, &s_nvs, &s_fs };
HAL* g_hal = nullptr;

void setUp(void) { g_hal = &s_hal; s_fs.clear_all(); }
void tearDown(void) {}

// Build a minimal valid .eaofh payload: a single 0x4C record with the given
// serial at body[5:17] and flight number at body[20:22].
static std::string makeEaofh(const char* serial, uint16_t flight) {
    const uint16_t RLEN = 28;  // 4-byte prefix + 24-byte body
    std::string s(RLEN, '\0');
    s[0] = (char)0xEA;
    s[1] = (char)0x4C;
    s[2] = (char)((RLEN >> 8) & 0xFF);
    s[3] = (char)(RLEN & 0xFF);
    size_t snLen = strnlen(serial, 12);
    memcpy(&s[4 + 5], serial, snLen);
    s[4 + 20] = (char)((flight >> 8) & 0xFF);
    s[4 + 21] = (char)(flight & 0xFF);
    return s;
}

// ── Harvest tests ───────────────────────────────────────────────────────────

void test_harvest_empty_dir(void) {
    s_fs.add_dir("/sd");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(0, r.count);
    TEST_ASSERT_EQUAL_STRING("0001", r.folder);
}

void test_harvest_single_file(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "hello,world\n");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/data.csv"));
    TEST_ASSERT_EQUAL_STRING("hello,world\n", s_fs.get_content("/sd/upload/0001/data.csv").c_str());
    // Source file should be deleted (moved)
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/data.csv"));
}

void test_harvest_skips_system_files(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "ok");
    s_fs.add_file_str("/sd/Thumbs.db", "skip");
    s_fs.add_file_str("/sd/desktop.ini", "skip");
    s_fs.add_file_str("/sd/.hidden", "skip");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/data.csv"));
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/upload/0001/Thumbs.db"));
}

void test_harvest_skips_harvested_dir(void) {
    s_fs.add_dir("/sd");
    s_fs.add_dir("/sd/upload");
    s_fs.add_file_str("/sd/data.csv", "ok");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
}

void test_harvest_skips_empty_files(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file("/sd/empty.csv", nullptr, 0);
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(0, r.count);
}

void test_harvest_subdirectory_flattening(void) {
    s_fs.add_dir("/sd");
    s_fs.add_dir("/sd/subdir");
    s_fs.add_file_str("/sd/subdir/flight.bin", "data");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/subdir__flight.bin"));
    // Source deleted
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/subdir/flight.bin"));
}

void test_harvest_eaofh_tracking(void) {
    // Serial + flight come from the file *contents* (last 0x4C record),
    // not from the filename. The filename is intentionally wrong here
    // to prove content parsing is what's actually used.
    s_fs.add_dir("/sd");
    auto a = makeEaofh("EA500.000243", 1218);
    auto b = makeEaofh("EA500.000243", 1220);
    s_fs.add_file("/sd/wrong_filename_1.eaofh", a.data(), a.size());
    s_fs.add_file("/sd/wrong_filename_2.eaofh", b.data(), b.size());
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(2, r.count);
    TEST_ASSERT_EQUAL_UINT32(1220, r.maxFlight);
    TEST_ASSERT_EQUAL_STRING("EA500.000243", r.dsuSerial);
}
void test_harvest_eaofh_partial_unparseable(void) {
    // File is .eaofh but contains no valid record (e.g. write interrupted
    // before any 0x0E/0x4C completed). Harvest should still copy the file
    // but maxFlight/serial remain unset.
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/partial.eaofh", "garbage with no record");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_EQUAL_UINT32(0, r.maxFlight);
    TEST_ASSERT_EQUAL_STRING("", r.dsuSerial);
}
void test_harvest_eaofh_serial_from_first_valid_file(void) {
    // First file has no valid record (interrupted), second file has one.
    // Harvest should report serial+flight from the second file.
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/aaa_first.eaofh", "interrupted no record here");
    auto b = makeEaofh("EA500.000179", 2090);
    s_fs.add_file("/sd/bbb_second.eaofh", b.data(), b.size());
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(2, r.count);
    TEST_ASSERT_EQUAL_UINT32(2090, r.maxFlight);
    TEST_ASSERT_EQUAL_STRING("EA500.000179", r.dsuSerial);
}

void test_harvest_multiple_files(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/file1.csv", "aaa");
    s_fs.add_file_str("/sd/file2.csv", "bbb");
    s_fs.add_file_str("/sd/file3.csv", "ccc");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(3, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/file1.csv"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/file2.csv"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/file3.csv"));
    // Sources deleted
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/file1.csv"));
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/file2.csv"));
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/file3.csv"));
}

void test_harvest_sequential_folders(void) {
    // Two harvests create separate subfolders
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/batch1.csv", "aaa");
    HarvestResult r1 = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(1, r1.count);
    TEST_ASSERT_EQUAL_STRING("0001", r1.folder);

    s_fs.add_file_str("/sd/batch2.csv", "bbb");
    HarvestResult r2 = harvestFiles("/sd", "/sd/upload", 2);
    TEST_ASSERT_EQUAL_UINT16(1, r2.count);
    TEST_ASSERT_EQUAL_STRING("0002", r2.folder);

    // Both subfolders should exist with correct files
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/batch1.csv"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0002/batch2.csv"));
}

void test_harvest_root_clean_after(void) {
    // After harvest, root should have no data files
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "content");
    s_fs.add_file_str("/sd/report.txt", "info");
    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(2, r.count);
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/data.csv"));
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/report.txt"));
}

void test_harvest_with_preexisting_queued_files(void) {
    // Simulates boot scan scenario: /upload/ already has files from a
    // previous harvest, but root also has unharvested aircraft data.
    // Both should be handled — harvest should still find root files.
    s_fs.add_dir("/sd");
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/old_file.csv", "previously harvested");
    // New aircraft data on root
    s_fs.add_file_str("/sd/new_flight.eaofh", "new data");
    s_fs.add_dir("/sd/flightHistory");
    s_fs.add_file_str("/sd/flightHistory/log1.bin", "flight log");

    HarvestResult r = harvestFiles("/sd", "/sd/upload", 2);
    TEST_ASSERT_EQUAL_UINT16(2, r.count);
    // New files harvested to /upload/0002/
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0002/new_flight.eaofh"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0002/flightHistory__log1.bin"));
    // Old queued file still there
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/upload/0001/old_file.csv"));
}

void test_harvest_root_subdirectory_files(void) {
    // Aircraft writes to flightHistory/ subdirectory — harvest must find them
    // metrics/ is in skip list (DSU system files) — should NOT be harvested
    s_fs.add_dir("/sd");
    s_fs.add_dir("/sd/flightHistory");
    auto a = makeEaofh("EA500.000243", 1210);
    auto b = makeEaofh("EA500.000243", 1211);
    s_fs.add_file("/sd/flightHistory/EA500.000243_01210_20260406.eaofh", a.data(), a.size());
    s_fs.add_file("/sd/flightHistory/EA500.000243_01211_20260407.eaofh", b.data(), b.size());
    s_fs.add_file_str("/sd/metrics/dsuUsage.eacuf", "metrics");
    s_fs.add_dir("/sd/metrics");

    HarvestResult r = harvestFiles("/sd", "/sd/upload", 1);
    TEST_ASSERT_EQUAL_UINT16(2, r.count);  // metrics/ skipped
    TEST_ASSERT_EQUAL_UINT32(1211, r.maxFlight);
    // Verify metrics file was NOT harvested (preserved for DSU)
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/metrics/dsuUsage.eacuf"));
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_harvest_empty_dir);
    RUN_TEST(test_harvest_single_file);
    RUN_TEST(test_harvest_skips_system_files);
    RUN_TEST(test_harvest_skips_harvested_dir);
    RUN_TEST(test_harvest_skips_empty_files);
    RUN_TEST(test_harvest_subdirectory_flattening);
    RUN_TEST(test_harvest_eaofh_tracking);
    RUN_TEST(test_harvest_eaofh_partial_unparseable);
    RUN_TEST(test_harvest_eaofh_serial_from_first_valid_file);
    RUN_TEST(test_harvest_multiple_files);
    RUN_TEST(test_harvest_sequential_folders);
    RUN_TEST(test_harvest_root_clean_after);
    RUN_TEST(test_harvest_with_preexisting_queued_files);
    RUN_TEST(test_harvest_root_subdirectory_files);

    return UNITY_END();
}

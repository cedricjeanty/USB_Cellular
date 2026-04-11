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

// ── Harvest tests ───────────────────────────────────────────────────────────

void test_harvest_empty_dir(void) {
    s_fs.add_dir("/sd");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(0, r.count);
}

void test_harvest_single_file(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "hello,world\n");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/harvested/data.csv"));
    TEST_ASSERT_EQUAL_STRING("hello,world\n", s_fs.get_content("/sd/harvested/data.csv").c_str());
}

void test_harvest_skips_system_files(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "ok");
    s_fs.add_file_str("/sd/Thumbs.db", "skip");
    s_fs.add_file_str("/sd/desktop.ini", "skip");
    s_fs.add_file_str("/sd/.hidden", "skip");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/harvested/data.csv"));
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/harvested/Thumbs.db"));
}

void test_harvest_skips_harvested_dir(void) {
    s_fs.add_dir("/sd");
    s_fs.add_dir("/sd/harvested");
    s_fs.add_file_str("/sd/data.csv", "ok");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
}

void test_harvest_skips_done_marker(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "content");
    s_fs.add_dir("/sd/harvested");
    s_fs.add_file_str("/sd/harvested/.done__data.csv", "");  // already uploaded
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(0, r.count);
}

void test_harvest_skips_same_size_pending(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "content");
    s_fs.add_dir("/sd/harvested");
    s_fs.add_file_str("/sd/harvested/data.csv", "content");  // same size = already pending
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(0, r.count);
}

void test_harvest_replaces_wrong_size(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/data.csv", "new content");
    s_fs.add_dir("/sd/harvested");
    s_fs.add_file_str("/sd/harvested/data.csv", "old");  // wrong size
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_EQUAL_STRING("new content", s_fs.get_content("/sd/harvested/data.csv").c_str());
}

void test_harvest_skips_empty_files(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file("/sd/empty.csv", nullptr, 0);
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(0, r.count);
}

void test_harvest_subdirectory_flattening(void) {
    s_fs.add_dir("/sd");
    s_fs.add_dir("/sd/logs");
    s_fs.add_file_str("/sd/logs/flight.bin", "data");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(1, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/harvested/logs__flight.bin"));
}

void test_harvest_eaofh_tracking(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/EA500.000243_01218_20260406.eaofh", "flight data");
    s_fs.add_file_str("/sd/EA500.000243_01220_20260407.eaofh", "more data");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(2, r.count);
    TEST_ASSERT_EQUAL_UINT32(1220, r.maxFlight);
    TEST_ASSERT_EQUAL_STRING("EA500.000243", r.dsuSerial);
}

void test_harvest_multiple_files(void) {
    s_fs.add_dir("/sd");
    s_fs.add_file_str("/sd/file1.csv", "aaa");
    s_fs.add_file_str("/sd/file2.csv", "bbb");
    s_fs.add_file_str("/sd/file3.csv", "ccc");
    HarvestResult r = harvestFiles("/sd", "/sd/harvested");
    TEST_ASSERT_EQUAL_UINT16(3, r.count);
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/harvested/file1.csv"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/harvested/file2.csv"));
    TEST_ASSERT_TRUE(s_fs.has_file("/sd/harvested/file3.csv"));
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_harvest_empty_dir);
    RUN_TEST(test_harvest_single_file);
    RUN_TEST(test_harvest_skips_system_files);
    RUN_TEST(test_harvest_skips_harvested_dir);
    RUN_TEST(test_harvest_skips_done_marker);
    RUN_TEST(test_harvest_skips_same_size_pending);
    RUN_TEST(test_harvest_replaces_wrong_size);
    RUN_TEST(test_harvest_skips_empty_files);
    RUN_TEST(test_harvest_subdirectory_flattening);
    RUN_TEST(test_harvest_eaofh_tracking);
    RUN_TEST(test_harvest_multiple_files);

    return UNITY_END();
}

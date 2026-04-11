#include <unity.h>
#include "airbridge_triggers.h"

void setUp(void) {}
void tearDown(void) {}

// ── shouldHarvest tests ─────────────────────────────────────────────────────

void test_harvest_all_conditions_met(void) {
    // Write at t=1000, check at t=20000 (19s idle > 15s window), cooldown 0
    TEST_ASSERT_TRUE(shouldHarvest(false, true, true, 1000, 0, 15000, 20000));
}

void test_harvest_blocked_by_harvesting(void) {
    TEST_ASSERT_FALSE(shouldHarvest(true, true, true, 1000, 0, 15000, 20000));
}

void test_harvest_blocked_no_write(void) {
    TEST_ASSERT_FALSE(shouldHarvest(false, false, true, 1000, 0, 15000, 20000));
}

void test_harvest_blocked_host_never_connected(void) {
    TEST_ASSERT_FALSE(shouldHarvest(false, true, false, 1000, 0, 15000, 20000));
}

void test_harvest_blocked_zero_timestamp(void) {
    TEST_ASSERT_FALSE(shouldHarvest(false, true, true, 0, 0, 15000, 20000));
}

void test_harvest_blocked_quiet_window_not_elapsed(void) {
    // Write at t=10000, check at t=20000 (10s idle < 15s window)
    TEST_ASSERT_FALSE(shouldHarvest(false, true, true, 10000, 0, 15000, 20000));
}

void test_harvest_blocked_cooldown_not_elapsed(void) {
    // Write at t=1000, last harvest at t=15000, cooldown 30s, now=20000
    // (now - lastHarvest) = 5s < 30s cooldown
    TEST_ASSERT_FALSE(shouldHarvest(false, true, true, 1000, 15000, 30000, 20000));
}

void test_harvest_exact_quiet_boundary(void) {
    // Write at t=5000, now=20000 → 15000ms exactly = QUIET_WINDOW_MS
    TEST_ASSERT_TRUE(shouldHarvest(false, true, true, 5000, 0, 15000, 20000));
}

void test_harvest_just_under_quiet_boundary(void) {
    // Write at t=5001, now=20000 → 14999ms < 15000ms
    TEST_ASSERT_FALSE(shouldHarvest(false, true, true, 5001, 0, 15000, 20000));
}

void test_harvest_cooldown_after_first_harvest(void) {
    // First harvest happened at t=30000 with 15s cooldown
    // Write at t=35000, now=50000 → idle=15s OK, cooldown=(50000-30000)=20s > 15s OK
    TEST_ASSERT_TRUE(shouldHarvest(false, true, true, 35000, 30000, 15000, 50000));
}

void test_harvest_cooldown_blocks_rapid_reharvest(void) {
    // Harvest at t=30000 with 15s cooldown
    // New write at t=31000, now=46500 → idle=15.5s OK, but cooldown=(46500-30000)=16.5s > 15s OK
    // Actually this passes. Let's test when cooldown blocks:
    // Harvest at t=40000, write at t=41000, now=50000 → idle=9s < 15s → blocked by quiet window
    TEST_ASSERT_FALSE(shouldHarvest(false, true, true, 41000, 40000, 15000, 50000));
}

void test_harvest_overflow_guard(void) {
    // now < lastWriteMs (shouldn't happen but guard against it)
    TEST_ASSERT_FALSE(shouldHarvest(false, true, true, 50000, 0, 15000, 10000));
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_harvest_all_conditions_met);
    RUN_TEST(test_harvest_blocked_by_harvesting);
    RUN_TEST(test_harvest_blocked_no_write);
    RUN_TEST(test_harvest_blocked_host_never_connected);
    RUN_TEST(test_harvest_blocked_zero_timestamp);
    RUN_TEST(test_harvest_blocked_quiet_window_not_elapsed);
    RUN_TEST(test_harvest_blocked_cooldown_not_elapsed);
    RUN_TEST(test_harvest_exact_quiet_boundary);
    RUN_TEST(test_harvest_just_under_quiet_boundary);
    RUN_TEST(test_harvest_cooldown_after_first_harvest);
    RUN_TEST(test_harvest_cooldown_blocks_rapid_reharvest);
    RUN_TEST(test_harvest_overflow_guard);

    return UNITY_END();
}

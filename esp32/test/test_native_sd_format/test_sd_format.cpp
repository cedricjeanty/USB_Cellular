// Tests for SD partition table manipulation logic.
//
// These exercise the MBR byte-level operations used in sd_init() Case 2
// (add P2 entry, fix P1 type) and the full-format path, without requiring
// ESP-IDF FatFs or sdmmc hardware.  They catch the class of bug where:
//   - mkfs.fat writes P1 type=0x07 (exFAT) instead of 0x0C (FAT32 LBA)
//   - Case 2 adds P2 but leaves P1's wrong type in place
//   - f_fdisk always writes type=0x07 as a placeholder that f_mkfs must fix

#include <unity.h>
#include <cstring>
#include <cstdint>

void setUp(void) {}
void tearDown(void) {}

// ── Helpers mirroring the firmware's MBR manipulation ───────────────────────

static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline void st32(uint8_t* p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}

// Build a minimal MBR with one partition (P1 only).
// system_id lets us reproduce the mkfs.fat 0x07 bug.
static void make_mbr_one_partition(uint8_t* mbr, uint8_t p1_type,
                                   uint32_t p1_start, uint32_t p1_size) {
    memset(mbr, 0, 512);
    mbr[0x1BE + 0] = 0x00;                    // not bootable
    mbr[0x1BE + 4] = p1_type;                 // system ID
    st32(mbr + 0x1BE + 8,  p1_start);
    st32(mbr + 0x1BE + 12, p1_size);
    mbr[510] = 0x55; mbr[511] = 0xAA;
}

// Simulate the Case 2 migration: adds P2 entry and (after fix) corrects P1 type.
static bool simulate_case2_migration(uint8_t* mbr, uint32_t card_sectors,
                                     uint32_t msc_max) {
    uint32_t p1_start = le32(mbr + 0x1BE + 8);
    uint32_t p1_size  = le32(mbr + 0x1BE + 12);
    uint8_t  p1_type  = mbr[0x1BE + 4];

    bool p1_is_ours = (p1_size > 0 && p1_size <= msc_max + 2048);
    uint64_t p1_end = (uint64_t)p1_start + p1_size;
    uint32_t avail  = (p1_end < card_sectors) ? (uint32_t)(card_sectors - p1_end) : 0;

    if (!p1_is_ours || avail <= 1024000) return false;

    // Fix P1 type if not already a FAT type
    if (p1_type != 0x0B && p1_type != 0x0C && p1_type != 0x0E) {
        mbr[0x1BE + 4] = 0x0C;
    }

    // Write P2 entry
    uint32_t p2_start = (uint32_t)p1_end;
    const int P2E = 0x1CE;
    mbr[P2E]   = 0x00;
    mbr[P2E+1] = 0xFE; mbr[P2E+2] = 0xFF; mbr[P2E+3] = 0xFF;
    mbr[P2E+4] = 0x0C;
    mbr[P2E+5] = 0xFE; mbr[P2E+6] = 0xFF; mbr[P2E+7] = 0xFF;
    st32(mbr + P2E + 8,  p2_start);
    st32(mbr + P2E + 12, avail);
    return true;
}

// Simulate what f_fdisk() writes: all partitions get type=0x07 (the bug).
static void simulate_fdisk(uint8_t* mbr, uint32_t p1_sectors,
                            uint32_t p2_sectors, uint32_t p1_start) {
    memset(mbr, 0, 512);
    // P1
    mbr[0x1BE + 4] = 0x07;
    st32(mbr + 0x1BE + 8,  p1_start);
    st32(mbr + 0x1BE + 12, p1_sectors);
    // P2 (immediately after P1)
    uint32_t p2_start = p1_start + p1_sectors;
    mbr[0x1CE + 4] = 0x07;
    st32(mbr + 0x1CE + 8,  p2_start);
    st32(mbr + 0x1CE + 12, p2_sectors);
    mbr[510] = 0x55; mbr[511] = 0xAA;
}

// Simulate the MBR type-fix that now happens after f_fdisk in the format code.
static void simulate_post_fdisk_type_fix(uint8_t* mbr) {
    mbr[0x1BE + 4] = 0x0C;
    if (le32(mbr + 0x1CE + 12) > 0) mbr[0x1CE + 4] = 0x0C;
}

// ── Case 2 migration tests ───────────────────────────────────────────────────

// mkfs.fat produces P1 with type=0x07.  Case 2 must fix it to 0x0C when
// adding P2.  This was the root cause of the field corruption.
void test_case2_fixes_p1_type_from_mkfs_fat(void) {
    uint8_t mbr[512];
    const uint32_t CARD  = 31116288;   // 16 GB card
    const uint32_t MSC   = 16777216;   // 8 GB MSC cap

    make_mbr_one_partition(mbr, 0x07, 63, 16777153);  // mkfs.fat layout
    bool ok = simulate_case2_migration(mbr, CARD, MSC);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1BE + 4]);  // P1 type fixed
}

void test_case2_adds_p2_with_correct_type(void) {
    uint8_t mbr[512];
    make_mbr_one_partition(mbr, 0x07, 63, 16777153);
    simulate_case2_migration(mbr, 31116288, 16777216);

    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1CE + 4]);  // P2 type = FAT32 LBA
}

void test_case2_p2_starts_after_p1(void) {
    uint8_t mbr[512];
    make_mbr_one_partition(mbr, 0x07, 63, 16777153);
    simulate_case2_migration(mbr, 31116288, 16777216);

    uint32_t p1_end   = 63 + 16777153;   // = 16777216
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    TEST_ASSERT_EQUAL_UINT32(p1_end, p2_start);
}

void test_case2_p1_plus_p2_covers_card(void) {
    uint8_t mbr[512];
    const uint32_t CARD = 31116288;
    make_mbr_one_partition(mbr, 0x07, 63, 16777153);
    simulate_case2_migration(mbr, CARD, 16777216);

    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);
    TEST_ASSERT_EQUAL_UINT32(CARD, p2_start + p2_size);
}

void test_case2_does_not_migrate_small_card(void) {
    uint8_t mbr[512];
    // 8 GB card — p1 fills it, no room for P2
    make_mbr_one_partition(mbr, 0x07, 63, 16777153);
    uint8_t original_type = 0x07;
    bool ok = simulate_case2_migration(mbr, 16777216, 16777216);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT8(original_type, mbr[0x1BE + 4]);  // unchanged
}

void test_case2_leaves_valid_p1_alone(void) {
    uint8_t mbr[512];
    // P1 already has 0x0C — should not be changed
    make_mbr_one_partition(mbr, 0x0C, 63, 16777153);
    simulate_case2_migration(mbr, 31116288, 16777216);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1BE + 4]);
}

// ── Full format (f_fdisk + post-fix) tests ───────────────────────────────────

// Before the fix: f_fdisk set both types to 0x07.  f_mkfs("0:", FM_FAT32)
// couldn't find a FAT partition, created its own single-partition MBR, losing
// P2.  The fix patches both types to 0x0C immediately after f_fdisk.
void test_fdisk_alone_leaves_wrong_types(void) {
    uint8_t mbr[512];
    simulate_fdisk(mbr, 16777216, 14339072, 63);
    TEST_ASSERT_EQUAL_UINT8(0x07, mbr[0x1BE + 4]);  // f_fdisk bug
    TEST_ASSERT_EQUAL_UINT8(0x07, mbr[0x1CE + 4]);
}

void test_post_fdisk_fix_corrects_both_types(void) {
    uint8_t mbr[512];
    simulate_fdisk(mbr, 16777216, 14339072, 63);
    simulate_post_fdisk_type_fix(mbr);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1BE + 4]);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1CE + 4]);
}

void test_fdisk_p1_size_equals_msc_cap(void) {
    uint8_t mbr[512];
    const uint32_t MSC_MAX = 16777216;
    simulate_fdisk(mbr, MSC_MAX, 14339072, 63);
    uint32_t p1_size = le32(mbr + 0x1BE + 12);
    TEST_ASSERT_EQUAL_UINT32(MSC_MAX, p1_size);
}

void test_fdisk_p2_size_covers_remaining(void) {
    uint8_t mbr[512];
    const uint32_t P1 = 16777216, P2 = 14339072, START = 63;
    simulate_fdisk(mbr, P1, P2, START);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);
    // p2_start = p1_start + p1_size, p2_size = P2
    TEST_ASSERT_EQUAL_UINT32(START + P1, p2_start);
    TEST_ASSERT_EQUAL_UINT32(P2, p2_size);
}

void test_post_fdisk_fix_does_not_create_p2_if_absent(void) {
    // If f_fdisk was given p2_sectors=0, slot 2 should remain empty after fix
    uint8_t mbr[512];
    simulate_fdisk(mbr, 16777216, 0, 63);  // no P2
    simulate_post_fdisk_type_fix(mbr);
    // P2 size is 0 → type should not have been touched
    TEST_ASSERT_EQUAL_UINT8(0x07, mbr[0x1CE + 4]);  // left as-is (type=0x07)
}

// ── MBR signature preservation ───────────────────────────────────────────────

void test_case2_preserves_mbr_signature(void) {
    uint8_t mbr[512];
    make_mbr_one_partition(mbr, 0x07, 63, 16777153);
    simulate_case2_migration(mbr, 31116288, 16777216);
    TEST_ASSERT_EQUAL_UINT8(0x55, mbr[510]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, mbr[511]);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    UNITY_BEGIN();

    // Case 2 migration
    RUN_TEST(test_case2_fixes_p1_type_from_mkfs_fat);
    RUN_TEST(test_case2_adds_p2_with_correct_type);
    RUN_TEST(test_case2_p2_starts_after_p1);
    RUN_TEST(test_case2_p1_plus_p2_covers_card);
    RUN_TEST(test_case2_does_not_migrate_small_card);
    RUN_TEST(test_case2_leaves_valid_p1_alone);

    // Full format sequence
    RUN_TEST(test_fdisk_alone_leaves_wrong_types);
    RUN_TEST(test_post_fdisk_fix_corrects_both_types);
    RUN_TEST(test_fdisk_p1_size_equals_msc_cap);
    RUN_TEST(test_fdisk_p2_size_covers_remaining);
    RUN_TEST(test_post_fdisk_fix_does_not_create_p2_if_absent);

    // Structural
    RUN_TEST(test_case2_preserves_mbr_signature);

    return UNITY_END();
}

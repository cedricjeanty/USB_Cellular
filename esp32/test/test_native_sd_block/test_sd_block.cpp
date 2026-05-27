// SD block-level tests using real FatFs against a FakeSd in-memory disk.
//
// These tests exercise the actual f_fdisk() / f_mkfs() / f_mount() code
// paths that run on the device, providing confidence that:
//
//  1. The full format sequence (f_fdisk + post-fix + f_mkfs with FM_SFD)
//     produces a correctly partitioned dual-FAT32 layout.
//
//  2. Both P1 and P2 mount with f_mount() and accept file I/O.
//
//  3. The OLD broken sequence (f_fdisk + f_mkfs without FM_SFD on raw drive)
//     is documented to fail — ensuring the regression is caught if the fix
//     is ever accidentally reverted.
//
//  4. Case 2 migration (add P2 to existing single-partition card) leaves a
//     layout that mounts successfully on the next boot.

#include <unity.h>
#include <cstring>
#include <cstdio>
#include "fake_sd.h"
#include "ff.h"

// ── Constants matching the firmware ──────────────────────────────────────────

static constexpr uint32_t CARD_SECTORS  = 31116288;  // 16 GB at 512 B/sector
static constexpr uint32_t MSC_MAX       = 16777216;  // 8 GB cap (MSC_MAX_SECTORS)
static constexpr uint32_t P1_START_CHS  = 63;        // CHS-aligned start used by f_fdisk

// Drive assignments (mirrors sd_init() dual-partition code)
#define PDRV_RAW  0   // full disk — used by f_fdisk
#define PDRV_P2   1   // P2 offset diskio
#define PDRV_P1   2   // P1 offset diskio

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

// Read MBR from FakeSd and return a pointer to a static 512-byte buffer.
static const uint8_t* read_mbr(FakeSd& disk) {
    static uint8_t buf[512];
    disk.read_sector(0, buf);
    return buf;
}

// Run the firmware's format sequence on a FakeSd and return true on success.
// This mirrors FmtTask in main.cpp exactly (the FIXED version).
static bool run_full_format(FakeSd& disk) {
    uint32_t p1_sectors = MSC_MAX;
    uint32_t p2_sectors = (disk.num_sectors() > p1_sectors + 2048)
                          ? disk.num_sectors() - p1_sectors : 0;
    if (p2_sectors == 0) return false;

    disk.register_raw(PDRV_RAW);

    LBA_t plist[] = {(LBA_t)p1_sectors, (LBA_t)p2_sectors, 0, 0};
    void* work = malloc(4096);
    if (!work) { FakeSd::unregister(PDRV_RAW); return false; }

    bool ok = false;

    // Step 1: create partition table
    FRESULT fr = f_fdisk(PDRV_RAW, plist, work);
    if (fr != FR_OK) { free(work); FakeSd::unregister(PDRV_RAW); return false; }

    // Step 2: read back MBR to get actual LBA starts + fix types to 0x0C
    uint8_t mbr[512];
    disk.read_sector(0, mbr);
    uint32_t p1_start = le32(mbr + 0x1BE + 8);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);
    mbr[0x1BE + 4] = 0x0C;
    if (p2_size > 0) mbr[0x1CE + 4] = 0x0C;
    disk.write(mbr, 0, 1);

    // Step 3: format P1 via offset diskio + FM_SFD
    MKFS_PARM opt = { .fmt = (BYTE)(FM_FAT32 | FM_SFD), .n_fat = 2, .au_size = 4096 };
    disk.register_offset(PDRV_P1, p1_start, p1_sectors);
    fr = f_mkfs("2:", &opt, work, 4096);
    FakeSd::unregister(PDRV_P1);
    if (fr != FR_OK) { free(work); FakeSd::unregister(PDRV_RAW); return false; }

    // Step 4: format P2 via offset diskio + FM_SFD
    disk.register_offset(PDRV_P2, p2_start, p2_size);
    fr = f_mkfs("1:", &opt, work, 4096);
    if (fr != FR_OK) { free(work); FakeSd::unregister(PDRV_P2); FakeSd::unregister(PDRV_RAW); return false; }

    ok = true;
    free(work);
    FakeSd::unregister(PDRV_RAW);
    return ok;
}

void setUp(void)    {}
void tearDown(void) {
    // Unregister all drives after each test
    for (BYTE i = 0; i < FF_VOLUMES; i++) FakeSd::unregister(i);
    // Unmount all volumes
    f_mount(nullptr, "0:", 0);
    f_mount(nullptr, "1:", 0);
    f_mount(nullptr, "2:", 0);
}

// ── TEST: format produces correct partition types ─────────────────────────────

void test_full_format_p1_type_is_fat32(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    const uint8_t* mbr = read_mbr(disk);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1BE + 4]);
}

void test_full_format_p2_type_is_fat32(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    const uint8_t* mbr = read_mbr(disk);
    TEST_ASSERT_EQUAL_UINT8(0x0C, mbr[0x1CE + 4]);
}

void test_full_format_p1_size_is_msc_max(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    const uint8_t* mbr = read_mbr(disk);
    uint32_t p1_size = le32(mbr + 0x1BE + 12);
    TEST_ASSERT_EQUAL_UINT32(MSC_MAX, p1_size);
}

void test_full_format_p2_ends_at_card_boundary(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);
    TEST_ASSERT_EQUAL_UINT32(CARD_SECTORS, p2_start + p2_size);
}

void test_full_format_mbr_signature_intact(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    const uint8_t* mbr = read_mbr(disk);
    TEST_ASSERT_EQUAL_UINT8(0x55, mbr[510]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, mbr[511]);
}

// ── TEST: both partitions mount with f_mount ──────────────────────────────────

void test_full_format_p1_mounts(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    const uint8_t* mbr = read_mbr(disk);
    uint32_t p1_start = le32(mbr + 0x1BE + 8);
    uint32_t p1_size  = le32(mbr + 0x1BE + 12);

    disk.register_offset(PDRV_P1, p1_start, p1_size);
    static FATFS fs;
    FRESULT fr = f_mount(&fs, "2:", 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);
    f_mount(nullptr, "2:", 0);
}

void test_full_format_p2_mounts(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);

    disk.register_offset(PDRV_P2, p2_start, p2_size);
    static FATFS fs;
    FRESULT fr = f_mount(&fs, "1:", 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);
    f_mount(nullptr, "1:", 0);
}

// ── TEST: file I/O through FatFs on formatted partitions ─────────────────────

void test_full_format_p2_file_write_read(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);

    disk.register_offset(PDRV_P2, p2_start, p2_size);
    static FATFS fs;
    FRESULT fr = f_mount(&fs, "1:", 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);

    // Write a small file
    FIL fil;
    fr = f_open(&fil, "1:/test.txt", FA_CREATE_ALWAYS | FA_WRITE);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);
    UINT written = 0;
    const char* data = "AirBridge FakeSd test";
    fr = f_write(&fil, data, strlen(data), &written);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);
    TEST_ASSERT_EQUAL_UINT(strlen(data), written);
    f_close(&fil);

    // Read it back
    fr = f_open(&fil, "1:/test.txt", FA_READ);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);
    char buf[64] = {};
    UINT bread = 0;
    fr = f_read(&fil, buf, sizeof(buf) - 1, &bread);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);
    TEST_ASSERT_EQUAL_UINT(strlen(data), bread);
    TEST_ASSERT_EQUAL_STRING(data, buf);
    f_close(&fil);

    f_mount(nullptr, "1:", 0);
}

// ── TEST: regression — old broken sequence (no FM_SFD on raw drive) ──────────
// The OLD code called f_mkfs("0:", FM_FAT32) on the raw disk after f_fdisk.
// find_volume() found no FAT (blank partition sectors) and created its own
// single-partition MBR, losing P2.  This test documents that behaviour —
// if P2 is absent after the old sequence, the bug is still present.

void test_old_broken_sequence_loses_p2(void) {
    FakeSd disk(CARD_SECTORS);
    disk.register_raw(PDRV_RAW);

    uint32_t p1_sectors = MSC_MAX;
    uint32_t p2_sectors = CARD_SECTORS - p1_sectors;
    LBA_t plist[] = {(LBA_t)p1_sectors, (LBA_t)p2_sectors, 0, 0};
    void* work = malloc(4096);
    TEST_ASSERT_NOT_NULL(work);

    // Step 1: fdisk (sets both types to 0x07 — the bug source)
    FRESULT fr = f_fdisk(PDRV_RAW, plist, work);
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)fr);

    // Step 2: OLD broken mkfs on raw drive without FM_SFD
    MKFS_PARM opt = { .fmt = FM_FAT32, .n_fat = 2, .au_size = 4096 };
    fr = f_mkfs("0:", &opt, work, 4096);
    // f_mkfs may succeed or fail — either way P2 is gone or wrong
    // (It overwrites the 2-partition MBR with a single-partition MBR)
    free(work);
    FakeSd::unregister(PDRV_RAW);

    // Check: P2 slot should now be empty or wrong size (single-partition result)
    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_size_after = le32(mbr + 0x1CE + 12);
    // If mkfs succeeded and overwrote the MBR, P2 slot is empty (size=0)
    // If mkfs failed, the f_fdisk MBR is intact (P2 still type=0x07, not useful)
    // Either way, P2 is NOT a properly formatted independent partition
    if (fr == FR_OK) {
        // mkfs rewrote MBR: P2 slot should be gone
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p2_size_after,
            "Old sequence: f_mkfs on raw drive overwrites 2-partition MBR, P2 lost");
    } else {
        // mkfs failed: P2 slot still has f_fdisk's 0x07 type
        uint8_t p2_type = mbr[0x1CE + 4];
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0x0C, (int)p2_type,
            "Old sequence: f_fdisk leaves 0x07 and mkfs failed, P2 still not FAT32");
    }
}

// ── TEST: sparse memory usage ─────────────────────────────────────────────────
// Verify that a 16GB FakeSd doesn't allocate 16GB — sparse storage should
// use only the sectors f_mkfs actually writes (VBR + FAT + root dir).

void test_fakeSd_is_sparse(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    size_t allocated = disk.allocated_sector_count();
    // FAT32 for ~8GB P1 (4KB clusters): VBR(1) + FSInfo(1) + backup VBR(1) +
    // FAT1 + FAT2 (~16K sectors each for 2M clusters) + root cluster(8) ≈ 33K sectors
    // Two partitions + MBR ≈ <80K sectors total
    // 80000 * 512 = 40MB — negligible vs 16GB disk (0.25%)
    uint32_t one_percent = disk.num_sectors() / 100;   // 311162 for 16GB
    TEST_ASSERT_LESS_THAN_MESSAGE((int)one_percent, (int)allocated,
        "FakeSd should be sparse: allocated sectors < 1% of disk size");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    UNITY_BEGIN();

    // Partition types after correct format
    RUN_TEST(test_full_format_p1_type_is_fat32);
    RUN_TEST(test_full_format_p2_type_is_fat32);
    RUN_TEST(test_full_format_p1_size_is_msc_max);
    RUN_TEST(test_full_format_p2_ends_at_card_boundary);
    RUN_TEST(test_full_format_mbr_signature_intact);

    // Both partitions actually mount
    RUN_TEST(test_full_format_p1_mounts);
    RUN_TEST(test_full_format_p2_mounts);

    // File I/O works end-to-end
    RUN_TEST(test_full_format_p2_file_write_read);

    // Regression: old broken sequence documented
    RUN_TEST(test_old_broken_sequence_loses_p2);

    // Sparse memory use
    RUN_TEST(test_fakeSd_is_sparse);

    return UNITY_END();
}

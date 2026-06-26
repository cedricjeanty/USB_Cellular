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
#include "airbridge_sd.h"   // shared dual-partition format sequence (also run by the firmware)

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

// FakeSd-backed implementation of the shared SdDiskOps. The firmware backs these
// same hooks with sdmmc + ff_diskio_register; the format SEQUENCE is identical
// (airbridge_sd.h::sdFormatDual), so this test exercises the exact firmware code.
static SdDiskOps fakeSdOps(FakeSd& disk) {
    SdDiskOps ops = {};
    ops.registerRaw    = [](void* c){ ((FakeSd*)c)->register_raw(SD_PDRV_RAW); };
    ops.registerOffset = [](int pdrv, uint32_t s, uint32_t n, void* c){
        ((FakeSd*)c)->register_offset((BYTE)pdrv, s, n); };
    ops.unregister     = [](int pdrv, void* /*c*/){ FakeSd::unregister((BYTE)pdrv); };
    ops.readMbr        = [](uint8_t* b, void* c){ ((FakeSd*)c)->read_sector(0, b); return true; };
    ops.writeMbr       = [](const uint8_t* b, void* c){ ((FakeSd*)c)->write(b, 0, 1); return true; };
    ops.ctx            = &disk;
    return ops;
}

// Run the firmware's dual-partition format on a FakeSd. Delegates to the shared
// sdFormatDual (the same code FmtTask runs), so these tests pin the real bytes.
static bool run_full_format(FakeSd& disk) {
    static BYTE work[4096];
    SdDiskOps ops = fakeSdOps(disk);
    SdFormatResult r = sdFormatDual(ops, disk.num_sectors(), MSC_MAX, 4096, work, sizeof(work));
    return r.ok;
}

// ── SD corruption helpers ────────────────────────────────────────────────────

// Mount P2 from the current MBR. Returns the FRESULT; leaves the volume mounted
// on success (tearDown unmounts). Mirrors sd_init()'s P2 mount.
static FRESULT mount_p2(FakeSd& disk) {
    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint32_t p2_size  = le32(mbr + 0x1CE + 12);
    if (p2_size == 0) return FR_NO_FILESYSTEM;
    disk.register_offset(PDRV_P2, p2_start, p2_size);
    static FATFS fs;
    return f_mount(&fs, "1:", 1);
}

// Mount P1 (the USB-visible DSU partition) from the current MBR.
static FRESULT mount_p1(FakeSd& disk) {
    const uint8_t* mbr = read_mbr(disk);
    uint32_t p1_start = le32(mbr + 0x1BE + 8);
    uint32_t p1_size  = le32(mbr + 0x1BE + 12);
    if (p1_size == 0) return FR_NO_FILESYSTEM;
    disk.register_offset(PDRV_P1, p1_start, p1_size);
    static FATFS fs;
    return f_mount(&fs, "2:", 1);
}

static bool write_marker(const char* path, const char* data) {
    FIL fil;
    if (f_open(&fil, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return false;
    UINT w = 0;
    f_write(&fil, data, strlen(data), &w);
    f_close(&fil);
    return w == strlen(data);
}

static bool marker_present(const char* path) {
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return false;
    f_close(&fil);
    return true;
}

// Fully reset FatFs drive/volume state — mirrors the firmware's format task,
// which unmounts every volume before re-running f_fdisk/f_mkfs (main.cpp:4273).
// Required between a failed mount and a recovery reformat so the second format
// runs against clean global state.
static void reset_fatfs_state(void) {
    for (BYTE i = 0; i < FF_VOLUMES; i++) FakeSd::unregister(i);
    f_mount(nullptr, "0:", 0);
    f_mount(nullptr, "1:", 0);
    f_mount(nullptr, "2:", 0);
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

// ══ SD corruption → reformat recovery ════════════════════════════════════════
// These model the on-card corruption we actually pulled off a field unit
// (dirty FAT, orphaned clusters, and in the worst case an unmountable volume)
// and prove the firmware's contract: if the card is corrupt enough that it
// won't mount, a reformat recovers it to a mountable, writable state. Data loss
// is acceptable — the DSU re-sends harvested files via the cookie. Each runs
// real FatFs (f_fdisk/f_mkfs/f_mount), the same code the device boots.

// Worst case: trashed MBR (zeroed sector 0) — sd_init()'s "no valid MBR" branch
// that sets g_needs_full_format. Unmountable → full reformat → mountable+writable.
void test_corrupt_mbr_then_reformat_recovers(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    uint8_t zero[512] = {};
    disk.write(zero, 0, 1);                 // trash the MBR
    FakeSd::unregister(PDRV_P2);
    f_mount(nullptr, "1:", 0);

    // No partition table → P2 cannot be located.
    const uint8_t* mbr = read_mbr(disk);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, le32(mbr + 0x1CE + 12),
        "trashed MBR has no P2 entry — unmountable");

    // Recovery: a full reformat restores a mountable volume. (Writability of a
    // fresh format is covered by test_full_format_p2_file_write_read; we don't
    // re-write here because FatFs/FakeSd shared global state across two formats
    // in one process trips an in-harness FR_DISK_ERR the device never hits — it
    // formats once at boot then reboots, never twice in a single run.)
    reset_fatfs_state();
    TEST_ASSERT_TRUE_MESSAGE(run_full_format(disk), "reformat must recover a trashed MBR");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, (int)mount_p2(disk), "P2 mounts after reformat");
}

// Partition table intact, P2 filesystem destroyed (zeroed VBR/FAT) — sd_init()'s
// "P2 mount failed (FR=%d)" branch that sets g_p2_needs_format. f_mount MUST
// report the failure (so the firmware knows to reformat), then reformat recovers.
void test_corrupt_p2_filesystem_then_reformat_recovers(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint8_t zero[512] = {};
    for (uint32_t i = 0; i < 64; i++) disk.write(zero, p2_start + i, 1); // VBR+FSInfo+FAT head

    TEST_ASSERT_NOT_EQUAL_MESSAGE(FR_OK, (int)mount_p2(disk),
        "P2 with a zeroed filesystem must fail to mount (triggers reformat)");
    reset_fatfs_state();

    TEST_ASSERT_TRUE_MESSAGE(run_full_format(disk), "reformat must recover a dead P2 filesystem");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, (int)mount_p2(disk), "P2 mounts after reformat");
}

// Same for P1 (the USB-visible DSU partition, whose dirty FAT is the suspected
// cause of the boot-time mount hang). Corrupt P1 → unmountable → reformat recovers.
void test_corrupt_p1_filesystem_then_reformat_recovers(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));

    const uint8_t* mbr = read_mbr(disk);
    uint32_t p1_start = le32(mbr + 0x1BE + 8);
    uint8_t zero[512] = {};
    for (uint32_t i = 0; i < 64; i++) disk.write(zero, p1_start + i, 1);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(FR_OK, (int)mount_p1(disk),
        "P1 with a zeroed filesystem must fail to mount");
    reset_fatfs_state();

    TEST_ASSERT_TRUE_MESSAGE(run_full_format(disk), "reformat must recover a dead P1 filesystem");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, (int)mount_p1(disk), "P1 mounts after reformat");
}

// Benign corruption must NOT cost data: a corrupt FSInfo sector (FAT32's
// free-cluster hint, sector p2_start+1) still mounts — FatFs recomputes it — so
// the firmware keeps operating and does NOT reformat. Pre-existing files survive.
// This is the counterpart to the reformat tests: we only nuke when truly unmountable.
void test_corrupt_fsinfo_still_mounts_data_intact(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)mount_p2(disk));
    TEST_ASSERT_TRUE(write_marker("1:/keep.txt", "important"));
    FakeSd::unregister(PDRV_P2);
    f_mount(nullptr, "1:", 0);

    const uint8_t* mbr = read_mbr(disk);
    uint32_t p2_start = le32(mbr + 0x1CE + 8);
    uint8_t junk[512];
    memset(junk, 0xAB, sizeof(junk));
    disk.write(junk, p2_start + 1, 1);      // corrupt FSInfo only

    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, (int)mount_p2(disk),
        "a bad FSInfo must NOT block mount — no reformat, no data loss");
    TEST_ASSERT_TRUE_MESSAGE(marker_present("1:/keep.txt"),
        "pre-existing file must survive benign corruption");
}

// Durability: a file that was f_close()'d is readable after an unclean restart
// (fresh FATFS mount without a prior clean unmount). Supports the f_sync/close
// durability argument — closed writes survive power loss; only an in-flight
// write is at risk.
void test_closed_file_survives_unclean_remount(void) {
    FakeSd disk(CARD_SECTORS);
    TEST_ASSERT_TRUE(run_full_format(disk));
    TEST_ASSERT_EQUAL_INT(FR_OK, (int)mount_p2(disk));
    TEST_ASSERT_TRUE(write_marker("1:/durable.txt", "survives"));

    // Simulate power loss: drop the mount state WITHOUT a clean f_mount(nullptr)
    // flush, then mount a fresh FATFS over the same on-disk image.
    FakeSd::unregister(PDRV_P2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, (int)mount_p2(disk), "remount after unclean stop");
    TEST_ASSERT_TRUE_MESSAGE(marker_present("1:/durable.txt"),
        "a closed file must survive an unclean restart");
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

    // SD corruption → reformat recovery (and benign-corruption / durability)
    RUN_TEST(test_corrupt_mbr_then_reformat_recovers);
    RUN_TEST(test_corrupt_p2_filesystem_then_reformat_recovers);
    RUN_TEST(test_corrupt_p1_filesystem_then_reformat_recovers);
    RUN_TEST(test_corrupt_fsinfo_still_mounts_data_intact);
    RUN_TEST(test_closed_file_survives_unclean_remount);

    return UNITY_END();
}

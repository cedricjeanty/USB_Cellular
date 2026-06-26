// Tests for FatFsFilesys — the emulator's IFilesys backed by real FatFs on an
// in-memory FakeSd block device. Proves the backend the emulator's SD is wired
// through: long-filename file/dir ops, host-tree import (the drop-files
// workflow), and the corrupt-FAT → ops-fail → reformat → recover lifecycle that
// makes the emulator exercise the firmware's degrade/reformat path end-to-end.
//
// This suite vendors its own LFN-enabled FatFs (ff.c/ffconf.h/ffunicode.c/…) so
// it is self-contained and CI-friendly (no SDL/emulator needed).

#include <unity.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include "hal/fatfs_filesys.h"

static const char* P1 = "./emu_sdcard";
static const char* P2 = "./emu_sdcard_internal";

void setUp(void) {}
void tearDown(void) {}

static FatFsFilesys* freshFormatted() {
    auto* fs = new FatFsFilesys(65536, P1, P2);  // 32 MB card
    TEST_ASSERT_TRUE_MESSAGE(fs->format(), "format must succeed");
    TEST_ASSERT_TRUE(fs->mounted());
    return fs;
}

void test_format_mounts(void) {
    auto* fs = freshFormatted();
    TEST_ASSERT_TRUE(fs->probeOk());
    delete fs;
}

void test_long_filename_write_read(void) {
    auto* fs = freshFormatted();
    TEST_ASSERT_TRUE(fs->mkdir("./emu_sdcard_internal/upload"));
    TEST_ASSERT_TRUE(fs->mkdir("./emu_sdcard_internal/upload/0001"));
    const char* path = "./emu_sdcard_internal/upload/0001/flightHistory__FLT00123.eaofh";
    void* w = fs->open(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(w, "open(wb) on a long name (LFN) must succeed");
    TEST_ASSERT_EQUAL_UINT(14, fs->write(w, "HELLO LONGNAME", 14));
    fs->close(w);

    void* r = fs->open(path, "rb");
    TEST_ASSERT_NOT_NULL(r);
    char buf[32] = {0};
    TEST_ASSERT_EQUAL_UINT(14, fs->read(r, buf, sizeof(buf)));
    fs->close(r);
    TEST_ASSERT_EQUAL_STRING("HELLO LONGNAME", buf);
    TEST_ASSERT_TRUE(fs->exists(path));
    delete fs;
}

void test_opendir_readdir(void) {
    auto* fs = freshFormatted();
    fs->mkdir("./emu_sdcard_internal/logs");
    void* f = fs->open("./emu_sdcard_internal/logs/boot_0007.log", "wb");
    TEST_ASSERT_NOT_NULL(f); fs->write(f, "x", 1); fs->close(f);

    void* d = fs->opendir("./emu_sdcard_internal/logs");
    TEST_ASSERT_NOT_NULL(d);
    FsDirEntry e; int n = 0; bool found = false;
    while (fs->readdir(d, &e)) { n++; if (!strcmp(e.name, "boot_0007.log")) found = true; }
    fs->closedir(d);
    TEST_ASSERT_TRUE_MESSAGE(found, "readdir must list the written log");
    delete fs;
}

void test_import_host_tree(void) {
    // Seed a host dir (the "drop files into ./emu_sdcard" workflow) and import.
    char tmpl[] = "/tmp/emusd_XXXXXX";
    char* host = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(host);
    std::string sub = std::string(host) + "/flightHistory";
    ::mkdir(sub.c_str(), 0755);
    std::string fpath = sub + "/FLT00123_longname.eaofh";
    FILE* hf = fopen(fpath.c_str(), "wb"); TEST_ASSERT_NOT_NULL(hf);
    fwrite("FLIGHTDATA", 1, 10, hf); fclose(hf);

    auto* fs = freshFormatted();
    int imported = fs->importHostTree(host, P1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, imported, "one host file imported into the FAT image");
    TEST_ASSERT_TRUE(fs->exists("./emu_sdcard/flightHistory/FLT00123_longname.eaofh"));
    delete fs;
}

void test_corrupt_then_ops_fail(void) {
    auto* fs = freshFormatted();
    void* w = fs->open("./emu_sdcard_internal/keep.txt", "wb");
    TEST_ASSERT_NOT_NULL(w); fs->write(w, "data", 4); fs->close(w);

    fs->corruptFat();
    TEST_ASSERT_FALSE_MESSAGE(fs->mounted(), "corruption drops the mount");
    TEST_ASSERT_FALSE_MESSAGE(fs->probeOk(), "probe fails on a corrupt FAT (remount fails)");
    TEST_ASSERT_NULL_MESSAGE(fs->open("./emu_sdcard_internal/keep.txt", "rb"),
                             "file ops fail while the card is unusable");
    delete fs;
}

void test_reformat_recovers(void) {
    auto* fs = freshFormatted();
    void* w = fs->open("./emu_sdcard_internal/keep.txt", "wb");
    TEST_ASSERT_NOT_NULL(w); fs->write(w, "data", 4); fs->close(w);

    fs->corruptFat();
    TEST_ASSERT_FALSE(fs->probeOk());

    TEST_ASSERT_TRUE_MESSAGE(fs->format(), "reformat must recover a mountable volume");
    TEST_ASSERT_TRUE(fs->mounted());
    TEST_ASSERT_TRUE(fs->probeOk());
    // Data is gone (the accepted trade-off; DSU re-sends via the cookie)...
    TEST_ASSERT_FALSE_MESSAGE(fs->exists("./emu_sdcard_internal/keep.txt"),
                              "reformat wipes the queue");
    // ...but the volume is writable again.
    void* w2 = fs->open("./emu_sdcard_internal/recovered.txt", "wb");
    TEST_ASSERT_NOT_NULL(w2); fs->write(w2, "ok", 2); fs->close(w2);
    delete fs;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_format_mounts);
    RUN_TEST(test_long_filename_write_read);
    RUN_TEST(test_opendir_readdir);
    RUN_TEST(test_import_host_tree);
    RUN_TEST(test_corrupt_then_ops_fail);
    RUN_TEST(test_reformat_recovers);
    return UNITY_END();
}

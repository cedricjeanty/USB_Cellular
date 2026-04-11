#include <unity.h>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include "hal/hal.h"
#include "airbridge_harvest.h"

// ── In-memory filesystem ────────────────────────────────────────────────────

struct MemFile {
    std::vector<uint8_t> data;
    bool is_dir;
};

class TestFilesys : public IFilesys {
public:
    std::map<std::string, MemFile> files;

    void clear_all() { files.clear(); }

    void add_file(const char* path, const void* data, size_t len) {
        MemFile f;
        f.data.assign((const uint8_t*)data, (const uint8_t*)data + len);
        f.is_dir = false;
        files[path] = f;
    }

    void add_file_str(const char* path, const char* content) {
        add_file(path, content, strlen(content));
    }

    void add_dir(const char* path) {
        MemFile f;
        f.is_dir = true;
        files[path] = f;
    }

    bool has_file(const char* path) {
        return files.count(path) && !files[path].is_dir;
    }

    std::string get_content(const char* path) {
        auto it = files.find(path);
        if (it == files.end() || it->second.is_dir) return "";
        return std::string(it->second.data.begin(), it->second.data.end());
    }

    // ── IFilesys implementation ──

    struct OpenFile { std::string path; size_t pos; std::string mode; std::vector<uint8_t> wbuf; };
    std::vector<OpenFile*> open_handles;

    void* open(const char* path, const char* mode) override {
        auto* h = new OpenFile{path, 0, mode, {}};
        if (mode[0] == 'r') {
            auto it = files.find(path);
            if (it == files.end() || it->second.is_dir) { delete h; return nullptr; }
        }
        open_handles.push_back(h);
        return h;
    }

    size_t read(void* f, void* buf, size_t len) override {
        auto* h = (OpenFile*)f;
        auto it = files.find(h->path);
        if (it == files.end()) return 0;
        size_t avail = it->second.data.size() - h->pos;
        size_t n = (len < avail) ? len : avail;
        memcpy(buf, it->second.data.data() + h->pos, n);
        h->pos += n;
        return n;
    }

    size_t write(void* f, const void* buf, size_t len) override {
        auto* h = (OpenFile*)f;
        h->wbuf.insert(h->wbuf.end(), (const uint8_t*)buf, (const uint8_t*)buf + len);
        return len;
    }

    bool seek(void* f, long offset, int whence) override {
        auto* h = (OpenFile*)f;
        if (whence == SEEK_SET) h->pos = offset;
        else if (whence == SEEK_END) {
            auto it = files.find(h->path);
            if (it != files.end()) h->pos = it->second.data.size() + offset;
        }
        return true;
    }

    long tell(void* f) override { return ((OpenFile*)f)->pos; }

    void close(void* f) override {
        auto* h = (OpenFile*)f;
        if (h->mode[0] == 'w' && !h->wbuf.empty()) {
            MemFile mf;
            mf.data = h->wbuf;
            mf.is_dir = false;
            files[h->path] = mf;
        }
        delete h;
    }

    // Directory operations
    struct DirIter { std::string prefix; std::vector<std::string> entries; size_t idx; };

    void* opendir(const char* path) override {
        std::string pfx = std::string(path) + "/";
        auto* d = new DirIter{pfx, {}, 0};
        for (auto& kv : files) {
            if (kv.first.substr(0, pfx.size()) == pfx) {
                std::string rest = kv.first.substr(pfx.size());
                if (rest.find('/') == std::string::npos) {
                    d->entries.push_back(rest);
                }
            }
        }
        return d;
    }

    bool readdir(void* dir, FsDirEntry* entry) override {
        auto* d = (DirIter*)dir;
        if (d->idx >= d->entries.size()) return false;
        std::string name = d->entries[d->idx++];
        strlcpy(entry->name, name.c_str(), sizeof(entry->name));
        std::string full = d->prefix + name;
        auto it = files.find(full);
        entry->is_dir = (it != files.end() && it->second.is_dir);
        entry->size = (it != files.end() && !it->second.is_dir) ? it->second.data.size() : 0;
        return true;
    }

    void closedir(void* d) override { delete (DirIter*)d; }

    bool stat(const char* path, uint32_t* size_out, bool* is_dir_out) override {
        auto it = files.find(path);
        if (it == files.end()) return false;
        if (size_out) *size_out = it->second.data.size();
        if (is_dir_out) *is_dir_out = it->second.is_dir;
        return true;
    }

    bool mkdir(const char* path) override { add_dir(path); return true; }

    bool remove(const char* path) override {
        return files.erase(path) > 0;
    }

    bool exists(const char* path) override {
        return files.count(path) > 0;
    }
};

// ── Stubs ───────────────────────────────────────────────────────────────────

class StubDisplay : public IDisplay {
public: bool init() override { return true; } void flush() override {} bool ok() const override { return true; }
};
class StubClock : public IClock {
public: uint32_t millis() override { return 0; } void delay_ms(uint32_t) override {}
};
class StubNvs : public INvs {
public:
    bool get_str(const char*, const char*, char* o, size_t) override { o[0]=0; return false; }
    bool set_str(const char*, const char*, const char*) override { return true; }
    bool get_u8(const char*, const char*, uint8_t*) override { return false; }
    bool set_u8(const char*, const char*, uint8_t) override { return true; }
    bool get_i32(const char*, const char*, int32_t*) override { return false; }
    bool set_i32(const char*, const char*, int32_t) override { return true; }
    bool get_u32(const char*, const char*, uint32_t*) override { return false; }
    bool set_u32(const char*, const char*, uint32_t) override { return true; }
    void erase_key(const char*, const char*) override {}
};

static StubDisplay  s_display;
static StubClock    s_clock;
static StubNvs      s_nvs;
static TestFilesys  s_fs;
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

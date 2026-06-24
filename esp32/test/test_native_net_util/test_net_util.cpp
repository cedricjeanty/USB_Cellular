#include <unity.h>
#include "hal/test_impls.h"
#include "hal/hal.h"
#include "hal/net_util.h"
#include <string>
#include <vector>

// netWriteAll() is the single home for the wall-clock no-progress write timeout that both
// real-TLS impls (Esp32Network, OpenSSLNetwork) route their write() through. A dead cellular
// link returns "would-block" forever; without the bound the upload task wedges and the
// device stops reconnecting (a real in-field failure). These tests pin the three behaviours:
// full write, dead-link timeout, hard error.

static TestClock s_clock;
static HAL       s_hal = { nullptr, &s_clock, nullptr, nullptr, nullptr, nullptr };
HAL* g_hal = nullptr;

void setUp(void)    { s_clock.now_ms = 0; g_hal = &s_hal; }
void tearDown(void) {}

// ── Fake networks exercising each writeSome() outcome ────────────────────────

// Accepts everything, but only `chunk` bytes per call — proves the loop reassembles a full
// buffer across multiple progress-making calls.
class ChunkNetwork : public INetwork {
public:
    size_t chunk;
    std::string sink;
    int calls = 0;
    explicit ChunkNetwork(size_t c) : chunk(c) {}
    TlsHandle connect(const char*) override { return (TlsHandle)1; }
    int writeSome(TlsHandle, const void* data, size_t len) override {
        calls++;
        size_t n = (len < chunk) ? len : chunk;
        sink.append((const char*)data, n);
        return (int)n;
    }
    bool write(TlsHandle c, const void* d, size_t l) override { return netWriteAll(this, c, d, l); }
    int read(TlsHandle, void*, size_t) override { return 0; }
    void destroy(TlsHandle) override {}
};

// Never makes progress: always "would-block" (0). Models a dead link.
class StallNetwork : public INetwork {
public:
    TlsHandle connect(const char*) override { return (TlsHandle)1; }
    int writeSome(TlsHandle, const void*, size_t) override { return 0; }
    bool write(TlsHandle c, const void* d, size_t l) override { return netWriteAll(this, c, d, l); }
    int read(TlsHandle, void*, size_t) override { return 0; }
    void destroy(TlsHandle) override {}
};

// Link alive at first (accepts a prefix), then DIES mid-transfer: writeSome
// returns 0 (would-block) forever after `aliveBytes`. This is exactly the field
// scenario that bricked a unit — the cellular link dropped partway through an
// upload and the (old, unbounded) write loop spun forever holding the SD mutex.
class LinkDiesNetwork : public INetwork {
public:
    size_t aliveBytes;
    size_t accepted = 0;
    explicit LinkDiesNetwork(size_t alive) : aliveBytes(alive) {}
    TlsHandle connect(const char*) override { return (TlsHandle)1; }
    int writeSome(TlsHandle, const void*, size_t len) override {
        if (accepted >= aliveBytes) return 0;            // link is dead now
        size_t room = aliveBytes - accepted;
        size_t n = (len < room) ? len : room;
        accepted += n;
        return (int)n;
    }
    bool write(TlsHandle c, const void* d, size_t l) override { return netWriteAll(this, c, d, l); }
    int read(TlsHandle, void*, size_t) override { return 0; }
    void destroy(TlsHandle) override {}
};

// Writes a few bytes, then errors (<0).
class ErrorNetwork : public INetwork {
public:
    int wroteOnce = 0;
    TlsHandle connect(const char*) override { return (TlsHandle)1; }
    int writeSome(TlsHandle, const void*, size_t len) override {
        if (!wroteOnce) { wroteOnce = 1; return (int)(len / 2); }
        return -1;
    }
    bool write(TlsHandle c, const void* d, size_t l) override { return netWriteAll(this, c, d, l); }
    int read(TlsHandle, void*, size_t) override { return 0; }
    void destroy(TlsHandle) override {}
};

// ── Tests ────────────────────────────────────────────────────────────────────

void test_writes_all_bytes_across_chunks(void) {
    ChunkNetwork net(7);  // 7 bytes per writeSome call
    std::string payload = "the quick brown fox jumps over the lazy dog";  // 43 bytes
    bool ok = netWriteAll(&net, (TlsHandle)1, payload.data(), payload.size());
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(payload.size(), net.sink.size());
    TEST_ASSERT_TRUE(net.sink == payload);
    TEST_ASSERT_TRUE(net.calls >= 7);  // 43 / 7 -> 7 calls (last partial)
}

void test_single_call_full_buffer(void) {
    ChunkNetwork net(1024);  // bigger than payload -> one call
    std::string payload = "hello";
    TEST_ASSERT_TRUE(netWriteAll(&net, (TlsHandle)1, payload.data(), payload.size()));
    TEST_ASSERT_EQUAL_INT(1, net.calls);
}

void test_dead_link_times_out(void) {
    // StallNetwork never makes progress; TestClock::delay_ms advances the clock, so the
    // loop trips its own wall-clock no-progress bound. Default 30s -> fails, not spins.
    StallNetwork net;
    char buf[16] = {0};
    uint32_t start = g_hal->clock->millis();
    bool ok = netWriteAll(&net, (TlsHandle)1, buf, sizeof(buf));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE(g_hal->clock->millis() - start > 30000);  // waited the full timeout
}

void test_custom_timeout_honored(void) {
    StallNetwork net;
    char buf[16] = {0};
    uint32_t start = g_hal->clock->millis();
    bool ok = netWriteAll(&net, (TlsHandle)1, buf, sizeof(buf), 1000);  // 1s bound
    TEST_ASSERT_FALSE(ok);
    uint32_t elapsed = g_hal->clock->millis() - start;
    TEST_ASSERT_TRUE(elapsed > 1000);
    TEST_ASSERT_TRUE(elapsed < 30000);  // honored the short bound, didn't wait the default
}

void test_link_dies_mid_upload_is_bounded(void) {
    // Regression for the 9-hour field brick: link accepts 100 KB of a larger
    // upload, then dies. The OLD code spun forever here; netWriteAll must give up
    // after the no-progress timeout instead of hanging (and holding the SD mutex).
    LinkDiesNetwork net(100 * 1024);
    static char big[300 * 1024];
    uint32_t start = g_hal->clock->millis();
    bool ok = netWriteAll(&net, (TlsHandle)1, big, sizeof(big));
    TEST_ASSERT_FALSE(ok);
    uint32_t elapsed = g_hal->clock->millis() - start;
    // It made real progress (100 KB) then timed out ~30s after progress stopped —
    // bounded, NOT a 9-hour spin.
    TEST_ASSERT_EQUAL_UINT32(100u * 1024u, net.accepted);
    TEST_ASSERT_TRUE(elapsed > 30000);
    TEST_ASSERT_TRUE(elapsed < 35000);
}

void test_error_aborts_immediately(void) {
    ErrorNetwork net;
    char buf[64] = {0};
    uint32_t start = g_hal->clock->millis();
    bool ok = netWriteAll(&net, (TlsHandle)1, buf, sizeof(buf));
    TEST_ASSERT_FALSE(ok);
    // Errored on the 2nd call, no stall loop -> clock barely moved (no timeout wait).
    TEST_ASSERT_TRUE(g_hal->clock->millis() - start < 1000);
}

void test_zero_length_is_noop_success(void) {
    StallNetwork net;  // would block if called, but len==0 means the loop never runs
    TEST_ASSERT_TRUE(netWriteAll(&net, (TlsHandle)1, "", 0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_writes_all_bytes_across_chunks);
    RUN_TEST(test_single_call_full_buffer);
    RUN_TEST(test_dead_link_times_out);
    RUN_TEST(test_custom_timeout_honored);
    RUN_TEST(test_link_dies_mid_upload_is_bounded);
    RUN_TEST(test_error_aborts_immediately);
    RUN_TEST(test_zero_length_is_noop_success);
    return UNITY_END();
}

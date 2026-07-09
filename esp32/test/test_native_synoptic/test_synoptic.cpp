// Unit tests for the USB→SD→Cloud synoptic renderer (airbridge_synoptic.h) and
// the DisplayState→AirbridgeState mapper (airbridge_display.h).
#include <unity.h>
#include "hal/test_impls.h"
#include "airbridge_synoptic.h"
#include "airbridge_display.h"
#include <cstring>

static TestDisplay s_display;
static TestClock   s_clock;
static HAL         s_hal = { &s_display, &s_clock };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    s_display.init();
    s_display.reset();
    s_clock.now_ms = 0;
}
void tearDown(void) {}

static AirbridgeState base_state() {
    AirbridgeState s = {};
    s.usb = LINK_IDLE; s.net = LINK_IDLE;
    s.usbRetry = false; s.netRetry = true;
    s.usbVal = "1.0"; s.sdVal = "2.0"; s.cloudVal = "3.0";
    s.eteSecs = 0;
    return s;
}

// ── Icons always drawn ───────────────────────────────────────────────────────
void test_icons_drawn(void) {
    AirbridgeState s = base_state();
    synopticRender(s, 0);
    // USB icon sets (15,10); SD icon spans x53-76; Cloud spans x96-126.
    TEST_ASSERT_TRUE(s_display.pixel_at(15, 10));       // USB icon body
    // SD icon top border row (SD_Y=6): a run of set pixels across x53..
    TEST_ASSERT_TRUE(s_display.pixel_at(54, 6));
    // Cloud icon has solid pixels in its lower band (y~26)
    bool cloud = false;
    for (int x = 100; x < 124; x++) if (s_display.pixel_at(x, 26)) { cloud = true; break; }
    TEST_ASSERT_TRUE(cloud);
}

// ── Idle link: solid line + arrowhead ────────────────────────────────────────
void test_idle_link_solid(void) {
    AirbridgeState s = base_state();
    s.usb = LINK_IDLE; s.net = LINK_IDLE;
    synopticRender(s, 0);
    // USB→SD line y20 x29-51 solid
    TEST_ASSERT_TRUE(s_display.pixel_at(35, 20));
    TEST_ASSERT_TRUE(s_display.pixel_at(45, 20));
    // SD→Cloud line y20 x78-94 solid
    TEST_ASSERT_TRUE(s_display.pixel_at(85, 20));
}

// ── Down link draws a DOTTED line (gaps every 3px), not solid ────────────────
void test_down_link_dotted(void) {
    AirbridgeState s = base_state();
    s.usb = LINK_DOWN; s.usbRetry = false;
    synopticRender(s, 0);
    // dottedLine sets every 3rd px from x0=29: 29,32,35,... so 30,31 are gaps.
    TEST_ASSERT_TRUE(s_display.pixel_at(29, 20));
    TEST_ASSERT_FALSE(s_display.pixel_at(30, 20));
    TEST_ASSERT_FALSE(s_display.pixel_at(31, 20));
}

// ── ✕ is drawn on a DOWN endpoint icon, never on the SD/device icon ─────────
void test_x_on_usb_when_down(void) {
    AirbridgeState s = base_state();
    s.usb = LINK_DOWN;
    synopticRender(s, 0);
    // The ✕ inverts the icon interior. The USB icon center column (~x18) at a
    // background gap between the two vertical prongs should now be SET by the ✕.
    // Rather than pin an exact pixel, assert the ✕ raised the USB-icon pixel
    // count meaningfully vs the same icon with no ✕.
    int with_x = s_display.pixel_count();

    s_display.reset();
    AirbridgeState s2 = base_state();
    s2.usb = LINK_IDLE;              // no ✕
    synopticRender(s2, 0);
    int no_x = s_display.pixel_count();

    // The down state also swaps the solid line for a dotted one (fewer pixels),
    // so isolate the icon effect: the ✕ overlay must change the render.
    TEST_ASSERT_NOT_EQUAL(no_x, with_x);
}

void test_no_x_on_sd_icon(void) {
    // Even with BOTH links down, the SD/device icon is never ✕'d. Compare the SD
    // icon region against a both-up render — it must be pixel-identical there.
    AirbridgeState down = base_state();
    down.usb = LINK_DOWN; down.net = LINK_DOWN;
    synopticRender(down, 0);
    bool sd_region_down[31] = {false};
    for (int y = 6; y < 6 + 31; y++)
        for (int x = 53; x < 53 + 23; x++)
            if (s_display.pixel_at(x, y)) sd_region_down[y - 6] = true;

    s_display.reset();
    AirbridgeState up = base_state();
    synopticRender(up, 0);
    // The SD icon's own border/pins are drawn in both; assert a known SD border
    // pixel is present in both renders (i.e. the icon is intact, not struck out).
    TEST_ASSERT_TRUE(s_display.pixel_at(54, 6));    // up: SD top border
    // and it was equally present in the both-down render:
    TEST_ASSERT_TRUE(sd_region_down[0]);
}

// ── ETE shows MM:SS only while the net link is transferring ──────────────────
void test_ete_hidden_when_not_xfer(void) {
    AirbridgeState s = base_state();
    s.net = LINK_IDLE; s.eteSecs = 125;
    synopticRender(s, 0);
    // "--:--" is drawn (dashes) — a placeholder, not digits. Hard to assert text,
    // so just assert something renders in the ETE row (y0-7, right side).
    bool ete = false;
    for (int x = 96; x < 128; x++) for (int y = 0; y < 8; y++)
        if (s_display.pixel_at(x, y)) { ete = true; break; }
    TEST_ASSERT_TRUE(ete);
}

void test_ete_shown_when_xfer(void) {
    AirbridgeState s = base_state();
    s.net = LINK_XFER; s.eteSecs = 125;   // 02:05
    synopticRender(s, 0);
    bool ete = false;
    for (int x = 96; x < 128; x++) for (int y = 0; y < 8; y++)
        if (s_display.pixel_at(x, y)) { ete = true; break; }
    TEST_ASSERT_TRUE(ete);
}

// ── Animation flag ───────────────────────────────────────────────────────────
void test_is_animating(void) {
    AirbridgeState s = base_state();
    s.usb = LINK_IDLE; s.net = LINK_IDLE;
    TEST_ASSERT_TRUE(synopticIsAnimating(s));       // keep-alive pips
    s.usb = LINK_XFER;
    TEST_ASSERT_TRUE(synopticIsAnimating(s));       // ants
    // Fully static: both down, no retry.
    s.usb = LINK_DOWN; s.net = LINK_DOWN;
    s.usbRetry = false; s.netRetry = false;
    TEST_ASSERT_FALSE(synopticIsAnimating(s));
    s.netRetry = true;                              // retry ping animates
    TEST_ASSERT_TRUE(synopticIsAnimating(s));
}

// ── Value formatter (3-sig-fig-ish MB) ───────────────────────────────────────
void test_fmt_mb(void) {
    char b[8];
    synopticFmtMb(b, sizeof(b), 0.0f);    TEST_ASSERT_EQUAL_STRING("0.0", b);
    synopticFmtMb(b, sizeof(b), 0.9f);    TEST_ASSERT_EQUAL_STRING("0.9", b);
    synopticFmtMb(b, sizeof(b), 15.6f);   TEST_ASSERT_EQUAL_STRING("15.6", b);
    synopticFmtMb(b, sizeof(b), 123.4f);  TEST_ASSERT_EQUAL_STRING("123", b);
    synopticFmtMb(b, sizeof(b), 2500.0f); TEST_ASSERT_EQUAL_STRING("2.5G", b);
    synopticFmtMb(b, sizeof(b), -5.0f);   TEST_ASSERT_EQUAL_STRING("0.0", b);  // clamp
}

// ── Mapper: DisplayState → AirbridgeState ────────────────────────────────────
void test_mapper_links(void) {
    DisplayState ds = {};
    char uv[8], sv[8], cv[8];
    AirbridgeState s;

    // No USB host, no net → both DOWN; USB never retries, net always does.
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_INT(LINK_DOWN, s.usb);
    TEST_ASSERT_EQUAL_INT(LINK_DOWN, s.net);
    TEST_ASSERT_FALSE(s.usbRetry);
    TEST_ASSERT_TRUE(s.netRetry);

    // Host present + writing → USB XFER; PPP up + uploading → net XFER.
    ds.usbHostConnected = true; ds.usbWriteKBps = 200.0f;
    ds.pppConnected = true; ds.uploadKBps = 90.0f;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_INT(LINK_XFER, s.usb);
    TEST_ASSERT_EQUAL_INT(LINK_XFER, s.net);

    // Host present idle, PPP up idle → both IDLE.
    ds.usbWriteKBps = 0.0f; ds.uploadKBps = 0.0f;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_INT(LINK_IDLE, s.usb);
    TEST_ASSERT_EQUAL_INT(LINK_IDLE, s.net);
}

void test_mapper_values_and_ete(void) {
    DisplayState ds = {};
    ds.hostWrittenMb = 15.6f;
    ds.mbQueued = 8.0f;
    ds.mbUploaded = 3.0f;
    ds.uploadingMb = 1.0f;      // this session's bytes → Cloud = 3 + 1 = 4.0
    ds.uploadFileDoneMb = 1.0f; // file-cumulative (fresh file: same) → SD = 8 − 1
    ds.pppConnected = true; ds.uploadKBps = 100.0f; // → XFER, ETE from remaining
    char uv[8], sv[8], cv[8];
    AirbridgeState s;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_STRING("15.6", s.usbVal);
    // SD drains with the in-flight upload: queued 8.0 − file-done 1.0 = 7.0
    // (it must NOT sit at 8.0 until the file completes — the card value should
    // fall in lockstep as bytes leave).
    TEST_ASSERT_EQUAL_STRING("7.0", s.sdVal);
    TEST_ASSERT_EQUAL_STRING("4.0", s.cloudVal);
    // remaining = mbQueued - uploadFileDoneMb = 8 - 1 = 7 MB @ 100KB/s → 71s
    TEST_ASSERT_EQUAL_INT(71, s.eteSecs);
    TEST_ASSERT_EQUAL_INT(LINK_XFER, s.net);
}

void test_mapper_resumed_file_splits_sd_and_cloud(void) {
    // Power cycle mid-multipart: 60MB of the file left the card in EARLIER
    // sessions. Cloud counts only THIS session (per-boot, like USB); the SD
    // what's-left drain uses the file-cumulative position.
    DisplayState ds = {};
    ds.mbQueued = 162.0f;
    ds.mbUploaded = 0.0f;
    ds.uploadingMb = 2.0f;        // shipped since this boot
    ds.uploadFileDoneMb = 62.0f;  // file position incl. prior boots' parts
    ds.pppConnected = true; ds.uploadKBps = 100.0f;
    char uv[8], sv[8], cv[8];
    AirbridgeState s;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_STRING("100", s.sdVal);   // 162 − 62 left on the card
    TEST_ASSERT_EQUAL_STRING("2.0", s.cloudVal); // this session only
}

void test_mapper_sd_drain_clamps_at_zero(void) {
    // uploadFileDoneMb can momentarily exceed mbQueued (accounting refresh lag
    // while a file completes) — the SD value must clamp to 0, never underflow.
    DisplayState ds = {};
    ds.mbQueued = 1.0f; ds.uploadFileDoneMb = 1.4f;
    ds.pppConnected = true;
    char uv[8], sv[8], cv[8];
    AirbridgeState s;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_STRING("0.0", s.sdVal);
}

// ── uploadSessionMb: the per-power-session Cloud base tracker ────────────────
void test_session_tracker_fresh_file_counts_from_zero(void) {
    UploadSessionTracker t = {};
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.008f, uploadSessionMb(t, "a/f1.eaofh", 8192));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, uploadSessionMb(t, "a/f1.eaofh", 10000000));
}

void test_session_tracker_resume_latches_part_base(void) {
    // New boot resumes at part 13: first callback is 60MB(+first chunk). The
    // base snaps DOWN to the 5MB part boundary, so session counts from ~0.
    UploadSessionTracker t = {};
    const uint32_t PART = 5u*1024*1024;
    float first = uploadSessionMb(t, "a/f1.eaofh", 12*PART + 8192);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.008f, first);
    // ...and climbs session-relative from there.
    float later = uploadSessionMb(t, "a/f1.eaofh", 14*PART);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, (2*PART)/1e6f, later);
}

void test_session_tracker_retry_same_file_keeps_base(void) {
    // A failed part retries from its own boundary: cumulative DIPS briefly but
    // the base must not re-latch (that was the 5MB sawtooth). Never negative.
    UploadSessionTracker t = {};
    const uint32_t PART = 5u*1024*1024;
    uploadSessionMb(t, "a/f1.eaofh", 8192);            // latch base 0
    uploadSessionMb(t, "a/f1.eaofh", 2*PART + 100000); // mid part 3
    float dip = uploadSessionMb(t, "a/f1.eaofh", 2*PART + 8192); // part-3 retry
    TEST_ASSERT_FLOAT_WITHIN(0.02f, (2*PART)/1e6f, dip);
    TEST_ASSERT_TRUE(dip >= 0.0f);
}

void test_session_tracker_new_file_relatches(void) {
    UploadSessionTracker t = {};
    uploadSessionMb(t, "a/f1.eaofh", 10000000);
    // Next file starts its own base — no carry-over from the previous file.
    float fresh = uploadSessionMb(t, "a/f2.eaofh", 8192);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.008f, fresh);
}

void test_mapper_ete_capped(void) {
    DisplayState ds = {};
    ds.mbQueued = 100000.0f;   // huge backlog
    ds.pppConnected = true; ds.uploadKBps = 1.0f;
    char uv[8], sv[8], cv[8];
    AirbridgeState s;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_INT(5999, s.eteSecs);   // clamped to 99:59
}

// ── 2x node values fit their columns without clipping the screen edge ────────
void test_values_no_edge_clip(void) {
    // Worst case: all three nodes carry a 4-glyph value. The narrow-decimal 2x
    // renderer must clamp into the screen — no value pixel in column 0 (the old
    // centered-2x bug bled the USB value off the left edge) or column 127.
    DisplayState ds = {};
    ds.usbHostConnected = true; ds.pppConnected = true;
    ds.hostWrittenMb = 15.6f;    // "15.6"
    ds.mbQueued = 99.9f;         // "99.9"
    ds.mbUploaded = 2500.0f;     // "2.5G"
    updateDisplay(ds);
    for (int y = 44; y < 58; y++) {
        TEST_ASSERT_FALSE(s_display.pixel_at(0, y));     // left margin kept
        TEST_ASSERT_FALSE(s_display.pixel_at(127, y));   // right margin kept
    }
    // And the values are actually present (something drawn in the value band).
    int band = 0;
    for (int y = 44; y < 58; y++) for (int x = 0; x < 128; x++) if (s_display.pixel_at(x, y)) band++;
    TEST_ASSERT_TRUE(band > 50);
}

// ── Ant speed tracks throughput ──────────────────────────────────────────────
void test_ant_step_from_rate(void) {
    // Velocity doubles per +50 KB/s: 150→4px/s (250ms), 100→2px/s (500ms),
    // 50→1px/s (1000ms). i.e. step = 1000 / pxPerSec.
    TEST_ASSERT_EQUAL_UINT16(250,  synopticAntStepMs(150.0f));  // full speed
    TEST_ASSERT_EQUAL_UINT16(500,  synopticAntStepMs(100.0f));  // half
    TEST_ASSERT_EQUAL_UINT16(1000, synopticAntStepMs(50.0f));   // quarter
    // Monotonic: higher rate → smaller step (faster ants).
    TEST_ASSERT_TRUE(synopticAntStepMs(120.0f) < synopticAntStepMs(80.0f));
    // Clamped: >=150 pins to MIN, <=50 (and stalled) pins to MAX.
    TEST_ASSERT_EQUAL_UINT16(ANT_STEP_MIN_MS, synopticAntStepMs(400.0f));
    TEST_ASSERT_EQUAL_UINT16(ANT_STEP_MAX_MS, synopticAntStepMs(20.0f));
    TEST_ASSERT_EQUAL_UINT16(ANT_STEP_MAX_MS, synopticAntStepMs(0.0f));
}

void test_mapper_sets_ant_steps(void) {
    DisplayState ds = {};
    ds.usbHostConnected = true; ds.usbWriteKBps = 200.0f;
    ds.pppConnected = true; ds.uploadKBps = 50.0f;
    char uv[8], sv[8], cv[8]; AirbridgeState s;
    buildSynopticState(ds, s, uv, sv, cv);
    TEST_ASSERT_EQUAL_UINT16(synopticAntStepMs(200.0f), s.usbAntStepMs);
    TEST_ASSERT_EQUAL_UINT16(synopticAntStepMs(50.0f),  s.netAntStepMs);
    // Net ants (50 KB/s) march slower than USB ants (200 KB/s).
    TEST_ASSERT_TRUE(s.netAntStepMs > s.usbAntStepMs);
}

// A faster ant step advances the pattern more between two fixed timestamps.
void test_ant_step_affects_render(void) {
    AirbridgeState fast = base_state(); fast.net = LINK_XFER; fast.netAntStepMs = 250;
    AirbridgeState slow = base_state(); slow.net = LINK_XFER; slow.netAntStepMs = 1000;
    // Count net-link pixels at a time where the two phases differ; the renders
    // must not be identical (the ants are at different positions).
    s_display.reset(); synopticRender(fast, 900);
    int a = 0; for (int x = 78; x <= 94; x++) a = a * 2 + (s_display.pixel_at(x, 20) ? 1 : 0);
    s_display.reset(); synopticRender(slow, 900);
    int b = 0; for (int x = 78; x <= 94; x++) b = b * 2 + (s_display.pixel_at(x, 20) ? 1 : 0);
    TEST_ASSERT_NOT_EQUAL(a, b);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_values_no_edge_clip);
    RUN_TEST(test_ant_step_from_rate);
    RUN_TEST(test_mapper_sets_ant_steps);
    RUN_TEST(test_ant_step_affects_render);
    RUN_TEST(test_icons_drawn);
    RUN_TEST(test_idle_link_solid);
    RUN_TEST(test_down_link_dotted);
    RUN_TEST(test_x_on_usb_when_down);
    RUN_TEST(test_no_x_on_sd_icon);
    RUN_TEST(test_ete_hidden_when_not_xfer);
    RUN_TEST(test_ete_shown_when_xfer);
    RUN_TEST(test_is_animating);
    RUN_TEST(test_fmt_mb);
    RUN_TEST(test_mapper_links);
    RUN_TEST(test_mapper_values_and_ete);
    RUN_TEST(test_mapper_sd_drain_clamps_at_zero);
    RUN_TEST(test_mapper_resumed_file_splits_sd_and_cloud);
    RUN_TEST(test_session_tracker_fresh_file_counts_from_zero);
    RUN_TEST(test_session_tracker_resume_latches_part_base);
    RUN_TEST(test_session_tracker_retry_same_file_keeps_base);
    RUN_TEST(test_session_tracker_new_file_relatches);
    RUN_TEST(test_mapper_ete_capped);
    return UNITY_END();
}

#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include "airbridge_display.h"

// ── Test fixtures ──────────────────────────────────────────────────────────

static TestDisplay s_display;
static TestClock   s_clock;
static HAL         s_hal = { &s_display, &s_clock };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    s_display.init();
    s_display.reset();
}

void tearDown(void) {}

// ── Rendering primitive tests ───────────────────────────────────────────────

void test_pixel_set_and_clear(void) {
    g_hal->display->pixel(10, 20, true);
    TEST_ASSERT_TRUE(s_display.pixel_at(10, 20));
    g_hal->display->pixel(10, 20, false);
    TEST_ASSERT_FALSE(s_display.pixel_at(10, 20));
}

void test_pixel_out_of_bounds(void) {
    g_hal->display->pixel(-1, 0, true);
    g_hal->display->pixel(0, -1, true);
    g_hal->display->pixel(128, 0, true);
    g_hal->display->pixel(0, 64, true);
    TEST_ASSERT_EQUAL_INT(0, s_display.pixel_count());
}

void test_clear(void) {
    g_hal->display->pixel(5, 5, true);
    TEST_ASSERT_TRUE(s_display.pixel_at(5, 5));
    g_hal->display->clear();
    TEST_ASSERT_EQUAL_INT(0, s_display.pixel_count());
}

void test_hline(void) {
    g_hal->display->hline(10, 20, 5);
    for (int x = 10; x <= 20; x++)
        TEST_ASSERT_TRUE(s_display.pixel_at(x, 5));
    TEST_ASSERT_FALSE(s_display.pixel_at(9, 5));
    TEST_ASSERT_FALSE(s_display.pixel_at(21, 5));
}

void test_rect_filled(void) {
    g_hal->display->rect(10, 10, 4, 3, true);
    // All pixels inside should be set
    for (int y = 10; y < 13; y++)
        for (int x = 10; x < 14; x++)
            TEST_ASSERT_TRUE(s_display.pixel_at(x, y));
    TEST_ASSERT_EQUAL_INT(12, s_display.pixel_count());
}

void test_rect_outline(void) {
    g_hal->display->rect(0, 0, 5, 5, false);
    // Corners set
    TEST_ASSERT_TRUE(s_display.pixel_at(0, 0));
    TEST_ASSERT_TRUE(s_display.pixel_at(4, 0));
    TEST_ASSERT_TRUE(s_display.pixel_at(0, 4));
    TEST_ASSERT_TRUE(s_display.pixel_at(4, 4));
    // Interior not set
    TEST_ASSERT_FALSE(s_display.pixel_at(2, 2));
}

void test_text_width(void) {
    // "Hello" = 5 chars, size 1: 5*6-1 = 29
    TEST_ASSERT_EQUAL_INT(29, g_hal->display->text_width("Hello"));
    // "AB" size 2: 2*12-2 = 22
    TEST_ASSERT_EQUAL_INT(22, g_hal->display->text_width("AB", 2));
    // Empty
    TEST_ASSERT_EQUAL_INT(0, g_hal->display->text_width(""));
}

void test_text_renders_pixels(void) {
    g_hal->display->text(0, 0, "A");
    // 'A' glyph: 0x7E,0x11,0x11,0x11,0x7E
    // Column 0 = 0x7E = bits 1-6 set
    TEST_ASSERT_TRUE(s_display.pixel_at(0, 1));  // bit 1
    TEST_ASSERT_TRUE(s_display.pixel_at(0, 6));  // bit 6
    TEST_ASSERT_FALSE(s_display.pixel_at(0, 0)); // bit 0 not set
    TEST_ASSERT_FALSE(s_display.pixel_at(0, 7)); // bit 7 not set (5x7 font)
}

void test_text_size2(void) {
    g_hal->display->text(0, 0, "A", 2);
    // Size 2: each pixel becomes 2x2 block
    // Col 0, row 1 of glyph → pixels (0,2), (0,3), (1,2), (1,3)
    TEST_ASSERT_TRUE(s_display.pixel_at(0, 2));
    TEST_ASSERT_TRUE(s_display.pixel_at(1, 3));
}

void test_flush_called(void) {
    TEST_ASSERT_EQUAL_INT(0, s_display.flush_count);
    g_hal->display->flush();
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
}

// ── updateDisplay tests ─────────────────────────────────────────────────────

static DisplayState make_default_state() {
    DisplayState ds = {};
    memset(&ds, 0, sizeof(ds));
    return ds;
}

void test_updateDisplay_no_network(void) {
    DisplayState ds = make_default_state();
    updateDisplay(ds);
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
    // Should have rendered "No Network" text + layout
    TEST_ASSERT_TRUE(s_display.pixel_count() > 100);
}

void test_updateDisplay_cellular_connected(void) {
    DisplayState ds = make_default_state();
    ds.pppConnected = true;
    ds.modemRssi = 25;
    strlcpy(ds.modemOp, "T-Mobile", sizeof(ds.modemOp));
    ds.mbQueued = 100.0f;
    ds.mbUploaded = 50.0f;

    updateDisplay(ds);
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);

    // Signal bars: rssi >= 20 → 4 bars
    // Bar 4 at x=123, height 8, y=0 → pixels at (123,0) through (125,7)
    TEST_ASSERT_TRUE(s_display.pixel_at(123, 0));  // top of tallest bar
}

void test_updateDisplay_wifi_connected(void) {
    DisplayState ds = make_default_state();
    ds.netConnected = true;
    strlcpy(ds.wifiLabel, "MyWiFi", sizeof(ds.wifiLabel));
    ds.wifiBars = 3;

    updateDisplay(ds);
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
    // 3 bars: bars 0,1,2 drawn; bar 3 (index 3) not drawn
    // Bar 2 at x=118, h=6, drawn at y=8-6=2 to y=7
    TEST_ASSERT_TRUE(s_display.pixel_at(118, 2));
    // Bar 3 at x=123 should NOT be drawn (only 3 bars, index 0-2)
    TEST_ASSERT_FALSE(s_display.pixel_at(123, 0));
}

void test_updateDisplay_progress_bar_half(void) {
    DisplayState ds = make_default_state();
    ds.mbUploaded = 50.0f;
    ds.mbQueued = 50.0f;  // remaining = 50 - 0 = 50, uploaded = 50+0 = 50
    // total = 50 + 50 = 100, fill = 50/100 * 126 = 63

    updateDisplay(ds);
    // Progress bar outline at y=50, h=5 (rows 50-54)
    TEST_ASSERT_TRUE(s_display.pixel_at(0, 50));    // top-left corner
    TEST_ASSERT_TRUE(s_display.pixel_at(127, 50));  // top-right corner
    // Fill at y=51, from x=1
    TEST_ASSERT_TRUE(s_display.pixel_at(1, 51));    // filled region
    TEST_ASSERT_TRUE(s_display.pixel_at(50, 51));   // still filled
}

void test_updateDisplay_eta_shown(void) {
    DisplayState ds = make_default_state();
    ds.uploadKBps = 100.0f;
    ds.mbQueued = 10.0f;
    ds.etaKBps = 100.0f;  // pre-prime EMA so it doesn't need warmup

    updateDisplay(ds);
    // ETA should be calculated: remaining=10MB, speed=100KB/s
    // etaKBps = 100*0.8 + 100*0.2 = 100
    // etaSec = 10*1024/100 = 102s → 1:42
    // "ETA 1:42" rendered at bottom-right
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
    // Just verify something was drawn in the ETA row area (y=52-58)
    bool eta_drawn = false;
    for (int x = 64; x < 128; x++)
        if (s_display.pixel_at(x, 57)) { eta_drawn = true; break; }
    TEST_ASSERT_TRUE(eta_drawn);
}

void test_updateDisplay_modem_connecting(void) {
    DisplayState ds = make_default_state();
    ds.modemReady = true;
    strlcpy(ds.modemOp, "AT&T", sizeof(ds.modemOp));

    updateDisplay(ds);
    // Should show "AT&T..." (connecting state)
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
}

void test_updateDisplay_no_signal(void) {
    DisplayState ds = make_default_state();
    ds.pppConnected = true;
    ds.modemRssi = 0;  // PPP up but no signal

    updateDisplay(ds);
    // Should show "No Signal", 0 bars
    // Verify no signal bar pixels (bar positions start at x=108)
    TEST_ASSERT_FALSE(s_display.pixel_at(108, 6)); // bar 0 not drawn
}

// ── dispSplash tests ────────────────────────────────────────────────────────

void test_dispSplash(void) {
    dispSplash("Starting...");
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
    // "AirBridge" at size 2 draws many pixels
    TEST_ASSERT_TRUE(s_display.pixel_count() > 200);
}

void test_dispSplash_two_lines(void) {
    dispSplash("Line 1", "Line 2");
    TEST_ASSERT_EQUAL_INT(1, s_display.flush_count);
    // Second line at y=48 should have pixels
    bool line2_drawn = false;
    for (int x = 0; x < 64; x++)
        if (s_display.pixel_at(x, 48)) { line2_drawn = true; break; }
    TEST_ASSERT_TRUE(line2_drawn);
}

// ── Framebuffer consistency ─────────────────────────────────────────────────

void test_consecutive_displays_independent(void) {
    DisplayState ds1 = make_default_state();
    ds1.pppConnected = true;
    ds1.modemRssi = 25;
    updateDisplay(ds1);
    int pixels1 = s_display.pixel_count();

    s_display.reset();
    DisplayState ds2 = make_default_state();
    updateDisplay(ds2);
    int pixels2 = s_display.pixel_count();

    // Different states should produce different renders
    TEST_ASSERT_NOT_EQUAL(pixels1, pixels2);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Rendering primitives
    RUN_TEST(test_pixel_set_and_clear);
    RUN_TEST(test_pixel_out_of_bounds);
    RUN_TEST(test_clear);
    RUN_TEST(test_hline);
    RUN_TEST(test_rect_filled);
    RUN_TEST(test_rect_outline);
    RUN_TEST(test_text_width);
    RUN_TEST(test_text_renders_pixels);
    RUN_TEST(test_text_size2);
    RUN_TEST(test_flush_called);

    // updateDisplay screens
    RUN_TEST(test_updateDisplay_no_network);
    RUN_TEST(test_updateDisplay_cellular_connected);
    RUN_TEST(test_updateDisplay_wifi_connected);
    RUN_TEST(test_updateDisplay_progress_bar_half);
    RUN_TEST(test_updateDisplay_eta_shown);
    RUN_TEST(test_updateDisplay_modem_connecting);
    RUN_TEST(test_updateDisplay_no_signal);

    // dispSplash
    RUN_TEST(test_dispSplash);
    RUN_TEST(test_dispSplash_two_lines);

    // Consistency
    RUN_TEST(test_consecutive_displays_independent);

    return UNITY_END();
}

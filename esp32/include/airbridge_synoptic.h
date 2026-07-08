#pragma once
// ============================================================================
//  airbridge_synoptic.h — USB → SD → Cloud synoptic display renderer
//
//  Ported from the design package's Adafruit_GFX renderer (airbridge_ui.cpp) to
//  the AirBridge HAL (g_hal->display), so it compiles into firmware + emulator +
//  unit tests identically. Assets (icon sprites + layout) live in
//  airbridge_display_assets.h.
//
//  Visual grammar (unchanged from the design):
//    * The LINE between two icons only ever shows TRAFFIC:
//        - quiet/solid + keep-alive pip  → connected, idle (proves not-frozen)
//        - marching ants (toward dest)   → transferring
//        - dotted + retry ping / quiet   → link down
//    * The ENDPOINT ICON carries an ✕ when its side is unreachable (USB / Cloud).
//    * The SD/device icon is NEVER ✕'d — it is the product, alive whenever the
//      screen is drawing.
//  Stateless: all animation derives from the nowMs passed in.
// ============================================================================
#include "hal/hal.h"
#include "airbridge_display_assets.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>

// Traffic state of a link (the LINE between two icons).
enum LinkState {
    LINK_DOWN = 0,   // endpoint unreachable  → ✕ on the icon + line ping/quiet
    LINK_IDLE = 1,   // connected, no traffic → solid line + slow keep-alive pip
    LINK_XFER = 2    // connected, transferring → marching ants (direction = flow)
};

// Everything the renderer needs for one frame. Populate from your app flags.
struct AirbridgeState {
    LinkState   usb;        // USB link (source → device)
    LinkState   net;        // internet link (device → cloud)
    bool        usbRetry;   // when usb==LINK_DOWN: true = ping ("still knocking"), false = quiet
    bool        netRetry;   // when net==LINK_DOWN: true = ping, false = quiet
    const char* usbVal;     // 3 sig figs under each node, e.g. "0.9" "15.6" "123"
    const char* sdVal;
    const char* cloudVal;
    uint16_t    eteSecs;    // upload ETE (s); shown MM:SS only while net==LINK_XFER
    uint16_t    usbAntStepMs; // ant step (ms/px) while usb==LINK_XFER; 0 = nominal
    uint16_t    netAntStepMs; // ant step (ms/px) while net==LINK_XFER; 0 = nominal
};

// ── animation timing (ms) — override before including if desired ──────────────
// Ant speed is tuned for the device's 4Hz (250ms) display refresh: at ANT_STEP_MIN_MS
// (=250) the ants advance exactly 1px per frame — the fastest that reads as smooth
// forward motion without aliasing the 4px pattern. Slower rates use a larger step so
// the ants advance <1px/frame (they visibly crawl).
#ifndef ANT_STEP_MS
#define ANT_STEP_MS      250    // nominal/fallback step (ms/px) = full speed @ 4Hz
#endif
#ifndef ANT_STEP_MIN_MS
#define ANT_STEP_MIN_MS  250    // fastest ants (>=150 KB/s) — 1px/frame @ 4Hz
#endif
#ifndef ANT_STEP_MAX_MS
#define ANT_STEP_MAX_MS 1000    // slowest ants (<=50 KB/s)  — 1px/sec
#endif
#ifndef KEEPALIVE_MS
#define KEEPALIVE_MS      2400  // idle keep-alive pip period
#endif
#ifndef KEEPALIVE_RUN_MS
#define KEEPALIVE_RUN_MS  600   // how long the pip takes to cross the line
#endif
#ifndef RETRY_MS
#define RETRY_MS          1300  // retry-ping period (a 2-pulse train)
#endif

// Ant step (ms/px) for a transfer at `kbps`. Ant *velocity* doubles per +50 KB/s:
// ~150 KB/s → 4 px/s (full speed @ 4Hz), 100 → 2 px/s, 50 → 1 px/s, clamped to
// [ANT_STEP_MIN_MS, ANT_STEP_MAX_MS] (i.e. 250..1000 ms/px, i.e. 4..1 px/s).
inline uint16_t synopticAntStepMs(float kbps) {
    if (kbps < 1.0f) return ANT_STEP_MAX_MS;              // stalled → slowest crawl
    float pxPerSec = exp2f((kbps - 50.0f) / 50.0f);       // 50→1, 100→2, 150→4
    if (pxPerSec < 1.0f) pxPerSec = 1.0f;
    if (pxPerSec > 4.0f) pxPerSec = 4.0f;
    return (uint16_t)(1000.0f / pxPerSec);                // ms per px (250..1000)
}

// True whenever the synoptic view is animating (a link is transferring, idle
// keep-alive, or retry-pinging) — i.e. the display should refresh faster than the
// static 1 Hz cadence to keep motion smooth. Purely a function of the state.
inline bool synopticIsAnimating(const AirbridgeState& s) {
    return s.usb == LINK_XFER || s.net == LINK_XFER
        || s.usb == LINK_IDLE || s.net == LINK_IDLE
        || (s.usb == LINK_DOWN && s.usbRetry)
        || (s.net == LINK_DOWN && s.netRetry);
}

namespace synoptic_detail {

// ── ✕ drawn THROUGH an icon: white over background, black where it crosses the
//    icon's solid pixels, so it reads at any position. 2 px thick. ────────────
inline void xThroughIcon(const uint8_t* bmp, int w, int h, int ox, int oy, int inset) {
    int ax0 = inset, ay0 = inset, ax1 = w - 1 - inset, ay1 = h - 1 - inset;
    int n = ax1 - ax0;
    if (n <= 0) return;
    for (int d = 0; d < 2; d++) {
        int sx = (d == 0) ? ax0 : ax1;
        int ex = (d == 0) ? ax1 : ax0;
        for (int i = 0; i <= n; i++) {
            int lx = sx + (ex - sx) * i / n;
            int ly = ay0 + (ay1 - ay0) * i / n;
            for (int t = 0; t < 2; t++) {                 // 2 px thick
                int px = lx + t, py = ly;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                bool on = !IDisplay::bitmap_bit(bmp, w, px, py); // invert over the icon
                g_hal->display->pixel(ox + px, oy + py, on);
            }
        }
    }
}

inline void arrowHead(int x1, int y) {
    IDisplay* d = g_hal->display;
    d->pixel(x1, y, true);
    d->pixel(x1 - 1, y - 1, true); d->pixel(x1 - 1, y + 1, true);
    d->pixel(x1 - 2, y - 2, true); d->pixel(x1 - 2, y + 2, true);
}

inline void dottedLine(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x += 3) g_hal->display->pixel(x, y, true);
}

// ── one link's LINE (never draws the ✕; that's on the icon) ──────────────────
// arrowDx nudges the arrowhead along x (e.g. +3 to seat it against the cloud).
// antStepMs is the ms/px march rate while transferring (0 = nominal ANT_STEP_MS).
inline void drawLink(int x0, int x1, int y, LinkState st, bool retry, uint32_t now,
                     int arrowDx = 0, uint32_t antStepMs = 0) {
    IDisplay* d = g_hal->display;
    int len = x1 - x0;
    if (st == LINK_XFER) {                                // marching ants toward x1
        if (antStepMs == 0) antStepMs = ANT_STEP_MS;
        long phase = now / antStepMs;
        for (int x = x0; x <= x1; x++) {
            long idx = ((x - x0 - phase) % 4 + 4) % 4;
            if (idx < 2) d->pixel(x, y, true);
        }
        arrowHead(x1 + arrowDx, y);
    } else if (st == LINK_IDLE) {                          // solid + keep-alive pip
        d->hline(x0, x1, y);
        arrowHead(x1 + arrowDx, y);
        uint32_t lt = now % KEEPALIVE_MS;
        if (lt < KEEPALIVE_RUN_MS) {
            int p = x0 + (int)((long)lt * len / KEEPALIVE_RUN_MS);
            d->pixel(p, y - 1, true); d->pixel(p, y + 1, true);
            d->pixel(p + 1, y, true);
        }
    } else {                                               // LINK_DOWN
        dottedLine(x0, x1, y);
        if (retry) {                                       // 2-pulse ping train toward endpoint
            for (int i = 0; i < 2; i++) {
                uint32_t ph = (now + (uint32_t)i * (RETRY_MS / 2)) % RETRY_MS;
                int p = x0 + (int)((long)ph * len / RETRY_MS);
                d->pixel(p, y, true);     d->pixel(p + 1, y, true);
                d->pixel(p, y - 1, true); d->pixel(p, y + 1, true);
            }
        }
    }
}

// Center 5x7 HAL text (any scale) horizontally about cx.
inline void textCentered(int cx, int y, const char* s, int size) {
    int w = g_hal->display->text_width(s, size);
    g_hal->display->text(cx - w / 2, y, s, size);
}

// Draw a node VALUE at 2x with a NARROW decimal point: the '.' advances only
// VAL_DOT_ADV px (a small block) instead of a full 12px glyph, so a 4-glyph value
// like "15.6" or "2.5G" fits under an edge-hugging node. Centered about cx, then
// clamped to the screen so the outer nodes (USB @18, Cloud @107) never clip.
inline void drawValue2x(int cx, int y, const char* s) {
    IDisplay* d = g_hal->display;
    const int DIG = 12;      // digit/letter advance at 2x (10px glyph + 2px gap)
    const int DOT = 5;       // narrow decimal advance (3px block + 2px gap)
    if (!s || !*s) return;
    int w = 0;
    for (const char* p = s; *p; ++p) w += (*p == '.') ? DOT : DIG;
    w -= 2;                                   // drop the trailing inter-char gap
    int x = cx - w / 2;
    if (x < 1) x = 1;                         // keep a 1px left margin
    if (x + w > SCREEN_W - 1) x = SCREEN_W - 1 - w;
    for (const char* p = s; *p; ++p) {
        if (*p == '.') { d->rect(x, y + 10, 3, 3, true); x += DOT; }  // baseline dot
        else           { d->draw_char(x, y, *p, 2);      x += DIG; }
    }
}

} // namespace synoptic_detail

// Draw the full baked frame (splash / boot screen), using the design's frame BMP.
inline void synopticSplash() {
    if (!g_hal || !g_hal->display) return;
    g_hal->display->clear();
    g_hal->display->bitmap(0, 0, FRAME_BMP, FRAME_W, FRAME_H);
    g_hal->display->flush();
}

// Draw one synoptic frame into the framebuffer (caller clears + flushes).
// Stateless — all animation is derived from `now` (millis()).
inline void synopticRender(const AirbridgeState& s, uint32_t now) {
    using namespace synoptic_detail;
    if (!g_hal || !g_hal->display) return;
    IDisplay* d = g_hal->display;

    // icons
    d->bitmap(USB_X,   USB_Y,   USB_BMP,   USB_W,   USB_H);
    d->bitmap(SD_X,    SD_Y,    SD_BMP,    SD_W,    SD_H);
    d->bitmap(CLOUD_X, CLOUD_Y, CLOUD_BMP, CLOUD_W, CLOUD_H);

    // ✕ on the unreachable endpoint icon (never the SD/device icon)
    if (s.usb == LINK_DOWN) xThroughIcon(USB_BMP,   USB_W,   USB_H,   USB_X,   USB_Y,   2);
    // inset 0: the cloud ✕ spans the full icon bounding box (corner to corner).
    if (s.net == LINK_DOWN) xThroughIcon(CLOUD_BMP, CLOUD_W, CLOUD_H, CLOUD_X, CLOUD_Y, 0);

    // links (line traffic only)
    drawLink(USBLINK_X0, USBLINK_X1, USBLINK_Y, s.usb, s.usbRetry, now,
             /*arrowDx=*/0, s.usbAntStepMs);
    drawLink(NETLINK_X0, NETLINK_X1, NETLINK_Y, s.net, s.netRetry, now,
             /*arrowDx=*/3, s.netAntStepMs);

    // ETE (top-right) — only meaningful while uploading. eteSecs is capped at
    // 5999 (99:59) upstream, but size the buffer so the formatter can't truncate.
    char ete[8];
    if (s.net == LINK_XFER) {
        unsigned mm = s.eteSecs / 60, ss = s.eteSecs % 60;
        if (mm > 99) mm = 99;
        snprintf(ete, sizeof(ete), "%02u:%02u", mm, ss);
    } else {
        strncpy(ete, "--:--", sizeof(ete));
    }
    textCentered(ETE_CX, ETE_Y, ete, 1);

    // Values under each node at 2x, with a narrow decimal point so a 3-sig-fig
    // value ("15.6", "2.5G") fits under the edge-hugging outer nodes.
    drawValue2x(NUM_USB_CX,   NUM_Y, s.usbVal   ? s.usbVal   : "");
    drawValue2x(NUM_SD_CX,    NUM_Y, s.sdVal    ? s.sdVal    : "");
    drawValue2x(NUM_CLOUD_CX, NUM_Y, s.cloudVal ? s.cloudVal : "");
}

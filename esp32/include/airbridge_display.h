#pragma once
// AirBridge display rendering — extracted from main.cpp for testing
// All functions use g_hal->display for rendering.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_synoptic.h"
#include <cstdio>

// State snapshot passed to updateDisplay (replaces direct global reads)
struct DisplayState {
    bool     pppConnected;
    bool     netConnected;
    bool     modemReady;
    bool     usbHostConnected; // an active host has enumerated our MSC volume
                               // (tud_mounted). false = powered but no host → USB link ✕.
    int      modemRssi;
    float    linkLatencyMs; // windowed-best request round-trip (ms) — connection-quality signal
    uint32_t linkAgeMs;     // ms since the last successful request (liveness)
    char     modemOp[32];
    char     wifiLabel[22];
    int8_t   wifiBars;
    float    hostWrittenMb;
    float    mbUploaded;
    float    mbQueued;
    float    uploadingMb;
    float    usbWriteKBps;
    float    uploadKBps;
    // OTA overlay (shown instead of USB/UPLOAD gauges when active)
    bool     otaActive;
    int      otaPct;       // -1=checking, 0-100=downloading
    char     otaVersion[20];
    // SD-fault overlay (shown when the card is unusable / being recovered)
    bool     sdError;
    bool     sdReformatting; // true once a destructive reformat has been scheduled
};

// Sliding-window over recent samples (value = whatever the caller stores: here,
// request round-trip latency in ms). Pure (nowMs passed in) so it's unit-testable.
// Iterating 0..count-1 covers every stored slot (the ring fills 0..CAP-1 then wraps).
struct LinkWindow {
    static const int CAP = 16;
    uint32_t ms[CAP];
    float    val[CAP];
    uint8_t  count;
    uint8_t  head;
};
inline void linkWindowAdd(LinkWindow& w, uint32_t nowMs, float val) {
    w.ms[w.head] = nowMs;
    w.val[w.head] = val;
    w.head = (uint8_t)((w.head + 1) % LinkWindow::CAP);
    if (w.count < LinkWindow::CAP) w.count++;
}
// Best (min) value in the window; returns <0 if no sample in window.
inline float linkWindowMin(const LinkWindow& w, uint32_t nowMs, uint32_t windowMs) {
    float mn = -1;
    for (int i = 0; i < w.count; i++)
        if ((uint32_t)(nowMs - w.ms[i]) <= windowMs && (mn < 0 || w.val[i] < mn)) mn = w.val[i];
    return mn;
}

// Connection-quality bars (0-4) from request round-trip LATENCY (ms). RSSI can't be
// read mid-PPP without +++ (disrupts data) and small transfers can't measure
// bandwidth, but they CAN measure latency — which tracks cellular quality well
// (good signal => low RTT). The 60s log upload gives a fresh latency sample every
// minute with no extra traffic; `rttMs` is the windowed MIN (best recent) so the
// bars read like a steady signal meter. `ageMs` = time since the last successful
// request; stale (>3 missed cycles) or no sample => 0 bars.
//
// Thresholds CALIBRATED from field logs: the log POST and presign/manifest all hit
// API Gateway + Lambda, so server processing dominates the floor — round-trips run
// ~1-3s (typ. ~2s) even on a strong link (RSSI 20). So this metric is best at
// flagging DEGRADATION (>4s = link struggling), not distinguishing great-vs-good
// (both ~1-2s). The windowed MIN excludes Lambda cold-start spikes.
inline int linkQualityBarsLatency(float rttMs, uint32_t ageMs) {
    if (ageMs > 200000 || rttMs < 0) return 0;     // no recent successful request
    if (rttMs < 2000)  return 4;                   // excellent (~floor: strong link)
    if (rttMs < 4000)  return 3;                   // good
    if (rttMs < 7000)  return 2;                   // fair
    if (rttMs < 15000) return 1;                   // poor — link struggling
    return 0;                                       // very poor / near-timeout
}

// Format a cumulative-MB value to a compact ~3-significant-figure string for a
// synoptic node ("0.9", "15.6", "123", "1.2G"). Kept short so the 2x-font value
// stays within its column even at large sizes.
inline void synopticFmtMb(char* buf, size_t n, float mb) {
    if (mb < 0) mb = 0;
    if (mb >= 1000.0f)     snprintf(buf, n, "%.1fG", mb / 1000.0f);   // >= 1 GB
    else if (mb >= 100.0f) snprintf(buf, n, "%.0f", mb);              // 100-999 MB
    else                   snprintf(buf, n, "%.1f", mb);              // < 100 MB (0.0-99.9)
}

// Map the rich DisplayState onto the synoptic AirbridgeState. Value buffers are
// caller-owned (live for the render call). Cumulative-MB node values:
//   USB = written this session, SD = queued on card, Cloud = uploaded.
inline void buildSynopticState(const DisplayState& ds, AirbridgeState& s,
                               char uv[8], char sv[8], char cv[8]) {
    // USB link, three states:
    //   no host connected (powered only) → LINK_DOWN → ✕ on the USB icon
    //   host connected + writing          → LINK_XFER → marching ants
    //   host connected + MSC active, idle → LINK_IDLE → solid line + keep-alive ping
    s.usb      = !ds.usbHostConnected ? LINK_DOWN
               : (ds.usbWriteKBps > 0.5f ? LINK_XFER : LINK_IDLE);
    s.usbRetry = false;                       // USB is physical: no retry animation

    // Net link: idle when connected, ants while uploading, down otherwise.
    bool netUp = ds.pppConnected || ds.netConnected;
    s.net      = netUp ? (ds.uploadKBps > 0.5f ? LINK_XFER : LINK_IDLE) : LINK_DOWN;
    s.netRetry = true;                        // internet is expected to retry forever

    synopticFmtMb(uv, 8, ds.hostWrittenMb);
    synopticFmtMb(sv, 8, ds.mbQueued);
    synopticFmtMb(cv, 8, ds.mbUploaded + ds.uploadingMb);
    s.usbVal = uv; s.sdVal = sv; s.cloudVal = cv;

    // Marching-ant speed tracks live throughput: fast link → fast ants.
    s.usbAntStepMs = synopticAntStepMs(ds.usbWriteKBps);
    s.netAntStepMs = synopticAntStepMs(ds.uploadKBps);

    // ETE (shown only while net==LINK_XFER): remaining queued / current rate.
    float remaining = (ds.mbQueued > ds.uploadingMb) ? ds.mbQueued - ds.uploadingMb : 0;
    if (s.net == LINK_XFER && ds.uploadKBps > 0.5f && remaining > 0.001f) {
        int etaSec = (int)(remaining * 1024.0f / ds.uploadKBps);
        s.eteSecs = etaSec > 5999 ? 5999 : (uint16_t)etaSec;   // cap at 99:59
    } else {
        s.eteSecs = 0;
    }
}

// Render the main operational display
inline void updateDisplay(DisplayState& ds) {
    g_hal->display->clear();

    // The top connection bar is drawn only for the OTA / SD-fault overlays (the
    // synoptic normal view owns the whole screen and encodes link state itself).
    if (ds.otaActive || ds.sdError)
    // Row 0: Connection status + signal bars
    {
        char label[18];
        int bars = 0;
        if (ds.pppConnected) {
            // Bars = connection QUALITY from link throughput (not RSSI, which
            // can't be read mid-PPP without disrupting the data stream).
            if (ds.modemOp[0]) strlcpy(label, ds.modemOp, sizeof(label));
            else               strlcpy(label, "Cellular", sizeof(label));
            bars = linkQualityBarsLatency(ds.linkLatencyMs, ds.linkAgeMs);
        } else if (ds.netConnected) {
            strlcpy(label, ds.wifiLabel, sizeof(label));
            bars = ds.wifiBars;
        } else if (ds.modemReady) {
            strlcpy(label, "Connecting...", sizeof(label));
        } else {
            strlcpy(label, "No Connection", sizeof(label));
        }
        g_hal->display->text(0, 0, label);

        const int8_t xs[4] = {108,113,118,123}, hs[4] = {2,4,6,8};
        for (int i = 0; i < bars; i++) {
            g_hal->display->rect(xs[i], 8-hs[i], 3, hs[i], true);
        }
        g_hal->display->hline(0, 127, 9);
    }

    if (ds.otaActive) {
        // ── OTA overlay (below connection bar) ─────────────────────────
        if (ds.otaPct >= 0) {
            // Downloading — show version on one line, progress below
            g_hal->display->text(20, 14, "Updating...");
            char verLine[20];
            snprintf(verLine, sizeof(verLine), "v%s", ds.otaVersion);
            int vw = strlen(verLine) * 6;
            g_hal->display->text((128 - vw) / 2, 26, verLine);

            g_hal->display->rect(4, 38, 120, 8, false);
            int fill = ds.otaPct * 116 / 100;
            if (fill > 0) g_hal->display->rect(6, 40, fill, 4, true);

            char pctStr[8];
            snprintf(pctStr, sizeof(pctStr), "%d%%", ds.otaPct);
            int pw = strlen(pctStr) * 6;
            g_hal->display->text((128 - pw) / 2, 49, pctStr);
        } else {
            // Waiting for connection or checking — centered with cycling dots
            static int dotFrame = 0;
            const char* dots[] = { "   ", ".  ", ".. ", "..." };
            char line2[16];
            snprintf(line2, sizeof(line2), "update%s", dots[dotFrame % 4]);
            dotFrame++;

            int w1 = 12 * 6; // "Checking for"
            int w2 = strlen(line2) * 6;
            g_hal->display->text((128 - w1) / 2, 28, "Checking for");
            g_hal->display->text((128 - w2) / 2, 40, line2);
        }

        // Row 55 left empty (was "Do not unplug")
    } else if (ds.sdError) {
        // ── SD-fault overlay (card unusable / being recovered) ──────────
        // Surfaces a card failure to anyone looking at the unit, even with no
        // serial. The connection bar above still shows cellular state, since in
        // this mode the firmware is logging the fault over the link.
        {
            const char* big = "SD ERROR";
            int w = g_hal->display->text_width(big, 2);
            g_hal->display->text((128 - w) / 2, 22, big, 2);
        }
        {
            const char* sub = ds.sdReformatting ? "reformatting..." : "recovering...";
            int w = strlen(sub) * 6;
            g_hal->display->text((128 - w) / 2, 44, sub);
        }
    } else {
        // ── Normal operational display: USB → SD → Cloud synoptic ────────
        AirbridgeState s;
        char uv[8], sv[8], cv[8];
        buildSynopticState(ds, s, uv, sv, cv);
        synopticRender(s, g_hal->clock->millis());
    }

    g_hal->display->flush();
}

// Boot splash screen — shows version, device ID, pending uploads
inline void dispBootSplash(const char* fwVersion, const char* deviceId,
                            const char* usbMode = "CDC+MSC") {
    g_hal->display->clear();
    g_hal->display->text(10, 1, "AirBridge", 2);

    // Device ID
    if (deviceId && deviceId[0]) {
        char idLine[22];
        snprintf(idLine, sizeof(idLine), "ID:%s", deviceId);
        int idW = strlen(idLine) * 6;
        g_hal->display->text((128 - idW) / 2, 28, idLine);
    }

    // USB mode + version
    {
        // Abbreviate YYYYMMDDHHMMSS to MMDD.HHMM for display
        char verShort[16];
        if (strlen(fwVersion) >= 14 && fwVersion[0] >= '2') {
            snprintf(verShort, sizeof(verShort), "%.4s.%.4s", fwVersion + 4, fwVersion + 8);
        } else {
            strlcpy(verShort, fwVersion, sizeof(verShort));
        }
        char modeLine[28];
        snprintf(modeLine, sizeof(modeLine), "%s v%s", usbMode, verShort);
        int mW = strlen(modeLine) * 6;
        g_hal->display->text((128 - mW) / 2, 46, modeLine);
    }

    g_hal->display->flush();
}

// Simple two-line splash/status display — all lines horizontally centered.
inline void dispSplash(const char* line1, const char* line2 = nullptr) {
    IDisplay* d = g_hal->display;
    d->clear();
    d->text((SCREEN_W - d->text_width("AirBridge", 2)) / 2, 6, "AirBridge", 2);
    d->hline(0, 127, 26);
    if (line1) d->text((SCREEN_W - d->text_width(line1, 1)) / 2, 32, line1);
    if (line2) d->text((SCREEN_W - d->text_width(line2, 1)) / 2, 48, line2);
    d->flush();
}

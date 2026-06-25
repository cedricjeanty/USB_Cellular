#pragma once
// AirBridge display rendering — extracted from main.cpp for testing
// All functions use g_hal->display for rendering.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include <cstdio>

// State snapshot passed to updateDisplay (replaces direct global reads)
struct DisplayState {
    bool     pppConnected;
    bool     netConnected;
    bool     modemReady;
    int      modemRssi;
    float    linkKBps;      // windowed-peak throughput (KB/s) — connection-quality signal
    uint32_t linkAgeMs;     // ms since the last successful transfer (liveness)
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
};

// Effective link-throughput ceiling (KB/s). The 3 Mbaud + HW-flow-control UART to
// the modem caps practical TLS upload throughput at ~150 KB/s, so the quality bars
// are scaled to that fixed maximum (not an auto-peak).
#define LINK_MAX_KBPS 150.0f

// Peak-hold over a sliding window: the displayed throughput is the MAX seen in the
// last windowMs. Bandwidth jitters every transfer; holding the recent peak makes
// the quality bars read like a steady signal meter (snap up to peaks, decay slowly
// only when nothing better arrives) even though the underlying metric is bandwidth.
// Pure (nowMs passed in) so it's unit-testable. Iterating 0..count-1 covers every
// stored slot (the ring fills 0..CAP-1 before wrapping).
struct LinkWindow {
    static const int CAP = 16;
    uint32_t ms[CAP];
    float    kBps[CAP];
    uint8_t  count;
    uint8_t  head;
};
inline void linkWindowAdd(LinkWindow& w, uint32_t nowMs, float kBps) {
    w.ms[w.head] = nowMs;
    w.kBps[w.head] = kBps;
    w.head = (uint8_t)((w.head + 1) % LinkWindow::CAP);
    if (w.count < LinkWindow::CAP) w.count++;
}
inline float linkWindowMax(const LinkWindow& w, uint32_t nowMs, uint32_t windowMs) {
    float mx = 0;
    for (int i = 0; i < w.count; i++)
        if ((uint32_t)(nowMs - w.ms[i]) <= windowMs && w.kBps[i] > mx) mx = w.kBps[i];
    return mx;
}

// Connection-quality bars (0-4) from windowed-peak throughput as a PERCENT of the
// fixed LINK_MAX_KBPS ceiling, at quartiles ~25/50/75/100%. Quality comes from the
// data path (no RSSI/+++ which would disrupt PPP): bandwidth from real file/OTA
// transfers, with the 60s log upload keeping `ageMs` fresh as a liveness heartbeat.
// Stale (no success in >2 log cycles) => 0 bars even if PPP claims up; alive but no
// recent bandwidth sample => 1 bar.
inline int linkQualityBars(float kBps, float maxKBps, uint32_t ageMs) {
    if (ageMs > 150000) return 0;                  // no recent successful transfer
    if (maxKBps < 1.0f) return kBps > 0 ? 1 : 0;
    float pct = kBps / maxKBps;
    if (pct >= 0.95f) return 4;                    // ~100%
    if (pct >= 0.70f) return 3;                    // ~75%
    if (pct >= 0.45f) return 2;                    // ~50%
    if (pct >= 0.20f) return 1;                    // ~25%
    return 1;                                      // alive but below 25% of ceiling
}

// Render the main operational display
inline void updateDisplay(DisplayState& ds) {
    g_hal->display->clear();

    // Row 0: Connection status + signal bars
    {
        char label[18];
        int bars = 0;
        if (ds.pppConnected) {
            // Bars = connection QUALITY from link throughput (not RSSI, which
            // can't be read mid-PPP without disrupting the data stream).
            if (ds.modemOp[0]) strlcpy(label, ds.modemOp, sizeof(label));
            else               strlcpy(label, "Cellular", sizeof(label));
            bars = linkQualityBars(ds.linkKBps, LINK_MAX_KBPS, ds.linkAgeMs);
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
    }
    g_hal->display->hline(0, 127, 9);

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
    } else {
        // ── Normal operational display ──────────────────────────────────
        float uploaded  = ds.mbUploaded + ds.uploadingMb;
        float remaining = (ds.mbQueued > ds.uploadingMb) ? ds.mbQueued - ds.uploadingMb : 0;
        float usbSessionMb = ds.hostWrittenMb;

        // Row 11: labels
        g_hal->display->text(13, 11, "USB IN");
        g_hal->display->text(78, 11, "UPLOAD");
        for (int y = 11; y < 48; y += 2) g_hal->display->rect(63, y, 1, 1, true);

        // Row 20: speeds
        {
            char usbSpd[12], upSpd[12];
            if (ds.usbWriteKBps > 0.5f)
                snprintf(usbSpd, sizeof(usbSpd), "%dKB/s", (int)ds.usbWriteKBps);
            else
                strlcpy(usbSpd, "0KB/s", sizeof(usbSpd));
            if (ds.uploadKBps > 0.5f)
                snprintf(upSpd, sizeof(upSpd), "%dKB/s", (int)ds.uploadKBps);
            else
                strlcpy(upSpd, "0KB/s", sizeof(upSpd));
            int usbW = strlen(usbSpd) * 6;
            int upW  = strlen(upSpd) * 6;
            g_hal->display->text((62 - usbW) / 2, 20, usbSpd);
            g_hal->display->text(65 + (62 - upW) / 2, 20, upSpd);
        }

        // Row 30: totals (size 2)
        {
            char usbTot[12], upTot[12];
            _fmtSizeShort(usbTot, sizeof(usbTot), usbSessionMb);
            _fmtSizeShort(upTot, sizeof(upTot), uploaded);
            int usbW = g_hal->display->text_width(usbTot, 2);
            int upW  = g_hal->display->text_width(upTot, 2);
            g_hal->display->text((62 - usbW) / 2, 30, usbTot, 2);
            g_hal->display->text(65 + (62 - upW) / 2, 30, upTot, 2);
        }

        // Row 50: progress bar
        {
            float totalMb = uploaded + remaining;
            g_hal->display->rect(0, 50, 128, 5, false);
            if (totalMb > 0.001f) {
                int fill = (int)(uploaded / totalMb * 126);
                if (fill > 126) fill = 126;
                if (fill > 0) g_hal->display->rect(1, 51, fill, 3, true);
            }
        }

        // Row 57: remaining + ETA
        {
            char remStr[14], etaStr[14];
            snprintf(remStr, sizeof(remStr), "REM:"); _fmtSize(remStr + 4, sizeof(remStr) - 4, remaining);
            g_hal->display->text(0, 57, remStr);
            if (ds.uploadKBps > 0.5f && remaining > 0.001f) {
                int etaSec = (int)(remaining * 1024.0f / ds.uploadKBps);
                int mm = etaSec / 60, ss = etaSec % 60;
                if (mm > 99) snprintf(etaStr, sizeof(etaStr), "ETA %dh%02d", mm / 60, mm % 60);
                else         snprintf(etaStr, sizeof(etaStr), "ETA %d:%02d", mm, ss);
            } else {
                strlcpy(etaStr, "ETA --:--", sizeof(etaStr));
            }
            int etaW = strlen(etaStr) * 6;
            g_hal->display->text(128 - etaW, 57, etaStr);
        }
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

// OTA update display — shows version being downloaded + progress
inline void dispOtaProgress(const char* newVersion, int pct) {
    g_hal->display->clear();
    g_hal->display->text(10, 1, "AirBridge", 2);
    g_hal->display->hline(0, 127, 20);

    char updLine[24];
    snprintf(updLine, sizeof(updLine), "Update: v%s", newVersion);
    int uw = g_hal->display->text_width(updLine);
    g_hal->display->text((128 - uw) / 2, 24, updLine);

    if (pct >= 0) {
        // Progress bar
        g_hal->display->rect(4, 38, 120, 8, false);
        int fill = pct * 116 / 100;
        if (fill > 0) g_hal->display->rect(6, 40, fill, 4, true);

        char pctStr[8];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        int pw = strlen(pctStr) * 6;
        g_hal->display->text((128 - pw) / 2, 50, pctStr);
    } else {
        static int dotFrame2 = 0;
        const char* dots[] = { "   ", ".  ", ".. ", "..." };
        char chkLine[20];
        snprintf(chkLine, sizeof(chkLine), "Checking%s", dots[dotFrame2 % 4]);
        dotFrame2++;
        int cw = strlen(chkLine) * 6;
        g_hal->display->text((128 - cw) / 2, 38, chkLine);
    }

    g_hal->display->flush();
}

// Simple two-line splash/status display
inline void dispSplash(const char* line1, const char* line2 = nullptr) {
    g_hal->display->clear();
    g_hal->display->text(14, 6, "AirBridge", 2);
    g_hal->display->hline(0, 127, 26);
    g_hal->display->text(0, 32, line1);
    if (line2) g_hal->display->text(0, 48, line2);
    g_hal->display->flush();
}

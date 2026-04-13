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
    char     modemOp[32];
    char     wifiLabel[22];
    int8_t   wifiBars;
    float    hostWrittenMb;
    float    mbUploaded;
    float    mbQueued;
    float    uploadingMb;
    float    usbWriteKBps;
    float    uploadKBps;
};

// Render the main operational display
inline void updateDisplay(DisplayState& ds) {
    g_hal->display->clear();

    // Row 0: Connection status + signal bars
    {
        char label[18];
        int bars = 0;
        if (ds.pppConnected && ds.modemRssi > 0) {
            if (ds.modemOp[0]) strlcpy(label, ds.modemOp, sizeof(label));
            else               strlcpy(label, "Cellular", sizeof(label));
            if      (ds.modemRssi >= 20) bars = 4;
            else if (ds.modemRssi >= 15) bars = 3;
            else if (ds.modemRssi >= 10) bars = 2;
            else                         bars = 1;
        } else if (ds.netConnected) {
            strlcpy(label, ds.wifiLabel, sizeof(label));
            bars = ds.wifiBars;
        } else if (ds.pppConnected) {
            strlcpy(label, "No Signal", sizeof(label));
        } else if (ds.modemReady) {
            if (ds.modemOp[0])
                snprintf(label, sizeof(label), "%s...", ds.modemOp);
            else
                strlcpy(label, "Connecting...", sizeof(label));
        } else {
            strlcpy(label, "No Network", sizeof(label));
        }
        g_hal->display->text(0, 0, label);

        const int8_t xs[4] = {108,113,118,123}, hs[4] = {2,4,6,8};
        for (int i = 0; i < bars; i++) {
            g_hal->display->rect(xs[i], 8-hs[i], 3, hs[i], true);
        }
    }
    g_hal->display->hline(0, 127, 9);

    // Split Gauges: USB IN | UPLOAD
    float uploaded  = ds.mbUploaded + ds.uploadingMb;
    float remaining = (ds.mbQueued > ds.uploadingMb) ? ds.mbQueued - ds.uploadingMb : 0;
    float usbSessionMb = ds.hostWrittenMb;

    // Row 11: labels
    g_hal->display->text(13, 11, "USB IN");
    g_hal->display->text(78, 11, "UPLOAD");
    for (int y = 11; y < 48; y += 2) g_hal->display->rect(63, y, 1, 1, true);

    // Row 20: speeds (size 1 — can be long strings like "1024KB/s")
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

    // Row 30: totals (size 2, short format: "4.2M", "99K", "1.2G")
    {
        char usbTot[12], upTot[12];
        _fmtSizeShort(usbTot, sizeof(usbTot), usbSessionMb);
        _fmtSizeShort(upTot, sizeof(upTot), uploaded);
        int usbW = g_hal->display->text_width(usbTot, 2);
        int upW  = g_hal->display->text_width(upTot, 2);
        g_hal->display->text((62 - usbW) / 2, 30, usbTot, 2);
        g_hal->display->text(65 + (62 - upW) / 2, 30, upTot, 2);
    }

    // Row 50: progress bar (thin)
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

        // ETA uses the display upload speed directly (already smoothed by 10s window)
        // Dashes out only when speed is truly zero (no data in entire 10s window)
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

    g_hal->display->flush();
}

// Boot splash screen — shows version, device ID, pending uploads
inline void dispBootSplash(const char* fwVersion, const char* deviceId,
                            const char* usbMode = "CDC+MSC") {
    g_hal->display->clear();
    g_hal->display->text(10, 0, "AirBridge", 2);

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
    g_hal->display->text(10, 0, "AirBridge", 2);
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
        g_hal->display->text(22, 38, "Checking...");
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

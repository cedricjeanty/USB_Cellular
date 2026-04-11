#pragma once
// AirBridge modem initialization — AT command sequence for SIM7600
// Uses mdm_write/mdm_read/mdm_flush (routed through HAL UART).
// Extracted from modemTask() for use in both firmware and emulator.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

// Forward declarations — these are defined in main.cpp for ESP32,
// or must be provided by the emulator.
extern int modem_at_cmd(const char* cmd, char* resp, int resp_size, int timeout_ms);
extern int mdm_write(const void* data, size_t len);
extern int mdm_read(void* buf, size_t len, uint32_t timeout_ms);
extern void mdm_flush();
extern void mdm_set_baudrate(uint32_t baud);

// Result of modem AT initialization
struct ModemInitResult {
    bool synced;           // AT sync succeeded
    bool registered;       // Network registration succeeded
    bool connected;        // PPP CONNECT received
    int  rssi;             // Signal quality (0-31, 99=unknown)
    char operatorName[32]; // Network operator
    uint32_t epoch;        // UTC epoch from AT+CCLK (0 if unavailable)
};

// Attempt AT sync at 115200. Returns true if modem responds to AT.
inline bool modemAtSync() {
    char resp[512];
    mdm_flush();

    // +++ escape (modem may be in PPP data mode from previous boot)
    g_hal->clock->delay_ms(1100);
    mdm_write("+++", 3);
    g_hal->clock->delay_ms(1100);
    mdm_flush();

    // Try AT at 115200, up to 20s
    for (int i = 0; i < 40; i++) {
        int len = modem_at_cmd("AT", resp, sizeof(resp), 500);
        if (len > 0 && strstr(resp, "OK")) return true;
    }
    return false;
}

// Run the full AT initialization sequence (post-sync).
// Assumes modem is already responding to AT at 115200.
inline ModemInitResult modemRunInit() {
    ModemInitResult r = {};
    r.synced = true;
    char resp[512];

    // Reset radio to clear stale PPP/PDP state
    modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 3000);
    g_hal->clock->delay_ms(500);
    modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 3000);
    g_hal->clock->delay_ms(1000);
    modem_at_cmd("AT", resp, sizeof(resp), 2000);

    // Disable echo
    modem_at_cmd("ATE0", resp, sizeof(resp), 1000);
    modem_at_cmd("AT+CTZU=1", resp, sizeof(resp), 500);

    // Time sync from AT+CCLK?
    if (modem_at_cmd("AT+CCLK?", resp, sizeof(resp), 2000) > 0) {
        char* q = strchr(resp, '"');
        if (q) {
            int yy = 0, mo = 0, dd = 0, hh = 0, mi = 0, ss = 0;
            char tzSign = '+';
            int tzVal = 0;
            sscanf(q + 1, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss);
            char* q2 = strchr(q + 1, '"');
            if (q2) sscanf(q2 + 1, "%c%d", &tzSign, &tzVal);
            if (yy >= 24 && yy <= 50 && mo >= 1 && mo <= 12) {
                struct tm tm = {};
                tm.tm_year = yy + 100;
                tm.tm_mon  = mo - 1;
                tm.tm_mday = dd;
                tm.tm_hour = hh;
                tm.tm_min  = mi;
                tm.tm_sec  = ss;
                time_t epoch = mktime(&tm);
                int tzOffsetSec = tzVal * 15 * 60;
                if (tzSign == '-') tzOffsetSec = -tzOffsetSec;
                epoch -= tzOffsetSec;
                r.epoch = (uint32_t)epoch;
            }
        }
    }

    // Enable registration URCs + auto RSSI
    modem_at_cmd("AT+CREG=1", resp, sizeof(resp), 1000);
    modem_at_cmd("AT+AUTOCSQ=1,1", resp, sizeof(resp), 1000);

    // Wait for registration (up to 30s)
    for (int i = 0; i < 15; i++) {
        modem_at_cmd("AT+CREG?", resp, sizeof(resp), 1000);
        if (strstr(resp, ",1") || strstr(resp, ",5")) {
            r.registered = true;
            break;
        }
        g_hal->clock->delay_ms(1000);
    }

    // Read RSSI
    modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000);
    {
        char* p = strstr(resp, "+CSQ:");
        if (p) sscanf(p, "+CSQ: %d", &r.rssi);
        else r.rssi = 99;
    }

    // Read operator
    if (modem_at_cmd("AT+COPS?", resp, sizeof(resp), 2000) > 0) {
        char* q1 = strchr(resp, '"');
        if (q1) {
            char* q2 = strchr(q1 + 1, '"');
            if (q2) {
                int olen = q2 - q1 - 1;
                if (olen > 31) olen = 31;
                memcpy(r.operatorName, q1 + 1, olen);
                r.operatorName[olen] = '\0';
            }
        }
    }

    // Set APN
    modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 5000);

    // Dial PPP (3 attempts)
    for (int attempt = 0; attempt < 3 && !r.connected; attempt++) {
        if (attempt > 0) {
            modem_at_cmd("ATH", resp, sizeof(resp), 2000);
            g_hal->clock->delay_ms(10000);
        }
        mdm_write("ATD*99#\r", 8);

        // Wait for CONNECT (up to 30s)
        uint32_t t0 = g_hal->clock->millis();
        char connbuf[256] = "";
        int connlen = 0;
        while (g_hal->clock->millis() - t0 < 30000) {
            int len = mdm_read((uint8_t*)connbuf + connlen,
                               sizeof(connbuf) - 1 - connlen, 500);
            if (len > 0) {
                connlen += len;
                connbuf[connlen] = '\0';
                if (strstr(connbuf, "CONNECT")) { r.connected = true; break; }
                if (strstr(connbuf, "ERROR") || strstr(connbuf, "NO CARRIER")) break;
            }
        }
    }

    return r;
}

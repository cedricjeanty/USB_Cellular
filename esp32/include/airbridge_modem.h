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

// Result of modem reconnect
struct ModemReconnectResult {
    bool connected;
    int  rssi;
    char operatorName[32];
    bool registered;
};

// Reconnect PPP after a connection drop.
//
// resetRadio=false (preferred): PDP-context deactivate + re-register + redial.
//   Does NOT use CFUN=0/1. After a carrier-terminated session the radio stays
//   registered; CFUN=0/1 would cause a real deregistration and force a 30-60s
//   re-attach cycle that makes things worse. AT+CGACT=0,1 cleanly deactivates
//   the stale PDP context so the next ATD*99 gets a fresh bearer.
//
// resetRadio=true (last resort): +++ escape → ATH → CFUN=0/1 → re-register.
//   Only use when the modem is completely unresponsive (not after a normal
//   carrier-terminated PPP drop).
//
// Registration check uses AT+CEREG (LTE packet-domain), NOT AT+CREG.
// T-Mobile denies voice registration (CREG returns state 3) but grants LTE
// data registration (CEREG returns state 5 = roaming). Checking CREG will
// always time out on T-Mobile/Hologram LTE-only SIMs.
inline ModemReconnectResult modemReconnect(bool resetRadio = true) {
    ModemReconnectResult r = {};
    char resp[256];

    if (resetRadio) {
        // Genuine modem reset: escape PPP mode, hang up, power-cycle radio.
        // Only needed when the modem stopped responding to AT commands.
        g_hal->clock->delay_ms(1100);
        mdm_write("+++", 3);
        g_hal->clock->delay_ms(1100);
        mdm_flush();
        modem_at_cmd("ATH", resp, sizeof(resp), 2000);
        modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 5000);
        g_hal->clock->delay_ms(10000);
        modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 5000);
        g_hal->clock->delay_ms(5000);
    } else {
        // Normal reconnect after carrier-terminated session:
        // 1. Hang up cleanly (exits PPP mode if needed)
        modem_at_cmd("ATH", resp, sizeof(resp), 2000);
        // 2. Explicitly deactivate the PDP context — the modem may still think
        //    context 1 is active after the carrier terminated the bearer, causing
        //    ATD*99 to return CONNECT on a dead context (IPCP never completes).
        modem_at_cmd("AT+CGACT=0,1", resp, sizeof(resp), 5000);
        // Allow the network to fully release the previous session
        g_hal->clock->delay_ms(5000);
    }

    // Wait for LTE data registration (up to 60s).
    // Use AT+CEREG (LTE/EPS), not AT+CREG (2G/3G voice).
    // T-Mobile/Hologram denies CREG (voice) but grants CEREG (data roaming).
    for (int w = 0; w < 30; w++) {
        modem_at_cmd("AT+CEREG?", resp, sizeof(resp), 2000);
        if (strstr(resp, ",1") || strstr(resp, ",5")) {
            r.registered = true;
            break;
        }
        // Also accept CGREG (2G/3G packet domain) as fallback
        modem_at_cmd("AT+CGREG?", resp, sizeof(resp), 2000);
        if (strstr(resp, ",1") || strstr(resp, ",5")) {
            r.registered = true;
            break;
        }
        g_hal->clock->delay_ms(2000);
    }
    if (!r.registered) return r;

    // Read RSSI — CSQ=99 may appear briefly after mode changes; retry once.
    modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000);
    {
        char* p = strstr(resp, "+CSQ:");
        if (p) {
            int rssi = 99;
            sscanf(p, "+CSQ: %d", &rssi);
            if (rssi == 99) {
                g_hal->clock->delay_ms(3000);
                modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000);
                p = strstr(resp, "+CSQ:");
                if (p) sscanf(p, "+CSQ: %d", &rssi);
            }
            if (rssi != 99) r.rssi = rssi;
        }
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

    // Always re-state APN before redialing (required after any mode change)
    modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 5000);

    // Redial PPP (3 attempts)
    for (int attempt = 0; attempt < 3 && !r.connected; attempt++) {
        if (attempt > 0) {
            modem_at_cmd("ATH", resp, sizeof(resp), 2000);
            modem_at_cmd("AT+CGACT=0,1", resp, sizeof(resp), 3000);
            g_hal->clock->delay_ms(5000);
            modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 5000);
        }
        mdm_write("ATD*99#\r", 8);

        uint32_t t0 = g_hal->clock->millis();
        char connbuf[256] = "";
        int connlen = 0;
        while (g_hal->clock->millis() - t0 < 15000) {
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

#pragma once
// AirBridge modem initialization — AT command sequence for SIM7600
// Uses mdm_write/mdm_read/mdm_flush (routed through HAL UART).
// Extracted from modemTask() for use in both firmware and emulator.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>

// Sentinel for "metric not available" in ModemInitResult signal fields (RSRP/
// RSRQ/SINR are negative-ish dBm/dB, so a large positive value is unambiguous).
static const int MODEM_SIG_NA = 9999;

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

// Reconnect escalation plan derived from the consecutive-failure count — the
// pure decision logic the modem task's pump loop runs (the AT sequence itself
// is modemReconnect()). Extracted so it's shared + unit-tested.
//
// Attempts 1-4 (failures 0-3): soft reconnect (no CFUN) — handles the common
// carrier-terminated drop where the radio stays registered.
// Attempt 5 (failures=4) and every 5th after: full CFUN=0/1 radio reset.
// After the 2nd reset (failures>=9): a progressive backoff (30s * reset#, capped
// at 300s) precedes each reset.
// Documented in CLAUDE.md "Reconnect backoff".
struct ModemReconnectPlan {
    bool     radioReset;       // do a CFUN=0/1 reset this attempt
    uint32_t backoffSeconds;   // wait before the attempt (0 = none)
};
inline ModemReconnectPlan modemReconnectPlan(int failures) {
    ModemReconnectPlan p = {};
    p.radioReset = (failures >= 4 && (failures - 4) % 5 == 0);
    // Only a short settle before a radio reset (CFUN=0/1 itself takes ~15s) — NO long
    // backoff. The reconnect loop is signal-gated (modemWaitForSignal polls coverage
    // every ~10s and retries the instant the network returns), so we never burn minutes
    // idle: in good coverage the device reconnects within seconds of a drop.
    p.backoffSeconds = p.radioReset ? 10u : 0u;
    return p;
}

// Poll LTE registration (AT+CEREG, with AT+CGREG fallback) every pollMs until the
// network is available, up to maxWaitMs — then read RSSI (AT+CSQ). Safe to call while
// disconnected (no PPP, so AT commands don't collide). This replaces fixed dead-waits
// in the reconnect loop: a redial fires the MOMENT coverage returns, not after a timer.
// Returns as soon as registered (registered=true) or when maxWaitMs elapses.
struct ModemSignalWait { bool registered; int rssi; };  // rssi: CSQ 0-31, 99/NA unknown
inline ModemSignalWait modemWaitForSignal(uint32_t maxWaitMs, uint32_t pollMs) {
    ModemSignalWait r = { false, 99 };
    char resp[256];
    uint32_t waited = 0;
    for (;;) {
        if (modem_at_cmd("AT+CEREG?", resp, sizeof(resp), 2000) > 0 &&
            (strstr(resp, ",1") || strstr(resp, ",5"))) r.registered = true;
        else if (modem_at_cmd("AT+CGREG?", resp, sizeof(resp), 2000) > 0 &&
                 (strstr(resp, ",1") || strstr(resp, ",5"))) r.registered = true;
        if (r.registered) {
            if (modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000) > 0) {
                char* p = strstr(resp, "+CSQ:");
                int rssi = 99;
                if (p) sscanf(p, "+CSQ: %d", &rssi);
                r.rssi = rssi;
            }
            return r;  // coverage is back — retry immediately
        }
        if (waited >= maxWaitMs) return r;
        if (g_hal && g_hal->clock) g_hal->clock->delay_ms(pollMs);
        waited += pollMs;
    }
}

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
        // CRITICAL: AT+CFUN=0 resets the SIM7600 UART baud rate to 115200 (default).
        // Downgrade both modem and host to 115200 BEFORE CFUN=0, so both sides are in
        // sync after the reset. Without this, subsequent AT commands at the upgraded
        // baud (3Mbaud) all fail silently — CEREG polls time out for 3 minutes each.
        modem_at_cmd("AT+IPR=115200", resp, sizeof(resp), 1000);
        mdm_set_baudrate(115200);
        modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 5000);
        g_hal->clock->delay_ms(10000);
        modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 5000);
        g_hal->clock->delay_ms(5000);
        // Both modem and host are now at 115200. The baud upgrade will happen
        // on the next boot (modemAtSync + baud upgrade sequence in modemTask).
    } else {
        // Soft reconnect — no radio reset, but always escape PPP data mode first.
        // After a reconnect attempt that got CONNECT but failed IPCP, the modem
        // stays in PPP data mode. Without +++ guard, ATH is sent as PPP frame
        // bytes and the modem ignores it; all subsequent AT commands (CEREG, etc.)
        // time out for the full 3-minute window. This is the primary cause of
        // repeated "registration timeout" failures in the soft reconnect path.
        g_hal->clock->delay_ms(1100);
        mdm_write("+++", 3);
        g_hal->clock->delay_ms(1100);
        mdm_flush();
        // 1. Hang up cleanly (safe in both command and data mode after +++)
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
    int  rsrp;             // LTE RSRP in dBm (MODEM_SIG_NA if unavailable)
    int  rsrq;             // LTE RSRQ in dB  (MODEM_SIG_NA if unavailable)
    int  sinr;             // LTE SINR in dB  (MODEM_SIG_NA if unavailable)
    char band[16];         // LTE band token from CPSI (e.g. "EUTRAN-BAND4")
    char operatorName[32]; // Network operator
    uint32_t epoch;        // UTC epoch from AT+CCLK (0 if unavailable)
};

// Parse an AT+CESQ response: "+CESQ: rxlev,ber,rscp,ecno,rsrq,rsrp".
// For LTE only rsrq/rsrp are valid (others 255). Converts the 3GPP indices to
// dBm/dB: RSRP = idx-141 (idx 0..97), RSRQ = idx/2-20 (idx 0..34); 255 => N/A.
inline void parseCesq(const char* resp, int* rsrpOut, int* rsrqOut) {
    int rsrp = MODEM_SIG_NA, rsrq = MODEM_SIG_NA;
    const char* p = resp ? strstr(resp, "+CESQ:") : nullptr;
    if (p) {
        int rxlev, ber, rscp, ecno, rq, rp;
        if (sscanf(p, "+CESQ: %d,%d,%d,%d,%d,%d",
                   &rxlev, &ber, &rscp, &ecno, &rq, &rp) == 6) {
            if (rp >= 0 && rp <= 97) rsrp = rp - 141;
            if (rq >= 0 && rq <= 34) rsrq = (rq / 2) - 20;
        }
    }
    if (rsrpOut) *rsrpOut = rsrp;
    if (rsrqOut) *rsrqOut = rsrq;
}

// Parse an AT+CPSI? LTE response (best-effort — field order varies by SIM7600
// firmware). Pulls the band token (the comma-field containing "BAND") and SINR
// (the last integer field on the line). Leaves band empty / SINR=N/A if absent.
inline void parseCpsi(const char* resp, char* band, size_t bandSz, int* sinrOut) {
    if (band && bandSz) band[0] = '\0';
    int sinr = MODEM_SIG_NA;
    const char* p = resp ? strstr(resp, "+CPSI:") : nullptr;
    if (!p) { if (sinrOut) *sinrOut = sinr; return; }

    char line[256];
    strlcpy(line, p, sizeof(line));
    char* nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';

    char* fields[24]; int nf = 0;
    char* save = nullptr;
    for (char* t = strtok_r(line, ",", &save); t && nf < 24;
         t = strtok_r(nullptr, ",", &save)) {
        while (*t == ' ') t++;       // trim leading spaces
        fields[nf++] = t;
    }
    if (band && bandSz) {
        for (int i = 0; i < nf; i++)
            if (strstr(fields[i], "BAND")) { strlcpy(band, fields[i], bandSz); break; }
    }
    for (int i = nf - 1; i >= 0; i--) {
        char* end = nullptr;
        long v = strtol(fields[i], &end, 10);
        if (end != fields[i] && *end == '\0') { sinr = (int)v; break; }
    }
    if (sinrOut) *sinrOut = sinr;
}

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

// Modem init, part 1 (post-sync): radio reset + echo-off + time sync.
// Assumes the modem already responds to AT. Leaves it at the current baud so the
// firmware can upgrade baud (3 Mbaud + HW flow control) before part 2. Returns a
// result with .synced and .epoch set; .registered/.rssi/.connected are filled by
// modemRunInitPost(). The single-baud emulator/tests call modemRunInit() (= Pre+Post).
inline ModemInitResult modemRunInitPre() {
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

    return r;
}

// Register on the LTE network and read all signal metrics — NO PPP dial. Shared
// by modemRunInitPost() (which then dials PPP) and the antenna signal-survey
// (which loops sampling instead). Fills r.registered/rssi/rsrp/rsrq/sinr/band/
// operatorName. All AT-command mode, so no +++ escape / data-session disruption.
inline void modemRegisterAndReadSignal(ModemInitResult& r) {
    char resp[512];

    // Enable LTE registration URCs + auto RSSI.
    // Use AT+CEREG (LTE/EPS), NOT AT+CREG (2G/3G voice): T-Mobile/Hologram
    // denies CREG (voice, state 3) but grants CEREG (data roaming, state 5) —
    // CREG always times out on these SIMs. Mirrors the firmware boot path.
    modem_at_cmd("AT+CEREG=1", resp, sizeof(resp), 1000);
    modem_at_cmd("AT+AUTOCSQ=1,1", resp, sizeof(resp), 1000);

    // Wait for LTE data registration, with CGREG packet-domain fallback.
    for (int i = 0; i < 30; i++) {
        modem_at_cmd("AT+CEREG?", resp, sizeof(resp), 1000);
        if (strstr(resp, ",1") || strstr(resp, ",5")) {
            r.registered = true;
            break;
        }
        // Accept CGREG (2G/3G packet domain) as fallback
        modem_at_cmd("AT+CGREG?", resp, sizeof(resp), 1000);
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

    // Read richer LTE signal quality — RSRP/RSRQ (CESQ) + band/SINR (CPSI).
    // Best-effort: fields stay MODEM_SIG_NA / empty if unsupported.
    r.rsrp = r.rsrq = r.sinr = MODEM_SIG_NA;
    r.band[0] = '\0';
    if (modem_at_cmd("AT+CESQ", resp, sizeof(resp), 2000) > 0)
        parseCesq(resp, &r.rsrp, &r.rsrq);
    if (modem_at_cmd("AT+CPSI?", resp, sizeof(resp), 2000) > 0)
        parseCpsi(resp, r.band, sizeof(r.band), &r.sinr);

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
}

// One signal snapshot for the antenna survey: CSQ + CESQ + CPSI + COPS. Pure AT
// reads (no PPP/+++), safe to call repeatedly while registered but NOT in a data
// session. Carrier/band/metrics fall back to sentinels if unsupported.
struct ModemSignalSample {
    int  rssi;             // 0-31, 99=unknown
    int  rsrp, rsrq, sinr; // dBm/dB, MODEM_SIG_NA=unknown
    char band[16];
    char carrier[32];
};

inline void modemSurveySample(ModemSignalSample* s) {
    char resp[512];
    s->rssi = 99;
    s->rsrp = s->rsrq = s->sinr = MODEM_SIG_NA;
    s->band[0] = '\0';
    s->carrier[0] = '\0';

    if (modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000) > 0) {
        char* p = strstr(resp, "+CSQ:");
        if (p) sscanf(p, "+CSQ: %d", &s->rssi);
    }
    if (modem_at_cmd("AT+CESQ", resp, sizeof(resp), 2000) > 0)
        parseCesq(resp, &s->rsrp, &s->rsrq);
    if (modem_at_cmd("AT+CPSI?", resp, sizeof(resp), 2000) > 0)
        parseCpsi(resp, s->band, sizeof(s->band), &s->sinr);
    if (modem_at_cmd("AT+COPS?", resp, sizeof(resp), 2000) > 0) {
        char* q1 = strchr(resp, '"');
        if (q1) {
            char* q2 = strchr(q1 + 1, '"');
            if (q2) {
                int n = q2 - q1 - 1;
                if (n > 31) n = 31;
                memcpy(s->carrier, q1 + 1, n);
                s->carrier[n] = '\0';
            }
        }
    }
}

// Modem init, part 2: network registration + RSSI/operator + APN + PPP dial.
// Call after modemRunInitPre() (on the firmware, after the baud upgrade). Fills
// r.registered / r.rssi / r.operatorName / r.connected in place.
inline void modemRunInitPost(ModemInitResult& r) {
    char resp[512];

    // Register + read signal (shared with the survey path), then dial PPP.
    modemRegisterAndReadSignal(r);

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
}

// Full init = Pre + Post (single-baud path used by the emulator and unit tests).
inline ModemInitResult modemRunInit() {
    ModemInitResult r = modemRunInitPre();
    modemRunInitPost(r);
    return r;
}

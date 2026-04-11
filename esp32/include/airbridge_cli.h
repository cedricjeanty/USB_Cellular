#pragma once
// AirBridge CLI command parsing — extracted from processCLI() for testing
// Hardware-independent commands that use HAL NVS.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_wifi_creds.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

// Result of CLI command processing
struct CliResult {
    bool handled;       // true if command was recognized
    char output[256];   // response text
};

// Parse and execute SETWIFI command. Returns parsed SSID/pass.
inline CliResult cliSetWifi(const char* args) {
    CliResult r = { true, "" };
    const char* sp = strchr(args, ' ');
    char ssid[33] = "", pass[65] = "";
    if (sp) {
        int slen = sp - args;
        if (slen > 32) slen = 32;
        memcpy(ssid, args, slen);
        ssid[slen] = '\0';
        strlcpy(pass, sp + 1, sizeof(pass));
    } else {
        strlcpy(ssid, args, sizeof(ssid));
    }
    if (!ssid[0]) {
        snprintf(r.output, sizeof(r.output), "SETWIFI <ssid> <pass>");
        return r;
    }
    saveNetwork(ssid, pass);
    snprintf(r.output, sizeof(r.output), "WiFi saved: '%s'", ssid);
    return r;
}

// Parse and execute SETS3 command.
inline CliResult cliSetS3(const char* args) {
    CliResult r = { true, "" };
    char apiHost[128], apiKey[64];
    if (sscanf(args, "%127s %63s", apiHost, apiKey) < 2) {
        snprintf(r.output, sizeof(r.output), "SETS3 <api_host> <api_key>");
        return r;
    }
    if (g_hal && g_hal->nvs) {
        g_hal->nvs->set_str("s3", "api_host", apiHost);
        g_hal->nvs->set_str("s3", "api_key", apiKey);
    }
    snprintf(r.output, sizeof(r.output), "S3 saved: api_host=%s", apiHost);
    return r;
}

// Parse and execute SETMODE command. Returns 0=CDC, 1=MSC, -1=error, -2=query.
struct SetModeResult {
    int mode;           // 0=CDC, 1=MSC, -1=error, -2=query (no change)
    char output[128];
};

inline SetModeResult cliSetMode(const char* args, bool currentMscOnly) {
    SetModeResult r = { -2, "" };
    while (*args == ' ') args++;
    if (!args[0]) {
        snprintf(r.output, sizeof(r.output), "current=%s  usage: SETMODE CDC | SETMODE MSC",
                 currentMscOnly ? "MSC" : "CDC");
        return r;
    }
    if (strcasecmp(args, "CDC") == 0) {
        r.mode = 0;
    } else if (strcasecmp(args, "MSC") == 0) {
        r.mode = 1;
    } else {
        r.mode = -1;
        snprintf(r.output, sizeof(r.output), "unknown mode '%s' — use CDC or MSC", args);
        return r;
    }
    if (g_hal && g_hal->nvs) {
        g_hal->nvs->set_u8("usb", "msc_only", (uint8_t)r.mode);
    }
    snprintf(r.output, sizeof(r.output), "USB mode set to %s — reboot to apply",
             r.mode ? "MSC-only" : "CDC+MSC");
    return r;
}

#pragma once
// AirBridge WiFi credential management — MRU network list via HAL NVS
// Extracted from main.cpp for testing.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include <cstdio>
#include <cstring>

#define MAX_KNOWN_NETS 5

struct NetCred {
    char ssid[33];
    char pass[65];
};

inline int loadKnownNets(NetCred* out) {
    if (!g_hal || !g_hal->nvs) return 0;
    int32_t n = 0;
    g_hal->nvs->get_i32("wifi", "count", &n);
    if (n > MAX_KNOWN_NETS) n = MAX_KNOWN_NETS;
    for (int i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        if (!g_hal->nvs->get_str("wifi", ks, out[i].ssid, sizeof(out[i].ssid)))
            out[i].ssid[0] = '\0';
        if (!g_hal->nvs->get_str("wifi", kp, out[i].pass, sizeof(out[i].pass)))
            out[i].pass[0] = '\0';
    }
    return (int)n;
}

inline void saveNetwork(const char* ssid, const char* pass) {
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    // Remove existing entry for this SSID
    int j = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(nets[i].ssid, ssid) != 0) nets[j++] = nets[i];
    n = j;
    // Prepend (MRU)
    if (n >= MAX_KNOWN_NETS) n = MAX_KNOWN_NETS - 1;
    memmove(&nets[1], &nets[0], sizeof(NetCred) * n);
    strlcpy(nets[0].ssid, ssid, sizeof(nets[0].ssid));
    strlcpy(nets[0].pass, pass, sizeof(nets[0].pass));
    n++;

    g_hal->nvs->set_i32("wifi", "count", n);
    for (int i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        g_hal->nvs->set_str("wifi", ks, nets[i].ssid);
        g_hal->nvs->set_str("wifi", kp, nets[i].pass);
    }
}

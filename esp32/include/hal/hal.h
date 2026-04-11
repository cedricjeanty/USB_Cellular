#pragma once
// AirBridge Hardware Abstraction Layer
// Global g_hal pointer holds interface implementations.
// ESP32: real hardware. Native tests: in-memory mocks.

#include "hal/display.h"
#include "hal/clock.h"
#include "hal/nvs.h"
#include "hal/filesys.h"
#include "hal/network.h"

struct HAL {
    IDisplay*  display;
    IClock*    clock;
    INvs*      nvs;
    IFilesys*  filesys;
    INetwork*  network;
};

extern HAL* g_hal;

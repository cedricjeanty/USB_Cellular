#pragma once
// AirBridge trigger logic — pure functions for state machine decisions
// Extracted from main_loop_task for testing.

#include <cstdint>

#define QUIET_WINDOW_MS 15000UL

// Returns true if harvest should be triggered.
// All timing values in milliseconds.
inline bool shouldHarvest(bool harvesting, bool writeDetected, bool hostWasConnected,
                          uint32_t lastWriteMs, uint32_t lastHarvestMs,
                          uint32_t harvestCoolMs, uint32_t now) {
    if (harvesting) return false;
    if (!writeDetected) return false;
    if (!hostWasConnected) return false;
    if (lastWriteMs == 0) return false;
    if (now < lastWriteMs) return false;  // overflow guard
    if ((now - lastWriteMs) < QUIET_WINDOW_MS) return false;
    if ((now - lastHarvestMs) < harvestCoolMs) return false;
    return true;
}

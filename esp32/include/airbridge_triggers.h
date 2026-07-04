#pragma once
// AirBridge trigger logic — pure functions for state machine decisions
// Extracted from main_loop_task for testing.

#include <cstdint>

#define QUIET_WINDOW_MS 15000UL

// A ready harvest must not START while the host is mid-transaction. The harvest holds
// the SD mutex for ~1-2s while it remounts P1 as FATFS; during that hold the MSC
// read10/write10 callbacks return not-ready, so a host operation that spans many
// sectors (e.g. a FAT scan for mkdir on the 8GB FAT32 P1 — several seconds over USB)
// fails mid-op with EIO even though the SD is fine. The 15s write-quiet trigger doesn't
// catch this because those are READS after the last write. So gate on last MSC I/O
// (reads OR writes, g_lastIoMs): defer the harvest until the host has been quiet for
// HARVEST_HOST_IO_GUARD_MS. Field-safe: the aircraft DSU writes a batch then goes quiet,
// so this adds at most a couple seconds; a real DSU does not poll continuously.
#define HARVEST_HOST_IO_GUARD_MS 1500UL
// Liveness cap: never defer a ready harvest longer than this, so a pathologically
// chatty host can't starve harvest forever (accepts one possible collision after the cap).
#define HARVEST_MAX_DEFER_MS 30000UL

// True ⇒ a harvest that shouldHarvest() has OK'd should be DEFERRED this cycle because
// the host did MSC I/O within guardMs. deferStartMs is when deferral began (0 = not yet
// deferring); once (now - deferStartMs) >= maxDeferMs the harvest proceeds regardless.
// Pure + unit-tested (test_native_triggers). now/lastIoMs use millis() wrap-safe deltas.
inline bool harvestShouldDeferForHost(uint32_t lastIoMs, uint32_t now, uint32_t guardMs,
                                      uint32_t deferStartMs, uint32_t maxDeferMs) {
    if (lastIoMs == 0) return false;                          // host has done no I/O
    if ((uint32_t)(now - lastIoMs) >= guardMs) return false;  // host quiet long enough
    if (deferStartMs != 0 && (uint32_t)(now - deferStartMs) >= maxDeferMs) return false; // cap
    return true;                                              // host active → defer
}

// Returns true if harvest should be triggered.
// All timing values in milliseconds.
// immediateHarvest: set true when files were found at boot (pre-existing from a
// previous session) — bypasses the quiet window since there is no active writer.
inline bool shouldHarvest(bool harvesting, bool writeDetected, bool hostWasConnected,
                          uint32_t lastWriteMs, uint32_t lastHarvestMs,
                          uint32_t harvestCoolMs, uint32_t now,
                          bool immediateHarvest = false) {
    if (harvesting) return false;
    if (!writeDetected) return false;
    if (!hostWasConnected) return false;
    if (!immediateHarvest) {
        if (lastWriteMs == 0) return false;
        if (now < lastWriteMs) return false;  // overflow guard
        if ((now - lastWriteMs) < QUIET_WINDOW_MS) return false;
    }
    if ((now - lastHarvestMs) < harvestCoolMs) return false;
    return true;
}

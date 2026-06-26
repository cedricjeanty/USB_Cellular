// FatFs configuration for native (non-ESP32) builds.
// Strips FreeRTOS / ESP-IDF dependencies so ff.c compiles under the
// native PlatformIO environment for unit testing.
// Based on the ESP-IDF ffconf.h with reentrancy and ESP extensions removed.

#pragma once
#define FFCONF_DEF 80286

// ── Functional config ─────────────────────────────────────────────────────
#define FF_FS_READONLY  0
#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0
#define FF_USE_MKFS     1   // need f_mkfs / f_fdisk
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  3

// ── Namespace / locale ────────────────────────────────────────────────────
#define FF_CODE_PAGE    437     // US (required even with LFN disabled)
#define FF_USE_LFN      1       // long filenames — the harvest produces __-flattened names >8.3 (static work buffer; FatFs ops are serialized under the SD lock)
#define FF_MAX_LFN      255
#define FF_LFN_UNICODE  0
#define FF_LFN_BUF      255
#define FF_SFN_BUF      12

// ── Drive / volume ────────────────────────────────────────────────────────
// 4 drives: 0=raw disk, 1=P2 offset, 2=P1 offset, 3=spare
#define FF_VOLUMES      4
#define FF_STR_VOLUME_ID 0
#define FF_VOLUME_STRS  "0","1","2","3"
// FF_MULTI_PARTITION must be 1 to compile f_fdisk().
// With FF_MULTI_PARTITION=1 and LD2PT(vol)=0 (VolToPart not provided),
// FatFs uses auto-partition search — the same behaviour as =0 for our tests.
#define FF_MULTI_PARTITION 1

// ── Sector size ───────────────────────────────────────────────────────────
// Only 512-byte sectors needed for SD card simulation
#define FF_MIN_SS   512
#define FF_MAX_SS   512

// ── Long-address / exFAT ─────────────────────────────────────────────────
#define FF_LBA64        0
#define FF_FS_EXFAT     0
#define FF_FS_NORTC     1   // no RTC — use timestamp 0
#define FF_NORTC_MON    1
#define FF_NORTC_MDAY   1
#define FF_NORTC_YEAR   2026

// ── Misc ──────────────────────────────────────────────────────────────────
#define FF_FS_LOCK      0       // no open-file locking
#define FF_FS_REENTRANT 0       // no threading (no FreeRTOS)
#define FF_FS_TINY      0
#define FF_FS_RPATH     0
#define FF_USE_TRIM     0
#define FF_USE_DYN_BUFFER 0

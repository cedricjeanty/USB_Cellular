# Intermittent Memory-Corruption Crash — Investigation & Fix Plan

**Status:** Root cause *manifestation* found (2026-07-06 via decoded coredump). The
corruption **source** is not yet located. This doc is the focused-effort starting point.

---

## 1. The bug (what the coredump says)

Decoded from a real on-device coredump (`/tmp/coredump.bin`, 9828-byte valid image):

```
Crashed task:  'main'
Panic reason:  assert failed: find_desc_for_source intr_alloc.c:183 (svd != NULL)
exccause:      0x1d  StoreProhibitedCause
excvaddr:      0x0            <-- a WRITE to NULL
pc/registers:  CORRUPTED — a1(SP)=0xa0a1ca3f (not a valid RAM address),
               repeating "…ca3f" pattern across a2/a7/a10-a12/a15,
               backtrace unrecoverable ("Cannot access memory")
```

**Interpretation:** this is **runtime memory corruption**. Something writes out of bounds
or to freed/invalid memory, which (a) smashes the stack (garbage SP) and (b) nulls/corrupts
an ESP-IDF interrupt-descriptor structure, so the next interrupt operation hits
`find_desc_for_source`'s `svd != NULL` assert and a null-pointer store (`excvaddr=0x0`).
The `intr_alloc` assert is the **symptom**, not the source.

**This explains every prior symptom** (see `memory/project_boot_crash_after_suite.md`):
- **Intermittent** — a timing/state race (boot_0151 ran 5 h clean; other boots crash in <1 s).
- **Inconsistent** — sometimes `reset=4` panic, sometimes a silent main-loop wedge.
- **Survives an SD reformat** — it is NOT SD/NVS content; it is a code-level memory-safety bug.
- Likely the same interrupt-alloc race family the code already half-fixed with `g_tlsMutex`
  (see `esp32/src/main.cpp:~90` — the mbedTLS lazy AES-interrupt alloc race), but here it
  fires in the **main task during boot**.

---

## 2. The fix plan (find the SOURCE)

The smashed stack means this dump shows only the manifestation. To catch the bad write **at
its source**:

1. **Build a debug variant** with corruption detection ON (a new `esp32s3-debug` env, or add
   to `sdkconfig`):
   - `CONFIG_HEAP_CORRUPTION_DETECTION=COMPREHENSIVE` (heap canaries + poisoning — traps an
     overrun/UAF at the next heap op, close to the culprit)
   - `CONFIG_COMPILER_STACK_CHECK_MODE=ALL` (stack-smashing canaries)
   - Keep coredump-to-flash ON (already on).
   - Pair with **CDC serial** (build `esp32s3-e2e` + the debug flags) so the panic prints the
     backtrace live — no coredump dance needed for each iteration.
2. **Reproduce** on Unit 1 (CoolGear power-cycle intermittently triggers it; may take several
   cycles). With corruption detection on, the panic should land AT the bad write → clean
   backtrace → the exact culprit.
3. **Audit targets** while reproducing — interrupt-alloc paths and recent buffer work:
   - SD/SPI, USB/TinyUSB (the custom MSC read10/write10 in `components/esp_tinyusb/`),
     I2C/display, modem UART, AES/TLS interrupt allocation.
   - Recently-touched buffers: the stack-HWM instrumentation, the compress (`airbridge_compress.h`
     PSRAM miniz), multipart `s3up` buffers, the large stack buffers in the upload path.
   - Any `esp_intr_alloc` / ISR registration racing across cores at boot.
4. **Fix** the overrun/UAF/race; verify with the same reproduce loop (corruption detection
   should stay silent through many power-cycles).

### Secondary fix (independent, also valuable)
**Coredump egress during a fast crash-loop.** The dump IS written to flash but never egresses,
because the rapid early-boot crash-loop (~9 s/boot) never lets the PPP-gated egress task run,
and once it settles in Safe Mode the dump apparently isn't shipped. Make Safe Mode reliably
egress any pending coredump so a field unit self-reports the backtrace (this is the whole point
of the coredump work). Check `egressCoredumpTask` / `g_cdDone` gating in `main.cpp:~1517/2955`.

---

## 3. Reproduce + read + decode recipe (proven working)

- **Reproduce:** `python3 scripts/coolgear.py cycle 3` — intermittently hits the crash → rapid
  crash-loop → Safe Mode. `up_stk=16712` on healthy boots (stack is NOT the problem).
- **Enter ROM download mode:** power-cycle **while physically holding the BOOT button** (the RST
  button alone does NOT latch this unit; `jtag_flash.py` auto-catch also fails on it). Device
  enumerates as `303a:1001` on `/dev/ttyACM0`.
- **Read coredump partition:**
  `python3 -m esptool --chip esp32s3 --port /dev/ttyACM0 --before no-reset --after no-reset read-flash 0x3F0000 0x10000 /tmp/coredump.bin`
  (partition offset/size from `esp32/partitions.csv`).
- **Decode:**
  `python3 <framework-espidf>/components/espcoredump/espcoredump.py info_corefile -c /tmp/coredump.bin -t raw --gdb <GDB> esp32/.pio/build/esp32s3/firmware.elf`
  **GDB gotcha:** the bundled `toolchain-xtensa-esp32s3` gdb is BROKEN (needs
  `libpython2.7.so.1.0`). Use
  `~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp-elf-gdb-3.12` (esp-gdb 14.2,
  python3.12). **Decode with the ELF that MATCHES the running fw** (register values are raw, but
  symbol/TCB resolution needs the right elf; rebuild the exact `FW_VERSION` from git if needed).

---

## 4. Context: what shipped 2026-07-06 (branch `gap1-upload-dedup`)

All committed; deployed fw = **`20260706172836`** (staged to BOTH bucket OTA sources).

| Commit | What |
|--------|------|
| `09a4435` | **coredump-to-flash enabled in prod** (was never on → why crashes were undebuggable) + task stack-HWM telemetry (`up_stk/hv_stk/md_stk` in STATUS + heartbeat) + survival-only build (`-DSURVIVAL_ONLY`) |
| `60cce4d` | survival-plane thrash (`scripts/survival_thrash.sh`, proven **20/20 bulletproof**) + power-cut thrash + `read_coredump.py` |
| `a37ca3b` | **carrier-reselection fix** — `AT+COPS=0` when the initial-dial loop is stranded on a dead carrier (fixes the "T-Mobile no bars" strand) |
| `3890cd9` | **runtime cred rollback** — heal a bad `s3` command without 3 reboots (`credsShouldRuntimeRollback` + `s3RuntimeRollback`) |
| `3867f7b` | FW_VERSION bump / deploy |

Robustness gaps that surfaced and are FIXED or noted: coredump-disabled-in-prod (fixed),
carrier-strand (fixed), cred self-sever (fixed), coredump-egress-during-crash-loop (open,
see §2 secondary).

---

## 5. Hardware state (verify at start of the fix effort)

- **Unit 1** = `9C139EF40188` (CoolGear-controlled). Prod build, uploads/heartbeat/logs/commands +
  **OTA source** all on the **`airbridge-uploads` (prod) bucket** keyed by RAW MAC. Just clean-flashed
  to `…172836` (NVS erased). **NOTE the bucket:** watching `airbridge-uploads-test/TEST_9C139EF40188`
  shows a frozen/dead heartbeat — the live telemetry is `airbridge-uploads/heartbeat/9C139EF40188.json`
  + `airbridge-uploads/9C139EF40188/logs/`. C2 command channel: `airbridge-uploads/commands/9C139EF40188/`.
- **Unit 2** = `9C139EF3D3D8` (PCB, direct-USB, NOT CoolGear-controllable). On OLD firmware, no
  heartbeat. Bring to baseline via power-cycle (OTA self-heal) or a BOOT-hold flash. USB volume `sdd1`.
- **CoolGear** hub at `/dev/ttyUSB0` (`scripts/coolgear.py {on|off|cycle}`) — controls Unit 1 power only.

## 6. Key gotchas (don't relearn these)
- Prod-vs-test bucket split for this device (see §5) — watch the RIGHT bucket.
- OTA source Unit 1 reads = the **prod** bucket `firmware/latest.json` (had a stale version; both
  buckets now serve `…172836`).
- Broken bundled gdb (use `xtensa-esp-elf-gdb-3.12`).
- Download mode needs **power-cycle + hold BOOT** on this unit.
- Churning/power-cycling this unit re-rolls the Hologram carrier (can strand it pre-carrier-fix);
  `…172836` has the fix. Don't over-churn.

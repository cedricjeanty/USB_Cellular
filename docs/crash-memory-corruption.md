# Intermittent Memory-Corruption Crash — RESOLVED (2026-07-06)

**Status: FIXED.** Root cause was a **stack overflow in `runCommandTextBuffer()`**
(`esp32/include/airbridge_commands.h`). This doc is kept as a post-mortem: the bug, the
fix, and — most usefully — the **diagnostic method that cracked it**, because the obvious
tool (heap poisoning) actively *masks* this class of bug.

---

## 1. The bug

`runCommandTextBuffer()` declared its parsed-command array **on the stack**:

```c
Command cmds[16];          // 16 * sizeof(Command)(~152 B) ≈ 2.4 KB on the stack
```

It runs deep in the boot call chain —
`app_main → bootProcessMagicFiles → check_p1_magic → run_command_file → runCommandTextBuffer`
— on the **3584-byte main task stack** (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`). The 2.4 KB array
overflowed the stack into adjacent `.data`, smashing `app_main`'s `s_hal` HAL-interface struct
(function pointers) and a heap-allocated ESP-IDF `vector_desc` (interrupt descriptor). The next
`esp_intr_*` operation — the `compress`-directive's log write touching the USB-Serial-JTAG IRQ —
walked the corrupted descriptor and tripped:

```
assert failed: find_desc_for_source intr_alloc.c:183 (svd != NULL)
exccause 0x1d StoreProhibited, excvaddr 0x0, SP/PC garbage (stack smashed)
```

**Why it looked so mysterious:**
- **Intermittent** — whether the smashed bytes actually crash depends on stack layout and
  interrupt timing; some boots limped on (one ran 5 h) before a fatal walk.
- **`find_desc_for_source` is the *manifestation*, not the cause** — the interrupt system just
  happened to be the first thing to dereference the corrupted memory.
- **Pre-task-spawn** — the coredump task list is only `main`/`IDLE`/`ipc`; the crash is in early
  `app_main`, before `upload`/`harvest`/`modem` exist. (Ruled out the mbedTLS AES-intr race.)
- **Needs an `airbridge.cmd`** on P1/P2 to reach the code (the affected unit's had `compress on`).
- **The FreeRTOS canary missed it** (`CONFIG_FREERTOS_CHECK_STACKOVERFLOW=CANARY`): the canary is
  checked only at a context switch — rare for the boot main task — and the overflow leapt *past*
  the canary straight into `.data`.

Tellingly, `run_command_file` had **already** moved its `text[2048]`/`out[2048]` buffers to
`static` to protect this exact stack (see its comment). The `cmds[16]` array *inside*
`runCommandTextBuffer` was the one that got missed.

## 2. The fix

Heap-allocate `cmds` instead of putting it on the stack (`airbridge_commands.h`):

```c
Command* cmds = (Command*)malloc(MAXCMD * sizeof(Command));
if (!cmds) return res;
...
free(cmds);
```

**Heap, not `static`:** `runCommandTextBuffer` is also invoked off the C2/modem task (the S3
command channel calls it directly), so a `static` array would race. The shared header already
uses `malloc` for the same reason (`dumpLogs`/`cmdCopyFile`). Covered by `test_native_commands`
(22/22 pass). Verified on hardware: the unit now boots healthy and presents its USB MSC stably,
where before every power-on went dark within ~85 s.

## 3. Diagnostic method (the reusable part) — heap poisoning is the WRONG tool here

The instinctive move — `CONFIG_HEAP_CORRUPTION_DETECTION=COMPREHENSIVE` — **does not work for a
stack overflow that smashes `.data`**, and worse, it *masks* the crash: both COMPREHENSIVE and
LIGHT heap poisoning changed the heap layout / added per-alloc latency enough that the unit
stopped crashing (it stayed reachable instead of going dark). Proven by A/B: **same commit, only
the sdkconfig differed** — stock crashed, poisoned builds didn't. So if a poisoned build "fixes"
a crash, suspect a layout/timing-sensitive bug (stack overflow, race, wild write), not a heap
overrun poisoning would trap.

**What worked — stock-config progress markers persisted to NVS:**
- Build the **stock** sdkconfig (so the triggering layout/timing is preserved) with a diagnostic
  flag that sprinkles progress markers through early `app_main`.
- Persist each marker to **NVS** (`dbg/mark`, same-key overwrite). The crash still reproduces with
  the markers (unlike poisoning), and **NVS survives the RAM smear** that destroys any in-RAM
  marker. The deepest marker written before the crash = the sub-step where it fired.
- Drove markers coarse→fine (`bootProcessMagicFiles` → `check_p1_magic` → `run_command_file` →
  inside `runCommandTextBuffer`) until the deepest reached was `rc_pre_runcmd` — pinpointing the
  function. Then the coredump's `.data` dump showed the corruption starting at `_data_start`
  (`s_hal`), and the culprit — a 2.4 KB stack array — was obvious from a code read.

**Gotcha — the crash-loop confound:** driving `dbg/boots` high (console-open USB resets, repeated
esptool resets) pushes the unit into **Safe Mode**, which *skips the entire app plane* — so it
stops crashing and looks "fixed." Always verify on a clean **power-on** boot (which forces NORMAL,
not Safe Mode) and don't touch the JTAG console during the run.

## 4. Reproduce + read + decode recipe (proven working)

- **Reproduce:** a clean power-on with an `airbridge.cmd` present on P1/P2. The unit goes **dark on
  USB** (crash-looping before it can present USB) — `lsusb` shows no `303a`/no MSC for it. A healthy
  boot instead presents its MSC (a 2nd `1209:` device / new `/dev/sdX`) at the ~90 s mark. That
  dark-vs-MSC signal is a coredump-free way to tell crashing from healthy.
- **Read the coredump / NVS on this unit:** it goes dark, so esptool can't attach. Get download
  mode by **power-cycle while holding BOOT** (RST alone doesn't latch this unit), or retry
  `scripts/jtag_flash.py`'s warm-catch (intermittently catches the sub-1 s cold-boot JTAG window).
  Then `esptool read-flash 0x3F0000 0x10000 dump.bin` (coredump) or `0x9000 0x5000 nvs.bin` (NVS).
- **Decode:** `espcoredump.py info_corefile -c dump.bin -t raw --gdb <GDB> firmware.elf`.
  - Use `~/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp-elf-gdb-3.12` (the bundled
    `toolchain-xtensa-esp32s3` gdb is broken — wants `libpython2.7`).
  - Decode with an ELF **matching the running fw**. If you rebuild it, esp-coredump's SHA guard
    rejects it (the app-descriptor timestamp differs though code addresses match) — comment out the
    check in `esp_coredump/corefile/loader.py` (~line 398), or set `CONFIG_APP_REPRODUCIBLE_BUILD`.
  - `epc1` may point into the panic handler (`esp_ptr_byte_accessible`) — a secondary fault on the
    smashed stack, not the source. Mine the crashed-task stack for surviving return addresses and
    read `.data`/globals instead.

Related: `memory/project_boot_crash_after_suite.md`, [[project_safe_mode]],
[[project_coredump_disabled_in_prod]].

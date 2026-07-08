# scripts/ — E2E harness & device tooling

- `e2e_lib.sh` is the shared harness library; `e2e_unified.sh` and
  `flight_cycle_test.sh` source it. Set `TARGET=emulator|device` before
  sourcing — config and helpers branch on it.
- Waits are STATE-TRIGGERED on log markers (`wait_for_log`, `wait_for_harvest`,
  `wait_for_upload_complete`, `wait_for_manifest`) — never add fixed sleeps.
- The emulator binary is `esp32/.pio/build/emulator/program`
  (`pio run -e emulator`). NEVER edit `platformio.ini` or rebuild while an
  emulator run is in flight — the build deletes the running binary mid-run.
- Debug a failing E2E test in isolation first; run the full suite only once it
  passes. Known pre-existing failures: TEST 15/16/17/18.
- `--target device` needs a persistent-CDC build (`esp32s3-e2e` env) plus the
  CoolGear hub (`coolgear.py`) for power cycling; `host_dsu.py` plays the
  aircraft DSU. Marker parity with the emulator and the 90s USB presentation
  delay are the usual timing gotchas.

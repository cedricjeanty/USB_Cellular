# esp32/include/ — shared headers

Everything here must be hardware-independent: it compiles into the firmware
(`src/main.cpp`), the SDL emulator (`emu/main.cpp`), AND the native Unity tests.

- No ESP-IDF, FreeRTOS, or Arduino includes. Hardware access goes only through
  the HAL interfaces in `hal/` (`g_hal`: nvs/uart/network/filesys/display/clock).
- Every `airbridge_*.h` has a matching suite in `../test/test_native_*`. New
  logic here ships with a test there — never land one without the other.
- New shared logic belongs in a header here, not in `main.cpp` (ADR 0001).
- `sim_modem.h` / `sim_dsu.h` are simulators for the emulator/tests only —
  never compiled into firmware.
- `airbridge_cli.h` and `airbridge_wifi_creds.h` are extracted + tested but
  DISABLED in the current firmware (no interactive CLI; WiFi off on the
  cellular branch). Don't wire them up without asking.

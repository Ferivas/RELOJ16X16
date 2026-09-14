# AGENTS.md — RELOJ16X16

PlatformIO + Arduino project: 16×16 LED clock (4× 8×8 matrices) on ESP32 (`esp32dev`).

## Commands

- Build: `pio run -e esp32dev`
- Upload firmware: `pio run -e esp32dev -t upload`
- Upload LittleFS image (`data/` → device): `pio run -e esp32dev -t uploadfs` — required after changing `data/config.json`
- Serial monitor: `pio device monitor -b 115200` (matches `monitor_speed` + `Serial.begin`)
- No tests, linter, or CI in repo. Verify with `pio run` (and `pio check` only if PlatformIO cppcheck is installed).

## Toolchain pin (do not "modernize")

- `platformio.ini` pins `espressif32@6.8.1` = Arduino core **2.x** timer API. Keep this style in `src/main.cpp`:
  `timerBegin(0, 80, true)` / `timerAttachInterrupt(..., true)` / `timerAlarmWrite(timer, 2000, true)` / `timerAlarmEnable`.
  The commented-out core 3.x snippet in `setup()` is intentionally disabled.

## Architecture (`src/` only — `lib/`, `include/`, `test/` are empty stubs)

- `src/main.cpp` — everything: 2 ms HW-timer multiplex ISR (`handleInterrupt`, `IRAM_ATTR`), 3 font tables (`FUENTES`), minute-change animation, buttons, NTP, `/config.json` persistence. Single env, no build flags beyond `CORE_DEBUG_LEVEL=3`, filesystem `littlefs`.
- `src/wifimgr.{h,cpp}` — WiFiManager port. Entry `wifi_get_connection()` blocks forever in AP-portal mode until STA connects; `setup()` halts if it ever returns false.

## Gotchas agents miss

- ISR `handleInterrupt` runs every 2 ms with `delayMicroseconds` for brightness via `PIN_OE`. Keep it `IRAM_ATTR`, no `Serial`/alloc/String; shared state uses `volatile` (`NIVEL_BRILLO`, `interruptCounter`, `flaginternet`). `buffram` bytes 0..7 are the "no-WiFi" icon window (blinked from ISR while `!flaginternet`, cleared in `setup()` after connect); other bytes are written from `loop()` — keep updates byte-sized.
- Hardware revision is a compile-time `#define VERSION_HW 2` in `main.cpp` (pins `LEDPIN`, `PIN_OE`, `PIN_DATA/CL​K/LOAD/MISO` change between v1/v2). Editing pins: change the `#if` block, not call sites.
- Buttons (`pin_fuente`, `pin_brillo`, `INPUT_PULLUP`, LOW-active) use non-blocking debounce (`boton_pulsado`, 50 ms stable, one shot per press) + deferred `guardar_config()` (single flash write 1.5 s after last press — flash writes can stall the 2 ms sweep and flash a stuck column). Canonical default pins are 18/19 in both code and `data/config.json` — devices with an old LittleFS `/config.json` (16/17) keep their stored copy until `uploadfs`/reformat.
- Config parsing is a minimal `json_get_int` (integer `"key": N` only). Adding a string/bool/float setting requires extending the parser, `Config`, `guardar_config`, and `cargar_config` together.
- NTP: `configTime(gmt_offset, 0, "pool.ntp.org")`, default `gmt_offset -18000` (UTC-5). Resync period is computed once as `static INTERVALO_NTP_SEG` from `config.n_horas` at boot — changing `n_horas` at runtime needs a reboot to take effect.
- WiFi profiles `/wifi.dat` format is `ssid;password` per line. Portal AP is `RELOJ2023` / `reloj2023` at `192.168.4.1`; its URL-decode handles **only** `%3F`/`%21` — do not replace with a full decoder without reason.
- `data/config.json` is the LittleFS upload image template; runtime reads `/config.json` from the device (auto-created with defaults if missing).

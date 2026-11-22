# Energia ESP32 department display

## Arduino IDE sketch (current hardware deliverable)

The primary artifact for direct Arduino IDE compilation is:

`arduino/EnergiaEsp32/EnergiaEsp32.ino`

It targets the supplied wiring: SSD1306 OLED on SDA GPIO21 / SCL GPIO22 at
`0x3C`, NAV on GPIO27, and POWER on GPIO32. There is **no microphone, recording,
upload, or voice code**. The device is display-only: it fetches server-approved
summaries from `GET /api/v1/devices/<uuid>/department-summaries/` and shows them
on the OLED. The device never computes a score itself; the backend supplies the
`overall_score` and `top_employee` fields already aggregated.

Button behavior:

| Button | Action | Result |
| --- | --- | --- |
| NAV (GPIO27), short press | next page | Cycles each department, then OVERALL SCORE, then BEST EMPLOYEE, then wraps |
| NAV (GPIO27), long press (~1.2 s) | refresh now | Re-fetches the summary immediately |
| POWER (GPIO32), short press | display off / on | Toggles the OLED without sleeping |
| POWER (GPIO32), long press (~2 s) | deep sleep | Enters deep sleep; press POWER again to wake |

Install **Adafruit GFX Library**, **Adafruit SSD1306**, and **ArduinoJson (7.x)**
in Arduino IDE, select an ESP32 board, and open the `.ino`. The LOCAL CONFIG
block is already filled with the current demo settings so department data can
appear immediately:

| Setting | Value |
| --- | --- |
| `WIFI_SSID` / `WIFI_PASS` | your WiFi network (from your earlier sketch) |
| `API_BASE` | `https://street-swim-investor-cave.trycloudflare.com` |
| `DEVICE_ID` | `78671e4c-7aaa-461c-a87e-08ab7f38546e` (seeded "Lobby display") |
| `DEVICE_TOKEN` | `energia-demo-device-token` (seeded demo device token) |

These are demo/temporary values: the Cloudflare Quick Tunnel URL expires, and
`DEVICE_TOKEN` is a bearer credential. Before committing this sketch or going
to production, replace the token with a device-specific one from Django admin
and keep it out of source control. `PAGE_SECONDS` controls automatic page
cycling when the NAV button is not pressed.

The PlatformIO project and host-side logic tests below remain available as the
read-only department-display reference implementation.

## PlatformIO reference firmware

PlatformIO/Arduino firmware for an ESP32 driving a 128x64 SSD1306 I2C OLED with
exactly two physical buttons. The device is strictly read-only: it fetches
server-approved department summaries and never computes scores itself.

Story mapping is inline in `src/main.cpp` and `src/energia_ui_model.*`.

## Quick start

```bash
# Firmware (requires PlatformIO)
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor

# Hardware-free logic tests (requires only g++/make)
make -C tests test
```

Full wiring, provisioning, endpoint contract and deployment notes are in
[`include/README.md`](include/README.md).

## Repository layout

| Path | Purpose |
| --- | --- |
| `platformio.ini` | `esp32dev` + Arduino, Adafruit GFX/SSD1306, ArduinoJson, WiFiManager; `time` from the core |
| `src/main.cpp` | Provisioning, HTTPS polling, NVS cache, watchdog, deep sleep, OLED rendering |
| `src/energia_ui_model.{h,cpp}` | Pure C++ finite-state UI, debouncing, bounded formatters, redraw signature |
| `tests/` | g++ host tests and minimal stubs; no hardware required |
| `include/README.md` | Wiring, provisioning steps, JSON contract, build/flash commands |

## Validation status

In this workspace `pio`, `platformio` and `arduino-cli` are **not** installed, so
the hardware build was not compiled here. The host-side pure logic suite was
compiled with `g++ -std=c++17 -Wall -Wextra -Werror -pedantic` and executed
successfully (112 checks, 0 failures). A full hardware build requires
PlatformIO as configured in `platformio.ini`.

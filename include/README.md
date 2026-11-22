# Energia ESP32 display — wiring, provisioning and contract

Firmware target: ESP32 (`esp32dev`), Arduino framework, 128x64 SSD1306 OLED on
I2C, exactly two momentary push buttons. No secrets are stored in source.

## 1. Wiring

| Signal | ESP32 GPIO | Notes |
| --- | --- | --- |
| OLED SDA | GPIO 21 | I2C data |
| OLED SCL | GPIO 22 | I2C clock |
| OLED VCC | 3V3 | SSD1306 must be 3.3V tolerant |
| OLED GND | GND | common ground |
| `BUTTON_NAV` | GPIO 32 | button to GND, `INPUT_PULLUP`, active LOW |
| `BUTTON_POWER` | GPIO 33 | button to GND, `INPUT_PULLUP`, active LOW; RTC GPIO used for deep-sleep wake |

No external pull-ups are required (internal pull-ups are enabled). A small
100 nF capacitor across each button is optional for extra debouncing; the
firmware already debounces at 40 ms. OLED I2C address is `0x3C`.

### Button behavior

| Action | Result |
| --- | --- |
| `BUTTON_NAV` short press (< 1200 ms) | Next department; list wraps and scrolls the highlight |
| `BUTTON_NAV` long press (>= 1200 ms) | List: open selected department detail; detail: return to list (short press is ignored on detail) |
| `BUTTON_POWER` short press | Display off / on. Polling continues while the panel is off |
| `BUTTON_POWER` long press | Request deep sleep; firmware waits for button release, then sleeps with EXT0 wake on the same active-low GPIO. Wake forces the display on |
| Both buttons held ~2 s during boot | Clear Preferences + WiFiManager settings and reopen the captive portal (re-provision / rotate token) |

## 2. Provisioning

1. Flash the firmware and power the board.
2. Join the AP `energia-esp32` (open network) and visit `http://192.168.4.1`.
3. Fill the captive portal fields:
   - **WiFi SSID / password** — normal WiFiManager fields.
   - **Device ID** — 1–64 chars, `[A-Za-z0-9_-]` only; used in the URL path.
   - **HTTPS API base URL** — must start with `https://`, e.g.
     `https://api.example.com`. Trailing slashes are removed; userinfo (`@`)
     is rejected.
   - **Device token** — revocable bearer token issued by the backend. It is
     never printed to serial and never compiled into the binary.
   - **Poll seconds** — clamped to 60–86400; default 300 (5 minutes).
   - **UTC offset minutes** — used only to format the on-screen clock.
   - **Root CA PEM** — paste the PEM with literal `\n` escapes between lines,
     for example:
     `-----BEGIN CERTIFICATE-----\n<BASE64...>\n-----END CERTIFICATE-----\n`
     Use the CA that signs the API server certificate. HTTPS certificate
     validation is mandatory; there is no `setInsecure()` fallback.
4. Save. The device connects, syncs NTP (best effort) and polls immediately.

All values are persisted with Arduino `Preferences` in namespace `energia`.
WiFiManager also keeps its own NVS copy of custom portal fields; the
both-buttons-at-boot reset clears both stores. If WiFi or config is invalid the
portal reopens automatically on boot, so no third button is needed.

### Security / revocation

- Requests are `GET {apiBase}/api/v1/devices/{deviceId}/department-summaries`
  with `Authorization: Bearer <token>`, `Accept: application/json` and
  `User-Agent: energia-esp32/1.0`.
- A `401`/`403` response means the token was revoked: polling stops immediately,
  the UI shows `TOKEN REVOKED / REPROVISION`, and cached data remains flagged
  stale. Re-provision with the two-button boot reset.
- Network errors and `429`/`5xx` get at most 3 attempts per poll cycle
  (1 s, then 4 s backoff), then the device waits one full poll interval.
- The firmware is read-only. It sends no user IDs, evidence, scores or
  employee records — only the device ID in the path and the token in the header.

## 3. Endpoint JSON contract

Response `200 application/json`:

```json
{
  "schema": "energia.department-summaries.v1",
  "generatedAt": 1763788800,
  "recognitionEnabled": true,
  "departments": [
    {
      "name": "Fabrication",
      "statusLabel": "On track",
      "scoreState": "final",
      "score": 87,
      "lastUpdate": 1763788700,
      "topEmployee": {
        "displayName": "A. Silva",
        "score": 95,
        "eligible": true
      }
    },
    {
      "name": "Warehouse",
      "statusLabel": "No data",
      "scoreState": "unavailable",
      "score": null,
      "lastUpdate": 1763700000,
      "topEmployee": null
    }
  ]
}
```

| Field | Type | Rules |
| --- | --- | --- |
| `schema` | string | Must be exactly `energia.department-summaries.v1` |
| `generatedAt` | uint32 epoch seconds | Server generation time; used as fallback for missing `lastUpdate` |
| `recognitionEnabled` | bool | `false` renders `TOP: policy off` |
| `departments` | array | Up to 12 entries are kept; extras are ignored |
| `departments[].name` | string | Truncated safely on screen with a `~` marker |
| `departments[].statusLabel` | string | Short server-approved status label |
| `departments[].scoreState` | string | `final`, `provisional` or `unavailable` |
| `departments[].score` | integer or null | 0–100 for `final`/`provisional`; `null` when unavailable. Device copies it verbatim and never recalculates it |
| `departments[].lastUpdate` | uint32 epoch seconds | Per-department update time shown as `UPD HH:MM (age)` |
| `departments[].topEmployee` | object or null | Only when policy allows recognition and an eligible employee exists |
| `topEmployee.displayName` | string | Approved alias only — **no user IDs or evidence** |
| `topEmployee.score` | integer | 0–100, selected server-side (EN-INT-008), never recomputed here |
| `topEmployee.eligible` | bool | `false` or missing renders `TOP: none this period` |

Unknown/extra JSON fields are ignored. The device caches a freshly serialized
allow-list-only copy in `Preferences["lastJson"]` (max ~4 KB). On reboot or any
failed/offline poll, cached data is displayed with `DEPTS!`, `OFFLINE`, `! STALE`
and a `!` status prefix. Stale threshold is `max(3 × poll interval, 15 min)`.

## 4. UI model and rendering

`src/energia_ui_model.cpp` is plain C++ with no Arduino dependencies. It owns:

- the list/detail finite-state machine and smooth list window scrolling;
- debounced short/long press decoding for both buttons;
- bounded, null-safe string truncation and all OLED line formatters;
- a content signature used by `main.cpp` to redraw only when state or content
  changes (minute tick on detail pages).

The display is monochrome, so the workspace palette is translated to contrast:
deep navy `#102A43` → dark background, teal `#0F766E` → ASCII status icons,
lime `#A3E635` → inverted highlight bar, light background → white pixels.
Meaning is carried by icons/labels (`[i]`, `[~]`, `[?]`, `!`, `>`), not color.

Scores are always rendered from the server value. `final` → `SCORE 87/100`,
`provisional` → `PROV ~74/100`, `unavailable` → `SCORE UNAVAILABLE`.

## 5. Build, flash and test

```bash
# Firmware — requires PlatformIO
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor

# Host logic tests — requires only g++ and make, no hardware
make -C tests test
make -C tests clean
```

`platformio.ini` pins `espressif32@6.7.0` and lists Adafruit GFX, Adafruit
SSD1306, ArduinoJson and WiFiManager. The `time` library is supplied by the
Arduino-ESP32 core (`<time.h>`), so no registry package is needed for it.

In the validation environment for this deliverable, `pio`, `platformio` and
`arduino-cli` were unavailable; the hardware build therefore requires
PlatformIO on the machine doing the flashing. The pure C++ suite was compiled
and run with:

```
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic \
    test_energia_ui_model.cpp ../src/energia_ui_model.cpp -o build/test_energia_ui_model
./build/test_energia_ui_model
energia_ui_model tests: 112 checks, 0 failures
```

## 6. Runtime watchdog and deep sleep

- A 20 s hardware task watchdog is armed after provisioning and fed every
  `loop()` iteration. It is intentionally not armed while the blocking
  WiFiManager captive portal is open.
- Long-pressing `BUTTON_POWER` fires the long event once while held; the actual
  `esp_deep_sleep_start()` happens only after release and debounce, so the EXT0
  wake level (LOW) is not already asserted when sleep begins.
- Wake reason `ESP_SLEEP_WAKEUP_EXT0` shows a short `WAKE / POWER BUTTON` splash
  and forces the display on.

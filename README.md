# Cardputer XL

A single-file "OS"/app launcher firmware for the [M5Stack Cardputer-ADV](https://docs.m5stack.com/en/core/Cardputer), built as one big Arduino sketch instead of a collection of separate apps.

Everything renders on an **external ILI9341 320x240 panel** wired to the Cardputer-ADV's EXT header — the launcher, every built-in app, the lock screen, all of it. The Cardputer's own built-in screen is only used for backlight sleep/wake.

## What's in it

Around 30 built-in apps, reachable from the Home screen or the full app list:

- **System tools** — device info, Wi-Fi scanner/monitor, Wi-Fi setup, a local web companion server, file browser, favourites, text tools (case/base64), a calculator, notes, a clock
- **C LAB** — a small on-device IDE for "CardC", a deliberately tiny, safe, local-only C-style scripting language this firmware interprets itself, plus a one-line CardC REPL console and a set of example programs
- **Games** — Snake, Grid Hunt, and **Kart Racer**, a full 3D wireframe/solid go-kart time-trial game with 5 procedurally generated tracks, drift boost, wall collision, best-lap saving, and haptic/audio feedback, rendered live with an off-screen double buffer
- **Creative** — Mini Paint (16x12 pixel canvas), Music Lab (a 4-track/16-step drum sequencer), QR code tools, a device self-test, dice/random
- **Security** — a PIN lock screen, an encrypted TOTP/loyalty-code vault, BLE keyboard mode (turns the Cardputer into a Bluetooth HID keyboard)
- **Floating quick-launch menu** — press **Opt** from anywhere, on any app, to pop up the same 6 tiles as Home without leaving what you're doing

## Controls

The Cardputer's `;` `,` `.` `/` keys double as printed directional arrows (up/left/down/right). A few keys are global, from any app:

| Key | Action |
|---|---|
| `Fn` | Back / cancel (closes dialogs, returns to Home) |
| `Opt` | Toggle the floating quick-launch menu |
| `Ctrl+F` | Favourite/unfavourite the current app |

## Building

- **Board:** `esp32:esp32:m5stack_cardputer` (arduino-esp32 core 3.x)
- **Partition scheme:** this sketch does **not** fit in the board's default 1.2MB app partition — build with **"Huge APP (3MB No OTA/1MB SPIFFS)"**:
  - Arduino IDE: *Tools → Partition Scheme → Huge APP*
  - arduino-cli: `--fqbn esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app`
- **Libraries:** the usual M5Stack/Adafruit stack (M5Cardputer, M5Unified, M5GFX, Adafruit GFX Library, Adafruit ILI9341), plus:
  - **QRCode** by Richard Moore — its `qrcode.c`/`.h` are vendored directly in this folder rather than relied on as an installed library, because the ESP32 core ships its own unrelated internal `qrcode.h` under the same name that otherwise shadows it
  - **HijelHID_BLEKeyboard** + **NimBLE-Arduino** — for the BLE Keyboard app

```
arduino-cli compile --upload -p <PORT> --fqbn esp32:esp32:m5stack_cardputer:PartitionScheme=huge_app CardputerXL
```

### Why `CardputerXL.ino` is nearly empty

The actual firmware lives in `CardputerXL.cpp`, compiled alongside the `.ino` as part of the same sketch. Arduino's `.ino` build step auto-generates forward declarations for every function it finds and inserts them all above the sketch's own code — before this file's own `struct` types (`KVec3`, `KartCam`, ...) are even defined, which breaks the build. A `.cpp` is compiled as an ordinary top-to-bottom translation unit with no such insertion, so it just works; the sketch keeps its own manual forward-declaration block at the top instead. See the comment above `struct KVec3` in `CardputerXL.cpp` for the full explanation.

## Hardware wiring (external ILI9341)

| Signal | GPIO |
|---|---|
| CS | 5 |
| RST | 15 |
| DC | 13 |
| MOSI | 3 |
| SCK | 6 |
| Backlight | 39 (PWM) |

Plus a GPIO-driven haptic motor (GPIO1) and the Cardputer-ADV's addressable RGB LED (GPIO21 data, GPIO38 power gate).

## Architecture, for anyone editing this

See the large comment block at the very top of `CardputerXL.cpp` — it covers the rendering model (single unbuffered panel + a `redrawNeeded`/`draw()`/`refreshLocalPage()` dirty-flag dispatch, with one exception: Kart Racer's live 3D view uses an off-screen canvas), the input dispatch (`keyboard()`), and how state is persisted (ESP32 `Preferences`/NVS, not a filesystem).

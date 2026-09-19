# Cardputer Companion (Android)

A native Android companion app for CardputerXL's **BLE COMPANION** app (in
the device's APPS list) - remote control (D-pad, quick app launches,
quick settings), and Notes/C LAB code read-write, all over a direct BLE
connection. No shared Wi-Fi network needed.

## How it talks to the device

The Cardputer's BLE COMPANION runs a Nordic UART Service (the same
well-known UUIDs a generic "BLE serial terminal" app already understands):

- Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (write) `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX (notify) `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

Commands/responses are newline-delimited plain text; multi-line payloads
(Notes, C LAB source) are base64-wrapped. See `Protocol.kt` for the exact
command set and `thecodeimtalkingabout.cpp`'s own `applyBleCommand()` for
the firmware side.

The device advertises as **"HijelHID KB"** by default (the BLE keyboard
library's default name, unchanged in firmware) - that's the name to enter
on the Connect screen.

## Building

Requires a JDK 17 and the Android SDK (API 34, build-tools 34.0.0).

```
# from this directory
gradle assembleDebug        # or ./gradlew if you generate a wrapper
```

`local.properties` (gitignored) must point `sdk.dir` at your SDK install, e.g.:

```
sdk.dir=C:\\Android\\sdk
```

## What's implemented

- Connect screen: scan by BLE device name, shows connection state and a
  live device status card (battery, uptime, Wi-Fi, free heap, current app).
- Remote screen: D-pad, Back, quick app launches, quick settings
  (theme/brightness/volume) - mirrors the existing Web Companion page.
- Notes screen: load/edit/save the device's Notes app content (up to 15 lines).
- C LAB screen: load/edit/save/run CardC source on the device.

## Not yet implemented

- Live screen preview / screenshotting (the firmware's display is
  write-only outside Sky Pilot/Kart Racer - see thecodeimtalkingabout.cpp's
  own "RENDERING MODEL" comment - so this needs a real shadow-framebuffer
  change on the firmware side first; deliberately deferred).
- mDNS/auto-discovery - you type the BLE device name once; there's no
  "nearby devices" list yet.
- This build has not been installed/run on a physical device or emulator
  from this environment (no phone or BLE-capable emulator available) -
  only `gradle assembleDebug` has been verified to succeed.

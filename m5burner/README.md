# CardputerXL

A single-firmware "OS" for the M5Stack Cardputer-ADV — dozens of built-in apps
in one flash: Notes, Calculator, a C interpreter, Wi-Fi tools, a 3D flight sim
(Sky Pilot) and a kart racer, a BLE remote-control companion (with a matching
Android app), a Bambu Lab printer monitor, an internet radio player, and more.

Hold `` ` `` at power-on to drop into a built-in Recovery Mode (reboot,
factory reset, or a hardware self-test) before anything else loads.

**Device:** M5Stack Cardputer-ADV (8MB flash)
**Source:** https://github.com/szuszjan/cardputer-xl

Flash the merged `.bin` from this tool, or build `CardputerXL.cpp` from
source with Arduino IDE / arduino-cli (`esp32:esp32:m5stack_cardputer`,
`PartitionScheme=huge_app`).

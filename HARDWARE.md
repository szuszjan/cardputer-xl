# Cardputer XL — hardware specification

The Cardputer XL is an M5Stack **Cardputer-ADV** with two additions:

- an external **ILI9341 320×240 SPI display**, wired to the EXT header;
- an analog **PSP thumbstick**, on the Grove port.

This file lists the wiring, the display settings and the joystick calibration needed to drive both.

## Base unit

| | |
|---|---|
| Board | M5Stack Cardputer-ADV (Stamp-S3A, ESP32-S3, 8 MB flash, **no PSRAM**) |
| Built-in screen | 1.14" ST7789, 240×135, on SPI3 (driven by M5Cardputer / M5GFX) |
| Keyboard | 56 keys through a TCA8418 on I2C (G8 SDA / G9 SCL) |
| Arduino FQBN | `esp32:esp32:m5stack_cardputer` (arduino-esp32 3.x) |

## EXT header (2.54 mm, 14 pins)

| Pin | Signal | Cardputer XL use |
|---|---|---|
| 1 | G3 | display **DIN / MOSI** |
| 2 | 5VIN | — |
| 3 | G4 | joystick **VCC** (driven HIGH = 3.3 V) |
| 4 | GND | display + joystick **GND** |
| 5 | G6 | display **CLK / SCK** |
| 6 | 5VOUT | display **VCC** |
| 7 | G40 | free (microSD SCK) |
| 8 | G8 | keyboard I2C SDA — do not use |
| 9 | G14 | free (microSD MOSI) |
| 10 | G9 | keyboard I2C SCL — do not use |
| 11 | G39 | display **BL** (backlight) |
| 12 | G13 | display **DC** |
| 13 | G5 | display **CS** |
| 14 | G15 | display **RST** |

The header has **no 3.3 V pin**. Anything that needs 3.3 V has to take it from a GPIO, as the joystick does on G4, or from its own regulator.

G39 is also the microSD card's MISO line. While G39 drives the backlight, the microSD slot can't be used at the same time.

## Grove port (HY2.0-4P)

| Wire | Signal | Cardputer XL use |
|---|---|---|
| Black | GND | — |
| Red | **5 V** | — (do **not** power the joystick from it) |
| Yellow | G2 | joystick axis |
| White | G1 | joystick axis |

---

## External display

### Wiring

| Display pin | Also labelled | Cardputer-ADV | EXT pin |
|---|---|---|---|
| VCC | | 5VOUT | 6 |
| GND | | GND | 4 |
| DIN | MOSI, SDI | G3 | 1 |
| CLK | SCK | G6 | 5 |
| CS | | G5 | 13 |
| DC | D/C, RS | G13 | 12 |
| RST | RESET | G15 | 14 |
| BL | LED | G39 | 11 |
| SDO | MISO | not connected | — |

### Specification

| | |
|---|---|
| Controller | **ILI9341** (an ST7789 module will not work with this init sequence) |
| Resolution | 320×240 landscape (240×320 native), RGB565 |
| Bus | SPI2 (the built-in screen keeps SPI3), mode 0, write-only (no MISO) |
| SPI clock | **40 MHz** write. 80 MHz garbles the image on this wiring. |
| DMA | yes (`SPI_DMA_CH_AUTO`) |
| Supply | 5 V into the module's own 3.3 V regulator. **Jumper J1 open.** J1 bypasses the regulator and must only be closed on 3.3 V. |
| Backlight | G39, LEDC PWM 5 kHz, 8-bit; 255 = fully on (steady 3.3 V), 0 = off |
| Colour order / inversion | RGB (`rgb_order = false`), not inverted |
| Rotation | Adafruit_ILI9341 rotation **3**. With LovyanGFX / M5GFX and the init list below the panel lands 180° off, so use `setRotation((3 + 2) & 3)` = 1. |

### Driver setup (M5GFX's bundled lgfx)

M5GFX ships no ILI9341 panel class, so the init sequence comes from LovyanGFX's `Panel_ILI9341` (FreeBSD licence, lovyan03):

```cpp
#include <M5Cardputer.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>

struct Panel_ILI9341_XL : public lgfx::Panel_LCD {
  Panel_ILI9341_XL() {
    _cfg.memory_width = _cfg.panel_width = 240;
    _cfg.memory_height = _cfg.panel_height = 320;
  }
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
      0xEF, 3, 0x03, 0x80, 0x02,
      0xCF, 3, 0x00, 0xC1, 0x30,
      0xED, 4, 0x64, 0x03, 0x12, 0x81,
      0xE8, 3, 0x85, 0x00, 0x78,
      0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
      0xF7, 1, 0x20,
      0xEA, 2, 0x00, 0x00,
      0xC0, 1, 0x23,
      0xC1, 1, 0x10,
      0xC5, 2, 0x3e, 0x28,
      0xC7, 1, 0x86,
      0xB1, 2, 0x00, 0x13,
      0xF2, 1, 0x00,
      CMD_GAMMASET, 1, 0x01,
      0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
      0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
      0xB6, 3, 0x08, 0xC2, 0x27,
      CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
      CMD_IDMOFF, 0,
      CMD_DISPON, 0 + CMD_INIT_DELAY, 100,
      0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class ExtPanel : public lgfx::LGFX_Device {
  Panel_ILI9341_XL _panel;
  lgfx::Bus_SPI _bus;
public:
  ExtPanel() {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST;  b.spi_mode = 0;
    b.freq_write = 40000000; b.freq_read = 16000000;
    b.spi_3wire = false;     b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = 6; b.pin_mosi = 3; b.pin_miso = -1; b.pin_dc = 13;
    _bus.config(b);
    _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = 5; p.pin_rst = 15; p.pin_busy = -1;
    p.readable = false; p.invert = false; p.rgb_order = false;
    p.dlen_16bit = false; p.bus_shared = false;
    _panel.config(p);
    setPanel(&_panel);
  }
};

ExtPanel tft;

void setup() {
  M5Cardputer.begin(M5.config(), true);
  tft.init();
  tft.setRotation(1);          // = Adafruit rotation 3
  ledcAttach(39, 5000, 8);     // backlight
  ledcWrite(39, 255);
}
```

With Adafruit_ILI9341 on hardware SPI, as the CardputerXL firmware uses it:

```cpp
Adafruit_ILI9341 tft(&SPI, 13 /*DC*/, 5 /*CS*/, 15 /*RST*/);

SPI.begin(6 /*SCK*/, -1 /*MISO*/, 3 /*MOSI*/, 5 /*CS*/);
tft.begin();
tft.setRotation(3);  // 1 = the same landscape, upside down
```

### Troubleshooting

| Symptom | Likely cause |
|---|---|
| Backlight on, screen **plain white or grey**, no image | Controller gets no data: CLK / DIN / CS / DC loose, or the panel's flex cable is cracked. Re-init only happens at boot, so power-cycle after re-wiring. |
| Backlight brightness **pulses with what's drawn** | Lost GND or VCC: the module is back-powered through the SPI lines' protection diodes. |
| Image garbled / noisy | SPI clock too high for the wire length: stay at 40 MHz. |
| Image upside down | Rotation off by 180°: see the rotation note above. |
| Wrong colours / mirrored | Not an ILI9341 (often ST7789) or wrong `rgb_order` / `invert`. |

---

## Joystick

### Hardware

| | |
|---|---|
| Part | PSP-style analog thumbstick: two potentiometers, 4 pins (VCC, GND, X, Y) |
| Axis inputs | G1 and G2 (Grove white / yellow), ADC1, so they still work while Wi-Fi is on |
| Supply | **3.3 V from G4** (EXT pin 3), set `OUTPUT` + `HIGH` at boot. A pot draws well under 1 mA. |
| Never | 5 V on VCC. At full deflection the wiper would put ~5 V on G1/G2, and the ESP32-S3 pins take 3.6 V at most. |
| Never | the BL pin as supply. It's PWM when the backlight is dimmed, and the readings would jump. |

Which pin is X and which is Y, and in which direction each one counts, doesn't matter: the calibration finds out. The stick can be mounted rotated by any multiple of 90°, but not at 45°.

The vibration motor that the original CardputerXL wiring had on G1 is gone: G1 now belongs to the joystick.

### Wiring

| Stick pin | Cardputer-ADV |
|---|---|
| VCC | G4 (EXT pin 3) |
| GND | GND (EXT pin 4 or Grove black) |
| X | G1 or G2 (Grove) |
| Y | the other one |

### Reading

```cpp
#define JOY_A   1   // Grove G1
#define JOY_B   2   // Grove G2
#define JOY_PWR 4   // EXT pin 3

pinMode(JOY_PWR, OUTPUT);
digitalWrite(JOY_PWR, HIGH);
analogReadResolution(12);                 // 0..4095
analogSetPinAttenuation(JOY_A, ADC_11db); // ~0..3.1 V full scale
analogSetPinAttenuation(JOY_B, ADC_11db);

int joyAvg(uint8_t pin) {                 // 4-sample average, read every 10 ms
  uint32_t sum = 0;
  for (int i = 0; i < 4; ++i) sum += analogRead(pin);
  return sum / 4;
}
```

The pots are rarely centred and their two halves are rarely equal. Each of the four directions therefore gets its own range (`+` = right / down on screen):

```cpp
// raw: ADC reading, c: centre, rPos/rNeg: range right|down / left|up, sign: +1/-1
float axis(int raw, int c, int rPos, int rNeg, int sign) {
  int d = (raw - c) * sign;
  float v = constrain(d / (float)(d >= 0 ? rPos : rNeg), -1.0f, 1.0f);
  float a = fabsf(v);
  if (a < 0.15f) return 0.0f;                     // dead zone: 15 %
  a = (a - 0.15f) / 0.85f;
  return (v < 0 ? -1.0f : 1.0f) * powf(a, 1.6f);  // curve: fine control near the centre
}
```

Mouse-style use: velocity = `axis × 600 px/s × speed`. Speed steps are 30 / 45 / 60 / 80 / **100** / 130 / 170 %.

### Calibration

Four steps. Steps 1–3 are sampled with ENTER: 16 readings of the 4-sample average. Step 4 records for 4 s.

| Step | Stick | Measured |
|---|---|---|
| 1 | left alone, centred | centre of both pins (`ca`, `cb`) |
| 2 | pushed fully **right** | `ra`, `rb` |
| 3 | pushed fully **up** | `ua`, `ub` |
| 4 | rolled around the whole **rim**, a few laps | min / max of both pins |

From those:

1. **X axis:** the pin that moved more in step 2 (`|ra − ca|` vs `|rb − cb|`). The other pin is Y.
2. **X sign:** +1 if X's raw value rose going right, otherwise −1.
3. **Y sign:** −1 if Y's raw value rose going up, otherwise +1. Screen Y grows downwards.
4. **Ranges:** distance from the centre to the min / max seen in step 4, mapped through the signs to right / left / down / up. Each is taken at **92 %** so that a full push surely reaches 1.0.
5. **Rejected** when step 2 or 3 moved less than 150 counts, or any of the four ranges is under 120 counts. Usually that means a wiring fault or a missing VCC.

Stored values (NVS, all `int16_t` unless noted):

| Value | Meaning |
|---|---|
| X pin, Y pin (`uint8_t`) | 1 or 2 |
| X sign, Y sign (`int8_t`) | +1 / −1 |
| centre X, centre Y | ADC counts, ~2048 for a centred pot |
| range right, left, down, up | ADC counts from the centre to full deflection |
| speed (`uint8_t`) | index into the speed steps |
| enabled (`bool`) | off until the first calibration, so floating inputs can't drift anything |

Recalibrate whenever the stick's supply changes, because the ranges scale with the supply voltage.

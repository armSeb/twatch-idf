# Hardware notes — T-Watch 2020 V1

[Français](../fr/hardware.md) · English

Reference: LilyGo's [V1 schematic](https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library/tree/master/Schematic).

## Pins

| Function | Pins |
|---|---|
| ST7789 display (SPI2) | MOSI 19, SCLK 18, CS 5, DC 27, backlight 12 (no reset, no TE line) |
| I2C 0 (sensors) | SDA 21, SCL 22 — AXP202 `0x35`, PCF8563 `0x51`, BMA423 `0x19` |
| I2C 1 (touch) | SDA 23, SCL 32 — FT6336 `0x38` |
| Interrupts | AXP202 35 (active low, input only), touch 38, RTC 37, BMA423 39 |
| Vibration motor | 4 (through a transistor) |
| Infrared LED | 13 |
| MAX98357 I2S amplifier | BCLK 26, LRCLK 25, DIN 33 |

The ESP32 module carries a 16 MB W25Q128 flash and an 8 MB IPS6404 PSRAM, both supplied by
the ESP32's own VDD_SDIO. There is no 32 kHz crystal.

## AXP202 power rails

| Rail | Feeds | Firmware |
|---|---|---|
| DCDC3 | ESP32 3.3 V | always on (never switched off) |
| LDO1 | RTC 3.3 V | always on (not switchable) |
| LDO2 | display (LCD_VDD) and backlight | 3.3 V, on |
| LDO3 | MAX98357 amplifier only | **off**; turned on during a sound with a soft start (see below) |
| LDO4 | VDD_1V8, test point only on V1 | 1.8 V, **kept on** (some module variants use 1.8 V flash) |
| DCDC2 | not connected on V1 (no inductor) | off |
| EXTEN | — | off |

Other AXP202 settings: shutdown voltage (VOFF) 3.2 V, charge current 300 mA (minimum of the
chip), charge LED off, coulomb counter on.

**The AXP202 keeps its registers as long as the battery is connected.** A previous firmware
that switched a rail off leaves it off for the next one: always set the rails explicitly at
boot.

## Pitfalls met along the way

- **ESP-IDF 5.5.3 Bluetooth controller**: encrypted link lost with 0x208 right after
  pairing. Use ESP-IDF 5.5.6.
- **NimBLE host-based privacy**: reconnection fails after the iPhone rotates its private
  address (~15 min, SMP timeout). Address resolution is left to the controller.
- **`BT_NIMBLE_GATT_CLIENT` depends on `BT_NIMBLE_ROLE_CENTRAL`** in ESP-IDF 5.5: disabling
  the central role silently removes `ble_gattc_*`, hence ANCS.
- **iOS sometimes ignores the Security Request**: the watch sends it 500 ms after connecting
  and drops the link if encryption has not happened 8 s later, then advertises again after
  3 s.
- **Advertising refused after a failed connection** (`ENOMEM`, the connection slot is not yet
  freed): advertising is retried until it works, otherwise the watch stayed invisible until
  Bluetooth was restarted.
- **Connection parameter collisions** (0x22A) when the watch asks for economical parameters
  while iOS changes them: retried with back-off until accepted.
- **Speaker amplifier inrush**: switching LDO3 from 0 to 3.3 V at once (amplifier input
  capacitors) can pull the system below the AXP cut-off with a weak battery, and the watch
  powers off. LDO3 starts at 1.3 V and ramps up in 0.1 V steps. Beeps are kept moderate to
  limit current peaks.
- **I2S mono on the ESP32 fills only one slot**: the amplifier may listen to the other.
  Sound is sent as stereo.
- **Cancelling an OTA upload**: `httpd_stop()` waits for the upload handler, which keeps
  receiving while the phone sends → frozen watch. Restart directly instead.
- **Full image at 0x0 erases NVS** (pairings). For USB updates, write the app at `0x20000`
  with `ota_data_initial.bin`.
- **Upside-down panel** on some batches: `board.h` → `BOARD_PANEL_ALT=1`.
- **ST7789 TE line not wired**: no tearing sync possible, hence QIO 80 MHz flash, a double
  60-line DMA buffer and short animations.

## Possible hardware improvements (not required)

- A 32.768 kHz oscillator on 32K_XN (GPIO 33 — shared with the amplifier, needs care) would
  allow light sleep with a BLE connection (`BTDM_CTRL_LPCLK_SEL_EXT_32K_XTAL`).
- The battery (generic 380 mAh "YX-W9A" type cell) ages fast when stored empty; the
  *Battery* screen shows the learned capacity to decide when to replace it.

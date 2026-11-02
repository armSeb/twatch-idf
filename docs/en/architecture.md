# Architecture, power and battery gauge

[Français](../fr/architecture.md) · English

## Tasks and ownership

```
            ┌───────────── NimBLE host task (core 0) ─────────────┐
            │ ble.c   GAP, passkey security, battery service,       │
 iPhone ◄──►│         connection parameters, advertising, diag log  │
            │ ancs.c  ANCS + CTS + AMS discovery, command queue,    │
            │         Data Source reassembly (ancs_parse.c)         │
            └──────────┬───────────────────────────┬────────────────┘
                       │ g_notif_queue (copies)    │ app_post(event)
            ┌──────────▼───────────────────────────▼────────────────┐
 AXP IRQ ──►│ app task (core 1): sole owner of LVGL                 │
 touch INT  │ main.c   event loop / lv_timer_handler / screen sleep │
 esp_timers │ ui.c, face_*.c, app_*.c   notif_store.c               │
            │ display.c (ST7789 + FT6336)  power.c, gauge.c, sound.c│
            └───────────────────────────────────────────────────────┘
```

Stability rules:

- **One owner per state.** LVGL and the notification store belong to the app task; ANCS
  state belongs to the NimBLE host task. They talk through queues and events, never through
  a shared LVGL mutex.
- **One ATT procedure in flight** on the ANCS side, with a generation number checked in every
  callback (late answers after a reconnection are ignored).
- **Data Source reassembled byte by byte**, tested on PC with random fragmentation.
- **iOS replay** filtered with `EventFlagPreExisting` plus content deduplication (FNV-1a,
  5-minute TTL); only the newest 32 notifications are kept.
- **iOS text sanitized** (truncated UTF-8, emoji, ZWJ sequences…) before it reaches LVGL.
- **Level-triggered AXP interrupt**: no interrupt is lost during light sleep.
- **Events from anywhere, UI in one place**: timers, BLE and interrupts only post events;
  `app_post_event()` lets an app react while its screen is closed.

No `esp_lvgl_port` (its tick timer and task wake the CPU constantly), no AXP/BMA/FT
libraries (a few registers are enough), native `esp_lcd`, ESP-IDF 5 `i2c_master`.

## User interface

- **Navigation by origin**: every screen remembers where it was opened from; swipe → or the
  « ‹ » header always returns there.
- **Lazy screens**: settings, now playing, launcher, quick controls and the hosted app are
  built on demand and destroyed when the screen turns off, so RAM only holds what is shown.
  Permanent screens: watch face, notification list, detail, call, battery, pairing, about.
- **Apps** (`app.h`): see [apps-and-i18n.md](apps-and-i18n.md). An app that reports itself
  *busy* (running stopwatch, counting timer, TV off sending) keeps the screen on its view
  across sleep and wake.
- **Language change** rebuilds every screen in place, without a restart.
- LVGL objects are allocated from the heap; allocations above 4 KB go to PSRAM, the internal
  RAM is kept for stacks, DMA buffers and the radio.

## Power

On a "classic" ESP32, **light sleep is impossible while the BLE controller is active**
without an external 32 kHz crystal (the T-Watch 2020 has none): the controller holds an
`ESP_PM_NO_LIGHT_SLEEP` lock, whatever the framework. Everything else is tuned instead:

| Lever | Setting |
|---|---|
| Frequency scaling | CPU at **10 MHz** at rest (the BT controller releases its frequency lock during modem sleep); **240 MHz** only during interaction, back to minimum 1.5 s after the last touch even with the screen on; full speed during a Wi-Fi update |
| BLE link | modem sleep; after ANCS is ready, economical parameters are requested (150–300 ms interval, latency 4, 6 s timeout, within Apple's rules) and **re-requested until accepted**, with growing back-off (iOS often collides with its own updates, error 0x22A) |
| Advertising | 160 ms for 30 s after a disconnection, then ~1 s; retried every second if the stack refuses it |
| Tasks | no periodic timer with the screen off; the app task sleeps until an event (60 s max); LVGL tick read on demand |
| Display | ST7789 in sleep mode, backlight off, touch controller in monitor mode with a double-tap detector polled only after a touch interrupt |
| AXP202 | ADCs limited to battery voltage/current, charge LED off; unused rails off; the speaker rail is powered only while a sound plays |
| Peripherals | BMA423 suspended, PCF8563 CLKOUT off, unused GPIOs driven to a defined level |
| Airplane mode | BT controller stopped → **automatic light sleep** |

Typical result: about **7.7 mA in standby** with the iPhone connected and notifications on.

## Adaptive battery gauge

`gauge.c` (pure core tested on PC) counts charge with the AXP202 coulomb counters (0.364 mAh
per count). These counters are monotonic and never reset, so they survive restarts and
power cuts. The count is re-anchored on three events:

| Anchor | Condition | Value |
|---|---|---|
| FULL | on USB, no longer charging, > 4.10 V | 100 % |
| LOW | < 3.45 V at low current, seen twice | 4 % |
| EMPTY | power-on reset after a last saved voltage < 3.55 V | 0 % |

Each interval between two anchors yields a capacity measurement. The learned capacity
converges with weights 1, ½, ⅓ then ¼ (the first measurement replaces the 380 mAh nominal);
outliers beyond ±50 % are rejected once two measurements exist. Percentage and remaining
time therefore follow the real battery, even a worn one. While charging the display caps at
99 %; state is saved to NVS every 10 minutes and at each anchor.

The capacity learned is the capacity *usable by the watch*: an aged cell with a high internal
resistance hits the cut-off voltage under load peaks earlier than on a bench test.

## Wi-Fi update (OTA)

- The watch stops Bluetooth, starts a WPA2 access point (`T-Watch-OTA`, password drawn once
  and stored in NVS), a small DNS server answering 192.168.4.1 to everything, and an HTTP
  server. Apple and Android connectivity probes get "success" answers so phones stay on the
  network.
- Upload is streamed in 4 KB chunks into the inactive slot. The first chunk is checked (ESP32
  image magic and application descriptor with project name `twatch`), the full image is
  verified (SHA-256) before activation.
- Rollback: a new image boots "pending verification" and is marked valid after 30 s of
  operation; a crash before that returns to the previous slot.
- Cancelling restarts the watch (clean return to Bluetooth). During an upload the restart is
  immediate: stopping the HTTP server would wait for the handler, which keeps receiving.
- *Go back to x.y.z* verifies the other slot completely before booting it.

## Source files

| File | Role |
|---|---|
| `main/main.c` | `app_main`, app task, event loop, screen sleep |
| `main/ble.c` | NimBLE: GAP, security, advertising, connection parameters, battery service, airplane mode, diagnostic log |
| `main/ancs.c` / `ancs_parse.c` | ANCS + CTS + AMS client (`media.h`); Data Source reassembly |
| `main/notif_store.c` | storage, sort by date, dedup, UTF-8 cleanup |
| `main/ui.c`, `ui_kit.h` | screens, navigation, shared UI building blocks |
| `main/face_classic.c`, `face_pub.c` | analog watch faces (drawn in code) |
| `main/app.h`, `apps.c`, `app_*.c` | app framework, registry, bundled apps |
| `main/display.c` | ST7789 (`esp_lcd`, SPI DMA), FT6336, LVGL glue, double tap, frequency scaling |
| `main/power.c`, `axp202.c`, `gauge.c` | PMIC, rails, battery, PM locks, backlight, vibration, adaptive gauge |
| `main/sound.c` | beeps on the I2S amplifier |
| `main/wclock.c` | PCF8563 (UTC), time zone, system clock resync |
| `main/settings.c` | settings in NVS |
| `main/i18n.c` | French → English table |
| `main/ota.c` | Wi-Fi update: access point, web page, OTA write, validation, going back |
| `components/tvbgone/` | infrared power codes and RMT transmitter (CC BY-SA 4.0) |

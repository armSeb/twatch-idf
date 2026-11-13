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
- **Apps** (`app.h`): see [apps-and-i18n.md](apps-and-i18n.md). The open app stays built
  when the screen turns off (it gets `on_hide()`, then `on_show()` on wake) and the watch
  wakes back to it.
- **Notification banner**: outside the watch face, a new notification slides in on the top
  layer instead of switching screens; tapping it opens the detail, which returns to the exact
  screen you were on. Incoming calls still take the full screen and return there afterwards.
- **Wi-Fi analyzer** (`app_wifi.c`): the Wi-Fi driver only exists while the app is visible
  with the screen on (`esp_wifi_init` in `on_show`, `esp_wifi_deinit` in `on_hide`); each
  fetch starts the radio, runs an active scan (~1.2 s) and stops it again for 4 s. BLE stays
  connected (software coexistence).
- **SDR** (`app_sdr.c`, `sdr_rx.c`, `sdr_mode.c`): the ESP32 Wi-Fi MAC can dump the raw
  receive ADC samples (I/Q, 10 bits, 40 MS/s here) into two fixed SRAM banks
  (0x3FFE8000–0x3FFF8000). Those 64 KB are reserved at link time and never join the general
  heap: they form a private heap (`multi_heap`, "rfbank") that the Radio app uses for its
  decoder state and work buffers (fast internal RAM instead of PSRAM). Opening the SDR app
  borrows the radio without restarting: it checks that Wi-Fi is free and the private heap is
  empty, stops Bluetooth, shrinks the display render buffers (2 × 28.8 → 2 × 9.6 KB) so the
  Wi-Fi driver gets its internal RAM, and abandons the private heap to the MAC; leaving the app recreates
  the private heap and restarts Bluetooth. A task on core 0 tunes standard channels (1/4/7/10/13 for the band view, one
  channel for the zoom), captures 4096 samples, removes DC, runs Hann-windowed FFTs
  (16 × 256 or 8 × 512 points, averaged) and builds 240-column rows in dBFS; the UI draws them
  into two RGB565 canvases in PSRAM with an auto-tracking noise floor. With the screen off the
  capture stops but the radio stays borrowed until the app is left.
- **WebRX** (`app_webrx.c`, `webrx.c`, `webrx_cfg.c`): OpenWebRX / OpenWebRX+ client (user
  guide: [webrx.md](webrx.md)). The Wi-Fi networks (up to 3) and the servers (up to 4, name
  and URL; listening state kept per server) are entered on a page served by the watch
  (`/webrx`, same access point and web server as the update; the session ends with a restart
  that reopens the app). `webrx.c` runs a task on core 0: Wi-Fi station (known networks in
  order of preference, modem sleep), `wss://` through `esp_transport` (TLS with the ESP-IDF
  root bundle, mbedTLS buffers in PSRAM; `user:password@` in the URL becomes HTTP Basic auth),
  the OpenWebRX handshake, `connectionproperties` (12 kHz audio for every mode),
  `selectprofile` and `dspcontrol`. The server's `modes` list (custom modes included) gives
  the listenable modes and their bandpass; digital modes fall back to their underlying mode.
  Audio frames (IMA-ADPCM with sync words) are decoded into a 2.7 s ring in PSRAM; a second
  task on core 1 keeps ~0.4 s ahead and feeds the I2S amplifier, skipping or doubling one
  sample per 20 ms block to follow the server's clock; a tone filter (high-pass, presence bump,
  2nd/4th-order low-pass, 5 steps) suits the small speaker. Text frames are built and sent in
  a single write: the OpenWebRX server drops a client whose frame header and payload arrive
  in separate TCP segments, which is what `esp_transport_ws` does. FFT frames become the mini waterfall
  only while the screen is on. CPU is capped at 160 MHz (80 MHz screen off) while the radio
  runs, and Bluetooth is paused only if the Wi-Fi driver cannot get its internal RAM.
  The Wi-Fi station part (scan, known networks, reasons, BLE fallback) lives in `wifi_sta.c`,
  the 5-step tone filter in `audio_fx.c`; both are shared with Radio.
- **Radio** (`app_radio.c`, `radio.c`; stations in `webrx_cfg.c`, user guide:
  [radio.md](radio.md)): web radio player. Up to 12 stations (name, URL) are entered on the
  same `/webrx` page. Task `rad` (core 0) connects Wi-Fi, opens the stream with
  `esp_http_client` (TLS with the root bundle, redirects followed by hand, `Icy-MetaData: 1`),
  follows `.m3u`/`.pls` playlists, picks the codec from `Content-Type` or by sniffing the first
  bytes, strips ICY metadata blocks (`StreamTitle`, Latin-1 converted to UTF-8) and fills a
  128 KB compressed ring in PSRAM; playback starts after ≈2 s of audio (from `icy-br`). Task
  `radplay` (core 1) decodes with the Helix fixed-point decoders (`components/helix_mp3`,
  `components/helix_aac`: MP3, AAC-LC, HE-AAC/SBR), downmixes to mono, applies the tone filter
  and the volume, and feeds the I2S amplifier through `sound_stream_*`. It measures the share
  of time spent decoding: above 60 % it takes a `CPU_FREQ_MAX` lock (160 MHz), below 25 % it
  releases it, so the screen-off cap of 80 MHz holds whenever the stream allows. Every 2 s,
  with the screen off and RSSI above −65 dBm, the Wi-Fi TX power is lowered to 11 dBm
  (19.5 dBm otherwise). Failures retry after 5, 10, 20, 30 then 60 s; a dropped stream
  resumes after 2 s. Tested on the host with the real decoders and a fake HTTP server
  (redirect, `.pls`, ICY titles).
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

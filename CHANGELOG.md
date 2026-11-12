# Changelog

All notable changes. Versions follow `PROJECT_VER` in `CMakeLists.txt`.

## 0.21.6 — 2026-11-12

- WebRX: the server closed the connection immediately (0 byte received, TLS "closed by
  FIN"). `esp_transport_ws` writes a frame's header and payload separately (two TLS records /
  TCP segments); the OpenWebRX server reads its non-blocking socket in exact-size pieces,
  loses the frame when the payload is not there yet, and drops the client. Text frames are
  now built by the watch (header, mask key and masked payload) and sent in a single write.

## 0.21.5 — 2026-11-12

- WebRX diagnostics: the BLE log keeps 47 characters per line, so the "cut" line was
  truncated. It is now split into short lines: cause; frames, bytes, duration and RSSI; last
  message type received; and on a read error the transport/errno/TLS codes.
- WebRX: a refusal from the server ("backoff": too many listeners, address banned by the
  OpenWebRX+ robot protection…) is shown with the server's own message, logged, and no longer
  triggers automatic reconnections (tap to retry).

## 0.21.4 — 2026-11-12

- WebRX, connection still cut ("read error"): generous timeouts for slow servers (20 s to
  connect, 8 s between two pieces of a frame, 30 s of silence before giving up, 10 s to
  send), no Wi-Fi modem sleep while listening (the server closes a client that cannot take
  its data for 10 s), larger Wi-Fi receive buffers and TCP window, A-MPDU aggregation on
  receive.
- WebRX reconnects are spaced out (2, 10, 20, 40 s, then wait for a tap): OpenWebRX+ bans for
  12 hours an address that reconnects too often. The countdown is shown. The BLE log line
  of a cut also gives the data received and the Wi-Fi RSSI.

## 0.21.3 — 2026-11-12

- WebRX: "connection lost" right after connecting. The server sends in 1 KB pieces and the
  Wi-Fi modem sleep delays packets; when a WebSocket frame header arrived in two parts more
  than 200 ms apart, `esp_transport_ws` gave up with a fatal error. Reads now wait up to
  2.5 s. The reason of a cut (frame cut, read error, closed by the server, silence, Wi-Fi
  lost) is shown under "Connection lost" and logged with the number of frames received.

## 0.21.2 — 2026-11-12

- **Internal RAM given back** (the OTA update failed again with `ESP_ERR_HTTPD_TASK`, 7 KB
  largest block): LVGL objects now live in PSRAM (custom allocator `lv_mem_psram.c`), the
  big static tables (notifications, ANCS app cache, BLE log copy, update page and buffer,
  About text, waterfall rows, Wi-Fi list rows…) are placed in PSRAM (`EXT_RAM_BSS_ATTR`),
  and so are the `.bss` of the Bluetooth host, Wi-Fi and lwIP as ESP-IDF allows
  (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`). Static internal RAM: 66 KB → 19 KB.
- WebRX: fixed a crash on connection — `esp_transport_ws` masks the payload in place, and the
  handshake was a string constant in flash; it now always sends a copy.
- WebRX: the screen stays on while listening (the button still turns it off; listening goes
  on). Wi-Fi failures show the reason code; Wi-Fi and server failures go to the BLE log
  with the free internal memory.
- About: "Restart the watch" button (with confirmation).

## 0.21.1 — 2026-11-12

- WebRX: "Set up" failed with "web server not started" (the access point was up). The app's
  view and the Wi-Fi station used for the network list kept internal RAM the web server
  needed: the view is now freed before the access point starts and the station is turned off
  right after the scan. A second failure is also written to the BLE log with the free memory.

## 0.21.0 — 2026-11-12

- New **WebRX** app: listens to an OpenWebRX / OpenWebRX+ server over Wi-Fi (`https://`
  and `http://`, optional `user:password@` for HTTP Basic auth) and plays the audio on the
  watch speaker. Big frequency display (tap a digit to choose the − / + step), bookmarks of
  the band, modes taken from the server (custom ones included, with their bandpass), profile
  list, volume and squelch, S-meter, mini waterfall (tap to tune, ×1/×4/×16 zoom). Listening
  continues with the screen off; leaving the app stops it. Automatic reconnection; the last
  profile, frequency, mode, volume and squelch are remembered.
- WebRX settings (up to three Wi-Fi networks in order of preference, server address) are
  entered from the phone on a page served by the watch, through the same access point as the
  update (visible networks are suggested). Saving restarts the watch straight into the app.
- Wi-Fi analyzer and WebRX cap the CPU at 160 MHz while the radio runs: with a 240 MHz maximum
  the ESP32 kept the CPU at 240 MHz whenever Wi-Fi was on, even with the screen off; it now
  drops to 80 MHz.
- Speaker: continuous audio streaming in addition to beeps.

## 0.20.1 — 2026-11-12

- SDR: a swipe → on the waterfall in the band view no longer leaves the app (leaving
  restarts the watch, which looked like a crash while handling the waterfall). Leave with
  « ‹ » or the button.

## 0.20.0 — 2026-11-12

- New **SDR** app: live 2.4 GHz spectrum and waterfall computed on the watch from the raw
  I/Q samples of the ESP32 radio (MAC dump mode, 40 MS/s). Whole band 2401–2483 MHz (five
  tunings per line) or one channel ±10 MHz (tap **+**, long-press on a frequency, swipe ← →
  to change channel); marker with frequency and level (tap), decaying peak hold, gain AGC or
  four manual levels with a clipping warning, auto-tracking noise floor, pause (tap the
  title). The app runs in a dedicated "SDR mode": opening it restarts the watch without
  Bluetooth, leaving it restarts in normal mode; the radio stops with the screen.
- The 64 KB of SRAM used by the capture are reserved at link time but handed back to the heap
  in normal mode.

## 0.19.1 — 2026-11-12

- Wi-Fi analyzer: if the Wi-Fi driver cannot get enough internal RAM next to Bluetooth,
  Bluetooth is paused while the app is in use (kept off while the watch sleeps on the app,
  restarted as soon as you leave it). A toast and the BLE log say so.
- Long toasts now wrap instead of overflowing the screen.

## 0.19.0 — 2026-11-12

- New **Wi-Fi analyzer** app: networks sorted by signal (RSSI, channel, security, width),
  2.4 GHz channel occupancy chart with the least busy of 1/6/11, and a detail sheet per access
  point (BSSID, universal/random address, channel and frequency, 20/40 MHz, security and
  ciphers, 802.11 modes, WPS, advertised country, signal history). Pause/resume chip. The Wi-Fi
  driver only runs while the app is on screen and the radio is off between fetches; it is
  fully de-initialised when leaving the app or when the screen turns off.
- **Notifications no longer pull you out of an app or a menu**: a banner slides in on top
  (tap to open the detail, which then returns to the same screen; swipe up to dismiss; 5 s).
  On the watch face, notifications still open full screen. Incoming calls return to the app
  afterwards.
- **Any open app is kept across sleep**: the watch wakes back to it (not only a running
  stopwatch or timer); apps get `on_hide()`/`on_show()` around sleep.

## 0.18.1 — 2026-11-09

- **Wi-Fi update could fail with "web server did not start"** (internal RAM too tight,
  made worse by the heap poisoning of the diagnostic builds): heap poisoning removed (the
  integrity checks and the crash breadcrumbs stay), fewer server sockets, and a second,
  leaner attempt if the server still cannot start; the cause and free memory go to the BLE
  log in *About*.

## 0.18.0 — 2026-11-08

- New **Carrier** app (RF generator): the ESP32 audio PLL (APLL) output straight on
  GPIO0 or GPIO1 (CLK_OUT), square wave from 5.31 to 125 MHz, set digit by digit (100 Hz
  steps), actual programmed frequency shown, automatic stop after 10 minutes. No antenna on
  those pins: a near-field injector / test signal, with strong odd harmonics.

## 0.17.0 — 2026-11-02

- **Battery saver removed** (back to the permanent link of 0.15): in practice iOS did not
  come back to a watch that had left on its own, so fetches rarely connected and
  notifications did not alert. Airplane mode remains for long periods without the iPhone.
- Keeps the 0.16.x crash fixes and diagnostics: screen animations finished before a
  screen is freed; heap poisoning and integrity checks; crash summary with the callers and
  the last UI actions in *About*.

## 0.16.3 — 2026-11-02

- Diagnostic build for a remaining crash (corrupted LVGL object tree): heap poisoning
  enabled and the internal heap checked after every event, every 2 s with the screen on
  and when it turns off, so a corruption stops the watch close to its cause. The crash
  summary in *About* now ends with the last UI actions before the crash ("Avant : …").
- Battery saver: the stack starts with the CPU at 80 MHz instead of full speed.

## 0.16.2 — 2026-11-02

- **Fixed random crashes** (heap corruption reported later in a `realloc`): a screen could
  be freed while its slide animation was still running (button pressed or screen turned
  off right after a swipe, a notification arriving mid-animation…); LVGL then kept
  animating freed memory. Any running screen animation is now finished before a screen is
  freed or another one is loaded. Found with a randomized UI test under AddressSanitizer.
- **Battery saver**: the iPhone could not re-establish the link after a fetch started with
  the screen off (immediate connection failures in a loop). The Bluetooth stack now starts
  with the CPU at 80 MHz (PLL on) and keeps it until the link is encrypted (8 s at most); after 6 failed
  attempts in a fetch, the stack is restarted once.
- Crash summary in *About* shows the callers instead of the abort frames; the BLE log shows
  the CPU frequency when the stack starts.

## 0.16.1 — 2026-10-31

- Last crash summary (task, reason, backtrace) in *Settings › About*; BLE log kept across a
  crash restart.

## 0.16.0 — 2026-10-31

- **Battery saver** (*Settings › Bluetooth*): the radio stays off and the watch fetches
  notifications every 2, 5 or 15 minutes (≈1 mA between fetches instead of ≈7.7 mA
  connected). Notifications received in between alert once per fetch; those cleared on the
  iPhone meanwhile disappear from the watch. Now playing, the notification list, a
  notification and pairing connect on demand while they are open. Incoming calls only
  ring if they happen during a fetch.
- Watch faces show the saver state (*LATER* sign on the Pub face).

## 0.15.1 — 2026-10-31

- First published release: same firmware as 0.15.0, CI allowed to publish the release
  (`permissions: contents: write`).

## 0.15.0 — 2026-10-30

- Wi-Fi update page in **French and English** (browser language, switch button); the server
  returns error codes translated by the page.
- Update access point renamed **T-Watch-OTA** (was *T-Watch-MAJ*): join it once with the
  password shown on the watch.
- The infrared app is now called **TV off**; `TW_NO_TVOFF=1` builds without it and without
  its CC BY-SA 4.0 code database.
- Bilingual documentation (README, building, architecture, hardware, apps and i18n),
  third-party notices, contributing guide.

## 0.14.0 — 2026-10-29

- **Go back to the previous version** from *Settings › Update* (full image check, then boot).
- PSRAM enabled again (it was not the cause of the 0x208 disconnections).

## 0.13.x — 2026-10-29

- PSRAM (8 MB, 4 MB mapped) used for large allocations; IRAM freed for it.
- BLE: advertising retried when the stack refuses it (the watch could stay invisible after
  a failed connection); economical connection parameters re-requested until iOS accepts them.
- Speaker: soft start of the amplifier rail (an abrupt start could power the watch off),
  quieter beeps.

## 0.12.x — 2026-10-29

- *TV off* app (TV-B-Gone codes over the IR LED).
- Timer **beeps** on the speaker; speaker test in *Alerts*; stereo I2S.
- Wi-Fi update: cancel during upload no longer freezes the watch; full CPU speed during the
  update; large password display (the QR code was dropped).
- Power: amplifier rail (LDO3) off except during sounds, unconnected DCDC2 off.

## 0.11.x — 2026-10-29

- **French / English** interface, switched in place.
- A running stopwatch or timer keeps the screen on its app across sleep and wake.
- Bluetooth appearance set to "watch".

## 0.10.0 — 2026-10-29

- **App framework**, launcher, *Stopwatch*, *Timer*, *Torch*.
- **Quick controls** and **Do not disturb**.
- New gestures: ↑ notifications, ↓ quick controls, ← apps, long press = face picker, → back
  to the origin screen.

## 0.9.x — 2026-10-28/29

- **Over-the-air update** over Wi-Fi with rollback; Safari compatibility.
- CPU at 10 MHz at rest; discharge history; **adaptive battery gauge**.
- BLE: Security Request sequencing and encryption watchdog; on-watch diagnostic log.
- *Pub* full-screen face, *Now playing* (AMS), double-tap wake, settings by category.

## 0.1 — 2026-10-27

- First version: ESP-IDF 5.5.6 + LVGL 9 + NimBLE, ANCS notifications, CTS time, *Retro* and
  digital faces, energy screen, silent mode.

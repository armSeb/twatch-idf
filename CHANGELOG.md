# Changelog

All notable changes. Versions follow `PROJECT_VER` in `CMakeLists.txt`.

## 0.32.1 — 2026-11-22

- IR lamp: a long press on a known key now shows its NEC address and code (besides search
  again / forget).

## 0.32.0 — 2026-11-22

- New **IR lamp** app: an infrared remote for solar lamps with an 8-key remote (ON, OFF, 3H,
  5H, 8H, 25 %, 50 %, 100 %), laid out like the original. The watch has no IR receiver, so it
  finds the NEC codes itself: it sweeps the 256 codes of the most common addresses (about
  30 s each), you tap "IT REACTED!" when the lamp responds, then a halving search over the
  codes of the last 2.5 s narrows it to one in 4–5 tries. Once one key is known, the others
  only sweep that address, starting next to the known code. Codes kept in NVS; long press a
  key to search again or forget it.
- `ir_nec.c`: NEC transmitter on the IR LED (RMT, 38 kHz carrier), the channel and its
  frequency lock held only while sending.

## 0.31.7 — 2026-11-21

- Soar has its own, gentler background music: a 16-bar loop with a music-box arpeggio, pad,
  soft triangle bass, muted kick and hi-hat, and a slow, sparse melody in the second half;
  about 10 dB below the demo so the sound effects stand out. The demo's tune is unchanged.

## 0.31.6 — 2026-11-20

- Soar: the game clock was capped at 0.1 s per frame, so below 10 frames/s the plane flew in
  slow motion (about a quarter of its speed at 8 frames/s). It now follows real time (cap
  0.2 s), and the game is rebalanced for it: starting speed 30 (was 42), at most 60, boost
  ×1.6; rings larger (radius 13) and closer together (95–130 ahead, gentler turns, lower);
  30 s on the clock, +6 s per ring (then +5, +4).
- Soar: the next ring shows through hills faintly, so it no longer pops up at the last moment.

## 0.31.5 — 2026-11-20

- Soar is never interrupted: notifications received while it is shown go to the list (and the
  watch-face counter) without a banner or vibration. Incoming calls still come through.
- Apps can ask for this with the new optional `no_interrupt()` callback in `app_t`.

## 0.31.4 — 2026-11-20

- Soar draws **straight to the display**, bypassing LVGL: the picture is built in two 20-line
  strips in internal DMA RAM, one being filled while the other is sent to the screen. No more
  240×240 canvas in PSRAM written then read back every frame (PSRAM writes are slow on this
  ESP32: each one is followed by a memory barrier). The interface is drawn strip by strip
  (surfaces with a line offset). 0.31.3 measured 9 frames/s.
- New `display_direct()` / `display_direct_end()` for games; `g_rect()` and banded surfaces
  in `demo_gfx`.

## 0.31.3 — 2026-11-20

- Landscape rendering on **both cores**: the left half of the picture is computed by a task
  on core 0 (nearly idle while playing), the right half on core 1, in parallel. Fewer PSRAM
  reads in the foreground (colour smoothing only very close up) and slightly sparser samples
  in the distance, hidden by the haze. Soar measured 8 frames/s in 0.31.2; the demo's flight
  benefits too.

## 0.31.2 — 2026-11-20

- Fix: **Battery** and **Settings** had vanished from the app launcher (and with them the
  update page). The launcher held 16 entries; with Demo and Soar there are 18, and the last two
  were silently dropped. It now holds 32, and those two always keep their place.

## 0.31.1 — 2026-11-20

- Soar: about 4 frames/s on the watch, now several times faster. The terrain frame buffer did
  not fit in internal RAM and fell back to PSRAM, written column by column (one cache miss per
  pixel), plus a PSRAM depth buffer. Now: 160×120 render in internal RAM (widened ×1.5), each
  column built in a local buffer then copied, height and colour merged into one 32-bit map
  word (one PSRAM read per sample), and a 2.5 KB per-column occlusion table instead of the
  depth buffer. The demo landscape benefits too.
- Soar: **pause** by tapping the score corner (top left), with a **Quit** button and the frame
  rate. Leaving during a game was nearly impossible: at 4 frames/s the swipe was not
  recognised and turned into a boost tap.

## 0.31.0 — 2026-11-20

- **Demo** rewritten and doubled in length (about 2 min, 64 bars): particle title, textured
  tunnel, rotozoom over plasma, shaded torus, a 700-dot swarm morphing sphere → torus → cube →
  galaxy over copper bars, metaballs with credits, burning title, the landscape flight with
  sine-scroller, a 3D Celtic knot, "THANKS!" and fade out; tap at the end to watch again. The
  wrist-flying part moved to its own game.
- New **Soar** game: fly through gold rings against the clock by tilting your wrist. +5 s per
  ring (less later on), combos for rings chained within 6 s, speed rising, boost on tap,
  missed rings cost 3 s, crash on the terrain, best score kept. Rings hide behind hills (depth
  buffer), arrow to the next ring, sun lens flare, speed lines, sparks, shake on crash.
- **Landscape** much more detailed and new at every launch (512×512, built in the background):
  winding rivers, lakes and beaches, patchwork meadows with hedgerows, ploughed and hay fields,
  forests, white farmhouses, moorland, rock and snow; low-sun lighting with cast shadows,
  perspective clouds, sky reflections on water, smoothed foreground.
- Music: 64-bar arrangement (verse, chorus, bridge with pad, finale an octave up, fade out),
  echo on the melody, a game loop and sound effects (ring, boost, countdown, crash).
- Fix: unmuting the demo did not bring the sound back. The audio stream now stays open and
  only sends silence while muted.

## 0.30.0 — 2026-11-20

- New **Demo** app, *Tír na nÓg*: a demoscene-style intro computed entirely on the watch, about
  1 min 10. Accelerating starfield, ~900 particles swirling into the title, endless textured
  tunnel, shaded 3D torus pulsing on the kick drum, then a flight over sunset hills in "voxel
  space" (heightmap generated at start-up, autopilot following the valleys, sine-scroller
  greetings). Then "Your turn!": you fly by tilting your wrist (turn and bank, climb and
  dive). Tap to mute; the screen stays on while it runs.
- 4-voice chiptune synthesised live at 16 kHz (melody, arpeggio, bass, kick/snare/hi-hat from
  noise), which is also the frame clock so effects land on the beat.
- Motion: raw accelerometer access for apps (`motion_accel_begin/read/end`), waking the sensor
  only if steps and raise to wake are both off.
- Fonts: rocket icon (U+F135).

## 0.29.3 — 2026-11-17

- Púca: **bedtime**. From sunset (or 8 pm) the moon button puts it to bed early, light off; it
  sleeps (and recovers) until 7 am. Before sunset: "Too early for bed". Asleep, the button
  switches the light; turning it back on before 10 pm wakes it up. It still falls asleep by
  itself at 10 pm. The saved state gains one field; a 0.29.0–0.29.2 pet is kept as is.

## 0.29.2 — 2026-11-17

- Púca: the sky follows the real sunrise and sunset of the day (computed for central France,
  local time with summer/winter offset) instead of fixed hours: dark by about 17:30 in
  November, daylight until about 22:00 in June. Dawn and dusk colours sit around those times.
  The pet still sleeps from 22:00 to 7:00.

## 0.29.1 — 2026-11-17

- Púca: energy rebalanced — the game tired it far too fast (12 % per game, a dozen games a day
  was impossible). Now 6 % per game, about 3.6 % per waking hour instead of 4.8 %, full energy
  at hatching (was 90 %), too tired to play below 10 % (was 15 %). A night with the light off
  still refills it.

## 0.29.0 — 2026-11-17

- New **Púca** app: a pixel-art virtual pet from Irish folklore. Hatch the egg (six taps),
  then feed it (meal, or cake paid in shamrocks), play *left or right?* (five rounds), clean up
  its droppings, heal it, turn off the light at night, pet it. Baby → young (24 h) → adult
  (72 h) with an animated evolution; the adult form follows the care received: hare (well cared
  for and many steps), horse (well cared for) or grumpy goat (neglected). Abandoned for 48 h,
  it returns to the fairy mound and a new egg appears.
- Scene redrawn at 12.5 frames/s while visible: sky following the time of day (dawn, day,
  dusk, starry night with moon), drifting clouds, hills, standing stone and fairy-ring
  mushrooms; composed expressions (blink, happy, asleep, sad with rain cloud, sick with swirl),
  particles (hearts, sparkles, Zzz, shells), glowing eyes in the dark. 8-bit sound effects
  (toggle in the app). Steps of the day earn shamrocks (one per 1000).
- No background cost: its life is computed when the app opens, in 5-minute steps since the
  last visit (up to 7 days).
- Art source in `tools/puca_art.py` (generates `main/puca_gfx.c/.h` and a preview sheet).
- Fonts: ghost (launcher icon) and mute glyphs.

## 0.28.3 — 2026-11-16

- *About*: AXP202 ADC and TS pin registers (0x82–0x84) with the TS current source decoded
  (current, when it is on, battery-temperature or external-ADC function). On the V1 the TS pin
  goes to ground through 0 Ω (no thermistor); this shows whether a constant current is wasted
  there before changing anything.

## 0.28.2 — 2026-11-16

- Unused GPIOs of the V1 board (0, 2, 14, 15, 34 and 36 — 36 is the BMA423 INT2 line, whose
  resistors are not fitted) now have both input and output disabled instead of being left as
  floating inputs: an active CMOS input on a floating line can settle mid-rail and draw
  current. GPIO34/36 cannot have internal pulls. The Carrier app returns GPIO0 to the same
  state when it stops.
- Correction to 0.28.1: GPIO39 has a 10 kΩ pull-down on the board (R10), so it never floated.
  Keeping the BMA423 INT1 output driven low is harmless and stays, but it was not a leak.

## 0.28.1 — 2026-11-16

- BMA423 INT1 kept as a push-pull output, active high, with nothing mapped, even while the
  sensor is suspended (line held low in every mode).

## 0.28.0 — 2026-11-16

- Raise to wake: **gesture sensitivity** (low / medium / high, default medium) in *Settings ›
  Motion*. The Bosch gesture has no tuning, so the watch now checks its position before
  lighting: screen facing up and held still for a moment (medium), stricter and never flat on
  a table (low), or the gesture alone (high, the 0.27 behaviour). No new wake for 1.5 s after
  a glance that timed out.
- **Glance** mode for raise to wake (on by default): the screen lights dimmed (10 / 25 / 50 %,
  default 25 %) with the CPU left at its 80 MHz minimum, for 3 / 4 / 6 s (default 4 s). A
  touch (not passed to the interface) or the button wakes it fully: normal brightness, CPU
  and timeout. Turned off, raise to wake lights the screen normally.
- Small font: Œ / œ glyphs.

## 0.27.2 — 2026-11-16

- BLE log prepared before the sensors start, so a BMA423 upload failure at boot stays visible
  in *About* (it was wiped by the log reset on a cold start).

## 0.27.1 — 2026-11-16

- Motion: fixed "Motion sensor unavailable" when turning on raise to wake or the step
  counter. The BMA423 resets while it receives the soft-reset command and may not acknowledge
  the end of the I2C write; that bus error aborted the firmware upload. It is now ignored, the
  sequence is retried up to 3 times, and a remaining failure is written to the BLE log in
  *About* (step, error code, chip ID, internal status).

## 0.27.0 — 2026-11-16

- **Raise to wake** and **step counter**, each optional in the new *Settings › Motion* page
  (both off by default). They run inside the BMA423's own feature engine: the Bosch firmware
  is uploaded at boot only when one of them is on, sampling is 50 Hz averaged low-power, and
  the ESP32 stays idle while you walk. Both off: the sensor stays suspended as before.
- Raise to wake: turning the wrist lights the screen (like the button: last unseen
  notification, otherwise the face or the open app). Works in airplane mode too (GPIO39 is a
  light-sleep wake source).
- Step counter: today's steps on all three watch faces; new **Steps** app with today's total,
  distance, 7-day average and a bar chart of the last 8 days, plus a button to turn the counter
  on. Days roll over at local midnight; totals survive restarts (saved every 15 minutes and
  before any software restart).
- *About* shows the sensor state and how many wrist wake-ups happened with the screen off.
- Bosch BMA423 Sensor API added under `components/bma423/` (BSD-3-Clause); host test for the
  step bookkeeping.

## 0.26.3 — 2026-11-13

- Back to the 0.25 behaviour for SDR: opening the app restarts the watch in "SDR mode"
  (Bluetooth never started, capture banks kept for the radio), leaving it restarts normally.
  Borrowing the radio without restarting (0.26.0, 0.26.2) left too little internal RAM and
  made the firmware less stable (no sound in Radio). The capture banks join the general heap
  again in normal mode, the Helix decoders allocate their state as before, display buffers
  and Wi-Fi TX buffers are back to their 0.25 sizes.
- Kept from 0.26.1: Radio and WebRX resume at the last volume used (15 % by default).

## 0.26.2 — 2026-11-13

- SDR: fixed "Radio error (wifi)" when opening the app: the Wi-Fi driver lacked internal RAM
  now that the capture banks no longer join the general heap. While the SDR app is open the
  display render buffers shrink (2 × 28.8 KB → 2 × 9.6 KB), the Wi-Fi driver starts before
  the FFT buffers, and a failure is logged with its cause and free RAM.
- Wi-Fi: 6 static TX buffers instead of 16 (16 KB of internal RAM back for WebRX, Radio, OTA
  and SDR; the watch mostly receives).

## 0.26.1 — 2026-11-13

- Radio and WebRX resume at the last volume used (remembered separately for each app; 15 %
  the first time, or when it was left at 0) instead of always starting at 10 %.

## 0.26.0 — 2026-11-13

- **SDR without restarting**: opening the app now stops Bluetooth and borrows the radio on
  the spot; leaving it gives everything back and Bluetooth reconnects. No more "SDR mode"
  reboot.
- The 64 KB of SRAM used by the SDR capture now form a private heap: the Radio app keeps its
  MP3 / AAC decoder state and work buffers there (fast internal RAM instead of PSRAM, less CPU
  at 80 MHz). The general heap no longer includes them.
- Helix decoders: state allocated through an application hook (`helix_alloc.h`, noted in the
  components' `NOTICE.md`).

## 0.25.0 — 2026-11-13

- New **Radio** app: web radio player on the watch speaker. MP3 and AAC / HE-AAC (Helix
  fixed-point decoders), `http` and `https`, redirects, `.m3u` / `.pls` playlists, ICY song
  titles (Latin-1 or UTF-8), up to 12 stations with previous / next and a stations list,
  volume (starts at 10 %) and the 5-step tone filter (default *Bright*), last station
  remembered. 128 KB compressed buffer in PSRAM, automatic resume and retries.
- Radio power saving: Wi-Fi modem sleep; CPU capped at 160 MHz and 80 MHz with the screen off,
  raised again only while decoding cannot keep up; screen off with a good signal, Wi-Fi
  transmit power lowered from 19.5 to 11 dBm. The side button turns the screen off and
  playback goes on.
- Setup page (`Wi-Fi, WebRX and radios`): new web radios section; empty rows stay hidden
  behind *+ Add a radio*.
- Wi-Fi station and tone filter code shared between WebRX and Radio (`wifi_sta.c`,
  `audio_fx.c`).
- Documentation: Radio guide (FR/EN), READMEs, architecture, third-party notices (Helix,
  RPSL/RCSL), new screenshots.

## 0.24.0 — 2026-11-13

- New **Tone** app: repeater access tone on the speaker, 1750 Hz by default (presets 1750,
  2135, 1000 Hz; any frequency from 100 to 4000 Hz with the digit editor, remembered). Hold
  the button to transmit; a short tap still sends a 1 s burst. Clean sine at 16 kHz with
  5 ms fade in/out (no click), loud but below clipping.
- Documentation: Tone in the READMEs and in the radio apps screenshots.

## 0.23.1 — 2026-11-13

- WebRX: some profiles could not be selected. OpenWebRX+ identifies SDRs and profiles by
  UUIDs (« sdr|profile » = 73 characters) and the watch cut identifiers at 71: the server
  ignored the truncated request. Identifiers now hold 95 characters, and up to 64 profiles
  (all SDRs of a server) are listed instead of 24.
- WebRX: the profile name at the top is updated when the active profile changes (it only
  followed changes of the list).

## 0.23.0 — 2026-11-13

- WebRX: **several servers** (up to 4, optional name + address) entered on the settings page;
  the server is picked on the watch in the new **Receiver** sheet (tap the profile name),
  next to the profiles. Frequency, mode, squelch and chosen profile are kept per server; a
  profile is re-selected at connection only if it was chosen on the watch (shared servers
  are not switched over to a profile merely seen there).
- WebRX: **tone in 5 steps** (Flat, Bright, Voice, Soft, Muffled; default Soft) replacing the
  on/off voice filter: high-pass, presence bump and a low-pass going lower and steeper
  (4th order from *Soft*), less treble and hiss.
- WebRX: long names no longer wrap onto two lines (bookmark, profile, mode, list rows).
- Documentation: WebRX user guide (FR/EN), new screenshots of the radio apps and of the
  settings page, updated UI screenshots.

## 0.22.0 — 2026-11-13

- WebRX: the side button now turns the screen off while listening instead of going back to
  the watch face (which stopped the app); listening continues and the button brings the app
  back. Applies to any app that keeps the screen on.
- WebRX: each listening session starts at 10 % volume.
- WebRX: "Voice filter" (on by default, in the sound sheet, remembered): 180 Hz high-pass,
  +5 dB around 700 Hz and 2.6 kHz low-pass — less FM hiss and harsh treble, fuller voices
  on the small speaker.

## 0.21.7 — 2026-11-13

- WebRX: every listening session starts with the volume at the minimum (muted); the speaker
  icon turns orange until the volume is raised in the sound sheet.

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

# Changelog

All notable changes. Versions follow `PROJECT_VER` in `CMakeLists.txt`.

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

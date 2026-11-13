# WebRX — listening to an OpenWebRX receiver

**[Français](../fr/webrx.md)** · English

WebRX connects the watch to one of your **OpenWebRX** or **OpenWebRX+** receivers over Wi-Fi
(`https://` or `http://`), plays the demodulated audio on the watch speaker and shows the
frequency, an S-meter and a small waterfall.

![WebRX and the other radio apps](../img/radio-en.png)

## Setting up (once, from the phone)

1. Open **WebRX** on the watch and tap **Set up** (later: tap the profile name at the top →
   *Wi-Fi and servers*). Bluetooth pauses; the watch creates the Wi-Fi network
   **T-Watch-OTA** (same network and password as the firmware update).
2. On the phone, join that network and open **http://192.168.4.1**.
3. Enter up to **3 Wi-Fi networks** (in order of preference; visible networks are suggested)
   and up to **4 OpenWebRX servers** (optional name + address). Saved passwords are not shown;
   leave the field empty to keep them.
4. **Save**: the watch restarts straight into WebRX.

![Settings page](../img/webrx-setup-en.png)

Server address: the address you type in a browser, e.g. `https://sdr.example.org/` or
`http://192.168.1.20:8073/`. A server behind a reverse proxy in a sub-folder works
(`https://example.org/owrx/`). If the proxy asks for a password, write it in the address:
`https://user:password@example.org/`. HTTPS certificates are checked against the usual public
authorities (Let's Encrypt and so on); self-signed certificates are refused.

## Using it

| Where | Action |
|---|---|
| Top bar, profile name | **Receiver** sheet: choose the **server** (when several are set) and the **profile** (band) |
| Top bar, speaker | **Sound** sheet: volume, squelch, tone |
| Frequency | tap a digit: it becomes the step of the − / + buttons (long press repeats) |
| Name under the frequency | **Bookmarks** of the band (the server's) |
| Mode chip | modes **of the server**, with their bandwidth (custom modes included) |
| Waterfall | tap: listen there (snaps to a nearby bookmark); ×1 / ×4 / ×16 zoom in its corner |
| ■ / ▶ | stop / start listening (Wi-Fi off when stopped) |
| Side button | screen off, **listening continues**; press again to come back |
| Swipe → or ‹ | leave the app: listening stops, Bluetooth comes back |

- Each listening session starts at **10 % volume**.
- **Squelch**: mutes the audio while the signal stays below the threshold (applied by the
  server). Far left: off. The current signal level is shown under the slider.
- **Tone** (5 steps, kept): *Flat*, *Bright*, *Voice*, *Soft* (default), *Muffled*. To the right:
  less treble and FM hiss, more body to voices on the small speaker.
- The screen stays on while listening; the button turns it off.
- The watch remembers the chosen server and, per server, the frequency, mode, squelch and the
  profile you picked on the watch. On a shared server, a profile is only re-selected if you
  chose it yourself.

## Messages

| Message | Meaning |
|---|---|
| *No known network in range* | none of the saved Wi-Fi networks is visible |
| *Wi-Fi password rejected* / *Could not connect to Wi-Fi (reason N)* | check the network on the settings page |
| *Server unreachable or certificate rejected* | wrong address, server down, or self-signed certificate |
| *No OpenWebRX WebSocket at this address* (HTTP code) | the address does not lead to OpenWebRX (path?) |
| *Refused by the server* + text | full (*Too many clients*) or banned address (*Client address banned*, OpenWebRX+ robot protection) |
| *Connection lost, resuming…* | automatic retry after 2, 10, 20 and 40 s, then tap to retry |

Each cut is also written to the log in *Settings › About*: cause, frames and bytes received,
duration, Wi-Fi RSSI, last message type and, on a read error, the transport/TLS codes.

## Power

While listening: Wi-Fi without modem sleep (the server sends audio and spectrum continuously),
CPU capped at 160 MHz (80 MHz with the screen off), screen on. Count on several tens of mA:
the listening time is measured in hours, not days. Leaving the app turns Wi-Fi fully off.

How it works inside: [architecture](architecture.md).

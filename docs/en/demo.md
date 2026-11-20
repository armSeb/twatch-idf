# Demo — "Tír na nÓg"

[Français](../fr/demo.md) · **English**

A small demoscene-style intro (a nod to farbrausch's *fr-08*): about 1 min 10 of visuals and
music, then you take the controls. Nothing is stored: stars, text, textures, terrain and music
are computed on the watch at start-up and on every frame. The name is the Otherworld of Irish
mythology, the "land of youth".

![Demo](../img/demo-en.png)

## What happens

| When | Scene |
|---|---|
| 0 – 7 s | accelerating starfield, "Seb & Claude present", then ~900 particles swirl into the title, a golden glint sweeps it, explosion and flash |
| 7 – 15 s | endless textured tunnel: interlaced pattern, cycling colours, twist, pulses on the drums, "100 % computed on the watch" |
| 15 – 31 s | shaded 3D torus (288 faces, painter's sort, specular highlight) swelling on the kick drum |
| 31 – 69 s | flight over hills at sunset: rivers, beaches, meadows, rock, snow, sun and glow; the autopilot follows the valleys and banks into turns; sine-scroller greetings |
| then | **"Your turn!"**: you fly by tilting your wrist |

## Controls

- **Tilt left / right**: turn (the plane banks).
- **Tilt towards you / away**: climb / dive.
- The neutral position slowly follows your pose: hold the watch as if reading the time.
- **Tap the screen**: sound off / on (the tune keeps running silently).
- **Swipe**: leave.

The screen stays on and the CPU at 240 MHz while the demo is shown; it stops (music included)
as soon as you leave or turn the screen off with the button. Flying reads the accelerometer
directly, even with the step counter and raise to wake off; they get their setting back when
you leave.

## The music

A small 4-voice tracker-style synth (`demo_synth.c`), 125 BPM, in A minor (Am – F – C – G):
arpeggio alone, then bass and hi-hats, a snare roll, and the melody comes in with the full
drums; the second half loops. Melody as a 25 % square, arpeggio and bass as 50 % squares, kick
as a falling sine, snare and hi-hats from noise. Rendered at 16 kHz in its own task; it is also
the frame clock, so effects land right on the beat.

## How it is done

Frames are computed at 120×120 in an internal-RAM buffer, then doubled into a 240×240 canvas
in PSRAM that LVGL sends to the display.

- **Tunnel**: precomputed per-pixel angle, depth and shade tables; each frame is just a
  128×128 texture lookup and a cycling 256-colour palette.
- **Torus**: vertices rotated on three axes, back faces culled, per-face lighting (diffuse +
  specular), far-to-near sort, scanline triangle fill.
- **Landscape**: 256×256 heightmap generated at start-up (4-octave value noise, periodic so
  the world tiles seamlessly) and a colour map shaded by slope; column-by-column "voxel space"
  rendering with a height buffer, fog towards the horizon, roll by shifting the horizon per
  column.
- **Text**: a tiny home-made 5×7 font, accents drawn (acute, grave, circumflex).

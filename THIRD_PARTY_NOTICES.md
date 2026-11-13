# Third-party notices / Mentions des composants tiers

The firmware source code of this repository is licensed under the MIT License (see
[LICENSE](LICENSE)), except where stated below. Firmware binaries built from it also contain
the following third-party software and assets, each under its own license.

*Le code source du firmware est sous licence MIT (voir [LICENSE](LICENSE)), sauf mention
contraire ci-dessous. Les binaires compilés contiennent aussi les logiciels et ressources
tiers suivants, chacun sous sa propre licence.*

| Component | Where | Copyright | License |
|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) 5.5.6 | linked into the firmware | Espressif Systems | Apache-2.0 |
| Espressif precompiled Wi-Fi, Bluetooth controller and PHY libraries (part of ESP-IDF) | linked into the firmware | Espressif Systems | Apache-2.0 |
| [FreeRTOS](https://www.freertos.org) kernel (via ESP-IDF) | linked | Amazon.com, Inc. or its affiliates | MIT |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) (via ESP-IDF) | linked | Swedish Institute of Computer Science | BSD-3-Clause |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (via ESP-IDF) | linked | The Mbed TLS Contributors | Apache-2.0 |
| [Apache NimBLE](https://github.com/apache/mynewt-nimble) (via ESP-IDF) | linked | The Apache Software Foundation | Apache-2.0 |
| wpa_supplicant, newlib and the other ESP-IDF firmware components | linked | see ESP-IDF's `docs/en/COPYRIGHT.rst` | BSD and others, as listed there |
| [LVGL](https://github.com/lvgl/lvgl) 9.2.2 | linked | LVGL Kft | MIT |
| [Montserrat](https://github.com/JulietaUla/Montserrat) (Medium), converted to bitmaps in `main/fonts/` | embedded | Julieta Ulanovsky and contributors | SIL Open Font License 1.1 |
| [Font Awesome Free](https://fontawesome.com) 5 icons, converted to bitmaps in `main/fonts/` | embedded | Fonticons, Inc. | Icons: CC BY 4.0 — font files: SIL OFL 1.1 |
| Helix MP3 decoder, in [`components/helix_mp3/`](components/helix_mp3/) (see its `NOTICE.md`), from [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) | linked (Radio app) | RealNetworks, Inc.; ESP adaptation Earle F. Philhower III | **RPSL 1.0** or RCSL 1.0 |
| Helix AAC / HE-AAC decoder, in [`components/helix_aac/`](components/helix_aac/) (see its `NOTICE.md`), from ESP8266Audio | linked (Radio app) | RealNetworks, Inc.; ESP adaptation Earle F. Philhower III | **RPSL 1.0** or RCSL 1.0 |
| TV-B-Gone® WORLDcodes power-code database and RMT transmitter, adapted in [`components/tvbgone/`](components/tvbgone/) (see its `NOTICE.md`) | embedded unless built with `TW_NO_TVOFF=1` | Mitch Altman, Limor Fried; Arduino port Ken Shirriff; ESP-IDF component Pedro Minatel | **CC BY-SA 4.0** |

Tools used to build, not shipped in the firmware: [lv_font_conv](https://github.com/lvgl/lv_font_conv)
(MIT), [esptool](https://github.com/espressif/esptool) (GPL-2.0-or-later, used as a separate
program).

References consulted, no code copied: [TTGO_TWatch_Library](https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library)
(MIT; pinout and schematics), [My-TTGO-Watch](https://github.com/sharandac/My-TTGO-Watch)
(GPL-3.0; design ideas only), Apple's public ANCS / AMS specifications and Accessory Design
Guidelines.

## Redistributing binaries / Redistribuer les binaires

When you redistribute a firmware binary, keep this file, [LICENSE](LICENSE) and the license
texts of the components above available with it (the Apache-2.0 text is in ESP-IDF, the
CC BY-SA 4.0 notice in `components/tvbgone/LICENSE`). A binary that includes *TV off*
contains CC BY-SA 4.0 material: its source, with modifications, is in
`components/tvbgone/`, under the same license. The Helix decoders are covered by the
RealNetworks Public Source License: their source (texts in `components/helix_mp3/RPSL.txt`,
`components/helix_aac/RPSL.txt`) must stay available with the binary, which this repository
does; the only changes are listed in each component's `NOTICE.md`.

*Pour redistribuer un binaire, joindre ce fichier, [LICENSE](LICENSE) et les textes de
licence des composants ci-dessus. Un binaire qui inclut *TV off* contient du contenu CC BY-SA
4.0, dont la source modifiée est dans `components/tvbgone/`, sous la même licence.*

## Trademarks / Marques

iPhone, iOS and Apple Watch are trademarks of Apple Inc. T-Watch, LilyGo and TV-B-Gone are
trademarks of their respective owners. This project is independent and not affiliated with,
endorsed or sponsored by them.

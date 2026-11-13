# Helix MP3 decoder

Fixed-point MPEG-1/2 layer III decoder from the Helix DNA project,
Copyright (c) 1995-2005 RealNetworks, Inc., licensed under the RealNetworks Public Source
License (RPSL, [RPSL.txt](RPSL.txt)) or the RealNetworks Community Source License (RCSL,
[RCSL.txt](RCSL.txt)). Each source file keeps its original license header.

Taken from the ESP8266Audio library (Earle F. Philhower III,
https://github.com/earlephilhower/ESP8266Audio), `src/libhelix-mp3`, which adapted the code
to Arduino/ESP (tables in flash). This project only adds `compat/` (empty Arduino stand-ins), accepts `ESP_PLATFORM` where `ARDUINO` was tested,
and the ESP-IDF build file; the decoder sources are otherwise unchanged.

# Licensing

## Clip-Boy — GNU General Public License v3.0

Copyright (C) 2026 The Clip-Boy Authors

> **Note:** set the copyright holder above to your name or legal entity.

Clip-Boy is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License** as published by the Free Software
Foundation, **version 3**. The full license text is in [`LICENSE`](LICENSE).

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

### Why GPLv3

The firmware links **arduino-audio-tools**, which is GPL (copyleft). A binary
that incorporates it must be distributed under GPL-compatible terms with
complete corresponding source, so the combined Clip-Boy firmware is licensed
GPLv3. (GPL still permits selling the badges; it just keeps derivatives open.)

The MP3 decoder is **minimp3** (public domain / CC0) — chosen to keep the audio
path free of additional copyleft (it replaced libhelix, whose RealNetworks RPSL
licensing was GPL-incompatible).

If you build a variant **without** audio-tools, the first-party Clip-Boy code
itself is also available to you under the MIT License — but anything shipped
with audio-tools is GPLv3 as a whole.

## Third-party notices

Clip-Boy incorporates third-party open-source software. MIT/BSD-licensed
components remain under their own (GPL-compatible) permissive licenses within
the combined work; copyleft components are noted below.

### ESP32 Marauder — MIT License

The `ClipBoy` library (Wi-Fi/Bluetooth tooling) is a fork of **ESP32 Marauder**
by **JustCallMeKoko** (https://github.com/justcallmekoko/ESP32Marauder), used
under the MIT License:

```
MIT License

Copyright (c) 2020 Just Call Me Koko

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Other libraries

Each retains its own license and copyright; consult each project for the
authoritative text.

| Library | Author / source | License |
|---|---|---|
| **arduino-audio-tools** | Phil Schatzmann | **GPL-3.0** (drives the GPLv3 of the firmware) |
| **minimp3** (MP3 decoder) | lieff (via pschatzmann) | Public domain (CC0-1.0) |
| LVGL | LVGL Kft. | MIT |
| LovyanGFX | lovyan03 | FreeBSD (BSD-2-Clause) |
| NimBLE-Arduino | h2zero | Apache-2.0 |
| ArduinoJson | Benoit Blanchon | MIT |
| Adafruit NeoPixel | Adafruit | LGPL-3.0 |
| SparkFun VL53L5CX | SparkFun | MIT |
| ESPAsyncWebServer / AsyncTCP | Mathieu Carbou forks | LGPL-3.0 |
| lz4 | Yann Collet | BSD-2-Clause |

> Licenses listed from each project's metadata; verify against the version you
> ship.

### First-party libraries

`LovyanInit-Waveshare`, `HRCode4x4`, and `HRScanGuidance` are part of this
project and are covered by the Clip-Boy license (GPLv3) above.

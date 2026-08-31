# Clip-Boy

Clip-Boy is an open-source DEF CON electronic badge with a digital wasteland
survivor soul, running on an ESP32-S3 with an LVGL touch UI.

> **Want one?** https://brycebadges.com  -  Docs & help: https://safehazard.github.io/Clip-Boy

## Repository layout
- **`Clip-Boy/`** - the firmware (Arduino sketch `Clip-Boy.ino` + sources, vendored
  `Clip-Boy/libs/`, build scripts in `Clip-Boy/scripts/`).
- **`Clip-Boy/hardware/`** - open-hardware package to build your own badge: bill of
  materials (`BOM_*.md`/`.csv`), PCB source (`Clip-Boy-PCBs.eprj2`, EasyEDA) + fab-ready
  Gerbers for both boards, the enclosure (`Enclosure-*.step`), and the OmniTag HR-code
  master (`omnitag.3mf`).
- **`Clip-Boy/tags/`** + **`Clip-Boy/stands/`** - ready-to-print `.3mf` models: one
  collectible HR-code tag per ID, plus display stands (regenerate any time with
  `Clip-Boy/scripts/build_all_tags.py`). Print assets only - not needed to compile.
- `index.html`, `postcon/` - the project's web pages. `README.md`, `LICENSE`.

## License
**GPLv3** (`LICENSE`, repo root) - forced by linking the GPL `audio-tools` library.
First-party code is also offered under MIT for audio-tools-free builds. Third-party
components: `Clip-Boy/THIRD_PARTY.md`. AI disclosure: `Clip-Boy/AI-DISCLOSURE.md`.
Acceptable use: `Clip-Boy/acceptable_use.md`.

## Credits
The **drone Remote-ID detector** (ASTM F3411 / Open Drone ID) was contributed by
**[zenrandom](https://github.com/zenrandom)**; the companion transmitter/simulator
toolkit lives at **[SafeHazard/drone-remoteid](https://github.com/SafeHazard/drone-remoteid)**.
Full on-device credits are under **Settings > Credits**; third-party components in
`Clip-Boy/THIRD_PARTY.md`.

## Build setup
Versions are pinned for reproducibility:
- **Git Bash** (Windows) to run the build scripts: https://git-scm.com/download/win
  (macOS/Linux already have bash). **Python 3.9+**: https://www.python.org/downloads/
- **`arduino-cli`** (1.4.x).
- **ESP32 core `esp32:esp32@2.0.10`** - REQUIRED, exact version (what the signed
  releases are built with; a different core = a different, non-reproducible binary):
  ```bash
  arduino-cli core install esp32:esp32@2.0.10
  ```
  `build.sh` enforces this and injects the ClipBoy linker flags itself, so **no
  `platform.txt` hand-patching** is needed (unlike a stock Marauder setup).
- Third-party libraries are **vendored** in `Clip-Boy/libs/` - nothing to fetch.

## Build
```bash
cd Clip-Boy
bash scripts/build.sh              # Sn34k-Boy (default, listen-only)
bash scripts/build.sh --res34rch   # Res34rch-Boy (active research tools)
```
Flags compose: `--test`, `--rift` (alt boot screen), `--upload --port COMx`.

## Reproducibility
Builds are deterministic (stamp derived from the commit or a pinned `VERSION`, and
the build path is stripped from the image). From the exact release commit (with the
real `secret.h` + the release `VERSION`), the owner reproduces the signed release
byte-for-byte; a public builder gets a **functionally identical** badge (a fixed ~64
bytes of metadata differ: the ELF self-hash + the esptool image hash). The sketch is
`Clip-Boy.ino` here; the official signed releases are built from the same name.

## The ARG secret
The badge's ARG finale derives phone-unlock codes from a private HMAC key that is
**not** in this repo: `arg_unlock.h` compiles an all-zero placeholder by default, so
the firmware builds and runs (everything works except matching the live phone/IVR).
Define your own 32-byte key to run your own finale.

## Fonts (verify licensing for YOUR use)
The UI typeface (**monofonto**) and tag typeface (**Trek**) are **not included**;
the pre-generated glyph headers (`ui_font_pipboy_*.c`) ARE, so the firmware builds
without them. You only need the `.otf` files to *regenerate* fonts or build the tags.
We don't host or vouch for these fonts - check the license for your use:
monofonto https://www.dafont.com/monofonto.font  -  Trek https://font.download/font/trek

## Legal
Clip-Boy is a fan-made parody project - **not affiliated with, endorsed by, or
sponsored by** any property it references; all such properties remain (C) their
respective owners. Use the badge's tools only on networks/devices you own or are
authorized to test - see `Clip-Boy/acceptable_use.md`.

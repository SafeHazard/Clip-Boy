# Third-party libraries (vendored)

To make a fresh clone build with no external fetches, the third-party Arduino
libraries Clip-Boy links are **vendored** into [`libs/`](libs) — copied in and
trimmed to what compiles (each library's `src/`, root sources, manifest, and
license; `examples/`/`docs/`/`tests/` removed). Re-vendor with
[`scripts/vendor_libs.sh`](scripts/vendor_libs.sh).

Each library keeps its own license file under `libs/<name>/` — **that file is
authoritative.** The table below records provenance (version + upstream) and the
license as a convenience; see also [`LICENSE.md`](LICENSE.md). The combined
firmware is GPLv3 (see `LICENSE.md` for why).

| Library | Version | Upstream | License |
|---|---|---|---|
| lvgl | 9.2.0 | github.com/lvgl/lvgl | MIT |
| LovyanGFX | 1.2.19 | github.com/lovyan03/LovyanGFX | BSD-2-Clause (FreeBSD) |
| audio-tools | 1.1.2 | github.com/pschatzmann/arduino-audio-tools | GPL-3.0 |
| minimp3 | 0.1.0 | github.com/pschatzmann/arduino-minimp3 (lieff/minimp3) | CC0-1.0 (public domain) |
| NimBLE-Arduino | 2.3.7 | github.com/h2zero/NimBLE-Arduino | Apache-2.0 |
| ArduinoJson | 7.4.2 | github.com/bblanchon/ArduinoJson | MIT |
| Adafruit_NeoPixel | 1.15.2 | github.com/adafruit/Adafruit_NeoPixel | LGPL-3.0 |
| SparkFun_VL53L5CX_Arduino_Library | 1.0.3 | github.com/sparkfun/SparkFun_VL53L5CX_Arduino_Library | MIT |
| ESPAsyncWebServer | 2.10.4 | github.com/mathieucarbou/ESPAsyncWebServer | LGPL-3.0 |
| AsyncTCP | 1.1.4 | github.com/mathieucarbou/AsyncTCP | LGPL-3.0 |
| ESP32Ping | 1.7 | github.com/marian-craciunescu/ESP32Ping | (see library) |
| LinkedList | 1.3.3 | github.com/ivanseidel/LinkedList | MIT |
| lz4-1.10.0 | 1.10.0 | github.com/lz4/lz4 (Arduino packaging) | BSD-2-Clause |
| ClipBoy | 1.0.0 | fork of github.com/justcallmekoko/ESP32Marauder | MIT |
| opendroneid-core-c | vendored source | github.com/opendroneid/opendroneid-core-c | Apache-2.0 |
| ClipBoyTheremin | 1.0.0 | first-party (this project) | GPL-3.0 |
| LovyanInit-Waveshare | 0.1.0 | first-party (this project) | GPL-3.0 |
| HRCode4x4 | 1.0.0 | first-party (this project) | GPL-3.0 |
| HRScanGuidance | 0.1.0 | first-party (this project) | GPL-3.0 |

**opendroneid-core-c** (the ASTM F3411 / Open Drone ID decoder that powers the
Detect ▸ Drone ID Remote-ID tool) is vendored as source *inside* the ClipBoy
library — `libs/ClipBoy/src/odid_decode.c` + `opendroneid.h` — rather than as its
own `libs/<name>/` directory. Its Apache-2.0 license text is kept beside those
files at `libs/ClipBoy/src/LICENSE.opendroneid`, and each file retains its SPDX
`Apache-2.0` header + `Copyright (C) 2019 Intel Corporation`. Apache-2.0 is
one-way compatible with the combined GPLv3 firmware.

`libs/lv_conf.h` is our LVGL configuration (LVGL reads it from the folder above
`libs/lvgl/`). Notably `LV_USE_FS_ARDUINO_ESP_LITTLEFS = 0` — see CLAUDE.md.

## Fonts

| Font | Source | License | Used for |
|---|---|---|---|
| Monofonto | Larabie Fonts (external, not committed) | freeware; embedded glyphs only | `ui_font_pipboy_*.c` (the Pip-Boy UI typeface) |
| Noto Emoji | google/fonts `ofl/notoemoji` (static instance in [`assets/fonts/`](assets/fonts)) | OFL-1.1 (`assets/fonts/OFL.txt`) | `ui_font_emoji_14.c` — 24-emoji fallback subset |

The `ui_font_*.c` files are **generated, committed artifacts** (the rendered
glyph bitmaps, not the source outlines). Regenerate with
[`scripts/build_fonts.sh`](scripts/build_fonts.sh). Monofonto's OTF is not
redistributed (font EULA); set `MONOFONTO_OTF` to point the script at it. Noto
Emoji is OFL, so its static instance is vendored for reproducibility.

## Local modifications to vendored libraries

Kept minimal; each is commented in-place with a `Clip-Boy local patch` marker so
it survives review and is easy to re-apply after a re-vendor.

- **SparkFun_VL53L5CX_Arduino_Library** — `begin()` is made idempotent
  (`SparkFun_VL53L5CX_Library.cpp`): it now frees `Dev` + `VL53L5CX_i2c` before
  re-`new`-ing them, and the two pointers are default-initialized to `nullptr`
  in the header. Upstream `begin()` allocates both unconditionally with no
  destructor, so every repeated `begin()` orphaned ~2.7 KB. The theremin
  re-`begin()`s the sensor on each enable, which leaked ~2,768 B per on/off
  cycle (caught by `scripts/tests/test_mutual_exclusion.py` / `probe_leak.py`).
  Re-apply if you re-vendor this library.

- **ClipBoy (PCAP single-owner + LittleFS fallback, DC34-147)** — the host badge
  owns the one global Arduino `SD` (mounted on HSPI in `coll_init`), so ClipBoy
  must not re-`SD.begin()`. Patches: `ClipBoyMarauder::setSDAvailable(bool)`
  (`ClipBoyMarauder.cpp/.h`) lets the badge flag the card up so PCAP uses the
  shared mount; `WiFiScan::startPcap` (`WiFiScan.cpp`, + `#include <LittleFS.h>`)
  prefers SD and falls back to the badge's LittleFS partition (size-capped to
  free − 768 KB so a capture can't starve CSV/.sav/radio data); `Buffer`
  (`Buffer.cpp/.h`) gains `setMaxBytes()` + a `writtenBytes` cap enforced in
  `saveFs()`. Each marked `Clip-Boy local patch (DC34-147)`. Re-apply on
  re-vendor.

- **ClipBoy (Settings in-memory only, no SPIFFS rewrite, DC34-147 perf)** —
  `Settings::saveSetting<bool>` (both `bool` and `String` value overloads,
  `Settings.cpp`) no longer rewrites `/settings.json` to SPIFFS; it updates only the
  in-memory `json_settings_string` that `loadSetting<>()` reads. The full-file
  re-serialize was a ~1-2s synchronous flash write that froze the UI when SavePCAP was
  toggled from a touch handler. Clip-Boy relies on none of Marauder's SPIFFS
  persistence (SavePCAP re-syncs from our NVS every boot; WiFi creds pass directly to
  `joinWiFi`). Marked `Clip-Boy local patch (DC34-147 perf)`. Re-apply on re-vendor.

- **ClipBoy (PCAP-only capture gated on 'Allow PCAP Saving', DC34-147)** — Raw/PCAP and
  PMKID/EAPOL are file-only captures; with saving off they'd run and write nothing. Gated
  at two layers so EVERY entry path errors cleanly: the Marauder CLI handlers `sniffraw`
  and `sniffpmkid` (`CommandLine.cpp`) return early with a message; `WiFiScan::RunRawScan`
  (pcap branch only — signal-strength monitor stays available) and `RunEapolScan`
  (`WiFiScan.cpp`) bail before `startPcap`. Gate reads `settings_obj.loadSetting<bool>
  ("SavePCAP")`. Marked `Clip-Boy local patch (DC34-147)`. Re-apply on re-vendor.

- **ClipBoy (MAC-tracker Serial writes made non-blocking, DC34)** — `WiFiScan::updateTrackerUI`
  (`WiFiScan.cpp`) ran every 1s on core 1 (via `cb.loop()` immediately before
  `lv_timer_handler`) and emitted ~21 unconditional `Serial` writes/sec. With no host
  draining the USB-CDC TX buffer each `write` blocked up to `setTxTimeoutMs` (50ms),
  stalling core 1 ~1s out of every 1s → the MAC-tracker UI froze regardless of packet
  density. Each write (`---`, `FOLLOWING`, per-entry MAC/frame line) is now guarded on
  `Serial.availableForWrite()`, so a serial-CLI user still sees the dump (their monitor
  drains the buffer → room available) while a touch-UI user with no reader skips it (buffer
  full) and never blocks. Deliberately NOT `DEBUG_VERBOSE`-gated, to preserve CLI output.
  Marked `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (Raw/PCAP UI-freeze: rate-limit the pcap flush, DC34)** — `ClipBoyMarauder::loop()`
  (`ClipBoyMarauder.cpp`) called `buffer_obj.save()` EVERY loop; `Buffer::save`/`saveFs` does a
  full open+append+close to SD/LittleFS, and during Raw/PCAP capture the RX buffer is refilled
  continuously, so core 1 did tens of ms of blocking file I/O per loop right before
  `lv_timer_handler()` -> the UI froze. Now flushed at most every 150ms (RAM double-buffer absorbs
  the gap; `save()` early-returns when empty, so non-capture tools are unaffected). Marked
  `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (Raw/PCAP Serial storm made non-blocking, DC34)** — `WiFiScan::renderRawStats`
  (`WiFiScan.cpp`) emitted ~11 unconditional `Serial.println` at 1Hz on core 1; with no host
  draining USB-CDC each blocked up to `setTxTimeoutMs` (50ms) -> ~0.5s core-1 stall/sec. Same

  > ⚠ 2026-07-27: the badge now runs a **0 ms** TX timeout until a host actually reads it
  > (`cb_serial_tx_relax()` in `Clip-Boy.ino`), so the no-reader case no longer blocks.
  > These guards still apply when a monitor IS attached. Measured: the 50 ms default was
  > costing every passive WiFi tool ~60% of its frames (`docs/test-reports/h1-cdc-stall-2026-07-27.md`).
  fix as the MAC dump: the whole block is guarded on `Serial.availableForWrite()` (a serial-CLI
  reader still sees it; no reader skips it). Marked `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (stale-accumulator clean-slate on tool start, DC34)** — Marauder-side accumulators
  the UI reads persisted across tool switches, so a tool could display a PRIOR tool's data (e.g.
  Raw/PCAP showed a stale `num_probe` from a Monitor run; MAC Tracker/BT/Flock/Pwnagotchi lists
  showed old rows on re-open). `StopScan` only resets `*_frames` (not `num_*`) and never touched
  the device lists; three `clear*` fns were dead code. Added `ClipBoyMarauder::resetDisplayAccumulators()`
  (`ClipBoyMarauder.cpp/.h`) — called on every tool start from `dispatch_clipboy_action` — which
  runs `resetPacketCounters()` + `clearBTDevices/clearFlockDevices/clearPwnagotchis` (were dead) +
  two new `WiFiScan::clearMacTracker()` (wipes the `mac_entries` hash table `clearMacHistory` misses)
  and `clearChannelActivity()` (`WiFiScan.cpp/.h`). Deliberately does NOT clear the AP/STA/SSID
  lists (select-then-run inputs). Also exposed `reqFrames`/`respFrames` in `CBPacketCounters` so
  Raw/PCAP shows real probe counts. Marked `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (Raw/PCAP channel hop + selector, DC34)** — Raw/PCAP was FIXED-channel (parked on
  whatever `set_channel` it inherited), so it captured 0 beacons on a quiet channel. `packetRateLoop`
  (`WiFiScan.cpp`) now `channelHop()`s ~1s/channel during `WIFI_SCAN_RAW_CAPTURE` when the new
  `raw_hop` flag is set (EAPOL is never hopped — a handshake must stay on the target channel). Added
  `WiFiScan::setRawChannel(uint8_t)` + `ClipBoyMarauder::setRawCaptureChannel(uint8_t)` (0 = hop the
  band = the UI "Hop (all)" default; 1-14 = lock via `changeChannel`). The UI adds a Channel dropdown
  to the Raw/PCAP pane. Marked `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (EAPOL capture: counter + target-channel lock, DC34)** — the badge never registered
  EAPOL. Two bugs in `WiFiScan.cpp`: (1) Raw/PCAP's `0x888e` ethertype check was nested under the
  `WIFI_PKT_MGMT` branch, but EAPOL is a 802.11 DATA frame -> it fell into `data_frames++` and the
  check never ran (EAP:0 while the handshake WAS written to the pcap); moved it into the data branch.
  (2) `RunEapolScan` sat on a stale `set_channel` and the EAPOL loop hopped all 14 channels every 2s,
  so a <100ms handshake almost never coincided; now it locks to the SELECTED AP's channel and only
  hops (`eapol_hop`) when no AP is selected. Marked `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (large PSRAM capture buffer, DC34)** — `Buffer` used two 8KB (`BUF_SIZE`) internal-RAM
  buffers; on a capture burst (a 4-way handshake amid beacons) both filled before the slow SD write
  drained and packets dropped (missed EAPOL msg 2/4). Now lazily allocates two `CB_PCAP_BUF_SIZE`
  (128KB) PSRAM buffers on first capture (`Buffer::ensureBuffers`, `Buffer.cpp/.h`), with `bufCap`
  driving the fill checks and a fallback to `BUF_SIZE` internal RAM if `ps_malloc` fails. Marked
  `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (expose Channel-Activity page, DC34)** — the ranged channel-activity hop only
  sweeps 7 channels per `activity_page` (page 1 = ch 1-7, page 2 = ch 8-14) and the vendored
  default is stuck at page 1, so a "channels 1-14" histogram left ch 8-14 permanently empty.
  Added `ClipBoyMarauder::setChannelActivityPage(uint8_t)` + `getChannelActivityPage()`
  (`ClipBoyMarauder.cpp/.h`) writing the public `wifi_scan_obj.activity_page`; the UI cycles
  it 1↔2 each poll tick so all 14 channels sample. No WiFiScan edit (member already public;
  picked up on the next 100ms hop). Marked `Clip-Boy local patch (DC34)`. Re-apply on re-vendor.

- **ClipBoy (CLI `help` reflects actual hardware/edition, DC34)** — the upstream
  Marauder `help` printer advertised commands ClipBoy can't run: the GPS family
  (`gps`/`gpsdata`/`nmea`/`gpspoi`/`gpstracker`) and `led` (Marauder's neopixel
  path is absent — badge LEDs use `neopixel_driver.h`). The GPS help lines are now
  `#ifdef HAS_GPS` (matching the already-gated dispatch + the existing `wardrive`
  pattern); the `led` help line is removed; `HELP_UPDATE_CMD_A` drops the dead
  `-w` (OTA) option (only `-s`/SD works). `CommandLine.cpp` + `CommandLine.h`. The
  active-TX tools were already `#ifdef CLIPBOY_RES34RCH`-gated (proven by
  `check_sku_binaries.py`) — unchanged. Re-apply on re-vendor.

- **ClipBoy (Evil Portal per-activation DRAM leak, DC34)** — each Evil Portal Start
  re-ran `setupServer()` (~12 `server.on` handlers) + `new CaptiveRequestHandler()` +
  `dnsServer.start()`, while Stop (`EvilPortal::cleanup`) freed only the PSRAM HTML — so
  every Start/Stop cycle leaked the DNS UDP pcb + SoftAP netif/DHCP pool and grew
  `server._handlers` unbounded (the integration test measured ~21 KB on first activation,
  ~2 KB/cycle after). Fix (`EvilPortal.cpp`): `startAP()` registers the web endpoints +
  captive handler ONCE (`static bool s_server_registered`); `cleanup()` now
  `dnsServer.stop()` + `WiFi.softAPdisconnect(true)`. The AsyncTCP service task (~16 KB,
  one-time, shared) is deliberately left alone. Marked `Clip-Boy local patch (DC34 heap-leak)`.
  Re-apply on re-vendor.

- **ClipBoy (active-TX UI FPS: cap per-call TX burst, DC34)** — active-transmit modes ran
  their whole packet burst inline in one `wifi_scan_obj.main()` call, starving
  `lv_timer_handler()` (runs once/loop): Beacon Spam Funny sent 7×12=84 `broadcastSetSSID`
  (+ a redundant `delay(1)` each) per call → ~9 fps and a 7-10 s start stall;
  Beacon-Random/Flood-Auth/Deauth sent 55/call. `WiFiScan.cpp`: Funny `7→1`, Beacon Random
  `55→8`, Auth/Deauth `55→15`, and dropped the redundant per-send `delay(1)` in
  `broadcastSetSSID` (`changeChannel()` already settles 1 ms). The bursts were sized for the
  old BLOCKING M5Stick UI; our non-blocking LVGL loop runs proportionally more often, so
  aggregate TX rate is ~unchanged. Marked `Clip-Boy local patch (DC34 fps)`. Re-apply on
  re-vendor. ⚠ verify TX still radiates with an external receiver (badge counters can mask it).

- **ClipBoy (SAE-commit `current_act` cross-task UAF hardening, DC34)** — `getSAEACT`
  (`WiFiScan.cpp`) ran `free(current_act); current_act = malloc(n); memcpy(...)` on a global
  heap pointer **inside the promiscuous RX callback** (`beaconSnifferCallback`, WiFi-driver
  task), while the main-task SAE flood (`sendSAECommitFrame`) is written to read `current_act[]`
  — a cross-task use-after-free (a beta tester's SCA finding; currently non-crashing only because
  `current_act_len` is never assigned, so the read is dead — a latent landmine). Replaced the
  heap pointer with a fixed static `current_act_buf[255]` (never freed) published under a new
  `portMUX` spinlock `current_act_mux`, with the copy length clamped; the reader copies under the
  same lock and re-clamps. No heap ops in the RX callback, no UAF, no OOB. Marked `Clip-Boy local
  patch (DC34)`. Re-apply on re-vendor. Upstream PR drafted — see `docs/prs/marauder-concurrency-prs.md`
  (PR-1), along with the wider audit of the same bug class (device-list/String races, Evil Portal
  creds, PCAP Buffer) for contribution back to justcallmekoko/ESP32Marauder.

- **ClipBoy (cross-task concurrency audit — 4 more fixes, DC34)** — a 4-agent hunt for the
  `current_act` bug class across the tool corpus found the SAME cross-task pattern LIVE (it was only
  dead-code-defused at the SAE spot). All fixed in `libs/`; upstream PR drafts in
  `docs/prs/marauder-concurrency-prs.md`:
  - **Device lists (`AccessPoint`) — the big one.** `access_points`/`stations` are mutated from 6
    sniffer RX callbacks (WiFi task) and copied by-value by the main-task UI/attack readers; the
    `String essid`/`man` fields would free+realloc their heap buffer during that copy → cross-task
    UAF/heap-corruption. **POD-ified `AccessPoint`** (essid/man → `char[33]`; `EvilPortal.h`) so the
    by-value copy is a plain memcpy; converted every String-idiom site in `WiFiScan.cpp`/
    `ClipBoyMarauder.cpp`/`CommandLine.cpp`. Plus NULL-guards in `LinkedList::set`/`operator[]`
    (`libs/LinkedList/LinkedList.h`) — `getNode()`'s shared node cache can be corrupted by a
    concurrent access, returning NULL for an in-range index → the old unconditional `->data` deref
    crashed (write-to-null).
  - **Evil Portal captured creds + `index_html` (`EvilPortal.cpp`/`.h`).** `user_name`/`password`
    `String`s reassigned by the `/get` handler (AsyncTCP task) while the main task read them →
    String UAF; now fixed `char[64]` buffers guarded by a `portMUX` (`cred_mux`). And `cleanup()` no
    longer `free()`s `index_html` while app-lifetime web handlers may still stream it (kept for the
    app lifetime, reused).
  - **`analyzer_name_string` (`WiFiScan.cpp`/`.h`).** Channel-Analyzer name reassigned in the RX
    callback, `.c_str()`-read on main → String UAF; now a `char[33]` behind guarded
    `setAnalyzerName`/`getAnalyzerName` accessors (`analyzer_name_mux`).
  - **PCAP `Buffer` (`Buffer.cpp`/`.h`).** The `while(saving)delay()` busy-wait was a TOCTOU
    (main-task `save()` zeroed sizes while the RX callback wrote); now a `portMUX` ping-pong swap
    (snapshot+flip under lock, drain outside). Also dropped the per-frame NVS read (`loadSetting`
    on every captured frame) — `append()` gates on `writing` instead.
  All marked `Clip-Boy local patch (DC34)`; re-apply on re-vendor.

## Not vendored

- **ESP32 board core** (`esp32:esp32` 2.0.10) and its bundled libraries (WiFi,
  FS, SD, SPI, LittleFS, Preferences, Wire, …) — these are part of the platform,
  installed via arduino-cli (see README). Pin this version for reproducibility.
- **libhelix** was removed entirely (replaced by minimp3) to drop its
  GPL-incompatible RPSL licensing.

## Updating a vendored library

Vendoring pins these versions. To update one, replace the upstream copy and
re-run `scripts/vendor_libs.sh`, then rebuild and re-test. (Re-apply the
LovyanGFX CJK-font trim if you re-vendor it — the script does this.)

- **`WiFiScan.cpp` `RunEvilPortal` + `EvilPortal.cpp` credential append** — *Clip-Boy local patch (audit 2026-07-24 SB4 / 2026-07-28 SB4b).* Both halves of the Evil Portal credential sink are compile-gated behind `CB_EVIL_PORTAL_LOG_SD` (unset by default): the `startLog("evil_portal")` file AND the `buffer_obj.append(line)` that would otherwise inherit an open pcap and flush harvested credentials to SD/LittleFS. **Re-apply on re-vendor.** Release gates `SB4` + `SB4b` fail the build if either half is dropped FROM THE FILE IT GUARDS (`WiFiScan.cpp` / `EvilPortal.cpp` respectively) -- so a lost patch breaks the build rather than silently shipping. ⚠ Scope honestly: neither gate would catch the credential write being MOVED to a different translation unit. They are regression guards on two known sites, not a proof that no credential reaches storage.

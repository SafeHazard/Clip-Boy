# Drone Remote ID detector

Receive-only detector for the **Remote ID** broadcasts that 14 CFR Part 89
requires drones to transmit in the clear: UAS serial, live position, altitude,
speed, and operator location. The format is ASTM F3411 / Open Drone ID.

Two radio paths carry it:

- **Bluetooth LE** — service data, 16-bit UUID `0xFFFA`, payload
  `[0x0D app-code][counter][ODID message or message-pack]`.
- **WiFi** — beacon-frame vendor IE (OUI `FA:0B:BC`, type `0x0D`, then
  `[counter][ODID message/pack]`), plus NAN action frames. DJI and most
  consumer drones use the WiFi beacon path.

Nothing here transmits. Decoding Remote ID is exactly what the broadcast is for.

## Hardware

The Clip-Boy and the DEF CON 33 SpaceBadge are the **same board**: Waveshare
ESP32-S3-Touch-LCD-2.8 (ESP32-S3, 16MB flash, OPI PSRAM, ST7789 240x320 IPS via
LovyanGFX, CST328 touch on I2C `Wire1`, LVGL 9.2, NimBLE 2.x).

Flash with esptool over USB-C. Download mode is **hold BOOT, tap RESET, release
BOOT**. The app image lives at `0x10000`, and app-only flashing preserves the
LittleFS assets:

```
esptool --chip esp32s3 -p <PORT> write_flash 0x10000 <app.bin>
```

## What lives where

### The Clip-Boy tool — Tools -> Detect -> "Drone ID"

Scan mode `BT_SCAN_REMOTE_ID` (79) in the `-DCLIPBOY_RES34RCH` build. Source is
in the sketch tree, not this directory:

- `Clip-Boy/libs/ClipBoy/src/drone_store.{h,cpp}` — walks a raw BLE
  advertisement (`drone_ingest_ble`) or takes ODID bytes directly
  (`drone_ingest_odid`, for the WiFi path), decodes, and keeps a fixed 16-slot
  contact table keyed by UAS serial. No LVGL / Marauder / NimBLE deps.
- `Clip-Boy/libs/ClipBoy/src/{opendroneid.h,odid_decode.c}` — the opendroneid
  reference decoder, Apache-2.0.
- `Clip-Boy/libs/ClipBoy/src/WiFiScan.{h,cpp}` — scan lifecycle, mirrored from
  `BT_SCAN_FLOCK`, plus the BLE advertised-device branch.
- `Clip-Boy/libs/ClipBoy/src/ClipBoyMarauder.{h,cpp}` — `btScanRemoteID()`.
- `Clip-Boy/{ui_nav.h,tool_info.h}` — menu item, live-log poller, `"%d drones"`
  status branch, More-Info entry, and **Utilities -> List Drones**: a row per
  contact, tap for a panel with position, MSL/AGL altitude, speed and heading,
  the operator's broadcast location, registration, description, MAC, link and
  age. Rows carry the real slot index, never their list position, because the
  builder skips free slots and the two diverge as soon as a contact ages out.

  The tool is named "Drone ID" in the UI; "Remote ID" is the standard's name
  and stays in the help text. Note `tool_is_bluetooth` matches tools by NAME,
  so a rename that misses it silently drops the tool from the airplane-mode
  dialog.

  The live log prints a contact once on sighting and once more when it gains a
  position or altitude, the follow-up marked `DRONE+`. It does not stream every
  update; the full record is a tap away. Slots are tagged with a hash of the ID
  they were logged under, so a recycled slot is announced as the new contact it
  now holds rather than inheriting the old one's "already logged" flag.

**Status: WiFi and Bluetooth LE. Builds clean, BLE verified on this hardware
2026-08-20, WiFi path not yet tested on hardware.** The WiFi path was ported
from DroneWatch, its byte offsets are covered by `tests/`, and it compiles and
links into the image (7,039,657 bytes, 74% of app flash, 34% static RAM, no
warnings), but no ODID frame has ever reached it.

The WiFi side works like this. `StartScan` sends `BT_SCAN_REMOTE_ID` through
`RunProbeScan`, the same promiscuous setup `BT_SCAN_FLOCK` uses, so the tool
runs the WiFi sniffer and the BLE scan together. `beaconSnifferCallback` gets a
`BT_SCAN_REMOTE_ID` branch that trims the FCS off `sig_len` and hands beacons
(`0x80`) and action frames (`0xD0`) to `rid_sniff_beacon` / `rid_sniff_nan_action`.
Those walk the frame for the ODID element and copy the bytes into a small ring,
because the callback runs on the WiFi task and `drone_ingest_odid()` puts a ~1KB
`ODID_UAS_Data` on the stack. `WiFiScan::main()` drains the ring through the
decoder on the Arduino task. `channelHop()` was already wired for this mode.

`WIFI_SCAN_WAR_DRIVE` turned out to be the wrong model to copy despite the
handoff note: it wardrives through `WiFi.scanNetworks` and returns early unless
a GPS module is present. `RunProbeScan` is the promiscuous path.

### `standalone-dronewatch/` — the reference implementation

A lean standalone firmware ("DRONE WATCH") that boots straight into a detector
UI and does **both WiFi and BLE**. Hardware-tested: detects the BLE sim, both
radios come up (status shows `W B`).

- `rid_scan.{h,cpp}` — the engine. WiFi promiscuous sniffer (`sniff_beacon`,
  `sniff_nan_action`), NimBLE scan, ODID decode, drone table, and a radio
  **time-slicer**, because one 2.4GHz antenna cannot saturate both paths.
- `rid_ui.{h,cpp}` — LVGL detector UI, contact list plus tap-for-detail.
- `Display_*`, `Touch_CST328`, `PWR_Key` — drivers copied from the badge.
- `src/odid/` — its own copy of the opendroneid decoder.

Built with ESP32 core **2.0.17**:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none" \
  --libraries <dir containing lvgl, LovyanGFX, NimBLE-Arduino, lv_conf.h> \
  --build-property "build.partitions=partitions" \
  --build-property "upload.maximum_size=3145728" \
  --build-path build  standalone-dronewatch/DroneDetector.ino
```

The `--libraries` dir is the SpaceBadge repo's `src/libraries`. `lv_conf.h` must
sit two levels above `lvgl/src` so LVGL's `../../lv_conf.h` include resolves.

### `windows-sim/` — a BLE drone simulator for Windows

`rid_sim_windows.py` broadcasts a fake drone ("SPACEBADGE-SIM-0001") over
Bluetooth LE using the Windows `winsdk` advertising API. `pip install winsdk`,
then `python rid_sim_windows.py`. The message bytes were generated by
`encode_messages.c` (links opendroneid) and are baked into the script.

There is **no Windows WiFi simulator**. Windows cannot inject 802.11 beacon
frames: NDIS driver limitation, and AirPcap is EOL. Simulating the WiFi path
needs an ESP32 transmitter, which does not exist yet.

## Building the Clip-Boy firmware

Toolchain is **ESP32 core 2.0.10 only**. Marauder is pinned to it, and a
different core changes byte layout and can break the link. arduino-cli's FQBN is
unversioned, so only one esp32 core may be installed at a time. Install the
right one before each build, since DroneWatch wants 2.0.17 and this wants
2.0.10.

Codegen first, from the sketch dir (`Clip-Boy/`, the folder with `Clip-Boy.ino`):

```
printf '#define CB_BUILD_STAMP "dev"\n' > build_stamp.h
[ -f secret.h ] || cp secret.h.example secret.h
python3 scripts/embed_csv.py
mkdir -p /tmp/lfsstage
python3 scripts/embed_audio.py "$PWD/audio" "$PWD/radio_audio_gen" 290000 /tmp/lfsstage
```

Then compile, with the linker wraps Marauder needs:

```
WRAPS="-Wl,--allow-multiple-definition"
for w in esp_wifi_init esp_wifi_set_country esp_wifi_set_mode esp_wifi_set_ps \
         esp_wifi_set_storage esp_wifi_start esp_wifi_deinit esp_wifi_restore \
         esp_wifi_stop esp_netif_deinit; do WRAPS="$WRAPS -Wl,--wrap=$w"; done

arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=default" \
  --libraries libs \
  --build-property "build.partitions=partitions" \
  --build-property "upload.maximum_size=9437184" \
  --build-property "compiler.cpp.extra_flags=-DCLIPBOY_RES34RCH" \
  --build-property "compiler.c.extra_flags=-DCLIPBOY_RES34RCH" \
  --build-property "compiler.c.elf.extra_flags=$WRAPS" \
  --build-path build  Clip-Boy.ino
```

Expect roughly 74% app flash and 34% static RAM, clean. The image is
`build/Clip-Boy.ino.bin`. Unsigned dev builds boot fine, there is no hardware
secure boot, so flash with esptool and skip the web flasher's signature check.

Sanity check that it linked:

```
xtensa-esp32s3-elf-nm build/Clip-Boy.ino.elf | grep -E "drone_ingest_odid|drone_count|btScanRemoteID|rid_wifi"
```

## Gotchas learned the hard way

- **BLE controller RAM.** The badge firmwares push most allocations into
  internal DRAM, so initializing NimBLE *late* fails with
  `BLE_INIT: Malloc failed`. DroneWatch fixes this by bringing the BLE
  controller up at boot (`rid_earlyInit`). On the Clip-Boy, NimBLE is already up
  at boot (`WiFiScan::RunSetup`), so no early init is needed there.
- **WiFi + BLE coexistence.** One antenna. Do not expect both flat out at once.
  Time-slice like DroneWatch does, or lean on the host firmware's existing scan
  cycling (Clip-Boy's `WiFiScan::main` cycles BLE about every second).
- **Core versions.** See above. This is the single easiest way to waste an hour.

## Hardware log

**2026-08-20, public Res34rch build (no `--rift`), Waveshare ESP32-S3-Touch-LCD-2.8.**
Built with core 2.0.10, flashed app-only to `0x10000`, hash verified, badge boots.
Ran `windows-sim/rid_sim_windows.py`; contacts appeared in Tools -> Detect ->
Remote ID carrying the `BT` source tag. That confirms the decoder, the contact
table, the BLE ingest branch, and the changed UI on real hardware. The WiFi
beacon and NAN paths are still unexercised.

Two things worth knowing next time:

- The BLE simulator produces several contacts, not one, and most of them have
  fields missing. This is worth understanding before you go bug-hunting.

  The simulator cycles five messages: Basic ID, Location, System, Operator ID,
  Self ID. Only Basic ID carries the UAS serial and only System carries the
  pilot's location. Windows rotates its BLE MAC between advertisements, so a
  message that matches neither a known serial nor a known MAC opens its own
  record. In practice one row gets the serial, another gets position and
  altitude and speed (they share the Location message, which is why they always
  appear together), and roughly one row in five gets PILOT. The rest read
  "unknown".

  A real drone does not behave this way. Remote ID broadcasters are meant to
  hold a stable MAC for the session precisely so a receiver can correlate the
  messages, and a WiFi beacon carries all five in a single frame, which is why
  the WiFi path should show one clean, fully populated contact.

  Do NOT be tempted to merge anonymous messages into an existing record on
  anything looser than a serial or MAC match to "fix" this. It would eventually
  fuse two real drones into one contact, and inventing a drone that is not there
  is a far worse failure than an incomplete row.
- The ESP32-S3 USB-Serial/JTAG bridge cannot sustain a long `read-flash`. It
  stalls at roughly 4MB cumulative, in one continuous read and across separate
  1MB reads alike, while 64KB reads work anywhere in flash. A full 16MB backup
  over this bridge did not work; the signed bins listed in `release/SHA256SUMS`
  are the practical restore path.

## Tests

`tests/test_ie_walk.py` mirrors the two IE walkers in Python and runs them
against 802.11 frames assembled from the layouts, so a drifted offset fails
instead of quietly decoding garbage. It also greps `WiFiScan.cpp` for the magic
constants, so changing them there without changing the test is caught.

```
python drone-remoteid/tests/test_ie_walk.py
```

This is not a substitute for flying a drone at the badge. It only checks the
part that can be checked without hardware.

## What's left

Hardware testing of the WiFi path is the next step: build, flash, and fly
something Remote ID compliant at it. Until that happens the WiFi path is
written but unproven.

Deliberately out of scope for this repo:

- **A WiFi transmitter simulator.** Windows cannot inject 802.11 beacons, so
  exercising the WiFi path without a real drone needs an ESP32 running
  opendroneid's `odid_wifi_build_message_pack_beacon_frame()` through
  `esp_wifi_80211_tx()`. That is a transmitter, not a detector, and belongs in
  its own project rather than inside the badge firmware.
- **SD logging, a north-up radar plot, and a GPS module** for range and
  bearing. Parked. The drone already broadcasts its own absolute position, so
  GPS would only add *your* position for the relative math.

## Licensing

- opendroneid-core-c decoder: Apache-2.0, see
  `standalone-dronewatch/src/odid/LICENSE.opendroneid`.
- Clip-Boy firmware: GPLv3, so these changes are GPLv3 and PR-able upstream to
  SafeHazard/Clip-Boy.
- SpaceBadge (source of the standalone drivers): MIT.
- NimBLE-Arduino, LVGL, LovyanGFX: their own permissive licenses.

Upstream repos: [Clip-Boy](https://github.com/SafeHazard/Clip-Boy),
[SpaceBadge](https://github.com/SafeHazard/SpaceBadge),
[opendroneid](https://github.com/opendroneid/opendroneid-core-c).

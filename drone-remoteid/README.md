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

Flash with esptool over USB-C. On a bare Waveshare board download mode is **hold
BOOT, tap RESET, release BOOT**. In the SpaceBadge enclosure there is no button
called RESET: the three, ordered from the front (farthest from the USB port),
are **Bootloader**, **Power off** and **Power on**. On USB power, Power off
reboots rather than shuts down, so it plays the part of RESET. If the badge is
off, hold Bootloader and then hold Power on for ~2 seconds instead. A badge that
is off does not enumerate at all, because this board uses the ESP32-S3 native
USB rather than a separate serial chip, so "no COM port" usually means unpowered
or a charge-only cable rather than a failed bootloader entry.

esptool can also reset a *running* badge into download mode by itself, so in
practice the button dance is rarely needed.

The app image lives at `0x10000`, and app-only flashing preserves the
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

**Status: WiFi and Bluetooth LE, both verified on this hardware.** BLE on
2026-08-20 against the Windows simulator, WiFi on 2026-08-21 against
`standalone-dronesim`. The image is 7,039,657 bytes, 74% of app flash, 34%
static RAM, no warnings.

The WiFi run was made unambiguous by turning the simulator BLE path off first,
so the only radio in play was WiFi. The tool raised a contact carrying serial
`SPACEBADGE-SIM-0001`, a moving position, a stationary operator location, and a
link line reading `WiFi ch6`. That last field is the proof: it comes from
`d->channel`, which is only ever non-zero on the WiFi ingest path, so a stale
BLE contact cannot produce it.

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
needs an ESP32 transmitter, which is what `standalone-dronesim/` is.

### `standalone-dronesim/` — a WiFi and BLE drone simulator on a second badge

A standalone firmware that transmits one simulated drone over all three
transports at once: WiFi beacon vendor IE, WiFi NAN action frame, and BLE
service data. It replaces the badge OS entirely, so any spare board of the same
type works. A DEF CON 33 SpaceBadge is the obvious donor, being the same
Waveshare ESP32-S3-Touch-LCD-2.8 hardware as the Clip-Boy.

This exists because the WiFi receive path had no way to be tested. Point it at
a Clip-Boy running Tools -> Detect -> Drone ID and the tool should raise a
contact tagged `WiFi`.

- `rid_tx.{h,cpp}` — encodes the drone into ASTM F3411 messages and puts them on
  the air. The WiFi frames go out through `esp_wifi_80211_tx`, which requires
  overriding `ieee80211_raw_frame_sanity_check` to return 0. That symbol already
  exists in `libnet80211.a` and is not weak, so the build needs
  `-Wl,--allow-multiple-definition` and relies on the sketch object linking
  first. `--wrap` does **not** work here: the SDK calls the function from inside
  the same object that defines it, so a wrap would not catch those calls.
- `sim_flight.{h,cpp}` — the drone itself, flying a 150 m circle with a slow
  altitude wander, so a receiver has something changing to track. The pilot
  stands about 800 m away from the orbit, **not** at its centre. That is
  deliberate: with the operator at the centre of a small circle the two
  locations agree to four decimal places, so a receiver that wrongly printed the
  drone position in the pilot field would look perfectly correct. Keeping them a
  kilometre apart makes that class of bug impossible to miss.
- `sim_ui.{h,cpp}` — the on-screen status page. Not decoration: see the gotcha
  about this board having no dependable serial console.
- `src/odid/` — the same opendroneid copy DroneWatch uses, this time for the
  encode half.

The five messages (Basic ID, Location, System, Self ID, Operator ID) go out as
one message pack over WiFi, and one message per advertisement over BLE, rotating
through them. That split is forced: BLE legacy advertising carries 31 bytes, and
a single 25 byte ODID message plus the service data header fills it exactly.
There is no room for a flags structure, which is why the code builds the
advertisement by hand instead of letting NimBLE assemble it.

Drive it over serial at 115200 (`?` for help), or just power it on: it starts
transmitting at boot so it is useful with no console attached.

Built with ESP32 core **2.0.10**, the same core the Clip-Boy needs, so you can
build both without swapping cores:

```
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none" \
  --libraries ../Clip-Boy/libs \
  --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" \
  --build-path build  standalone-dronesim
```

Roughly 26% of app flash and 15% static RAM. Note the sketch is passed as a
**directory**: Arduino requires the main `.ino` to be named after its folder,
which is why the file is `standalone-dronesim.ino` rather than something
prettier.

**Status: running on hardware 2026-08-21.** Boots, holds up, and transmits on
all three paths with zero WiFi or encode errors: counters climb steadily and the
simulated drone moves. See the hardware log at the end for what it took to get
there. The frame layouts are
covered by `tests/test_tx_frames.py`, which is a byte-level round trip rather
than an inspection: it mirrors the three frame builders, feeds their output
through mirrors of the receiver walkers in `WiFiScan.cpp`, and checks the ODID
payload and source MAC come back unchanged. It also cross-checks that the
constants on both sides still match and that a full nine message pack cannot
overflow the 8 bit IE length or the receiver ring slot.

```
python drone-remoteid/tests/test_ie_walk.py     # receiver offsets, 22 checks
python drone-remoteid/tests/test_tx_frames.py   # TX to RX round trip, 29 checks
```

Both share the receiver mirrors in `tests/rid_mirror.py`, so a drift in the C is
caught in one place rather than fixed in one copy and left stale in the other.

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
- **The simulator broadcasts its own AP beacons too.** `esp_wifi_80211_tx` only
  radiates on `WIFI_IF_AP` when the AP interface is actually up, so the sim
  runs a hidden SoftAP purely to satisfy that. It still emits ordinary beacons
  of its own alongside the forged Remote ID ones. In a sniffer they look almost
  identical, same SSID and same MAC; the Remote ID frames are the ones carrying
  the `FA:0B:BC` vendor IE.
- **Never disable WiFi power save while BLE is up.** `esp_wifi_set_ps(WIFI_PS_NONE)`
  looks harmless and is not. With the BLE controller running, coexistence is
  active, and the call gets queued to the WiFi `ppTask` which then **aborts** in
  `pm_set_sleep_type`. The board boot-loops with
  `Error! Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled`.
  It does not fail gracefully and it does not complain at the call site, so the
  backtrace points at a task you never wrote. Use `WIFI_PS_MIN_MODEM`. This cost
  an afternoon, and the tempting instinct that a transmitter wants its radio
  awake is exactly what causes it.
- **This board has no dependable boot indicator except the screen.** A firmware
  with no display looks identical to one that never reached `setup()`, and that
  is how a boot loop hid for a full build-flash cycle. Put something on the LCD
  early, and show failures there too: the simulator prints `FAILED` with a reason
  rather than sitting dark.
- **Reading the USB console needs care.** `Serial` does surface on the native USB
  (COM port, `CDCOnBoot=cdc`, and `hwcdc` is the default `USBMode` since it is
  first in `boards.txt`), but a line-oriented read returns nothing useful and
  pyserial asserts DTR/RTS on open, which is the auto-reset signal. Open with
  `dtr = False` and `rts = False`, then read raw blocks. Done that way the full
  panic backtrace is right there, and
  `xtensa-esp32s3-elf-addr2line -pfiaC -e <elf> <addrs>` names the failing
  function in seconds.

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

**2026-08-21, simulator first light, SpaceBadge donor board (ESP32-S3 rev v0.2,
16MB flash, 8MB PSRAM, MAC `94:a9:90:17:70:d0`).** Flashed app-only to `0x10000`,
hash verified. The SpaceBadge partition table was still in place, since DroneWatch
had also been flashed app-only: `app0` at `0x10000` sized `0x2f0000`, `otadata`
selecting `ota_0`, and `spiffs` untouched. So the badge data survives and
restoring the stock firmware is one `write_flash` of a binary from the SpaceBadge
repo.

Two failures on the way, both recorded as gotchas above:

- The first build had no display, and it boot-looped. With nothing on the LCD and
  a serial console that appeared dead, a crashing firmware and a firmware that
  never started were indistinguishable. Adding the status page turned this from
  guesswork into a two-minute diagnosis.
- The loop itself was `esp_wifi_set_ps(WIFI_PS_NONE)` aborting under BLE
  coexistence. WiFi and the SoftAP came up fine; the abort landed later, on the
  WiFi task, which is why the backtrace named `pm_set_sleep_type` and `ppTask`
  rather than anything in this repo.

After the fix: no reboots, no aborts, and counters climbing steadily with zero
WiFi and zero encode errors. The on-air source address is `96:A9:90:17:70:D0`,
the AP MAC, which is the USB device MAC with the locally-administered bit set.

**2026-08-21, WiFi receive path proven.** With the simulator BLE path disabled,
so WiFi was the only radio transmitting, the Clip-Boy raised a contact in
Tools -> Detect -> Drone ID showing serial `SPACEBADGE-SIM-0001`, a position
that tracked the simulated flight, an operator location that stayed put, and
`WiFi ch6` on the link line. `d->channel` is set only by `drone_ingest_odid()`
on the WiFi path, so that field is what makes the result airtight rather than a
BLE contact left over from an earlier run.

This closes the gap opened by `267cdad`, which added the WiFi receive path in
code and left it untested for want of anything that could transmit to it.

The simulator also caught a false alarm worth recording. The Clip-Boy detail
panel appeared to show the same coordinates for POS and PILOT. The panel was
correct: the original simulator put the operator at the centre of a 150 m orbit,
so the two agreed to four decimal places. Moving the pilot 800 m away made them
plainly distinct and confirmed the operator field decodes properly. A simulator
whose values are too similar to tell apart cannot verify anything.

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

The WiFi receive path now decodes a real frame on real hardware, fed by the
transmitter in `standalone-dronesim/`. What it has never seen is an actual
drone: a commercial aircraft may order its messages differently, split them
across frames rather than packing them, or lean on NAN where we lean on
beacons. Flying something Remote ID compliant at it is still worth doing.

Still open:

- **More than a status page on the simulator.** It transmits one hardcoded
  drone. Selectable flight profiles, several simultaneous aircraft, and touch
  controls to start and stop would make it a better test rig.
- **Contact fragmentation on the BLE path.** `table_find()` matches on serial
  first and MAC second, and rewrites the stored MAC on every ingest. BLE sends
  one message per advertisement and only one of five carries the serial, so the
  others can open a second record keyed by the BLE address. Running the
  simulator with both radios on reproduces this on demand, which is the first
  time it has been reproducible at all.
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

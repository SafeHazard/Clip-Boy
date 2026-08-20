#pragma once
// tool_info.h - Plain-English "More Info" descriptions for each Clip-Boy tool.
//
// Each ToolInfo has four short fields used to build the collapsible info
// panel at the bottom of the tool's detail view. Any NULL field is omitted
// from the rendered output so tools with nothing meaningful in a category
// (e.g. a passive scan with no prerequisites) don't show an empty header.
//
// Tone guide: plain English, accurate, slightly wry is fine. Prioritize the
// responsible-use angle in "avoid" - DEFCON audience is a mix of curious
// first-timers and veteran pen-testers, so the text has to respect both.
//
// Conventions baked into the audit:
//   - Lead "what" with an accurate active tag - "ACTIVE - DISRUPTIVE.",
//     "ACTIVE - TRANSMITS.", or "ACTIVE - PHISHING." - for any tool that
//     transmits frames. Describe what it DOES (denial-of-service, spectrum
//     noise, credential capture); state the technique plainly rather than
//     framing it as an aggressive act or imputing someone harmed. Passive
//     listeners get no active tag even when their *output* could be misused.
//   - Name the device a tool acts on by how it was chosen: "selected" when
//     the user picked it, "discovered"/"target" when the tool acts on all
//     it finds.
//   - Inline responsibility guidance instead of cross-referencing another
//     entry ("Same as X") - cross-refs rot when sibling entries change.
//   - "avoid" can be NULL only when there is genuinely no responsibility
//     consideration (rate counters, list views, local-state operations).
//
// TAT_LIST_VIEW tools intentionally skip the More Info panel (see
// ui_nav.h append_tool_info_section call site), so no entries exist for
// utility list views.

struct ToolInfo {
    const char *what;      // What the tool does (always required)
    const char *requires;  // Prerequisites / setup (nullable)
    const char *effects;   // What users will see/hear/feel (nullable)
    const char *avoid;     // Responsible-use guidance (nullable)
};

// Shown automatically (by action type, from append_tool_info_section) on every tool that
// displays or targets from the shared AP/station lists. Kept as ONE string so the wording
// cannot drift across the ~15 tools it applies to, and so tools added later inherit it.
//
// Wording rule: state only what a user may OBSERVE. No mechanism, no cause, no attribution --
// this text ships to end users and appears alongside the on-badge Help note and the README
// Known Issues entry, which say the same thing at increasing length.
static const char *TOOL_KNOWN_LIST_NOTE =
    "While a scan is still adding results, a row may briefly show blank or partial details, "
    "and an action aimed at a selected device may skip it for one pass. Stopping the scan "
    "before selecting a target avoids this.";

// Second by-rule note, same wording discipline: OBSERVABLE FACTS ONLY, no mechanism.
// Device lists are CUMULATIVE -- they answer "what did I see" rather than "what is here now",
// which is the right model for a survey tool but is not self-evident from a list that only
// ever grows. Requested explicitly by the owner (2026-07-26) after a loss-of-signal run showed
// counts holding steady 30 s after the target stopped transmitting: the behaviour is correct and
// documented, so the fix is to SAY so on the tool that shows the list, not to change the list.
// Rendered through the same (cat,item) rule as the note above so ~15 tools share one string and
// tools added later inherit it.
static const char *TOOL_CUMULATIVE_LIST_NOTE =
    "Device lists show everything seen since you tapped START -- not only what is in range now. "
    "A device that has left keeps its last reading, and counts do not fall when it goes away. "
    // "Utilities/Lists", not "Utilities" -- that is the live category name (tool_categories[]), and
    // check_help.py FAILS the build on a tutorial path that cites a category which does not exist.
    // It caught exactly this in the sibling Help entry; keeping both spellings identical.
    "For a fresh picture, Stop and start a new scan, or use Utilities/Lists > Clear All. "
    // Added 2026-07-26 with the eviction change, CORRECTED 2026-07-27. Worth stating because a
    // list that silently stops growing is indistinguishable from a quiet room.
    // ⚠ The two lists do NOT behave the same, and the original wording claimed evict-oldest for
    // both. For Bluetooth the binding limit is NimBLE's own `setMaxResults(50)` (set in
    // WiFiScan::RunBluetoothScan -- cited by symbol because line numbers in that file rot every
    // time someone adds a comment), enforced BEFORE our callback runs, and its policy is
    // drop-new -- so our evict-oldest
    // branch is unreachable in that mode and the honest description is first-50-per-scan. Only the
    // Pwnagotchi list actually evicts. Describe each as it behaves, not as the code we wrote
    // intended: an over-promise here reads as a bug report from the user's side.
    "The Bluetooth scan reports about the first 50 distinct devices per run -- one that turns up "
    "later will not appear until you restart the scan, so treat a crowded-room list as a sample. "
    "The Pwnagotchi list holds a hundred and drops its longest-unseen entry for a new arrival. "
    // Flock has its own first-party cap, and THIS note is what renders on the Flock tool page
    // (Detect is category id 0) -- the Help screen is somewhere else. Omitting it meant the one
    // note a user sees while running the detector described two other lists and not that one.
    // ⚠ "devices", not "cameras": what this tool matches is the BATTERY PACK accessory, which is
    // the whole reason the tool was renamed. Saying "cameras" here re-asserts the misconception
    // the rename exists to kill, on the very page the user is looking at.
    "Detect > Flock Batteries keeps up to 50 devices per run.";

// Lookup is by (cat, item). Only populated entries override the default
// "no details yet" fallback shown in the UI.
struct ToolInfoEntry {
    uint8_t cat;
    uint8_t item;
    ToolInfo info;
};

// NOTE (Jun 2026): re-keyed to the DETECT-LED taxonomy. (cat,item) are the
// stable category id + item index from ui_nav.h tool_categories[]. Prose is
// preserved from the prior layout; only the (id,item) keys moved.
static const ToolInfoEntry tool_info_table[] = {
    // ─── Detect (id 0): items 0-3 BT detect, 4-5 WiFi rogue/evil-twin ──────
    { 0, 0, {
        "Continuous AirTag detection with alerting. Filters the BLE stream to "
        "AirTag's advertising pattern. Only sees tags advertising publicly "
        "(lost-mode or separated from their owner); tags actively paired to a "
        "phone in range mostly stay silent.",
        "Bluetooth antenna enabled.",
        "Output lists AirTags with persistence indicators. A tag that appears "
        "repeatedly as you move is the worrying pattern.",
        "If you find a tag that isn't yours, use Apple's official 'Find Nearby "
        "Item' tooling and contact authorities if needed. Don't confront a "
        "stranger over a stray detection."
    }},
    { 0, 1, {
        "Heuristic check for BLE devices that match historically-known "
        "credit-card-skimmer module names (HC-03/05/06 and similar). Modern "
        "skimmers don't use these defaults, and the same module names show up "
        "on hobby projects, fitness gear, and old IoT devices. Treat hits as "
        "suspicious, not as evidence.",
        "Bluetooth antenna enabled.",
        "Output flags BLE devices whose advertised name matches the heuristic "
        "list. False positives are common - a hit at a gas pump is a reason to "
        "pay inside or use tap, not a reason to call anyone.",
        "Don't act on a hit beyond your own personal-safety choice. The "
        "signatures are stale and the false-positive rate is high enough that "
        "calling authorities or confronting staff is far more likely to cause "
        "trouble than catch a real skimmer."
    }},
    { 0, 2, {
        "Detects Flipper Zero devices by their characteristic BLE advertising "
        "pattern. Handy for finding other hackers at a con.",
        "Bluetooth antenna enabled.",
        "Output lists Flipper MACs and advertised names.",
        NULL
    }},
    { 0, 3, {
        "Listens for the Bluetooth signature used by Flock Safety equipment -- in practice the "
        "battery pack fitted to SOME of their units, not the camera itself. Cameras running on "
        "solar usually transmit no Bluetooth at all and will not appear here. Flock's "
        "license-plate readers are deployed by some municipal police and HOAs; this is a passive "
        "listener. A hit means Flock hardware is nearby. A quiet screen means nothing matched -- "
        "not that nothing is there.",
        "Bluetooth antenna enabled.",
        "Output lists each detected Flock device once, with its MAC and the signal strength when "
        "the badge first logged it. The status bar shows how long ago it last heard one -- that "
        "number resets every time it hears the device again. A number that keeps climbing means "
        "the badge has stopped hearing the device, not that the badge has stopped working; short "
        "gaps of a few seconds are normal. It matches one specific broadcast, so a quiet screen "
        // ⚠ Path CORRECTED 2026-07-27: this said "Help > Detect Tools > Flock Safety". There is
        // no "Detect Tools" Help category (the category is "Tools"), so the one page that tells
        // the whole truth was unreachable from the pointer to it. check_help.py lints tutorial
        // paths in ui_nav.h's Help arrays but only denylist-scans tool_info.h, so the gate could
        // not see it -- which is exactly why the comment near the top of this file claiming the
        // gate catches bad category paths is true of Help copy and NOT of this file.
        "is not proof anything is absent -- see Help > Tools > Flock Batteries.",
        "Detection only. Don't interfere with or damage any infrastructure - "
        "tampering is a serious crime and won't help your case. And don't treat a quiet screen "
        "as an all-clear."
    }},
    { 0, 4, {
        "Flags APs that respond to probe requests for SSIDs they shouldn't know - "
        "the behavior a WiFi Pineapple exhibits when it's set to 'be every network'. "
        "Some legitimate captive portal hotspots also probe-answer, so a hit is "
        "suspicious but not proof.",
        "WiFi antenna enabled.",
        "Output flags suspect APs by BSSID. Success criteria: the same BSSID "
        "answering probes for two or more unrelated SSIDs is the strong signal.",
        "Detection only, no action taken. If you want to report a suspected "
        "device, tell venue staff in private rather than calling it out publicly."
    }},
    { 0, 5, {
        "Flags duplicate SSIDs across multiple BSSIDs and multi-SSID broadcasters: "
        "one BSSID broadcasting many SSIDs, or many APs claiming the same SSID. "
        "The latter is what an evil-twin looks like, but it's also what corporate "
        "guest+staff splits look like at this layer.",
        "WiFi antenna enabled. Works best in dense networks where duplicates "
        "stand out.",
        "Output lists the SSIDs and the BSSIDs broadcasting them. Success "
        "criteria for evil-twin suspicion: same SSID, different vendor OUIs, "
        "wildly different RSSI - then walk the floor with RSSI Monitor on the "
        "outlier to see if it physically moves.",
        "Detection only. False positives are common at scale, so don't accuse "
        "anyone in public based on a single hit - report to venue staff if needed."
    }},
    { 0, 6, {
        "Listens for drone Remote ID broadcasts (ASTM F3411 / Open Drone ID) over "
        "WiFi and Bluetooth. US federal law requires most drones to broadcast their "
        "serial, live position, altitude, and the operator's location in the clear - "
        "this is a passive receiver for exactly that public broadcast. A hit means a "
        "compliant drone is transmitting nearby.",
        "WiFi and Bluetooth antennas enabled.",
        "Output lists each drone by its UAS ID (serial) with altitude, signal "
        "strength, and where it was heard - a WiFi channel, or BT for Bluetooth. "
        "The status bar shows a live count. One antenna serves both radios and the "
        "WiFi side hops channels, so a drone can take a few seconds to appear. A "
        "quiet screen is not proof the sky is empty.",
        "Receive only - it transmits nothing. Reading Remote ID is what the "
        "broadcast is for; use what you see responsibly."
    }},

    // ─── Scan (id 1): 0-2 WiFi, 3 BT Devices, 4 BLE Adverts ───────────────
    { 1, 0, {
        "Passively listens for all nearby WiFi access points (routers, hotspots) "
        "and records their names, signal strength, and channel. Doesn't transmit "
        "anything - it's just eavesdropping on beacons that APs already broadcast.",
        "WiFi antenna enabled. Airplane mode disables this.",
        "The output log fills with AP entries - SSID, RSSI, channel, BSSID. "
        "Strong signals appear first. Scan runs until you stop it.",
        "Passive listening on broadcast management frames is legal almost "
        "everywhere. Aggregating SSIDs/BSSIDs into a published dataset can run "
        "into wiretap-style statutes in a few states (CA, FL among them). When "
        "in doubt, keep it on-device."
    }},
    { 1, 1, {
        "Like APs (full) but also identifies stations (phones, laptops) currently "
        "connected to each AP. Builds a map of who's talking to what.",
        "WiFi antenna enabled. Gets richer data in dense areas like conference floors.",
        "Output shows both APs and the stations (client MACs) associated with them.",
        "Tracking specific stations can leak personal movement patterns. "
        "Keep any logs you save local to your badge."
    }},
    { 1, 2, {
        "Listens only for stations - devices that are associated with or probing "
        "for WiFi networks. Useful when you want to see who's around without "
        "caring which AP they're on.",
        "WiFi antenna enabled.",
        "Output shows station MAC addresses and any AP they're currently bound to.",
        "Don't use this to single out specific people - at a con, at a coffee "
        "shop, anywhere. That's creepy regardless of venue."
    }},
    { 1, 3, {
        "Full Bluetooth + BLE scan. Builds a running list of every Bluetooth "
        "device the radio sees, classic and low-energy both. Passive, no "
        "pairing attempted.",
        "Bluetooth antenna enabled.",
        "Output lists device name (if advertised), MAC, and RSSI. Many devices "
        "use rotating MACs so the list grows faster than the actual device count.",
        "Passive discovery is generally safe, but aggregating BT MACs over time "
        "is a tracking vector. Don't save or share the output."
    }},
    { 1, 4, {
        "Counts BLE advertisement packets per second. No per-device detail - "
        "just the raw rate. Useful for a quick 'how many BLE-chatty things are "
        "around me' reading.",
        "Bluetooth antenna enabled.",
        "Output shows a single running counter. Quiet room ~10/s. Conference "
        "floor 500+/s.",
        NULL
    }},

    // ─── Monitor (id 2) ───────────────────────────────────────────────────
    { 2, 0, {
        "Live tally of the management frames the radio sees - beacons, deauths, "
        "and probe requests - shown as three bars. A quick read on how busy the air is.",
        "WiFi antenna enabled.",
        "Output shows counts for beacon / deauth / probe frames. A quiet room vs "
        "the DEFCON floor looks very different.",
        NULL
    }},
    { 2, 1, {
        "Rate of management frames (beacons, deauths, probes) per second, as a rolling "
        "graph. It is NOT tied to one AP or channel: the radio sweeps channels 1-14 at "
        "about one per second, so each point reflects whatever channel it was on then.",
        "WiFi antenna enabled.",
        "Output is frames/sec over roughly the last 24 seconds. Because it hops channels, "
        "the line is naturally bumpy; a high plateau sustained across the sweep means a "
        "busy or flooded area nearby.",
        NULL
    }},
    { 2, 2, {
        "Tracks signal strength (RSSI) of a selected AP over time. Useful for "
        "walking around to find the physical location of an AP, or to see how "
        "stable a connection would be.",
        "Select the AP first. WiFi antenna enabled.",
        "Output shows dBm readings as they change. Closer to the AP - higher (less "
        "negative) numbers. Around -30 dBm is right next to it; -90 is barely alive.",
        NULL
    }},
    { 2, 3, {
        "Live histogram of activity across WiFi channels 1 through 14. Shows where "
        "the RF noise is concentrated - useful for picking a quiet channel for "
        "your own AP, or spotting someone camping a single channel.",
        "WiFi antenna enabled.",
        "Output shows per-channel packet counts updating in real time. Most "
        "traffic clusters on 1/6/11 in the US; noise on 2/3/4/5 often means "
        "something misbehaving.",
        NULL
    }},
    { 2, 4, {
        "Ranks the loudest MAC addresses by how many frames each has sent - a live "
        "leaderboard of who's talking most on the air. No target entry needed.",
        "WiFi antenna enabled.",
        "Output lists the top talker MACs with their frame counts, refreshed every "
        "few seconds. The busiest radios (APs, active clients) rise to the top.",
        "Watching who is transmitting IS a form of presence tracking. Only use on "
        "hardware you own or devices whose owners have explicitly consented."
    }},

    // ─── Analyze (id 3): passive capture ──────────────────────────────────
    { 3, 0, {
        "Captures WiFi beacon frames - the periodic 'I exist' broadcasts every AP "
        "sends ~10 times per second. Passive and non-interactive.",
        "WiFi antenna enabled.",
        "Frame counter climbs steadily. Useful for understanding how "
        "saturated the local radio spectrum is.",
        NULL
    }},
    { 3, 1, {
        "Captures probe request frames - these are emitted by phones and laptops "
        "when they're searching for known networks. Each probe often includes an "
        "SSID the device has connected to in the past.",
        "WiFi antenna enabled.",
        "Output shows probing station MACs and the SSIDs they're searching for. "
        "This can reveal a person's home/work network names.",
        "Probe data is surprisingly personal - the SSID list is effectively a "
        "history of every place a device has connected. Treat it like location "
        "data: keep it local, don't aggregate or share it."
    }},
    { 3, 2, {
        "Listens for deauthentication frames - management frames that disconnect "
        "a station from an AP. Also drives the STATS > Radiation geiger display: "
        "the deauth rate maps to the geiger 'radiation level' so you can hear and "
        "see when something noisy is happening nearby.",
        "WiFi antenna enabled. 2.4 GHz only - deauths on a 5 GHz or 6 GHz network "
        "are invisible to this tool.",
        "Output shows src/dst MACs, channel, RSSI. Background deauth is common "
        "in dense RF environments; a sustained burst from one source is unusual. "
        "The Channel selector sets which channels are listened to, and it is SHARED "
        "with STATS > Radiation - changing it here changes it there. 1/6/11 (the "
        "default) covers the three channels nearly every 2.4 GHz network uses; 1-14 "
        "sweeps the whole band but sits on each channel only a fourteenth of the "
        "time, so short bursts are easy to miss; a single channel listens "
        "continuously to that one and hears nothing on the other thirteen. The mode "
        "is named in the status bar while a scan runs, whenever it is not 1-14.",
        "Pure listener, no transmission risk. Acting on what you find (e.g. "
        "publicly accusing a 'suspected bad actor') is a different judgment call - "
        "tell venue staff, not the internet."
    }},
    { 3, 3, {
        "Raw 802.11 packet capture to SD card (pcap). Grabs every frame the radio sees, not just "
        "management types. Use the Channel selector: 'Hop (all)' sweeps the band; lock a channel "
        "to focus one network. Much heavier on storage.",
        "WiFi antenna enabled. SD card inserted and mounted. REQUIRES "
        "DATA > Settings > Allow PCAP Saving = ON, or nothing is written.",
        "Output shows captured frame counts (BCN/PRB/DEA/EAP). Best-effort: under heavy traffic a "
        "burst can outrun the SD write and gap a few frames -- for reliable WPA handshakes use "
        "Analyze > EAPOL/PMKID (it locks to the target's channel). Analyze the pcap in Wireshark.",
        "Packet captures can contain identifying metadata even when payloads are "
        "encrypted. Don't share raw pcaps from public places without scrubbing."
    }},
    { 3, 4, {
        "Detects nearby Pwnagotchi devices by recognizing their characteristic "
        "probe and beacon signature. Pwnagotchis are automated WPA handshake "
        "collectors with an AI-themed personality layer.",
        "WiFi antenna enabled.",
        "Output lists detected Pwnagotchi MACs, names, and version (only if the "
        "Pwnagotchi advertises it - many forks strip the version field).",
        NULL
    }},
    { 3, 5, {
        "Identifies ESP8266 and ESP32 devices by their MAC address prefix (the "
        "first three bytes of a MAC, called the OUI, identify the chip vendor). "
        "Useful for counting badges and other ESP-based projects at a hacker con.",
        "WiFi antenna enabled.",
        "Output lists MACs whose OUI is registered to Espressif. Expect a lot of "
        "these at DEFCON.",
        NULL
    }},
    { 3, 6, {
        "Captures WPA3 SAE commit frames. SAE (Simultaneous Authentication of "
        "Equals) is WPA3's password-authenticated key exchange, based on Dragonfly. "
        "Commits are the first round and require an EC point validation per "
        "message. WPA3-specific variant of handshake sniffing.",
        "WiFi antenna enabled. Target must be WPA3-capable.",
        "Output shows SAE commit/confirm frames by station. Both commit AND "
        "confirm are needed to attempt offline password recovery; commit alone is "
        "partial.",
        "Same WPA-cracking caveat as EAPOL/PMKID: offline analysis only, and only "
        "on networks you own or are explicitly authorized to test."
    }},
    { 3, 7, {
        "Captures WPA/WPA2 4-way handshakes (EAPOL) or PMKIDs from a selected "
        "network. These can be used offline to attempt to recover the network's "
        "password by guessing (an offline dictionary run).",
        "Select the AP first (Utilities/Lists > Select AP). WiFi antenna "
        "enabled. SD card recommended for the pcap output. REQUIRES "
        "DATA > Settings > Allow PCAP Saving = ON, or nothing is written.",
        "Capture saved to the SD card (or internal storage) as /pcaps/<name>_<n>.pcap. You need to actually "
        "crack the file offline with hashcat or similar - the badge doesn't do "
        "the cracking. Success criteria: a complete EAPOL handshake needs all "
        "four messages (M1-M4) from the same MAC pair to be crackable; M1+M2 "
        "alone is a partial and won't crack. PMKID is opportunistic and can come "
        "from the AP alone, no station required. EAPOL needs a station to "
        "(re)authenticate during your capture window - on a network you own, "
        "briefly toggle a device's WiFi off/on yourself to force a reconnect.",
        "Only run against networks you own or have explicit written authorization "
        "to test. Cracking WPA on someone else's AP is a crime in most places."
    }},

#ifdef CLIPBOY_RES34RCH  // ─── ACTIVE RESEARCH More-Info tail (Res34rch-Boy only), ids 6-11 ───
    // ─── Deauth (id 6) ────────────────────────────────────────────────────
    { 6, 0, {
        "ACTIVE - DISRUPTIVE. Transmits spoofed deauthentication frames that "
        "disconnect every client from the selected access point(s). Clients try "
        "to reconnect, so this is a denial-of-service that recovers when you stop.",
        "Select the AP(s) first (Utilities/Lists > Select AP). WiFi antenna "
        "enabled. You must own or have written authorization to disrupt the "
        "network.",
        "Counter climbs showing deauth frames sent. On affected networks, phones "
        "and laptops drop WiFi and re-associate until you stop.",
        "Do NOT run against networks you don't own. Transmitting deauth frames "
        "against networks you aren't authorized to test is illegal under "
        "computer-misuse laws in the US, UK, EU, and most other places. "
        "Testing your own home AP is fine."
    }},
    { 6, 1, {
        "ACTIVE - DISRUPTIVE. Like Discovered, but you specify the exact AP and "
        "station pair. Precise, single-client denial-of-service.",
        "Select the AP first (Utilities/Lists > Select AP). WiFi antenna "
        "enabled. You must own or have authorization to disrupt the affected "
        "device.",
        "Output shows frame count sent to that specific client.",
        "Disrupting one client is more invasive than knocking down a whole AP - "
        "it cuts off a specific person's connection. Authorized test rigs "
        "and your own devices only."
    }},
    { 6, 2, {
        "ACTIVE - DISRUPTIVE. Deauth aimed at one or more specific STATIONS (client "
        "devices) regardless of which AP they're on. Disconnects the station from "
        "whatever it's currently using.",
        "Select the station(s) first. WiFi antenna enabled. Authorization "
        "required.",
        "Output shows per-station frame counts -- transmission, not delivery. If a "
        "station is tracked on a stale or wrong channel (it associated to an AP the "
        "badge last saw elsewhere), the frames go out on that channel and can "
        "silently miss. Confirm the disconnect independently.",
        "Stations are people's phones and laptops. Cutting off a specific one is "
        "the most personal use of this tool. Don't do it to anyone who hasn't "
        "agreed in advance."
    }},

    // ─── Beacon Spam (id 8): items 0-4 ────────────────────────────────────
    { 8, 0, {
        "ACTIVE - TRANSMITS. Broadcasts beacon frames with randomly-generated "
        "SSIDs. Fills nearby phones' WiFi scan lists with garbage. Doesn't connect "
        "to or disrupt anything - just noise.",
        "WiFi antenna enabled.",
        "On phones in range, the Available Networks list fills with random "
        "names that change every refresh. FPS drops while running (expected).",
        "Spectrum pollution -- annoying to nearby operators. Don't run it for "
        "extended periods in spaces where it would interfere with legitimate WiFi "
        "setup (venue networks, medical, etc.)."
    }},
    { 8, 1, {
        "ACTIVE - TRANSMITS. Broadcasts beacons with SSIDs drawn from your saved "
        "SSID list. Use Utilities/Lists > Add SSID to populate the list first.",
        "WiFi antenna enabled. SSID list must be populated (see Add SSID).",
        "Chosen SSIDs appear on nearby WiFi scan lists.",
        "Same spectrum-pollution caveat as Beacon Random. Using real or "
        "recognizable SSID names can confuse people and may qualify as spoofing "
        "in some jurisdictions - keep your list to obviously-fake names."
    }},
    { 8, 2, {
        "ACTIVE - TRANSMITS. Clones a real AP's SSID and BSSID into fake beacons. "
        "Creates apparent duplicates of legitimate networks at slightly different "
        "signal strengths.",
        "Select the AP first. WiFi antenna enabled.",
        "Phones see multiple 'copies' of the selected AP. Some phones may probe "
        "your fake beacons trying to decide which to use.",
        "This is evil-twin groundwork. Running it near a real network could "
        "confuse users into connecting to your radio. Only do this on networks "
        "you own or control."
    }},
    { 8, 3, {
        "ACTIVE - TRANSMITS. Broadcasts beacon frames with SSID names drawn from "
        "a certain song's lyrics. The names will sit in any nearby phone's WiFi "
        "scan cache for a while - clearable in WiFi settings if it bothers anyone.",
        "WiFi antenna enabled.",
        "Nearby phones see ~64 beacons named after that song. FPS drops (heavy).",
        "Spectrum pollution, same as Beacon Random. Keep it short - the joke "
        "wears off faster than the beacons do."
    }},
    { 8, 4, {
        "ACTIVE - TRANSMITS. Broadcasts beacon frames with humorous / pop-culture "
        "SSIDs (FBI Van, Get off my LAN, etc.). The harmless spiritual cousin of "
        "the classic meme.",
        "WiFi antenna enabled.",
        "Nearby phones see funny names. FPS drops (heavy).",
        "Annoying but usually harmless. Don't run it during a real emergency "
        "when venue staff need WiFi to coordinate."
    }},
    // ─── Flood (id 7): items 0-4 ──────────────────────────────────────────
    { 7, 0, {
        "ACTIVE - DISRUPTIVE. Floods a selected AP with authentication requests "
        "from spoofed MAC addresses. Can overwhelm the AP's association table and "
        "cause it to refuse new legitimate clients.",
        "Select the AP first (Utilities/Lists > Select AP). WiFi antenna "
        "enabled. Authorization required.",
        "Counter shows frames sent. Selected AP may slow down, drop clients, or "
        "stop accepting new associations entirely.",
        "This IS a denial-of-service against someone else's equipment. Don't run "
        "it against networks you don't own."
    }},
    { 7, 1, {
        "ACTIVE - DISRUPTIVE, BROADCAST. Transmits malformed WiFi management "
        "frames as broadcast - they reach every WiFi device in range, not just an "
        "intended recipient. Some older or poorly-written drivers mis-handle the "
        "frames and lock up or crash the WiFi stack.",
        "WiFi antenna enabled.",
        "Bystander warning: any device in range that has a vulnerable driver "
        "could see WiFi freeze, drop, or need a reboot - including phones, "
        "laptops, smart-home gear, and your own equipment. Counter shows frames "
        "sent.",
        "Broadcast crash bug, no targeting. Don't run it anywhere bystanders' "
        "devices could catch the broadcast (cons, coffee shops, your apartment "
        "building, etc.). In a pen-test lab where you've scoped every device on "
        "the air, fine."
    }},
    { 7, 2, {
        "ACTIVE - DISRUPTIVE. Like Bad Msg, but aimed at one or more "
        "specific stations rather than broadcast. Precise driver-confusion "
        "technique.",
        "Select the station(s) first. WiFi antenna enabled. Authorization "
        "required.",
        "Counter shows per-station frames sent -- transmission, not a confirmed hit. "
        "A station tracked on the wrong channel is missed silently; verify the effect "
        "with a separate sniffer.",
        "You are pointing a known crash bug at a specific person's device. "
        "Authorized hardware only - this can leave a phone in a state that "
        "needs a reboot to recover."
    }},
    { 7, 3, {
        "ACTIVE - DISRUPTIVE. Broadcasts power-save-poll frames addressed to every "
        "station in range. Stations that honor them believe they have queued "
        "data and stay awake, draining batteries.",
        "WiFi antenna enabled.",
        "Counter shows frames sent. No dramatic visible effect on targets - just "
        "unexpectedly short battery life until the flood stops. To verify it's "
        "actually working, sniff in another instance / Wireshark for the PS-Poll "
        "frames you're transmitting, or watch a known target's battery telemetry.",
        "Battery drain is a denial-of-service even when it's subtle, and the "
        "broadcast form hits everyone in range. Only on your own hardware."
    }},
    { 7, 4, {
        "ACTIVE - DISRUPTIVE. Sleep aimed at specific stations. Same battery-"
        "drain mechanism as the broadcast version, narrower target.",
        "Select the station(s) first. WiFi antenna enabled. Authorization "
        "required.",
        "Counter shows per-station frames sent -- transmission, not a confirmed hit. "
        "If a station is tracked on the wrong channel the drain frames miss silently; "
        "verify the battery effect or sniff independently.",
        "Targeted battery drain is still a denial-of-service against someone "
        "else's device. Not okay without explicit authorization."
    }},

    // ─── SAE (id 10) ──────────────────────────────────────────────────────
    { 10, 0, {
        "ACTIVE - DISRUPTIVE (WPA3). Floods a selected AP with SAE commit frames, "
        "which are computationally expensive to validate. Can DoS the AP by "
        "exhausting its CPU.",
        "Select the AP first (Utilities/Lists > Select AP). AP must support "
        "WPA3-SAE. WiFi antenna enabled. Authorization required.",
        "Counter shows frames sent. Selected AP may become unresponsive for new "
        "connections. Legitimate clients may fail to associate.",
        "This is a documented research technique against WPA3 but it's still a "
        "denial-of-service against someone else's infrastructure. Only use on "
        "your own WPA3 equipment."
    }},

    // ─── BLE Spam (id 9) ──────────────────────────────────────────────────
    { 9, 0, {
        "ACTIVE - DISRUPTIVE. Transmits spoofed BLE advertisements that trigger "
        "iOS pairing popups on nearby iPhones. Known as 'Sour Apple'. Can leave "
        "phones unresponsive while in range.",
        "Bluetooth antenna enabled. Affects: iOS devices.",
        "iPhones near you show 'Not Your AirPods?' / other pairing dialogs "
        "repeatedly. FPS drops while running.",
        "This is harassment if directed at strangers, and it can render a "
        "phone effectively unusable until it leaves radio range. Demo on your "
        "own devices only."
    }},
    { 9, 1, {
        "ACTIVE - DISRUPTIVE. Windows Swiftpair spoofing. Triggers 'Found new "
        "Bluetooth device' popups on Windows laptops and tablets in range.",
        "Bluetooth antenna enabled. Affects: Windows devices.",
        "Windows machines show pairing dialogs repeatedly. Less disruptive than "
        "Sour Apple but still annoying.",
        "Pop-up spam aimed at strangers is harassment regardless of how mild "
        "the dialog is. Your own test rig only."
    }},
    { 9, 2, {
        "ACTIVE - DISRUPTIVE. Samsung-style BLE pairing popups. Affects Samsung "
        "Android devices.",
        "Bluetooth antenna enabled. Affects: Samsung Android devices.",
        "Samsung phones show Fast-Pair style popups.",
        "Pop-up spam in a crowd of unsuspecting Samsung users is harassment. "
        "Demo on devices you own, ideally with the owner watching."
    }},
    { 9, 3, {
        "ACTIVE - DISRUPTIVE. Google Fast Pair popups. Affects Android devices "
        "that support Fast Pair (Pixel and similar).",
        "Bluetooth antenna enabled. Affects: Android devices with Fast Pair.",
        "Affected Android phones show pairing popups.",
        "Same harassment caveat as the other BT spam variants - don't aim it "
        "at strangers' phones."
    }},
    { 9, 4, {
        "ACTIVE - TRANSMITS. Flipper-Zero-style BLE notifications. A novelty mode "
        "that spoofs Flipper-themed advertisements; it doesn't do anything a real "
        "Flipper does. A novelty for Flipper meetups.",
        "Bluetooth antenna enabled.",
        "Nearby phones may see Flipper-themed pairing popups depending on OS.",
        "Mostly harmless but still spammy. Quick demos only - don't sit "
        "anywhere running this at strangers for an hour, Flipper meetup or not."
    }},
    { 9, 5, {
        "ACTIVE - DISRUPTIVE. Runs all five BT spam variants at once (Sour Apple, "
        "Swiftpair, Samsung, Google, Flipper). Maximum disruption for "
        "demonstration purposes - affects every nearby phone simultaneously. "
        "KNOWN LIMITATION: this 'All' mode can be slow or briefly unresponsive to "
        "start (a quirk of cycling every BT payload on the shared WiFi/BT radio). If "
        "it doesn't start within a few seconds, back out and pick a single spam type "
        "- Sour Apple, Swiftpair, Samsung, Google, or Flipper all start instantly.",
        "Bluetooth antenna enabled. Affects: all phones and laptops in range.",
        "Phones in range light up with overlapping pairing popups from every "
        "vendor they recognize. Visible from across a room. On recent iOS "
        "versions Sour Apple alone can render a phone unusable until it leaves "
        "radio range; layered with the others it's significantly worse.",
        "Demo only on devices you control or with full-room consent. The "
        "broadcast nature means nearby strangers' phones get hit too."
    }},

#endif // CLIPBOY_RES34RCH
    // ─── Utilities/Lists (id 4) ───────────────────────────────────────────
    // Items 0-6 are TAT_LIST_VIEW (List APs, List SSIDs, List Stations, List
    // BT Devices, List AirTags, List Flippers, Saved Networks) - the More
    // Info panel is suppressed for those, so no entries here.
    { 4, 7, {
        "Add a custom SSID to the badge's SSID list. Entries are viewable under "
        "List SSIDs.",
        NULL,
        "Keyboard prompt appears. Type the SSID, tap OK. New entry shows up "
        "next time you open List SSIDs.",
        NULL
    }},
    { 4, 8, {
        "Generate 20 random SSIDs and add them to the badge's SSID list in one "
        "go. A quick way to populate the list; entries are viewable under List SSIDs.",
        NULL,
        "Status updates briefly. New entries visible in List SSIDs.",
        NULL
    }},
    { 4, 9, {
        "Pick an AP from the scan results to set as the selected AP for tools "
        "that need one (EAPOL capture, RSSI monitor, etc.).",
        "Run an AP scan first so there are APs to choose from.",
        "AP list opens; tap one to select. Selection persists across tools "
        "until you change it.",
        NULL
    }},
    { 4, 10, {
        "Wipe all collected scan data: AP list, Station list, SSID list. "
        "Starts fresh - useful between tests or when leaving a venue.",
        NULL,
        "Lists empty immediately. No device-level effect.",
        NULL
    }},
    { 4, 11, {
        "Pick a specific WiFi channel (1-14) to monitor, or 'Auto' for channel "
        "hopping. Affects subsequent passive scans - fixed channel sees more "
        "detail there, 'Auto' is broad but shallow. Auto hops every ~250 ms.",
        NULL,
        // RSSI removed from this advice 2026-07-26: Monitor > RSSI now tunes to the selected AP's
        // channel by itself, so setting the channel here is overridden for that tool -- telling the
        // user to do it would be advice that silently does nothing.
        "Dropdown with the choice. Takes effect on the next scan started. For "
        "EAPOL/PMKID on a known AP, set the AP's channel explicitly "
        "first - Auto's hop interval is too short to reliably catch a four-way "
        "handshake. Monitor > RSSI does not need this: it tunes to the selected "
        "AP's channel on its own.",
        NULL
    }},

    // ─── Network (id 5) ───────────────────────────────────────────────────
    { 5, 0, {
        "Connect to a WiFi network by SSID and password. After connecting, the "
        "badge can use the internet for networked features.",
        "SSID list populated (run an AP scan or type one in). Password known. "
        "Badge reboots to connect.",
        "Keyboard prompts for SSID + password. Badge restarts and reconnects. "
        "Status bar WiFi icon shows the connection.",
        "Credentials are stored unencrypted in NVS. A badge that boots can be "
        "flashed and read in seconds - don't save creds for networks you "
        "wouldn't write on a sticky note. Use Saved Networks > delete to remove "
        "stored entries when finished."
    }},
    { 5, 1, {
        // NOTE: keep this string SKU-NEUTRAL. Cat 5 (Network) compiles into BOTH SKUs, but
        // Evil Portal (cat 11) is #ifdef CLIPBOY_RES34RCH-only -- naming it here would leak a
        // Res34rch tool name into the listen-only Sn34k Help (on-device AND the web Help generated
        // from this table). "acts as an AP" covers every AP-mode consumer without naming one.
        "Randomize the AP-mode MAC address. When the badge later acts as an AP, "
        "it uses the new MAC.",
        NULL,
        "Status message confirms the new MAC. No visible external effect until "
        "you run an AP-mode tool.",
        "MAC randomization is a privacy feature, not a license to impersonate. "
        "Don't deliberately spoof a specific known MAC you don't own."
    }},
    { 5, 2, {
        "Randomize the station-mode MAC address. Changes the MAC used when the "
        "badge connects as a client to another AP (Join WiFi).",
        NULL,
        "Status message confirms the new MAC.",
        "MAC randomization is a privacy feature, not a license to impersonate. "
        "Don't deliberately spoof a specific known MAC you don't own."
    }},

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH More-Info (Evil Portal, id 11)
    // ─── Evil Portal (id 11) ──────────────────────────────────────────────
    { 11, 0, {
        "ACTIVE - PHISHING. Hosts a captive-portal page that prompts connecting "
        "users for credentials. This is a credential-harvesting / phishing setup.",
        "No prerequisites - uses a built-in HTML portal. WiFi antenna enabled.",
        "Any device connecting to the badge's AP is redirected to the portal. "
        "Captured input is shown on-screen in the scan output (CRED: user / pass) "
        "and held in memory only - it is NOT written to the SD card, and it clears "
        "when you stop the portal or reboot.",
        "This is straightforwardly phishing. Running it against anyone "
        "who didn't explicitly agree is a crime. Use only for scoped red-team "
        "engagements or blue-team training where every participant knows the "
        "portal is fake."
    }},
    { 11, 1, {
        "ACTIVE - PHISHING. Same as Start Default but loads your own HTML from SD. "
        "Lets you tailor portals for a specific engagement.",
        "SD card with an .html file in the card's ROOT (e.g. /myportal.html). WiFi "
        "antenna enabled. The page must submit the fields named 'email' and "
        "'password' (to /get) to be captured. Asset folders (CSS/JS/images) "
        "aren't supported - inline everything in the HTML.",
        "File picker appears (lists the .html files in the SD root). Pick the HTML, "
        "portal starts. Otherwise identical to Start Default - captured input shows "
        "on-screen, not saved to SD.",
        "A custom portal is still phishing. Authorized red-team or training use "
        "only - don't 'test' tailored phishing pages on any network where "
        "unsuspecting people could connect."
    }},
    { 11, 2, {
        "Stop any running Evil Portal and tear down the AP. Use this after "
        "finishing a test engagement.",
        NULL,
        "Badge stops broadcasting its AP. Any connected clients drop. Captured "
        "credentials are held in memory only and are cleared on stop or reboot - "
        "nothing is written to the SD card.",
        NULL
    }},
#endif // CLIPBOY_RES34RCH
};

static const int NUM_TOOL_INFO = sizeof(tool_info_table) / sizeof(tool_info_table[0]);

// Lookup by (cat, item). Returns NULL if no entry exists yet.
static const ToolInfo* tool_info_lookup(uint8_t cat, uint8_t item) {
    for (int i = 0; i < NUM_TOOL_INFO; i++) {
        if (tool_info_table[i].cat == cat && tool_info_table[i].item == item)
            return &tool_info_table[i].info;
    }
    return NULL;
}

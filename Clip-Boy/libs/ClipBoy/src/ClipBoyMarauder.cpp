// ClipBoyMarauder - Library Implementation
// Declares all global objects the original Marauder code expects,
// then wraps them behind a clean API.

#include "ClipBoyMarauder.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_system.h"

// ============================================================
// ClipBoy ESP32-S3 WiFi coexistence workaround
// ============================================================
// The ESP-IDF 4.4 coexistence module (coex_core_enable) aborts when WiFi is
// repeatedly stopped/deinitialized and restarted.  Fix: init and start WiFi
// exactly once; never stop or deinit it.  Between scans only promiscuous mode
// is toggled.
//
// Using the linker's --wrap feature (see platform.local.txt), all calls to
// the functions below — from ANY compilation unit including the pre-compiled
// Arduino WiFi library — are redirected here.
// ============================================================
#ifdef CLIPBOY_ESP32S3
extern "C" {
    static bool _cb_wifi_hw_up = false;

    // --- first-call-through wrappers ---
    esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);
    esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config) {
        if (_cb_wifi_hw_up) return ESP_OK;
        return __real_esp_wifi_init(config);
    }

    esp_err_t __real_esp_wifi_start(void);
    esp_err_t __wrap_esp_wifi_start(void) {
        if (_cb_wifi_hw_up) return ESP_OK;
        esp_err_t r = __real_esp_wifi_start();
        if (r == ESP_OK) _cb_wifi_hw_up = true;
        return r;
    }

    // --- always no-op (never tear down WiFi) ---
    esp_err_t __wrap_esp_wifi_stop(void)    { return ESP_OK; }
    esp_err_t __wrap_esp_wifi_deinit(void)  { return ESP_OK; }
    esp_err_t __wrap_esp_wifi_restore(void) { return ESP_OK; }
    esp_err_t __wrap_esp_netif_deinit(void) { return ESP_OK; }

    // --- no-op once WiFi is running (prevent ioctl crashes) ---
    esp_err_t __real_esp_wifi_set_mode(wifi_mode_t mode);
    esp_err_t __wrap_esp_wifi_set_mode(wifi_mode_t mode) {
        return _cb_wifi_hw_up ? ESP_OK : __real_esp_wifi_set_mode(mode);
    }

    esp_err_t __real_esp_wifi_set_country(const wifi_country_t *country);
    esp_err_t __wrap_esp_wifi_set_country(const wifi_country_t *country) {
        return _cb_wifi_hw_up ? ESP_OK : __real_esp_wifi_set_country(country);
    }

    esp_err_t __real_esp_wifi_set_storage(wifi_storage_t storage);
    esp_err_t __wrap_esp_wifi_set_storage(wifi_storage_t storage) {
        return _cb_wifi_hw_up ? ESP_OK : __real_esp_wifi_set_storage(storage);
    }

    esp_err_t __real_esp_wifi_set_ps(wifi_ps_type_t type);
    esp_err_t __wrap_esp_wifi_set_ps(wifi_ps_type_t type) {
        return _cb_wifi_hw_up ? ESP_OK : __real_esp_wifi_set_ps(type);
    }

#if defined(CB_WIFI_PS_EXPERIMENT) && CB_WIFI_PS_EXPERIMENT
    // EXPERIMENT BUILD ONLY (CB_EXTRA_DEFS='-DCB_WIFI_PS_EXPERIMENT=1'). NOT shippable.
    // The wrap above deliberately no-ops power-save changes once the hardware is up, which means
    // there is NO way to A/B modem sleep at runtime -- and MEASURED 2026-07-28, calling
    // WiFi.setSleep(false) in the one window before cb.begin() does NOT stick either (the flag
    // still reads MIN_MODEM afterwards). This passthrough exists ONLY so the cost of MIN_MODEM on
    // passive reception can be priced by a within-badge crossover. It bypasses the wrap on purpose.
    esp_err_t cb_experiment_force_wifi_ps(int type) {
        return __real_esp_wifi_set_ps((wifi_ps_type_t)type);
    }
#endif
}
#endif // CLIPBOY_ESP32S3

// ============================================================
// Global objects required by original ESP32Marauder code.
// These satisfy all extern declarations in the original headers.
// ============================================================
WiFiScan wifi_scan_obj;
EvilPortal evil_portal_obj;
Buffer buffer_obj;
Settings settings_obj;
SDInterface sd_obj;
LedInterface led_obj;

extern const String PROGMEM version_number;
extern const String PROGMEM board_target;
const String PROGMEM version_number = MARAUDER_VERSION;
const String PROGMEM board_target = "ClipBoy";
uint32_t currentTime = 0;

// Extern linked lists and counters defined in WiFiScan.cpp
extern LinkedList<ssid>* ssids;
extern LinkedList<AccessPoint>* access_points;
extern LinkedList<Station>* stations;
extern LinkedList<AirTag>* airtags;
extern LinkedList<Flipper>* flippers;
extern LinkedList<ProbeReqSsid>* probe_req_ssids;
extern int num_beacon, num_deauth, num_probe, num_eapol;

// ============================================================
// ClipBoyMarauder Implementation
// ============================================================

ClipBoyMarauder::ClipBoyMarauder() : _state(ClipBoyState::IDLE) {}

bool ClipBoyMarauder::begin() {
    // Seed RNG
    randomSeed(esp_random());

    // Suppress noisy ESP logs unless debugging
    #ifndef DEVELOPER
    esp_log_level_set("*", ESP_LOG_NONE);
    #endif

    // Serial
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0 < 3000)) { delay(10); }

    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F("  ClipBoy Marauder Library"));
    Serial.println(F("  Based on ESP32Marauder " MARAUDER_VERSION));
    Serial.println(F("========================================"));

    // PSRAM
    #ifdef HAS_PSRAM
    if (psramInit()) {
        Serial.printf("PSRAM: %u KB free\n", ESP.getFreePsram() / 1024);
    } else {
        Serial.println(F("PSRAM: init failed"));
    }
    #endif

    Serial.printf("Heap: %u KB free\n", ESP.getFreeHeap() / 1024);
    Serial.println("ESP-IDF: " + String(esp_get_idf_version()));
    Serial.println("Loading settings...");

    // Settings (SPIFFS)
    settings_obj.begin();
    Serial.println(F("Settings loaded."));

    // Buffer
    buffer_obj = Buffer();

    // SD Card
    #ifdef HAS_SD
    pinMode(SD_CS, OUTPUT);
    delay(10);
    digitalWrite(SD_CS, HIGH);
    delay(10);
    if (sd_obj.initSD()) {
        Serial.println(F("SD card mounted."));
    } else {
        Serial.println(F("SD card not detected (continuing without)."));
    }
    #endif

    // WiFi + BT hardware init
    wifi_scan_obj.RunSetup();
    Serial.println(F("WiFi/BT initialized."));

    // Evil Portal data structures
    evil_portal_obj.setup();

    // LED (no-op without NeoPixel)
    led_obj.RunSetup();

    // Set idle state
    wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
    _state = ClipBoyState::IDLE;

    Serial.println(F("ClipBoy ready."));
    Serial.println();
    return true;
}

void ClipBoyMarauder::loop() {
    currentTime = millis();
    wifi_scan_obj.main(currentTime);
    #ifdef HAS_SD
    sd_obj.main();
    #endif
    // Clip-Boy local patch (DC34): buffer_obj.save() does a full open+append+close to
    // SD/LittleFS. During Raw/PCAP capture the RX buffer is refilled continuously, so
    // calling it EVERY loop meant tens of ms of blocking file I/O per loop on core 1 --
    // right before lv_timer_handler() -- which froze the UI. Flush at most ~every 150ms;
    // the RAM double-buffer absorbs the gap (Buffer drops when full: lossy-but-safe), and
    // save() early-returns instantly when the buffer is empty so non-capture tools are
    // unaffected. See THIRD_PARTY.md.
    static uint32_t _lastBufferSave = 0;
    if (currentTime - _lastBufferSave >= 150) {
        _lastBufferSave = currentTime;
        buffer_obj.save();
    }
    settings_obj.main(currentTime);
    led_obj.main(currentTime);
    _updateState();
    delay(1); // yield
}

void ClipBoyMarauder::_updateState() {
    uint8_t mode = wifi_scan_obj.currentScanMode;
    if (mode == WIFI_SCAN_OFF) {
        _state = ClipBoyState::IDLE;
    } else if (mode == WIFI_SCAN_EVIL_PORTAL) {
        _state = ClipBoyState::PORTAL_ACTIVE;
    } else if (mode >= WIFI_ATTACK_BEACON_SPAM && mode != WIFI_SCAN_EVIL_PORTAL
               && mode != WIFI_SCAN_TARGET_AP && mode != WIFI_SCAN_TARGET_AP_FULL
               && mode != WIFI_SCAN_STATION && mode != WIFI_SCAN_AP_STA
               && mode != WIFI_SCAN_RAW_CAPTURE && mode != WIFI_SCAN_ESPRESSIF
               && mode != WIFI_SCAN_PINESCAN && mode != WIFI_SCAN_MULTISSID
               && mode != WIFI_SCAN_SIG_STREN && mode != WIFI_SCAN_CHAN_ACT
               && mode != WIFI_SCAN_DETECT_FOLLOW && mode != WIFI_SCAN_SAE_COMMIT
               && mode != WIFI_SCAN_PACKET_RATE) {
        // Active (transmitting) modes
        _state = ClipBoyState::TRANSMITTING;
    } else if (mode != WIFI_SCAN_OFF) {
        _state = ClipBoyState::SCANNING;
    }
}

// ============================================================
// WiFi Scanning
// ============================================================

void ClipBoyMarauder::scanAPs()            { wifi_scan_obj.StartScan(WIFI_SCAN_TARGET_AP, 0); }
void ClipBoyMarauder::scanAPsFull()        { wifi_scan_obj.StartScan(WIFI_SCAN_TARGET_AP_FULL, 0); }
void ClipBoyMarauder::scanAPsAndStations() { wifi_scan_obj.StartScan(WIFI_SCAN_AP_STA, 0); }
void ClipBoyMarauder::scanStations()       { wifi_scan_obj.StartScan(WIFI_SCAN_STATION, 0); }
// Clip-Boy local patch (DC34-146): WIFI_SCAN_AP (raw beacon sniff) only Serial-prints and
// never fills access_points, so the UI monitor/table showed nothing. Use TARGET_AP so
// beacons populate the deduped AP list the log poller + monitor table read.
void ClipBoyMarauder::sniffBeacons()       { wifi_scan_obj.StartScan(WIFI_SCAN_TARGET_AP, 0); }
void ClipBoyMarauder::sniffProbes()        { wifi_scan_obj.StartScan(WIFI_SCAN_PROBE, 0); }
void ClipBoyMarauder::sniffDeauth()        { wifi_scan_obj.StartScan(WIFI_SCAN_DEAUTH, 0); }
void ClipBoyMarauder::sniffEAPOL()         { wifi_scan_obj.StartScan(WIFI_SCAN_EAPOL, 0); }
void ClipBoyMarauder::sniffRaw()           { wifi_scan_obj.StartScan(WIFI_SCAN_RAW_CAPTURE, 0); }
void ClipBoyMarauder::sniffPwnagotchi()    { wifi_scan_obj.StartScan(WIFI_SCAN_PWN, 0); }
// Clip-Boy local patch (DC34-146): WIFI_SCAN_ESPRESSIF (12) has no case in StartScan --
// it never runs. Use a normal AP scan; the UI poller (cb_poll_espressif) filters the AP
// list to Espressif OUIs so the tool actually surfaces ESP-family devices.
void ClipBoyMarauder::sniffEspressif()     { wifi_scan_obj.StartScan(WIFI_SCAN_TARGET_AP, 0); }
void ClipBoyMarauder::sniffPinescan()      { wifi_scan_obj.StartScan(WIFI_SCAN_PINESCAN, 0); }
void ClipBoyMarauder::sniffMultiSSID()     { wifi_scan_obj.StartScan(WIFI_SCAN_MULTISSID, 0); }
void ClipBoyMarauder::sniffSAE()           { wifi_scan_obj.StartScan(WIFI_SCAN_SAE_COMMIT, 0); }
void ClipBoyMarauder::packetMonitor()      { wifi_scan_obj.StartScan(WIFI_PACKET_MONITOR, 0); }
void ClipBoyMarauder::packetRate()         { wifi_scan_obj.StartScan(WIFI_SCAN_PACKET_RATE, 0); }
// Monitor > RSSI. Tunes the radio to the SELECTED AP's channel, because the SIG_STREN sniffer
// does not do it: it listens on whatever channel it inherited, so unless the target happens to sit
// there the tool hears nothing from it and simply leaves the last AP-scan reading on screen.
// MEASURED 2026-07-26 (COM4): monitoring on the inherited channel gave beacon_frames = 0 across
// 12 s while a 5-way beacon fanout ran on ch6 -- mgmt frames climbed (ambient traffic) but not one
// beacon from the target. Setting the channel afterwards took beacon 0 -> 138 in five seconds and
// collapsed the freshness counter from 7233 ms to 234 ms.
// ORDER: after StartScan is what was measured working, and either order is in fact valid.
// ⚠ An earlier version of this comment claimed the order was LOAD-BEARING because "StartScan resets
// the channel". That is FALSE in source and was corrected by review: changeChannel() writes
// `set_channel` FIRST (WiFiScan.cpp:11051), StartScan -> initWiFi RE-APPLIES it (2226 -> 2203-2205
// -> 11060-11064), and RunRawScan re-applies it again after promiscuous comes up (5547). Nothing
// resets it to a default -- every writer of set_channel was enumerated. So tuning BEFORE StartScan
// would also survive, and is arguably marginally safer because it walks access_points before the
// sniffer is live. The "before doesn't work" measurement was taken on a run whose ap_scan selection
// had silently failed, i.e. no tune happened in either order and both read identical.
// Left as-is because THIS order is the one verified on hardware; if you move it, re-verify rather
// than trusting either comment.
// ⚠ This is our wrapper, so the Marauder serial CLI path (CommandLine.cpp, direct
// StartScan(WIFI_SCAN_SIG_STREN,...)) does NOT get the tune and stays channel-limited.
// No-op when nothing is selected or the record carries a nonsense channel: leave the inherited
// channel rather than guessing, so this cannot make the untargeted case worse.
void ClipBoyMarauder::signalMonitor() {
    wifi_scan_obj.StartScan(WIFI_SCAN_SIG_STREN, 0);
    if (!access_points) return;
    for (int i = 0; i < access_points->size(); i++) {
        AccessPoint ap = access_points->get(i);
        if (!ap.selected) continue;
        if (ap.channel >= 1 && ap.channel <= 14) {
            wifi_scan_obj.changeChannel(ap.channel);
            Serial.printf("[RSSI] tuned to ch %d for the selected AP\n", ap.channel);
        }
        return;   // first selected AP wins, matching the sniffer's own first-match behaviour
    }
}
void ClipBoyMarauder::channelActivity()    { wifi_scan_obj.StartScan(WIFI_SCAN_CHAN_ACT, 0); }
void ClipBoyMarauder::macTracker()         { wifi_scan_obj.StartScan(WIFI_SCAN_DETECT_FOLLOW, 0); }

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH primitives (Res34rch-Boy only)
void ClipBoyMarauder::sniffPMKID(int channel, bool withDeauth) {
    if (channel > 0) {
        wifi_scan_obj.changeChannel(channel);
    }
    if (withDeauth) {
        wifi_scan_obj.StartScan(WIFI_SCAN_ACTIVE_EAPOL, 0);
    } else {
        wifi_scan_obj.StartScan(WIFI_SCAN_EAPOL, 0);
    }
}

void ClipBoyMarauder::sniffPMKIDList() {
    wifi_scan_obj.StartScan(WIFI_SCAN_ACTIVE_LIST_EAPOL, 0);
}
#endif // CLIPBOY_RES34RCH

// ============================================================
// WiFi active (transmitting) tools
// ============================================================

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH primitives (Res34rch-Boy only)
void ClipBoyMarauder::deauthAPs()           { wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH, 0); }
void ClipBoyMarauder::deauthManual()     { wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH_MANUAL, 0); }
void ClipBoyMarauder::deauthStations()   { wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH_TARGETED, 0); }
void ClipBoyMarauder::beaconSpamRandom()       { wifi_scan_obj.StartScan(WIFI_ATTACK_BEACON_SPAM, 0); }
void ClipBoyMarauder::beaconSpamList()       { wifi_scan_obj.StartScan(WIFI_ATTACK_BEACON_LIST, 0); }
void ClipBoyMarauder::beaconSpamClone()    { wifi_scan_obj.StartScan(WIFI_ATTACK_AP_SPAM, 0); }
void ClipBoyMarauder::beaconRickRoll()         { wifi_scan_obj.StartScan(WIFI_ATTACK_RICK_ROLL, 0); }
void ClipBoyMarauder::beaconFunny()      { wifi_scan_obj.StartScan(WIFI_ATTACK_FUNNY_BEACON, 0); }
void ClipBoyMarauder::probeFlood()            { wifi_scan_obj.StartScan(WIFI_ATTACK_AUTH, 0); }
void ClipBoyMarauder::authFlood()             { wifi_scan_obj.StartScan(WIFI_ATTACK_AUTH, 0); }
void ClipBoyMarauder::badMsgFlood()           { wifi_scan_obj.StartScan(WIFI_ATTACK_BAD_MSG, 0); }
void ClipBoyMarauder::badMsgStations()   { wifi_scan_obj.StartScan(WIFI_ATTACK_BAD_MSG_TARGETED, 0); }
void ClipBoyMarauder::sleepFlood()            { wifi_scan_obj.StartScan(WIFI_ATTACK_SLEEP, 0); }
void ClipBoyMarauder::sleepStations()    { wifi_scan_obj.StartScan(WIFI_ATTACK_SLEEP_TARGETED, 0); }
void ClipBoyMarauder::saeCommitFlood()              { wifi_scan_obj.StartScan(WIFI_ATTACK_SAE_COMMIT, 0); }
#endif // CLIPBOY_RES34RCH

// ============================================================
// Bluetooth
// ============================================================

void ClipBoyMarauder::btScanAll()           { wifi_scan_obj.StartScan(BT_SCAN_ALL, 0); }
void ClipBoyMarauder::btScanSimple()        { wifi_scan_obj.StartScan(BT_SCAN_SIMPLE, 0); }
void ClipBoyMarauder::btScanSkimmers()      { wifi_scan_obj.StartScan(BT_SCAN_SKIMMERS, 0); }
void ClipBoyMarauder::btScanAirtags()       { wifi_scan_obj.StartScan(BT_SCAN_AIRTAG, 0); }
void ClipBoyMarauder::btScanAirtagMonitor() { wifi_scan_obj.StartScan(BT_SCAN_AIRTAG_MON, 0); }
void ClipBoyMarauder::btScanFlippers()      { wifi_scan_obj.StartScan(BT_SCAN_FLIPPER, 0); }
void ClipBoyMarauder::btScanFlock()         { wifi_scan_obj.StartScan(BT_SCAN_FLOCK, 0); }
#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH primitives (Res34rch-Boy only)
void ClipBoyMarauder::btSpamApple()         { wifi_scan_obj.StartScan(BT_ATTACK_SOUR_APPLE, 0); }
void ClipBoyMarauder::btSpamWindows()       { wifi_scan_obj.StartScan(BT_ATTACK_SWIFTPAIR_SPAM, 0); }
void ClipBoyMarauder::btSpamSamsung()       { wifi_scan_obj.StartScan(BT_ATTACK_SAMSUNG_SPAM, 0); }
void ClipBoyMarauder::btSpamGoogle()        { wifi_scan_obj.StartScan(BT_ATTACK_GOOGLE_SPAM, 0); }
void ClipBoyMarauder::btSpamFlipper()       { wifi_scan_obj.StartScan(BT_ATTACK_FLIPPER_SPAM, 0); }
void ClipBoyMarauder::btSpamAll()           { wifi_scan_obj.StartScan(BT_ATTACK_SPAM_ALL, 0); }
#endif // CLIPBOY_RES34RCH

// ============================================================
// Evil Portal
// ============================================================

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH primitives (Res34rch-Boy only)
void ClipBoyMarauder::startEvilPortal(String htmlFile) {
    if (htmlFile.length() > 0) {
        // Set HTML file before starting
        evil_portal_obj.html_files->clear();
        evil_portal_obj.html_files->add(htmlFile);
    }
    wifi_scan_obj.StartScan(WIFI_SCAN_EVIL_PORTAL, 0);
}

void ClipBoyMarauder::stopEvilPortal() {
    stopScan();
}
#endif // CLIPBOY_RES34RCH

// ============================================================
// Control
// ============================================================

void ClipBoyMarauder::stopScan() {
    wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
    _state = ClipBoyState::IDLE;
}

// Clip-Boy (PCAP perf): drain + close any open capture file. The pcap file handle is
// now held open for the whole capture (O(1) drains); this releases it. Idempotent --
// a no-op when no capture is open -- so it's safe to call on every tool stop. Runs on
// the loop/main task (same task as buffer_obj.save()), so no extra locking is needed.
void ClipBoyMarauder::finishCapture() {
    buffer_obj.finalize();
}

void ClipBoyMarauder::setChannel(int channel) {
    wifi_scan_obj.changeChannel(channel);
}

// Clip-Boy local patch (DC34): bring the AP interface up (APSTA) on demand so the
// active-transmit tools' esp_wifi_80211_tx(WIFI_IF_AP,...) actually radiate, then
// drop back to STA-only when idle so no SoftAP is broadcast. The badge boots STA
// (Sn34k listen-only + no idle 'ESP_xxxxxx' AP); Res34rch transmit tools call this
// at start/stop. Must use __real_esp_wifi_set_mode -- the --wrap shim above no-ops
// esp_wifi_set_mode once _cb_wifi_hw_up is set, which is exactly why WIFI_IF_AP TX
// was dead (mode never left STA). APSTA keeps the STA netif, so joinWiFi + scans
// are unaffected. startWiFiAttacks configures the AP hidden.
void ClipBoyMarauder::setRawTxMode(bool apOn) {
#ifdef CLIPBOY_ESP32S3
    // __real_esp_wifi_set_mode is declared at file scope in the --wrap block above.
    __real_esp_wifi_set_mode(apOn ? WIFI_MODE_APSTA : WIFI_MODE_STA);
#else
    esp_wifi_set_mode(apOn ? WIFI_MODE_APSTA : WIFI_MODE_STA);
#endif
}

int ClipBoyMarauder::getChannel() {
    return wifi_scan_obj.set_channel;
}

bool ClipBoyMarauder::joinWiFi(String ssid, String password) {
    return wifi_scan_obj.joinWiFi(ssid, password, false);
}

// ============================================================
// SSID Management
// ============================================================

void ClipBoyMarauder::addSSID(String essid) {
    wifi_scan_obj.addSSID(essid);
}

void ClipBoyMarauder::generateSSIDs(int count) {
    wifi_scan_obj.RunGenerateSSIDs(count);
}

void ClipBoyMarauder::clearSSIDs() {
    wifi_scan_obj.RunClearSSIDs();
}

int ClipBoyMarauder::getSSIDCount() {
    if (ssids) return ssids->size();
    return 0;
}

// ============================================================
// AP Management
// ============================================================

void ClipBoyMarauder::clearAPs() {
    // ⚠ MUST stay RunClearAPs() -- do NOT "narrow" this to WiFiScan::clearAPs().
    //
    // I tried exactly that (2026-07-26) on the reasoning that clearing APs should not discard a
    // station selection, since this file documents the AP/STA/SSID lists as select-then-run
    // INPUTS. That reasoning is WRONG, and an adversarial review caught the regression before
    // it shipped. Station links are DERIVED from the AP list and cannot outlive it:
    //
    //   - AccessPoint.stations holds INDICES into the global `stations` list. That sub-list is
    //     the only AP<->station linkage.
    //   - WiFiScan::clearAPs() deletes every AP and frees each sub-list, but leaves the global
    //     `stations` list populated.
    //   - The station sniffer refuses to re-link a MAC already present in that global list
    //     (the `in_list` early return, WiFiScan.cpp ~:8756, returns BEFORE the
    //     ap.stations->add() at ~:8829).
    //
    // So after a narrow AP clear + rescan, stations still EXIST but no AP references them.
    // Every station-targeted consumer walks AP->sub-list (sendDeauthFrame targeted ~:12384,
    // sendBadMsgAttack / sendAssocSleepAttack ~:9948+), finds it empty, and TRANSMITS NOTHING --
    // while the STA list still renders the station and still shows it ticked, because that view
    // reads the intact global list. On Res34rch that is a silent no-op on an active-TX tool.
    //
    // RunClearAPs() clears both, so the STA list empties and the failure is VISIBLE. A visible
    // failure beats a silent one; that is the whole thesis of this campaign, and narrowing this
    // call inverted it.
    wifi_scan_obj.RunClearAPs();
}

int ClipBoyMarauder::getAPCount() {
    if (access_points) return access_points->size();
    return 0;
}

void ClipBoyMarauder::selectAP(int index) {
    if (access_points && index >= 0 && index < access_points->size()) {
        AccessPoint ap = access_points->get(index);
        ap.selected = true;
        access_points->set(index, ap);
    }
}

void ClipBoyMarauder::deselectAPs() {
    if (!access_points) return;
    for (int i = 0; i < access_points->size(); i++) {
        AccessPoint ap = access_points->get(i);
        ap.selected = false;
        access_points->set(i, ap);
    }
}

// ============================================================
// Station Management
// ============================================================

void ClipBoyMarauder::clearStations() {
    // Reverted alongside clearAPs() above. Clearing stations alone IS coherent in isolation
    // (WiFiScan::clearStations empties the global list and every AP's sub-list, so no dangling
    // indices), but the two wrappers were changed together and only clearAPs was reviewed in
    // depth. Keeping both on the upstream Run* handlers means the pair stays consistent with
    // each other and with the serial CLI, and nothing depends on a difference nobody has tested.
    // Revisit only with a test that proves station re-linking still works after each variant.
    wifi_scan_obj.RunClearStations();
}

int ClipBoyMarauder::getStationCount() {
    if (stations) return stations->size();
    return 0;
}

void ClipBoyMarauder::selectStation(int index) {
    if (stations && index >= 0 && index < stations->size()) {
        Station st = stations->get(index);
        st.selected = true;
        stations->set(index, st);
    }
}

// ============================================================
// MAC
// ============================================================

void ClipBoyMarauder::randomizeAPMac()  { wifi_scan_obj.RunGenerateRandomMac(true); }
void ClipBoyMarauder::randomizeSTAMac() { wifi_scan_obj.RunGenerateRandomMac(false); }
// setMac() applies both ap_mac and sta_mac to WIFI_IF_AP/STA. It is proven to take on an
// enabled+started interface (the Join WiFi/STA path applies its random MAC on air). Evil Portal's
// startAP() never calls it, so this is invoked from dispatch (case 11) after APSTA is up.
void ClipBoyMarauder::applyStagedMacs() { wifi_scan_obj.setMac(); }

// ============================================================
// Info
// ============================================================

ClipBoyState ClipBoyMarauder::getState()          { return _state; }
uint8_t ClipBoyMarauder::getCurrentScanMode()     { return wifi_scan_obj.currentScanMode; }
bool ClipBoyMarauder::isScanning()                { return wifi_scan_obj.currentScanMode != WIFI_SCAN_OFF; }
bool ClipBoyMarauder::isSDSupported()             { return sd_obj.supported; }

String ClipBoyMarauder::getFreeHeap() {
    return String(ESP.getFreeHeap() / 1024) + " KB";
}

String ClipBoyMarauder::getFreePSRAM() {
    #ifdef HAS_PSRAM
    return String(ESP.getFreePsram() / 1024) + " KB";
    #else
    return "N/A";
    #endif
}

void ClipBoyMarauder::printAPList() {
    if (!access_points) { Serial.println(F("No AP list.")); return; }
    Serial.printf("Access Points (%d):\n", access_points->size());
    for (int i = 0; i < access_points->size(); i++) {
        AccessPoint ap = access_points->get(i);
        Serial.printf("  [%d]%s %-32s  ch:%2d  rssi:%4d  sec:%d  pkts:%d\n",
            i, ap.selected ? "*" : " ",
            ap.essid, ap.channel, ap.rssi, ap.sec, ap.packets);
    }
}

void ClipBoyMarauder::printSSIDList() {
    if (!ssids) { Serial.println(F("No SSID list.")); return; }
    Serial.printf("SSIDs (%d):\n", ssids->size());
    for (int i = 0; i < ssids->size(); i++) {
        ssid s = ssids->get(i);
        Serial.printf("  [%d]%s %s\n", i, s.selected ? "*" : " ", s.essid.c_str());
    }
}

void ClipBoyMarauder::printStationList() {
    if (!stations) { Serial.println(F("No station list.")); return; }
    Serial.printf("Stations (%d):\n", stations->size());
    for (int i = 0; i < stations->size(); i++) {
        Station st = stations->get(i);
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
            st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
        Serial.printf("  [%d]%s %s  pkts:%d  ap:%d\n",
            i, st.selected ? "*" : " ", mac, st.packets, st.ap);
    }
}

void ClipBoyMarauder::printSettings() {
    Serial.println(settings_obj.getSettingsString());
}

void ClipBoyMarauder::printSystemInfo() {
    Serial.println(F("---- ClipBoy System Info ----"));
    Serial.println("Marauder: " + String(MARAUDER_VERSION));
    Serial.println("Board:    ClipBoy ESP32-S3");
    Serial.println("Heap:     " + getFreeHeap());
    Serial.println("PSRAM:    " + getFreePSRAM());
    Serial.println("SD Card:  " + String(isSDSupported() ? "Mounted" : "Not available"));
    Serial.println("Channel:  " + String(getChannel()));
    Serial.println("APs:      " + String(getAPCount()));
    Serial.println("SSIDs:    " + String(getSSIDCount()));
    Serial.println("Stations: " + String(getStationCount()));
    Serial.println("Scan:     " + String(isScanning() ? "Active" : "Idle"));
    Serial.println(F("-----------------------------"));
}

// ============================================================
// Settings
// ============================================================

void ClipBoyMarauder::toggleSetting(String name) {
    settings_obj.toggleSetting(name);
}

void ClipBoyMarauder::setSavePCAP(bool enabled) {
    settings_obj.saveSetting<bool>("SavePCAP", enabled);
}

bool ClipBoyMarauder::getSavePCAP() {
    return settings_obj.loadSetting<bool>("SavePCAP");
}

// Clip-Boy local patch (DC34-147): single SD owner. The host badge mounts the
// global Arduino SD on its own HSPI bus AFTER cb.begin() ran initSD(), so
// sd_obj.supported reflects ClipBoy's (failed/clobbered) first mount and PCAP
// got a NULL filesystem. The badge calls this once SD is up so PCAP uses the
// correctly-mounted singleton. Do NOT SD.begin() again here -- re-begin
// corrupts the shared singleton.
void ClipBoyMarauder::setSDAvailable(bool available) {
    sd_obj.supported = available;
    if (available) sd_obj.cardType = SD.cardType();
}

// ============================================================
// Group A: Scan Result Readers
// ============================================================

bool ClipBoyMarauder::getAP(int index, CBAccessPointInfo& out) {
    if (!access_points || index < 0 || index >= access_points->size()) return false;
    AccessPoint ap = access_points->get(index);
    memset(&out, 0, sizeof(out));
    out.index = index;
    strlcpy(out.essid, ap.essid, sizeof(out.essid));
    memcpy(out.bssid, ap.bssid, 6);
    out.channel = ap.channel;
    out.rssi = ap.rssi;
    out.security = ap.sec;
    out.wps = ap.wps;
    strlcpy(out.manufacturer, ap.man, sizeof(out.manufacturer));
    out.packets = ap.packets;
    out.selected = ap.selected;
    out.has_handshake = ap.has_msg_1 && ap.has_msg_2 && ap.has_msg_3 && ap.has_msg_4;
    return true;
}

bool ClipBoyMarauder::getStation(int index, CBStationInfo& out) {
    if (!stations || index < 0 || index >= stations->size()) return false;
    Station st = stations->get(index);
    memset(&out, 0, sizeof(out));
    out.index = index;
    memcpy(out.mac, st.mac, 6);
    out.packets = st.packets;
    out.apIndex = (int)st.ap;
    out.selected = st.selected;
    return true;
}

bool ClipBoyMarauder::getSSID(int index, CBSSIDInfo& out) {
    if (!ssids || index < 0 || index >= ssids->size()) return false;
    ssid s = ssids->get(index);
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.essid, s.essid.c_str(), sizeof(out.essid) - 1);
    out.channel = s.channel;
    memcpy(out.bssid, s.bssid, 6);
    out.selected = s.selected;
    return true;
}

bool ClipBoyMarauder::getAirTag(int index, CBAirTagInfo& out) {
    if (!airtags || index < 0 || index >= airtags->size()) return false;
    AirTag at = airtags->get(index);
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.mac, at.mac.c_str(), sizeof(out.mac) - 1);
    out.rssi = at.rssi;
    out.lastSeen = at.last_seen;
    out.selected = at.selected;
    return true;
}

int ClipBoyMarauder::getAirTagCount() {
    if (airtags) return airtags->size();
    return 0;
}

bool ClipBoyMarauder::getFlipper(int index, CBFlipperInfo& out) {
    if (!flippers || index < 0 || index >= flippers->size()) return false;
    Flipper f = flippers->get(index);
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.mac, f.mac.c_str(), sizeof(out.mac) - 1);
    strncpy(out.name, f.name.c_str(), sizeof(out.name) - 1);
    return true;
}

int ClipBoyMarauder::getFlipperCount() {
    if (flippers) return flippers->size();
    return 0;
}

bool ClipBoyMarauder::getProbeSSID(int index, CBProbeSSIDInfo& out) {
    if (!probe_req_ssids || index < 0 || index >= probe_req_ssids->size()) return false;
    ProbeReqSsid p = probe_req_ssids->get(index);
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.essid, p.essid.c_str(), sizeof(out.essid) - 1);
    out.requests = p.requests;
    out.selected = p.selected;
    return true;
}

int ClipBoyMarauder::getProbeSSIDCount() {
    if (probe_req_ssids) return probe_req_ssids->size();
    return 0;
}

// ============================================================
// Group A2: BT / Detection Result Readers
// ============================================================

bool ClipBoyMarauder::getBTDevice(int index, CBBTDeviceInfo& out) {
    int count = wifi_scan_obj.getBTDeviceCount();
    if (index < 0 || index >= count) return false;
    BTDevice dev;
    if (!wifi_scan_obj.getBTDeviceEntry(index, dev)) return false;
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.name, dev.name.c_str(), sizeof(out.name) - 1);
    strncpy(out.mac, dev.mac.c_str(), sizeof(out.mac) - 1);
    out.rssi = dev.rssi;
    return true;
}

int ClipBoyMarauder::getBTDeviceCount() {
    return wifi_scan_obj.getBTDeviceCount();
}

bool ClipBoyMarauder::getFlockDevice(int index, CBFlockInfo& out) {
    int count = wifi_scan_obj.getFlockDeviceCount();
    if (index < 0 || index >= count) return false;
    FlockDevice fd;
    if (!wifi_scan_obj.getFlockDeviceEntry(index, fd)) return false;
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.mac, fd.mac.c_str(), sizeof(out.mac) - 1);
    strncpy(out.name, fd.name.c_str(), sizeof(out.name) - 1);
    strncpy(out.serial, fd.serial.c_str(), sizeof(out.serial) - 1);
    out.rssi = fd.rssi;
    out.last_seen = fd.last_seen;
    return true;
}

int ClipBoyMarauder::getFlockDeviceCount() {
    return wifi_scan_obj.getFlockDeviceCount();
}

bool ClipBoyMarauder::getPwnagotchi(int index, CBPwnagotchiInfo& out) {
    int count = wifi_scan_obj.getPwnagotchiCount();
    if (index < 0 || index >= count) return false;
    PwnagotchiDevice pwn;
    if (!wifi_scan_obj.getPwnagotchiEntry(index, pwn)) return false;
    memset(&out, 0, sizeof(out));
    out.index = index;
    strncpy(out.name, pwn.name.c_str(), sizeof(out.name) - 1);
    strncpy(out.version, pwn.version.c_str(), sizeof(out.version) - 1);
    out.pwnd_tot = pwn.pwnd_tot;
    out.uptime = pwn.uptime;
    out.deauth = pwn.deauth;
    return true;
}

int ClipBoyMarauder::getPwnagotchiCount() {
    return wifi_scan_obj.getPwnagotchiCount();
}

bool ClipBoyMarauder::getPinescan(int index, CBPinescanInfo& out) {
    int count = wifi_scan_obj.getConfirmedPinescanCount();
    if (index < 0 || index >= count) return false;
    uint8_t mac[6];
    String type, essid;
    uint8_t ch;
    int8_t rssi;
    if (!wifi_scan_obj.getConfirmedPinescanEntry(index, mac, type, essid, ch, rssi))
        return false;
    memset(&out, 0, sizeof(out));
    out.index = index;
    snprintf(out.mac, sizeof(out.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strncpy(out.detection_type, type.c_str(), sizeof(out.detection_type) - 1);
    strncpy(out.essid, essid.c_str(), sizeof(out.essid) - 1);
    out.channel = ch;
    out.rssi = rssi;
    return true;
}

int ClipBoyMarauder::getPinescanCount() {
    return wifi_scan_obj.getConfirmedPinescanCount();
}

bool ClipBoyMarauder::getMultiSSIDResult(int index, CBMultiSSIDInfo& out) {
    int count = wifi_scan_obj.getConfirmedMultiSSIDCount();
    if (index < 0 || index >= count) return false;
    uint8_t mac[6];
    String essid;
    uint8_t ch, ssid_count;
    int8_t rssi;
    if (!wifi_scan_obj.getConfirmedMultiSSIDEntry(index, mac, essid, ch, rssi, ssid_count))
        return false;
    memset(&out, 0, sizeof(out));
    out.index = index;
    snprintf(out.mac, sizeof(out.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strncpy(out.essid, essid.c_str(), sizeof(out.essid) - 1);
    out.channel = ch;
    out.rssi = rssi;
    out.ssid_count = ssid_count;
    return true;
}

int ClipBoyMarauder::getMultiSSIDCount() {
    return wifi_scan_obj.getConfirmedMultiSSIDCount();
}

// ============================================================
// Group B: Real-Time Monitoring Data
// ============================================================

CBPacketCounters ClipBoyMarauder::getPacketCounters() {
    CBPacketCounters c;
    memset(&c, 0, sizeof(c));
    c.numBeacon    = num_beacon;
    c.numDeauth    = num_deauth;
    c.numProbe     = num_probe;
    c.numEapol     = num_eapol;
    c.mgmtFrames   = wifi_scan_obj.mgmt_frames;
    c.dataFrames   = wifi_scan_obj.data_frames;
    c.beaconFrames = wifi_scan_obj.beacon_frames;
    c.deauthFrames = wifi_scan_obj.deauth_frames;
    c.eapolFrames  = wifi_scan_obj.eapol_frames;
    c.reqFrames    = wifi_scan_obj.req_frames;    // Clip-Boy (DC34): probe req/resp for Raw/PCAP
    c.respFrames   = wifi_scan_obj.resp_frames;
    c.completeEapol = wifi_scan_obj.complete_eapol;
    c.saeFrames    = wifi_scan_obj.sae_frames;
    c.minRssi      = wifi_scan_obj.min_rssi;
    c.maxRssi      = wifi_scan_obj.max_rssi;
    return c;
}

void ClipBoyMarauder::resetPacketCounters() {
    num_beacon = 0;
    num_deauth = 0;
    num_probe = 0;
    num_eapol = 0;
    wifi_scan_obj.mgmt_frames = 0;
    wifi_scan_obj.data_frames = 0;
    wifi_scan_obj.beacon_frames = 0;
    wifi_scan_obj.req_frames = 0;
    wifi_scan_obj.resp_frames = 0;
    wifi_scan_obj.deauth_frames = 0;
    wifi_scan_obj.eapol_frames = 0;
    wifi_scan_obj.complete_eapol = 0;
    wifi_scan_obj.min_rssi = 0;
    wifi_scan_obj.max_rssi = -128;
}

// Clip-Boy local patch (DC34): one "standard event" reset, called on every tool start
// (dispatch_clipboy_action), so no tool displays a prior tool's leftovers. Clears the
// pure-OUTPUT accumulators only -- NOT the AP/STA/SSID lists, which are select-then-run
// INPUTS the user builds up and targets across tools (wiping those would delete a selection).
void ClipBoyMarauder::resetDisplayAccumulators() {
    resetPacketCounters();            // num_* + *_frames families
    wifi_scan_obj.clearBTDevices();   // was dead code
    wifi_scan_obj.clearFlockDevices();
    wifi_scan_obj.clearPwnagotchis();
    wifi_scan_obj.clearMacTracker();      // DETECT_FOLLOW top-10 table
    wifi_scan_obj.clearChannelActivity(); // channel bars + snapshot
}

CBChannelActivity ClipBoyMarauder::getChannelActivity() {
    CBChannelActivity ca;
    memset(&ca, 0, sizeof(ca));
    for (int i = 0; i < 14; i++) {
        uint8_t live = wifi_scan_obj.channel_activity[i];
        uint8_t snap = wifi_scan_obj.channel_activity_snapshot[i];
        ca.counts[i] = (live > 0) ? live : snap;
    }
    return ca;
}

// Clip-Boy local patch (DC34): expose the ranged-hop activity_page so the UI can cycle it.
// Channel Activity only sweeps 7 channels per page (page 1 = ch 1-7, page 2 = ch 8-14) and
// the vendored default is stuck at page 1, so channels 8-14 never sampled. Cycling the page
// lets the Channel Stats histogram cover all 14. Picked up on the next 100ms hop, no re-init.
void ClipBoyMarauder::setChannelActivityPage(uint8_t page) {
    if (page < 1) page = 1;
    if (page > 2) page = 2;                 // MAX_CHANNEL / CHAN_PER_PAGE = 2
    wifi_scan_obj.activity_page = page;
}
uint8_t ClipBoyMarauder::getChannelActivityPage() {
    return wifi_scan_obj.activity_page;
}

void ClipBoyMarauder::setRawCaptureChannel(uint8_t ch) {
    wifi_scan_obj.setRawChannel(ch);   // 0 = hop the band (default), 1-14 = lock the channel
}

// Clip-Boy (2026-07-28): deauth channel policy, shared by the Radiation gauge, Analyze >
// Deauth and the CLI `sniffdeauth` -- all three run WIFI_SCAN_DEAUTH.
void ClipBoyMarauder::setDeauthChannel(uint8_t mode) {
    wifi_scan_obj.setDeauthChannel(mode);   // 0=hop all, 200=1/6/11, 1-14=lock
}

uint8_t ClipBoyMarauder::getDeauthChannelMode() {
    return wifi_scan_obj.deauth_mode;
}

// The channel the radio is ACTUALLY on, read back from the driver rather than from our own
// bookkeeping: changeChannel() discards esp_wifi_set_channel()'s return value, so
// set_channel records what we asked for, not what took effect. A test that asserts the lock
// worked has to read this one.
uint8_t ClipBoyMarauder::getLiveChannel() {
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
    return primary;
}

CBAnalyzerData ClipBoyMarauder::getAnalyzerData() {
    CBAnalyzerData ad;
    memset(&ad, 0, sizeof(ad));
    ad.channel = wifi_scan_obj.set_channel;
    ad.value = wifi_scan_obj._analyzer_value;
    wifi_scan_obj.getAnalyzerName(ad.name, sizeof(ad.name));   // guarded snapshot (Clip-Boy DC34)
    ad.nameUpdated = wifi_scan_obj.analyzer_name_update;
    return ad;
}

uint8_t ClipBoyMarauder::getMACTrackerTop10(MacEntry* out, MacSortMode mode) {
    return wifi_scan_obj.build_top10_for_ui(out, mode);
}

// ============================================================
// Group C: State & Status
// ============================================================

CBHardwareState ClipBoyMarauder::getHardwareState() {
    CBHardwareState hs;
    memset(&hs, 0, sizeof(hs));
    hs.wifiInitialized = wifi_scan_obj.wifi_initialized;
    hs.bleInitialized = wifi_scan_obj.ble_initialized;
    hs.wifiConnected = wifi_scan_obj.wifi_connected;
    hs.sdMounted = sd_obj.supported;
    hs.bleScanning = wifi_scan_obj.ble_scanning;
    strncpy(hs.connectedNetwork, wifi_scan_obj.connected_network.c_str(), sizeof(hs.connectedNetwork) - 1);
    hs.channel = wifi_scan_obj.set_channel;
    return hs;
}

CBMemoryInfo ClipBoyMarauder::getMemoryInfo() {
    CBMemoryInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.freeHeap = ESP.getFreeHeap();
    mi.totalHeap = ESP.getHeapSize();
    #ifdef HAS_PSRAM
    mi.freePsram = ESP.getFreePsram();
    mi.totalPsram = ESP.getPsramSize();
    #endif
    if (mi.totalHeap > 0)
        mi.dramPercent = (uint8_t)(100 - (mi.freeHeap * 100 / mi.totalHeap));
    if (mi.totalPsram > 0)
        mi.psramPercent = (uint8_t)(100 - (mi.freePsram * 100 / mi.totalPsram));
    return mi;
}

bool ClipBoyMarauder::isWiFiInitialized() { return wifi_scan_obj.wifi_initialized; }
bool ClipBoyMarauder::isBLEInitialized()  { return wifi_scan_obj.ble_initialized; }
bool ClipBoyMarauder::isWiFiConnected()   { return wifi_scan_obj.wifi_connected; }

int ClipBoyMarauder::getPacketsSent() { return wifi_scan_obj.getPacketsSent(); }
int ClipBoyMarauder::getBTFrames()    { return wifi_scan_obj.bt_frames; }

int ClipBoyMarauder::getDeauthEventCount() {
    return wifi_scan_obj.deauth_ring_count;
}

bool ClipBoyMarauder::getDeauthEvent(int index, CBDeauthEvent& out) {
    if (index < 0 || index >= wifi_scan_obj.deauth_ring_count) return false;
    int pos = (wifi_scan_obj.deauth_ring_head - wifi_scan_obj.deauth_ring_count + index + CB_DEAUTH_RING_SIZE) % CB_DEAUTH_RING_SIZE;
    out = wifi_scan_obj.deauth_ring[pos];
    return true;
}

void ClipBoyMarauder::clearDeauthEvents() {
    wifi_scan_obj.deauth_ring_head = 0;
    wifi_scan_obj.deauth_ring_count = 0;
    wifi_scan_obj.deauth_total = 0;
}

uint32_t ClipBoyMarauder::getDeauthTotal() {
    return wifi_scan_obj.deauth_total;
}

// millis() of the last frame received from the SELECTED AP while Monitor > RSSI is running.
// The UI compares successive reads: a CHANGE means a frame arrived. See the member's comment in
// WiFiScan.h for why this is a plain counter rather than a field on the shared AP record.
uint32_t ClipBoyMarauder::getSigStrenLastRxMs() {
    return wifi_scan_obj.sig_stren_last_rx_ms;
}

const char* ClipBoyMarauder::scanModeToString(uint8_t mode) {
    switch (mode) {
        case WIFI_SCAN_OFF:                 return "Off";
        case WIFI_SCAN_PROBE:               return "Probe Sniff";
        case WIFI_SCAN_AP:                  return "Beacon Sniff";
        case WIFI_SCAN_PWN:                 return "Pwnagotchi Detect";
        case WIFI_SCAN_EAPOL:               return "EAPOL/PMKID Sniff";
        case WIFI_SCAN_DEAUTH:              return "Deauth Sniff";
        case WIFI_SCAN_ALL:                 return "Scan All";
        case WIFI_PACKET_MONITOR:           return "Packet Monitor";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_BEACON_SPAM:       return "Beacon Spam";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_RICK_ROLL:         return "Rick Roll";
#endif
        case BT_SCAN_ALL:                   return "BT Scan All";
        case BT_SCAN_SKIMMERS:             return "BT Skimmer Scan";
        case WIFI_SCAN_ESPRESSIF:           return "Espressif Scan";
        case LV_JOIN_WIFI:                  return "Join WiFi";
        case LV_ADD_SSID:                   return "Add SSID";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_BEACON_LIST:       return "Beacon List";
#endif
        case WIFI_SCAN_TARGET_AP:           return "Scan APs";
        case LV_SELECT_AP:                  return "Select AP";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_AUTH:              return "Auth Flood";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_MIMIC:             return "Mimic";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_DEAUTH:            return "Deauth";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_AP_SPAM:           return "AP Clone Spam";
#endif
        case WIFI_SCAN_TARGET_AP_FULL:      return "Full AP Scan";
#ifdef CLIPBOY_RES34RCH
        case WIFI_SCAN_ACTIVE_EAPOL:        return "Active EAPOL";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_DEAUTH_MANUAL:     return "Manual Deauth";
#endif
        case WIFI_SCAN_RAW_CAPTURE:         return "Raw Capture";
        case WIFI_SCAN_STATION:             return "Station Scan";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_DEAUTH_TARGETED:   return "Deauth Stations";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_SCAN_ACTIVE_LIST_EAPOL:   return "Active List EAPOL";
#endif
        case WIFI_SCAN_SIG_STREN:           return "Signal Monitor";
#ifdef CLIPBOY_RES34RCH
        case WIFI_SCAN_EVIL_PORTAL:         return "Evil Portal";
#endif
        case WIFI_SCAN_GPS_DATA:            return "GPS Data";
        case WIFI_SCAN_WAR_DRIVE:           return "War Drive";
        case WIFI_SCAN_STATION_WAR_DRIVE:   return "Station War Drive";
        case BT_SCAN_WAR_DRIVE:             return "BT War Drive";
        case BT_SCAN_WAR_DRIVE_CONT:        return "BT War Drive Cont";
#ifdef CLIPBOY_RES34RCH
        case BT_ATTACK_SOUR_APPLE:          return "Sour Apple";
#endif
#ifdef CLIPBOY_RES34RCH
        case BT_ATTACK_SWIFTPAIR_SPAM:      return "Swiftpair Spam";
#endif
#ifdef CLIPBOY_RES34RCH
        case BT_ATTACK_SPAM_ALL:            return "BT Spam All";
#endif
#ifdef CLIPBOY_RES34RCH
        case BT_ATTACK_SAMSUNG_SPAM:        return "Samsung Spam";
#endif
        case WIFI_SCAN_GPS_NMEA:            return "GPS NMEA";
#ifdef CLIPBOY_RES34RCH
        case BT_ATTACK_GOOGLE_SPAM:         return "Google Spam";
#endif
#ifdef CLIPBOY_RES34RCH
        case BT_ATTACK_FLIPPER_SPAM:        return "Flipper Spam";
#endif
        case BT_SCAN_AIRTAG:               return "AirTag Scan";
#ifdef CLIPBOY_RES34RCH
        case BT_SPOOF_AIRTAG:              return "AirTag Spoof";
#endif
        case BT_SCAN_FLIPPER:              return "Flipper Scan";
        case WIFI_SCAN_CHAN_ANALYZER:        return "Channel Analyzer";
        case BT_SCAN_ANALYZER:             return "BT Analyzer";
        case WIFI_SCAN_PACKET_RATE:         return "Packet Rate";
        case WIFI_SCAN_AP_STA:             return "AP+Station Scan";
        case WIFI_SCAN_PINESCAN:            return "PineScan";
        case WIFI_SCAN_MULTISSID:           return "Multi-SSID Scan";
        case WIFI_CONNECTED:                return "WiFi Connected";
        case WIFI_PING_SCAN:                return "Ping Scan";
        case WIFI_PORT_SCAN_ALL:            return "Port Scan";
        case GPS_TRACKER:                   return "GPS Tracker";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_BAD_MSG:           return "Bad Message";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_BAD_MSG_TARGETED:  return "Bad Msg Stations";
#endif
        case WIFI_SCAN_TELNET:              return "Telnet Scan";
        case WIFI_SCAN_SSH:                 return "SSH Scan";
        case WIFI_ARP_SCAN:                 return "ARP Scan";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_SLEEP:             return "Sleep Flood";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_SLEEP_TARGETED:    return "Sleep Stations";
#endif
        case GPS_POI:                       return "GPS POI";
        case WIFI_SCAN_DNS:                 return "DNS Scan";
        case WIFI_SCAN_HTTP:                return "HTTP Scan";
        case WIFI_SCAN_HTTPS:               return "HTTPS Scan";
        case WIFI_SCAN_SMTP:                return "SMTP Scan";
        case WIFI_SCAN_RDP:                 return "RDP Scan";
        case WIFI_HOSTSPOT:                 return "Hotspot";
        case BT_SCAN_AIRTAG_MON:            return "AirTag Monitor";
        case WIFI_SCAN_CHAN_ACT:            return "Channel Activity";
        case BT_SCAN_FLOCK:                return "Flock Scan";
        case BT_SCAN_SIMPLE:               return "BT Simple Scan";
        case BT_SCAN_SIMPLE_TWO:           return "BT Simple Scan 2";
        case BT_SCAN_FLOCK_WARDRIVE:       return "Flock War Drive";
        case WIFI_SCAN_DETECT_FOLLOW:       return "MAC Tracker";
        case WIFI_SCAN_SAE_COMMIT:          return "SAE Sniff";
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_SAE_COMMIT:        return "SAE Commit";
#endif
#ifdef CLIPBOY_RES34RCH
        case WIFI_ATTACK_FUNNY_BEACON:      return "Funny Beacon";
#endif
        default:                            return "Unknown";
    }
}

// ============================================================
// Group D: Evil Portal
// ============================================================

CBEvilPortalStatus ClipBoyMarauder::getEvilPortalStatus() {
    CBEvilPortalStatus ep;
    memset(&ep, 0, sizeof(ep));
    ep.active = (wifi_scan_obj.currentScanMode == WIFI_SCAN_EVIL_PORTAL);
    String uname = evil_portal_obj.get_user_name();
    String pwd = evil_portal_obj.get_password();
    ep.hasCredentials = (uname.length() > 0);
    strncpy(ep.userName, uname.c_str(), sizeof(ep.userName) - 1);
    strncpy(ep.password, pwd.c_str(), sizeof(ep.password) - 1);
    return ep;
}

// ============================================================
// Group E: Safe Operation Transitions
// ============================================================

bool ClipBoyMarauder::canStartOperation() {
    return wifi_scan_obj.currentScanMode == WIFI_SCAN_OFF;
}

bool ClipBoyMarauder::startOperation(uint8_t scanMode) {
    // Stop current operation if running
    if (wifi_scan_obj.currentScanMode != WIFI_SCAN_OFF) {
        wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
        delay(100); // Brief pause for cleanup
    }
    wifi_scan_obj.StartScan(scanMode, 0);
    _updateState();
    return true;
}

// ============================================================
// Group F: Selection Helpers
// ============================================================

void ClipBoyMarauder::selectAllAPs() {
    if (!access_points) return;
    for (int i = 0; i < access_points->size(); i++) {
        AccessPoint ap = access_points->get(i);
        ap.selected = true;
        access_points->set(i, ap);
    }
}

void ClipBoyMarauder::selectAllStations() {
    if (!stations) return;
    for (int i = 0; i < stations->size(); i++) {
        Station st = stations->get(i);
        st.selected = true;
        stations->set(i, st);
    }
}

void ClipBoyMarauder::deselectStations() {
    if (!stations) return;
    for (int i = 0; i < stations->size(); i++) {
        Station st = stations->get(i);
        st.selected = false;
        stations->set(i, st);
    }
}

int ClipBoyMarauder::getSelectedAPCount() {
    if (!access_points) return 0;
    int count = 0;
    for (int i = 0; i < access_points->size(); i++) {
        if (access_points->get(i).selected) count++;
    }
    return count;
}

int ClipBoyMarauder::getSelectedStationCount() {
    if (!stations) return 0;
    int count = 0;
    for (int i = 0; i < stations->size(); i++) {
        if (stations->get(i).selected) count++;
    }
    return count;
}

bool ClipBoyMarauder::isAPSelected(int index) {
    if (!access_points || index < 0 || index >= access_points->size()) return false;
    return access_points->get(index).selected;
}

bool ClipBoyMarauder::isStationSelected(int index) {
    if (!stations || index < 0 || index >= stations->size()) return false;
    return stations->get(index).selected;
}

// ============================================================
// Group G: Formatting Utilities
// ============================================================

String ClipBoyMarauder::macToString(const uint8_t mac[6]) {
    return ::macToString(mac);
}

String ClipBoyMarauder::securityToString(uint8_t secType) {
    return wifi_scan_obj.security_int_to_string(secType);
}

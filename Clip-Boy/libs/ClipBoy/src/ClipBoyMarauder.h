// ClipBoyMarauder - WiFi/BT Library Wrapper for ESP32Marauder
// Provides a clean API for WiFi scanning, active tools, Bluetooth, and Evil Portal
// without any UI dependencies.

#pragma once

#ifndef CLIPBOY_MARAUDER_H
#define CLIPBOY_MARAUDER_H

#include "configs.h"
#include "WiFiScan.h"
#include "EvilPortal.h"
#include "SDInterface.h"
#include "Buffer.h"
#include "settings.h"
#include "LedInterface.h"

enum class ClipBoyState : uint8_t {
    IDLE,
    SCANNING,
    TRANSMITTING,
    PORTAL_ACTIVE
};

// ============================================================
// UI-friendly data structs — flat, copyable, no String members.
// The UI copies data out of these; no lifetime management needed.
// ============================================================

struct CBAccessPointInfo {
    int     index;
    char    essid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
    uint8_t security;       // WIFI_SECURITY_* constant
    bool    wps;
    char    manufacturer[24];
    uint16_t packets;
    bool    selected;
    bool    has_handshake;  // has_msg_1 && has_msg_2 && has_msg_3 && has_msg_4
};

struct CBStationInfo {
    int     index;
    uint8_t mac[6];
    uint16_t packets;
    int     apIndex;        // index into AP list (-1 if none)
    bool    selected;
};

struct CBSSIDInfo {
    int     index;
    char    essid[33];
    uint8_t channel;
    uint8_t bssid[6];
    bool    selected;
};

struct CBAirTagInfo {
    int     index;
    char    mac[18];        // "XX:XX:XX:XX:XX:XX"
    int8_t  rssi;
    uint32_t lastSeen;      // millis
    bool    selected;
};

struct CBFlipperInfo {
    int     index;
    char    mac[18];
    char    name[33];
};

struct CBProbeSSIDInfo {
    int     index;
    char    essid[33];
    uint8_t requests;
    bool    selected;
};

struct CBPacketCounters {
    int     numBeacon;
    int     numDeauth;
    int     numProbe;
    int     numEapol;
    uint32_t mgmtFrames;
    uint32_t dataFrames;
    uint32_t beaconFrames;
    uint32_t deauthFrames;
    uint32_t eapolFrames;
    uint32_t reqFrames;     // Clip-Boy (DC34): probe-request frames (Raw/PCAP real count)
    uint32_t respFrames;    // Clip-Boy (DC34): probe-response frames
    uint32_t completeEapol;
    uint32_t saeFrames;     // Clip-Boy (DC34-146): SAE-commit frames
    int8_t  minRssi;
    int8_t  maxRssi;
};

struct CBChannelActivity {
    uint8_t counts[14];     // MAX_CHANNEL
};

struct CBAnalyzerData {
    uint8_t channel;
    int16_t value;          // _analyzer_value
    char    name[33];       // analyzer_name_string
    bool    nameUpdated;
};

struct CBHardwareState {
    bool    wifiInitialized;
    bool    bleInitialized;
    bool    wifiConnected;
    bool    sdMounted;
    bool    bleScanning;
    char    connectedNetwork[33];
    uint8_t channel;
};

struct CBEvilPortalStatus {
    bool    active;
    bool    hasCredentials;
    char    userName[128];
    char    password[128];
};

struct CBBTDeviceInfo {
    int     index;
    char    name[33];
    char    mac[18];
    int8_t  rssi;
};

struct CBFlockInfo {
    int      index;
    char     mac[18];
    char     name[33];
    char     serial[33];
    int8_t   rssi;
    // millis() of the most recent sighting. WITHOUT THIS THE UI CANNOT SEE FRESHNESS AT ALL:
    // FlockDevice::last_seen has existed and been populated all along, but it stopped at this
    // wire struct, so every consumer could only ever show first-contact data. That is what made
    // a dedup'd list indistinguishable from a hung scan.
    uint32_t last_seen;
};

struct CBPwnagotchiInfo {
    int     index;
    char    name[33];
    char    version[16];
    int     pwnd_tot;
    int     uptime;
    bool    deauth;
};

struct CBPinescanInfo {
    int     index;
    char    mac[18];
    char    detection_type[24];
    char    essid[33];
    uint8_t channel;
    int8_t  rssi;
};

struct CBMultiSSIDInfo {
    int     index;
    char    mac[18];
    char    essid[33];
    uint8_t channel;
    int8_t  rssi;
    uint8_t ssid_count;
};

// CBDeauthEvent + CB_DEAUTH_RING_SIZE defined in WiFiScan.h

struct CBMemoryInfo {
    uint32_t freeHeap;
    uint32_t totalHeap;
    uint32_t freePsram;
    uint32_t totalPsram;
    uint8_t  dramPercent;
    uint8_t  psramPercent;
};

class ClipBoyMarauder {
public:
    ClipBoyMarauder();

    // ---- Lifecycle ----
    bool begin();
    void loop();

    // ---- WiFi Scanning ----
    void scanAPs();
    void scanAPsFull();
    void scanAPsAndStations();
    void scanStations();
    void sniffBeacons();
    void sniffProbes();
    void sniffDeauth();
    void sniffEAPOL();
    void sniffPMKID(int channel = -1, bool withDeauth = false);
    void sniffPMKIDList();
    void sniffRaw();
    void sniffPwnagotchi();
    void sniffEspressif();
    void sniffPinescan();
    void sniffMultiSSID();
    void sniffSAE();
    void packetMonitor();
    void packetRate();
    void signalMonitor();
    void channelActivity();
    void macTracker();

    // ---- WiFi Active Tools ----
    void deauthAPs();
    void deauthManual();
    void deauthStations();
    void beaconSpamRandom();
    void beaconSpamList();
    void beaconSpamClone();
    void beaconRickRoll();
    void beaconFunny();
    void probeFlood();
    void authFlood();
    void badMsgFlood();
    void badMsgStations();
    void sleepFlood();
    void sleepStations();
    void saeCommitFlood();

    // ---- Bluetooth ----
    void btScanAll();
    void btScanSimple();
    void btScanSkimmers();
    void btScanAirtags();
    void btScanAirtagMonitor();
    void btScanFlippers();
    void btScanFlock();
    void btSpamApple();
    void btSpamWindows();
    void btSpamSamsung();
    void btSpamGoogle();
    void btSpamFlipper();
    void btSpamAll();

    // ---- Evil Portal ----
    void startEvilPortal(String htmlFile = "");
    void stopEvilPortal();

    // ---- Control ----
    void stopScan();
    // Clip-Boy (PCAP perf): finalize any open capture (drain + close the held pcap file
    // handle). Call from the UI's tool-stop path so a capture closes cleanly.
    void finishCapture();
    void setChannel(int channel);
    int  getChannel();
    bool joinWiFi(String ssid, String password);

    // ---- SSID Management ----
    void addSSID(String essid);
    void generateSSIDs(int count = 20);
    void clearSSIDs();
    int  getSSIDCount();

    // ---- AP Management ----
    void clearAPs();
    int  getAPCount();
    void selectAP(int index);
    void deselectAPs();

    // ---- Station Management ----
    void clearStations();
    int  getStationCount();
    void selectStation(int index);

    // ---- MAC ----
    void randomizeAPMac();
    void randomizeSTAMac();
    // Apply the staged AP+STA MACs to the live interfaces via WiFiScan::setMac().
    // Needed because EvilPortal::startAP() brings up its softAP with WiFi.softAP() and
    // never calls setMac() (unlike the raw-TX tool starts), so the portal otherwise ignores
    // Rnd/Set AP MAC. Call AFTER setRawTxMode(true) (APSTA up) and BEFORE the softAP.
    void applyStagedMacs();

    // ---- Info ----
    ClipBoyState getState();
    uint8_t getCurrentScanMode();
    bool isScanning();
    bool isSDSupported();
    String getFreeHeap();
    String getFreePSRAM();
    void printAPList();
    void printSSIDList();
    void printStationList();
    void printSettings();
    void printSystemInfo();

    // ---- Settings ----
    void toggleSetting(String name);
    void setSavePCAP(bool enabled);
    bool getSavePCAP();
    void setSDAvailable(bool available);  // Clip-Boy local patch (DC34-147): host badge owns SD.begin

    // ---- Scan Result Readers (Group A) ----
    bool getAP(int index, CBAccessPointInfo& out);
    bool getStation(int index, CBStationInfo& out);
    bool getSSID(int index, CBSSIDInfo& out);
    bool getAirTag(int index, CBAirTagInfo& out);
    int  getAirTagCount();
    bool getFlipper(int index, CBFlipperInfo& out);
    int  getFlipperCount();
    bool getProbeSSID(int index, CBProbeSSIDInfo& out);
    int  getProbeSSIDCount();

    // ---- BT / Detection Result Readers (Group A2) ----
    bool getBTDevice(int index, CBBTDeviceInfo& out);
    int  getBTDeviceCount();
    bool getFlockDevice(int index, CBFlockInfo& out);
    int  getFlockDeviceCount();
    bool getPwnagotchi(int index, CBPwnagotchiInfo& out);
    int  getPwnagotchiCount();
    bool getPinescan(int index, CBPinescanInfo& out);
    int  getPinescanCount();
    bool getMultiSSIDResult(int index, CBMultiSSIDInfo& out);
    int  getMultiSSIDCount();

    // ---- Real-Time Monitoring (Group B) ----
    CBPacketCounters getPacketCounters();
    void resetPacketCounters();
    void resetDisplayAccumulators();   // Clip-Boy (DC34): full clean-slate on every tool start
    CBChannelActivity getChannelActivity();
    void    setChannelActivityPage(uint8_t page);   // Clip-Boy local patch (DC34): 1=ch1-7, 2=ch8-14
    uint8_t getChannelActivityPage();
    void    setRawCaptureChannel(uint8_t ch);       // Clip-Boy (DC34): 0=hop the band, 1-14=lock
    // Clip-Boy (2026-07-28): deauth channel policy (0=hop all, 200=1/6/11, 1-14=lock).
    void    setDeauthChannel(uint8_t mode);
    uint8_t getDeauthChannelMode();
    uint8_t getLiveChannel();                       // from the driver, not our bookkeeping
    void    setRawTxMode(bool apOn);                // Clip-Boy (DC34): APSTA on-demand for WIFI_IF_AP raw TX
    CBAnalyzerData getAnalyzerData();
    uint8_t getMACTrackerTop10(MacEntry* out, MacSortMode mode);

    // ---- State & Status (Group C) ----
    CBHardwareState getHardwareState();
    CBMemoryInfo getMemoryInfo();
    bool isWiFiInitialized();
    bool isBLEInitialized();
    bool isWiFiConnected();
    const char* scanModeToString(uint8_t mode);
    int getPacketsSent();
    int getBTFrames();

    // Deauth event ring buffer
    int  getDeauthEventCount();
    bool getDeauthEvent(int index, CBDeauthEvent& out);
    void clearDeauthEvents();
    uint32_t getDeauthTotal();   // FB13: monotonic, never saturates
    uint32_t getSigStrenLastRxMs();  // last RSSI-target frame arrival (freshness signal)

    // ---- Evil Portal (Group D) ----
    CBEvilPortalStatus getEvilPortalStatus();

    // ---- Safe Operation Transitions (Group E) ----
    bool canStartOperation();
    bool startOperation(uint8_t scanMode);

    // ---- Selection Helpers (Group F) ----
    void selectAllAPs();
    void selectAllStations();
    void deselectStations();
    int  getSelectedAPCount();
    int  getSelectedStationCount();
    bool isAPSelected(int index);
    bool isStationSelected(int index);

    // ---- Formatting Utilities (Group G) ----
    String macToString(const uint8_t mac[6]);
    String securityToString(uint8_t secType);

private:
    ClipBoyState _state;
    void _updateState();
};

#endif // CLIPBOY_MARAUDER_H

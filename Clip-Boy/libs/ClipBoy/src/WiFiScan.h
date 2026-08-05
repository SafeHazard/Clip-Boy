#pragma once

#ifndef WiFiScan_h
#define WiFiScan_h

// Deauth event ring buffer — shared between WiFiScan and ClipBoyMarauder
#ifndef CB_DEAUTH_RING_SIZE
#define CB_DEAUTH_RING_SIZE 32
struct CBDeauthEvent {
    char    src[18];
    char    dst[18];
    uint8_t channel;
    int8_t  rssi;
};
#endif

#include "configs.h"
#include "utils.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

#ifdef HAS_BT
  #include <NimBLEDevice.h> // 1.3.8, 2.3.2
#endif

#ifdef HAS_IDF_3
  extern "C" {
    #include "esp_netif.h"
    #include "esp_netif_net_stack.h"
  }
#endif

#include <WiFi.h>
#include <ESP32Ping.h>
#include "EvilPortal.h"
#include <math.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <esp_timer.h>
#include "mbedtls/entropy.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#ifndef HAS_IDF_3
  #include <lwip/etharp.h>
  #include <lwip/ip_addr.h>
#endif
#ifdef HAS_IDF_3
  #include "esp_system.h"
  #include "esp_mac.h"
#endif
#if defined(HAS_BT) && !defined(HAS_NIMBLE_2)
  #include "esp_bt.h"
#endif
#ifdef HAS_SCREEN
  #include "Display.h"
#endif
#ifdef HAS_SD
  #include "SDInterface.h"
#endif
#include "Buffer.h"
#ifdef HAS_BATTERY
  #include "BatteryInterface.h"
#endif
#ifdef HAS_GPS
  #include "GpsInterface.h"
#endif
#include "settings.h"
#include "Assets.h"
#ifdef HAS_FLIPPER_LED
  #include "flipperLED.h"
#elif defined(XIAO_ESP32_S3)
  #include "xiaoLED.h"
#elif defined(MARAUDER_M5STICKC)
  #include "stickcLED.h"
#else
  #include "LedInterface.h"
#endif

#define bad_list_length 3

#define OTA_UPDATE 100
#define SHOW_INFO 101
#define ESP_UPDATE 102
#define WIFI_SCAN_OFF 0
#define WIFI_SCAN_PROBE 1
#define WIFI_SCAN_AP 2
#define WIFI_SCAN_PWN 3
#define WIFI_SCAN_EAPOL 4
#define WIFI_SCAN_DEAUTH 5
#define WIFI_SCAN_ALL 6
#define WIFI_PACKET_MONITOR 7
#define WIFI_ATTACK_BEACON_SPAM 8
#define WIFI_ATTACK_RICK_ROLL 9
#define BT_SCAN_ALL 10
#define BT_SCAN_SKIMMERS 11
#define WIFI_SCAN_ESPRESSIF 12
#define LV_JOIN_WIFI 13
#define LV_ADD_SSID 14
#define WIFI_ATTACK_BEACON_LIST 15
#define WIFI_SCAN_TARGET_AP 16
#define LV_SELECT_AP 17
#define WIFI_ATTACK_AUTH 18
#define WIFI_ATTACK_MIMIC 19
#define WIFI_ATTACK_DEAUTH 20
#define WIFI_ATTACK_AP_SPAM 21
#define WIFI_SCAN_TARGET_AP_FULL 22
#define WIFI_SCAN_ACTIVE_EAPOL 23
#define WIFI_ATTACK_DEAUTH_MANUAL 24
#define WIFI_SCAN_RAW_CAPTURE 25
#define WIFI_SCAN_STATION 26
#define WIFI_ATTACK_DEAUTH_TARGETED 27
#define WIFI_SCAN_ACTIVE_LIST_EAPOL 28
#define WIFI_SCAN_SIG_STREN 29
#define WIFI_SCAN_EVIL_PORTAL 30
#define WIFI_SCAN_GPS_DATA 31
#define WIFI_SCAN_WAR_DRIVE 32
#define WIFI_SCAN_STATION_WAR_DRIVE 33
#define BT_SCAN_WAR_DRIVE 34
#define BT_SCAN_WAR_DRIVE_CONT 35
#define BT_ATTACK_SOUR_APPLE 36
#define BT_ATTACK_SWIFTPAIR_SPAM 37
#define BT_ATTACK_SPAM_ALL 38
#define BT_ATTACK_SAMSUNG_SPAM 39
#define WIFI_SCAN_GPS_NMEA 40
#define BT_ATTACK_GOOGLE_SPAM 41
#define BT_ATTACK_FLIPPER_SPAM 42
#define BT_SCAN_AIRTAG 43
#define BT_SPOOF_AIRTAG 44
#define BT_SCAN_FLIPPER 45
#define WIFI_SCAN_CHAN_ANALYZER 46
#define BT_SCAN_ANALYZER 47
#define WIFI_SCAN_PACKET_RATE 48
#define WIFI_SCAN_AP_STA 49
#define WIFI_SCAN_PINESCAN 50
#define WIFI_SCAN_MULTISSID 51
#define WIFI_CONNECTED 52
#define WIFI_PING_SCAN 53
#define WIFI_PORT_SCAN_ALL 54
#define GPS_TRACKER 55
#define WIFI_ATTACK_BAD_MSG 56
#define WIFI_ATTACK_BAD_MSG_TARGETED 57
#define WIFI_SCAN_TELNET 58
#define WIFI_SCAN_SSH 59
#define WIFI_ARP_SCAN 60
#define WIFI_ATTACK_SLEEP 61
#define WIFI_ATTACK_SLEEP_TARGETED 62
#define GPS_POI 63
#define WIFI_SCAN_DNS 64
#define WIFI_SCAN_HTTP 65
#define WIFI_SCAN_HTTPS 66
#define WIFI_SCAN_SMTP 67
#define WIFI_SCAN_RDP 68
#define WIFI_HOSTSPOT 69 // Nice
#define BT_SCAN_AIRTAG_MON 70
#define WIFI_SCAN_CHAN_ACT 71
#define BT_SCAN_FLOCK 72
#define BT_SCAN_SIMPLE 73
#define BT_SCAN_SIMPLE_TWO 74
#define BT_SCAN_FLOCK_WARDRIVE 75
#define WIFI_SCAN_DETECT_FOLLOW 76
#define WIFI_SCAN_SAE_COMMIT 77
#define WIFI_ATTACK_SAE_COMMIT 78

#define WIFI_ATTACK_FUNNY_BEACON 99 

#define BASE_MULTIPLIER 4

#define ANALYZER_NAME_REFRESH 100 // Number of events to refresh the name

// PineScan and Multi SSID
#define MULTISSID_THRESHOLD 3 // Threshold For Multi SSID
#define MAX_MULTISSID_ENTRIES 100 // Max number of confirmed MultiSSIDs to store
#define MAX_AP_ENTRIES 100 // Max number of APs to track for analysis
#define MAX_DISPLAY_ENTRIES 1 // Max Unique MACs to display
#define MAX_PINESCAN_ENTRIES 100 // PineScan Max Entries

#define MAX_CHANNEL     14

// Clip-Boy (2026-07-28): deauth channel policy. NAMED constants, never a bare 255 --
// setRawChannel() clamps `ch > MAX_CHANNEL` to 14, so a raw sentinel passed through any
// channel-shaped parameter would silently become "locked to channel 14". Anything that
// accepts a mode must ALLOWLIST these two plus 1..MAX_CHANNEL and reject the rest, not
// range-check: a `> 14` test either admits a bad value or rejects TRI depending on its
// numeric value.
#define CB_DEAUTH_HOP_ALL  0     // walk 1..14 (stock behaviour)
#define CB_DEAUTH_HOP_TRI  200   // walk 1/6/11 only -- default
#define CB_DEAUTH_MODE_OK(v) ((v) == CB_DEAUTH_HOP_ALL || (v) == CB_DEAUTH_HOP_TRI || \
                              ((v) >= 1 && (v) <= MAX_CHANNEL))

#define MAX_PORT 65535

#define WIFI_SECURITY_OPEN   0
#define WIFI_SECURITY_WEP    1
#define WIFI_SECURITY_WPA    2
#define WIFI_SECURITY_WPA2   3
#define WIFI_SECURITY_WPA3   4
#define WIFI_SECURITY_WPA_WPA2_MIXED 5
#define WIFI_SECURITY_WPA2_ENTERPRISE 6
#define WIFI_SECURITY_WPA3_ENTERPRISE 7
#define WIFI_SECURITY_WAPI 8
#define WIFI_SECURITY_UNKNOWN 255

#define WPS_CONFIG_USBA              0x0001
#define WPS_CONFIG_ETHERNET          0x0002
#define WPS_CONFIG_LABEL             0x0004
#define WPS_CONFIG_DISPLAY           0x0008
#define WPS_CONFIG_EXT_NFC_TOKEN     0x0010
#define WPS_CONFIG_INT_NFC_TOKEN     0x0020
#define WPS_CONFIG_NFC_INTERFACE     0x0040
#define WPS_CONFIG_PUSH_BUTTON       0x0080
#define WPS_CONFIG_KEYPAD            0x0100
#define WPS_CONFIG_VIRT_PUSH_BUTTON  0x1000
#define WPS_CONFIG_PHY_PUSH_BUTTON   0x2000
#define WPS_CONFIG_VIRT_DISPLAY      0x4000
#define WPS_CONFIG_PHY_DISPLAY       0x8000

extern EvilPortal evil_portal_obj;

#ifdef HAS_SCREEN
  extern Display display_obj;
#endif
#ifdef HAS_SD
  extern SDInterface sd_obj;
#endif
#ifdef HAS_GPS
  extern GpsInterface gps_obj;
#endif
extern Buffer buffer_obj;
#ifdef HAS_BATTERY
  extern BatteryInterface battery_obj;
#endif
extern Settings settings_obj;
#ifdef HAS_FLIPPER_LED
  extern flipperLED flipper_led;
#elif defined(XIAO_ESP32_S3)
  extern xiaoLED xiao_led;
#elif defined(MARAUDER_M5STICKC)
  extern stickcLED stickc_led;
#else
  extern LedInterface led_obj;
#endif

esp_err_t esp_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);

#define EMPTY_ENTRY 0
#define VALID_ENTRY 1
#define TOMBSTONE_ENTRY 2

#pragma pack(push, 1)
struct MacEntry {
  uint8_t  mac[6];
  uint32_t last_seen_ms;
  uint16_t frame_count;
  int32_t  first_lat_e6;
  int32_t  first_lon_e6;
  int32_t  last_lat_e6;
  int32_t  last_lon_e6;
  bool following;
  int32_t dloc;
  int8_t rssi;
  bool bt;
};
#pragma pack(pop)

struct AirTag {
    String mac;                  // MAC address of the AirTag
    std::vector<uint8_t> payload; // Payload data
    uint16_t payloadSize;
    bool selected;
    int8_t rssi;
    uint32_t last_seen;
};

struct Flipper {
  String mac;
  String name;
};

struct BTDevice {
  String mac;
  String name;
  int8_t rssi;
  // Clip-Boy local patch (2026-07-26): needed so a FULL list can evict the OLDEST entry instead of
  // dropping new arrivals. Without it the cap meant "the first N distinct devices seen since the
  // scan started, forever" -- so in a busy room the list froze on whatever happened to be seen
  // first, which is the opposite of useful for a "what is around me" view. PwnagotchiDevice already
  // carried this field; BTDevice did not.
  uint32_t last_seen;
};

struct FlockDevice {
  String mac;
  String name;
  String serial;
  int8_t rssi;
  uint32_t last_seen;
};

struct PwnagotchiDevice {
  String name;
  String version;
  int pwnd_tot;
  int uptime;
  bool deauth;
  uint32_t last_seen;
};

#ifdef HAS_PSRAM
  extern struct mac_addr* mac_history;
#endif

enum class MacSortMode : uint8_t {
  MOST_RECENT,
  MOST_FRAMES
};

class WiFiScan
{
  private:
    // Wardriver thanks to https://github.com/JosephHewitt
    int arp_count = 0;
    #ifndef HAS_PSRAM
      struct mac_addr mac_history[mac_history_len];
    #endif

    int current_act_len = 0;

    uint32_t chanActTime = 0;

    uint8_t ap_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
    uint8_t sta_mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};

    // Settings
    uint mac_history_cursor = 0;
    uint8_t channel_hop_delay = 1;
  
    int x_pos; //position along the graph x axis
    float y_pos_x; //current graph y axis position of X value
    float y_pos_x_old = 120; //old y axis position of X value
    float y_pos_y; //current graph y axis position of Y value
    float y_pos_y_old = 120; //old y axis position of Y value
    float y_pos_z; //current graph y axis position of Z value
    float y_pos_z_old = 120; //old y axis position of Z value
    int midway = 0;
    byte x_scale = 1; //scale of graph x axis, controlled by touchscreen buttons
    byte y_scale = 1;

    bool do_break = false;

    bool wsl_bypass_enabled = false;

    bool scan_complete = false;

    //int num_beacon = 0; // GREEN
    //int num_probe = 0; // BLUE
    //int num_deauth = 0; // RED

    uint32_t initTime = 0;
    uint32_t last_ui_update = 0;
    uint32_t last_sour_apple_update = 0;
    bool run_setup = true;
    void initWiFi(uint8_t scan_mode);
    uint8_t bluetoothScanTime = 5;
    int packets_sent = 0;
    const wifi_promiscuous_filter_t filt = {.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    #ifdef HAS_BT
      NimBLEScan* pBLEScan;
    #endif

    //String connected_network = "";
    //const String alfa = "1234567890qwertyuiopasdfghjkklzxcvbnm QWERTYUIOPASDFGHJKLZXCVBNM_";

    const char* rick_roll[8] = {
      "01 Never gonna give you up",
      "02 Never gonna let you down",
      "03 Never gonna run around",
      "04 and desert you",
      "05 Never gonna make you cry",
      "06 Never gonna say goodbye",
      "07 Never gonna tell a lie",
      "08 and hurt you"
    };

    // H4W9 added Funny Beacon Spam
    const char* funny_beacon[12] = {
      "Abraham Linksys",
      "Benjamin FrankLAN",
      "Dora the Internet Explorer",
      "FBI Surveillance Van 4",
      "Get Off My LAN",
      "Loading...",
      "Martin Router King",
      "404 Wi-Fi Unavailable",
      "Test Wi-Fi Please Ignore",
      "This LAN is My LAN",
      "Titanic Syncing",
      "Winternet is Coming"
    };

    char* prefix = "G";

    typedef struct
    {
      int16_t fctl;
      int16_t duration;
      uint8_t da;
      uint8_t sa;
      uint8_t bssid;
      int16_t seqctl;
      unsigned char payload[];
    } __attribute__((packed)) WifiMgmtHdr;
    
    typedef struct {
      uint8_t payload[0];
      WifiMgmtHdr hdr;
    } wifi_ieee80211_packet_t;

		// Tracking structures for PineScan (similar to MultiSSID)
    struct PineScanTracker {
        uint8_t mac[6];
        bool suspicious_oui;
        bool tag_and_susp_cap;
        uint8_t channel;
        int8_t rssi;
        bool reported;
    };

    // For confirmed Pineapple devices
    struct ConfirmedPineScan {
        uint8_t mac[6];
        String detection_type;
        String essid;
        uint8_t channel;
        int8_t rssi;
        bool displayed;
    };
    LinkedList<PineScanTracker>* pinescan_trackers;
    LinkedList<ConfirmedPineScan>* confirmed_pinescan;
    bool pinescan_list_full_reported;
    
    // Security Conditions For Pineapple detection
    enum SecurityCondition {
        NONE = 0x00,
        SUSPICIOUS_WHEN_OPEN = 0x01,
        SUSPICIOUS_WHEN_PROTECTED = 0x02,
        SUSPICIOUS_ALWAYS = 0x04
    };

    // SuspiciousVendor struct
    struct SuspiciousVendor {
        const char* vendor_name;
        uint8_t security_flags;
        uint32_t ouis[20];                 // Array of OUIs (max 20 per vendor)
        uint8_t oui_count;                 // Number of OUIs for this vendor
    };

    // Declare the table for Pineapple
    static const SuspiciousVendor suspicious_vendors[];
    static const int NUM_SUSPICIOUS_VENDORS;

    // Track for AP list limit (Uninitialised, Done in RunSetup)
    bool ap_list_full_reported;

    // MULTI SSID STRUCTS

    struct MultiSSIDTracker {
        uint8_t mac[6];
        uint16_t ssid_hashes[MULTISSID_THRESHOLD];
        uint8_t unique_ssid_count;
        bool reported;
    };

    // New struct for confirmed MultiSSID devices
    struct ConfirmedMultiSSID {
        uint8_t mac[6];
        String essid;
        uint8_t channel;
        int8_t rssi;
        uint8_t ssid_count;
        bool displayed;
    };
    LinkedList<MultiSSIDTracker>* multissid_trackers;
    LinkedList<ConfirmedMultiSSID>* confirmed_multissid;
    bool multissid_list_full_reported;


    uint8_t sae_commit[32] = {
      0xb0, 0x00, 0x00, 0x00,                     // Type/Subtype, Duration
      0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,         // Destination
      0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,         // Source
      0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,         // BSSID (Destination)
      0x00, 0x00,                                 // Frag num
      0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x00  // Auth alg (SAE), SAE sequence, group 19
    };

    // barebones packet
    uint8_t packet[128] = { 0x80, 0x00, 0x00, 0x00, //Frame Control, Duration
                    /*4*/   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, //Destination address 
                    /*10*/  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, //Source address - overwritten later
                    /*16*/  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, //BSSID - overwritten to the same as the source address
                    /*22*/  0xc0, 0x6c, //Seq-ctl
                    /*24*/  0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, //timestamp - the number of microseconds the AP has been active
                    /*32*/  0x64, 0x00, //Beacon interval
                    /*34*/  0x01, 0x04, //Capability info
                    /* SSID */
                    /*36*/  0x00
                    };

    uint8_t prob_req_packet[128] = {0x40, 0x00, 0x00, 0x00, 
                                  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination
                                  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, // Source
                                  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Dest
                                  0x01, 0x00, // Sequence
                                  0x00, // SSID Parameter
                                  0x00, // SSID Length
                                  /* SSID */
                                  };

    uint8_t deauth_frame_default[26] = {
                              0xc0, 0x00, 0x3a, 0x01,
                              0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0xf0, 0xff, 0x02, 0x00
                          };

    uint8_t eapol_packet_bad_msg1[153] = {
                              0x08, 0x02,                         // Frame Control (EAPOL)
                              0x00, 0x00,                         // Duration
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination (Broadcast)
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source (BSSID)
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
                              0x30, 0x00,                         // Sequence Control
                              /* LLC / SNAP */
                              0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00,
                              0x88, 0x8e,                          // Ethertype = EAPOL
                              /* -------- 802.1X Header -------- */
                              0x02,                               // Version 802.1X‑2004
                              0x03,                               // Type Key
                              0x00, 0x75,                          // Length 117 bytes
                              /* -------- EAPOL‑Key frame body (117 B) -------- */
                              0x02,                               // Desc Type 2 (AES/CCMP)
                              0x00, 0xCA,                          // Key Info (Install|Ack…)
                              0x00, 0x10,                          // Key Length = 16
                              /* Replay Counter (8) */
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                              /* Nonce (32) */
                              0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                              0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                              0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                              0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
                              /* Key IV (16) */
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              /* Key RSC (8) */
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              /* Key ID  (8) */ 
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              /* Key MIC (16) */ 
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                              /* Key Data Len (2) */ 
                              0x00, 0x16,
                              /* Key Data (22 B) */
                              0xDD, 0x14,                // Vendor‑specific (PMKID IE)
                              0x00, 0x0F, 0xAC, 0x04,      // OUI + Type (PMKID)
                              /* PMKID (16 byte zero) */
                              0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 
                              0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11
                          };

    uint8_t association_packet[200] = {
                              0x00, 0x10, // Frame Control (Association Request) PM=1
                              0x3a, 0x01, // Duration
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination (Broadcast)
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source (Fake Source or BSSID)
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
                              0x00, 0x00,                         // Sequence Control
                              0x31, 0x00,                         // Capability Information (PM=1)
                              0x0a, 0x00,                         // Listen Interval
                              0x00,                               // SSID tag
                              0x00,                               // SSID length      
                          };

    enum EBLEPayloadType
    {
      Microsoft,
      Apple,
      Samsung,
      Google,
      FlipperZero,
      Airtag
    };

      #ifdef HAS_BT

      struct BLEData
      {
        NimBLEAdvertisementData AdvData;
        NimBLEAdvertisementData ScanData;
      };

      struct WatchModel
      {
          uint8_t value;
          const char *name;
      };

      WatchModel* watch_models = nullptr;

      static void scanCompleteCB(BLEScanResults scanResults);
      NimBLEAdvertisementData GetUniversalAdvertisementData(EBLEPayloadType type);
    #endif

    void updateTrackerUI();
    void showNetworkInfo();
    void setNetworkInfo();
    void fullARP();
    bool readARP(IPAddress targ_ip);
    bool singleARP(IPAddress ip_addr);
    void pingScan(uint8_t scan_mode = WIFI_PING_SCAN);
    void portScan(uint8_t scan_mode = WIFI_PORT_SCAN_ALL, uint16_t targ_port = 22);
    bool isHostAlive(IPAddress ip);
    bool checkHostPort(IPAddress ip, uint16_t port, uint16_t timeout = 100);
    String extractManufacturer(const uint8_t* payload, int len);
    int checkMatchAP(char addr[], bool update_ap = true);
    bool beaconHasWPS(const uint8_t* payload, int len);
    uint8_t getSecurityType(const uint8_t* beacon, uint16_t len);
    void addAnalyzerValue(int16_t value, int rssi_avg, int16_t target_array[], int array_size);
    bool mac_cmp(struct mac_addr addr1, struct mac_addr addr2);
    bool mac_cmp(uint8_t addr1[6], uint8_t addr2[6]);
    void clearMacHistory();
    void executeWarDrive();
    void executeSourApple();
    void executeSpoofAirtag();
    void executeSwiftpairSpam(EBLEPayloadType type);
    void startWardriverWiFi();
    void saeAttackLoop(uint32_t currentTime);
    //void generateRandomMac(uint8_t* mac);
    //void generateRandomName(char *name, size_t length);
    String processPwnagotchiBeacon(const uint8_t* frame, int length);

    void startWiFiAttacks(uint8_t scan_mode, uint16_t color, String title_string);

    void signalAnalyzerLoop(uint32_t tick);
    void channelAnalyzerLoop(uint32_t tick);
    void channelActivityLoop(uint32_t tick);
    void packetRateLoop(uint32_t tick);
    void packetMonitorMain(uint32_t currentTime);
    void eapolMonitorMain(uint32_t currentTime);
    void updateMidway();
    void tftDrawXScalButtons();
    void tftDrawYScaleButtons();
    void tftDrawChannelScaleButtons();
    void tftDrawColorKey();
    void tftDrawGraphObjects();
    bool sendSAECommitFrame(uint8_t* targ_addr, uint8_t* src_addr) ;
    void sendProbeAttack(uint32_t currentTime);
    void sendDeauthAttack(uint32_t currentTime, String dst_mac_str = "ff:ff:ff:ff:ff:ff");
    void sendBadMsgAttack(uint32_t currentTime, bool all = false);
    void sendAssocSleepAttack(uint32_t currentTime, bool all = false);
    void sendDeauthFrame(uint8_t bssid[6], int channel, String dst_mac_str = "ff:ff:ff:ff:ff:ff");
    void sendDeauthFrame(uint8_t bssid[6], int channel, uint8_t mac[6]);
    void sendEapolBagMsg1(uint8_t bssid[6], int channel, String dst_mac_str = "ff:ff:ff:ff:ff:ff", uint8_t sec = WIFI_SECURITY_WPA2);
    void sendEapolBagMsg1(uint8_t bssid[6], int channel, uint8_t mac[6], uint8_t sec = WIFI_SECURITY_WPA2);
    void sendAssociationSleep(const char* ESSID, uint8_t bssid[6], int channel, uint8_t mac[6]);
    void sendAssociationSleep(const char* ESSID, uint8_t bssid[6], int channel, String dst_mac_str = "ff:ff:ff:ff:ff:ff");
    void broadcastRandomSSID(uint32_t currentTime);
    void broadcastCustomBeacon(uint32_t current_time, ssid custom_ssid);
    void broadcastCustomBeacon(uint32_t current_time, AccessPoint custom_ssid);
    void broadcastSetSSID(uint32_t current_time, const char* ESSID);
    void RunAPScan(uint8_t scan_mode, uint16_t color);
    void RunGPSNmea();
    void RunMimicFlood(uint8_t scan_mode, uint16_t color);
    void RunPwnScan(uint8_t scan_mode, uint16_t color);
    void RunPineScan(uint8_t scan_mode, uint16_t color);
    void RunMultiSSIDScan(uint8_t scan_mode, uint16_t color);
    void RunBeaconScan(uint8_t scan_mode, uint16_t color);
    void RunRawScan(uint8_t scan_mode, uint16_t color);
    void RunStationScan(uint8_t scan_mode, uint16_t color);
    void RunDeauthScan(uint8_t scan_mode, uint16_t color);
    void RunEapolScan(uint8_t scan_mode, uint16_t color);
    void RunProbeScan(uint8_t scan_mode, uint16_t color);
    void RunSAEScan(uint8_t scan_mode, uint16_t color);
    void RunPacketMonitor(uint8_t scan_mode, uint16_t color);
    void RunBluetoothScan(uint8_t scan_mode, uint16_t color);
    void RunSourApple(uint8_t scan_mode, uint16_t color);
    void RunSwiftpairSpam(uint8_t scan_mode, uint16_t color);
    void RunEvilPortal(uint8_t scan_mode, uint16_t color);
    void RunPingScan(uint8_t scan_mode, uint16_t color);
    void RunPortScanAll(uint8_t scan_mode, uint16_t color);
    bool checkMem();
    void parseBSSID(const char* bssidStr, uint8_t* bssid);
    void writeHeader(bool poi = false);
    void writeFooter(bool poi = false);
    void displayWardriveStats();


  public:
    WiFiScan();
#ifdef TEST_HARNESS
    // Directed Bad Msg / Sleep for the badmsg_sta / sleep_sta harness commands -- thin public
    // wrappers over the private senders so the test harness can burst to ONE explicit client on
    // an explicit channel, bypassing the AP-list duplicate-entry channel bug (ch6 vs the real
    // ch11) that makes Bad Msg/Sleep Target miss. TEST BUILDS ONLY -- adds no capability to a
    // shipping binary (the senders it calls already exist and are used by the AP-list tools).
    void txBadMsgDirected(uint8_t bssid[6], int channel, uint8_t mac[6]) { sendEapolBagMsg1(bssid, channel, mac); }
    void txSleepDirected(const char *essid, uint8_t bssid[6], int channel, uint8_t mac[6]) { sendAssociationSleep(essid, bssid, channel, mac); }
#endif

    //AccessPoint ap_list;

    //LinkedList<ssid>* ssids;

    // ClipBoy FB10: dedup-and-cap insert for bt_devices (definition in WiFiScan.cpp).
    // PUBLIC because two of the four call sites are NimBLE advertised-device callbacks defined
    // outside the class, which reach it via the global wifi_scan_obj.
    void addOrUpdateBTDevice(const String &mac, const String &name, int8_t rssi);
    // Returns true if `fd` was appended, false if it was a known MAC or the list was full.
    // DEDUP-BY-SKIP + DROP-NEW, deliberately NOT the evict-oldest shape used for BT/pwnagotchi
    // -- see the rationale block above the definition in WiFiScan.cpp.
    bool addFlockDeviceDeduped(const FlockDevice &fd);

    volatile bool bt_cb_busy = false;
    volatile bool bt_pending_clear = false;

    bool send_deauth = false;


    static MacEntry mac_entries[mac_history_len_half];
    static uint8_t mac_entry_state[mac_history_len_half];

    uint8_t dual_band_channels[DUAL_BAND_CHANNELS] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177};

    uint8_t dual_band_channel_index = 0;

    // Stuff for RAW stats
    uint32_t mgmt_frames = 0;
    uint32_t data_frames = 0;
    uint32_t beacon_frames = 0;
    uint32_t req_frames = 0;
    uint32_t resp_frames = 0;
    uint32_t deauth_frames = 0;
    CBDeauthEvent deauth_ring[CB_DEAUTH_RING_SIZE];
    uint8_t  deauth_ring_head = 0;   // next write position
    uint8_t  deauth_ring_count = 0;  // number of entries (max CB_DEAUTH_RING_SIZE)
    // Clip-Boy local patch (audit 2026-07-24 FB13): MONOTONIC count of every deauth ever
    // seen this run. deauth_ring_count SATURATES at CB_DEAUTH_RING_SIZE, so a UI poller that
    // used it as a high-watermark stopped emitting lines forever once the ring filled
    // (32 > 32 is false). This never saturates, so the poller can tell how many arrived --
    // including how many it MISSED when more than a ringful landed between polls.
    uint32_t deauth_total = 0;
    // Clip-Boy local patch (2026-07-26): millis() of the last frame received FROM THE SELECTED AP
    // while Monitor > RSSI is running. PUBLIC, alongside deauth_total, because the wrapper reads it
    // directly (a first attempt put it near initTime, which is private -- the compiler caught it).
    //
    // This is the ONLY freshness signal available for that tool. `ap.packets` is incremented solely
    // by checkMatchAP() on the AP-SCAN path and is never touched by the SIG_STREN handler, so it
    // does not advance during RSSI monitoring at all (tried; measured on hardware). And `ap.rssi` is
    // rewritten only when the value MOVES -- the live handler's filter is +/-1 dBm -- so a steady
    // target updates nothing and is indistinguishable from a dead one.
    // ⚠ An earlier version of this comment said +/-5 dBm. That was read off the DEAD nested handler;
    // the live one filters at +/-1. Same conclusion either way, but the number was wrong.
    //
    // Deliberately a plain counter rather than a field on the shared AP record: incrementing
    // something inside `access_points` from a promiscuous RX callback would add ~10 writes/sec to
    // the exact structure whose unsynchronised access we chose to guard-but-not-fix (FB9). This is
    // one 32-bit store per matched frame and touches nothing shared.
    volatile uint32_t sig_stren_last_rx_ms = 0;
    uint32_t eapol_frames = 0;
    uint32_t complete_eapol = 0;
    uint32_t sae_frames = 0;         // Clip-Boy (DC34-146): SAE-commit frames seen
    int8_t min_rssi = 0;
    int8_t max_rssi = -128;

    int bt_frames = 0;

    // BT device storage (accessed from BLE callbacks)
    LinkedList<BTDevice>* bt_devices;
    LinkedList<FlockDevice>* flock_devices;
    LinkedList<PwnagotchiDevice>* pwnagotchis;

    bool force_pmkid = false;
    bool force_probe = false;
    bool save_pcap = false;
    bool ep_deauth = false;
    bool ble_scanning = false;

    char* flock_ssid[4] = {
      "flock",
      "penguin",
      "pigvision",
      "fs ext battery"
    };

    #ifdef HAS_DUAL_BAND
      uint8_t channel_activity[DUAL_BAND_CHANNELS] = {};
      uint8_t channel_activity_snapshot[DUAL_BAND_CHANNELS] = {};
    #else
      uint8_t channel_activity[MAX_CHANNEL] = {};
      uint8_t channel_activity_snapshot[MAX_CHANNEL] = {};  // stable copy for UI
    #endif

    uint8_t activity_page = 1;

    // Clip-Boy local patch (DC34): fixed buffer (not Arduino String) written by the
    // Channel-Analyzer RX callback (WiFi task) and read by the main task via the
    // guarded setAnalyzerName/getAnalyzerName accessors -> no cross-task String UAF.
    char analyzer_name_string[33] = "";
    void setAnalyzerName(const char *s);
    void getAnalyzerName(char *out, size_t n);

    uint8_t analyzer_frames_recvd = 0;

    bool analyzer_name_update = false;

    uint8_t set_channel = 1;

    uint8_t old_channel = 0;

    int16_t _analyzer_value = 0;

    bool orient_display = false;
    bool wifi_initialized = false;
    bool ble_initialized = false;
    bool wifi_connected = false;

    String free_ram = "";
    String old_free_ram = "";
    String connected_network = "";

    IPAddress ip_addr;
    IPAddress gateway;
    IPAddress subnet;

    IPAddress current_scan_ip;

    uint16_t current_scan_port = 1;

    String dst_mac = "ff:ff:ff:ff:ff:ff";
    byte src_mac[6] = {};

    #ifdef HAS_SCREEN
      int16_t _analyzer_values[TFT_WIDTH];
      int16_t _temp_analyzer_values[TFT_WIDTH];
    #endif

    String current_mini_kb_ssid = "";

    const String alfa = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789-=[];',./`\\_+{}:\"<>?~|!@#$%^&*()";

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    #ifndef HAS_IDF_3
      wifi_init_config_t cfg2 = { \
          .event_handler = &esp_event_send_internal, \
          .osi_funcs = &g_wifi_osi_funcs, \
          .wpa_crypto_funcs = g_wifi_default_wpa_crypto_funcs, \
          .static_rx_buf_num = 6,\
          .dynamic_rx_buf_num = 6,\
          .tx_buf_type = 0,\
          .static_tx_buf_num = 1,\
          .dynamic_tx_buf_num = WIFI_DYNAMIC_TX_BUFFER_NUM,\
          .cache_tx_buf_num = 0,\
          .csi_enable = false,\
          .ampdu_rx_enable = false,\
          .ampdu_tx_enable = false,\
          .amsdu_tx_enable = false,\
          .nvs_enable = false,\
          .nano_enable = WIFI_NANO_FORMAT_ENABLED,\
          .rx_ba_win = 6,\
          .wifi_task_core_id = WIFI_TASK_CORE_ID,\
          .beacon_max_len = 752, \
          .mgmt_sbuf_num = 8, \
          .feature_caps = g_wifi_feature_caps, \
          .sta_disconnected_pm = WIFI_STA_DISCONNECTED_PM_ENABLED,  \
          .espnow_max_encrypt_num = 0, \
          .magic = WIFI_INIT_CONFIG_MAGIC\
      };
    #else
      wifi_country_t country = {
        .cc = "PH",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
      };

      wifi_init_config_t cfg2 = WIFI_INIT_CONFIG_DEFAULT();
    #endif

    wifi_config_t ap_config;

    uint32_t getCompleteEapol(int check_index = -1);
    void drawChannelLine();
    #ifdef HAS_SCREEN
      int8_t checkAnalyzerButtons(uint32_t currentTime);
    #endif
    bool seen_mac(unsigned char* mac, bool simple = true);
    int16_t seen_mac_int(unsigned char* mac, bool simple = true);
    int update_mac_entry(const uint8_t mac[6], int8_t rssi = 0, bool bt = false);
    inline void insert_mac_entry(uint32_t idx, const uint8_t mac[6], uint32_t now_ms, int8_t rssi = 0, bool bt = false);
    void evict_and_insert(const uint8_t mac[6], uint32_t now_ms);
    uint8_t build_top10_for_ui(MacEntry* out_top10, MacSortMode mode);
    void save_mac(unsigned char* mac);
    #ifdef HAS_BT
      void copyNimbleMac(const BLEAddress &addr, unsigned char out[6]);
    #endif
    bool filterActive();
    bool RunGPSInfo(bool tracker = false, bool display = true, bool poi = false);
    void logPoint(String lat, String lon, float alt, String datetime, bool poi = false);
    void setMac();
    void renderRawStats();
    void renderPacketRate();
    void displayAnalyzerString(String str);
    String security_int_to_string(int security_type);
    char* stringToChar(String string);
    void RunSetup();
    int clearSSIDs();
    int clearAPs();
    int clearIPs();
    int clearAirtags();
    int clearFlippers();
    int clearStations();
    int clearPineScanTrackers();
    int clearMultiSSID();
    int clearBTDevices();
    int clearFlockDevices();
    int clearPwnagotchis();
    void clearMacTracker();       // Clip-Boy (DC34): wipe the DETECT_FOLLOW mac_entries table
    void clearChannelActivity();  // Clip-Boy (DC34): wipe channel_activity + its UI snapshot
    void setRawChannel(uint8_t ch);   // Clip-Boy (DC34): 0 = hop the band, 1-14 = lock Raw/PCAP

    // Clip-Boy (2026-07-28): channel policy for WIFI_SCAN_DEAUTH -- shared by the Radiation
    // gauge, Tools > Analyze > Deauth, and the CLI `sniffdeauth` (all three call StartScan
    // with the same mode). Values: CB_DEAUTH_HOP_ALL / CB_DEAUTH_HOP_TRI / 1..14 = lock.
    //
    // ⚠ The AUTHORITATIVE copy of the locked channel is this member, not the shared
    // `set_channel`. (changeChannel() does still assign set_channel -- so set_channel is
    // written on every tune, and Utilities > Set Channel will render whatever a deauth run
    // last left there, exactly as it does after any other hopping tool. What matters is that
    // we never READ set_channel to decide where the lock should be.) `set_channel` is walked by every other tool's channelHop(), so storing
    // the lock there means "lock ch 6 -> run Scan APs -> come back -> Start" silently listens
    // on whatever channel the last tool left behind, while the dropdown still reads ch 6.
    // Keeping our own copy also lets RunDeauthScan re-assert it on EVERY start, which is what
    // makes the CLI starter inherit the setting for real rather than in principle.
    void setDeauthChannel(uint8_t mode);
    void applyDeauthChannel();        // re-assert at scan start (all 3 starters)
    uint8_t deauth_mode = CB_DEAUTH_HOP_TRI;  // default 1/6/11 -- see applyDeauthChannel()
    uint8_t deauth_tri_i = 0;         // step index for the 1/6/11 walk; reset by setDeauthChannel
    bool raw_hop = true;              // Clip-Boy (DC34): Raw/PCAP hops when true (UI "Hop (all)")
    bool eapol_hop = true;            // Clip-Boy (DC34): EAPOL hops only when NO target AP is selected

    // BT device getters (for BT_SCAN_ALL / BT_SCAN_SKIMMERS)
    int getBTDeviceCount() { return bt_devices ? bt_devices->size() : 0; }
    bool getBTDeviceEntry(int index, BTDevice& out) {
        if (!bt_devices || index < 0 || index >= bt_devices->size()) return false;
        out = bt_devices->get(index);
        return true;
    }

    // Flock device getters
    int getFlockDeviceCount() { return flock_devices ? flock_devices->size() : 0; }
    bool getFlockDeviceEntry(int index, FlockDevice& out) {
        if (!flock_devices || index < 0 || index >= flock_devices->size()) return false;
        out = flock_devices->get(index);
        return true;
    }

    // Pwnagotchi getters
    int getPwnagotchiCount() { return pwnagotchis ? pwnagotchis->size() : 0; }
    bool getPwnagotchiEntry(int index, PwnagotchiDevice& out) {
        if (!pwnagotchis || index < 0 || index >= pwnagotchis->size()) return false;
        out = pwnagotchis->get(index);
        return true;
    }

    // PineScan confirmed results getters
    int getConfirmedPinescanCount() { return confirmed_pinescan ? confirmed_pinescan->size() : 0; }
    bool getConfirmedPinescanEntry(int index, uint8_t mac_out[6],
                                    String& type_out, String& essid_out,
                                    uint8_t& ch_out, int8_t& rssi_out) {
        if (!confirmed_pinescan || index < 0 || index >= confirmed_pinescan->size()) return false;
        ConfirmedPineScan ps = confirmed_pinescan->get(index);
        memcpy(mac_out, ps.mac, 6);
        type_out = ps.detection_type;
        essid_out = ps.essid;
        ch_out = ps.channel;
        rssi_out = ps.rssi;
        return true;
    }

    // MultiSSID confirmed results getters
    int getConfirmedMultiSSIDCount() { return confirmed_multissid ? confirmed_multissid->size() : 0; }
    bool getConfirmedMultiSSIDEntry(int index, uint8_t mac_out[6],
                                     String& essid_out, uint8_t& ch_out,
                                     int8_t& rssi_out, uint8_t& count_out) {
        if (!confirmed_multissid || index < 0 || index >= confirmed_multissid->size()) return false;
        ConfirmedMultiSSID ms = confirmed_multissid->get(index);
        memcpy(mac_out, ms.mac, 6);
        essid_out = ms.essid;
        ch_out = ms.channel;
        rssi_out = ms.rssi;
        count_out = ms.ssid_count;
        return true;
    }
    bool addSSID(String essid);
    int generateSSIDs(int count = 20);
    bool shutdownWiFi();
    bool shutdownBLE();
    bool scanning();
    bool joinWiFi(String ssid, String password, bool gui = true);
    bool startWiFi(String ssid, String password, bool gui = true);
    void getMAC(bool get_sta, uint8_t* mac);
    String freeRAM();
    void changeChannel();
    void changeChannel(int chan);
    void RunAPInfo(uint16_t index, bool do_display = true);
    void RunInfo();
    //void RunShutdownBLE();
    void RunSetMac(uint8_t * mac, bool ap = true);
    void RunGenerateRandomMac(bool ap = true);
    void RunGenerateSSIDs(int count = 20);
    void RunClearSSIDs();
    void RunClearAPs();
    void RunClearStations();
    void RunSaveSSIDList(bool save_as = true);
    void RunLoadSSIDList();
    void RunSaveAPList(bool save_as = true);
    void RunLoadAPList();
    void RunSaveATList(bool save_as = true);
    void RunLoadATList();
    void RunSetupGPSTracker(uint8_t scan_mode);
    void channelHop(bool filtered = false, bool ranged = false);
    uint8_t currentScanMode = 0;
    void main(uint32_t currentTime);
    void StartScan(uint8_t scan_mode, uint16_t color = 0);
    void StopScan(uint8_t scan_mode);
    void setBaseMacAddress(uint8_t macAddr[6]);
    //const char* generateRandomName();

    bool save_serial = false;
    void startPcap(String file_name);
    void startLog(String file_name);
    void startGPX(String file_name);
    //String macToString(const Station& station);

    static bool initMbedtls();
    static int mbedtls_entropy_source(void *data, unsigned char *output, size_t len);
    static bool getSAEACT(const uint8_t *frame, size_t frame_len, uint16_t &group_out, size_t &act_len_out);
    static bool sae_group_sizes(uint16_t group, size_t &scalar_len, size_t &element_len);
    static bool mac_cmp(const uint8_t *a, const uint8_t *b);
    static inline uint16_t le16(const uint8_t *p);
    static void getMAC(char *addr, uint8_t* data, uint16_t offset);
    static void getMAC(uint8_t* mac, const uint8_t* data, uint16_t offset);
    static void pwnSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void beaconSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    //static void rawSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void stationSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void apSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void apSnifferCallbackFull(void* buf, wifi_promiscuous_pkt_type_t type);
    static void deauthSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    //static void probeSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void beaconListSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void activeEapolSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void eapolSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void pineScanSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type); // Pineapple
    static int extractPineScanChannel(const uint8_t* payload, int len); // Pineapple
    static void multiSSIDSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type); // MultiSSID
    static inline uint32_t hash_mac(const uint8_t mac[6]);

#ifdef CLIPBOY_ESP32S3
    int getPacketsSent() const { return packets_sent; }
#endif
};
#endif

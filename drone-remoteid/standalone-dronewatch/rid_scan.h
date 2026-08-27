#pragma once
// =====================================================================
// Standalone UAV Remote ID scanner engine (ASTM F3411 / Open Drone ID).
// Receives over WiFi (beacon vendor IE + NAN) and Bluetooth LE, decodes
// with opendroneid-core-c, and maintains a live drone table.
//
// No badge / mesh / LVGL dependencies — pure receiver. The UI polls the
// accessors below. Receive-only; transmits nothing.
// =====================================================================
#include <stdint.h>
#include <stdbool.h>
#include "src/odid/opendroneid.h"

#define RID_MAX_DRONES     16
#define RID_EXPIRE_MS      90000
#define RID_WIFI_WINDOW_MS 1000
#define RID_BLE_WINDOW_MS  1200
#define RID_HOP_MS         300

enum RidSrc : uint8_t {
    RID_SRC_WIFI_BEACON = 0,
    RID_SRC_WIFI_NAN,
    RID_SRC_BT_LEGACY,
    RID_SRC_BT_EXTENDED,
};

struct RidDrone {
    bool     inUse;
    uint8_t  mac[6];
    char     uasId[ODID_ID_SIZE + 1];
    char     operatorId[ODID_ID_SIZE + 1];
    char     selfDesc[ODID_STR_SIZE + 1];
    uint8_t  idType;
    uint8_t  uaType;
    uint8_t  hasSerial;
    uint8_t  status;
    double   lat, lon;
    float    altGeo;
    float    height;
    float    speedH;
    float    direction;
    double   opLat, opLon;
    int8_t   rssi;
    uint8_t  src;
    uint8_t  channel;
    uint32_t lastSeenMs;
    uint32_t packets;
};

// Bring up the BLE controller early (before big allocations), if possible.
void rid_earlyInit(void);

// Start WiFi promiscuous + BLE scanning.
bool rid_begin(void);

// Call every loop iteration: advances the radio time-slicer and drains the
// decode queue into the drone table. Cheap when idle.
void rid_service(void);

// Accessors (call from the UI / main thread only).
int             rid_droneCount(void);
const RidDrone* rid_getDrone(int idx);       // idx 0..RID_MAX_DRONES-1, NULL if free
uint32_t        rid_wifiFrames(void);
uint32_t        rid_btFrames(void);
uint32_t        rid_bleAdvRaw(void);
uint8_t         rid_currentChannel(void);    // 0 == Bluetooth window
bool            rid_bleReady(void);
bool            rid_wifiReady(void);
// Returns true once (and clears) when a brand-new drone was just added.
bool            rid_takeNewDetection(void);

#pragma once
// =====================================================================
// Remote ID drone store — self-contained ASTM F3411 / Open Drone ID
// decode + fixed-size contact table for the Clip-Boy "Remote ID" tool.
//
// The BLE scan callback (WiFiScan.cpp, BT_SCAN_REMOTE_ID branch) hands us
// the raw advertisement payload; we walk it for the ODID service-data AD
// (16-bit UUID 0xFFFA), decode, and keep a small table keyed by serial.
// No LVGL / Marauder / NimBLE deps — pure C-linkage logic.
// =====================================================================
#include <stdint.h>
#include "opendroneid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRONE_MAX 16
#define DRONE_EXPIRE_MS 90000

typedef struct {
    uint8_t  inUse;
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
    uint8_t  channel;   // WiFi channel (0 = Bluetooth)
    uint32_t lastSeenMs;
    uint32_t packets;
} DroneRec;

// Feed a raw BLE advertisement payload (the full AD structure list). Walks
// for the 0xFFFA service-data element and, if found, decodes + updates the
// table. src_channel 0 = Bluetooth. Returns 1 if an ODID message was decoded.
int drone_ingest_ble(const uint8_t mac[6], int8_t rssi,
                     const uint8_t* payload, uint16_t payload_len);

// Feed a decoded ODID message or message-pack directly (WiFi beacon path,
// future use). buf points at the ODID message/pack (first byte is the
// message type/header). Returns 1 if decoded.
int drone_ingest_odid(const uint8_t mac[6], int8_t rssi, uint8_t channel,
                      const uint8_t* buf, uint16_t len);

int             drone_count(void);
const DroneRec* drone_get(int idx);   // idx 0..DRONE_MAX-1, NULL if free
void            drone_clear(void);
int             drone_take_new(void); // 1 once when a new contact appeared

#ifdef __cplusplus
}
#endif

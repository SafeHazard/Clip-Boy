// =====================================================================
// Remote ID drone store — see drone_store.h
// =====================================================================
#include <Arduino.h>
#include <string.h>
#include "drone_store.h"

// SINGLE-WRITER INVARIANT: every access to s_drones / s_new happens on the
// Arduino loopTask (core 1). Writers: drone_ingest_odid() (called only from
// rid_wifi_service() in WiFiScan::main() <- cb.loop() in loop()) and drone_clear()
// (called under lv_timer_handler()). Readers: drone_count()/drone_get() (LVGL, same
// task). The BLE scan callback (nimble_host, core 0) must NEVER touch this table --
// it only extracts raw ODID bytes (drone_ble_extract_odid) and enqueues them to the
// WiFiScan Remote-ID ring, which the loopTask drains. Do not add an s_drones access
// from a BLE callback / lv_async / other-task timer, or the cross-core race returns.
static DroneRec s_drones[DRONE_MAX];
static volatile int s_new = 0;

static DroneRec* table_find(const uint8_t* mac, const char* serial)
{
    uint32_t now = millis();
    DroneRec* freeSlot = 0;
    DroneRec* oldest = 0;
    for (int i = 0; i < DRONE_MAX; i++) {
        DroneRec* r = &s_drones[i];
        if (!r->inUse) { if (!freeSlot) freeSlot = r; continue; }
        if (now - r->lastSeenMs > DRONE_EXPIRE_MS) {
            r->inUse = 0; if (!freeSlot) freeSlot = r; continue;
        }
        if (serial && serial[0] && r->hasSerial &&
            strncmp(r->uasId, serial, ODID_ID_SIZE) == 0)
            return r;
        if (!oldest || (int32_t)(r->lastSeenMs - oldest->lastSeenMs) < 0) oldest = r;
    }
    for (int i = 0; i < DRONE_MAX; i++) {
        DroneRec* r = &s_drones[i];
        if (r->inUse && memcmp(r->mac, mac, 6) == 0) return r;
    }
    DroneRec* r = freeSlot ? freeSlot : oldest;
    if (!r) return 0;
    memset(r, 0, sizeof(*r));
    r->inUse = 1;
    memcpy(r->mac, mac, 6);
    r->altGeo = INV_ALT; r->height = INV_ALT;
    r->speedH = INV_SPEED_H; r->direction = INV_DIR;
    if (serial && serial[0]) {
        strncpy(r->uasId, serial, ODID_ID_SIZE);
        r->uasId[ODID_ID_SIZE] = 0;
        r->hasSerial = 1;
    } else {
        snprintf(r->uasId, sizeof(r->uasId), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return r;
}

int drone_ingest_odid(const uint8_t mac[6], int8_t rssi, uint8_t channel,
                      const uint8_t* buf, uint16_t len)
{
    if (!buf || len < 1) return 0;
    ODID_UAS_Data uas;
    odid_initUasData(&uas);
    ODID_messagetype_t t = decodeMessageType(buf[0]);
    if (t == ODID_MESSAGETYPE_INVALID) return 0;
    if (t == ODID_MESSAGETYPE_PACKED) {
        if (len < 3) return 0;
        // Bound the buffer against the DECLARED pack size before decoding. buf[2]
        // (ODID_MessagePack_encoded.MsgPackSize) is attacker-controlled over the air.
        // decodeMessagePack() -> checkPackContent() reads Messages[0..MsgPackSize-1]
        // at a fixed ODID_MESSAGE_SIZE (25 B) stride; it rejects a count > 9 but takes
        // NO buffer-length parameter, so it can never reject a SHORT buffer. Without
        // this a frame claiming MsgPackSize=9 over-reads ~200 B (BLE: past the advert;
        // WiFi: fabricates a contact from stale ring bytes).
        uint8_t packSize = buf[2];
        if (packSize == 0 || packSize > ODID_PACK_MAX_MESSAGES) return 0;
        if (len < (uint16_t)(3 + packSize * ODID_MESSAGE_SIZE)) return 0;
        if (decodeMessagePack(&uas, (const ODID_MessagePack_encoded*)buf) != ODID_SUCCESS)
            return 0;
    } else {
        if (len < ODID_MESSAGE_SIZE) return 0;
        if (decodeOpenDroneID(&uas, buf) == ODID_MESSAGETYPE_INVALID) return 0;
    }

    const char* serial = 0;
    for (int i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++)
        if (uas.BasicIDValid[i] && uas.BasicID[i].UASID[0]) { serial = uas.BasicID[i].UASID; break; }

    DroneRec* r = table_find(mac, serial);
    if (!r) return 0;
    if (r->packets == 0) s_new = 1;
    memcpy(r->mac, mac, 6);
    r->lastSeenMs = millis();
    r->packets++;
    r->rssi = rssi;
    r->channel = channel;

    for (int i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++) {
        if (uas.BasicIDValid[i] && uas.BasicID[i].UASID[0]) {
            strncpy(r->uasId, uas.BasicID[i].UASID, ODID_ID_SIZE);
            r->uasId[ODID_ID_SIZE] = 0;
            r->hasSerial = 1;
            r->idType = uas.BasicID[i].IDType;
            r->uaType = uas.BasicID[i].UAType;
            break;
        }
    }
    if (uas.LocationValid) {
        r->status = uas.Location.Status;
        r->lat = uas.Location.Latitude;
        r->lon = uas.Location.Longitude;
        r->altGeo = uas.Location.AltitudeGeo;
        r->height = uas.Location.Height;
        r->speedH = uas.Location.SpeedHorizontal;
        r->direction = uas.Location.Direction;
    }
    if (uas.SystemValid) {
        r->opLat = uas.System.OperatorLatitude;
        r->opLon = uas.System.OperatorLongitude;
    }
    if (uas.OperatorIDValid && uas.OperatorID.OperatorId[0]) {
        strncpy(r->operatorId, uas.OperatorID.OperatorId, ODID_ID_SIZE);
        r->operatorId[ODID_ID_SIZE] = 0;
    }
    if (uas.SelfIDValid && uas.SelfID.Desc[0]) {
        strncpy(r->selfDesc, uas.SelfID.Desc, ODID_STR_SIZE);
        r->selfDesc[ODID_STR_SIZE] = 0;
    }
    return 1;
}

// Pure AD-structure walk -- locate the ODID service-data element and return a
// pointer to the ODID message/pack bytes (with *out_len set), or NULL if absent.
// NO decode here: this is called from the NimBLE scan callback (nimble_host task,
// core 0), so it must stay cheap. The heavy ODID decode (drone_ingest_odid: a
// ~920 B ODID_UAS_Data plus the decodeOpenDroneID call tree -- MEASURED ~5.3 KB of
// peak stack, 2026-08-30, baseline nimble_host free 9312 B -> 4016 B under live BLE
// adverts) is deferred to the Arduino loopTask via the WiFiScan Remote-ID ring,
// exactly like the WiFi beacon path. That keeps the spike off nimble_host (it would
// overflow a stock 4 KB BLE task stack; this build's happens to be larger) and keeps
// s_drones single-writer.
const uint8_t* drone_ble_extract_odid(const uint8_t* payload, uint16_t len,
                                      uint16_t* out_len)
{
    if (out_len) *out_len = 0;
    if (!payload || len < 5) return 0;
    // AD element = [adlen][type][data...]; look for Service Data - 16-bit UUID
    // (AD type 0x16) with UUID 0xFFFA, then app_code 0x0D.
    uint16_t i = 0;
    while (i + 1 < len) {
        uint8_t adlen = payload[i];
        if (adlen == 0) { i++; continue; }
        if (i + 1 + adlen > len) break;          // truncated AD element
        uint8_t adtype = payload[i + 1];
        const uint8_t* data = &payload[i + 2];
        uint8_t datalen = adlen - 1;
        if (adtype == 0x16 && datalen >= 4 &&
            data[0] == 0xFA && data[1] == 0xFF) { // UUID 0xFFFA, little-endian
            // service data after UUID = [app_code=0x0D][counter][ODID msg/pack]
            if (data[2] == 0x0D) {
                if (out_len) *out_len = (uint16_t)(datalen - 4);
                return &data[4];                 // ODID message/pack bytes
            }
        }
        i += 1 + adlen;
    }
    return 0;
}

int drone_count(void)
{
    int n = 0; uint32_t now = millis();
    for (int i = 0; i < DRONE_MAX; i++)
        if (s_drones[i].inUse && now - s_drones[i].lastSeenMs <= DRONE_EXPIRE_MS) n++;
    return n;
}
const DroneRec* drone_get(int idx)
{
    if (idx < 0 || idx >= DRONE_MAX) return 0;
    return s_drones[idx].inUse ? &s_drones[idx] : 0;
}
void drone_clear(void) { memset(s_drones, 0, sizeof(s_drones)); s_new = 0; }
int  drone_take_new(void) { int v = s_new; s_new = 0; return v; }

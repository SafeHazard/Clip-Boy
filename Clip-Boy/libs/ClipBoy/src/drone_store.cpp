// =====================================================================
// Remote ID drone store — see drone_store.h
// =====================================================================
#include <Arduino.h>
#include <string.h>
#include "drone_store.h"

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

int drone_ingest_ble(const uint8_t mac[6], int8_t rssi,
                     const uint8_t* payload, uint16_t len)
{
    if (!payload || len < 5) return 0;
    // Walk the AD structure list looking for Service Data - 16-bit UUID
    // (AD type 0x16) with UUID 0xFFFA. AD element = [adlen][type][data...].
    uint16_t i = 0;
    while (i + 1 < len) {
        uint8_t adlen = payload[i];
        if (adlen == 0) { i++; continue; }
        if (i + 1 + adlen > len) break;          // truncated
        uint8_t adtype = payload[i + 1];
        const uint8_t* data = &payload[i + 2];
        uint8_t datalen = adlen - 1;
        if (adtype == 0x16 && datalen >= 4 &&
            data[0] == 0xFA && data[1] == 0xFF) { // UUID 0xFFFA, little-endian
            // service data after UUID = [app_code=0x0D][counter][ODID msg/pack]
            if (data[2] == 0x0D && datalen >= 4) {
                const uint8_t* msg = &data[4];
                uint16_t msglen = datalen - 4;
                return drone_ingest_odid(mac, rssi, 0, msg, msglen);
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

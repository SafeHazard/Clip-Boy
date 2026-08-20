// =====================================================================
// Standalone UAV Remote ID scanner engine — see rid_scan.h.
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <NimBLEDevice.h>
#include <string.h>

#include "rid_scan.h"

// ---------------------------------------------------------------------
// Raw packet queue (sniffer callbacks -> service() thread)
// ---------------------------------------------------------------------
#define RID_RAW_MAX 250
struct RidRaw {
    uint8_t  buf[RID_RAW_MAX];
    uint16_t len;
    uint8_t  mac[6];
    int8_t   rssi;
    uint8_t  src;
    uint8_t  channel;
};
static QueueHandle_t s_rawQueue = nullptr;

static volatile uint32_t s_wifiFrames = 0;
static volatile uint32_t s_btFrames   = 0;
static volatile uint32_t s_bleAdvRaw  = 0;
static bool s_bleInited  = false;
static bool s_wifiReady  = false;
static bool s_classicReleased = false;
static bool s_newDetection = false;

static RidDrone s_drones[RID_MAX_DRONES];

// WiFi channel hop sequence (weighted to 1/6/11)
static const uint8_t s_hopSeq[] = {1, 6, 11, 6, 2, 6, 11, 1, 7, 11, 6, 12,
                                   1, 3, 11, 8, 6, 4, 1, 9, 11, 5, 6, 10,
                                   1, 13, 11, 6};
static uint8_t  s_hopIdx = 1;
static uint8_t  s_curChannel = 0;
static bool     s_wifiPhase = false;
static uint32_t s_phaseStart = 0;
static uint32_t s_lastHop = 0;

// ---------------------------------------------------------------------
static void enqueue_raw(const uint8_t* payload, uint16_t len,
                        const uint8_t* mac, int8_t rssi,
                        uint8_t src, uint8_t channel)
{
    if (!s_rawQueue || len == 0) return;
    RidRaw r;
    if (len > RID_RAW_MAX) len = RID_RAW_MAX;
    memcpy(r.buf, payload, len);
    r.len = len;
    memcpy(r.mac, mac, 6);
    r.rssi = rssi;
    r.src = src;
    r.channel = channel;
    xQueueSend(s_rawQueue, &r, 0);
}

// ---- WiFi sniffer ---------------------------------------------------
static void sniff_beacon(const uint8_t* d, uint16_t len, int8_t rssi, uint8_t ch)
{
    static const uint8_t ODID_OUI[3] = {0xFA, 0x0B, 0xBC};
    if (len < 36 + 2) return;
    const uint8_t* sa = d + 10;
    uint16_t pos = 36;
    while (pos + 2 <= len) {
        uint8_t id = d[pos], l = d[pos + 1];
        if (pos + 2 + l > len) break;
        if (id == 221 && l >= 6 && memcmp(d + pos + 2, ODID_OUI, 3) == 0 &&
            d[pos + 5] == 0x0D) {
            s_wifiFrames = s_wifiFrames + 1;
            enqueue_raw(d + pos + 7, l - 5, sa, rssi, RID_SRC_WIFI_BEACON, ch);
            return;
        }
        pos += 2 + l;
    }
}

static void sniff_nan_action(const uint8_t* d, uint16_t len, int8_t rssi, uint8_t ch)
{
    static const uint8_t NAN_DA[6]      = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};
    static const uint8_t WFA_OUI[3]     = {0x50, 0x6F, 0x9A};
    static const uint8_t ODID_SVC_ID[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};
    if (len < 24 + 4 + 3 + 10) return;
    if (memcmp(d + 4, NAN_DA, 6) != 0) return;
    const uint8_t* sa = d + 10;
    const uint8_t* p = d + 24;
    if (p[0] != 0x04 || p[1] != 0x09) return;
    if (memcmp(p + 2, WFA_OUI, 3) != 0 || p[5] != 0x13) return;
    p += 6;
    if (p + 13 > d + len) return;
    if (p[0] != 0x03) return;
    if (memcmp(p + 3, ODID_SVC_ID, 6) != 0) return;
    uint8_t svcInfoLen = p[12];
    const uint8_t* si = p + 13;
    if (si + svcInfoLen > d + len || svcInfoLen < 2) return;
    s_wifiFrames = s_wifiFrames + 1;
    enqueue_raw(si + 1, svcInfoLen - 1, sa, rssi, RID_SRC_WIFI_NAN, ch);
}

static void wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    uint16_t len = p->rx_ctrl.sig_len;
    if (len < 24) return;
    const uint8_t* d = p->payload;
    uint8_t subtype = (d[0] >> 4) & 0x0F;
    int8_t rssi = p->rx_ctrl.rssi;
    uint8_t ch = p->rx_ctrl.channel;
    if (subtype == 0x08) sniff_beacon(d, len, rssi, ch);
    else if (subtype == 0x0D) sniff_nan_action(d, len, rssi, ch);
}

// ---- BLE scanner ----------------------------------------------------
class RidBLECallbacks : public NimBLEScanCallbacks {
    void handle(const NimBLEAdvertisedDevice* dev) {
        s_bleAdvRaw = s_bleAdvRaw + 1;
        if (!dev->haveServiceData()) return;
        for (uint8_t i = 0; i < dev->getServiceDataCount(); i++) {
            static const NimBLEUUID odidUuid((uint16_t)0xFFFA);
            if (dev->getServiceDataUUID(i) != odidUuid) continue;
            std::string sd = dev->getServiceData(i);
            if (sd.size() < 2 + ODID_MESSAGE_SIZE || (uint8_t)sd[0] != 0x0D)
                continue;
            uint8_t src = RID_SRC_BT_LEGACY;
#if CONFIG_BT_NIMBLE_EXT_ADV
            if (!dev->isLegacyAdvertisement()) src = RID_SRC_BT_EXTENDED;
#endif
            s_btFrames = s_btFrames + 1;
            uint8_t mac[6];
            memcpy(mac, dev->getAddress().getBase()->val, 6);
            enqueue_raw((const uint8_t*)sd.data() + 2, sd.size() - 2,
                        mac, dev->getRSSI(), src, 0);
        }
    }
    void onResult(const NimBLEAdvertisedDevice* dev) override { handle(dev); }
    void onDiscovered(const NimBLEAdvertisedDevice* dev) override { handle(dev); }
};
static RidBLECallbacks s_bleCallbacks;

// ---- drone table (service thread only) ------------------------------
static RidDrone* table_find(const uint8_t* mac, const char* serial)
{
    uint32_t now = millis();
    RidDrone* free_slot = nullptr;
    RidDrone* oldest = nullptr;
    for (int i = 0; i < RID_MAX_DRONES; i++) {
        RidDrone* r = &s_drones[i];
        if (!r->inUse) { if (!free_slot) free_slot = r; continue; }
        if (now - r->lastSeenMs > RID_EXPIRE_MS) {
            r->inUse = false; if (!free_slot) free_slot = r; continue;
        }
        if (serial && serial[0] && r->hasSerial &&
            strncmp(r->uasId, serial, ODID_ID_SIZE) == 0)
            return r;
        if (!oldest || r->lastSeenMs < oldest->lastSeenMs) oldest = r;
    }
    for (int i = 0; i < RID_MAX_DRONES; i++) {
        RidDrone* r = &s_drones[i];
        if (r->inUse && memcmp(r->mac, mac, 6) == 0) return r;
    }
    RidDrone* r = free_slot ? free_slot : oldest;
    if (!r) return nullptr;
    bool wasInUse = r->inUse;
    memset(r, 0, sizeof(*r));
    r->inUse = true;
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
    (void)wasInUse;
    return r;
}

static void table_ingest(const RidRaw* raw)
{
    ODID_UAS_Data uas;
    odid_initUasData(&uas);
    ODID_messagetype_t t = decodeMessageType(raw->buf[0]);
    if (t == ODID_MESSAGETYPE_INVALID) return;
    if (t == ODID_MESSAGETYPE_PACKED) {
        if (raw->len < 3) return;
        if (decodeMessagePack(&uas, (const ODID_MessagePack_encoded*)raw->buf) != ODID_SUCCESS)
            return;
    } else {
        if (raw->len < ODID_MESSAGE_SIZE) return;
        if (decodeOpenDroneID(&uas, raw->buf) == ODID_MESSAGETYPE_INVALID) return;
    }

    const char* serial = nullptr;
    for (int i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++)
        if (uas.BasicIDValid[i] && uas.BasicID[i].UASID[0]) { serial = uas.BasicID[i].UASID; break; }

    RidDrone* r = table_find(raw->mac, serial);
    if (!r) return;
    if (r->packets == 0) s_newDetection = true;
    memcpy(r->mac, raw->mac, 6);
    r->lastSeenMs = millis();
    r->packets++;
    r->rssi = raw->rssi;
    r->src = raw->src;
    r->channel = raw->channel;

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
}

// ---- lifecycle ------------------------------------------------------
static void release_classic_bt(void)
{
    if (s_classicReleased) return;
    s_classicReleased = true;
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
}

void rid_earlyInit(void)
{
    if (s_bleInited) return;
    release_classic_bt();
    s_bleInited = NimBLEDevice::init("");
    Serial.printf("[RID] earlyInit BLE -> %d, free internal %u\n",
                  (int)s_bleInited,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool rid_begin(void)
{
    memset(s_drones, 0, sizeof(s_drones));
    if (!s_rawQueue) s_rawQueue = xQueueCreate(24, sizeof(RidRaw));
    if (!s_rawQueue) return false;

    // WiFi promiscuous (no AP association, we only listen)
    WiFi.mode(WIFI_STA);
    delay(100);
    esp_err_t we = esp_wifi_start();
    (void)we;
    esp_wifi_set_promiscuous(false);
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_cb);
    if (esp_wifi_set_promiscuous(true) == ESP_OK) s_wifiReady = true;
    s_hopIdx = 1;
    s_curChannel = s_hopSeq[s_hopIdx];
    esp_wifi_set_channel(s_curChannel, WIFI_SECOND_CHAN_NONE);
    s_wifiPhase = true;
    s_phaseStart = millis();
    s_lastHop = millis();
    Serial.printf("[RID] wifi promiscuous ready=%d free internal %u\n",
                  (int)s_wifiReady,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // BLE controller (should be up from rid_earlyInit)
    if (!NimBLEDevice::isInitialized()) { release_classic_bt(); s_bleInited = NimBLEDevice::init(""); }
    Serial.printf("[RID] BLE initialized=%d free internal %u\n",
                  (int)NimBLEDevice::isInitialized(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan) {
        scan->setScanCallbacks(&s_bleCallbacks, true);
        scan->setActiveScan(false);
        scan->setDuplicateFilter(false);
        scan->setInterval(160);
        scan->setWindow(160);
#if CONFIG_BT_NIMBLE_EXT_ADV
        scan->setPhy(NimBLEScan::SCAN_ALL);
#endif
        bool started = scan->start(0, false, true);
        Serial.printf("[RID] scan->start -> %d\n", (int)started);
    }
    return true;
}

void rid_service(void)
{
    // Drain decode queue.
    if (s_rawQueue) {
        RidRaw raw;
        int budget = 24;
        while (budget-- && xQueueReceive(s_rawQueue, &raw, 0) == pdTRUE)
            table_ingest(&raw);
    }

    // Radio time-slicer (only if WiFi came up; otherwise stay on BLE).
    if (!s_wifiReady) { s_curChannel = 0; return; }
    uint32_t now = millis();
    if (s_wifiPhase) {
        if (now - s_phaseStart >= RID_WIFI_WINDOW_MS) {
            esp_wifi_set_promiscuous(false);
            s_wifiPhase = false; s_phaseStart = now; s_curChannel = 0;
            return;
        }
        if (now - s_lastHop >= RID_HOP_MS) {
            s_lastHop = now;
            s_hopIdx = (s_hopIdx + 1) % (uint8_t)sizeof(s_hopSeq);
            s_curChannel = s_hopSeq[s_hopIdx];
            esp_wifi_set_channel(s_curChannel, WIFI_SECOND_CHAN_NONE);
        }
    } else {
        if (now - s_phaseStart >= RID_BLE_WINDOW_MS) {
            esp_wifi_set_promiscuous(true);
            s_hopIdx = 1; s_curChannel = s_hopSeq[s_hopIdx];
            esp_wifi_set_channel(s_curChannel, WIFI_SECOND_CHAN_NONE);
            s_wifiPhase = true; s_phaseStart = now; s_lastHop = now;
        }
    }
}

int rid_droneCount(void)
{
    int n = 0; uint32_t now = millis();
    for (int i = 0; i < RID_MAX_DRONES; i++)
        if (s_drones[i].inUse && now - s_drones[i].lastSeenMs <= RID_EXPIRE_MS) n++;
    return n;
}
const RidDrone* rid_getDrone(int idx)
{
    if (idx < 0 || idx >= RID_MAX_DRONES) return nullptr;
    return s_drones[idx].inUse ? &s_drones[idx] : nullptr;
}
uint32_t rid_wifiFrames(void) { return s_wifiFrames; }
uint32_t rid_btFrames(void)   { return s_btFrames; }
uint32_t rid_bleAdvRaw(void)  { return s_bleAdvRaw; }
uint8_t  rid_currentChannel(void) { return s_curChannel; }
bool     rid_bleReady(void)  { return NimBLEDevice::isInitialized(); }
bool     rid_wifiReady(void) { return s_wifiReady; }
bool     rid_takeNewDetection(void) { bool v = s_newDetection; s_newDetection = false; return v; }

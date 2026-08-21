#include "rid_tx.h"
#include "sim_flight.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include <NimBLEDevice.h>

#include "src/odid/opendroneid.h"

// The WiFi driver drops frames it judges malformed, which is every beacon we
// are about to forge. Overriding the check at link time is how Marauder does
// it, and it is the same trick that makes the Clip-Boy transmit tools radiate.
// Returning 0 means "no objection". See Clip-Boy/libs/ClipBoy/src/WiFiScan.cpp.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg;
    (void)arg2;
    (void)arg3;
    return 0;
}

// The wire format depends on these structs being byte-exact. opendroneid marks
// them __packed__; if a compiler or a library bump ever stops honouring that,
// fail the build here rather than transmit silently wrong frames.
static_assert(sizeof(ODID_Message_encoded) == ODID_MESSAGE_SIZE,
              "ODID_Message_encoded must be exactly one 25 byte message");
static_assert(sizeof(ODID_MessagePack_encoded) == 3 + ODID_PACK_MAX_MESSAGES * ODID_MESSAGE_SIZE,
              "ODID_MessagePack_encoded must be a 3 byte header plus packed messages");

static const uint8_t ODID_OUI[3]    = {0xFA, 0x0B, 0xBC};   // ASTM vendor OUI
static const uint8_t WFA_OUI[3]     = {0x50, 0x6F, 0x9A};   // WiFi Alliance
static const uint8_t NAN_CLUSTER[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};
static const uint8_t ODID_SVC_ID[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

static const uint8_t ODID_APP_CODE  = 0x0D;   // ASTM application code
static const uint8_t IE_VENDOR      = 221;

// The hidden SoftAP exists only so WIFI_IF_AP is up and will transmit. It is
// hidden, but it does still emit its own beacons on the channel.
static const char* AP_SSID = "dronesim";

#define RID_TX_FRAME_MAX 512
#define RID_TX_ODID_MAX  (3 + RID_TX_MSG_COUNT * ODID_MESSAGE_SIZE)

// 2019-01-01 UTC to 2026-08-21 UTC in seconds. ODID_System_data.Timestamp is
// relative to that epoch and this board has no clock, so the build date stands
// in as the base and uptime accumulates on top. Replace if a GNSS module or an
// RTC ever lands.
static const uint32_t SIM_EPOCH_BASE_S = 240969600UL;

static RidTxConfig          s_cfg;
static RidTxStats           s_stats;
static bool                 s_running   = false;
static bool                 s_begun     = false;
static uint8_t              s_mac[6]    = {0};
static uint8_t              s_counter   = 0;
static uint8_t              s_bleIndex  = 0;
static uint32_t             s_lastWifi  = 0;
static uint32_t             s_lastBle   = 0;
static ODID_Message_encoded s_msgs[RID_TX_MSG_COUNT];
static uint8_t              s_frame[RID_TX_FRAME_MAX];
static NimBLEAdvertising*   s_adv       = nullptr;

void rid_tx_default_config(RidTxConfig* cfg)
{
    if (!cfg) return;
    cfg->wifiBeacon     = true;
    cfg->wifiNan        = true;
    cfg->ble            = true;
    cfg->channel        = 6;
    // A receiver that channel hops only hears us during its dwell on our
    // channel, so transmit far more often than the 1 Hz the standard asks for.
    cfg->wifiIntervalMs = 200;
    cfg->bleIntervalMs  = 300;
    cfg->uasId          = "SPACEBADGE-SIM-0001";
    cfg->operatorId     = "FA-SIM-OPERATOR-001";
    cfg->selfIdDesc     = "Clip-Boy RID test rig";
}

// ---------------------------------------------------------------- encoding --

static void build_messages(void)
{
    const SimFix* fix = sim_flight_fix();

    ODID_BasicID_data basic;
    odid_initBasicIDData(&basic);
    basic.IDType = ODID_IDTYPE_SERIAL_NUMBER;
    basic.UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
    strncpy(basic.UASID, s_cfg.uasId, ODID_ID_SIZE);
    if (encodeBasicIDMessage(&s_msgs[0].basicId, &basic) != ODID_SUCCESS) s_stats.encodeErrors++;

    ODID_Location_data loc;
    odid_initLocationData(&loc);
    loc.Status          = ODID_STATUS_AIRBORNE;
    loc.Direction       = fix->direction;
    loc.SpeedHorizontal = fix->speedH;
    loc.SpeedVertical   = fix->speedV;
    loc.Latitude        = fix->lat;
    loc.Longitude       = fix->lon;
    loc.AltitudeBaro    = fix->altGeo;
    loc.AltitudeGeo     = fix->altGeo;
    loc.HeightType      = ODID_HEIGHT_REF_OVER_TAKEOFF;
    loc.Height          = fix->height;
    loc.HorizAccuracy   = ODID_HOR_ACC_3_METER;
    loc.VertAccuracy    = ODID_VER_ACC_3_METER;
    loc.BaroAccuracy    = ODID_VER_ACC_3_METER;
    loc.SpeedAccuracy   = ODID_SPEED_ACC_1_METERS_PER_SECOND;
    loc.TSAccuracy      = ODID_TIME_ACC_1_0_SECOND;
    loc.TimeStamp       = fix->timeStamp;
    if (encodeLocationMessage(&s_msgs[1].location, &loc) != ODID_SUCCESS) s_stats.encodeErrors++;

    ODID_System_data sys;
    odid_initSystemData(&sys);
    sys.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_TAKEOFF;
    sys.ClassificationType   = ODID_CLASSIFICATION_TYPE_UNDECLARED;
    sim_flight_operator(&sys.OperatorLatitude, &sys.OperatorLongitude, &sys.OperatorAltitudeGeo);
    sys.AreaCount  = 1;
    sys.AreaRadius = 0;
    sys.Timestamp  = SIM_EPOCH_BASE_S + (millis() / 1000UL);
    if (encodeSystemMessage(&s_msgs[2].system, &sys) != ODID_SUCCESS) s_stats.encodeErrors++;

    ODID_SelfID_data self;
    odid_initSelfIDData(&self);
    self.DescType = ODID_DESC_TYPE_TEXT;
    strncpy(self.Desc, s_cfg.selfIdDesc, ODID_STR_SIZE);
    if (encodeSelfIDMessage(&s_msgs[3].selfId, &self) != ODID_SUCCESS) s_stats.encodeErrors++;

    ODID_OperatorID_data op;
    odid_initOperatorIDData(&op);
    op.OperatorIdType = ODID_OPERATOR_ID;
    strncpy(op.OperatorId, s_cfg.operatorId, ODID_ID_SIZE);
    if (encodeOperatorIDMessage(&s_msgs[4].operatorId, &op) != ODID_SUCCESS) s_stats.encodeErrors++;
}

// Flatten the five messages into a message pack: a 3 byte header then the
// messages back to back. Returns the byte count, or 0 if the encoder refused.
static uint16_t build_pack(uint8_t* out)
{
    ODID_MessagePack_data pack;
    odid_initMessagePackData(&pack);
    pack.SingleMessageSize = ODID_MESSAGE_SIZE;
    pack.MsgPackSize       = RID_TX_MSG_COUNT;
    memcpy(pack.Messages, s_msgs, sizeof(s_msgs));

    ODID_MessagePack_encoded enc;
    if (encodeMessagePack(&enc, &pack) != ODID_SUCCESS) {
        s_stats.encodeErrors++;
        return 0;
    }
    const uint16_t len = 3 + RID_TX_MSG_COUNT * ODID_MESSAGE_SIZE;
    memcpy(out, &enc, len);
    return len;
}

// ------------------------------------------------------------ frame layout --

// Beacon: 24 byte MAC header, 12 bytes of fixed parameters, then the IE list
// starting at offset 36. Remote ID rides in a vendor IE carrying OUI FA:0B:BC,
// type 0x0D, a message counter, then the ODID pack.
static uint16_t build_beacon(uint8_t* out, const uint8_t* mac, uint8_t channel,
                             const uint8_t* odid, uint16_t odidLen, uint8_t counter)
{
    static const uint8_t RATES[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    const uint8_t ssidLen = (uint8_t)strlen(AP_SSID);

    uint16_t i = 0;
    out[i++] = 0x80;  out[i++] = 0x00;              // type/subtype: beacon
    out[i++] = 0x00;  out[i++] = 0x00;              // duration
    memset(out + i, 0xFF, 6);        i += 6;        // DA: broadcast
    memcpy(out + i, mac, 6);         i += 6;        // SA
    memcpy(out + i, mac, 6);         i += 6;        // BSSID
    out[i++] = 0x00;  out[i++] = 0x00;              // seq/frag, driver fills it
    memset(out + i, 0, 8);           i += 8;        // timestamp
    out[i++] = 0x64;  out[i++] = 0x00;              // beacon interval, 100 TU
    out[i++] = 0x01;  out[i++] = 0x00;              // capability: ESS

    out[i++] = 0x00;  out[i++] = ssidLen;           // SSID
    memcpy(out + i, AP_SSID, ssidLen); i += ssidLen;

    out[i++] = 0x01;  out[i++] = sizeof(RATES);     // supported rates
    memcpy(out + i, RATES, sizeof(RATES)); i += sizeof(RATES);

    out[i++] = 0x03;  out[i++] = 0x01;              // DS parameter set
    out[i++] = channel;

    // Vendor IE length covers OUI(3) + type(1) + counter(1) + payload, so the
    // receiver recovers the ODID length as len - 5 and its start as pos + 7.
    out[i++] = IE_VENDOR;
    out[i++] = (uint8_t)(5 + odidLen);
    memcpy(out + i, ODID_OUI, 3);    i += 3;
    out[i++] = ODID_APP_CODE;
    out[i++] = counter;
    memcpy(out + i, odid, odidLen);  i += odidLen;

    return i;
}

// NAN action frame: the fixed NAN cluster address, a WFA vendor-specific public
// action, then one service descriptor attribute whose service ID is the ODID
// hash and whose service info is [counter][ODID pack].
static uint16_t build_nan(uint8_t* out, const uint8_t* mac,
                          const uint8_t* odid, uint16_t odidLen, uint8_t counter)
{
    uint16_t i = 0;
    out[i++] = 0xD0;  out[i++] = 0x00;              // type/subtype: action
    out[i++] = 0x00;  out[i++] = 0x00;              // duration
    memcpy(out + i, NAN_CLUSTER, 6); i += 6;        // DA
    memcpy(out + i, mac, 6);         i += 6;        // SA
    memcpy(out + i, NAN_CLUSTER, 6); i += 6;        // BSSID
    out[i++] = 0x00;  out[i++] = 0x00;              // seq/frag

    out[i++] = 0x04;                                // category: public action
    out[i++] = 0x09;                                // vendor specific
    memcpy(out + i, WFA_OUI, 3);     i += 3;
    out[i++] = 0x13;                                // NAN service discovery frame

    // Attribute length counts everything after the length field itself:
    // service ID 6, instance 1, requestor 1, control 1, info length 1, info.
    const uint16_t attrLen = 10 + 1 + odidLen;
    out[i++] = 0x03;                                // service descriptor attribute
    out[i++] = (uint8_t)(attrLen & 0xFF);
    out[i++] = (uint8_t)(attrLen >> 8);
    memcpy(out + i, ODID_SVC_ID, 6); i += 6;
    out[i++] = 0x01;                                // instance id
    out[i++] = 0x00;                                // requestor instance id
    out[i++] = 0x10;                                // service control: info present
    out[i++] = (uint8_t)(1 + odidLen);              // service info length
    out[i++] = counter;
    memcpy(out + i, odid, odidLen);  i += odidLen;

    return i;
}

// ----------------------------------------------------------------- radios ---

static void ble_advertise(void)
{
    if (!s_adv) return;

    // Legacy advertising carries 31 bytes. One service data structure with a
    // 25 byte ODID message fills it exactly, which is why there is no flags
    // structure here: adding one would overflow and truncate the message.
    uint8_t ad[31];
    uint8_t n = 0;
    ad[n++] = 1 + 2 + 2 + ODID_MESSAGE_SIZE;   // type, UUID, app code + counter, message
    ad[n++] = 0x16;                            // service data, 16 bit UUID
    ad[n++] = 0xFA;                            // 0xFFFA, little endian
    ad[n++] = 0xFF;
    ad[n++] = ODID_APP_CODE;
    ad[n++] = s_counter;
    memcpy(ad + n, s_msgs[s_bleIndex % RID_TX_MSG_COUNT].rawData, ODID_MESSAGE_SIZE);
    n += ODID_MESSAGE_SIZE;

    NimBLEAdvertisementData data;
    data.addData(ad, n);

    // The payload only takes effect on restart, so cycle the advertiser.
    s_adv->stop();
    s_adv->setAdvertisementData(data);
    s_adv->start();

    s_bleIndex++;
    s_stats.bleAdverts++;
}

static void tx_wifi(void)
{
    uint8_t  odid[RID_TX_ODID_MAX];
    uint16_t odidLen = build_pack(odid);
    if (!odidLen) return;

    if (s_cfg.wifiBeacon) {
        const uint16_t n = build_beacon(s_frame, s_mac, s_cfg.channel, odid, odidLen, s_counter);
        if (esp_wifi_80211_tx(WIFI_IF_AP, s_frame, n, true) == ESP_OK) s_stats.beacons++;
        else                                                          s_stats.wifiErrors++;
    }
    if (s_cfg.wifiNan) {
        const uint16_t n = build_nan(s_frame, s_mac, odid, odidLen, s_counter);
        if (esp_wifi_80211_tx(WIFI_IF_AP, s_frame, n, true) == ESP_OK) s_stats.nanFrames++;
        else                                                          s_stats.wifiErrors++;
    }
}

// -------------------------------------------------------------------- api ---

void rid_tx_early_init(void)
{
    if (s_adv) return;
    NimBLEDevice::init("");
    s_adv = NimBLEDevice::getAdvertising();
    if (!s_adv) return;
    s_adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    s_adv->setMinInterval(160);   // 160 * 0.625ms = 100ms
    s_adv->setMaxInterval(160);
}

bool rid_tx_begin(const RidTxConfig* cfg)
{
    if (!cfg) return false;
    s_cfg = *cfg;
    memset(&s_stats, 0, sizeof(s_stats));

    // BLE must already be up. If setup() did not call rid_tx_early_init() this
    // is the last chance, and by now the display stack has probably taken the
    // DRAM the controller needs.
    rid_tx_early_init();
    if (!s_adv) return false;

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    // Hidden, single client. We never accept a connection; the AP exists so
    // that WIFI_IF_AP is up and esp_wifi_80211_tx actually radiates.
    if (!WiFi.softAP(AP_SSID, nullptr, s_cfg.channel, 1, 1)) return false;

    // Modem sleep is MANDATORY while the BLE controller is up. One antenna, and
    // the WiFi driver refuses to run without power save when coexistence is
    // active: esp_wifi_set_ps(WIFI_PS_NONE) is queued to ppTask, which then
    // aborts in pm_set_sleep_type and boot-loops the board. It does not fail
    // gracefully and it does not warn at the call site.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_set_channel(s_cfg.channel, WIFI_SECOND_CHAN_NONE);
    if (esp_wifi_get_mac(WIFI_IF_AP, s_mac) != ESP_OK) return false;

    build_messages();
    s_begun = true;
    return true;
}

void rid_tx_service(uint32_t nowMs)
{
    if (!s_begun || !s_running) return;

    sim_flight_update(nowMs);

    const bool wifiDue = (s_cfg.wifiBeacon || s_cfg.wifiNan) &&
                         (uint32_t)(nowMs - s_lastWifi) >= s_cfg.wifiIntervalMs;
    const bool bleDue  = s_cfg.ble &&
                         (uint32_t)(nowMs - s_lastBle) >= s_cfg.bleIntervalMs;
    if (!wifiDue && !bleDue) return;

    build_messages();

    if (wifiDue) {
        s_lastWifi = nowMs;
        tx_wifi();
    }
    if (bleDue) {
        s_lastBle = nowMs;
        ble_advertise();
    }
    // One counter across all three paths, as a real transmitter does.
    s_counter++;
}

void rid_tx_set_running(bool on)
{
    if (!s_begun || on == s_running) return;
    s_running = on;
    if (on) {
        const uint32_t now = millis();
        sim_flight_update(now);
        // Backdate both timers so the first frame goes out immediately.
        s_lastWifi = now - s_cfg.wifiIntervalMs;
        s_lastBle  = now - s_cfg.bleIntervalMs;
    } else if (s_adv) {
        s_adv->stop();
    }
}

bool rid_tx_running(void)
{
    return s_running;
}

bool rid_tx_set_channel(uint8_t ch)
{
    if (ch < 1 || ch > 14) return false;
    s_cfg.channel = ch;
    return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

RidTxConfig* rid_tx_config(void)
{
    return &s_cfg;
}

const RidTxStats* rid_tx_stats(void)
{
    return &s_stats;
}

const uint8_t* rid_tx_mac(void)
{
    return s_mac;
}

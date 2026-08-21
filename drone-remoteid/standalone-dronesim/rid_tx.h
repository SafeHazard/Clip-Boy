// Remote ID transmitter. Encodes the simulated drone into ASTM F3411 messages
// and puts them on the air three ways: WiFi beacon vendor IE, WiFi NAN action
// frame, and BLE service data.
//
// Nothing here receives. This is a test source for a Remote ID receiver.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Number of ODID messages in each pack, and the rotation length on BLE.
#define RID_TX_MSG_COUNT 5

struct RidTxConfig {
    bool        wifiBeacon;
    bool        wifiNan;
    bool        ble;
    uint8_t     channel;         // 802.11 channel, both WiFi paths
    uint16_t    wifiIntervalMs;  // gap between WiFi bursts
    uint16_t    bleIntervalMs;   // gap between BLE payload swaps
    const char* uasId;           // serial number, up to 20 chars
    const char* operatorId;      // up to 20 chars
    const char* selfIdDesc;      // up to 23 chars
};

struct RidTxStats {
    uint32_t beacons;
    uint32_t nanFrames;
    uint32_t bleAdverts;
    uint32_t wifiErrors;
    uint32_t encodeErrors;
};

void              rid_tx_default_config(RidTxConfig* cfg);

// Bring up the BLE controller. Call this FIRST in setup(), before the display,
// LVGL or PSRAM init. The controller needs a large contiguous block of internal
// DRAM and cannot get it once the display stack has taken its share; the symptom
// is "BLE_INIT: Malloc failed". DroneWatch does the same thing for the same
// reason. rid_tx_begin() calls it if you forgot, which may be too late.
void              rid_tx_early_init(void);

bool              rid_tx_begin(const RidTxConfig* cfg);
void              rid_tx_service(uint32_t nowMs);
void              rid_tx_set_running(bool on);
bool              rid_tx_running(void);
bool              rid_tx_set_channel(uint8_t ch);
RidTxConfig*      rid_tx_config(void);
const RidTxStats* rid_tx_stats(void);
const uint8_t*    rid_tx_mac(void);   // 6 bytes, the source address on the air

// DRONE SIM -- a Remote ID transmitter for testing a Remote ID receiver.
//
// Runs on the Waveshare ESP32-S3-Touch-LCD-2.8 board, which is both the DEF CON
// 33 SpaceBadge and the Clip-Boy. It replaces the badge firmware entirely.
//
// It broadcasts one simulated drone flying a circle, over all three transports
// the standard defines: WiFi beacon vendor IE, WiFi NAN action frame, and BLE
// service data. The point is to give the Clip-Boy "Drone ID" tool a WiFi signal
// to decode, which until now nothing on this bench could produce: Windows
// cannot inject 802.11 frames.
//
// The screen shows what it is doing. That is not decoration: the USB serial
// console on this board does not reliably surface, so without a display a
// working firmware and a dead one look exactly the same. A serial console is
// still available at 115200 if it happens to work for you.

#include <lvgl.h>

#include "Display_Driver.h"    // init_display(), LVGL glue (LovyanGFX)
#include "Touch_CST328.h"      // Touch_Init()
#include "rid_tx.h"
#include "sim_flight.h"
#include "sim_ui.h"

static uint32_t s_lastReport = 0;
static uint32_t s_lastTick   = 0;
static uint32_t s_lastUi     = 0;
static char     s_line[32];
static uint8_t  s_lineLen = 0;

static void printHelp(void)
{
    Serial.println();
    Serial.println(F("DRONE SIM -- Remote ID transmitter"));
    Serial.println(F("  g  start transmitting"));
    Serial.println(F("  x  stop"));
    Serial.println(F("  w  toggle WiFi beacon path"));
    Serial.println(F("  n  toggle WiFi NAN path"));
    Serial.println(F("  b  toggle BLE path"));
    Serial.println(F("  c<n>  set WiFi channel, 1 to 14"));
    Serial.println(F("  i  show status"));
    Serial.println(F("  ?  this help"));
    Serial.println();
}

static void printStatus(void)
{
    const RidTxConfig* cfg   = rid_tx_config();
    const RidTxStats*  stats = rid_tx_stats();
    const SimFix*      fix   = sim_flight_fix();
    const uint8_t*     mac   = rid_tx_mac();

    Serial.printf("[%s] ch %u  paths:%s%s%s  mac %02X:%02X:%02X:%02X:%02X:%02X\n",
                  rid_tx_running() ? "TX" : "idle",
                  cfg->channel,
                  cfg->wifiBeacon ? " beacon" : "",
                  cfg->wifiNan    ? " nan"    : "",
                  cfg->ble        ? " ble"    : "",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("  sent: %lu beacons, %lu nan, %lu ble  errors: %lu wifi, %lu encode\n",
                  (unsigned long)stats->beacons,
                  (unsigned long)stats->nanFrames,
                  (unsigned long)stats->bleAdverts,
                  (unsigned long)stats->wifiErrors,
                  (unsigned long)stats->encodeErrors);
    Serial.printf("  drone: %.6f, %.6f  alt %.1fm HAE  agl %.1fm  %.1f m/s hdg %.0f\n",
                  fix->lat, fix->lon, fix->altGeo, fix->height,
                  fix->speedH, fix->direction);
}

static void handleCommand(const char* cmd)
{
    RidTxConfig* cfg = rid_tx_config();

    switch (cmd[0]) {
        case 'g':
            rid_tx_set_running(true);
            Serial.println(F("transmitting"));
            break;
        case 'x':
            rid_tx_set_running(false);
            Serial.println(F("stopped"));
            break;
        case 'w':
            cfg->wifiBeacon = !cfg->wifiBeacon;
            Serial.printf("beacon path %s\n", cfg->wifiBeacon ? "on" : "off");
            break;
        case 'n':
            cfg->wifiNan = !cfg->wifiNan;
            Serial.printf("nan path %s\n", cfg->wifiNan ? "on" : "off");
            break;
        case 'b':
            cfg->ble = !cfg->ble;
            Serial.printf("ble path %s\n", cfg->ble ? "on" : "off");
            break;
        case 'c': {
            const int ch = atoi(cmd + 1);
            if (rid_tx_set_channel((uint8_t)ch)) Serial.printf("channel %d\n", ch);
            else                                 Serial.println(F("channel must be 1 to 14"));
            break;
        }
        case 'i':
            printStatus();
            break;
        case '?':
            printHelp();
            break;
        default:
            Serial.println(F("unknown command, ? for help"));
            break;
    }
}

static void handleSerial(void)
{
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (s_lineLen) {
                s_line[s_lineLen] = '\0';
                handleCommand(s_line);
                s_lineLen = 0;
            }
            continue;
        }
        if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = c;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(50);
    Serial.println(F("\n[DRONESIM] boot"));

    // BLE controller first, while internal RAM is still wide open. Everything
    // below this line competes for the DRAM it needs.
    rid_tx_early_init();

    // PSRAM for LVGL allocations.
    psramInit();

    // Touch must init before the display registers its input device.
    Touch_Init();
    init_display();
    sim_ui_init();
    lv_tick_inc(5);
    lv_timer_handler();

    SimFlightConfig flight;
    sim_flight_default_config(&flight);
    sim_flight_begin(&flight);

    RidTxConfig cfg;
    rid_tx_default_config(&cfg);

    if (!rid_tx_begin(&cfg)) {
        // Say so on the screen. A dark badge and a badge whose radios failed
        // are indistinguishable otherwise, which cost us an afternoon once.
        Serial.println(F("[DRONESIM] rid_tx_begin failed -- radios did not come up"));
        sim_ui_fatal("radios did not come up");
        return;
    }

    printHelp();
    // Start transmitting on boot so the board is useful with no console
    // attached: power it up next to the receiver and it is already a drone.
    rid_tx_set_running(true);
    printStatus();
    Serial.println(F("[DRONESIM] transmitting"));
}

void loop()
{
    const uint32_t now = millis();

    if (now - s_lastTick >= 5) {
        lv_tick_inc(now - s_lastTick);
        s_lastTick = now;
    }
    lv_timer_handler();

    handleSerial();
    rid_tx_service(now);

    if (now - s_lastUi >= 250) {
        s_lastUi = now;
        sim_ui_tick();
    }

    if (now - s_lastReport >= 5000) {
        s_lastReport = now;
        if (rid_tx_running()) printStatus();
    }

    delay(5);
}

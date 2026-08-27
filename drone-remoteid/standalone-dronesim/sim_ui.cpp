#include "sim_ui.h"
#include "rid_tx.h"
#include "sim_flight.h"

#include <lvgl.h>
#include <stdio.h>

// Same palette as DroneWatch, so the two badges read as a matched pair on a
// bench: amber chrome, green for live, dim for off.
#define C_BG      lv_color_hex(0x000000)
#define C_PANEL   lv_color_hex(0x0b1410)
#define C_AMBER   lv_color_hex(0xffb000)
#define C_GREEN   lv_color_hex(0x39ff14)
#define C_DIM     lv_color_hex(0x2a3a2a)
#define C_TXT     lv_color_hex(0xd8f5d0)
#define C_SUB     lv_color_hex(0x6f8f6f)
#define C_RED     lv_color_hex(0xff3b30)

static lv_obj_t* s_scr      = nullptr;
static lv_obj_t* s_stateLbl = nullptr;
static lv_obj_t* s_pathsLbl = nullptr;
static lv_obj_t* s_countLbl = nullptr;
static lv_obj_t* s_posLbl   = nullptr;
static lv_obj_t* s_macLbl   = nullptr;
static lv_obj_t* s_beacon   = nullptr;   // blinks on each transmit burst

static uint32_t s_lastBeacons = 0;

static lv_obj_t* make_label(lv_obj_t* parent, int x, int y, lv_color_t colour)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, 240 - x - 4);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_label_set_text(l, "");
    return l;
}

void sim_ui_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hdr = lv_obj_create(s_scr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, 240, 34);
    lv_obj_set_style_bg_color(hdr, C_PANEL, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_obj_set_pos(title, 8, 6);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, C_AMBER, 0);
    lv_label_set_text(title, "DRONE SIM");

    // A dot that flips bright on every transmit burst. If this is blinking the
    // radio loop is alive, which no counter on its own can tell you at a glance.
    s_beacon = lv_obj_create(hdr);
    lv_obj_set_pos(s_beacon, 214, 12);
    lv_obj_set_size(s_beacon, 12, 12);
    lv_obj_set_style_radius(s_beacon, 6, 0);
    lv_obj_set_style_border_width(s_beacon, 0, 0);
    lv_obj_set_style_bg_color(s_beacon, C_DIM, 0);
    lv_obj_set_style_bg_opa(s_beacon, LV_OPA_COVER, 0);

    s_stateLbl = lv_label_create(s_scr);
    lv_obj_set_pos(s_stateLbl, 8, 44);
    lv_obj_set_style_text_font(s_stateLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_stateLbl, C_GREEN, 0);
    lv_label_set_text(s_stateLbl, "STARTING");

    s_pathsLbl = make_label(s_scr, 8, 72,  C_TXT);
    s_countLbl = make_label(s_scr, 8, 96,  C_SUB);
    s_posLbl   = make_label(s_scr, 8, 130, C_TXT);
    s_macLbl   = make_label(s_scr, 8, 292, C_DIM);

    lv_screen_load(s_scr);
}

void sim_ui_fatal(const char* msg)
{
    if (!s_stateLbl) return;
    lv_obj_set_style_text_color(s_stateLbl, C_RED, 0);
    lv_label_set_text(s_stateLbl, "FAILED");
    if (s_pathsLbl) lv_label_set_text(s_pathsLbl, msg ? msg : "unknown error");
    if (s_countLbl) lv_label_set_text(s_countLbl, "");
    if (s_posLbl)   lv_label_set_text(s_posLbl, "");
}

void sim_ui_tick(void)
{
    if (!s_scr) return;

    const RidTxConfig* cfg   = rid_tx_config();
    const RidTxStats*  stats = rid_tx_stats();
    const SimFix*      fix   = sim_flight_fix();
    const uint8_t*     mac   = rid_tx_mac();
    const bool         run   = rid_tx_running();

    char buf[128];

    lv_obj_set_style_text_color(s_stateLbl, run ? C_GREEN : C_SUB, 0);
    snprintf(buf, sizeof(buf), run ? "TRANSMITTING  ch %u" : "IDLE  ch %u", cfg->channel);
    lv_label_set_text(s_stateLbl, buf);

    snprintf(buf, sizeof(buf), "%s  %s  %s",
             cfg->wifiBeacon ? "BEACON" : "-",
             cfg->wifiNan    ? "NAN"    : "-",
             cfg->ble        ? "BLE"    : "-");
    lv_label_set_text(s_pathsLbl, buf);

    snprintf(buf, sizeof(buf), "sent %lu / %lu / %lu   err %lu",
             (unsigned long)stats->beacons,
             (unsigned long)stats->nanFrames,
             (unsigned long)stats->bleAdverts,
             (unsigned long)(stats->wifiErrors + stats->encodeErrors));
    lv_label_set_text(s_countLbl, buf);

    double opLat = 0.0, opLon = 0.0;
    float  opAlt = 0.0f;
    sim_flight_operator(&opLat, &opLon, &opAlt);

    // Pilot is shown next to position on purpose: the two must never look
    // alike, so any receiver that confuses them is obvious at a glance.
    snprintf(buf, sizeof(buf),
             "pos   %.5f\n      %.5f\npilot %.5f\n      %.5f\n\n"
             "alt %d HAE  agl %d\nspd %d m/s  hdg %d",
             fix->lat, fix->lon, opLat, opLon,
             (int)fix->altGeo, (int)fix->height,
             (int)fix->speedH, (int)fix->direction);
    lv_label_set_text(s_posLbl, buf);

    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_label_set_text(s_macLbl, buf);

    const bool blink = (stats->beacons != s_lastBeacons);
    s_lastBeacons = stats->beacons;
    lv_obj_set_style_bg_color(s_beacon, blink ? C_GREEN : C_DIM, 0);
}

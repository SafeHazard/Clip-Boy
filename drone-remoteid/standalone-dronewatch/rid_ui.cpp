// =====================================================================
// Dedicated drone-detector UI (LVGL). 240x320 portrait.
// Header (title + radios) | status line | contact list | detail overlay.
// =====================================================================
#include <Arduino.h>
#include <lvgl.h>
#include "rid_ui.h"
#include "rid_scan.h"

#define C_BG      lv_color_hex(0x000000)
#define C_PANEL   lv_color_hex(0x0b1410)
#define C_AMBER   lv_color_hex(0xffb000)
#define C_GREEN   lv_color_hex(0x39ff14)
#define C_DIM     lv_color_hex(0x2a3a2a)
#define C_TXT     lv_color_hex(0xd8f5d0)
#define C_SUB     lv_color_hex(0x6f8f6f)
#define C_RED     lv_color_hex(0xff3b30)

static lv_obj_t* s_scr;
static lv_obj_t* s_statusLbl;
static lv_obj_t* s_radar;         // small sweep in the header
static lv_obj_t* s_list;
static lv_obj_t* s_empty;
static lv_obj_t* s_detail;
static lv_obj_t* s_detailLbl;
static int       s_detailIdx = -1;

struct Row { lv_obj_t* row; lv_obj_t* title; lv_obj_t* sub; lv_obj_t* pin; int idx; };
static Row s_rows[RID_MAX_DRONES];

static const char* srcName(uint8_t s) {
    switch (s) { case RID_SRC_WIFI_BEACON: return "WIFI";
                 case RID_SRC_WIFI_NAN: return "NAN";
                 case RID_SRC_BT_LEGACY: return "BT4";
                 case RID_SRC_BT_EXTENDED: return "BT5"; }
    return "?";
}
static const char* statusName(uint8_t s) {
    switch (s) { case ODID_STATUS_GROUND: return "GROUND";
                 case ODID_STATUS_AIRBORNE: return "AIRBORNE";
                 case ODID_STATUS_EMERGENCY: return "EMERGENCY";
                 case ODID_STATUS_REMOTE_ID_SYSTEM_FAILURE: return "RID FAIL"; }
    return "UNKNOWN";
}
static const char* uaTypeName(uint8_t t) {
    switch (t) {
        case ODID_UATYPE_AEROPLANE: return "Fixed-wing";
        case ODID_UATYPE_HELICOPTER_OR_MULTIROTOR: return "Multirotor";
        case ODID_UATYPE_GYROPLANE: return "Gyroplane";
        case ODID_UATYPE_HYBRID_LIFT: return "Hybrid";
        case ODID_UATYPE_FREE_BALLOON: case ODID_UATYPE_CAPTIVE_BALLOON: return "Balloon";
        case ODID_UATYPE_AIRSHIP: return "Airship";
        case ODID_UATYPE_FREE_FALL_PARACHUTE: return "Parachute";
        case ODID_UATYPE_ROCKET: return "Rocket";
        case ODID_UATYPE_GROUND_OBSTACLE: return "Obstacle";
    }
    return "Unknown";
}

static void detail_close_cb(lv_event_t*) {
    if (s_detail) lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_detailIdx = -1;
}
static void detail_show(int idx) {
    const RidDrone* r = rid_getDrone(idx);
    if (!r || !s_detail) return;
    s_detailIdx = idx;
    char buf[512], loc[48], op[48], alt[48], spd[48];
    if (r->lat != 0.0 || r->lon != 0.0) snprintf(loc, sizeof(loc), "%.5f, %.5f", r->lat, r->lon);
    else snprintf(loc, sizeof(loc), "unknown");
    if (r->opLat != 0.0 || r->opLon != 0.0) snprintf(op, sizeof(op), "%.5f, %.5f", r->opLat, r->opLon);
    else snprintf(op, sizeof(op), "unknown");
    if (r->altGeo > INV_ALT) snprintf(alt, sizeof(alt), "%.0f m MSL / %.0f m AGL",
                                      r->altGeo, r->height > INV_ALT ? r->height : 0.0f);
    else snprintf(alt, sizeof(alt), "unknown");
    if (r->speedH < INV_SPEED_H) snprintf(spd, sizeof(spd), "%.1f m/s  hdg %.0f",
                                          r->speedH, r->direction < INV_DIR ? r->direction : 0.0f);
    else snprintf(spd, sizeof(spd), "unknown");
    snprintf(buf, sizeof(buf),
             "ID    %s\nTYPE  %s\nSTAT  %s\nPOS   %s\nALT   %s\nSPD   %s\n"
             "PILOT %s\nOPER  %s\nDESC  %s\nLINK  %s  %d dBm\nPKTS  %u   AGE %us",
             r->uasId, uaTypeName(r->uaType), statusName(r->status), loc, alt, spd,
             op, r->operatorId[0] ? r->operatorId : "-",
             r->selfDesc[0] ? r->selfDesc : "-",
             srcName(r->src), r->rssi, r->packets,
             (unsigned)((millis() - r->lastSeenMs) / 1000));
    lv_label_set_text(s_detailLbl, buf);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}
static void row_click_cb(lv_event_t* e) {
    Row* rw = (Row*)lv_event_get_user_data(e);
    if (rw && rw->idx >= 0) detail_show(rw->idx);
}

static void build_row(int i) {
    Row* rw = &s_rows[i];
    rw->row = lv_obj_create(s_list);
    lv_obj_set_size(rw->row, lv_pct(100), 46);
    lv_obj_set_style_bg_color(rw->row, C_PANEL, 0);
    lv_obj_set_style_bg_opa(rw->row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rw->row, 1, 0);
    lv_obj_set_style_border_color(rw->row, C_DIM, 0);
    lv_obj_set_style_radius(rw->row, 4, 0);
    lv_obj_set_style_pad_all(rw->row, 3, 0);
    lv_obj_clear_flag(rw->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rw->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rw->row, row_click_cb, LV_EVENT_CLICKED, rw);

    rw->pin = lv_obj_create(rw->row);
    lv_obj_set_pos(rw->pin, 0, 2);
    lv_obj_set_size(rw->pin, 6, 36);
    lv_obj_set_style_bg_color(rw->pin, C_GREEN, 0);
    lv_obj_set_style_bg_opa(rw->pin, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rw->pin, 0, 0);
    lv_obj_set_style_radius(rw->pin, 2, 0);
    lv_obj_clear_flag(rw->pin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rw->pin, LV_OBJ_FLAG_CLICKABLE);

    rw->title = lv_label_create(rw->row);
    lv_obj_set_pos(rw->title, 14, 2);
    lv_obj_set_width(rw->title, 218);
    lv_obj_set_style_text_color(rw->title, C_TXT, 0);
    lv_label_set_long_mode(rw->title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(rw->title, "");

    rw->sub = lv_label_create(rw->row);
    lv_obj_set_pos(rw->sub, 14, 24);
    lv_obj_set_width(rw->sub, 218);
    lv_obj_set_style_text_color(rw->sub, C_SUB, 0);
    lv_label_set_long_mode(rw->sub, LV_LABEL_LONG_CLIP);
    lv_label_set_text(rw->sub, "");

    lv_obj_add_flag(rw->row, LV_OBJ_FLAG_HIDDEN);
    rw->idx = -1;
}

void rid_ui_init(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header bar
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
    lv_label_set_text(title, "DRONE WATCH");

    s_radar = lv_spinner_create(hdr);
    lv_obj_set_pos(s_radar, 208, 4);
    lv_obj_set_size(s_radar, 26, 26);
    lv_spinner_set_anim_params(s_radar, 1800, 70);
    lv_obj_set_style_arc_color(s_radar, C_DIM, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_radar, C_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_radar, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_radar, 3, LV_PART_INDICATOR);

    // Status line (single line, clipped — never wraps into the list)
    s_statusLbl = lv_label_create(s_scr);
    lv_obj_set_pos(s_statusLbl, 6, 38);
    lv_obj_set_width(s_statusLbl, 232);
    lv_label_set_long_mode(s_statusLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_statusLbl, C_SUB, 0);
    lv_label_set_text(s_statusLbl, "starting...");

    // Contact list
    s_list = lv_obj_create(s_scr);
    lv_obj_set_pos(s_list, 0, 58);
    lv_obj_set_size(s_list, 240, 320 - 58);
    lv_obj_set_style_bg_color(s_list, C_BG, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 5, 0);
    lv_obj_set_style_pad_row(s_list, 5, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_empty = lv_label_create(s_list);
    lv_obj_set_style_text_color(s_empty, C_DIM, 0);
    lv_label_set_text(s_empty, "SCANNING...\n\nNo Remote ID contacts.\nWiFi 2.4G + Bluetooth.");

    for (int i = 0; i < RID_MAX_DRONES; i++) build_row(i);

    // Detail overlay
    s_detail = lv_obj_create(s_scr);
    lv_obj_set_pos(s_detail, 8, 50);
    lv_obj_set_size(s_detail, 224, 262);
    lv_obj_set_style_bg_color(s_detail, C_PANEL, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_detail, C_AMBER, 0);
    lv_obj_set_style_border_width(s_detail, 2, 0);
    lv_obj_set_style_radius(s_detail, 6, 0);
    lv_obj_set_style_pad_all(s_detail, 8, 0);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail, detail_close_cb, LV_EVENT_CLICKED, NULL);
    s_detailLbl = lv_label_create(s_detail);
    lv_obj_set_width(s_detailLbl, 206);
    lv_obj_set_style_text_color(s_detailLbl, C_TXT, 0);
    lv_label_set_long_mode(s_detailLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_detailLbl, "");
    lv_obj_t* hint = lv_label_create(s_detail);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 4);
    lv_obj_set_style_text_color(hint, C_SUB, 0);
    lv_label_set_text(hint, "tap to close");

    lv_screen_load(s_scr);
}

void rid_ui_tick(void) {
    if (!s_scr) return;

    uint8_t ch = rid_currentChannel();
    char chbuf[8];
    if (ch == 0) snprintf(chbuf, sizeof(chbuf), "BT");
    else         snprintf(chbuf, sizeof(chbuf), "%u", ch);
    char st[80];
    // compact single line: [radios] ch  N contacts  BT/WiFi RID frame counts
    snprintf(st, sizeof(st), "%s%s ch:%s  N:%d  bt:%lu wf:%lu",
             rid_wifiReady() ? "W" : "-", rid_bleReady() ? "B" : "-",
             chbuf, rid_droneCount(),
             (unsigned long)rid_btFrames(), (unsigned long)rid_wifiFrames());
    lv_label_set_text(s_statusLbl, st);

    uint32_t now = millis();
    int shown = 0;
    for (int i = 0; i < RID_MAX_DRONES; i++) {
        Row* rw = &s_rows[i];
        const RidDrone* r = rid_getDrone(i);
        bool live = r && (now - r->lastSeenMs <= RID_EXPIRE_MS);
        if (!live) { lv_obj_add_flag(rw->row, LV_OBJ_FLAG_HIDDEN); rw->idx = -1; continue; }
        rw->idx = i; shown++;

        char altStr[16];
        if (r->altGeo > INV_ALT)      snprintf(altStr, sizeof(altStr), "%.0fm", r->altGeo);
        else if (r->height > INV_ALT) snprintf(altStr, sizeof(altStr), "%.0fm", r->height);
        else                          snprintf(altStr, sizeof(altStr), "alt?");
        char sub[80];
        snprintf(sub, sizeof(sub), "%s %s %ddBm %s",
                 statusName(r->status), altStr, r->rssi, srcName(r->src));
        lv_label_set_text(rw->title, r->uasId);
        lv_label_set_text(rw->sub, sub);
        bool emerg = (r->status == ODID_STATUS_EMERGENCY);
        lv_obj_set_style_text_color(rw->title, emerg ? C_RED : C_TXT, 0);
        lv_obj_set_style_bg_color(rw->pin, emerg ? C_RED : C_GREEN, 0);
        lv_obj_clear_flag(rw->row, LV_OBJ_FLAG_HIDDEN);
    }
    if (shown > 0) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    if (s_detailIdx >= 0 && !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN))
        detail_show(s_detailIdx);
}

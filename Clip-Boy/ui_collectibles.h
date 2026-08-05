#pragma once
// ui_collectibles.h  -  Collectible data system for Clip-Boy badge
//
// Loads collectible definitions from LittleFS CSV (/data/collectibles.csv)
// with compiled-in fallback. Tracks collected state in NVS.
//
// Header-only: all functions are static, included from ui_test.ino
// after ui_config.h.

#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include "collectibles_csv.h"
#include "clipboy_pins.h"

// Serial debug commands (coll add/remove/list/reset). Enabled ONLY in test
// builds via `build.sh --test` (-DCOLL_DEBUG). MUST NOT ship in release bins:
// they let anyone unlock/wipe collectibles over USB and defeat the HR-code
// scavenger hunt. Uncomment locally only if you need them in a normal build.
// #define COLL_DEBUG

// ─────────────────────── CONSTANTS ─────────────────────────────────────────

#define COLL_MAX_ITEMS     128    // Max collectibles supported
#define COLL_MAX_TITLE      48    // Max title length
#define COLL_MAX_SOURCE     32    // Max source/IP length
#define COLL_MAX_DESC      400    // Max description length (longest is
                                  // SheetmetalCon at 371; coll_items is
                                  // PSRAM-allocated so the extra bytes are
                                  // cheap). Several lore descriptions
                                  // (Dark Tangent, LineCon Vet, the Fedora)
                                  // exceed 256 and were being truncated.
#define COLL_MAX_STAT_NAME  80    // Max stat name length (longest: 78 chars)
#define COLL_MAX_MODS        3    // Max modifiers per collectible
#define COLL_CSV_PATH   "/collectibles.csv"
#define COLL_NVS_NS     "coll"   // NVS namespace for collected state

// ─────────────────────── DATA STRUCTURES ───────────────────────────────────

struct CollMod {
    int8_t   value;              // e.g. +5, -10, 99 for ∞
    char     stat[COLL_MAX_STAT_NAME];
};

struct Collectible {
    uint8_t  id;                 // HR code ID (0-255)
    char     title[COLL_MAX_TITLE];
    char     source[COLL_MAX_SOURCE];
    uint8_t  tier;               // 0=Common, 1=Rare, 2=Legendary, 3=WYDTD
    char     desc[COLL_MAX_DESC];
    CollMod  mods[COLL_MAX_MODS];
    uint8_t  mod_count;          // Actual number of modifiers (0-3)
    bool     collected;          // Has the user found this one?
};

// ─────────────────────── RUNTIME STATE ─────────────────────────────────────

static Collectible *coll_items = NULL;   // PSRAM-allocated array
static uint16_t     coll_count = 0;      // Number loaded
static bool         coll_loaded = false;

// ─────────────────────── TIER HELPERS ──────────────────────────────────────

static uint8_t coll_parse_tier(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == 'R' || s[0] == 'r') return 1;  // Rare
    if (s[0] == 'L' || s[0] == 'l') return 2;  // Legendary
    if (s[0] == '0')               return 3;  // 0-Day
    if (s[0] == 'S' || s[0] == 's') return 3;  // legacy alias (SHA-512 Collision)
    if (s[0] == 'W' || s[0] == 'w') return 3;  // legacy alias (Why did you do this)
    return 0;  // Common
}

static const char* coll_tier_name(uint8_t tier) {
    switch (tier) {
        case 1:  return "Rare";
        case 2:  return "Legendary";
        case 3:  return "0-Day";
        default: return "Common";
    }
}

// ─────────────────────── CSV PARSER ────────────────────────────────────────
// Parses a line from the reformatted CSV:
// ID,Title,Source,Tier,Description,Mod1,Stat1,Mod2,Stat2,Mod3,Stat3

// Read one CSV field, handling quoted fields with embedded commas
static const char* coll_read_field(const char *p, char *out, size_t max) {
    if (!p || !*p) { out[0] = '\0'; return p; }

    size_t i = 0;
    if (*p == '"') {
        p++;  // skip opening quote
        while (*p) {
            if (*p == '"') {
                if (*(p + 1) == '"') {
                    if (i < max - 1) out[i++] = '"';  // escaped quote
                    p += 2;
                } else {
                    p++;  // closing quote
                    break;
                }
            } else {
                if (i < max - 1) out[i++] = *p;
                p++;
            }
        }
        if (*p == ',') p++;  // skip trailing comma
    } else {
        while (*p && *p != ',' && *p != '\n' && *p != '\r') {
            if (i < max - 1) out[i++] = *p;
            p++;
        }
        if (*p == ',') p++;
    }
    out[i] = '\0';
    return p;
}

static bool coll_parse_line(const char *line, Collectible *c) {
    char buf[COLL_MAX_DESC];  // Reusable buffer for field parsing

    // ID
    line = coll_read_field(line, buf, sizeof(buf));
    if (!buf[0]) return false;
    int id = atoi(buf);
    if (id < 0 || id > 255) return false;
    c->id = (uint8_t)id;

    // Title
    line = coll_read_field(line, c->title, COLL_MAX_TITLE);
    if (!c->title[0]) return false;

    // Source
    line = coll_read_field(line, c->source, COLL_MAX_SOURCE);

    // Tier
    line = coll_read_field(line, buf, sizeof(buf));
    c->tier = coll_parse_tier(buf);

    // Description
    line = coll_read_field(line, c->desc, COLL_MAX_DESC);

    // Modifiers: Mod1,Stat1,Mod2,Stat2,Mod3,Stat3
    c->mod_count = 0;
    for (int i = 0; i < COLL_MAX_MODS; i++) {
        // Modifier value
        line = coll_read_field(line, buf, sizeof(buf));
        if (!buf[0]) break;
        int val = atoi(buf);
        if (val > 127) val = 99;
        if (val < -127) val = -99;
        c->mods[i].value = (int8_t)val;

        // Stat name
        line = coll_read_field(line, c->mods[i].stat, COLL_MAX_STAT_NAME);
        if (!c->mods[i].stat[0]) break;

        c->mod_count++;
    }

    c->collected = false;
    return true;
}

// ─────────────────────── LOAD FROM LITTLEFS ────────────────────────────────

static uint16_t coll_load_csv(const char *path) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[COLL] Cannot open %s\n", path);
        return 0;
    }

    // Skip header line
    String header = f.readStringUntil('\n');
    Serial.printf("[COLL] CSV header: %s\n", header.c_str());

    uint16_t count = 0;
    while (f.available() && count < COLL_MAX_ITEMS) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        if (coll_parse_line(line.c_str(), &coll_items[count])) {
            count++;
        } else {
            Serial.printf("[COLL] Parse error on line: %.40s...\n", line.c_str());
        }
    }

    f.close();
    return count;
}

// ─────────────────────── COMPILED-IN FALLBACK ──────────────────────────────
// Parse the PROGMEM-embedded CSV (from collectibles_csv.h)

static uint16_t coll_load_progmem(void) {
    // Copy PROGMEM data to a PSRAM buffer for parsing
    char *buf = (char *)heap_caps_malloc(collectibles_csv_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.println("[COLL] PSRAM alloc failed for CSV buffer");
        return 0;
    }
    memcpy_P(buf, collectibles_csv_data, collectibles_csv_len);
    buf[collectibles_csv_len] = '\0';

    // Skip header line
    char *p = buf;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;

    uint16_t count = 0;
    while (*p && count < COLL_MAX_ITEMS) {
        // Find end of line
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (eol > p && coll_parse_line(p, &coll_items[count])) {
            count++;
        }

        if (saved == '\0') break;
        p = eol + 1;
    }

    heap_caps_free(buf);
    return count;
}

// ─────────────────────── NVS: COLLECTED STATE ──────────────────────────────
// Stored as a 32-byte bitfield (256 bits, one per possible HR code ID)

static uint8_t coll_found_bits[32] = {};  // 256-bit bitfield

static void coll_load_found(void) {
    Preferences p;
    p.begin(COLL_NVS_NS, true);
    memset(coll_found_bits, 0, sizeof(coll_found_bits));
    p.getBytes("found", coll_found_bits, sizeof(coll_found_bits));
    p.end();

    // Apply to loaded collectibles
    for (uint16_t i = 0; i < coll_count; i++) {
        uint8_t id = coll_items[i].id;
        coll_items[i].collected = (coll_found_bits[id / 8] >> (id % 8)) & 1;
    }

    // Count found
    int found = 0;
    for (uint16_t i = 0; i < coll_count; i++)
        if (coll_items[i].collected) found++;
    Serial.printf("[COLL] %d/%d collected\n", found, coll_count);
}

static void coll_save_found(void) {
    Preferences p;
    p.begin(COLL_NVS_NS, false);
    p.putBytes("found", coll_found_bits, sizeof(coll_found_bits));
    p.end();
    Serial.println("[COLL] Collected state saved");
}

static void coll_mark_found(uint8_t id) {
    coll_found_bits[id / 8] |= (1 << (id % 8));

    // Update the matching collectible
    for (uint16_t i = 0; i < coll_count; i++) {
        if (coll_items[i].id == id) {
            coll_items[i].collected = true;
            Serial.printf("[COLL] Collected: %s (ID %d)\n", coll_items[i].title, id);
            break;
        }
    }
    coll_save_found();
}

static bool coll_is_found(uint8_t id) {
    return (coll_found_bits[id / 8] >> (id % 8)) & 1;
}

// Inverse of coll_mark_found -- used by the manual-entry "Not this? Fix" path to
// undo a wrong scan's auto-unlock before the user re-enters the correct tag.
static void coll_mark_not_found(uint8_t id) {
    coll_found_bits[id / 8] &= ~(1 << (id % 8));
    for (uint16_t i = 0; i < coll_count; i++) {
        if (coll_items[i].id == id) { coll_items[i].collected = false; break; }
    }
    coll_save_found();
}

static void coll_reset_found(void) {
    memset(coll_found_bits, 0, sizeof(coll_found_bits));
    for (uint16_t i = 0; i < coll_count; i++)
        coll_items[i].collected = false;
    coll_save_found();
    Serial.println("[COLL] All collectibles reset");
}

// Set true by coll_init_sd() (SD section below); declared here so the
// export/import helpers can gate on it.
static bool coll_sd_available = false;

// ─────────────────────── SD EXPORT / IMPORT (DC34-92) ──────────────────────
// Save/restore collected-state to an SD file so finds survive a reflash and can
// be backed up. v2 = 54 bytes: magic "CBSAVE" (6) + version (1) + reserved (1),
// all PLAINTEXT; then a 42-byte payload [8..50) XOR'd with a per-badge keystream
// (bitfield 32 + arg.progress 1 + duck 1 + abandoned 1 + theme_active 1 +
// p4_correct 2 + p4_answered 2 + custom_hue 2); then CRC32 (4, LE) over the
// DECODED record [0..50).
// SPEEDBUMP (not crypto — deliberately crackable): the payload is XOR'd with a
// keystream derived from hash(eFuse MAC). Same badge -> decodes -> CRC matches;
// a save from a DIFFERENT badge decodes to garbage -> CRC fails -> rejected. This
// binds a backup to its badge (so it can't be trivially shared to forge another
// badge's ARG/Quanta) while surviving a factory reset (the eFuse MAC is immutable).
// The magic/version stay plaintext so v1 (legacy, 44-byte plaintext collectibles-
// only) files still import. Bits are memory-safe; phantom/blacklist IDs ignored.
#define COLL_SAV_PATH     "/collectibles.sav"
// Staging path for the export's write-verify-rename (see coll_export_sd). Distinct extension
// so a leftover .tmp from a yanked card is never mistaken for a backup by the importer, which
// only ever opens COLL_SAV_PATH.
#define COLL_SAV_TMP_PATH "/collectibles.tmp"
#define COLL_SAV_MAGIC    "CBSAVE"   // 6 bytes (plaintext, every version)
#define COLL_SAV_VERSION  2          // current export version
#define COLL_SAV_V1_SIZE  44         // legacy: plaintext, 32-byte bitfield only
#define COLL_SAV_V2_SIZE  54         // v2: + ARG progress/duck/abandoned/theme/p4 + hue
#define COLL_SAV_PAY_OFF  8          // XOR'd payload region start
#define COLL_SAV_PAY_LEN  42         // 32 bitfield + 10 arg/hue bytes

enum CollImport {
    COLL_IMP_OK = 0, COLL_IMP_NO_SD, COLL_IMP_NO_FILE,
    COLL_IMP_BAD_SIZE, COLL_IMP_BAD_MAGIC, COLL_IMP_BAD_VERSION, COLL_IMP_BAD_CRC,
    COLL_IMP_WRONG_BADGE   // v2 CRC fail after MAC-decode: different badge (or corrupt)
};

// Decoded save contents. v1 fills only bits (has_arg=false); v2 also carries the
// ARG progress + completionist hue.
struct CollSaveData {
    uint8_t  bits[32];
    bool     has_arg;
    uint8_t  arg_progress;   // P1..P5 bitmask
    uint8_t  arg_duck;       // Rubber Ducky theme unlocked
    uint8_t  arg_abandoned;  // opted out of the ARG (grants Zenith)
    uint8_t  arg_theme;      // theme_active selection
    uint16_t p4_correct;
    uint16_t p4_answered;
    uint16_t custom_hue;
};

// Cheap re-check of card presence at export/import time. cardType() is a cached
// read (no SPI re-init), so it's safe on the Marauder-shared SD singleton — we
// MUST NOT SD.begin() again here (see coll_init_sd: re-begin corrupts ownership
// and breaks PCAP). This catches a card pulled since boot when the SD layer
// reflects it; if the type is cached-stale the NO_FILE copy still names the card.
static bool coll_sd_check(void) {
    if (coll_sd_available && SD.cardType() == CARD_NONE) coll_sd_available = false;
    return coll_sd_available;
}

static uint32_t coll_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Per-badge XOR keystream for the v2 save speedbump. Seeded by hash(eFuse MAC)
// (arg_mac_seed, from arg_core.h) then run through xorshift32 — NOT a raw
// repeating-MAC XOR (that would leak the key against the known "CBSAVE" magic).
// Deterministic per badge; stable across factory reset (eFuse MAC is immutable).
static void coll_keystream(uint8_t *ks, size_t n) {
    uint32_t s = arg_mac_seed();
    if (!s) s = 0x1234ABCDu;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;   // xorshift32
        ks[i] = (uint8_t)(s >> 24);
    }
}

// Re-apply the bitfield to the loaded catalog (used after an import).
static void coll_reconcile(void) {
    for (uint16_t i = 0; i < coll_count; i++) {
        uint8_t id = coll_items[i].id;
        coll_items[i].collected = (coll_found_bits[id / 8] >> (id % 8)) & 1;
    }
}

static int coll_count_found(void) {
    int n = 0;
    for (uint16_t i = 0; i < coll_count; i++) if (coll_items[i].collected) n++;
    return n;
}
// True once every loaded collectible is found (the completionist / custom-theme gate).
static bool coll_all_found(void) {
    return coll_count > 0 && coll_count_found() >= (int)coll_count;
}

// Export collected-state + ARG progress to SD (v2, badge-bound). Returns true on success.
static bool coll_export_sd(void) {
    if (!coll_sd_check()) return false;
    uint8_t buf[COLL_SAV_V2_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, COLL_SAV_MAGIC, 6);
    buf[6] = COLL_SAV_VERSION;                          // 2
    buf[7] = 0;                                         // reserved
    memcpy(buf + 8, coll_found_bits, 32);              // [8..40) bitfield
    buf[40] = arg.progress;                            // [40] ARG progress P1..P5
    buf[41] = arg_duck_unlocked ? 1 : 0;               // [41] Rubber Ducky theme
    buf[42] = arg.abandoned;                           // [42] opted-out / Zenith
    buf[43] = arg.theme_active;                        // [43] selected reward theme
    buf[44] = arg.p4_correct & 0xFF;  buf[45] = (arg.p4_correct >> 8) & 0xFF;   // [44..46)
    buf[46] = arg.p4_answered & 0xFF; buf[47] = (arg.p4_answered >> 8) & 0xFF;  // [46..48)
    buf[48] = cfg.custom_hue & 0xFF;  buf[49] = (cfg.custom_hue >> 8) & 0xFF;   // [48..50)
    uint32_t crc = coll_crc32(buf, 50);               // CRC over the PLAINTEXT record
    buf[50] = crc & 0xFF;          buf[51] = (crc >> 8) & 0xFF;
    buf[52] = (crc >> 16) & 0xFF;  buf[53] = (crc >> 24) & 0xFF;
    // Speedbump: XOR the payload with this badge's keystream (magic/ver/CRC stay plaintext).
    uint8_t ks[COLL_SAV_PAY_LEN];
    coll_keystream(ks, COLL_SAV_PAY_LEN);
    for (int i = 0; i < COLL_SAV_PAY_LEN; i++) buf[COLL_SAV_PAY_OFF + i] ^= ks[i];
    // WRITE-TEMP -> VERIFY -> RENAME, because this file is not reconstructible. Opening the
    // real path with "w" TRUNCATES it before a single byte is written, so any failure after
    // that point (card pulled, FAT error, card full, short write) leaves the user with a
    // ruined backup and no other copy: a v2 save is badge-bound, so it is the only thing that
    // can restore THIS badge's ARG progress after a reflash. Destroying the good copy in order
    // to write the new one is the whole defect -- an earlier redundant SD.remove() made the
    // same mistake one step sooner.
    // The record itself was never the exposure (all fields + CRC32 over the plaintext + the
    // per-badge XOR are fully built in `buf` above, before anything touches the card). The
    // exposure is the ORDER of the filesystem operations, so that is what changes here.
    // Ordering rationale: FAT's rename fails when the destination exists, so the old file must
    // be removed between the verify and the rename -- that leaves a window, but it spans a
    // metadata rename with the new bytes already durable on the card, not a whole write. If
    // the rename still fails, the verified bytes are sitting at COLL_SAV_TMP_PATH and we fall
    // back to the old direct write, so this can never be worse than the previous behaviour.
    // The COMPLETE fix (no window at all) is two renames: path -> .bak, tmp -> path, then drop
    // .bak, restoring from .bak if the middle step fails. That is deliberately NOT taken a week
    // before DEF CON -- it adds three more failure modes to the save path to close a window that
    // spans two FAT metadata operations with the new bytes already durable and a RAM copy still
    // in hand. Written down so the next reader does not have to re-derive it.
    File f = SD.open(COLL_SAV_TMP_PATH, "w");
    if (!f) { Serial.println("[COLL] export: temp open failed"); return false; }
    size_t wrote = f.write(buf, COLL_SAV_V2_SIZE);
    f.close();
    if (wrote != COLL_SAV_V2_SIZE) {
        Serial.printf("[COLL] export: short write (%u/%u) -- previous backup left intact\n",
                      (unsigned)wrote, (unsigned)COLL_SAV_V2_SIZE);
        SD.remove(COLL_SAV_TMP_PATH);
        return false;
    }
    // Read it BACK off the card. A successful write() only proves the bytes reached the driver;
    // this proves they are readable, which is the property a restore depends on.
    {
        uint8_t vfy[COLL_SAV_V2_SIZE];
        File vf = SD.open(COLL_SAV_TMP_PATH, "r");
        size_t got = vf ? vf.read(vfy, COLL_SAV_V2_SIZE) : 0;
        if (vf) vf.close();
        if (got != COLL_SAV_V2_SIZE || memcmp(vfy, buf, COLL_SAV_V2_SIZE) != 0) {
            Serial.println("[COLL] export: readback mismatch -- previous backup left intact");
            SD.remove(COLL_SAV_TMP_PATH);
            return false;
        }
    }
    SD.remove(COLL_SAV_PATH);            // FAT rename requires the destination to be absent
    if (!SD.rename(COLL_SAV_TMP_PATH, COLL_SAV_PATH)) {
        Serial.printf("[COLL] export: rename failed; verified copy is at %s, retrying direct\n",
                      COLL_SAV_TMP_PATH);
        File df = SD.open(COLL_SAV_PATH, "w");
        if (!df) { Serial.println("[COLL] export: direct open failed"); return false; }
        size_t dw = df.write(buf, COLL_SAV_V2_SIZE);
        df.close();
        if (dw != COLL_SAV_V2_SIZE) return false;
        SD.remove(COLL_SAV_TMP_PATH);
        Serial.printf("[COLL] export -> %s (%u bytes, v2 badge-bound, direct)\n",
                      COLL_SAV_PATH, (unsigned)dw);
        return true;
    }
    Serial.printf("[COLL] export -> %s (%u bytes, v2 badge-bound, verified)\n",
                  COLL_SAV_PATH, (unsigned)wrote);
    return true;
}

// Load + validate /collectibles.sav into out_bits (32 bytes). Does NOT touch
// the live store — the caller decides merge vs overwrite, then applies via
// coll_apply_import(). Split out (DC34-92) so the UI can insert the
// merge/overwrite choice between validation and applying.
static CollImport coll_import_load(CollSaveData *out) {
    memset(out, 0, sizeof(*out));
    if (!coll_sd_check()) return COLL_IMP_NO_SD;
    File f = SD.open(COLL_SAV_PATH, "r");
    if (!f) return COLL_IMP_NO_FILE;
    size_t sz = f.size();
    if (sz != COLL_SAV_V1_SIZE && sz != COLL_SAV_V2_SIZE) { f.close(); return COLL_IMP_BAD_SIZE; }
    uint8_t buf[COLL_SAV_V2_SIZE];
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz)                           return COLL_IMP_BAD_SIZE;
    if (memcmp(buf, COLL_SAV_MAGIC, 6) != 0) return COLL_IMP_BAD_MAGIC;   // magic plaintext in both
    uint8_t ver = buf[6];

    if (ver == 1 && sz == COLL_SAV_V1_SIZE) {            // legacy plaintext, collectibles only
        uint32_t want = (uint32_t)buf[40] | ((uint32_t)buf[41] << 8) |
                        ((uint32_t)buf[42] << 16) | ((uint32_t)buf[43] << 24);
        if (coll_crc32(buf, 40) != want) return COLL_IMP_BAD_CRC;
        memcpy(out->bits, buf + 8, 32);
        out->has_arg = false;
        return COLL_IMP_OK;
    }
    if (ver == 2 && sz == COLL_SAV_V2_SIZE) {            // badge-bound: XOR-decode with our keystream
        uint8_t ks[COLL_SAV_PAY_LEN];
        coll_keystream(ks, COLL_SAV_PAY_LEN);
        for (int i = 0; i < COLL_SAV_PAY_LEN; i++) buf[COLL_SAV_PAY_OFF + i] ^= ks[i];
        uint32_t want = (uint32_t)buf[50] | ((uint32_t)buf[51] << 8) |
                        ((uint32_t)buf[52] << 16) | ((uint32_t)buf[53] << 24);
        if (coll_crc32(buf, 50) != want) return COLL_IMP_WRONG_BADGE;    // different badge OR corrupt
        memcpy(out->bits, buf + 8, 32);
        out->has_arg       = true;
        out->arg_progress  = buf[40];
        out->arg_duck      = buf[41];
        out->arg_abandoned = buf[42];
        out->arg_theme     = buf[43];
        out->p4_correct    = (uint16_t)buf[44] | ((uint16_t)buf[45] << 8);
        out->p4_answered   = (uint16_t)buf[46] | ((uint16_t)buf[47] << 8);
        out->custom_hue    = (uint16_t)buf[48] | ((uint16_t)buf[49] << 8);
        return COLL_IMP_OK;
    }
    return COLL_IMP_BAD_VERSION;   // ver/size mismatch, or newer firmware
}

// Apply validated bits to the live store. merge=true unions with current finds
// (bitwise OR — keep mine AND add the file's); merge=false replaces them
// (overwrite). Persists to NVS + reconciles the catalog. Returns the count of
// NEWLY-collected catalog items (always >=0 for merge; for overwrite the UI
// reports the new total via coll_count_found() instead).
void radio_sync_announced(void);  // fwd (defined in ui_nav.h): resync radio announce mask
static int coll_apply_import(const CollSaveData *d, bool merge) {
    int before = coll_count_found();
    if (merge) {
        for (int i = 0; i < 32; i++) coll_found_bits[i] |= d->bits[i];
    } else {
        memcpy(coll_found_bits, d->bits, 32);
    }
    coll_save_found();
    coll_reconcile();
    radio_sync_announced();   // imported finds shouldn't spuriously "new station!" later

    // ARG state (v2 saves only). MERGE = union achievements, keep prefs; OVERWRITE
    // = replace. NOTE progress is set directly (not via arg_set_flag) so the Quanta
    // reward animation doesn't fire on a silent restore; arg_quanta_earned() still
    // reads the imported bits. Cross-badge forgery is blocked upstream (WRONG_BADGE).
    if (d->has_arg) {
        if (merge) {
            uint8_t np = arg.progress | d->arg_progress;
            if (np != arg.progress)                 { arg.progress = np; arg_save_u8("progress", np); }
            if (d->arg_duck && !arg_duck_unlocked)  { arg_duck_unlocked = true; arg_save_u8("duck", 1); }
            if (d->arg_abandoned && !arg.abandoned) { arg.abandoned = 1; arg_save_u8("abandoned", 1); }
            if (d->p4_correct  > arg.p4_correct)    { arg.p4_correct  = d->p4_correct;  arg_save_u16("p4ok",  arg.p4_correct); }
            if (d->p4_answered > arg.p4_answered)   { arg.p4_answered = d->p4_answered; arg_save_u16("p4ans", arg.p4_answered); }
            // preferences (theme_active, custom_hue) kept as-is on merge
        } else {
            arg.progress      = d->arg_progress;        arg_save_u8("progress", arg.progress);
            arg_duck_unlocked = d->arg_duck ? true : false; arg_save_u8("duck", arg_duck_unlocked ? 1 : 0);
            arg.abandoned     = d->arg_abandoned;       arg_save_u8("abandoned", arg.abandoned);
            arg.theme_active  = d->arg_theme;           arg_save_u8("theme", arg.theme_active);
            arg.p4_correct    = d->p4_correct;          arg_save_u16("p4ok",  arg.p4_correct);
            arg.p4_answered   = d->p4_answered;         arg_save_u16("p4ans", arg.p4_answered);
            uint16_t hue = d->custom_hue > 359 ? 190 : d->custom_hue;
            cfg.custom_hue = hue; g_custom_hue = hue;   cfg_save_custom_theme();
        }
    }
    int after = coll_count_found();
    Serial.printf("[COLL] import applied (%s%s) -> %d collected\n",
                  merge ? "merge" : "overwrite", d->has_arg ? "+arg" : "", after);
    return after - before;
}

// Convenience: load+validate then apply in one call (used by the serial test
// command; the UI calls load/apply separately to interleave its modal).
// On success, *added_out (if non-NULL) gets the newly-collected count.
static CollImport coll_import_sd(bool merge, int *added_out) {
    CollSaveData d;
    CollImport r = coll_import_load(&d);
    if (r != COLL_IMP_OK) return r;
    int added = coll_apply_import(&d, merge);
    if (added_out) *added_out = added;
    return COLL_IMP_OK;
}

// ─────────────────────── LOOKUP ────────────────────────────────────────────

// Find collectible index by HR code ID, returns -1 if not found
static int coll_find_by_id(uint8_t id) {
    for (uint16_t i = 0; i < coll_count; i++) {
        if (coll_items[i].id == id) return (int)i;
    }
    return -1;
}

// ─────────────────────── UI REFRESH CALLBACK ─────────────────────────────
// Set by ui_nav.h to refresh the collectibles UI when data changes
static void (*coll_on_change)(void) = NULL;

// ─────────────────────── SD CARD ──────────────────────────────────────────
// SD card CSV overrides LittleFS/PROGMEM values. SD images under /images/
// override internal images. This enables user modding.

#define COLL_SD_CSV_PATH   "/collectibles.csv"
#define COLL_SD_IMG_PATH   "/images/"
#define COLL_SD_CSV_MAX_SIZE  (53 * 1024)  // ~1.5x current CSV size; reject larger

static SPIClass coll_sd_spi(HSPI);

static bool coll_init_sd(void) {
    // ClipBoy/Marauder's cb.begin() already mounts the SD on the shared global
    // SD singleton BEFORE coll_init() runs. Re-begin()ing it here with a SECOND
    // SPIClass on the same physical pins corrupts which instance owns the card,
    // which makes Marauder's PCAP writes fail intermittently. So REUSE the
    // existing mount when present; only do our own begin() as a fallback
    // (e.g. Marauder SD compiled out, or its mount failed).
    if (SD.cardType() != CARD_NONE) {
        Serial.println("[COLL] Reusing SD already mounted by ClipBoy (shared singleton)");
        coll_sd_available = true;
        return true;
    }
    coll_sd_spi.begin(CB_SD_SCK, CB_SD_MISO, CB_SD_MOSI, CB_SD_CS);
    if (SD.begin(CB_SD_CS, coll_sd_spi)) {
        Serial.println("[COLL] SD card mounted (own init - Marauder did not mount)");
        coll_sd_available = true;
        return true;
    }
    Serial.println("[COLL] No SD card (optional)");
    return false;
}

// Validate CSV header  -  must start with "ID,Title,Source,Tier,Description"
static bool coll_validate_header(const String &header) {
    // Normalize: trim whitespace
    String h = header;
    h.trim();
    // Check required columns (case-insensitive prefix match)
    // Expected: ID,Title,Source,Tier,Description,Mod1,Stat1,...
    if (h.length() < 29) return false;  // minimum "ID,Title,Source,Tier,Description"
    String lower = h;
    lower.toLowerCase();
    return lower.startsWith("id,title,source,tier,description");
}

// Load CSV from SD card  -  overwrites existing items by ID, adds new ones
static uint16_t coll_load_sd_csv(void) {
    if (!coll_sd_available) return 0;

    // Step 1: Check if CSV exists
    File f = SD.open(COLL_SD_CSV_PATH, "r");
    if (!f) {
        Serial.println("[COLL] No CSV on SD card");
        return 0;
    }

    // Step 2: Size guard  -  reject excessively large files
    size_t fsize = f.size();
    if (fsize > COLL_SD_CSV_MAX_SIZE) {
        Serial.printf("[COLL] SD CSV too large (%u > %u), skipping\n",
                      (unsigned)fsize, (unsigned)COLL_SD_CSV_MAX_SIZE);
        f.close();
        return 0;
    }

    // Step 3: Validate header
    String header = f.readStringUntil('\n');
    if (!coll_validate_header(header)) {
        Serial.printf("[COLL] SD CSV has invalid header: %.60s\n", header.c_str());
        f.close();
        return 0;
    }
    Serial.printf("[COLL] SD CSV header OK: %.60s\n", header.c_str());

    // Step 4: Parse rows  -  override by ID within HR code range (0-255)
    uint16_t overwritten = 0, added = 0, skipped = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        Collectible tmp;
        if (!coll_parse_line(line.c_str(), &tmp)) {
            skipped++;
            continue;
        }

        // ID range check (HR code uint8_t: 0-255)
        // Already enforced by coll_parse_line (id < 0 || id > 255 -> false)

        // Check if this ID already exists  -  overwrite it
        int existing = coll_find_by_id(tmp.id);
        if (existing >= 0) {
            bool was_collected = coll_items[existing].collected;
            coll_items[existing] = tmp;
            coll_items[existing].collected = was_collected;  // preserve NVS state
            overwritten++;
        } else if (coll_count < COLL_MAX_ITEMS) {
            coll_items[coll_count] = tmp;
            coll_count++;
            added++;
        }
    }
    f.close();

    Serial.printf("[COLL] SD: %d overwritten, %d added, %d skipped\n",
                  overwritten, added, skipped);
    return overwritten + added;
}

// ─────────────────────── A8 IMAGE LOADER ─────────────────────────────────
// Loads raw square A8 files: /images/<id>.a8 (e.g. 200x200 = 40000 bytes;
// 80x80 = 6400 also accepted -- dimension is inferred from the file size).
// Priority: SD card > LittleFS > compiled-in (caller handles compiled-in)
// Returns a PSRAM-allocated lv_image_dsc_t, or NULL if not found.
// Single-slot cache  -  reuses (and grows) the same buffer.

#define COLL_IMG_MIN_DIM   16     // reject implausibly tiny files
#define COLL_IMG_MAX_DIM   256    // and absurdly large ones
#define COLL_LFS_IMG_PATH  "/images/"

static uint8_t       *coll_ext_img_buf  = NULL;   // PSRAM pixel buffer
static size_t         coll_ext_img_cap  = 0;       // current buffer capacity
static lv_image_dsc_t coll_ext_img_dsc  = {};      // Reusable descriptor
static int16_t        coll_ext_img_id   = -1;      // Currently loaded ID

// Integer square root for inferring a square A8's edge length from its size.
static int coll_isqrt(size_t n) {
    int r = 0;
    while ((size_t)(r + 1) * (r + 1) <= n) r++;
    return r;
}

// Try to open <id>.a8 from a filesystem, read into buffer. Returns true on success.
static bool coll_try_load_a8(fs::FS &fs, const char *base, uint8_t id) {
    char path[40];
    snprintf(path, sizeof(path), "%s%d.a8", base, id);
    File f = fs.open(path, "r");
    if (!f) return false;

    size_t sz  = (size_t)f.size();
    // Early-reject absurdly large (hostile) files before coll_isqrt: its loop
    // is bounded by sqrt(sz), and on a multi-GB SD file (size_t)(r+1)*(r+1) can
    // wrap 32-bit and spin. The max valid A8 is COLL_IMG_MAX_DIM^2 bytes.
    if (sz > (size_t)COLL_IMG_MAX_DIM * COLL_IMG_MAX_DIM) {
        Serial.printf("[COLL] Image %s too large (%u bytes)\n", path, (unsigned)sz);
        f.close();
        return false;
    }
    int    dim = coll_isqrt(sz);
    if ((size_t)dim * dim != sz || dim < COLL_IMG_MIN_DIM || dim > COLL_IMG_MAX_DIM) {
        Serial.printf("[COLL] Image %s not a valid square A8 (%u bytes)\n", path, (unsigned)sz);
        f.close();
        return false;
    }

    // (Re)allocate if the cached buffer can't hold this image.
    if (coll_ext_img_cap < sz) {
        uint8_t *nb = (uint8_t *)heap_caps_realloc(coll_ext_img_buf, sz, MALLOC_CAP_SPIRAM);
        if (!nb) { f.close(); return false; }
        coll_ext_img_buf = nb;
        coll_ext_img_cap = sz;
    }

    size_t bytes_read = f.read(coll_ext_img_buf, sz);
    f.close();
    if (bytes_read < sz) {
        Serial.printf("[COLL] Image %s short read (%u/%u)\n", path, (unsigned)bytes_read, (unsigned)sz);
        return false;
    }

    coll_ext_img_dsc.header.magic   = LV_IMAGE_HEADER_MAGIC;
    coll_ext_img_dsc.header.cf      = LV_COLOR_FORMAT_A8;
    coll_ext_img_dsc.header.flags   = 0;
    coll_ext_img_dsc.header.w       = dim;
    coll_ext_img_dsc.header.h       = dim;
    coll_ext_img_dsc.header.stride  = dim;
    coll_ext_img_dsc.data_size      = sz;
    coll_ext_img_dsc.data           = coll_ext_img_buf;
    coll_ext_img_id = (int16_t)id;

    Serial.printf("[COLL] Loaded image %s (%dx%d)\n", path, dim, dim);
    return true;
}

// Load A8 image for collectible: SD > LittleFS > NULL (caller uses compiled-in)
static const lv_image_dsc_t* coll_load_image(uint8_t id) {
    // Already cached?
    if (coll_ext_img_id == (int16_t)id && coll_ext_img_buf) return &coll_ext_img_dsc;

    // Try SD card first
    if (coll_sd_available && coll_try_load_a8(SD, COLL_SD_IMG_PATH, id))
        return &coll_ext_img_dsc;

    // Try LittleFS
    if (coll_try_load_a8(LittleFS, COLL_LFS_IMG_PATH, id))
        return &coll_ext_img_dsc;

    return NULL;
}

// ─────────────────────── STAT ROLLUP ─────────────────────────────────────
// Sums all stat modifiers from collected items, grouped by stat name.

#define COLL_MAX_STATS  64    // Max unique stat names in rollup

struct CollStatRollup {
    char  stat[COLL_MAX_STAT_NAME];
    int16_t total;            // Summed value (can exceed int8 range)
};

static CollStatRollup *coll_rollup = NULL;
static uint16_t        coll_rollup_count = 0;

static void coll_compute_rollup(void) {
    // Allocate on first use (PSRAM)
    if (!coll_rollup) {
        coll_rollup = (CollStatRollup *)heap_caps_calloc(
            COLL_MAX_STATS, sizeof(CollStatRollup), MALLOC_CAP_SPIRAM);
        if (!coll_rollup) return;
    }
    coll_rollup_count = 0;

    for (uint16_t i = 0; i < coll_count; i++) {
        if (!coll_items[i].collected) continue;
        for (int m = 0; m < coll_items[i].mod_count; m++) {
            const CollMod &mod = coll_items[i].mods[m];
            // Find existing stat in rollup
            bool found = false;
            for (uint16_t s = 0; s < coll_rollup_count; s++) {
                if (strcasecmp(coll_rollup[s].stat, mod.stat) == 0) {
                    coll_rollup[s].total += mod.value;
                    found = true;
                    break;
                }
            }
            if (!found && coll_rollup_count < COLL_MAX_STATS) {
                strncpy(coll_rollup[coll_rollup_count].stat, mod.stat,
                        COLL_MAX_STAT_NAME - 1);
                coll_rollup[coll_rollup_count].total = mod.value;
                coll_rollup_count++;
            }
        }
    }
}

// ─────────────────────── INIT ──────────────────────────────────────────────

static void coll_init(void) {
    if (coll_loaded) return;

    // Allocate in PSRAM
    coll_items = (Collectible *)heap_caps_calloc(
        COLL_MAX_ITEMS, sizeof(Collectible), MALLOC_CAP_SPIRAM);
    if (!coll_items) {
        Serial.println("[COLL] PSRAM alloc failed!");
        return;
    }

    // Step 1: Load base data (LittleFS > PROGMEM)
    if (LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
        coll_count = coll_load_csv(COLL_CSV_PATH);
        if (coll_count > 0) {
            Serial.printf("[COLL] Loaded %d collectibles from LittleFS\n", coll_count);
        } else {
            Serial.println("[COLL] CSV empty or not found, using compiled-in data");
            coll_count = coll_load_progmem();
            Serial.printf("[COLL] Loaded %d collectibles from flash\n", coll_count);
        }
    } else {
        Serial.println("[COLL] LittleFS mount failed, using compiled-in data");
        coll_count = coll_load_progmem();
        Serial.printf("[COLL] Loaded %d collectibles from flash\n", coll_count);
    }

    // Step 2: Load collected state from NVS
    coll_load_found();

    // Step 3: SD card overlay (overwrites matching IDs, adds new ones)
    coll_init_sd();
    coll_load_sd_csv();
    // Re-apply NVS collected state after SD overlay
    for (uint16_t i = 0; i < coll_count; i++) {
        uint8_t id = coll_items[i].id;
        coll_items[i].collected = (coll_found_bits[id / 8] >> (id % 8)) & 1;
    }

    coll_loaded = true;
}

// ─────────────────────── DEBUG COMMANDS ───────────────────────────────────
// Serial commands: "coll add <id>", "coll add all", "coll remove <id>", "coll list", "coll reset"

#ifdef COLL_DEBUG
// Test helper (DC34-92): copy /<name> -> /collectibles.sav on the SD card so a
// fixture can be staged WITHOUT physically swapping the card. Deletes any
// existing save first, then byte-copies. The tester/harness then exercises the
// REAL importer (UI button or `coll import`) against the canonical path — no
// test-only parse fork. Arduino SD has no portable rename, so this is a copy.
static bool coll_stage_file(const String &name) {
    if (!coll_sd_check()) { Serial.println("[COLL] stage: no SD"); return false; }
    String src = name.startsWith("/") ? name : ("/" + name);
    File in = SD.open(src.c_str(), "r");
    if (!in) { Serial.printf("[COLL] stage: src not found: %s\n", src.c_str()); return false; }
    SD.remove(COLL_SAV_PATH);
    File out = SD.open(COLL_SAV_PATH, "w");
    if (!out) { in.close(); Serial.println("[COLL] stage: dst open failed"); return false; }
    uint8_t b[64];
    size_t total = 0;
    while (in.available()) {
        size_t n = in.read(b, sizeof(b));
        if (n == 0) break;
        out.write(b, n);
        total += n;
    }
    in.close();
    out.close();
    Serial.printf("[COLL] staged %s -> %s (%u bytes)\n",
                  src.c_str(), COLL_SAV_PATH, (unsigned)total);
    return true;
}

static const char *coll_import_result_str(CollImport r) {
    switch (r) {
        case COLL_IMP_OK:          return "OK";
        case COLL_IMP_NO_SD:       return "NO_SD";
        case COLL_IMP_NO_FILE:     return "NO_FILE";
        case COLL_IMP_BAD_SIZE:    return "BAD_SIZE";
        case COLL_IMP_BAD_MAGIC:   return "BAD_MAGIC";
        case COLL_IMP_BAD_VERSION: return "BAD_VERSION";
        case COLL_IMP_BAD_CRC:     return "BAD_CRC";
        case COLL_IMP_WRONG_BADGE: return "WRONG_BADGE";
        default:                   return "ERR";
    }
}

void radio_sync_announced(void);  // fwd (defined in ui_nav.h): resync the radio
                                  // announce mask after a BULK collectible change

static void coll_process_serial(const String &line) {
    if (!line.startsWith("coll ")) return;

    String cmd = line.substring(5);
    cmd.trim();

    if (cmd == "add all") {
        for (uint16_t i = 0; i < coll_count; i++) {
            coll_mark_found(coll_items[i].id);
        }
        Serial.printf("[COLL] Marked all %d collectibles as found\n", coll_count);
        radio_sync_announced();   // don't spuriously announce already-unlocked stations
        if (coll_on_change) coll_on_change();
    }
    else if (cmd.startsWith("add ")) {
        int id = cmd.substring(4).toInt();
        if (id < 0 || id > 255) {
            Serial.println("[COLL] Invalid ID (0-255)");
            return;
        }
        int ci = coll_find_by_id((uint8_t)id);
        if (ci < 0) {
            Serial.printf("[COLL] ID %d not in collectible list\n", id);
            return;
        }
        coll_mark_found((uint8_t)id);
        Serial.printf("[COLL] Added: %s (ID %d)\n", coll_items[ci].title, id);
        if (coll_on_change) coll_on_change();
    }
    else if (cmd.startsWith("remove ")) {
        int id = cmd.substring(7).toInt();
        if (id < 0 || id > 255) {
            Serial.println("[COLL] Invalid ID (0-255)");
            return;
        }
        // Clear the bit
        coll_found_bits[id / 8] &= ~(1 << (id % 8));
        for (uint16_t i = 0; i < coll_count; i++) {
            if (coll_items[i].id == (uint8_t)id) {
                coll_items[i].collected = false;
                Serial.printf("[COLL] Removed: %s (ID %d)\n", coll_items[i].title, id);
                break;
            }
        }
        coll_save_found();
        if (coll_on_change) coll_on_change();
    }
    else if (cmd == "list") {
        Serial.printf("[COLL] %d collectibles loaded:\n", coll_count);
        for (uint16_t i = 0; i < coll_count; i++) {
            Serial.printf("  %3d %-40s %s\n",
                coll_items[i].id,
                coll_items[i].title,
                coll_items[i].collected ? "[*]" : "[ ]");
        }
    }
    else if (cmd == "reset") {
        coll_reset_found();
        Serial.println("[COLL] All collectibles reset to uncollected");
        if (coll_on_change) coll_on_change();
    }
    else if (cmd.startsWith("stage ")) {
        coll_stage_file(cmd.substring(6));
    }
    else if (cmd == "import" || cmd == "import overwrite" || cmd == "import merge") {
        bool merge = cmd.endsWith("merge");
        int added = 0;
        CollImport r = coll_import_sd(merge, &added);
        Serial.printf("[COLL] import result: %s\n", coll_import_result_str(r));
        if (r == COLL_IMP_OK) {
            Serial.printf("[COLL] import %s: +%d new, %d total\n",
                          merge ? "merge" : "overwrite", added, coll_count_found());
            if (coll_on_change) coll_on_change();
        }
    }
    else {
        Serial.println("[COLL] Commands: coll add <id> | coll add all | coll remove <id> | "
                       "coll list | coll reset | coll stage <file> | coll import [merge|overwrite]");
    }
}
#endif

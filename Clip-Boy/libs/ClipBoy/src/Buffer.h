#pragma once

#ifndef Buffer_h
#define Buffer_h

#include "Arduino.h"
#include "FS.h"
#include "settings.h"
#include "esp_wifi_types.h"
#include "configs.h"

// Clip-Boy local patch (DC34): large PSRAM capture double-buffer (vs the 8KB internal BUF_SIZE)
// so a burst (a 4-way handshake amid beacons) isn't dropped while the slow SD write drains.
// Lazily allocated on first capture (see Buffer::ensureBuffers); falls back to BUF_SIZE.
#define CB_PCAP_BUF_SIZE (128 * 1024)

//#define BUF_SIZE 3 * 1024 // Had to reduce buffer size to save RAM. GG @spacehuhn
//#define SNAP_LEN 2324 // max len of each recieved packet

//extern bool useSD;

extern Settings settings_obj;

// Clip-Boy local patch (DC34-147): true when a PCAP write was skipped because 'Allow PCAP
// Saving' is off. The UI reads it after starting a tool to show a "not saving" note.
extern volatile bool cb_pcap_write_blocked;

// Clip-Boy local patch: true when a PCAP file could NOT be created (SD full, /pcaps
// directory full, or too many files) so the UI can warn the user + tell them how to
// fix it. Distinct from cb_pcap_write_blocked (which is the saving-off case).
extern volatile bool cb_pcap_write_failed;

// Clip-Boy local patch (PCAP perf): the monotonic NVS name-index ("seq") the last
// createFile claimed, exposed O(1) so the UI can warn when /pcaps has piled up (FAT
// dir ops grow with file count). It's a proxy, not an exact count -- it over-counts
// after the user prunes /pcaps externally (the /pcaps-missing reset zeroes it), so the
// UI must warn in coarse tiers, not print the raw value.
extern volatile uint32_t cb_pcap_seq;

// Safety cap on the exists()-verify scan (max probes past the NVS hint before we give
// up and report the directory full). Set well above what a FAT16 /pcaps subdir can hold
// (~30k) so the verify can always find a real gap; the hint keeps the common case O(1),
// so this bound is only approached on a pathological/desynced card. TEST_HARNESS makes
// it settable from serial to exercise the full-directory path without 30k real files.
#ifdef TEST_HARNESS
  extern volatile int cb_pcap_name_limit;
  #define CB_PCAP_NAME_LIMIT cb_pcap_name_limit
#else
  #define CB_PCAP_NAME_LIMIT 65536
#endif

class Buffer {
  public:
    Buffer();
    void pcapOpen(String file_name, fs::FS* fs, bool serial);
    void logOpen(String file_name, fs::FS* fs, bool serial);
    void gpxOpen(String file_name, fs::FS* fs, bool serial);
    void append(wifi_promiscuous_pkt_t *packet, int len);
    void append(String log);
    void save();
    // Clip-Boy local patch (PCAP perf): finalize an open capture -- drain the last
    // buffer, close the held file handle, stop accepting writes. Idempotent (safe when
    // no file is open). Called from the main/loop task when a pcap capture stops.
    void finalize();
    String getFileName();
    // Clip-Boy local patch (DC34-147): cap on-disk capture size (0 = unlimited).
    // Used for the LittleFS fallback so a capture can't starve the badge's data
    // partition. When the file reaches the cap, writing stops.
    void setMaxBytes(size_t m) { maxBytes = m; }
  private:
    void createFile(String name, bool is_pcap, bool is_gpx = false);
    void open(bool is_pcap);
    void openFile(String file_name, fs::FS* fs, bool serial, bool is_pcap, bool is_gpx = false);
    void add(const uint8_t* buf, uint32_t len, bool is_pcap);
    void write(int32_t n);
    void write(uint32_t n);
    void write(uint16_t n);
    void write(const uint8_t* buf, uint32_t len);
    void saveFs(const uint8_t* buf, uint32_t len);       // Clip-Boy (DC34): drain one buffer
    void saveSerial(const uint8_t* buf, uint32_t len);   // Clip-Boy (DC34): drain one buffer
    void ensureBuffers();      // Clip-Boy (DC34): lazy-alloc the PSRAM double-buffer

    uint8_t* bufA;
    uint8_t* bufB;
    uint32_t bufCap = 0;       // Clip-Boy (DC34): actual buffer capacity (PSRAM size or BUF_SIZE fallback)

    // Clip-Boy local patch (DC34): the RX-callback writer (WiFi task) and the main-task
    // save() share these -> volatile + guarded by buf_mux (in the .cpp). ping-pong:
    // save() flips useA to hand the writer a fresh buffer, then drains the old one.
    volatile uint32_t bufSizeA = 0;
    volatile uint32_t bufSizeB = 0;

    volatile bool writing = false; // acceppting writes to buffer
    volatile bool useA = true; // writing to bufA or bufB
    volatile bool saving = false; // (legacy; retained, no longer the sync primitive)
    // Clip-Boy (PCAP perf): re-entrancy guard for finalize() (size-cap -> saveFs ->
    // finalize while already finalizing). Main-task only; no lock needed.
    bool finalizing = false;

    String fileName = "/0.pcap";
    File file;
    fs::FS* fs;
    bool serial;
    size_t maxBytes = 0;       // Clip-Boy local patch (DC34-147): 0 = unlimited
    size_t writtenBytes = 0;   // running total written to disk this capture
};

#endif

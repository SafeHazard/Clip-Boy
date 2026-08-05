#include "Buffer.h"
#include "lang_var.h"
#include <Preferences.h>   // Clip-Boy: persist the pcap name-index hint (fast, no rescan)

// Clip-Boy local patch (DC34): serialize the double-buffer size/select fields between
// the RX-callback writer (WiFi task) and the main-task save(). Only SHORT sections are
// held under the spinlock (memcpy/size updates, and the save() swap) -- the blocking
// SD/serial drain runs OUTSIDE the lock on a buffer the writer no longer touches.
static portMUX_TYPE buf_mux = portMUX_INITIALIZER_UNLOCKED;

Buffer::Buffer(){
  // Clip-Boy local patch (DC34): defer allocation to ensureBuffers() (first capture) -- the
  // constructor runs at static-init, before PSRAM is guaranteed ready for ps_malloc().
  bufA = nullptr;
  bufB = nullptr;
  bufCap = 0;
}

// Clip-Boy local patch (DC34): allocate the large PSRAM capture double-buffer on first use so a
// burst (e.g. a 4-way handshake amid beacon traffic) isn't dropped while the SD write drains.
// The old 8KB (BUF_SIZE) internal buffers overflowed on bursts (missed EAPOL msg 2/4). Falls back
// to a small internal-RAM buffer if PSRAM is unavailable.
void Buffer::ensureBuffers(){
  if (bufA && bufB) return;
  bufCap = CB_PCAP_BUF_SIZE;
  bufA = (uint8_t*) ps_malloc(bufCap);
  bufB = (uint8_t*) ps_malloc(bufCap);
  if (!bufA || !bufB) {
    if (bufA) { free(bufA); bufA = nullptr; }
    if (bufB) { free(bufB); bufB = nullptr; }
    bufCap = BUF_SIZE;
    bufA = (uint8_t*) malloc(bufCap);
    bufB = (uint8_t*) malloc(bufCap);
  }
}

// Clip-Boy local patch: pcaps now live in the /pcaps/ subdir, NOT the SD root -- the
// FAT16 root has a hard ~512-entry cap, and keeping captures out of root keeps the card
// tidy. The filename search is BOUNDED (0..999) and fs->open()'s return is CHECKED, so a
// full directory / full card fails GRACEFULLY (sets cb_pcap_write_failed -> UI warning)
// instead of the old behaviour: it ignored open()'s return and wrote a pcap header into
// an invalid File, silently producing no capture with no user feedback.
void Buffer::createFile(String name, bool is_pcap, bool is_gpx){
  const char* ext = is_pcap ? ".pcap" : (is_gpx ? ".gpx" : ".log");

  if (!is_pcap) {
    // logs/gpx keep the historical SD-root path + a simple bounded search.
    bool found = false;
    for (int i = 0; i < CB_PCAP_NAME_LIMIT; i++) {
      fileName = "/" + name + "_" + (String)i + ext;
      if (!fs->exists(fileName)) { found = true; break; }
    }
    if (!found) { Serial.println("[CB] log/gpx create FAILED: directory full"); return; }
    Serial.println(fileName);
    file = fs->open(fileName, FILE_WRITE);
    if (!file) { Serial.println("[CB] log/gpx create FAILED: open failed"); return; }
    file.close();
    return;
  }

  // --- pcap: /pcaps subdir + a MONOTONIC NVS name-index ("seq"). Why monotonic: the
  // old scheme crawled from a hint with an exists() per probe, and a hint/card desync
  // could make each capture scan thousands of names -- an O(N^2) cliff that stutters
  // the UI once /pcaps holds thousands of files. "seq" is never crawled backward: it
  // only ever advances, so the common case is ONE exists() probe. It is still
  // exists()-VERIFIED (a small PROBE_BUDGET) so a card swap / desync can never truncate
  // an existing capture. Card-swap-safe: a fresh card has no /pcaps -> we reset seq=0.
  // The file handle is KEPT OPEN (see saveFs/finalize) so subsequent drains append to
  // it with an O(1) flush instead of the old per-150ms open+append+close re-scan. ---
  Preferences prefs;
  prefs.begin("pcap", false);

  uint32_t seq;
  if (!fs->exists("/pcaps")) {
    // Fresh / wiped / newly-swapped card with no /pcaps yet: create it and RESET the
    // counter so this card numbers cleanly from raw_0.
    fs->mkdir("/pcaps");
    seq = 0;
  } else {
    seq = prefs.getULong("seq", prefs.getULong("next", 0));  // migrate the legacy "next" key
  }

  const int PROBE_BUDGET = 64;
  bool found = false;
  for (int p = 0; p < PROBE_BUDGET; p++, seq++) {
    fileName = "/pcaps/" + name + "_" + String(seq) + ext;
    if (!fs->exists(fileName)) { found = true; break; }  // usually hits on the 1st probe
  }
  if (!found) {
    // The verified window was full (heavily desynced card). Jump well past it once and
    // try a single name; if THAT collides too, the dir is genuinely saturated -> fail.
    seq += 4096;
    fileName = "/pcaps/" + name + "_" + String(seq) + ext;
    if (fs->exists(fileName)) {
      prefs.end();
      cb_pcap_write_failed = true;
      Serial.println("[CB] PCAP: too many files, clear /pcaps");
      return;
    }
    seq++;
    found = true;
  }

  Serial.println(fileName);
  file = fs->open(fileName, FILE_WRITE);
  if (!file) {                              // card full / dir full / FS error
    prefs.end();
    cb_pcap_write_failed = true;
    Serial.println("[CB] PCAP create FAILED: SD full or directory full");
    return;
  }
  // Clip-Boy (PCAP perf): DO NOT close -- keep `file` open so the header write (open())
  // and every packet drain (saveFs) append sequentially to the same handle. finalize()
  // closes it when the capture stops.
  prefs.putULong("seq", seq);               // persist past the name we just claimed
  prefs.end();
  cb_pcap_seq = seq;                        // expose O(1) to the UI (pile-up warning)
}

void Buffer::open(bool is_pcap){
  ensureBuffers();   // Clip-Boy (DC34): PSRAM buffers ready before the header writes below
  bufSizeA = 0;
  bufSizeB = 0;

  bufSizeB = 0;

  writtenBytes = 0;  // Clip-Boy local patch (DC34-147): reset size-cap counter
  writing = true;

  if (is_pcap) {
    write(uint32_t(0xa1b2c3d4)); // magic number
    write(uint16_t(2)); // major version number
    write(uint16_t(4)); // minor version number
    write(int32_t(0)); // GMT to local correction
    write(uint32_t(0)); // accuracy of timestamps
    write(uint32_t(SNAP_LEN)); // max length of captured packets, in octets
    write(uint32_t(105)); // data link type
  }
}

String Buffer::getFileName() {
  return this->fileName;
}

// Clip-Boy local patch (DC34-147): set true when a PCAP write is blocked because
// 'Allow PCAP Saving' is off, so the UI can show a "not saving to file" note for the
// live-analysis tools (the ones that aren't hard-gated). The UI resets it per tool start.
volatile bool cb_pcap_write_blocked = false;
volatile bool cb_pcap_write_failed = false;
// Clip-Boy (PCAP perf): last claimed monotonic name-index; O(1) pile-up proxy for the UI.
volatile uint32_t cb_pcap_seq = 0;
#ifdef TEST_HARNESS
volatile int cb_pcap_name_limit = 65536;  // TEST: lower via `th_pcap_limit` to exercise full-dir
#endif

void Buffer::openFile(String file_name, fs::FS* fs, bool serial, bool is_pcap, bool is_gpx) {
  bool save_pcap = settings_obj.loadSetting<bool>("SavePCAP");
  if (!save_pcap) {
    if (is_pcap) cb_pcap_write_blocked = true;
    this->fs = NULL;
    this->serial = false;
    writing = false;
    return;
  }
  this->fs = fs;
  this->serial = serial;
  if (this->fs) {
    createFile(file_name, is_pcap, is_gpx);
    // Clip-Boy: if file creation failed (SD/dir full), drop the fs so we don't write a
    // header into a dead File. A serial sink (if requested) still streams.
    if (is_pcap && cb_pcap_write_failed) {
      this->fs = NULL;
    }
  }
  if (this->fs || this->serial) {
    open(is_pcap);
  } else {
    writing = false;
  }
}

void Buffer::pcapOpen(String file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, true);
}

void Buffer::logOpen(String file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false);
}

void Buffer::gpxOpen(String file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false, true);
}

void Buffer::add(const uint8_t* buf, uint32_t len, bool is_pcap){
  if(!bufA || !bufB) return;   // Clip-Boy (DC34): no buffer (alloc failed) -> nothing to do
  // Clip-Boy local patch (DC34): drop the WHOLE record if it won't fit the active
  // buffer (record = payload + up to the 16-byte pcap header). The mid-cycle self-
  // switch was removed -- save() now owns the ping-pong swap, so add() never mutates
  // useA (which the main task also touches). write() re-bounds-checks under the lock,
  // so a stale unlocked read here can only cause a benign spurious drop, never an overrun.
  {
    volatile uint32_t &sz = useA ? bufSizeA : bufSizeB;
    if(sz + len + 16 > bufCap) return;
  }

  uint32_t microSeconds = micros(); // e.g. 45200400 => 45s 200ms 400us
  uint32_t seconds = (microSeconds/1000)/1000; // e.g. 45200400/1000/1000 = 45200 / 1000 = 45s

  microSeconds -= seconds*1000*1000; // e.g. 45200400 - 45*1000*1000 = 45200400 - 45000000 = 400us (because we only need the offset)
  
  if (is_pcap) {
    write(seconds); // ts_sec
    write(microSeconds); // ts_usec
    write(len); // incl_len
    write(len); // orig_len
  }
  
  write(buf, len); // packet payload
}

// Clip-Boy local patch (DC34): gate on `writing` (set from the SavePCAP check in
// openFile) instead of hitting NVS via loadSetting() on EVERY captured frame in the
// RX callback -- a flash-backed read per packet was both slow and pointless (write()
// already early-returns when !writing). Removes the per-frame NVS access entirely.
void Buffer::append(wifi_promiscuous_pkt_t *packet, int len) {
  if (writing) add(packet->payload, len, true);
}

void Buffer::append(String log) {
  if (writing) add((const uint8_t*)log.c_str(), log.length(), false);
}

void Buffer::write(int32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint16_t n){
  uint8_t buf[2];
  buf[0] = n;
  buf[1] = n >> 8;
  write(buf,2);
}

void Buffer::write(const uint8_t* buf, uint32_t len){
  if(!writing || !bufA || !bufB) return;   // Clip-Boy (DC34): guard the fallback/alloc-fail case
  // Clip-Boy local patch (DC34): copy + size-bump under the spinlock so save()'s swap
  // can't zero/flip the buffer mid-write. Bounds-checked so a full buffer drops the
  // record instead of overrunning (was the old while(saving)delay busy-wait/TOCTOU).
  portENTER_CRITICAL(&buf_mux);
  if(useA){
    if(bufSizeA + len <= bufCap){ memcpy(&bufA[bufSizeA], buf, len); bufSizeA += len; }
  }else{
    if(bufSizeB + len <= bufCap){ memcpy(&bufB[bufSizeB], buf, len); bufSizeB += len; }
  }
  portEXIT_CRITICAL(&buf_mux);
}

void Buffer::saveFs(const uint8_t* buf, uint32_t len){
  // Clip-Boy (PCAP perf): pcap captures keep `file` OPEN across the whole capture
  // (createFile no longer closes it), so a drain is an O(1) append + flush -- no
  // per-150ms fs->open() that re-scans the FAT directory (the O(N) cliff at thousands
  // of /pcaps files). flush() updates the cached dir entry so the data is durable if
  // power drops mid-capture, without re-opening. The log/gpx path is UNCHANGED: those
  // close their handle in createFile, so `file` is closed here and we take the original
  // re-open/append/close branch. (A pcap whose createFile failed never reaches saveFs:
  // openFile nulls this->fs, and save() only calls saveFs when this->fs is set.)
  if (file) {
    if (len > 0) file.write(buf, len);
    file.flush();
  } else {
    file = fs->open(fileName, FILE_APPEND);
    if (!file) {
      Serial.println(text02+fileName+"'");
      return;
    }
    if (len > 0) file.write(buf, len);
    file.close();
  }

  // Clip-Boy local patch (DC34-147): enforce the optional size cap (LittleFS
  // fallback) so a capture can't fill the badge's data partition. finalize() closes
  // the held pcap handle cleanly (idempotent; re-entrancy-guarded so the save() it
  // calls can't recurse back through here).
  writtenBytes += (size_t)len;
  if (maxBytes && writtenBytes >= maxBytes) {
    Serial.printf("[PCAP] size cap reached (%u bytes) - capture stopped\n",
                  (unsigned)writtenBytes);
    finalize();
  }
}

void Buffer::saveSerial(const uint8_t* src, uint32_t len) {
  // Saves to main console UART, user-facing app will ignore these markers
  // Uses / and ] in markers as they are illegal characters for SSIDs
  const char* mark_begin = "[BUF/BEGIN]";
  const size_t mark_begin_len = strlen(mark_begin);
  const char* mark_close = "[BUF/CLOSE]";
  const size_t mark_close_len = strlen(mark_close);

  // Additional buffer and memcpy's so that a single Serial.write() is called
  // This is necessary so that other console output isn't mixed into buffer stream
  uint8_t* buf = (uint8_t*)malloc(mark_begin_len + len + mark_close_len);
  if(!buf) return;
  uint8_t* it = buf;
  memcpy(it, mark_begin, mark_begin_len);
  it += mark_begin_len;
  if(len > 0){ memcpy(it, src, len); it += len; }
  memcpy(it, mark_close, mark_close_len);
  it += mark_close_len;
  Serial.write(buf, it - buf);
  free(buf);
}

void Buffer::save() {
  // Clip-Boy local patch (DC34): ping-pong drain. Under the spinlock, snapshot the
  // active buffer + flip useA so the RX-callback writer immediately fills the OTHER
  // (already-drained, empty) buffer; then drain the snapshotted buffer OUTSIDE the
  // lock (the writer no longer touches it) -> no writer/drainer overlap, no TOCTOU.
  uint8_t* drainBuf = nullptr;
  uint32_t drainLen = 0;
  portENTER_CRITICAL(&buf_mux);
  if(useA){
    if(bufSizeA > 0){ drainBuf = bufA; drainLen = bufSizeA; bufSizeA = 0; useA = false; }
  } else {
    if(bufSizeB > 0){ drainBuf = bufB; drainLen = bufSizeB; bufSizeB = 0; useA = true; }
  }
  portEXIT_CRITICAL(&buf_mux);

  if(!drainBuf || drainLen == 0) return;

  if(this->fs)     saveFs(drainBuf, drainLen);
  if(this->serial) saveSerial(drainBuf, drainLen);
}

// Clip-Boy local patch (PCAP perf): finalize an open capture. Drains whatever is left
// in the ping-pong buffer, then closes the held file handle and stops writes. Idempotent
// and re-entrancy-guarded: the save() below can hit the size-cap branch in saveFs, which
// calls finalize() again -- the `finalizing` flag makes that a no-op so we never recurse
// back through save()/saveFs(). Called on the main/loop task only (see cb_stop_operation
// -> cb.finishCapture()), the same task that owns save()/saveFs()/`file`, so no lock is
// needed for the file state. Safe to call when nothing is open (no file, empty buffers).
void Buffer::finalize(){
  if (finalizing) return;
  finalizing = true;
  save();                     // drain the last buffer (writing still true so RX may add)
  if (file) file.close();     // release the held handle (no-op for a closed/absent handle)
  writing = false;            // stop accepting further writes (append() gates on this)
  finalizing = false;
}

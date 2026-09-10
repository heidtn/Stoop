#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <WebServer.h>
#include "MyMesh.h"

#ifndef STOOP_NODE_NAME
#define STOOP_NODE_NAME "stoop"
#endif

// TODO(Heidt) unclear if there's a limit in the FW on username length, 
// but https://github.com/meshcore-dev/MeshCore/issues/2613 suggests 23 is the max 
#define MAX_USERNAME_LEN 23
// save up to 8 bytes
#define STOOP_MAX_USERNAME_LEN 15 // assume 8 bytes for node name, Meshcore max username is 31 bytes
#define STOOP_MAX_NODENAME_LEN (MAX_USERNAME_LEN - STOOP_MAX_USERNAME_LEN - 1 /* @ */)

// TODO(Heidt) maybe want to have a static size for the node name, TBR 
#define STOOP_SENDER_OVERHEAD (STOOP_MAX_USERNAME_LEN + 1 /* @ */ + STOOP_MAX_NODENAME_LEN + 2 /* ": " */)
#define STOOP_MAX_POST_BYTES (MAX_TEXT_LEN - STOOP_SENDER_OVERHEAD)

#include "RateLimiter.h"
#include <web_assets.h>

#define STOOP_SESSION_HEADER "X-Stoop-Session"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

// escape a string for safe embedding inside a JSON string literal
static void appendJsonEscaped(String& out, const char* s) {
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default:
        out += ((uint8_t)*p < 0x20) ? ' ' : *p;
    }
  }
}

// interface manager
#include <helpers/MultiSerialInterface.h>
MultiSerialInterface interface_manager;

// include bluetooth interface
#if defined(BLE_PIN_CODE)
  #ifdef ESP32
    // include esp32 bluetooth interface
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #elif defined(NRF52_PLATFORM)
    // include nrf52 bluetooth interface
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #else
    #error "SerialBLEInterface is not defined for this platform"
  #endif
#endif

// include wifi interface
#ifdef WIFI_SSID
  #ifndef TCP_PORT
    #define TCP_PORT 5000
  #endif
  #ifdef ESP32
    // include esp32 wifi interface
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface wifi_interface;
    WebServer server(80);
    #include <DNSServer.h>
    DNSServer dns_server;
    RateLimiter rate_limiter;
  #else
    #error "SerialWifiInterface is not defined for this platform"
  #endif

static bool getSessionTokenFromRequest(uint8_t out_token[STOOP_SESSION_TOKEN_LEN]) {
  if (!server.hasHeader(STOOP_SESSION_HEADER)) return false;
  return mesh::Utils::fromHex(out_token, STOOP_SESSION_TOKEN_LEN, server.header(STOOP_SESSION_HEADER).c_str());
}

// shared by GET /api/limits and POST /api/post, send the current rate limiting status
// as json
static void sendLimitsJson(int code, const uint8_t session_token[STOOP_SESSION_TOKEN_LEN],
                            uint32_t user_tokens, uint32_t user_next_ms,
                            uint32_t node_tokens, uint32_t node_next_ms) {
  char token_hex[STOOP_SESSION_TOKEN_LEN * 2 + 1];
  mesh::Utils::toHex(token_hex, session_token, STOOP_SESSION_TOKEN_LEN);
  server.sendHeader(STOOP_SESSION_HEADER, token_hex);
  uint32_t next_ms = user_next_ms > node_next_ms ? user_next_ms : node_next_ms;

  String json = "{\"tokens_remaining\":";
  json += user_tokens;
  json += ",\"next_token_ms\":";
  json += next_ms;
  json += ",\"node_tokens_remaining\":";
  json += node_tokens;
  json += ",\"max_post_bytes\":";
  json += STOOP_MAX_POST_BYTES;
  json += "}";
  server.send(code, "application/json", json);
}
#endif

// include usb interface
#if defined(ENABLE_USB_INTERFACE)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface usb_serial_interface;
#endif

// include ethernet interface
#if defined(ETHERNET_ENABLED)
  #include <helpers/ethernet/EthernetInterface.h>
  ETHERNET_CLASS ethernet_interface;
#endif

// include hardware serial interface
#if defined(SERIAL_RX)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface hardware_serial_interface;
  HardwareSerial companion_serial(1);
#endif

// platform file system
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
    #if defined(EXTRAFS)
      #include <CustomLFS.h>
      CustomLFS ExtraFS(0xD4000, 0x19000, 128);
      DataStore store(InternalFS, ExtraFS, rtc_clock);
    #else
      DataStore store(InternalFS, rtc_clock);
    #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &interface_manager);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

void setup() {
  Serial.begin(115200);
  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#else
  #error "need to define filesystem"
#endif

// add bluetooth interface
#if defined(BLE_PIN_CODE)
  bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  interface_manager.addInterface(InterfaceType::Bluetooth, &bluetooth_interface);
#endif

// add wifi interface (as an open access point)
#ifdef WIFI_SSID
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID);
  WIFI_DEBUG_PRINTLN("WiFi AP started");

  dns_server.start(53, "*", WiFi.softAPIP()); // redirect all DNS lookups to us

  rate_limiter.begin(&fast_rng);

  static const char* collected_headers[] = { STOOP_SESSION_HEADER };
  server.collectHeaders(collected_headers, 1);

  server.on("/", HTTP_GET, [](){
    WIFI_DEBUG_PRINTLN("Serving index.html");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  server.on("/chat.html", HTTP_GET, [](){
    WIFI_DEBUG_PRINTLN("Serving chat.html");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)CHAT_HTML_GZ, CHAT_HTML_GZ_LEN);
  });
  server.on("/stoop.html", HTTP_GET, [](){
    WIFI_DEBUG_PRINTLN("Serving stoop.html");
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)STOOP_HTML_GZ, STOOP_HTML_GZ_LEN);
  });

  // returns recent #stoop channel messages as JSON: [{"ts":123,"text":"name: hi"}, ...]
  server.on("/api/stoop/messages", HTTP_GET, [](){
    int count = the_mesh.stoop_log_count;
    int n = count < MyMesh::STOOP_LOG_SIZE ? count : MyMesh::STOOP_LOG_SIZE;
    int start = count - n;
    String json = "[";
    for (int i = 0; i < n; i++) {
      auto& msg = the_mesh.stoop_log[(start + i) % MyMesh::STOOP_LOG_SIZE];
      if (i > 0) json += ",";
      json += "{\"ts\":";
      json += msg.timestamp;
      json += ",\"text\":\"";
      appendJsonEscaped(json, msg.text);
      json += "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  // reports the caller's current rate-limit status without consuming a token
  server.on("/api/limits", HTTP_GET, [](){
    uint8_t token[STOOP_SESSION_TOKEN_LEN];
    bool token_valid = getSessionTokenFromRequest(token);

    uint8_t out_token[STOOP_SESSION_TOKEN_LEN];
    uint32_t user_tokens, user_next_ms, node_tokens, node_next_ms;
    rate_limiter.getStatus(token, token_valid, millis(), out_token,
                            user_tokens, user_next_ms, node_tokens, node_next_ms);
    sendLimitsJson(200, out_token, user_tokens, user_next_ms, node_tokens, node_next_ms);
  });

  // sends a message on the #stoop channel as "username@<node name>: text",
  // subject to the per-user and per-node token buckets
  server.on("/api/post", HTTP_POST, [](){
    String text = server.arg("text");
    String username = server.arg("username");
    if (text.length() == 0 || (int)text.length() > (int)STOOP_MAX_POST_BYTES) {
      server.send(400, "text/plain", "bad request");
      return;
    }

    uint8_t token[STOOP_SESSION_TOKEN_LEN];
    bool token_valid = getSessionTokenFromRequest(token);

    uint8_t out_token[STOOP_SESSION_TOKEN_LEN];
    uint32_t user_tokens, user_next_ms, node_tokens, node_next_ms;
    bool allowed = rate_limiter.tryConsume(token, token_valid, millis(), out_token,
                                            user_tokens, user_next_ms, node_tokens, node_next_ms);
    if (!allowed) {
      sendLimitsJson(429, out_token, user_tokens, user_next_ms, node_tokens, node_next_ms);
      return;
    }

    if (username.length() == 0) username = "anon";
    if (username.length() > STOOP_MAX_USERNAME_LEN) username = username.substring(0, STOOP_MAX_USERNAME_LEN);

    char sender[STOOP_MAX_USERNAME_LEN + 1 + sizeof(STOOP_NODE_NAME) + 1];
    snprintf(sender, sizeof(sender), "%s@%s", username.c_str(), STOOP_NODE_NAME);

    ChannelDetails channel;
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    if (the_mesh.getChannel(0, channel) &&
        the_mesh.sendGroupMessage(ts, channel.channel, sender, text.c_str(), text.length())) {
      char full[sizeof(sender) + MAX_TEXT_LEN + 2];
      snprintf(full, sizeof(full), "%s: %s", sender, text.c_str());
      the_mesh.logStoopMsg(ts, full);
      sendLimitsJson(200, out_token, user_tokens, user_next_ms, node_tokens, node_next_ms);
    } else {
      // note: the token was already spent even though the radio-level send failed;
      // this is a rare failure path, not something worth refunding for
      server.send(500, "text/plain", "send failed");
    }
  });

  // Any request for a page that we don't host is treated as a captive portal probe.
  // and redirected to the main page.
  server.onNotFound([](){
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  });

  wifi_interface.begin(TCP_PORT);
  interface_manager.addInterface(InterfaceType::WiFi, &wifi_interface);
  server.begin();
#endif

// add usb interface
#if defined(ENABLE_USB_INTERFACE)
  usb_serial_interface.begin(Serial);
  interface_manager.addInterface(InterfaceType::USB, &usb_serial_interface);
#endif

// add ethernet interface
#if defined(ETHERNET_ENABLED)
  ethernet_interface.begin();
  interface_manager.addInterface(InterfaceType::Ethernet, &ethernet_interface);
#endif

// add hardware serial interface
#if defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  hardware_serial_interface.begin(companion_serial);
  interface_manager.addInterface(InterfaceType::HardwareSerial, &hardware_serial_interface);
#endif

  the_mesh.startInterface(interface_manager);
  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  interface_manager.loop();
  sensors.loop();
#ifdef WIFI_SSID
  server.handleClient();
  dns_server.processNextRequest();
#endif
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif

  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#endif
  }
}

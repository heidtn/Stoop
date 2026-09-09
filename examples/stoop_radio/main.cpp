#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <WebServer.h>
#include "MyMesh.h"
#include <web_assets.h>

#ifndef STOOP_NODE_NAME
#define STOOP_NODE_NAME "stoop"
#endif

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
  #else
    #error "SerialWifiInterface is not defined for this platform"
  #endif
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

  // sends a message on the #stoop channel as "username@<node name>: text"
  server.on("/api/stoop/send", HTTP_POST, [](){
    String text = server.arg("text");
    String username = server.arg("username");
    if (text.length() == 0 || (int)text.length() > MAX_TEXT_LEN) {
      server.send(400, "text/plain", "bad request");
      return;
    }
    if (username.length() == 0) username = "anon";
    if (username.length() > 24) username = username.substring(0, 24);

    char sender[24 + 1 + sizeof(STOOP_NODE_NAME) + 1];
    snprintf(sender, sizeof(sender), "%s@%s", username.c_str(), STOOP_NODE_NAME);

    ChannelDetails channel;
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    if (the_mesh.getChannel(0, channel) &&
        the_mesh.sendGroupMessage(ts, channel.channel, sender, text.c_str(), text.length())) {
      char full[sizeof(sender) + MAX_TEXT_LEN + 2];
      snprintf(full, sizeof(full), "%s: %s", sender, text.c_str());
      the_mesh.logStoopMsg(ts, full);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(500, "text/plain", "send failed");
    }
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

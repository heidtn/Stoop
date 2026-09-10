#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <WebServer.h>
#include "MyMesh.h"
#include "RateLimiter.h"
#include "server.h"
#include <esp_wifi.h>

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
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
    ConnectionLimiter connection_limiter;
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
  // For some reason, android wants the AP to be on IP 8.8.8.8 for captive portal detection to work
  IPAddress ap_ip(8, 8, 8, 8);
  WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(WIFI_SSID, NULL, 1, false, STOOP_MAX_CONNECTED_CLIENTS);
  WIFI_DEBUG_PRINTLN("WiFi AP started");

  dns_server.start(53, "*", WiFi.softAPIP()); // redirect all DNS lookups to us

  rate_limiter.begin(&fast_rng);

  configureServer();

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        WIFI_DEBUG_PRINTLN("Client connected: %02X:%02X:%02X:%02X:%02X:%02X",
                           info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                           info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
                           info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
        connection_limiter.connectClient(info.wifi_ap_staconnected.mac, millis());
        break;
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        WIFI_DEBUG_PRINTLN("Client disconnected: %02X:%02X:%02X:%02X:%02X:%02X",
                           info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
                           info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
                           info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
        connection_limiter.disconnectClient(info.wifi_ap_stadisconnected.mac);
        break;
      default:
        break;
    }
  });

  wifi_interface.begin(TCP_PORT);
  interface_manager.addInterface(InterfaceType::WiFi, &wifi_interface);
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

// If a client has been on the network too long, disconnect them to make room for new clients.
void checkDisconnectClient() {
  #ifdef WIFI_SSID
    uint8_t mac[6];
    while (connection_limiter.getClientDisconnect(mac, millis())) {
      uint16_t aid;
      esp_wifi_ap_get_sta_aid(mac, &aid);
      esp_err_t err = esp_wifi_deauth_sta(aid);
      WIFI_DEBUG_PRINTLN("Disconnecting client %02X:%02X:%02X:%02X:%02X:%02X due to max connection length",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
  #endif
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
  // TODO(Heidt) we can probably make this only check every once in awhile
  checkDisconnectClient();

  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#endif
  }
}

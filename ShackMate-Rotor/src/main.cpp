// --------------------
// main.cpp
// --------------------
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <time.h>       // For NTP and time functions
#include <LittleFS.h>   // For serving HTML files from filesystem
#include <ArduinoOTA.h> // OTA update support
#include <vector>
#include "esp_task_wdt.h"  // Watchdog Timer
#include "esp_heap_caps.h" // For heap_caps_get_total_size.h
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ArduinoJson.h> // For JSON processing
#include <math.h>
#include "ble_provisioning.h"  // BLE provisioning functions

// --------------------
// Global Preferences Instance (for WiFi credentials)
// --------------------
Preferences wifiPrefs;  // All WiFi credentials will be stored in the "wifi" namespace

// --------------------
// Project Constants
// --------------------
#define NAME "ShackMate - Rotor (G-5500)"
#define VERSION "1.0"
#define AUTHOR "Half Baked Circuits"
#define MDNS_NAME "shackmate-rotor"
#define UDP_PORT 4210     // UDP broadcast port (new JSON format)
#define MAC_UDP_PORT 9932 // UDP listener port for MacLogger updates
#define LED_GREEN 48      // RGB LED

// --------------------
// Global Variables
// --------------------
String deviceIP = "";
String wsPortStr = "4000";    // Default WebSocket port
String rotorPortStr = "4532"; // Default rotor port

// Current positions (updated via EasyComm) and target positions.
int currentAz = 240;
int currentEl = 60;
int targetAZ = 0;
int targetEL = 0;

bool tracking = false;
bool autoTrack = true; // Default to AUTOMATIC mode

// Global satellite info variables – updated via UDP messages.
String satName = "";
String channelName = "";
// Preserve the last non‑empty satellite info:
String lastSatName = "";
String lastChannelName = "";

// Global variable for the grid square.
String gridSQ = "";

AsyncWebServer httpServer(80);      // HTTP server on port 80
AsyncWebServer *wsServer = nullptr; // Pointer for the WebSocket server
AsyncWebSocket ws("/ws");           // WebSocket on path "/ws"
WiFiUDP udp;                        // UDP broadcast listener
WiFiUDP macLoggerUdp;               // UDP listener for MacLogger updates

unsigned long lastUdpBroadcast = 0;
const unsigned long broadcastInterval = 2000; // 2 seconds
std::vector<String> broadcastMessages;

// For UDP position broadcast (to avoid broadcasting every frame)
int prevTargetAZ = -1;
int prevTargetEL = -1;

// Global to store the last state update message (to avoid duplicates)
String lastStateMsg = "";

// --------------------
// Forward Declarations
// --------------------
String loadFile(const char *path);
String processTemplate(String tmpl);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleFavicon(AsyncWebServerRequest *request);
void handleRoot(AsyncWebServerRequest *request);
void handleAbout(AsyncWebServerRequest *request);
void handleConfig(AsyncWebServerRequest *request);
void handleBroadcasts(AsyncWebServerRequest *request);
void handleSaveConfig(AsyncWebServerRequest *request);
void handleRestoreConfig(AsyncWebServerRequest *request);
void handleSaveMemory(AsyncWebServerRequest *request);
void handleGetMemory(AsyncWebServerRequest *request);
void handleCalcGrid(AsyncWebServerRequest *request);
void udpWebSocketTask(void *parameter);
void broadcastTrackingUDP(bool enabled);
void broadcastPosition(int az, int el);
void broadcastStateUpdate(bool force = false);
void startWiFiManager();

// --------------------
// Maidenhead Grid Helpers
// --------------------
double myRadians(double deg) { return deg * PI / 180.0; }
double myDegrees(double rad) { return rad * 180.0 / PI; }

struct LatLon {
  double lat;
  double lon;
};

LatLon maidenheadToLatLon(const String &rawLocator) {
  String loc = rawLocator;
  loc.toUpperCase();
  if (loc.length() < 4)
    return {0, 0};
  int fieldLon = loc.charAt(0) - 'A';
  int fieldLat = loc.charAt(1) - 'A';
  int squareLon = loc.charAt(2) - '0';
  int squareLat = loc.charAt(3) - '0';
  double lon = fieldLon * 20.0 - 180.0;
  double lat = fieldLat * 10.0 - 90.0;
  lon += squareLon * 2.0;
  lat += squareLat * 1.0;
  lon += 1.0;
  lat += 0.5;
  if (loc.length() >= 6) {
    char subsqLonChar = loc.charAt(4);
    char subsqLatChar = loc.charAt(5);
    if ((subsqLonChar >= 'A' && subsqLonChar <= 'X') &&
        (subsqLatChar >= 'A' && subsqLatChar <= 'X')) {
      int subsqLonVal = subsqLonChar - 'A';
      int subsqLatVal = subsqLatChar - 'A';
      double subsqLonWidth = 2.0 / 24.0;
      double subsqLatHeight = 1.0 / 24.0;
      lon += subsqLonVal * subsqLonWidth;
      lat += subsqLatVal * subsqLatHeight;
      lon += subsqLonWidth / 2.0;
      lat += subsqLatHeight / 2.0;
    }
  }
  return {lat, lon};
}

double haversine(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0;
  double dLat = myRadians(lat2 - lat1);
  double dLon = myRadians(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(myRadians(lat1)) * cos(myRadians(lat2)) *
             sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  double distanceKm = R * c;
  return distanceKm * 0.621371;
}

double calculateBearing(double lat1, double lon1, double lat2, double lon2) {
  double dLon = myRadians(lon2 - lon1);
  double y = sin(dLon) * cos(myRadians(lat2));
  double x = cos(myRadians(lat1)) * sin(myRadians(lat2)) -
             sin(myRadians(lat1)) * cos(myRadians(lat2)) * cos(dLon);
  double brng = atan2(y, x);
  brng = myDegrees(brng);
  if (brng < 0)
    brng += 360;
  return brng;
}

// --------------------
// /calcGrid Handler
// --------------------
void handleCalcGrid(AsyncWebServerRequest *request) {
  if (!request->hasArg("dest")) {
    request->send(400, "text/plain", "Missing destination parameter");
    return;
  }
  String dest = request->arg("dest");
  Preferences configPrefs;
  configPrefs.begin("config", true);
  String sourceGrid = configPrefs.getString("grid_sq", "");
  configPrefs.end();
  if (sourceGrid.length() < 4 || dest.length() < 4) {
    request->send(400, "text/plain", "Both source and destination grid locators must be >= 4 chars");
    return;
  }
  LatLon src = maidenheadToLatLon(sourceGrid);
  LatLon dst = maidenheadToLatLon(dest);
  double distanceMiles = haversine(src.lat, src.lon, dst.lat, dst.lon);
  double dLon = myRadians(dst.lon - src.lon);
  double y = sin(dLon) * cos(myRadians(dst.lat));
  double x = cos(myRadians(src.lat)) * sin(myRadians(dst.lat)) -
             sin(myRadians(src.lat)) * cos(myRadians(dst.lat)) * cos(dLon);
  double bearingDeg = myDegrees(atan2(y, x));
  if (bearingDeg < 0)
    bearingDeg += 360.0;
  DynamicJsonDocument doc(128);
  doc["distance"] = distanceMiles;
  doc["bearing"] = bearingDeg;
  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
  Serial.println("calcGrid complete");
}

// --------------------
// Memory Handlers (M1–M6)
// --------------------
void handleSaveMemory(AsyncWebServerRequest *request) {
  if (!request->hasArg("slot") || !request->hasArg("az") || !request->hasArg("el")) {
    request->send(400, "text/plain", "Missing parameters");
    return;
  }
  int slot = request->arg("slot").toInt();
  if (slot < 1 || slot > 6) {
    request->send(400, "text/plain", "Invalid slot number (must be 1-6)");
    return;
  }
  int az = request->arg("az").toInt();
  int el = request->arg("el").toInt();
  Preferences memPrefs;
  memPrefs.begin("memory", false);
  String keyAz = "M" + String(slot) + "_az";
  String keyEl = "M" + String(slot) + "_el";
  memPrefs.putInt(keyAz.c_str(), az);
  memPrefs.putInt(keyEl.c_str(), el);
  memPrefs.end();
  request->send(200, "text/plain", "Memory M" + String(slot) + " saved.");
  Serial.println("Memory saved for slot M" + String(slot));
}

void handleGetMemory(AsyncWebServerRequest *request) {
  if (!request->hasArg("slot")) {
    request->send(400, "text/plain", "Missing slot parameter");
    return;
  }
  int slot = request->arg("slot").toInt();
  if (slot < 1 || slot > 6) {
    request->send(400, "text/plain", "Invalid slot number (must be 1-6)");
    return;
  }
  Preferences memPrefs;
  memPrefs.begin("memory", false);
  String keyAz = "M" + String(slot) + "_az";
  String keyEl = "M" + String(slot) + "_el";
  int az = memPrefs.getInt(keyAz.c_str(), 0);
  int el = memPrefs.getInt(keyEl.c_str(), 0);
  memPrefs.end();
  DynamicJsonDocument doc(128);
  doc["az"] = az;
  doc["el"] = el;
  String response;
  serializeJson(doc, response);
  request->send(200, "application/json", response);
  Serial.println("Memory recalled for slot M" + String(slot));
}

// --------------------
// Utility Functions
// --------------------
String loadFile(const char *path) {
  File file = LittleFS.open(path, "r");
  if (!file || file.isDirectory()) {
    Serial.printf("Failed to open file: %s\n", path);
    return "";
  }
  String content;
  while (file.available())
    content += (char)file.read();
  file.close();
  return content;
}

String processTemplate(String tmpl) {
  struct tm timeinfo;
  char timeStr[64];
  if (getLocalTime(&timeinfo))
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  else
    strcpy(timeStr, "TIME_NOT_SET");
  tmpl.replace("%PROJECT_NAME%", String(NAME));
  tmpl.replace("%TIME%", String(timeStr));
  tmpl.replace("%IP%", deviceIP);
  tmpl.replace("%WEBSOCKET_PORT%", wsPortStr);
  tmpl.replace("%UDP_PORT%", String(UDP_PORT));
  tmpl.replace("%VERSION%", VERSION);
  tmpl.replace("%ROTOR_PORT%", rotorPortStr);
  unsigned long uptimeSec = millis() / 1000;
  tmpl.replace("%UPTIME%", String(uptimeSec) + " s");
  tmpl.replace("%GRID_SQ%", gridSQ);
  tmpl.replace("%CHIP_ID%", String(ESP.getEfuseMac(), HEX));
  tmpl.replace("%CHIP_REV%", String(ESP.getChipRevision()));
  tmpl.replace("%FLASH_TOTAL%", String(ESP.getFlashChipSize()));
  tmpl.replace("%PSRAM_SIZE%", (ESP.getPsramSize() > 0 ? String(ESP.getPsramSize()) : "N/A"));
  tmpl.replace("%CPU_FREQ%", String(ESP.getCpuFreqMHz()));
  tmpl.replace("%FREE_HEAP%", String(ESP.getFreeHeap()));
  tmpl.replace("%MEM_USED%", "N/A");
  tmpl.replace("%MEM_TOTAL%", "N/A");
  tmpl.replace("%SKETCH_USED%", "N/A");
  tmpl.replace("%SKETCH_TOTAL%", "N/A");
  tmpl.replace("%TEMPERATURE_C%", "N/A");
  tmpl.replace("%TEMPERATURE_F%", "N/A");
  return tmpl;
}

// --------------------
// UDP Broadcast Helper Functions
// --------------------
void broadcastTrackingUDP(bool enabled) {
  struct tm timeinfo;
  char timeStr[64];
  if (getLocalTime(&timeinfo))
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  else
    strcpy(timeStr, "TIME_NOT_SET");
  uint64_t chipid = ESP.getEfuseMac();
  String chipIdStr = String((uint32_t)(chipid >> 32), HEX) +
                     String((uint32_t)chipid, HEX);
  DynamicJsonDocument doc(256);
  doc["TimeStamp"] = timeStr;
  doc["Device"] = chipIdStr;
  doc["Name"] = "shackmate-rotor";
  String udpMsg;
  serializeJson(doc, udpMsg);
  udp.beginPacket("255.255.255.255", UDP_PORT);
  udp.print(udpMsg);
  udp.endPacket();
}

void broadcastPosition(int az, int el) {
  struct tm timeinfo;
  char timeStr[64];
  if (getLocalTime(&timeinfo))
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  else
    strcpy(timeStr, "TIME_NOT_SET");
  uint64_t chipid = ESP.getEfuseMac();
  String chipIdStr = String((uint32_t)(chipid >> 32), HEX) +
                     String((uint32_t)chipid, HEX);
  DynamicJsonDocument doc(256);
  doc["TimeStamp"] = timeStr;
  doc["Device"] = chipIdStr;
  doc["Name"] = "shackmate-rotor";
  doc["TargetAZ"] = az;
  doc["TargetEL"] = el;
  String udpMsg;
  serializeJson(doc, udpMsg);
  udp.beginPacket("255.255.255.255", UDP_PORT);
  udp.print(udpMsg);
  udp.endPacket();
}

// --------------------
// Broadcast State Update Function
// Also saves rotor positions if they have stopped moving.
// --------------------
void broadcastStateUpdate(bool force) {
  DynamicJsonDocument doc(256);
  doc["type"] = "stateUpdate";
  doc["targetAZ"] = (int)round(targetAZ);
  doc["targetEL"] = (int)round(targetEL);
  doc["autoTrack"] = autoTrack;
  doc["rotorsEnabled"] = true;
  if (satName.length() > 0)
    lastSatName = satName;
  doc["satName"] = lastSatName;
  if (channelName.length() > 0)
    lastChannelName = channelName;
  doc["channelName"] = lastChannelName;
  doc["satIndicator"] = autoTrack ? "ENABLED" : "DISABLED";
  
  // Save rotor positions if they have essentially stopped.
  {
    Preferences rotorPrefs;
    rotorPrefs.begin("rotor", false);
    rotorPrefs.putInt("rotorAZPosition", targetAZ);
    rotorPrefs.putInt("rotorELPosition", targetEL);
    rotorPrefs.end();
  }
  
  String outMsg;
  serializeJson(doc, outMsg);
  if (force || outMsg != lastStateMsg) {
    lastStateMsg = outMsg;
    Serial.printf("Broadcasting state update: %s\n", outMsg.c_str());
    ws.textAll(outMsg);
  }
}

// --------------------
// WebSocket Event Handler
// --------------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  String msg;
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA: {
      for (size_t i = 0; i < len; i++)
        msg += (char)data[i];
      server->textAll(msg);
      DynamicJsonDocument doc(256);
      DeserializationError error = deserializeJson(doc, msg);
      if (!error) {
        if (doc.containsKey("command") && String(doc["command"].as<const char*>()) == "stateUpdate") {
          broadcastStateUpdate(true);
        }
        const char *typeStr = doc["type"];
        if (typeStr && strcmp(typeStr, "setTarget") == 0) {
          if (doc.containsKey("targetAZ") && doc.containsKey("targetEL")) {
            targetAZ = doc["targetAZ"].as<int>();
            targetEL = doc["targetEL"].as<int>();
            broadcastStateUpdate(true);
            broadcastPosition(targetAZ, targetEL);
          }
        }
        if (typeStr && strcmp(typeStr, "setMode") == 0) {
          if (doc.containsKey("mode")) {
            String mode = doc["mode"].as<String>();
            if (mode.equalsIgnoreCase("manual"))
              autoTrack = false;
            else if (mode.equalsIgnoreCase("automatic"))
              autoTrack = true;
            broadcastStateUpdate(true);
          }
        }
        if (typeStr && strcmp(typeStr, "rotorSet") == 0) {
          if (doc.containsKey("set_AZ") && doc.containsKey("set_EL")) {
            int az = doc["set_AZ"].as<int>();
            int el = doc["set_EL"].as<int>();
            currentAz = az;
            currentEl = el;
            String outMsg;
            serializeJson(doc, outMsg);
            ws.textAll(outMsg);
          }
        }
      }
      break;
    }
    default:
      break;
  }
}

// --------------------
// HTTP Handlers
// --------------------
void handleFavicon(AsyncWebServerRequest *request) {
  request->send(204);
}

void handleRoot(AsyncWebServerRequest *request) {
  String page = loadFile("/index.html");
  if (page == "") {
    request->send(500, "text/plain", "Error loading page");
    return;
  }
  page = processTemplate(page);
  request->send(200, "text/html", page);
}

void handleAbout(AsyncWebServerRequest *request) {
  String page = loadFile("/about.html");
  if (page == "") {
    request->send(500, "text/plain", "Error loading page");
    return;
  }
  page = processTemplate(page);
  request->send(200, "text/html", page);
}

void handleConfig(AsyncWebServerRequest *request) {
  String page = loadFile("/config.html");
  if (page == "") {
    request->send(500, "text/plain", "Error loading config page");
    return;
  }
  page = processTemplate(page);
  request->send(200, "text/html", page);
}

void handleBroadcasts(AsyncWebServerRequest *request) {
  String page = loadFile("/broadcasts.html");
  if (page == "") {
    request->send(500, "text/plain", "Error loading Broadcasts page");
    return;
  }
  page = processTemplate(page);
  String listItems;
  for (size_t i = 0; i < broadcastMessages.size(); i++)
    listItems += "<li>" + broadcastMessages[i] + "</li>";
  page.replace("%BROADCASTS_LIST%", listItems);
  request->send(200, "text/html", page);
}

void handleSaveConfig(AsyncWebServerRequest *request) {
  if (request->hasArg("tcpPort"))
    wsPortStr = request->arg("tcpPort");
  if (request->hasArg("rotorPort"))
    rotorPortStr = request->arg("rotorPort");
  if (request->hasArg("gridSQ"))
    gridSQ = request->arg("gridSQ");
  Preferences configPrefs;
  configPrefs.begin("config", false);
  configPrefs.putString("tcp_port", wsPortStr);
  configPrefs.putString("rotor_port", rotorPortStr);
  configPrefs.putString("grid_sq", gridSQ);
  configPrefs.end();
  request->send(200, "text/html", "<html><body><h1>Configuration Saved</h1><p>The device will now reboot.</p></body></html>");
  delay(2000);
  ESP.restart();
}

void handleRestoreConfig(AsyncWebServerRequest *request) {
  Preferences configPrefs;
  configPrefs.begin("config", false);
  configPrefs.clear();
  configPrefs.end();
  WiFi.disconnect(true, true);
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  request->send(200, "text/html", "<html><body><h1>Defaults Restored</h1><p>The device will now reboot.</p></body></html>");
  Preferences tempPrefs;
  tempPrefs.begin("wifi", false);
  tempPrefs.clear(); // Erase all keys under "wifi"
  tempPrefs.end();
  Serial.println("[BLE] Stored WiFi credentials cleared.");
  delay(2000);
  ESP.restart();
}

// --------------------
// UDP/WebSocket Task (Pinned to Core 1)
// --------------------
void udpWebSocketTask(void *parameter) {
  for (;;) {
    // Process regular UDP packets
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char packetBuffer[packetSize + 1];
      int len = udp.read(packetBuffer, packetSize);
      if (len > 0)
        packetBuffer[len] = '\0';
      String incomingMessage = String(packetBuffer);
      if (incomingMessage.indexOf("\"timestamp\":\"") == -1) {
        broadcastMessages.push_back(incomingMessage);
        if (broadcastMessages.size() > 10)
          broadcastMessages.erase(broadcastMessages.begin());
      }
    }
    // Process MacLogger UDP packets to update satName and channelName
    int macPacketSize = macLoggerUdp.parsePacket();
    if (macPacketSize > 0) {
      char macBuffer[macPacketSize + 1];
      int len = macLoggerUdp.read(macBuffer, macPacketSize);
      if (len > 0)
        macBuffer[len] = '\0';
      String macMsg = String(macBuffer);
      if (macMsg.indexOf("[New Satellite Status Report:") != -1) {
        int satNamePos = macMsg.indexOf("SatName:");
        int channelPos = macMsg.indexOf(" channelName:");
        if (satNamePos != -1 && channelPos != -1) {
          int satNameStart = satNamePos + strlen("SatName:");
          String satNameValue = macMsg.substring(satNameStart, channelPos);
          satNameValue.trim();
          int channelNameStart = channelPos + strlen(" channelName:");
          int endBracket = macMsg.indexOf("]", channelNameStart);
          String channelNameValue;
          if (endBracket != -1)
            channelNameValue = macMsg.substring(channelNameStart, endBracket);
          else
            channelNameValue = macMsg.substring(channelNameStart);
          channelNameValue.trim();
          if (satNameValue.length() > 0)
            satName = satNameValue;
          if (channelNameValue.length() > 0)
            channelName = channelNameValue;
          DynamicJsonDocument wsDoc(128);
          wsDoc["type"] = "macDoppler";
          wsDoc["satName"] = satName;
          wsDoc["channelName"] = channelName;
          String wsMsg;
          serializeJson(wsDoc, wsMsg);
          ws.textAll(wsMsg);
          goto UDP_END;
        }
      }
      if (macMsg.indexOf("MacDoppler set_pos") != -1) {
        int azIndex = macMsg.indexOf("AZ=");
        int elIndex = macMsg.indexOf("EL=");
        if (azIndex != -1 && elIndex != -1) {
          int azEnd = macMsg.indexOf(",", azIndex);
          if (azEnd == -1)
            azEnd = macMsg.length();
          String azStr = macMsg.substring(azIndex + 3, azEnd);
          int elEnd = macMsg.indexOf("]", elIndex);
          if (elEnd == -1)
            elEnd = macMsg.length();
          String elStr = macMsg.substring(elIndex + 3, elEnd);
          azStr.trim();
          elStr.trim();
          int azVal = azStr.toInt();
          int elVal = elStr.toInt();
          DynamicJsonDocument wsDoc(128);
          wsDoc["type"] = "macDoppler";
          wsDoc["set_AZ"] = azVal;
          wsDoc["set_EL"] = elVal;
          String wsMsg;
          serializeJson(wsDoc, wsMsg);
          ws.textAll(wsMsg);
          goto UDP_END;
        }
      }
    }
  UDP_END:
    unsigned long currentMillis = millis();
    if (currentMillis - lastUdpBroadcast >= broadcastInterval) {
      lastUdpBroadcast = currentMillis;
      struct tm timeinfo;
      char timeStr[64];
      if (getLocalTime(&timeinfo))
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
      else
        strcpy(timeStr, "TIME_NOT_SET");
      unsigned long uptimeSec = millis() / 1000;
      unsigned long days = uptimeSec / 86400;
      String uptimeStr = String(days) + " days";
      uint64_t chipid = ESP.getEfuseMac();
      String chipIdStr = String((uint32_t)(chipid >> 32), HEX) +
                         String((uint32_t)chipid, HEX);
      DynamicJsonDocument doc(256);
      doc["TimeStamp"] = timeStr;
      doc["Device"] = chipIdStr;
      doc["Name"] = "shackmate-rotor";
      doc["Address"] = deviceIP;
      doc["Port"] = wsPortStr;
      doc["UpTime"] = uptimeStr;
      String udpMessage;
      serializeJson(doc, udpMessage);
      udp.beginPacket("255.255.255.255", UDP_PORT);
      udp.print(udpMessage);
      udp.endPacket();
      broadcastMessages.push_back(udpMessage);
      if (broadcastMessages.size() > 10)
        broadcastMessages.erase(broadcastMessages.begin());
      digitalWrite(LED_GREEN, HIGH);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      digitalWrite(LED_GREEN, LOW);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// --------------------
// EasyComm Server
// --------------------
WiFiServer easyCommServer(rotorPortStr.toInt());

void startWiFiManager() {
  WiFi.mode(WIFI_AP_STA);
  WiFiManager wifiManager;
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println("Entered configuration mode (AP mode)");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  });

  Preferences configPrefs;
  configPrefs.begin("config", false);
  String storedWsPort = configPrefs.getString("tcp_port", "4000");
  String storedRotorPort = configPrefs.getString("rotor_port", "4532");
  gridSQ = configPrefs.getString("grid_sq", "");
  configPrefs.end();
  wsPortStr = storedWsPort;
  rotorPortStr = storedRotorPort;

  WiFiManagerParameter customWsPortParam("tcpPort", "WebSocket Port", storedWsPort.c_str(), 6);
  WiFiManagerParameter customRotorPortParam("rotorPort", "Rotor Port", storedRotorPort.c_str(), 6);
  WiFiManagerParameter customGridParam("gridSQ", "Grid SQ", gridSQ.c_str(), 8, "placeholder=\"FM08TO\"");
  String customHTML = "<div style='text-align:left;'>"
                      "<p><strong>Additional Configuration</strong></p>"
                      "<p>WebSocket Port: <input type='text' name='tcpPort' value='%PORT%'></p>"
                      "<p>Rotor Port: <input type='text' name='rotorPort' value='%ROTOR_PORT%'></p>"
                      "<p>Grid SQ: <input type='text' name='gridSQ' value='%GRID_SQ%' placeholder='FM08TO'></p>"
                      "</div>";
  wifiManager.setCustomMenuHTML(customHTML.c_str());
  wifiManager.addParameter(&customWsPortParam);
  wifiManager.addParameter(&customRotorPortParam);
  wifiManager.addParameter(&customGridParam);

  if (!wifiManager.autoConnect("shackmate-rotor")) {
    Serial.println("Failed to connect to WiFi, restarting...");
    delay(3000);
    ESP.restart();
    delay(5000);
  }
  deviceIP = WiFi.localIP().toString();
  Serial.println("Connected, IP address: " + deviceIP);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(LED_GREEN, OUTPUT);
  if (!LittleFS.begin())
    Serial.println("LittleFS mount failed");
  else
    Serial.println("LittleFS mounted successfully");

  // Open the global Preferences instance in the "wifi" namespace
  wifiPrefs.begin("wifi", false);

  // Start BLE Provisioning Service (this uses wifiPrefs for storing credentials)
  startBLEProvisioning();

  // Try BLE-Provisioned Credentials First
  String ssid = wifiPrefs.getString("ssid", "");
  String pass = wifiPrefs.getString("password", "");
  if (ssid.length() > 0 && pass.length() > 0) {
    Serial.println("[BLE] WiFi credentials found. Connecting...");
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long startAttemptTime = millis();
    // Try to connect for 10 seconds
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(100);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("");
      Serial.println("[BLE] Connected via Provisioned WiFi.");
      deviceIP = WiFi.localIP().toString();
      Serial.println("Connected, IP address: " + deviceIP);
      // Stop BLE advertising and disable further BLE connections now that WiFi is up.
      stopBLEAdvertising();
    } else {
      Serial.println("\n[BLE] Failed to connect. Starting WiFiManager AP.");
      startWiFiManager();
    }
  } else {
    Serial.println("[BLE] No BLE provisioned credentials found. Starting WiFiManager AP.");
    startWiFiManager();
  }

  // ---- Remaining System Initialization ----
  Preferences rotorPrefs;
  rotorPrefs.begin("rotor", false);
  targetAZ = rotorPrefs.getInt("rotorAZPosition", 0);
  targetEL = rotorPrefs.getInt("rotorELPosition", 0);
  rotorPrefs.end();
  currentAz = targetAZ;
  currentEl = targetEL;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    Serial.println("Failed to obtain time");
  else
    Serial.println("Time synchronized");

  if (!MDNS.begin(MDNS_NAME))
    Serial.println("Error setting up mDNS responder!");
  else {
    Serial.print("mDNS responder started: http://");
    Serial.print(MDNS_NAME);
    Serial.println(".local");
  }

  udp.begin(UDP_PORT);
  macLoggerUdp.begin(MAC_UDP_PORT);
  Serial.printf("UDP listener started on port %d\n", UDP_PORT);
  Serial.printf("MacLogger UDP listener started on port %d\n", MAC_UDP_PORT);

  // HTTP Server Setup
  httpServer.on("/favicon.ico", HTTP_GET, handleFavicon);
  httpServer.on("/saveMemory", HTTP_GET, handleSaveMemory);
  httpServer.on("/getMemory", HTTP_GET, handleGetMemory);
  httpServer.on("/calcGrid", HTTP_GET, handleCalcGrid);
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/about", HTTP_GET, handleAbout);
  httpServer.on("/config", HTTP_GET, handleConfig);
  httpServer.on("/saveConfig", HTTP_POST, handleSaveConfig);
  httpServer.on("/restoreConfig", HTTP_POST, handleRestoreConfig);
  httpServer.on("/broadcasts", HTTP_GET, handleBroadcasts);
  httpServer.on("/rotor", HTTP_GET, [](AsyncWebServerRequest *request) {
    String page = loadFile("/rotor.html");
    if (page == "") {
      request->send(500, "text/plain", "Error loading rotor page");
      return;
    }
    page = processTemplate(page);
    request->send(200, "text/html", page);
  });
  httpServer.begin();
  Serial.println("HTTP server started on port 80");

  // WebSocket Server Setup
  ws.onEvent(onWsEvent);
  int wsPort = wsPortStr.toInt();
  if (wsPort <= 0)
    wsPort = 4000;
  wsServer = new AsyncWebServer(wsPort);
  wsServer->addHandler(&ws);
  wsServer->begin();
  Serial.printf("WebSocket server started on port %d\n", wsPort);

  // Serve static files from LittleFS
  httpServer.serveStatic("/rotor.js", LittleFS, "/rotor.js");
  httpServer.serveStatic("/rotor.css", LittleFS, "/rotor.css");

  // EasyComm Server
  easyCommServer = WiFiServer(rotorPortStr.toInt());
  easyCommServer.begin();
  Serial.printf("TCP EasyComm Server started on port %s\n", rotorPortStr.c_str());

  // OTA Setup
  ArduinoOTA.onStart([]() { Serial.println("OTA update starting..."); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA update complete"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA update service started");

  // Launch background UDP/WebSocket task on Core 1
  xTaskCreatePinnedToCore(udpWebSocketTask, "udpWS_Task", 8192, NULL, 1, NULL, 1);
}

void loop() {
  ArduinoOTA.handle();
  WiFiClient easyClient = easyCommServer.available();
  if (easyClient) {
    if (!autoTrack) {
      autoTrack = true;
      DynamicJsonDocument doc(64);
      doc["type"] = "autoTrack";
      doc["auto"] = true;
      String wsMsg;
      serializeJson(doc, wsMsg);
      ws.textAll(wsMsg);
    }
    tracking = true;
    while (easyClient.connected()) {
      if (easyClient.available()) {
        String cmd = easyClient.readStringUntil('\n');
        cmd.trim();
        if (cmd.length() > 0) {
          if (cmd.startsWith("+\\set_pos ")) {
            String args = cmd.substring(9);
            args.trim();
            int spacePos = args.indexOf(' ');
            if (spacePos != -1) {
              String azStr = args.substring(0, spacePos);
              String elStr = args.substring(spacePos + 1);
              azStr.trim();
              elStr.trim();
              int azVal = azStr.toInt();
              int elVal = elStr.toInt();
              currentAz = azVal;
              currentEl = elVal;
              easyClient.println("OK setpos: AZ=" + String(currentAz) + " EL=" + String(currentEl));
              DynamicJsonDocument wsDoc(128);
              wsDoc["type"] = "macDoppler";
              wsDoc["set_AZ"] = currentAz;
              wsDoc["set_EL"] = currentEl;
              String wsMsg;
              serializeJson(wsDoc, wsMsg);
              ws.textAll(wsMsg);
              // In AUTOMATIC mode, update target positions and broadcast state.
              targetAZ = currentAz;
              targetEL = currentEl;
              broadcastPosition(targetAZ, targetEL);
              broadcastStateUpdate(true);
            }
          } else if (cmd.equals("+\\get_pos")) {
            easyClient.println("AZ=" + String(currentAz) + " EL=" + String(currentEl));
          } else {
            int azIndex = cmd.indexOf("AZ=");
            int elIndex = cmd.indexOf("EL=");
            if (azIndex != -1 && elIndex != -1) {
              int endAz = cmd.indexOf(' ', azIndex);
              if (endAz == -1)
                endAz = cmd.length();
              String azS = cmd.substring(azIndex + 3, endAz);
              int endEl = cmd.indexOf(' ', elIndex);
              if (endEl == -1)
                endEl = cmd.length();
              String elS = cmd.substring(elIndex + 3, endEl);
              int azVal = azS.toInt();
              int elVal = elS.toInt();
              currentAz = azVal;
              currentEl = elVal;
              easyClient.println("OK AZ=" + String(currentAz) + " EL=" + String(currentEl));
              DynamicJsonDocument satDoc(128);
              satDoc["type"] = "satellite";
              satDoc["msg"] = "Satellite update => AZ=" + String(currentAz) + ", EL=" + String(currentEl);
              String satMsg;
              serializeJson(satDoc, satMsg);
              ws.textAll(satMsg);
            } else {
              easyClient.println("Invalid EasyComm command");
            }
          }
        }
      }
      delay(10);
    }
    autoTrack = false;
    {
      DynamicJsonDocument doc(64);
      doc["type"] = "autoTrack";
      doc["auto"] = false;
      String wsMsg;
      serializeJson(doc, wsMsg);
      ws.textAll(wsMsg);
    }
    tracking = false;
    easyClient.stop();
  }
  delay(1);
}
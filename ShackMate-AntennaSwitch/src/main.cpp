/*
 * ShackMate Antenna Switch (RCS-8/RCS-10) Firmware
 *
 * All Antenna Switch Settings are sent as JSON messages over WebSocket
 * when connecting to the ESP IP:Port 4000.
 *
 * Example: Change antenna to port 3:
 * ws.send(JSON.stringify({
 *   "type": "antennaChange",
 *   "currentAntennaIndex": 2
 * }));
 *
 * Example: Update antenna names:
 * ws.send(JSON.stringify({
 *   "type": "stateUpdate",
 *   "antennaNames": ["20m Beam", "40m Dipole", "80m Loop", "Vertical", "Spare", "Spare", "Spare", "Spare"]
 * }));
 */

// --- Libraries and Dependencies ---
#include "SMCIV.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <vector>
#include <WebSocketsClient.h>
#include <Adafruit_NeoPixel.h>

// --- Global Objects ---
SMCIV smciv;
WebSocketsClient wsClient;

// --- Project Configuration ---
#define NAME "ShackMate - Switch (RCS-8/10)"
#define VERSION "2.0"
#define MY_MDNS_NAME "shackmate-switch"
#define MY_UDP_PORT 4210
#define BUTTON_PIN 41 // AtomS3 Lite physical button
#define WS_PORT 4000  // WebSocket server port (cannot change)

// --- Antenna Control GPIO Pins ---
#define ANTENNA_GPIO_1 5  // G5 - Antenna 1 (RCS-8) / BCD Bit A (RCS-10)
#define ANTENNA_GPIO_2 6  // G6 - Antenna 2 (RCS-8) / BCD Bit B (RCS-10)
#define ANTENNA_GPIO_3 7  // G7 - Antenna 3 (RCS-8) / BCD Bit C (RCS-10)
#define ANTENNA_GPIO_4 8  // G8 - Antenna 4 (RCS-8) / Unused (RCS-10)
#define ANTENNA_GPIO_5 39 // G39 - Antenna 5 (RCS-8) / Unused (RCS-10)

// --- Atom S3 RGB LED Configuration ---
#define ATOM_LED_PIN 35
#define ATOM_NUM_LEDS 1
;
Adafruit_NeoPixel atom_led(ATOM_NUM_LEDS, ATOM_LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Function Prototypes ---
void setAtomLed(uint8_t r, uint8_t g, uint8_t b);
String loadFile(const char *path);
String processTemplate(String t);
void onAntennaStateChanged(uint8_t antennaPort, uint8_t rcsType);
void saveAntennaDetails(int antennaIndex, int typeIndex, int styleIndex, int polIndex, int mfgIndex, int bandPattern, bool disabled);
void loadAntennaDetails(int antennaIndex, int *typeIndex, int *styleIndex, int *polIndex, int *mfgIndex, int *bandPattern, bool *disabled);
void saveAllAntennaDetails(JsonArray antennaStateArray);
void loadAllAntennaDetails(JsonArray antennaStateArray);
void setupButtonOutputs();
void setAntennaOutput(uint8_t antennaIndex);
void clearAllAntennaOutputs();

// --- Global State Variables ---
bool captivePortalActive = false;
bool otaActive = false;
bool wsConnected = false;
bool updatingFromWebSocket = false; // Flag to prevent infinite loops during WebSocket updates

// --- Device IP (global, used in processTemplate, setup, loop, etc.) ---
String deviceIP = "";

// --- Dynamic WebSocket server address (from UDP discovery) ---
String discoveredWsServer = ""; // e.g., "192.168.1.100:4000"
String discoveredWsIp = "";     // Only the IP part
uint16_t discoveredWsPort = 0;  // Only the port part

// --- Configuration Variables ---
int deviceNumber = 1;
int rcsType = 0;        // Default to RCS-8 (0)
uint8_t civAddr = 0xB4; // Default, will be set from deviceNumber

// --- Global Preferences Objects ---
Preferences configPrefs;  // "config" namespace
Preferences wifiPrefs;    // "wifi" namespace
Preferences antennaPrefs; // "antennaNames" namespace
Preferences statePrefs;   // "antenna" namespace
Preferences detailsPrefs; // "antennaDetails" namespace

// --- Network Objects ---
AsyncWebServer httpServer(80);
AsyncWebServer *wsServer = nullptr;
AsyncWebSocket ws("/ws");
WiFiUDP udp;
WiFiUDP udpDiscovery;

// --- Dashboard Status Broadcast Helper ---
void broadcastDashboardStatus()
{
  DynamicJsonDocument doc(256);
  doc["type"] = "dashboardStatus";
  doc["wsServer"] = discoveredWsServer.length() > 0 ? discoveredWsServer : String("Unknown");
  doc["wsStatus"] = wsClient.isConnected() ? "Connected" : "Disconnected";
  // Optionally add CI-V address if needed
  char civAddrStr[8];
  snprintf(civAddrStr, sizeof(civAddrStr), "0x%02X", civAddr);
  doc["civAddress"] = String(civAddrStr);
  String msg;
  serializeJson(doc, msg);
  ws.textAll(msg);
  Serial.printf("[WS] Broadcasted dashboardStatus: %s\n", msg.c_str());
}

// --- Default CI-V Baud Rate ---
#define CIV_BAUD_DEFAULT 19200
int civBaud = CIV_BAUD_DEFAULT;

// -------------------------------------------------------------------------
// Utility Functions
// -------------------------------------------------------------------------

// --- Function to set Atom S3 LED Color ---
void setAtomLed(uint8_t r, uint8_t g, uint8_t b)
{
  atom_led.setPixelColor(0, atom_led.Color(r, g, b));
  atom_led.show();
}

// --- Reload CI-V Address based on device number ---
void reloadCivAddress()
{
  configPrefs.begin("config", false);
  int deviceNumber = configPrefs.getInt("deviceNumber", 1);
  configPrefs.end();
  civAddr = 0xB3 + deviceNumber;
  smciv.begin(&wsClient, &civAddr);
  smciv.setAntennaStateCallback(onAntennaStateChanged);
  smciv.setGpioOutputCallback(setAntennaOutput); // Register GPIO callback

  Serial.printf("[CI-V] This device CI-V address: 0x%02X (Device #%d)\n", civAddr, deviceNumber);
}

// -------------------------------------------------------------------------
// CI-V Callback Functions
// -------------------------------------------------------------------------

// --- Callback function for antenna state changes from CI-V ---
void onAntennaStateChanged(uint8_t antennaPort, uint8_t receivedRcsType)
{
  // Prevent infinite loops when updating from WebSocket
  if (updatingFromWebSocket)
  {
    Serial.printf("[CALLBACK] Skipping callback during WebSocket update (port=%u, rcsType=%u)\n", antennaPort, receivedRcsType);
    return;
  }

  Serial.printf("[CALLBACK] Antenna state changed via CI-V: port=%u, rcsType=%u\n", antennaPort, receivedRcsType);

  // Update global rcsType if it changed
  if (receivedRcsType != rcsType)
  {
    rcsType = receivedRcsType;
    Serial.printf("[CALLBACK] Updated global rcsType to %u\n", rcsType);
  }

  // Create JSON message for web UI update
  DynamicJsonDocument doc(1024);
  doc["type"] = "stateUpdate";
  doc["currentAntennaIndex"] = antennaPort; // antennaPort is already zero-based
  doc["rcsType"] = rcsType;                 // Include rcsType in the update
  doc["source"] = "ci-v";                   // indicate this came from CI-V

  // Include the full antenna state array with stored details
  JsonArray antennaStateArray = doc.createNestedArray("antennaState");
  loadAllAntennaDetails(antennaStateArray);

  String jsonStr;
  serializeJson(doc, jsonStr);

  // Broadcast to all connected WebSocket clients
  ws.textAll(jsonStr);
  Serial.printf("[WS] Broadcasted CI-V state change to web clients: %s\n", jsonStr.c_str());

  // Save to the SAME preferences namespace/key that the main app uses
  Preferences switchPrefs;
  switchPrefs.begin("switch", false);
  switchPrefs.putInt("selectedIndex", antennaPort);
  switchPrefs.end();
  Serial.printf("[CALLBACK] Saved antenna index %u to switch/selectedIndex\n", antennaPort);

  // Update physical GPIO outputs
  setAntennaOutput(antennaPort);
}

// -------------------------------------------------------------------------
// Stub Functions (placeholder implementations)
// -------------------------------------------------------------------------

void setupButtonOutputs()
{
  // Initialize antenna control GPIO pins as outputs
  pinMode(ANTENNA_GPIO_1, OUTPUT);
  pinMode(ANTENNA_GPIO_2, OUTPUT);
  pinMode(ANTENNA_GPIO_3, OUTPUT);
  pinMode(ANTENNA_GPIO_4, OUTPUT);
  pinMode(ANTENNA_GPIO_5, OUTPUT);

  // Initialize all outputs to LOW (no antenna selected)
  clearAllAntennaOutputs();

  Serial.println("[GPIO] Antenna control outputs initialized");
  Serial.printf("[GPIO] Pin assignments - G%d: Ant1/BCD_A, G%d: Ant2/BCD_B, G%d: Ant3/BCD_C, G%d: Ant4, G%d: Ant5\n",
                ANTENNA_GPIO_1, ANTENNA_GPIO_2, ANTENNA_GPIO_3, ANTENNA_GPIO_4, ANTENNA_GPIO_5);
}

void clearAllAntennaOutputs()
{
  // Set all antenna control pins to LOW
  digitalWrite(ANTENNA_GPIO_1, LOW);
  digitalWrite(ANTENNA_GPIO_2, LOW);
  digitalWrite(ANTENNA_GPIO_3, LOW);
  digitalWrite(ANTENNA_GPIO_4, LOW);
  digitalWrite(ANTENNA_GPIO_5, LOW);

  Serial.println("[GPIO] All antenna outputs cleared");
}

void setAntennaOutput(uint8_t antennaIndex)
{
  // antennaIndex is zero-based (0-7 for antennas 1-8)

  // First clear all outputs
  clearAllAntennaOutputs();

  // Get current RCS type from global variable
  if (rcsType == 0) // RCS-8 Mode: Direct GPIO control
  {
    // RCS-8: antennaIndex 0-4 maps to antennas 1-5
    // Each antenna gets its own dedicated GPIO pin

    switch (antennaIndex)
    {
    case 0: // Antenna 1
      digitalWrite(ANTENNA_GPIO_1, HIGH);
      Serial.printf("[GPIO] RCS-8: Antenna 1 selected (G%d HIGH)\n", ANTENNA_GPIO_1);
      break;
    case 1: // Antenna 2
      digitalWrite(ANTENNA_GPIO_2, HIGH);
      Serial.printf("[GPIO] RCS-8: Antenna 2 selected (G%d HIGH)\n", ANTENNA_GPIO_2);
      break;
    case 2: // Antenna 3
      digitalWrite(ANTENNA_GPIO_3, HIGH);
      Serial.printf("[GPIO] RCS-8: Antenna 3 selected (G%d HIGH)\n", ANTENNA_GPIO_3);
      break;
    case 3: // Antenna 4
      digitalWrite(ANTENNA_GPIO_4, HIGH);
      Serial.printf("[GPIO] RCS-8: Antenna 4 selected (G%d HIGH)\n", ANTENNA_GPIO_4);
      break;
    case 4: // Antenna 5
      digitalWrite(ANTENNA_GPIO_5, HIGH);
      Serial.printf("[GPIO] RCS-8: Antenna 5 selected (G%d HIGH)\n", ANTENNA_GPIO_5);
      break;
    default:
      Serial.printf("[GPIO] RCS-8: Invalid antenna index %d (valid: 0-4)\n", antennaIndex);
      break;
    }
  }
  else if (rcsType == 1) // RCS-10 Mode: BCD control
  {
    // RCS-10: antennaIndex 0-7 maps to antennas 1-8
    // Use 3-bit BCD encoding on G5, G6, G7 per logic table:
    // A(1) = G5 controls antennas 2,4,6,8 (160mA)
    // B(2) = G6 controls antennas 3,4,7,8 (80mA)
    // C(3) = G7 controls antennas 5,6,7,8 (40mA)

    uint8_t antennaNumber = antennaIndex + 1; // Convert to 1-based antenna number

    if (antennaNumber >= 1 && antennaNumber <= 8)
    {
      // Logic table implementation:
      // Ant1: A=0,B=0,C=0  Ant2: A=1,B=0,C=0  Ant3: A=0,B=1,C=0  Ant4: A=1,B=1,C=0
      // Ant5: A=0,B=0,C=1  Ant6: A=1,B=0,C=1  Ant7: A=0,B=1,C=1  Ant8: A=1,B=1,C=1

      bool bitA = (antennaNumber == 2 || antennaNumber == 4 || antennaNumber == 6 || antennaNumber == 8);
      bool bitB = (antennaNumber == 3 || antennaNumber == 4 || antennaNumber == 7 || antennaNumber == 8);
      bool bitC = (antennaNumber == 5 || antennaNumber == 6 || antennaNumber == 7 || antennaNumber == 8);

      digitalWrite(ANTENNA_GPIO_1, bitA ? HIGH : LOW); // A(1) - G5
      digitalWrite(ANTENNA_GPIO_2, bitB ? HIGH : LOW); // B(2) - G6
      digitalWrite(ANTENNA_GPIO_3, bitC ? HIGH : LOW); // C(3) - G7

      Serial.printf("[GPIO] RCS-10: Antenna %d selected - Logic A=%d,B=%d,C=%d (G%d=%s, G%d=%s, G%d=%s)\n",
                    antennaNumber,
                    bitA ? 1 : 0, bitB ? 1 : 0, bitC ? 1 : 0,
                    ANTENNA_GPIO_1, bitA ? "HIGH" : "LOW",
                    ANTENNA_GPIO_2, bitB ? "HIGH" : "LOW",
                    ANTENNA_GPIO_3, bitC ? "HIGH" : "LOW");
    }
    else
    {
      Serial.printf("[GPIO] RCS-10: Invalid antenna index %d (valid: 0-7)\n", antennaIndex);
    }
  }
  else
  {
    Serial.printf("[GPIO] Error: Unknown RCS type %d\n", rcsType);
  }
}

void loadLatchedStates()
{
  // Load latched states if used.
}

// -------------------------------------------------------------------------
// loadFile: Read a file from LittleFS
// -------------------------------------------------------------------------
String loadFile(const char *path)
{
  File f = LittleFS.open(path, "r");
  if (!f || f.isDirectory())
  {
    Serial.printf("Failed to open %s\n", path);
    return "";
  }
  String s;
  while (f.available())
    s += char(f.read());
  f.close();
  return s;
}

// -------------------------------------------------------------------------
// processTemplate: Replace placeholders in HTML templates.
// -------------------------------------------------------------------------
String processTemplate(String t)
{
  t.replace("%PROJECT_NAME%", NAME);
  t.replace("%VERSION%", VERSION);

  struct tm tm;
  char buf[64];
  if (getLocalTime(&tm))
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  else
    strcpy(buf, "TIME_NOT_SET");
  t.replace("%TIME%", buf);

  t.replace("%IP%", deviceIP);
  // Remove hardcoded port, use discoveredWsServer if set, else fallback to deviceIP:WS_PORT
  String wsServerStr = discoveredWsServer.length() > 0 ? discoveredWsServer : (deviceIP + ":" + String(WS_PORT));
  t.replace("%WS_SERVER%", wsServerStr);
  t.replace("%WEBSOCKET_PORT%", String(WS_PORT));
  t.replace("%UDP_PORT%", String(MY_UDP_PORT));

  unsigned long secs = millis() / 1000;
  unsigned long days = secs / 86400;
  unsigned long hours = (secs % 86400) / 3600;
  unsigned long mins = (secs % 3600) / 60;
  unsigned long secs2 = secs % 60;
  snprintf(buf, sizeof(buf), "%lu days %02lu:%02lu:%02lu", days, hours, mins, secs2);
  t.replace("%UPTIME%", buf);

  uint64_t chipid = ESP.getEfuseMac();
  snprintf(buf, sizeof(buf), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  t.replace("%CHIP_ID%", buf);
  t.replace("%CHIP_REV%", String(ESP.getChipRevision()));
  t.replace("%FLASH_TOTAL%", String(ESP.getFlashChipSize() / 1024) + " KB");
  t.replace("%PSRAM_SIZE%", String(ESP.getPsramSize() / 1024) + " KB");
  t.replace("%CPU_FREQ%", String(ESP.getCpuFreqMHz()));
  t.replace("%FREE_HEAP%", String(ESP.getFreeHeap()));

  uint32_t totalMem = ESP.getHeapSize();
  uint32_t freeMem = ESP.getFreeHeap();
  uint32_t usedMem = totalMem > freeMem ? totalMem - freeMem : 0;
  t.replace("%MEM_TOTAL%", String(totalMem / 1024) + " KB");
  t.replace("%MEM_USED%", String(usedMem / 1024) + " KB");

  t.replace("%SKETCH_USED%", String(ESP.getSketchSize() / 1024) + " KB");
  t.replace("%SKETCH_TOTAL%", String(ESP.getFlashChipSize() / 1024) + " KB");

  float tempC = 25.0;
  float tempF = tempC * 9.0f / 5.0f + 32.0f;
  dtostrf(tempC, 4, 2, buf);
  t.replace("%TEMPERATURE_C%", buf);
  dtostrf(tempF, 4, 2, buf);
  t.replace("%TEMPERATURE_F%", buf);

  antennaPrefs.begin("antennaNames", false);
  for (int i = 1; i <= 8; i++)
  {
    String key = "ant" + String(i);
    String ph = "%ANT" + String(i) + "%";
    String name = antennaPrefs.getString(key.c_str(), "Antenna #" + String(i));
    t.replace(ph, name);
  }
  antennaPrefs.end();

  // --- Populate Antenna Switch Model & Device Number placeholders ---
  configPrefs.begin("config", false);
  int rcsType = configPrefs.getInt("rcs_type", 1); // 0 = RCS-8, 1 = RCS-10
  int devNum = configPrefs.getInt("deviceNumber", 1);
  configPrefs.end();

  t.replace("%MODEL8_CHECKED%", rcsType == 0 ? "checked" : "");
  t.replace("%MODEL10_CHECKED%", rcsType == 1 ? "checked" : "");
  t.replace("%DEVICE_NUMBER%", String(devNum));
  t.replace("%RCS_TYPE%", String(rcsType));
  t.replace("%CIV_BAUD%", String(civBaud));

  char civAddrStr[8];
  snprintf(civAddrStr, sizeof(civAddrStr), "0x%02X", civAddr);
  t.replace("%CIV_ADDRESS%", String(civAddrStr));

  return t;
}

// -------------------------------------------------------------------------
// WebSocket Event Handling
// -------------------------------------------------------------------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
  {
    Serial.printf("[WS] Client connected, sending current state\n");

    // Always create a fresh state document with current values
    DynamicJsonDocument doc(2048);
    doc["type"] = "stateUpdate";

    // Create antenna state array (load from stored state if available)
    statePrefs.begin("antenna", false);
    String storedState = statePrefs.getString("state", "");
    statePrefs.end();

    // Load antenna state from NVS storage (new method)
    JsonArray antennaStateArray = doc.createNestedArray("antennaState");
    loadAllAntennaDetails(antennaStateArray);

    // Get current antenna selection from NVS (this is the authoritative source)
    Preferences switchPrefs;
    switchPrefs.begin("switch", false);
    int currentAntennaIndex = switchPrefs.getInt("selectedIndex", 0);
    switchPrefs.end();
    doc["currentAntennaIndex"] = currentAntennaIndex;

    // Add antenna names
    auto names = doc.createNestedArray("antennaNames");
    antennaPrefs.begin("antennaNames", false);
    for (int i = 1; i <= 8; i++)
      names.add(antennaPrefs.getString(("ant" + String(i)).c_str(), "Antenna #" + String(i)));
    antennaPrefs.end();

    // Add current configuration
    configPrefs.begin("config", false);
    doc["rcsType"] = configPrefs.getInt("rcs_type", 0);
    doc["deviceNumber"] = configPrefs.getInt("deviceNumber", 1);
    configPrefs.end();

    // Send the state to the newly connected client
    String stateStr;
    serializeJson(doc, stateStr);
    client->text(stateStr);

    Serial.printf("[WS] Sent current state to new client: currentAntennaIndex=%d, rcsType=%d\n",
                  currentAntennaIndex, doc["rcsType"].as<int>());
    // --- Send dashboardStatus to new client ---
    broadcastDashboardStatus();
    break;
  }
  case WS_EVT_DATA:
  {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len)
    {
      String msg;
      for (size_t i = 0; i < len; i++)
        msg += char(data[i]);

      DynamicJsonDocument doc(4096); // adjust size as needed
      DeserializationError err = deserializeJson(doc, msg);

      if (!err && doc.containsKey("type") && strcmp(doc["type"], "stateUpdate") == 0)
      {
        // Save antennaState using new NVS-based system
        if (doc.containsKey("antennaState"))
        {
          JsonArray antennaStateArray = doc["antennaState"];
          saveAllAntennaDetails(antennaStateArray);

          // Also keep the old format for backward compatibility if needed
          statePrefs.begin("antenna", false);
          statePrefs.putString("state", msg);
          statePrefs.end();
        }

        // Save antennaNames to preferences
        if (doc.containsKey("antennaNames"))
        {
          antennaPrefs.begin("antennaNames", false);
          JsonArray names = doc["antennaNames"].as<JsonArray>();
          for (int i = 0; i < 8; i++)
          {
            String key = "ant" + String(i + 1);
            if (names.size() > i)
              antennaPrefs.putString(key.c_str(), names[i].as<const char *>());
          }
          antennaPrefs.end();
        }

        // Save currentAntennaIndex
        if (doc.containsKey("currentAntennaIndex"))
        {
          int selectedIndex = doc["currentAntennaIndex"];

          // Validate antenna index range based on RCS type
          int maxIndex = (rcsType == 0) ? 4 : 7; // RCS-8: 0-4, RCS-10: 0-7
          if (selectedIndex < 0 || selectedIndex > maxIndex)
          {
            Serial.printf("[ERROR] Invalid antenna index %d for RCS type %d (max: %d)\n", selectedIndex, rcsType, maxIndex);
          }
          else
          {
            // Set flag to prevent callback loop
            updatingFromWebSocket = true;

            // Update SMCIV with the new antenna selection
            smciv.setSelectedAntennaPort(selectedIndex);
            Serial.printf("[DEBUG] stateUpdate: updating selected antenna port to: %d\n", selectedIndex);

            Preferences switchPrefs;
            switchPrefs.begin("switch", false);
            switchPrefs.putInt("selectedIndex", selectedIndex);
            switchPrefs.end();

            // Update physical GPIO outputs
            setAntennaOutput(selectedIndex);

            // Clear flag
            updatingFromWebSocket = false;
          }
        }

        // Save modelValue and deviceNumber if included
        configPrefs.begin("config", false);
        if (doc.containsKey("modelValue"))
        {
          rcsType = doc["modelValue"];
          configPrefs.putInt("rcs_type", rcsType);
          smciv.setRcsType(rcsType);
        }
        else if (doc.containsKey("rcsType"))
        {
          rcsType = doc["rcsType"];
          configPrefs.putInt("rcs_type", rcsType);
          smciv.setRcsType(rcsType);
        }
        if (doc.containsKey("deviceNumber"))
        {
          deviceNumber = doc["deviceNumber"];
          deviceNumber = constrain(deviceNumber, 1, 4);
          configPrefs.putInt("deviceNumber", deviceNumber);
          reloadCivAddress();
        }
        configPrefs.end();

        // Broadcast updated state to other clients
        for (auto *c : ws.getClients())
        {
          if (c && c->id() != client->id() && c->status() == WS_CONNECTED)
          {
            c->text(msg);
          }
        }
      }

      // --- Improved DEBUG and antennaChange handling ---
      if (!err)
      {
        Serial.println("[DEBUG] Received WebSocket JSON:");
        String debugStr;
        serializeJsonPretty(doc, debugStr);
        Serial.println(debugStr);

        if (doc.containsKey("type"))
        {
          String msgType = doc["type"].as<String>();

          if (msgType == "antennaChange")
          {
            if (doc.containsKey("currentAntennaIndex"))
            {
              int newIndex = doc["currentAntennaIndex"];
              Serial.printf("[DEBUG] antennaChange received, updating selected antenna port to: %d\n", newIndex);

              // Validate antenna index range based on RCS type
              int maxIndex = (rcsType == 0) ? 4 : 7; // RCS-8: 0-4, RCS-10: 0-7
              if (newIndex < 0 || newIndex > maxIndex)
              {
                Serial.printf("[ERROR] Invalid antenna index %d for RCS type %d (max: %d)\n", newIndex, rcsType, maxIndex);
                return;
              }

              // Set flag to prevent callback loop
              updatingFromWebSocket = true;

              smciv.setSelectedAntennaPort(newIndex);

              Preferences switchPrefs;
              switchPrefs.begin("switch", false);
              bool success = switchPrefs.putInt("selectedIndex", newIndex);
              Serial.printf("[DEBUG] Saving selectedIndex to NVS %d, success=%d\n", newIndex, success);
              switchPrefs.end();

              // Update physical GPIO outputs
              setAntennaOutput(newIndex);

              // Clear flag
              updatingFromWebSocket = false;

              DynamicJsonDocument broadcastDoc(512);
              broadcastDoc["type"] = "stateUpdate";
              broadcastDoc["currentAntennaIndex"] = newIndex;
              String broadcastStr;
              serializeJson(broadcastDoc, broadcastStr);

              for (auto *c : ws.getClients())
              {
                if (c && c->status() == WS_CONNECTED)
                {
                  c->text(broadcastStr);
                }
              }
            }
          }
        }
      }
    }
    break;
  }
  default:
    break;
  }
}

// --- Function to broadcast current antenna state to all connected clients ---
void broadcastCurrentAntennaState()
{
  Preferences switchPrefs;
  switchPrefs.begin("switch", false);
  int currentIndex = switchPrefs.getInt("selectedIndex", 0);
  switchPrefs.end();

  DynamicJsonDocument doc(512);
  doc["type"] = "stateUpdate";
  doc["currentAntennaIndex"] = currentIndex;
  doc["source"] = "broadcast";

  String jsonStr;
  serializeJson(doc, jsonStr);

  // Broadcast to all connected WebSocket clients
  ws.textAll(jsonStr);
  Serial.printf("[WS] Broadcasted current antenna state to all clients: %s\n", jsonStr.c_str());
}

// -------------------------------------------------------------------------
// HTTP Request Handlers
// -------------------------------------------------------------------------
void handleRestoreConfig(AsyncWebServerRequest *req)
{
  req->send(200, "text/plain", "Restore config not implemented");
}
void handleScanWifi(AsyncWebServerRequest *req)
{
  req->send(200, "text/plain", "WiFi scan not implemented");
}
void handleUpdateLatch(AsyncWebServerRequest *req)
{
  req->send(200, "text/plain", "Update latch not implemented");
}

// -------------------------------------------------------------------------
// Configuration and Setup Functions
// -------------------------------------------------------------------------

// --- Set antenna names in NVS if missing ---
void ensureDefaultAntennaNames()
{
  antennaPrefs.begin("antennaNames", false);
  for (int i = 1; i <= 8; i++)
  {
    String key = "ant" + String(i);
    if (!antennaPrefs.isKey(key.c_str()))
    {
      antennaPrefs.putString(key.c_str(), "Antenna #" + String(i));
    }
  }
  antennaPrefs.end();
}

// --- Save configuration and optionally reboot ---
void handleSaveConfig(AsyncWebServerRequest *req)
{
  String action = "";
  if (req->hasArg("action"))
    action = req->arg("action");

  if (action == "restoreDefaults")
  {
    Serial.println("Restoring defaults...");
    antennaPrefs.begin("antennaNames", false);
    for (int i = 1; i <= 8; i++)
    {
      String key = "ant" + String(i);
      String defaultName = "Antenna #" + String(i);
      antennaPrefs.putString(key.c_str(), defaultName);
      Serial.printf("Set %s to %s\n", key.c_str(), defaultName.c_str());
    }
    antennaPrefs.end();
    statePrefs.begin("antenna", false);
    statePrefs.remove("state");
    statePrefs.end();
    {
      WiFiManager wm;
      wm.resetSettings();
      Serial.println("WiFi credentials erased using WiFiManager.resetSettings().");
    }
    req->send(200, "text/html", "<html><body><h1>Defaults Restored</h1><p>Rebooting...</p></body></html>");
    delay(3000);
    ESP.restart();
    return;
  }
  else if (action == "autosave")
  {
    // Save rcs_type and deviceNumber to configPrefs
    configPrefs.begin("config", false);
    if (req->hasArg("rcs_type"))
    {
      int rcsTypeInt = req->arg("rcs_type").toInt();
      configPrefs.putInt("rcs_type", rcsTypeInt);
      rcsType = rcsTypeInt;
      smciv.setRcsType(rcsType);
    }
    if (req->hasArg("deviceNumber"))
    {
      int num = req->arg("deviceNumber").toInt();
      num = constrain(num, 1, 4);
      configPrefs.putInt("deviceNumber", num);
      reloadCivAddress();
    }
    configPrefs.end();
    antennaPrefs.begin("antennaNames", false);
    for (int i = 1; i <= 8; i++)
    {
      String key = "ant" + String(i);
      if (req->hasArg(key.c_str()))
        antennaPrefs.putString(key.c_str(), req->arg(key.c_str()));
    }
    antennaPrefs.end();
    req->send(200, "text/plain", "Auto-save successful");
    return;
  }
  else
  {
    antennaPrefs.begin("antennaNames", false);
    for (int i = 1; i <= 8; i++)
    {
      String key = "ant" + String(i);
      if (req->hasArg(key.c_str()))
        antennaPrefs.putString(key.c_str(), req->arg(key.c_str()));
    }
    antennaPrefs.end();
    statePrefs.begin("antenna", false);
    statePrefs.remove("state");
    statePrefs.end();
    req->send(200, "text/html", "<html><body><h1>Config Saved</h1><p>Rebooting...</p></body></html>");
    delay(1000);
    ESP.restart();
  }
}

// -------------------------------------------------------------------------
// Additional HTTP Handlers
// -------------------------------------------------------------------------
void handleRoot(AsyncWebServerRequest *r)
{
  String p = loadFile("/index.html");
  if (p == "")
    return r->send(500, "text/plain", "Error loading index.html");
  r->send(200, "text/html", processTemplate(p));
}

void handleConfig(AsyncWebServerRequest *r)
{
  String p = loadFile("/config.html");
  if (p == "")
    return r->send(500, "text/plain", "Error loading config.html");
  r->send(200, "text/html", processTemplate(p));
}

void handleSwitch(AsyncWebServerRequest *r)
{
  String p = loadFile("/switch.html");
  if (p == "")
    return r->send(500, "text/plain", "Error loading switch.html");
  r->send(200, "text/html", processTemplate(p));
}

// -------------------------------------------------------------------------
// WebSocket Client Event Handler
// -------------------------------------------------------------------------
void onWsClientEvent(WStype_t type, uint8_t *payload, size_t length)
{
  smciv.handleWsClientEvent(type, payload, length);
}

// -------------------------------------------------------------------------
// Main Setup and Loop Functions
// -------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(2000); // Allow serial port to settle

  Serial.println();
  Serial.println("==================================");
  Serial.println("   ShackMate - Switch (RCS-10)    ");
  Serial.println("         BOOTING...               ");
  Serial.println("==================================");

  atom_led.begin();
  atom_led.setBrightness(50);
  setAtomLed(0, 0, 0); // LED OFF
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  setupButtonOutputs();
  ensureDefaultAntennaNames();

  captivePortalActive = true;
  setAtomLed(128, 0, 128); // PURPLE (solid) when entering captive portal mode
  Serial.println("Entered Captive Portal mode (WiFiManager AP).");
  Serial.println("[INFO] ESP32 entered Captive Portal mode (WiFiManager AP).");
  otaActive = false;

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount failed");
  }
  else
  {
    Serial.println("LittleFS mounted successfully");
  }

  // Initialize Preferences for config and WiFi.
  configPrefs.begin("config", false);
  rcsType = configPrefs.getInt("rcs_type", 0);
  deviceNumber = configPrefs.getInt("deviceNumber", 1); // default 1
  configPrefs.end();
  loadLatchedStates();
  wifiPrefs.begin("wifi", false);
  wifiPrefs.end();

  // Enable dual-mode (AP+STA) for WiFiManager captive portal.
  WiFi.mode(WIFI_AP_STA);

  // --- Begin WiFi connection via WiFiManager ---
  Serial.println("[WiFiManager] Starting captive portal for WiFi configuration.");

  WiFiManager wifiManager;
  wifiManager.setAPCallback([](WiFiManager *wm)
                            {
    Serial.println("Entered configuration mode (AP mode)");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP().toString()); });
  // --- Add RCS-8 / RCS-10 radio buttons and Device Number input to captive portal ---
  // New: Add parameters for rcsType and deviceNumber using WiFiManagerParameter
  char rcsTypeBuf[2];
  snprintf(rcsTypeBuf, sizeof(rcsTypeBuf), "%d", rcsType); // Use current stored value

  char deviceNumberBuf[2];
  snprintf(deviceNumberBuf, sizeof(deviceNumberBuf), "%d", deviceNumber);

  WiFiManagerParameter rcsTypeParam("rcs_type", "Switch Model (0=RCS-8, 1=RCS-10)", rcsTypeBuf, 2);
  WiFiManagerParameter deviceNumberParam("deviceNumber", "Device Number (1-4)", deviceNumberBuf, 2);
  wifiManager.addParameter(&rcsTypeParam);
  wifiManager.addParameter(&deviceNumberParam);
  // Set captive portal active before autoConnect
  captivePortalActive = true;
  if (!wifiManager.autoConnect("shackmate-switch"))
  {
    Serial.println("Failed to connect using WiFiManager, restarting...");
    delay(3000);
    ESP.restart();
    delay(5000);
  }
  // WiFiManager connected successfully.
  captivePortalActive = false;
  setAtomLed(0, 255, 0); // Green solid after WiFi connects
  deviceIP = WiFi.localIP().toString();

  // --- Save radio button selection and deviceNumber to NVS ---
  int rcsTypeInt = atoi(rcsTypeParam.getValue());
  int deviceNumberValue = atoi(deviceNumberParam.getValue());
  if (deviceNumberValue < 1)
    deviceNumberValue = 1;
  if (deviceNumberValue > 4)
    deviceNumberValue = 4;
  configPrefs.begin("config", false);
  configPrefs.putInt("rcs_type", rcsTypeInt);
  configPrefs.putInt("deviceNumber", deviceNumberValue);
  configPrefs.end();
  rcsType = rcsTypeInt;
  smciv.setRcsType(rcsType);
  Serial.printf("[MAIN] Updated smciv rcsType to %d after captive portal\n", rcsTypeInt);
  deviceNumber = deviceNumberValue;
  Serial.printf("[CAPTIVE PORTAL] Selected: rcs_type=%d, Device #: %d\n", rcsTypeInt, deviceNumberValue);
  Serial.printf("[MAIN] Global rcsType: %d, deviceNumber: %d\n", rcsType, deviceNumber);
  reloadCivAddress();

  wsClient.onEvent(onWsClientEvent);

  // Check if this is the first configuration after captive portal connection.
  configPrefs.begin("config", false);
  bool configured = configPrefs.getBool("configured", false);
  if (!configured)
  {
    Serial.println("[WiFiManager] First configuration detected. Rebooting to free captive portal resources.");
    configPrefs.putBool("configured", true);
    configPrefs.end();
    delay(2000);
    ESP.restart();
  }
  else
  {
    configPrefs.end();
  }

  // After captive portal connects, store the new credentials in our "wifi" Preferences.
  wifiPrefs.begin("wifi", false);
  wifiPrefs.putString("ssid", WiFi.SSID());
  // WiFi.password() is not available; store a dummy value.
  wifiPrefs.putString("password", "unknown");
  wifiPrefs.end();
  // --- End WiFi connection section ---

  Serial.println("Connected, IP address: " + deviceIP);

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    Serial.println("Failed to obtain time");
  else
    Serial.println("Time synchronized");

  if (!MDNS.begin(MY_MDNS_NAME))
    Serial.println("Error setting up mDNS responder!");
  else
  {
    Serial.print("mDNS responder started: http://");
    Serial.print(MY_MDNS_NAME);
    Serial.println(".local");
  }

  udp.begin(MY_UDP_PORT);
  udpDiscovery.begin(MY_UDP_PORT);
  Serial.printf("UDP discovery listener started on port %d\n", MY_UDP_PORT);

  // Serve static files from LittleFS.
  httpServer.serveStatic("/antenna.css", LittleFS, "/antenna.css");
  httpServer.serveStatic("/antenna.js", LittleFS, "/antenna.js");
  httpServer.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");

  ws.onEvent(onWsEvent);
  wsServer = new AsyncWebServer(4000);
  wsServer->addHandler(&ws);
  wsServer->begin();
  Serial.printf("WebSocket server started on port %d\n", 4000);

  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/config", HTTP_GET, handleConfig);
  httpServer.on("/saveConfig", HTTP_POST, handleSaveConfig);
  httpServer.on("/restoreConfig", HTTP_POST, handleRestoreConfig);
  httpServer.on("/scanWifi", HTTP_GET, handleScanWifi);
  httpServer.on("/switch", HTTP_GET, handleSwitch);
  httpServer.on("/updateLatch", HTTP_GET, handleUpdateLatch);
  // Add redirect from /config.html to /config
  httpServer.on("/config.html", HTTP_GET, [](AsyncWebServerRequest *req)
                { req->redirect("/config"); });
  httpServer.begin();
  Serial.println("HTTP server started on port 80");

  // OTA handlers: manage otaActive and LED blink
  ArduinoOTA.onStart([]()
                     {
                       Serial.println("OTA update starting...");
                       otaActive = true;
                       setAtomLed(255, 255, 255); // Immediately show white
                     });
  ArduinoOTA.onEnd([]()
                   {
                     Serial.println("\nOTA update complete");
                     otaActive = false;
                     setAtomLed(0, 255, 0); // Green solid
                   });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("OTA Progress: %u%%\r", (progress * 100) / total); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Authentication Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed"); });
  ArduinoOTA.begin();
  Serial.println("OTA update service started");

  // At end of setup, ensure LED is solid ON
  setAtomLed(0, 255, 0); // Green solid at end of setup

  // --- Load selected antenna port from NVS and set smciv ---
  Preferences switchPrefs;
  switchPrefs.begin("switch", true);
  int loadedPort = switchPrefs.getInt("selectedIndex", 0);
  switchPrefs.end();
  Serial.printf("[DEBUG] Loaded selectedAntennaPort from switch/selectedIndex: %d\n", loadedPort);

  // Set the SMCIV library to use the same value the main app uses
  smciv.setSelectedAntennaPort(loadedPort);

  // Also ensure the SMCIV's internal storage is synchronized
  // (This will save to antenna/selectedPort to keep SMCIV's internal storage in sync)
  Serial.printf("[DEBUG] Synchronized SMCIV internal storage with main app value: %d\n", loadedPort);

  // Set initial antenna output based on stored selection
  setAntennaOutput(loadedPort);
  Serial.printf("[SETUP] Initial antenna output set to index %d\n", loadedPort);
}

// -------------------------------------------------------------------------
// Main Loop Function
// -------------------------------------------------------------------------
void loop()
{
  ArduinoOTA.handle();

  unsigned long now = millis();

  // Atom LED blink logic for Atom S3
  static unsigned long ledLastToggle = 0;
  static bool ledOn = false;
  unsigned long ledBlinkInterval = 0;
  static uint8_t curR = 0, curG = 255, curB = 0; // Default: green

  if (otaActive)
  {
    ledBlinkInterval = 100;
    curR = 255;
    curG = 255;
    curB = 255; // WHITE fast blink
  }
  else if (captivePortalActive)
  {
    ledBlinkInterval = 500;
    curR = 128;
    curG = 0;
    curB = 128; // Purple
  }
  else
  {
    ledBlinkInterval = 0; // No blink, just solid green
    curR = 0;
    curG = 255;
    curB = 0; // Green
  }

  if (otaActive)
  {
    if (now - ledLastToggle >= ledBlinkInterval)
    {
      ledLastToggle = now;
      ledOn = !ledOn;
      setAtomLed(ledOn ? curR : 0, ledOn ? curG : 0, ledOn ? curB : 0);
    }
  }
  else if (captivePortalActive)
  {
    if (!ledOn || curR != 128 || curG != 0 || curB != 128)
    {
      ledOn = true;
      curR = 128;
      curG = 0;
      curB = 128;
      setAtomLed(curR, curG, curB);
    }
  }
  else
  {
    // Not OTA/captive, keep green solid
    if (!ledOn || curR != 0 || curG != 255 || curB != 0)
    {
      ledOn = true;
      curR = 0;
      curG = 255;
      curB = 0;
      setAtomLed(curR, curG, curB);
    }
  }

  // Only parse UDP packets if WS client NOT connected
  if (!wsClient.isConnected())
  {
    int ps = udp.parsePacket();
    while (ps > 0)
    {
      IPAddress rip = udp.remoteIP();
      if (rip != WiFi.localIP() && rip != WiFi.softAPIP())
      {
        char buf[ps + 1];
        int len = udp.read(buf, ps);
        if (len <= 0)
        {
          // Bad read: flush and break to avoid repeated errors
          udp.flush();
          break;
        }
        buf[len] = '\0';
        String msg(buf);

        Serial.printf("[UDP] Packet from %s: %s\n", rip.toString().c_str(), msg.c_str());

        // Note: UDP messages are logged but not stored for broadcast
        // (broadcastMessages functionality removed as unused)
      }
      else
      {
        // Packet from self IP: flush buffer
        udp.flush();
      }
      ps = udp.parsePacket();
    }
  }

  // --- UDP ShackMate Discovery Listener (responds to "ShackMate" requests) ---
  int discLen = udpDiscovery.parsePacket();
  if (discLen > 0)
  {
    char discBuf[discLen + 1];
    int discRead = udpDiscovery.read(discBuf, discLen);
    discBuf[discRead] = '\0';
    String discMsg(discBuf);
    if (discMsg.startsWith("ShackMate"))
    {
      int comma1 = discMsg.indexOf(',');
      int comma2 = discMsg.lastIndexOf(',');
      if (comma1 > 0 && comma2 > comma1)
      {
        String foundIp = discMsg.substring(comma1 + 1, comma2);
        String portStr = discMsg.substring(comma2 + 1);
        uint16_t foundPort = portStr.toInt();
        static bool connecting = false;
        static String lastIp = "";
        static uint16_t lastPort = 0;

        if (foundIp != lastIp || foundPort != lastPort)
        {
          lastIp = foundIp;
          lastPort = foundPort;
          connecting = true;
          // --- Update discoveredWsServer and broadcast dashboardStatus ---
          discoveredWsServer = foundIp + ":" + String(foundPort);
          discoveredWsIp = foundIp;
          discoveredWsPort = foundPort;
          broadcastDashboardStatus();
        }

        if (connecting && foundIp.length() > 0 && foundPort > 0)
        {
          Serial.printf("[UDP DISCOVERY] Connecting to new WS endpoint %s:%d\n", foundIp.c_str(), foundPort);
          if (wsClient.isConnected())
          {
            Serial.println("[UDP DISCOVERY] Disconnecting existing WS client connection...");
            wsClient.disconnect();
            delay(100);
          }
          smciv.connectToRemoteWs(foundIp, foundPort);
          delay(200); // allow time for connect attempt
          Serial.printf("[UDP DISCOVERY] Post-delay wsClient.isConnected()=%d\n", wsClient.isConnected());
          connecting = false;
          Serial.println("[UDP DISCOVERY] connectToRemoteWs() called.");
        }
      }
      String resp = "ShackMate," + deviceIP + "," + String(WS_PORT);
      udpDiscovery.beginPacket(udpDiscovery.remoteIP(), udpDiscovery.remotePort());
      udpDiscovery.print(resp);
      udpDiscovery.endPacket();
    }
  }

  // --- WebSocket Client Keepalive/Ping and Reconnect Logic ---
  static unsigned long lastWsPing = 0;
  static unsigned long lastWsReconnect = 0;
  const unsigned long wsPingInterval = 10000;     // 10s ping
  const unsigned long wsReconnectInterval = 5000; // 5s reconnect

  // Track last used IP/port for wsClient
  static String wsClientLastIp = "";
  static uint16_t wsClientLastPort = 0;

  if (wsClient.isConnected())
  {
    // If connected, send ping every 10s
    if (now - lastWsPing > wsPingInterval)
    {
      lastWsPing = now;
      wsClient.sendPing();
      Serial.printf("[WS CLIENT] Sent ping to %s:%u\n", wsClientLastIp.c_str(), wsClientLastPort);
    }
  }
  else
  {
    // If not connected, try to reconnect every 5s to the most recently discovered UDP server
    if (now - lastWsReconnect > wsReconnectInterval)
    {
      lastWsReconnect = now;
      // Only attempt if we have a discovered server
      if (discoveredWsServer.length() > 0)
      {
        int colonIdx = discoveredWsServer.indexOf(":");
        if (colonIdx > 0)
        {
          String ip = discoveredWsServer.substring(0, colonIdx);
          uint16_t port = discoveredWsServer.substring(colonIdx + 1).toInt();
          if (ip != wsClientLastIp || port != wsClientLastPort)
          {
            wsClientLastIp = ip;
            wsClientLastPort = port;
          }
          Serial.printf("[WS CLIENT] Attempting reconnect to %s:%u\n", ip.c_str(), port);
          smciv.connectToRemoteWs(ip, port);
        }
      }
    }
  }

  smciv.loop();
  wsClient.loop();

  // --- LED color update on WebSocket connection status change ---
  bool connected = wsClient.isConnected();
  if (connected != wsConnected)
  {
    wsConnected = connected;
    if (wsConnected)
    {
      Serial.println("WebSocket client connected, LED BLUE.");
      setAtomLed(0, 0, 255); // Blue
    }
    else
    {
      Serial.println("WebSocket client disconnected, LED GREEN.");
      setAtomLed(0, 255, 0); // Green
    }
    // --- Broadcast dashboardStatus on connection status change ---
    broadcastDashboardStatus();
  }

  // --- Long press to reset WiFi ---
  static unsigned long buttonPressStart = 0;
  static bool buttonWasPressed = false;
  bool buttonPressed = digitalRead(BUTTON_PIN) == LOW;
  if (buttonPressed && !buttonWasPressed)
  {
    buttonPressStart = millis();
    buttonWasPressed = true;
  }
  if (!buttonPressed && buttonWasPressed)
  {
    buttonWasPressed = false;
    buttonPressStart = 0;
  }
  if (buttonPressed && (millis() - buttonPressStart > 5000))
  {
    Serial.println("[BUTTON] Held 5s, erasing WiFi credentials and rebooting...");
    WiFiManager wm;
    wm.resetSettings();
    configPrefs.begin("config", false);
    configPrefs.putBool("configured", false);
    configPrefs.end();
    setAtomLed(255, 128, 0); // Orange before reboot
    delay(500);
    ESP.restart();
  }

  delay(1);
}

// -------------------------------------------------------------------------
// Antenna Details Persistence Functions
// -------------------------------------------------------------------------

// Save antenna details for a specific antenna index to NVS
void saveAntennaDetails(int antennaIndex, int typeIndex, int styleIndex, int polIndex, int mfgIndex, int bandPattern, bool disabled)
{
  if (antennaIndex < 0 || antennaIndex >= 10)
    return;

  detailsPrefs.begin("antennaDetails", false);

  String prefix = "ant" + String(antennaIndex) + "_";
  detailsPrefs.putInt((prefix + "type").c_str(), typeIndex);
  detailsPrefs.putInt((prefix + "style").c_str(), styleIndex);
  detailsPrefs.putInt((prefix + "pol").c_str(), polIndex);
  detailsPrefs.putInt((prefix + "mfg").c_str(), mfgIndex);
  detailsPrefs.putInt((prefix + "bands").c_str(), bandPattern);
  detailsPrefs.putBool((prefix + "disabled").c_str(), disabled);

  detailsPrefs.end();

  Serial.printf("[NVS] Saved antenna %d details: type=%d, style=%d, pol=%d, mfg=%d, bands=%d, disabled=%s\n",
                antennaIndex, typeIndex, styleIndex, polIndex, mfgIndex, bandPattern, disabled ? "true" : "false");
}

// Load antenna details for a specific antenna index from NVS
void loadAntennaDetails(int antennaIndex, int *typeIndex, int *styleIndex, int *polIndex, int *mfgIndex, int *bandPattern, bool *disabled)
{
  if (antennaIndex < 0 || antennaIndex >= 10)
  {
    *typeIndex = 0;
    *styleIndex = 0;
    *polIndex = 0;
    *mfgIndex = 0;
    *bandPattern = 0;
    *disabled = false;
    return;
  }

  detailsPrefs.begin("antennaDetails", true); // Read-only

  String prefix = "ant" + String(antennaIndex) + "_";
  *typeIndex = detailsPrefs.getInt((prefix + "type").c_str(), 0);
  *styleIndex = detailsPrefs.getInt((prefix + "style").c_str(), 0);
  *polIndex = detailsPrefs.getInt((prefix + "pol").c_str(), 0);
  *mfgIndex = detailsPrefs.getInt((prefix + "mfg").c_str(), 0);
  *bandPattern = detailsPrefs.getInt((prefix + "bands").c_str(), 0);
  *disabled = detailsPrefs.getBool((prefix + "disabled").c_str(), false);

  detailsPrefs.end();
}

// Save all antenna details from a JSON antennaState array to NVS
void saveAllAntennaDetails(JsonArray antennaStateArray)
{
  for (int i = 0; i < antennaStateArray.size() && i < 10; i++)
  {
    JsonObject antenna = antennaStateArray[i];
    if (antenna.isNull())
      continue;

    int typeIndex = antenna["typeIndex"] | 0;
    int styleIndex = antenna["styleIndex"] | 0;
    int polIndex = antenna["polIndex"] | 0;
    int mfgIndex = antenna["mfgIndex"] | 0;
    int bandPattern = antenna["bandPattern"] | 0;
    bool disabled = antenna["disabled"] | false;

    saveAntennaDetails(i, typeIndex, styleIndex, polIndex, mfgIndex, bandPattern, disabled);
  }
  Serial.println("[NVS] Saved all antenna details to NVS");
}

// Load all antenna details from NVS into a JSON antennaState array
void loadAllAntennaDetails(JsonArray antennaStateArray)
{
  for (int i = 0; i < 10; i++)
  {
    JsonObject antenna = antennaStateArray.createNestedObject();

    int typeIndex, styleIndex, polIndex, mfgIndex, bandPattern;
    bool disabled;
    loadAntennaDetails(i, &typeIndex, &styleIndex, &polIndex, &mfgIndex, &bandPattern, &disabled);

    antenna["typeIndex"] = typeIndex;
    antenna["styleIndex"] = styleIndex;
    antenna["polIndex"] = polIndex;
    antenna["mfgIndex"] = mfgIndex;
    antenna["bandPattern"] = bandPattern;
    antenna["disabled"] = disabled;
  }
}
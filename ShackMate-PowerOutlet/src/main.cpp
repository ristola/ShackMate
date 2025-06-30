/*
Control Output 1 and Output 2 via WebSocket Port 4000
Valid Commands:
  { "command": "output1", "value": true }
  { "command": "output1", "value": false }
  { "command": "output2", "value": true }
  { "command": "output2", "value": false }

Debug/Maintenance Commands:
  { "command": "resetRebootCounter" }  // Resets the boot counter to 0
  { "command": "testCaptivePortal", "enable": true }   // Enable captive portal LED test mode
  { "command": "testCaptivePortal", "enable": false }  // Disable captive portal LED test mode
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <time.h>       // For NTP and time functions
#include <SPIFFS.h>     // For serving HTML files from filesystem
#include <ArduinoOTA.h> // OTA update support
#include <HLW8012.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_spi_flash.h>
#include <WebSocketsClient.h> // For client WebSocket connections
#include "SMCIV.h"            // For CI-V message parsing and handling
#include <vector>             // For CI-V message data storage

// -------------------------------------------------------------------------
// Project Constants
// -------------------------------------------------------------------------
#define NAME "ShackMate - Outlet"
#define VERSION "2.00"
#define AUTHOR "Half Baked Circuits"

// mDNS Name
#define MDNS_NAME "shackmate-outlet"

// Define total available RAM (adjust if needed)
#define TOTAL_RAM 327680

// UDP Port definition
#define UDP_PORT 4210

// Sensor pins and calibration
#define PIN_LUX_ADC 34
#define PIN_HLW_CF 27
#define PIN_HLW_CF1 26
#define PIN_HLW_SEL 25
static constexpr float CURRENT_RESISTOR = 0.001f;
// Hardware voltage divider ratio from schematic
static constexpr float VOLTAGE_DIVIDER = 770.0f;
static constexpr uint32_t SENSOR_UPDATE_INTERVAL_MS = 10000; // 10s

// -------------------------------------------------------------------------
// HLW8012 Interrupt Setup (Required for CF/CF1 pulse counting)
// -------------------------------------------------------------------------
// Forward declarations
void IRAM_ATTR hlw8012_cf_interrupt();
void IRAM_ATTR hlw8012_cf1_interrupt();

// Library expects an interrupt on both edges
void setInterrupts()
{
  attachInterrupt(digitalPinToInterrupt(PIN_HLW_CF1), hlw8012_cf1_interrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_HLW_CF), hlw8012_cf_interrupt, FALLING);
}

// Pin Definitions
// Relay control pins
#define PIN_RELAY1 15
#define PIN_RELAY2 32
#define PIN_RELAY1_LED 19
#define PIN_RELAY2_LED 16
// Button pins
#define PIN_BUTTON1 18
#define PIN_BUTTON2 17

// -------------------------------------------------------------------------
// Global Objects & Variables
// -------------------------------------------------------------------------
Preferences preferences;
AsyncWebServer httpServer(80);
AsyncWebSocket ws("/ws");
AsyncWebServer wsServer(4000);
WiFiUDP udpListener;       // UDP listener for ShackMate discovery
WebSocketsClient wsClient; // Client WebSocket for connecting to other ShackMate devices
HLW8012 hlw;
SMCIV civHandler; // CI-V message handler instance

// HLW8012 interrupt handlers (defined after hlw declaration)
void IRAM_ATTR hlw8012_cf_interrupt()
{
  hlw.cf_interrupt();
}
void IRAM_ATTR hlw8012_cf1_interrupt()
{
  hlw.cf1_interrupt();
}


String deviceIP = "";
String tcpPort = "4000";
String wsPortStr = tcpPort;

// WebSocket client connection state
bool wsClientConnected = false;
bool wsClientEverConnected = false; // Track if we were ever connected to help distinguish reconnecting vs discovering
String connectedServerIP = "";
uint16_t connectedServerPort = 0;
unsigned long lastConnectionAttempt = 0;
unsigned long lastWebSocketActivity = 0;         // Track when we last heard from the server
unsigned long lastPingSent = 0;                  // Track when we last sent a ping
const unsigned long CONNECTION_COOLDOWN = 10000; // 10 seconds between connection attempts
const unsigned long WEBSOCKET_TIMEOUT = 60000;   // 60 seconds without activity = assume dead (increased since no ping)
const unsigned long PING_INTERVAL = 30000;       // Send ping every 30 seconds

unsigned long bootTime = 0;
uint32_t rebootCounter = 0; // Track number of reboots

// Sensor update timing
unsigned long lastSensorUpdate = 0;

// Device ID and CIV Address configuration
uint8_t deviceId = 1;     // Default Device ID (1-4)
String civAddress = "B0"; // Default CIV Address (B0-B3)

// Helper function to get CI-V address as byte value
uint8_t getCivAddressByte()
{
  return 0xB0 + (deviceId - 1); // Device ID 1->0xB0, 2->0xB1, 3->0xB2, 4->0xB3
}

// Persistent relay states
bool relay1State = false;
bool relay2State = false;
char label1Text[32];
char label2Text[32];

// Runtime voltage calibration
float voltageCalibrationFactor = 1.0f;
bool voltageCalibrated = false;

// Button press flags (set in ISR)
volatile bool button1Pressed = false;
volatile bool button2Pressed = false;

// Captive Portal LED alternating variables
bool captivePortalActive = false;
unsigned long lastLedToggleTime = 0;
bool ledAlternateState = false;
const unsigned long LED_ALTERNATE_INTERVAL = 250; // 250ms delay
// Debounce globals
unsigned long lastButton1Time = 0;
unsigned long lastButton2Time = 0;
const unsigned long debounceDelay = 50; // milliseconds

// -------------------------------------------------------------------------
// Function Prototypes
// -------------------------------------------------------------------------

String getUptime();
String getChipID();
int getChipRevision();
uint32_t getFlashSize();
uint32_t getPsramSize();
int getCpuFrequency();
uint32_t getFreeHeap();
uint32_t getTotalHeap();
uint32_t getSketchSize();
uint32_t getFreeSketchSpace();
float readInternalTemperature();

String loadFile(const char *path);
String processTemplate(String tmpl);

// UDP Discovery and WebSocket Client functions
void handleUdpDiscovery();
void connectToShackMateServer(String ip, uint16_t port);
void onWebSocketClientEvent(WStype_t type, uint8_t *payload, size_t length);
void disconnectWebSocketClient();

// -------------------------------------------------------------------------
// Debug Helper Function
// -------------------------------------------------------------------------
void sendDebugMessage(String message)
{
  // Always log to serial
  Serial.println("DEBUG: " + message);

  // Simple web client messaging - no rate limiting for now
  if (ws.count() > 0)
  {
    String simpleMsg = "{\"type\":\"debug\",\"message\":\"" + message + "\"}";
    ws.textAll(simpleMsg);
  }
}

// -------------------------------------------------------------------------
// System Info Functions
// -------------------------------------------------------------------------
String getUptime()
{
  unsigned long now = millis();
  unsigned long secs = (now - bootTime) / 1000;
  unsigned long days = secs / 86400;
  secs %= 86400;
  unsigned long hours = secs / 3600;
  secs %= 3600;
  unsigned long mins = secs / 60;
  secs %= 60;
  char buf[50];
  if (days > 0)
  {
    sprintf(buf, "%lu days %lu hrs %lu mins %lu secs", days, hours, mins, secs);
  }
  else
  {
    sprintf(buf, "%lu hrs %lu mins %lu secs", hours, mins, secs);
  }
  return String(buf);
}

String getChipID()
{
  uint64_t chipid = ESP.getEfuseMac();
  char idString[17];
  sprintf(idString, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  return String(idString);
}

int getChipRevision()
{
  return ESP.getChipRevision();
}

uint32_t getFlashSize()
{
  return ESP.getFlashChipSize();
}

uint32_t getPsramSize()
{
  return psramFound() ? ESP.getPsramSize() : 0;
}

int getCpuFrequency()
{
  return ESP.getCpuFreqMHz();
}

uint32_t getFreeHeap()
{
  return ESP.getFreeHeap();
}

uint32_t getTotalHeap()
{
  return heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

uint32_t getSketchSize()
{
  return ESP.getSketchSize();
}

uint32_t getFreeSketchSpace()
{
  return ESP.getFreeSketchSpace();
}

float readInternalTemperature()
{
  return 42.0; // Dummy value; use an external sensor for real measurements.
}

// -------------------------------------------------------------------------
// Template Functions
// -------------------------------------------------------------------------
String loadFile(const char *path)
{
  File file = SPIFFS.open(path, "r");
  if (!file || file.isDirectory())
  {
    Serial.printf("Failed to open file: %s\n", path);
    return "";
  }
  String content;
  while (file.available())
  {
    content += char(file.read());
  }
  file.close();
  return content;
}

String processTemplate(String tmpl)
{
  struct tm timeinfo;
  char timeStr[64];
  if (getLocalTime(&timeinfo))
  {
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  }
  else
  {
    strcpy(timeStr, "TIME_NOT_SET");
  }
  tmpl.replace("%PROJECT_NAME%", String(NAME));
  tmpl.replace("%TIME%", String(timeStr));
  tmpl.replace("%IP%", deviceIP);
  tmpl.replace("%WEBSOCKET_PORT%", wsPortStr);
  tmpl.replace("%UDP_PORT%", String(UDP_PORT));
  tmpl.replace("%VERSION%", VERSION);

  uint32_t totalMem = getTotalHeap();
  uint32_t freeMem = getFreeHeap();
  uint32_t usedMem = totalMem - freeMem;
  tmpl.replace("%MEM_TOTAL%", String(totalMem / 1024) + " KB");
  tmpl.replace("%MEM_USED%", String(usedMem / 1024) + " KB");
  tmpl.replace("%MEM_FREE%", String(freeMem / 1024) + " KB");

  uint32_t flashTotal = getFlashSize();
  uint32_t sketchSize = getSketchSize();
  tmpl.replace("%FLASH_TOTAL%", String(flashTotal / 1024) + " KB");
  tmpl.replace("%FLASH_USED%", String(sketchSize / 1024) + " KB");
  tmpl.replace("%FLASH_FREE%", String((flashTotal - sketchSize) / 1024) + " KB");

  preferences.begin("config", true);
  preferences.end();

  tmpl.replace("%UPTIME%", getUptime());
  tmpl.replace("%CHIP_ID%", getChipID());
  tmpl.replace("%CHIP_REV%", String(getChipRevision()));
  tmpl.replace("%PSRAM_SIZE%", String(getPsramSize()));
  tmpl.replace("%CPU_FREQ%", String(getCpuFrequency()));
  tmpl.replace("%FREE_HEAP%", String(getFreeHeap()));

  uint32_t totalSketch = sketchSize + getFreeSketchSpace();
  tmpl.replace("%SKETCH_USED%", String(sketchSize));
  tmpl.replace("%SKETCH_TOTAL%", String(totalSketch));

  float tempC = readInternalTemperature();
  float tempF = (tempC * 1.8) + 32.0;
  tmpl.replace("%TEMPERATURE_C%", String(tempC, 2));
  tmpl.replace("%TEMPERATURE_F%", String(tempF, 2));

  return tmpl;
}

// -------------------------------------------------------------------------
// CI-V Message Parsing and Handling
// -------------------------------------------------------------------------

// Structure to hold parsed CI-V message
struct CivMessage
{
  bool valid;
  uint8_t toAddr;
  uint8_t fromAddr;
  uint8_t command;
  uint8_t subCommand;
  std::vector<uint8_t> data;
};

// Parse CI-V message from hex string
CivMessage parseCivMessage(const String &hexMsg)
{
  CivMessage msg = {false, 0, 0, 0, 0, {}};

  sendDebugMessage("CI-V: Parsing message: '" + hexMsg + "'");

  // Remove spaces and convert to upper case
  String cleanHex = hexMsg;
  cleanHex.replace(" ", "");
  cleanHex.toUpperCase();

  sendDebugMessage("CI-V: Clean hex string: '" + cleanHex + "' (length: " + String(cleanHex.length()) + ")");

  // Check minimum length (FE FE TO FROM CMD FD = 12 hex chars = 6 bytes minimum)
  if (cleanHex.length() < 12 || cleanHex.length() % 2 != 0)
  {
    sendDebugMessage("CI-V: Invalid message length: " + String(cleanHex.length()));
    return msg;
  }

  // Convert hex string to bytes
  std::vector<uint8_t> bytes;
  for (int i = 0; i < cleanHex.length(); i += 2)
  {
    String byteStr = cleanHex.substring(i, i + 2);
    uint8_t byte = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    bytes.push_back(byte);
  }

  // Validate CI-V message format
  if (bytes.size() < 6)
  {
    sendDebugMessage("CI-V: Message too short: " + String(bytes.size()) + " bytes");
    return msg;
  }

  // Check preamble (FE FE)
  if (bytes[0] != 0xFE || bytes[1] != 0xFE)
  {
    sendDebugMessage("CI-V: Invalid preamble - expected FE FE, got " + String(bytes[0], HEX) + " " + String(bytes[1], HEX));
    return msg;
  }

  // Check terminator (FD)
  if (bytes[bytes.size() - 1] != 0xFD)
  {
    String invalidHex = String(bytes[bytes.size() - 1], HEX);
    invalidHex.toUpperCase();
    if (invalidHex.length() == 1)
      invalidHex = "0" + invalidHex;
    sendDebugMessage("CI-V: Invalid terminator - expected FD, got " + invalidHex);
    return msg;
  }

  // Extract message components
  msg.toAddr = bytes[2];
  msg.fromAddr = bytes[3];
  msg.command = bytes[4];

  // For command 35 (outlet status), there is no subcommand - data comes directly after command
  if (msg.command == 0x35)
  {
    msg.subCommand = 0x00; // No subcommand for command 35

    // Extract data portion (after command, before terminator)
    if (bytes.size() > 6)
    {
      for (size_t i = 5; i < bytes.size() - 1; i++)
      {
        msg.data.push_back(bytes[i]);
      }
    }
  }
  else
  {
    // For other commands: FE FE TO FROM CMD [SUB] [DATA...] FD
    if (bytes.size() == 6)
    {
      // Basic command, no subcommand
      msg.subCommand = 0x00;
    }
    else if (bytes.size() >= 7)
    {
      // Command with subcommand and optional data
      msg.subCommand = bytes[5];

      // Extract data portion (after subcommand, before terminator)
      if (bytes.size() > 7)
      {
        for (size_t i = 6; i < bytes.size() - 1; i++)
        {
          msg.data.push_back(bytes[i]);
        }
      }
    }
  }

  msg.valid = true;

  // Debug output
  String toAddrHex = String(msg.toAddr, HEX);
  toAddrHex.toUpperCase();
  if (toAddrHex.length() == 1)
    toAddrHex = "0" + toAddrHex;

  String fromAddrHex = String(msg.fromAddr, HEX);
  fromAddrHex.toUpperCase();
  if (fromAddrHex.length() == 1)
    fromAddrHex = "0" + fromAddrHex;

  String cmdHex = String(msg.command, HEX);
  cmdHex.toUpperCase();
  if (cmdHex.length() == 1)
    cmdHex = "0" + cmdHex;

  String subHex = String(msg.subCommand, HEX);
  subHex.toUpperCase();
  if (subHex.length() == 1)
    subHex = "0" + subHex;

  String debugMsg = "CI-V: Parsed - TO:" + toAddrHex +
                    " FROM:" + fromAddrHex +
                    " CMD:" + cmdHex +
                    " SUB:" + subHex;
  sendDebugMessage(debugMsg);

  return msg;
}

// Check if CI-V message is addressed to us
bool isCivMessageForUs(const CivMessage &msg)
{
  uint8_t ourAddress = getCivAddressByte();
  bool isBroadcast = (msg.toAddr == 0x00);
  bool isAddressedToUs = (msg.toAddr == ourAddress);

  if (isBroadcast || isAddressedToUs)
  {
    String addrType = isBroadcast ? "broadcast" : "direct";
    sendDebugMessage("CI-V: Message for us (" + addrType + ") - Our addr: " + String(ourAddress, HEX) + ", TO: " + String(msg.toAddr, HEX));
    return true;
  }

  // Message not for us - debug message removed to reduce spam
  return false;
}

// Process CI-V message and generate appropriate response
// Returns true if message was handled internally, false if it should be forwarded to physical CI-V port
bool processCivMessage(const CivMessage &msg)
{
  uint8_t ourCivAddr = getCivAddressByte();
  sendDebugMessage("CI-V: Processing command " + String(msg.command, HEX) + " subcommand " + String(msg.subCommand, HEX) + " with our address 0x" + String(ourCivAddr, HEX));

  // Convert message back to hex string for SMCIV library processing
  String hexString = "";
  hexString += "FE FE ";

  String toAddrHex = String(msg.toAddr, HEX);
  toAddrHex.toUpperCase();
  if (toAddrHex.length() == 1)
    toAddrHex = "0" + toAddrHex;
  hexString += toAddrHex + " ";

  String fromAddrHex = String(msg.fromAddr, HEX);
  fromAddrHex.toUpperCase();
  if (fromAddrHex.length() == 1)
    fromAddrHex = "0" + fromAddrHex;
  hexString += fromAddrHex + " ";

  String commandHex = String(msg.command, HEX);
  commandHex.toUpperCase();
  if (commandHex.length() == 1)
    commandHex = "0" + commandHex;
  hexString += commandHex + " ";

  String subCommandHex = String(msg.subCommand, HEX);
  subCommandHex.toUpperCase();
  if (subCommandHex.length() == 1)
    subCommandHex = "0" + subCommandHex;
  hexString += subCommandHex;

  if (msg.data.size() > 0)
  {
    for (uint8_t dataByte : msg.data)
    {
      String dataByteHex = String(dataByte, HEX);
      dataByteHex.toUpperCase();
      if (dataByteHex.length() == 1)
        dataByteHex = "0" + dataByteHex;
      hexString += " " + dataByteHex;
    }
  }
  hexString += " FD";

  sendDebugMessage("CI-V: Forwarding to SMCIV library: " + hexString);

  // Handle specific CI-V commands directly BEFORE forwarding to SMCIV library
  sendDebugMessage("CI-V: Checking command 0x" + String(msg.command, HEX) + " with subcommand 0x" + String(msg.subCommand, HEX));

  // Check for commands we handle directly (19 00 and 19 01)
  if (msg.command == 0x19 && (msg.subCommand == 0x00 || msg.subCommand == 0x01))
  {
    sendDebugMessage("CI-V: Handling command 19 directly - WILL NOT call SMCIV library");

    if (msg.subCommand == 0x00)
    {
      sendDebugMessage("CI-V: Echo request (19 00) received - responding with CI-V address 0x" + String(getCivAddressByte(), HEX));

      // Create echo response: FE FE [TO=original FROM] [FROM=our addr] 19 00 [our addr] FD
      // Ensure proper hex formatting with uppercase and zero-padding
      String toAddrHex = String(msg.fromAddr, HEX); // TO = original sender
      toAddrHex.toUpperCase();
      if (toAddrHex.length() == 1)
        toAddrHex = "0" + toAddrHex;

      String fromAddrHex = String(getCivAddressByte(), HEX); // FROM = our address
      fromAddrHex.toUpperCase();
      if (fromAddrHex.length() == 1)
        fromAddrHex = "0" + fromAddrHex;

      String echoResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " 19 00 " + fromAddrHex + " FD";

      sendDebugMessage("CI-V: Sending echo response: " + echoResponse);

      // Send response via WebSocket to the CI-V server
      if (wsClientConnected)
      {
        wsClient.sendTXT(echoResponse);
        sendDebugMessage("CI-V: Echo response sent via WebSocket");
      }
      else
      {
        sendDebugMessage("CI-V: Warning - WebSocket not connected, echo response not sent");
      }
    }
    else if (msg.subCommand == 0x01)
    {
      sendDebugMessage("CI-V: ModelID request (19 01) received - responding with ESP32 IP address");

      // Create ModelID response with IP address: FE FE [TO=original FROM] [FROM=our addr] 19 01 [IP bytes] FD
      // Ensure proper hex formatting with uppercase and zero-padding
      String toAddrHex = String(msg.fromAddr, HEX); // TO = original sender
      toAddrHex.toUpperCase();
      if (toAddrHex.length() == 1)
        toAddrHex = "0" + toAddrHex;

      String fromAddrHex = String(getCivAddressByte(), HEX); // FROM = our address
      fromAddrHex.toUpperCase();
      if (fromAddrHex.length() == 1)
        fromAddrHex = "0" + fromAddrHex;

      // Convert IP address to hex bytes
      IPAddress ip = WiFi.localIP();
      String ipHex = "";
      for (int i = 0; i < 4; i++)
      {
        String byteHex = String(ip[i], HEX);
        byteHex.toUpperCase();
        if (byteHex.length() == 1)
          byteHex = "0" + byteHex;
        ipHex += " " + byteHex;
      }

      String modelResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " 19 01" + ipHex + " FD";

      sendDebugMessage("CI-V: Sending IP address response: " + modelResponse + " (IP: " + ip.toString() + ")");

      // Send response via WebSocket to the CI-V server
      if (wsClientConnected)
      {
        wsClient.sendTXT(modelResponse);
        sendDebugMessage("CI-V: IP address response sent via WebSocket");
      }
      else
      {
        sendDebugMessage("CI-V: Warning - WebSocket not connected, IP address response not sent");
      }
    }

    sendDebugMessage("CI-V: Command 19 handled directly - exiting without calling SMCIV library");
    return true; // Message was handled internally - do NOT forward to physical CI-V port
  }

  // Handle Command 34 - Read Model
  if (msg.command == 0x34 && msg.data.size() == 0)
  {
    sendDebugMessage("CI-V: Command 34 (Read Model) received - responding with model type");

    String toAddrHex = String(msg.fromAddr, HEX); // TO = original sender
    toAddrHex.toUpperCase();
    if (toAddrHex.length() == 1)
      toAddrHex = "0" + toAddrHex;

    String fromAddrHex = String(getCivAddressByte(), HEX); // FROM = our address
    fromAddrHex.toUpperCase();
    if (fromAddrHex.length() == 1)
      fromAddrHex = "0" + fromAddrHex;

    String modelResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " 34 01 FD"; // Respond with 01 (Outdoor Dual Outlet)

    sendDebugMessage("CI-V: Sending model response: " + modelResponse);

    if (wsClientConnected)
    {
      wsClient.sendTXT(modelResponse);
      sendDebugMessage("CI-V: Model response sent via WebSocket");
    }
    else
    {
      sendDebugMessage("CI-V: Warning - WebSocket not connected, model response not sent");
    }

    return true; // Message handled internally
  }

  // Handle Command 35 - Read/Set Outlet Status
  if (msg.command == 0x35)
  {
    // For SET operations (with data), check if it's on broadcast address (00)
    // For READ operations (no data), respond to both direct addresses and broadcast
    if (msg.data.size() > 0 && msg.toAddr == 0x00)
    {
      // Check if the value is invalid (> 0x03) and respond with FA if so
      uint8_t setValue = msg.data[0];
      if (setValue > 0x03)
      {
        sendDebugMessage("CI-V: Command 35 SET operation on broadcast with invalid value 0x" + String(setValue, HEX) + " - responding with FA (NAK)");

        String toAddrHex = String(msg.fromAddr, HEX); // TO = original sender
        toAddrHex.toUpperCase();
        if (toAddrHex.length() == 1)
          toAddrHex = "0" + toAddrHex;

        String fromAddrHex = String(getCivAddressByte(), HEX); // FROM = our address
        fromAddrHex.toUpperCase();
        if (fromAddrHex.length() == 1)
          fromAddrHex = "0" + fromAddrHex;

        String nakResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " FA FD";
        sendDebugMessage("CI-V: Sending FA (NAK) response for broadcast invalid value: " + nakResponse);

        if (wsClientConnected)
        {
          wsClient.sendTXT(nakResponse);
          sendDebugMessage("CI-V: FA (NAK) response sent via WebSocket");
        }
        else
        {
          sendDebugMessage("CI-V: Warning - WebSocket not connected, FA (NAK) response not sent");
        }

        return true; // Invalid broadcast SET handled with NAK response
      }
      else
      {
        sendDebugMessage("CI-V: Command 35 SET operation received on broadcast address (00) with valid value - ignoring as per protocol");
        return true; // Ignore valid broadcast SET messages for command 35
      }
    }

    String toAddrHex = String(msg.fromAddr, HEX); // TO = original sender
    toAddrHex.toUpperCase();
    if (toAddrHex.length() == 1)
      toAddrHex = "0" + toAddrHex;

    String fromAddrHex = String(getCivAddressByte(), HEX); // FROM = our address
    fromAddrHex.toUpperCase();
    if (fromAddrHex.length() == 1)
      fromAddrHex = "0" + fromAddrHex;

    if (msg.data.size() == 0)
    {
      // Read current outlet status
      sendDebugMessage("CI-V: Command 35 (Read Outlet Status) received");

      // Calculate current status based on relay states
      uint8_t currentStatus = 0;
      if (relay1State && relay2State)
        currentStatus = 0x03; // Both ON
      else if (relay1State && !relay2State)
        currentStatus = 0x01; // Outlet 1 ON, Outlet 2 OFF
      else if (!relay1State && relay2State)
        currentStatus = 0x02; // Outlet 1 OFF, Outlet 2 ON
      else
        currentStatus = 0x00; // Both OFF

      String statusHex = String(currentStatus, HEX);
      statusHex.toUpperCase();
      if (statusHex.length() == 1)
        statusHex = "0" + statusHex;

      String statusResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " 35 " + statusHex + " FD";

      sendDebugMessage("CI-V: Sending outlet status response: " + statusResponse + " (status: 0x" + statusHex + ")");

      if (wsClientConnected)
      {
        wsClient.sendTXT(statusResponse);
        sendDebugMessage("CI-V: Outlet status response sent via WebSocket");
      }
      else
      {
        sendDebugMessage("CI-V: Warning - WebSocket not connected, outlet status response not sent");
      }
    }
    else if (msg.data.size() == 1)
    {
      // Set outlet status - validate value first
      uint8_t newStatus = msg.data[0];
      sendDebugMessage("CI-V: Command 35 (Set Outlet Status) received with status: 0x" + String(newStatus, HEX));

      // Check if value is valid (00-03)
      if (newStatus > 0x03)
      {
        // Invalid value - respond with FA (NAK)
        sendDebugMessage("CI-V: Invalid outlet status: 0x" + String(newStatus, HEX) + " - responding with FA (NAK)");

        String nakResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " FA FD";
        sendDebugMessage("CI-V: Sending FA (NAK) response: " + nakResponse);

        if (wsClientConnected)
        {
          wsClient.sendTXT(nakResponse);
          sendDebugMessage("CI-V: FA (NAK) response sent via WebSocket");
        }
        else
        {
          sendDebugMessage("CI-V: Warning - WebSocket not connected, FA (NAK) response not sent");
        }

        return true; // Invalid command handled with NAK response
      }

      bool newRelay1State = false;
      bool newRelay2State = false;

      switch (newStatus)
      {
      case 0x00: // Both outlets OFF
        newRelay1State = false;
        newRelay2State = false;
        sendDebugMessage("CI-V: Setting both outlets OFF");
        break;
      case 0x01: // Outlet 1 ON, Outlet 2 OFF
        newRelay1State = true;
        newRelay2State = false;
        sendDebugMessage("CI-V: Setting Outlet 1 ON, Outlet 2 OFF");
        break;
      case 0x02: // Outlet 1 OFF, Outlet 2 ON
        newRelay1State = false;
        newRelay2State = true;
        sendDebugMessage("CI-V: Setting Outlet 1 OFF, Outlet 2 ON");
        break;
      case 0x03: // Both outlets ON
        newRelay1State = true;
        newRelay2State = true;
        sendDebugMessage("CI-V: Setting both outlets ON");
        break;
      }

      // Apply the new relay states
      relay1State = newRelay1State;
      relay2State = newRelay2State;

      // Update hardware
      digitalWrite(PIN_RELAY1, relay1State ? HIGH : LOW);
      digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
      digitalWrite(PIN_RELAY2, relay2State ? HIGH : LOW);
      digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);

      // Save to preferences
      preferences.begin("outlet", false);
      preferences.putBool("output1", relay1State);
      preferences.putBool("output2", relay2State);
      preferences.end();

      // Send updated status response (acknowledge the set command)
      String statusHex = String(newStatus, HEX);
      statusHex.toUpperCase();
      if (statusHex.length() == 1)
        statusHex = "0" + statusHex;

      String statusResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " 35 " + statusHex + " FD";

      sendDebugMessage("CI-V: Outlet states updated, sending response: " + statusResponse);

      if (wsClientConnected)
      {
        wsClient.sendTXT(statusResponse);
        sendDebugMessage("CI-V: Outlet status update response sent via WebSocket");
      }
      else
      {
        sendDebugMessage("CI-V: Warning - WebSocket not connected, outlet status update response not sent");
      }

      // Broadcast state change to web clients
      DynamicJsonDocument stateDoc(128);
      stateDoc["type"] = "state";
      stateDoc["output1State"] = relay1State;
      stateDoc["output2State"] = relay2State;
      stateDoc["label1"] = label1Text;
      stateDoc["label2"] = label2Text;
      String stateMsg;
      serializeJson(stateDoc, stateMsg);
      ws.textAll(stateMsg);
      sendDebugMessage("CI-V: Broadcasted state change to web clients");
    }

    return true; // Message handled internally
  }

  // For all other commands, log and handle through SMCIV library
  switch (msg.command)
  {
  case 0x30: // Device type/model
    sendDebugMessage("CI-V: Device type request received");
    break;

  case 0x31: // Antenna/port selection
    if (msg.subCommand == 0x00)
    {
      sendDebugMessage("CI-V: Antenna port read request");
    }
    else if (msg.subCommand >= 1 && msg.subCommand <= 8)
    {
      sendDebugMessage("CI-V: Antenna port set to " + String(msg.subCommand));
    }
    break;

  default:
    sendDebugMessage("CI-V: Unknown command " + String(msg.command, HEX));
    break;
  }

  // Use SMCIV library to handle the message properly (only for non-19 commands)
  sendDebugMessage("CI-V: About to call SMCIV library with: " + hexString);
  civHandler.handleIncomingWsMessage(hexString);
  sendDebugMessage("CI-V: SMCIV library call completed");

  return false; // Message was forwarded to SMCIV library - also forward to physical CI-V port
}

// -------------------------------------------------------------------------
// WebSocket Event Handler (Modified: No echo back to client)
// -------------------------------------------------------------------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, unsigned char *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
  {
    Serial.printf("WebSocket client #%u connected\n", client->id());

    // Send comprehensive reboot banner and debug messages to the newly connected client
    sendDebugMessage("=== DEVICE REBOOT BANNER ===");
    sendDebugMessage("ShackMate Outlet v" + String(VERSION) + " - " + String(AUTHOR));
    sendDebugMessage("Reboot counter: " + String(rebootCounter) + " (device has rebooted " + String(rebootCounter) + " times)");

    // Get reset reason for web debug
    esp_reset_reason_t resetReason = esp_reset_reason();
    String resetReasonStr = "";
    switch (resetReason)
    {
    case ESP_RST_POWERON:
      resetReasonStr = "Power-on reset";
      break;
    case ESP_RST_EXT:
      resetReasonStr = "External reset";
      break;
    case ESP_RST_SW:
      resetReasonStr = "Software restart";
      break;
    case ESP_RST_PANIC:
      resetReasonStr = "Software panic";
      break;
    case ESP_RST_INT_WDT:
      resetReasonStr = "Interrupt watchdog";
      break;
    case ESP_RST_TASK_WDT:
      resetReasonStr = "Task watchdog";
      break;
    case ESP_RST_WDT:
      resetReasonStr = "Other watchdog";
      break;
    case ESP_RST_DEEPSLEEP:
      resetReasonStr = "Deep sleep reset";
      break;
    case ESP_RST_BROWNOUT:
      resetReasonStr = "Brownout reset";
      break;
    case ESP_RST_SDIO:
      resetReasonStr = "SDIO reset";
      break;
    default:
      resetReasonStr = "Unknown reset";
      break;
    }
    sendDebugMessage("Last reset reason: " + resetReasonStr);
    sendDebugMessage("Device started at: " + String(bootTime) + "ms");
    sendDebugMessage("Current uptime: " + String(millis()) + "ms");
    sendDebugMessage("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
    sendDebugMessage("CPU frequency: " + String(ESP.getCpuFreqMHz()) + "MHz");
    sendDebugMessage("Device IP: " + deviceIP);
    sendDebugMessage("Web client connected! Debug messages enabled.");
    sendDebugMessage("UDP listener active on port 4210. Waiting for 'ShackMate,IP,Port' messages...");
    if (connectedServerIP.length() > 0)
    {
      sendDebugMessage("Last discovered IP: " + connectedServerIP + ":" + String(connectedServerPort));
      if (wsClientConnected)
      {
        sendDebugMessage("WebSocket client status: CONNECTED to " + connectedServerIP);
      }
      else
      {
        sendDebugMessage("WebSocket client status: DISCONNECTED (last target: " + connectedServerIP + ")");
      }
    }
    else
    {
      sendDebugMessage("No ShackMate devices discovered yet.");
    }
    sendDebugMessage("=== END REBOOT BANNER ===");
    // 1) Send current state (relay + labels)
    {
      DynamicJsonDocument stateDoc(128);
      stateDoc["type"] = "state";
      stateDoc["output1State"] = relay1State;
      stateDoc["output2State"] = relay2State;
      stateDoc["label1"] = label1Text;
      stateDoc["label2"] = label2Text;
      String stateMsg;
      serializeJson(stateDoc, stateMsg);
      client->text(stateMsg);
    }
    // 2) Immediately send full status (including sensors & timestamp)
    {
      // Build timestamp
      struct tm ti;
      char timeStr[32];
      if (getLocalTime(&ti))
      {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &ti);
      }
      else
      {
        strcpy(timeStr, "TIME_NOT_SET");
      }
      // Read sensors
      float lux = analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
      float amps = hlw.getCurrent();
      float rawV = hlw.getVoltage();
      float volts = rawV * voltageCalibrationFactor;
      float watts = hlw.getActivePower();
      // Assemble status JSON
      DynamicJsonDocument statusDoc(512);
      statusDoc["type"] = "status";
      statusDoc["timestamp"] = timeStr;
      statusDoc["output1State"] = relay1State;
      statusDoc["output2State"] = relay2State;
      statusDoc["label1"] = label1Text;
      statusDoc["label2"] = label2Text;
      statusDoc["lux"] = lux;
      statusDoc["amps"] = amps;
      statusDoc["volts"] = volts;
      statusDoc["watts"] = watts;
      // CI-V Server status
      statusDoc["civServerConnected"] = wsClientConnected;
      statusDoc["civServerEverConnected"] = wsClientEverConnected;
      statusDoc["civServerIP"] = connectedServerIP;
      statusDoc["civServerPort"] = connectedServerPort;
      // Device ID and CIV Address
      statusDoc["deviceId"] = deviceId;
      statusDoc["civAddress"] = civAddress;
      // Include UDP port and PSRAM size
      statusDoc["udpPort"] = UDP_PORT;
      statusDoc["psramSize"] = getPsramSize();
      // Formatted uptime
      statusDoc["uptime"] = getUptime();
      // Include calibration multipliers
      statusDoc["currentMultiplier"] = hlw.getCurrentMultiplier();
      statusDoc["voltageMultiplier"] = hlw.getVoltageMultiplier();
      statusDoc["powerMultiplier"] = hlw.getPowerMultiplier();
      String statusMsg;
      serializeJson(statusDoc, statusMsg);
      client->text(statusMsg);
    }
    break;
  }
  case WS_EVT_DISCONNECT:
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
    break;
  case WS_EVT_DATA:
  {
    String msg = "";
    for (size_t i = 0; i < len; i++)
    {
      msg += (char)data[i];
    }
    // Do not echo any info back to the WebSocket client.
    msg.trim();
    //  msg.replace(" ", "");  // removed so that JSON strings can contain spaces
    // Handle single-output commands
    if (msg.startsWith("{"))
    {
      DynamicJsonDocument j(128);
      if (deserializeJson(j, msg) == DeserializationError::Ok)
      {
        // Handle single-output commands
        if (j.containsKey("command") && j.containsKey("value"))
        {
          const char *cmd = j["command"] | "";
          bool value = j["value"] | false;

          // Apply and persist new output state
          preferences.begin("outlet", false);
          if (strcmp(cmd, "output1") == 0)
          {
            relay1State = value;
            digitalWrite(PIN_RELAY1, relay1State ? HIGH : LOW);
            digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
            preferences.putBool("output1", relay1State);
          }
          else if (strcmp(cmd, "output2") == 0)
          {
            relay2State = value;
            digitalWrite(PIN_RELAY2, relay2State ? HIGH : LOW);
            digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);
            preferences.putBool("output2", relay2State);
          }
          preferences.end();

          // Immediately broadcast updated status
          {
            // Time stamp
            struct tm ti;
            char timeStr[32];
            if (getLocalTime(&ti))
            {
              strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &ti);
            }
            else
            {
              strcpy(timeStr, "TIME_NOT_SET");
            }

            // Sensor readings
            float lux = analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
            float amps = hlw.getCurrent();
            float rawV = hlw.getVoltage();
            float volts = rawV * voltageCalibrationFactor;
            float watts = hlw.getActivePower();

            // Build JSON
            DynamicJsonDocument resp(512);
            resp["type"] = "status";
            resp["timestamp"] = timeStr;
            resp["output1State"] = relay1State;
            resp["output2State"] = relay2State;
            resp["lux"] = lux;
            resp["amps"] = amps;
            resp["volts"] = volts;
            resp["watts"] = watts;
            // CI-V Server status
            resp["civServerConnected"] = wsClientConnected;
            resp["civServerEverConnected"] = wsClientEverConnected;
            resp["civServerIP"] = connectedServerIP;
            resp["civServerPort"] = connectedServerPort;
            String payload;
            serializeJson(resp, payload);
            ws.textAll(payload);
          }

          break;
        }
        // (Retain legacy relay control for "cmd":"relay" messages)
        if (j["cmd"] == "relay")
        {
          int r = j["relay"];
          // Determine action
          const char *action = j["action"] | "";
          bool onCmd = (strcmp(action, "on") == 0);
          int pin = (r == 1 ? PIN_RELAY1 : PIN_RELAY2);
          int led = (r == 1 ? PIN_RELAY1_LED : PIN_RELAY2_LED);
          // Apply relay state
          digitalWrite(pin, onCmd ? HIGH : LOW);
          digitalWrite(led, onCmd ? LOW : HIGH);
          // Save state to Preferences
          preferences.begin("outlet", false);
          if (r == 1)
          {
            relay1State = onCmd;
            preferences.putBool("output1", relay1State);
          }
          else
          {
            relay2State = onCmd;
            preferences.putBool("output2", relay2State);
          }
          preferences.end();
          break;
        }
        // Handle label-change commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "setLabel") == 0)
        {
          int outlet = j["outlet"] | 0;
          const char *newText = j["text"] | "";
          preferences.begin("labels", false);
          if (outlet == 1)
          {
            preferences.putString("label1", newText);
            strncpy(label1Text, newText, sizeof(label1Text));
          }
          else
          {
            preferences.putString("label2", newText);
            strncpy(label2Text, newText, sizeof(label2Text));
          }
          preferences.end();

          DynamicJsonDocument resp(128);
          resp["type"] = "labels";
          resp["outlet"] = outlet;
          resp["text"] = newText;
          String out;
          serializeJson(resp, out);
          ws.textAll(out);
          break;
        }

        // Handle reboot and restore commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "reboot") == 0)
        {
          client->text("{\"type\":\"info\",\"msg\":\"Rebooting device...\"}");
          delay(250);
          ESP.restart();
          break;
        }
        if (j.containsKey("command") && strcmp(j["command"] | "", "restore") == 0)
        {
          client->text("{\"type\":\"info\",\"msg\":\"Erasing WiFi credentials completely...\"}");
          // Complete WiFi reset to ensure captive portal activation
          WiFiManager wm;
          wm.resetSettings(); // Clear WiFiManager's saved credentials
          WiFi.disconnect(true, true); // Erase WiFi creds and reset
          WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
          delay(100);
          WiFi.mode(WIFI_STA);         // Set to station mode
          sendDebugMessage("WiFi credentials and WiFiManager settings completely erased");
          sendDebugMessage("Captive portal WILL activate on next boot");
          delay(500);
          ESP.restart();
          break;
        }
        if (j.containsKey("command") && strcmp(j["command"] | "", "resetRebootCounter") == 0)
        {
          preferences.begin("system", false);
          preferences.putUInt("rebootCount", 0);
          preferences.end();
          rebootCounter = 0;
          client->text("{\"type\":\"info\",\"msg\":\"Reboot counter reset to 0\"}");
          sendDebugMessage("Reboot counter manually reset to 0");
          break;
        }

        // Handle test captive portal LED alternating
        if (j.containsKey("command") && strcmp(j["command"] | "", "testCaptivePortal") == 0)
        {
          bool enable = j["enable"] | false;
          if (enable)
          {
            captivePortalActive = true;
            lastLedToggleTime = millis(); // Initialize timing
            ledAlternateState = false;
            digitalWrite(PIN_RELAY1_LED, LOW);  // Turn on LED1
            digitalWrite(PIN_RELAY2_LED, HIGH); // Turn off LED2
            client->text("{\"type\":\"info\",\"msg\":\"Captive Portal LED test mode ENABLED\"}");
            sendDebugMessage("Captive Portal LED test mode activated");
            Serial.println("DEBUG: Test mode - captivePortalActive=" + String(captivePortalActive) + 
                          ", lastLedToggleTime=" + String(lastLedToggleTime) + 
                          ", ledAlternateState=" + String(ledAlternateState));
          }
          else
          {
            captivePortalActive = false;
            // Restore normal LED operation based on relay states
            digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
            digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);
            client->text("{\"type\":\"info\",\"msg\":\"Captive Portal LED test mode DISABLED\"}");
            sendDebugMessage("Captive Portal LED test mode deactivated");
            Serial.println("DEBUG: Test mode disabled - normal LED operation restored");
          }
          break;
        }

        // Handle Device ID configuration
        if (j.containsKey("command") && strcmp(j["command"] | "", "setDeviceId") == 0)
        {
          uint8_t newDeviceId = j["deviceId"] | 1;
          if (newDeviceId >= 1 && newDeviceId <= 4)
          {
            // Only update if the device ID is actually changing
            if (deviceId != newDeviceId)
            {
              deviceId = newDeviceId;
              civAddress = "B" + String(deviceId - 1); // ID 1->B0, 2->B1, 3->B2, 4->B3

              // Save to preferences
              preferences.begin("config", false);
              preferences.putUChar("deviceId", deviceId);
              preferences.putString("civAddress", civAddress);
              preferences.end();

              // Update CI-V handler with new address
              uint8_t civAddrByte = getCivAddressByte();
              civHandler.begin(&wsClient, &civAddrByte);

              client->text("{\"type\":\"info\",\"msg\":\"Device ID set to " + String(deviceId) + " (CIV: " + civAddress + ")\"}");
              sendDebugMessage("Device ID changed to " + String(deviceId) + ", CIV Address: " + civAddress + " (0x" + String(civAddrByte, HEX) + ")");
            }
            else
            {
              // Device ID unchanged - just acknowledge without debug spam
              client->text("{\"type\":\"info\",\"msg\":\"Device ID confirmed as " + String(deviceId) + " (CIV: " + civAddress + ")\"}");
              // Debug message removed to reduce spam
            }
          }
          else
          {
            client->text("{\"type\":\"error\",\"msg\":\"Invalid Device ID. Must be 1-4.\"}");
            sendDebugMessage("Invalid Device ID received: " + String(newDeviceId));
          }
          break;
        }

        // Handle ping/pong for connection testing
        if (j.containsKey("type"))
        {
          const char *msgType = j["type"] | "";
          if (strcmp(msgType, "ping") == 0)
          {
            // Respond to ping with pong
            client->text("{\"type\":\"pong\",\"timestamp\":" + String(millis()) + "}");
            sendDebugMessage("Received ping from client, sent pong response");
            break;
          }
          else if (strcmp(msgType, "pong") == 0)
          {
            // Received pong response
            // Pong received - connection is healthy (no debug message to reduce spam)
            // sendDebugMessage("Received pong from client (connection healthy)");
            break;
          }
        }
      }
    }

    // Handle CI-V hex messages (must be hex format, not JSON)
    if (!msg.startsWith("{") && msg.length() > 0)
    {
      sendDebugMessage("CI-V: Received hex message: " + msg);
      sendDebugMessage("CI-V: Message length: " + String(msg.length()) + " characters");

      // Parse and validate CI-V message
      CivMessage civMsg = parseCivMessage(msg);

      sendDebugMessage("CI-V: Parse result - valid: " + String(civMsg.valid ? "true" : "false"));

      if (civMsg.valid)
      {
        // Check if message is addressed to us
        if (isCivMessageForUs(civMsg))
        {
          sendDebugMessage("CI-V: Message IS for us - processing...");
          // Process the CI-V message and check if it was handled internally
          bool handledInternally = processCivMessage(civMsg);

          // Only forward to physical CI-V port if NOT handled internally
          if (!handledInternally)
          {
            sendDebugMessage("CI-V: Message not handled internally - forwarding to physical CI-V port");

            // Also forward to physical CI-V port if available
            String cleanHex = msg;
            cleanHex.replace(" ", "");
            int byteCount = cleanHex.length() / 2;
            uint8_t buffer[128];

            if (byteCount > 0 && byteCount <= 128)
            {
              for (int i = 0; i < byteCount; i++)
              {
                String byteStr = cleanHex.substring(i * 2, i * 2 + 2);
                buffer[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
              }

              // Forward to physical CI-V port via Serial2
              Serial2.write(buffer, byteCount);
              Serial.print("Forwarded to CI-V Port: ");
              for (int i = 0; i < byteCount; i++)
              {
                Serial.printf("%02X ", buffer[i]);
              }
              Serial.println();
            }
          }
          else
          {
            sendDebugMessage("CI-V: Message handled internally - NOT forwarding to physical CI-V port");
          }
        }
        else
        {
          // Message not for us - debug message removed to reduce spam
        }
      }
      else
      {
        sendDebugMessage("CI-V: Invalid message format - ignoring");
      }
    }
    else if (msg.startsWith("{"))
    {
      sendDebugMessage("CI-V: JSON message received on CI-V channel - ignoring");
    }
    else
    {
      sendDebugMessage("CI-V: Empty message received - ignoring");
    }

    break;
  }
  default:
    break;
  }
}

// -------------------------------------------------------------------------
// HTTP Server Handlers using LittleFS and template processing
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// JSON Data Handler (consolidated for /index/data)
void handleDataJson(AsyncWebServerRequest *request)
{
  float lux = analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
  float amps = hlw.getCurrent();
  float volts = hlw.getVoltage();
  float watts = hlw.getActivePower();
  String json = String("{\"lux\":") + String(lux, 1) + ",\"amps\":" + String(amps, 2) + ",\"volts\":" + String(volts, 1) + ",\"watts\":" + String(watts, 0) + "}";
  request->send(200, "application/json", json);
}

void handleRoot(AsyncWebServerRequest *request)
{
  String page = loadFile("/index.html");
  if (page == "")
  {
    request->send(500, "text/plain", "Error loading page");
    return;
  }
  page = processTemplate(page);
  request->send(200, "text/html", page);
}

void handleSaveConfig(AsyncWebServerRequest *request)
{
  if (request->hasArg("tcpPort"))
    tcpPort = request->arg("tcpPort");
  preferences.begin("config", false);
  preferences.putString("tcp_port", tcpPort);
  preferences.end();
  request->send(200, "text/html", "<html><body><h1>Configuration Saved</h1><p>The device will now reboot.</p></body></html>");
  delay(2000);
  ESP.restart();
}

void handleRestoreConfig(AsyncWebServerRequest *request)
{
  // Complete WiFi reset to ensure captive portal activation
  WiFiManager wm;
  wm.resetSettings(); // Clear WiFiManager's saved credentials
  WiFi.disconnect(true, true); // Erase WiFi creds and reset
  WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
  delay(100);
  WiFi.mode(WIFI_STA);         // Set to station mode
  request->send(200, "text/html", "<html><body><h1>WiFi Completely Erased</h1><p>Captive portal WILL activate on reboot.</p></body></html>");
  delay(2000);
  ESP.restart();
}

// -------------------------------------------------------------------------
// UDP Discovery and WebSocket Client Functions
// -------------------------------------------------------------------------

void handleUdpDiscovery()
{
  int packetSize = udpListener.parsePacket();
  if (packetSize)
  {
    // Read the packet
    char packetBuffer[256];
    int len = udpListener.read(packetBuffer, sizeof(packetBuffer) - 1);
    packetBuffer[len] = '\0';

    String message = String(packetBuffer);
    Serial.printf("UDP packet received: %s\n", message.c_str());

    // Only send UDP debug message if we're not connected (to reduce spam)
    if (!wsClientConnected)
    {
      sendDebugMessage("UDP received: " + message);
    }

    // Parse message format: 'ShackMate,10.146.1.118,4000'
    if (message.indexOf("ShackMate") >= 0)
    {
      Serial.printf("Processing ShackMate message: %s\n", message.c_str());

      // Simple parsing - find the commas
      int firstComma = message.indexOf(',');
      int secondComma = message.indexOf(',', firstComma + 1);

      if (firstComma > 0 && secondComma > firstComma)
      {
        String remoteIP = message.substring(firstComma + 1, secondComma);
        String remotePort = message.substring(secondComma + 1);

        // Clean up any whitespace
        remoteIP.trim();
        remotePort.trim();

        if (remoteIP.length() > 0 && remotePort.length() > 0)
        {
          uint16_t port = remotePort.toInt();

          // Don't connect to ourselves
          if (remoteIP != deviceIP)
          {
            // If we're already connected to this exact server, ignore UDP messages completely
            if (wsClientConnected && connectedServerIP == remoteIP && connectedServerPort == port)
            {
              // Skip all processing - we're already connected to this server
              // No debug message, no status updates, no variable changes
              return;
            }

            // Check if this is a new/different server discovery
            bool isNewDiscovery = (connectedServerIP != remoteIP || connectedServerPort != port);

            // Only process if this is actually a new discovery or we're not connected
            if (isNewDiscovery || !wsClientConnected)
            {
              sendDebugMessage("Parsed IP: " + remoteIP + ", Port: " + remotePort);
              Serial.printf("Found ShackMate device at %s:%d\n", remoteIP.c_str(), port);

              // Update stored IP for display
              connectedServerIP = remoteIP;
              connectedServerPort = port;

              // Reset everConnected flag if this is a new server
              if (isNewDiscovery)
              {
                wsClientEverConnected = false;
                sendDebugMessage("New server discovered - reset everConnected flag");
              }

              sendDebugMessage("Updated display IP: " + connectedServerIP);

              // Send discovery status update to web clients only if not connected
              if (!wsClientConnected)
              {
                String statusMsg = "{\"type\":\"status\",\"civServerConnected\":false,\"civServerEverConnected\":" + String(wsClientEverConnected ? "true" : "false") + ",\"civServerIP\":\"" + connectedServerIP + "\",\"civServerPort\":" + String(connectedServerPort) + "}";
                ws.textAll(statusMsg);
                sendDebugMessage("Sent discovery status update to web clients");
              }
              else
              {
                sendDebugMessage("Skipping discovery status update - already connected");
              }

              // Only connect if not already connected and cooldown period has passed
              unsigned long now = millis();
              if (!wsClientConnected && now - lastConnectionAttempt >= CONNECTION_COOLDOWN)
              {
                sendDebugMessage("Initiating WebSocket client connection to discovered server...");
                lastConnectionAttempt = now;
                connectToShackMateServer(remoteIP, port);
              }
              else if (!wsClientConnected)
              {
                unsigned long remaining = (CONNECTION_COOLDOWN - (now - lastConnectionAttempt)) / 1000;
                sendDebugMessage("Connection cooldown active - " + String(remaining) + " seconds remaining");
              }
            }
          }
          else
          {
            sendDebugMessage("Ignoring self-discovery");
          }
        }
      }
      else
      {
        sendDebugMessage("Failed to parse UDP message");
      }
    }
  }
}

void connectToShackMateServer(String ip, uint16_t port)
{
  Serial.printf("Connecting to ShackMate server at %s:%d\n", ip.c_str(), port);

  // Send debug info to web clients
  sendDebugMessage("ATTEMPTING WebSocket client connection to: " + ip + ":" + String(port));

  // Check if we're already connected to this exact server
  if (wsClientConnected && connectedServerIP == ip && connectedServerPort == port)
  {
    sendDebugMessage("Already connected to " + ip + ":" + String(port) + " - aborting connection attempt");
    return;
  }

  // Disconnect any existing connection safely
  if (wsClientConnected)
  {
    sendDebugMessage("Disconnecting existing WebSocket connection");
    wsClient.disconnect();
    wsClientConnected = false;
    delay(500); // Give more time for cleanup
  }

  // Connect to the server with enhanced error handling
  sendDebugMessage("Starting WebSocket client connection...");

  try
  {
    wsClient.begin(ip, port, "/ws");
    wsClient.onEvent(onWebSocketClientEvent);
    wsClient.setReconnectInterval(10000);     // Longer reconnect interval for stability
    wsClient.enableHeartbeat(15000, 3000, 2); // Enable heartbeat

    // Update connection info immediately
    connectedServerIP = ip;
    connectedServerPort = port;

    sendDebugMessage("WebSocket client setup complete for: " + connectedServerIP + ":" + String(connectedServerPort));

    // Send immediate status update showing discovery
    String statusMsg = "{\"type\":\"status\",\"civServerConnected\":false,\"civServerEverConnected\":" + String(wsClientEverConnected ? "true" : "false") + ",\"civServerIP\":\"" + connectedServerIP + "\",\"civServerPort\":" + String(connectedServerPort) + "}";
    ws.textAll(statusMsg);
    sendDebugMessage("Sent discovery status to web clients");
  }
  catch (const std::exception &e)
  {
    Serial.println("Exception during WebSocket begin");
    sendDebugMessage("ERROR: Exception starting WebSocket client connection");
  }
  catch (...)
  {
    Serial.println("Unknown exception during WebSocket begin");
    sendDebugMessage("ERROR: Unknown exception starting WebSocket client connection");
  }
}

void onWebSocketClientEvent(WStype_t type, uint8_t *payload, size_t length)
{
  try
  {
    switch (type)
    {
    case WStype_DISCONNECTED:
      Serial.printf("WebSocket client disconnected from %s:%d\n", connectedServerIP.c_str(), connectedServerPort);
      wsClientConnected = false;
      // Send detailed debug info to web clients about the disconnection
      sendDebugMessage("WebSocket client DISCONNECTED from " + connectedServerIP + " (connection lost)");
      sendDebugMessage("Will attempt reconnection on next UDP discovery...");
      // Broadcast status update to web clients
      {
        DynamicJsonDocument statusDoc(256);
        statusDoc["type"] = "status";
        statusDoc["civServerConnected"] = false;
        statusDoc["civServerEverConnected"] = wsClientEverConnected;
        statusDoc["civServerIP"] = connectedServerIP; // Keep the IP visible
        statusDoc["civServerPort"] = connectedServerPort;
        String statusMsg;
        serializeJson(statusDoc, statusMsg);
        ws.textAll(statusMsg);
        sendDebugMessage("Broadcasted DISCONNECTED status to web clients");
      }
      break;

    case WStype_CONNECTED:
      Serial.printf("WebSocket client connected to %s:%d\n", connectedServerIP.c_str(), connectedServerPort);
      wsClientConnected = true;
      wsClientEverConnected = true;     // Mark that we've successfully connected at least once
      lastWebSocketActivity = millis(); // Track connection time
      lastPingSent = millis();          // Initialize ping timer
      // Send debug info to web clients
      sendDebugMessage("WebSocket client status: CONNECTED to " + connectedServerIP);
      sendDebugMessage("Broadcasting CONNECTED status to web clients...");
      // Broadcast status update to web clients
      {
        DynamicJsonDocument statusDoc(256);
        statusDoc["type"] = "status";
        statusDoc["civServerConnected"] = true;
        statusDoc["civServerEverConnected"] = true;
        statusDoc["civServerIP"] = connectedServerIP;
        statusDoc["civServerPort"] = connectedServerPort;
        String statusMsg;
        serializeJson(statusDoc, statusMsg);
        ws.textAll(statusMsg);
        sendDebugMessage("Broadcasted CONNECTED status: " + statusMsg);
      }
      break;

    case WStype_ERROR:
      sendDebugMessage("WebSocket client error occurred");
      Serial.printf("WebSocket client error\n");
      wsClientConnected = false;
      break;

    case WStype_PING:
      lastWebSocketActivity = millis(); // Track ping activity
      // Ping/pong messages suppressed to reduce debug spam
      break;

    case WStype_PONG:
      lastWebSocketActivity = millis(); // Track pong activity
      // Ping/pong messages suppressed to reduce debug spam
      break;

    case WStype_TEXT:
    {
      lastWebSocketActivity = millis(); // Track message activity
      String message = String((char *)payload);
      Serial.printf("WebSocket client received: %s\n", message.c_str());

      // Check if this is a CI-V hex message (not JSON)
      if (!message.startsWith("{") && message.length() > 0)
      {
        sendDebugMessage("CI-V: WebSocket CLIENT received hex message: " + message);
        sendDebugMessage("CI-V: Message length: " + String(message.length()) + " characters");

        // Parse and validate CI-V message
        CivMessage civMsg = parseCivMessage(message);

        sendDebugMessage("CI-V: Parse result - valid: " + String(civMsg.valid ? "true" : "false"));

        if (civMsg.valid)
        {
          // Check if message is addressed to us
          if (isCivMessageForUs(civMsg))
          {
            sendDebugMessage("CI-V: Message IS for us - processing...");
            // Process the CI-V message and check if it was handled internally
            bool handledInternally = processCivMessage(civMsg);

            // Only forward to physical CI-V port if NOT handled internally
            if (!handledInternally)
            {
              sendDebugMessage("CI-V: Message not handled internally - forwarding to physical CI-V port");

              // Forward to physical CI-V port if available
              String cleanHex = message;
              cleanHex.replace(" ", "");
              int byteCount = cleanHex.length() / 2;
              uint8_t buffer[128];

              if (byteCount > 0 && byteCount <= 128)
              {
                for (int i = 0; i < byteCount; i++)
                {
                  String byteStr = cleanHex.substring(i * 2, i * 2 + 2);
                  buffer[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
                }

                // Forward to physical CI-V port via Serial2
                Serial2.write(buffer, byteCount);
                Serial.print("Forwarded to CI-V Port: ");
                for (int i = 0; i < byteCount; i++)
                {
                  Serial.printf("%02X ", buffer[i]);
                }
                Serial.println();
              }
            }
            else
            {
              sendDebugMessage("CI-V: Message handled internally - NOT forwarding to physical CI-V port");
            }
          }
          else
          {
            // Message not for us - debug message removed to reduce spam
          }
        }
        else
        {
          sendDebugMessage("CI-V: Invalid message format - ignoring");
        }
      }
      else if (message.startsWith("{"))
      {
        // Parse and handle JSON messages from the server
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, message) == DeserializationError::Ok)
        {
          String msgType = doc["type"] | "";

          if (msgType == "ping")
          {
            // Respond to ping with pong - DISABLED to prevent server log spam
            // sendDebugMessage("Received ping from server, sending pong");
            // wsClient.sendTXT("{\"type\":\"pong\",\"timestamp\":" + String(millis()) + "}");
          }
          else if (msgType == "pong")
          {
            // Received pong response to our ping
            // Pong received - connection is healthy (no debug message to reduce spam)
            // sendDebugMessage("Received pong from server (connection healthy)");
          }
          else if (msgType == "status")
          {
            // Handle status updates from remote device
            Serial.println("Received status update from remote ShackMate device");
          }
          else if (msgType == "command")
          {
            // Handle commands from remote device
            Serial.println("Received command from remote ShackMate device");
          }
        }
      }
      else
      {
        sendDebugMessage("CI-V: Empty message received from WebSocket client - ignoring");
      }
    }
    break;

    default:
      break;
    }
  }
  catch (...)
  {
    Serial.println("Exception in WebSocket event handler");
    wsClientConnected = false;
  }
}

void disconnectWebSocketClient()
{
  if (wsClientConnected)
  {
    wsClient.disconnect();
    wsClientConnected = false;
    connectedServerIP = "";
    connectedServerPort = 0;
    Serial.println("WebSocket client disconnected");
  }
}

// -------------------------------------------------------------------------
// OTA Task (runs on Core 1)
// -------------------------------------------------------------------------
// This task handles OTA updates on Core 1, separate from the main loop
// which runs on Core 0. This improves reliability and responsiveness
// during OTA updates by isolating the OTA handling from critical
// system functions like WebSocket handling and relay control.
void otaTask(void *pvParameters)
{
  Serial.println("OTA Task started on Core 1");

  for (;;)
  {
    ArduinoOTA.handle();
    vTaskDelay(10 / portTICK_PERIOD_MS); // Small delay to prevent watchdog issues
  }
}

// -------------------------------------------------------------------------
// Setup Function
// -------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(2000); // Longer delay for stability

  bootTime = millis();

  Serial.println();
  Serial.println("================================================");
  Serial.println("        DEVICE REBOOT DETECTED");
  Serial.println("================================================");
  Serial.println("=== ShackMate Outlet Starting ===");
  Serial.println("Version: " + String(VERSION));
  Serial.println("Boot time: " + String(bootTime) + "ms");
  Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");

  // Get reset reason for better debugging
  esp_reset_reason_t resetReason = esp_reset_reason();
  String resetReasonStr = "";
  switch (resetReason)
  {
  case ESP_RST_POWERON:
    resetReasonStr = "Power-on reset";
    break;
  case ESP_RST_EXT:
    resetReasonStr = "External reset";
    break;
  case ESP_RST_SW:
    resetReasonStr = "Software restart";
    break;
  case ESP_RST_PANIC:
    resetReasonStr = "Software panic";
    break;
  case ESP_RST_INT_WDT:
    resetReasonStr = "Interrupt watchdog";
    break;
  case ESP_RST_TASK_WDT:
    resetReasonStr = "Task watchdog";
    break;
  case ESP_RST_WDT:
    resetReasonStr = "Other watchdog";
    break;
  case ESP_RST_DEEPSLEEP:
    resetReasonStr = "Deep sleep reset";
    break;
  case ESP_RST_BROWNOUT:
    resetReasonStr = "Brownout reset";
    break;
  case ESP_RST_SDIO:
    resetReasonStr = "SDIO reset";
    break;
  default:
    resetReasonStr = "Unknown reset";
    break;
  }
  Serial.println("Reset reason: " + resetReasonStr);
  Serial.println("Chip model: " + String(ESP.getChipModel()));
  Serial.println("Chip revision: " + String(ESP.getChipRevision()));
  Serial.println("CPU frequency: " + String(ESP.getCpuFreqMHz()) + "MHz");
  Serial.println("Flash size: " + String(ESP.getFlashChipSize()) + " bytes");
  Serial.println("================================================");

  // Configure relay GPIOs
  pinMode(PIN_RELAY1, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  pinMode(PIN_RELAY1_LED, OUTPUT);
  pinMode(PIN_RELAY2_LED, OUTPUT);

  // Configure LUX ADC pin as input
  pinMode(PIN_LUX_ADC, INPUT);

  // Set ADC resolution to 12-bit (0-4095 range)
  analogSetAttenuation(ADC_11db);                 // Global attenuation setting
  analogSetPinAttenuation(PIN_LUX_ADC, ADC_11db); // Pin-specific attenuation
  analogReadResolution(12);                       // Set resolution to 12-bit (0-4095)

  // Initialize HLW8012 power monitoring chip
  hlw.begin(PIN_HLW_CF, PIN_HLW_CF1, PIN_HLW_SEL, HIGH, true);
  hlw.setResistors(CURRENT_RESISTOR, VOLTAGE_DIVIDER, 1000000);

  // Set up interrupts for pulse counting
  setInterrupts();

  Serial.println("HLW8012 power monitoring initialized");
  Serial.println("CF Pin: " + String(PIN_HLW_CF));
  Serial.println("CF1 Pin: " + String(PIN_HLW_CF1));
  Serial.println("SEL Pin: " + String(PIN_HLW_SEL));
  Serial.println("Current Resistor: " + String(CURRENT_RESISTOR, 6) + " ohms");
  Serial.println("Voltage Divider: " + String(VOLTAGE_DIVIDER, 1));
  hlw.begin(
      PIN_HLW_CF,
      PIN_HLW_CF1,
      PIN_HLW_SEL,
      3,     // change mode every 3 pulses
      false, // disable SEL inversion (enable CF1 interrupts)
      500000 // timeout in microseconds
  );
  // Calibrate resistors and voltage divider
  hlw.setResistors(CURRENT_RESISTOR, VOLTAGE_DIVIDER, 1.0f);
  // Direct hardware calibration: VOLTAGE_DIVIDER is pre-scaled, so skip expectedVoltage().
  // hlw.expectedVoltage(123.8f);

  // Load calibration multipliers from preferences (if available)
  preferences.begin("calibration", true);
  float storedCurrentMultiplier = preferences.getFloat("currentMultiplier", 0.0f);
  float storedVoltageMultiplier = preferences.getFloat("voltageMultiplier", 0.0f);
  float storedPowerMultiplier = preferences.getFloat("powerMultiplier", 0.0f);
  preferences.end();

  if (storedCurrentMultiplier > 0 && storedVoltageMultiplier > 0 && storedPowerMultiplier > 0)
  {
    hlw.setCurrentMultiplier(storedCurrentMultiplier);
    hlw.setVoltageMultiplier(storedVoltageMultiplier);
    hlw.setPowerMultiplier(storedPowerMultiplier);
    voltageCalibrated = true;
    Serial.println("Loaded calibration multipliers from preferences.");
  }

  // Enable HLW8012 pulse interrupts for CF and CF1
  setInterrupts();
  Serial.println("HLW8012 interrupts enabled for power monitoring");

  // Configure hardware buttons
  pinMode(PIN_BUTTON1, INPUT_PULLDOWN);
  pinMode(PIN_BUTTON2, INPUT_PULLDOWN);

  if (!SPIFFS.begin(false))
  {
    Serial.println("SPIFFS mount failed");
    return;
  }
  else
  {
    Serial.println("SPIFFS mounted successfully");

    // Check if index.html exists
    File indexFile = SPIFFS.open("/index.html", "r");
    if (indexFile)
    {
      Serial.println("DEBUG: index.html found, size: " + String(indexFile.size()) + " bytes");
      indexFile.close();
    }
    else
    {
      Serial.println("WARNING: index.html not found in SPIFFS!");
    }

    // List all files in SPIFFS for debugging
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    Serial.println("SPIFFS files:");
    while (file)
    {
      Serial.println("  " + String(file.name()) + " (" + String(file.size()) + " bytes)");
      file = root.openNextFile();
    }
  }

  // Load persisted relay states
  preferences.begin("outlet", true);
  relay1State = preferences.getBool("output1", false);
  relay2State = preferences.getBool("output2", false);
  preferences.end();
  // Apply saved states
  digitalWrite(PIN_RELAY1, relay1State ? HIGH : LOW);
  digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
  digitalWrite(PIN_RELAY2, relay2State ? HIGH : LOW);
  digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);

  preferences.begin("labels", true);
  String s1 = preferences.getString("label1", "Outlet 1");
  String s2 = preferences.getString("label2", "Outlet 2");
  preferences.end();

  // Load and increment reboot counter for debugging
  preferences.begin("system", false);
  rebootCounter = preferences.getUInt("rebootCount", 0);
  rebootCounter++;
  preferences.putUInt("rebootCount", rebootCounter);
  preferences.end();

  Serial.println("Reboot counter: " + String(rebootCounter));

  // Load Device ID and CIV Address configuration
  preferences.begin("config", false);
  deviceId = preferences.getUChar("deviceId", 1);         // Default to 1
  civAddress = preferences.getString("civAddress", "B0"); // Default to B0
  // Validate and correct if needed
  if (deviceId < 1 || deviceId > 4)
  {
    deviceId = 1;
    civAddress = "B0";
  }
  // Ensure CIV address matches Device ID
  civAddress = "B" + String(deviceId - 1);
  preferences.end();

  Serial.println("Device ID: " + String(deviceId) + ", CIV Address: " + civAddress);

  // Initialize CI-V handler with WebSocket client and Device ID
  uint8_t civAddrByte = getCivAddressByte();
  civHandler.begin(&wsClient, &civAddrByte);
  Serial.println("CI-V handler initialized with address: 0x" + String(civAddrByte, HEX));

  s1.toCharArray(label1Text, sizeof(label1Text));
  s2.toCharArray(label2Text, sizeof(label2Text));

  // Declare WiFiManagerParameter at function scope to ensure it remains valid
  String storedPort = "4000";
  preferences.begin("config", false);
  storedPort = preferences.getString("tcp_port", storedPort);
  preferences.end();

  WiFiManagerParameter customPortParam("port", "WebSocket Port", storedPort.c_str(), 6);

  WiFiManager wifiManager;
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager)
                            {
    Serial.println("Entered configuration mode (AP mode)");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    captivePortalActive = true; // Set flag for LED alternating
    lastLedToggleTime = millis(); // Initialize timing for LED alternating
    ledAlternateState = false; // Start with LED1 ON, LED2 OFF
    Serial.println("Captive Portal activated - LED alternating enabled");
    Serial.println("DEBUG: captivePortalActive flag set to true");
    Serial.println("DEBUG: lastLedToggleTime initialized to " + String(lastLedToggleTime));
    
    // Start LED alternating immediately
    digitalWrite(PIN_RELAY1_LED, LOW);   // Turn on LED1
    digitalWrite(PIN_RELAY2_LED, HIGH);  // Turn off LED2
    Serial.println("DEBUG: Initial LED state set - LED1=ON, LED2=OFF");
    Serial.println("DEBUG: PIN_RELAY1_LED state: " + String(digitalRead(PIN_RELAY1_LED)));
    Serial.println("DEBUG: PIN_RELAY2_LED state: " + String(digitalRead(PIN_RELAY2_LED))); });

  String customHTML = "<div style='text-align:left;'>"
                      "<p><strong>Additional Configuration</strong></p>"
                      "<p>WebSocket Port: <input type='text' name='port' value='%PORT%'></p>"
                      "</div>";
  wifiManager.setCustomMenuHTML(customHTML.c_str());
  wifiManager.addParameter(&customPortParam);

  if (!wifiManager.autoConnect("ShackMate - Outlet"))
  {
    Serial.println("Failed to connect, restarting...");
    delay(3000);
    ESP.restart();
    delay(5000);
  }

  deviceIP = WiFi.localIP().toString();
  Serial.println("Connected, IP address: " + deviceIP);

  // Check if we were in captive portal mode and need to reboot for proper web server initialization
  bool wasInCaptivePortal = captivePortalActive;

  // Clear captive portal flag and restore normal LED operation
  captivePortalActive = false;
  digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
  digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);
  Serial.println("WiFi connected - Captive Portal deactivated, LED alternating disabled");
  Serial.println("DEBUG: captivePortalActive flag set to false");
  Serial.println("DEBUG: Normal LED states restored");

  // Send debug info about WiFi connection (when web clients connect later)
  Serial.println("WiFi connected successfully, IP: " + deviceIP);

  // After exiting captive portal, delay then reboot to ensure web server works properly
  if (wasInCaptivePortal)
  {
    Serial.println("Exited captive portal - delaying 2 seconds then rebooting to ensure web server initialization...");
    delay(2000);
    ESP.restart();
  }

  // Configure NTP time synchronization
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP time synchronization configured");

  // Determine WebSocket port for mDNS advertisement
  int wsPort = tcpPort.toInt();
  if (wsPort <= 0)
    wsPort = 4000;

  // Configure mDNS service discovery
  if (!MDNS.begin(MDNS_NAME))
  {
    Serial.println("Error setting up mDNS responder!");
  }
  else
  {
    Serial.print("mDNS responder started: http://");
    Serial.print(MDNS_NAME);
    Serial.println(".local");
    // Advertise HTTP service via mDNS
    MDNS.addService("http", "tcp", 80);
    // Advertise WebSocket service via mDNS
    MDNS.addService("ws", "tcp", wsPort);
  }

  // Initialize UDP listener for ShackMate discovery on port 4210
  udpListener.begin(4210);
  Serial.println("UDP discovery listener started on port 4210");
  Serial.println("Ready to receive UDP discovery messages...");

  preferences.begin("config", false);
  tcpPort = customPortParam.getValue();
  preferences.putString("tcp_port", tcpPort);
  preferences.end();

  wsPortStr = tcpPort;

  wsPort = tcpPort.toInt();
  if (wsPort <= 0)
    wsPort = 4000;
  ws.onEvent(onWsEvent);
  httpServer.addHandler(&ws);
  Serial.println("WebSocket handler attached to HTTP server");

  // Root
  httpServer.on("/", HTTP_GET, handleRoot);

  // Config and Broadcasts HTML pages have been removed.
  // The device retains JSON/WebSocket and HTTP POST endpoints for config/broadcasts.

  httpServer.on("/saveConfig", HTTP_POST, handleSaveConfig);
  httpServer.on("/restoreConfig", HTTP_POST, handleRestoreConfig);

  // Reboot and restore HTTP endpoints for web interface
  httpServer.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *req)
                {
    sendDebugMessage("Reboot Requested");
    req->send(200, "text/plain", "Rebooting device...");
    delay(250);
    ESP.restart(); });

  httpServer.on("/restore", HTTP_POST, [](AsyncWebServerRequest *req)
                {
    req->send(200, "text/plain", "Completely erasing WiFi credentials...");
    // Complete WiFi reset to ensure captive portal activation
    WiFiManager wm;
    wm.resetSettings(); // Clear WiFiManager's saved credentials
    WiFi.disconnect(true, true); // Erase WiFi creds and reset
    WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
    delay(100);
    WiFi.mode(WIFI_STA);         // Set to station mode
    delay(500);
    ESP.restart(); });

  // Consolidated JSON data endpoint (handles both with and without trailing slash)
  httpServer.on("^/index/data/?$", HTTP_GET, handleDataJson);

  // Relay control HTTP endpoints
  httpServer.on("/relay1/on", HTTP_GET, [](AsyncWebServerRequest *req)
                {
    relay1State = true;
    digitalWrite(PIN_RELAY1,     HIGH);
    digitalWrite(PIN_RELAY1_LED, LOW);
    // Save state
    preferences.begin("outlet", false);
    preferences.putBool("output1", relay1State);
    preferences.end();
    req->send(200, "text/plain", "OK"); });
  httpServer.on("/relay1/off", HTTP_GET, [](AsyncWebServerRequest *req)
                {
    relay1State = false;
    digitalWrite(PIN_RELAY1,     LOW);
    digitalWrite(PIN_RELAY1_LED, HIGH);
    // Save state
    preferences.begin("outlet", false);
    preferences.putBool("output1", relay1State);
    preferences.end();
    req->send(200, "text/plain", "OK"); });
  httpServer.on("/relay2/on", HTTP_GET, [](AsyncWebServerRequest *req)
                {
    relay2State = true;
    digitalWrite(PIN_RELAY2,     HIGH);
    digitalWrite(PIN_RELAY2_LED, LOW);
    // Save state
    preferences.begin("outlet", false);
    preferences.putBool("output2", relay2State);
    preferences.end();
    req->send(200, "text/plain", "OK"); });
  httpServer.on("/relay2/off", HTTP_GET, [](AsyncWebServerRequest *req)
                {
    relay2State = false;
    digitalWrite(PIN_RELAY2,     LOW);
    digitalWrite(PIN_RELAY2_LED, HIGH);
    // Save state
    preferences.begin("outlet", false);
    preferences.putBool("output2", relay2State);
    preferences.end();
    req->send(200, "text/plain", "OK"); });

  // Handle browser favicon requests
  httpServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
                {
    // Return no content (204) to avoid errors when favicon is missing
    request->send(204, "image/x-icon", ""); });

  // Simple connectivity test endpoint
  httpServer.on("/test", HTTP_GET, [](AsyncWebServerRequest *request)
                {
    String response = "OK - ShackMate Outlet v" + String(VERSION) + " - IP: " + deviceIP + " - Time: " + String(millis()) + "ms";
    request->send(200, "text/plain", response); });

  httpServer.begin();
  Serial.println("HTTP server started on port 80");
  Serial.println("DEBUG: Web interface should be accessible at: http://" + deviceIP);
  Serial.println("DEBUG: WebSocket should be accessible at: ws://" + deviceIP + ":4000/ws");

  // Start WebSocket on port 4000
  ws.onEvent(onWsEvent);
  wsServer.addHandler(&ws);
  wsServer.begin();
  Serial.println("WebSocket server started on port 4000");

  ArduinoOTA.onStart([]()
                     { Serial.println("OTA update starting..."); });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\nOTA update complete"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("OTA Progress: %u%%\r", (progress * 100) / total); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Authentication Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed"); });
  ArduinoOTA.begin();
  Serial.println("OTA update service started");

  // Create OTA task on Core 1
  // This separates OTA handling from main loop operations, improving
  // reliability and responsiveness during OTA updates
  xTaskCreatePinnedToCore(
      otaTask,    // Task function
      "OTA_Task", // Task name for debugging
      4096,       // Stack size (4KB is sufficient for OTA handling)
      NULL,       // Task parameters (none needed)
      1,          // Task priority (1 = low priority, doesn't interfere with critical tasks)
      NULL,       // Task handle (not needed for this use case)
      1           // Core ID: 1 (Core 0 handles main loop, Core 1 handles OTA)
  );
  Serial.println("OTA Task created on Core 1");
}

// -------------------------------------------------------------------------
// Main Loop (runs on Core 0, OTA handled separately on Core 1)
// -------------------------------------------------------------------------
void loop()
{
  // Handle OTA updates
  ArduinoOTA.handle();

  // Handle WebSocket events and other network tasks
  ws.cleanupClients();

  // Handle UDP discovery for ShackMate connections
  handleUdpDiscovery();

  // CRITICAL: Handle WebSocket client loop for connections and messages
  wsClient.loop();

  // Handle CI-V message processing
  civHandler.loop();

  // Check WebSocket connection health and manage timeouts
  if (wsClientConnected && lastWebSocketActivity > 0)
  {
    unsigned long now = millis();
    unsigned long timeSinceActivity = now - lastWebSocketActivity;

    // Check for timeout
    if (timeSinceActivity > WEBSOCKET_TIMEOUT)
    {
      sendDebugMessage("WebSocket connection timeout (" + String(timeSinceActivity / 1000) + "s) - forcing disconnect");
      wsClient.disconnect();
      wsClientConnected = false;
    }
  }

  // Handle hardware button presses with debouncing
  unsigned long currentTime = millis();

  // Handle Captive Portal LED alternating
  if (captivePortalActive)
  {
    if (currentTime - lastLedToggleTime >= LED_ALTERNATE_INTERVAL)
    {
      lastLedToggleTime = currentTime;
      ledAlternateState = !ledAlternateState;

      // Alternate LED1 and LED2 (one on, one off)
      digitalWrite(PIN_RELAY1_LED, ledAlternateState ? LOW : HIGH); // LOW = LED ON
      digitalWrite(PIN_RELAY2_LED, ledAlternateState ? HIGH : LOW); // HIGH = LED OFF

      // Enhanced debug message for every toggle to help diagnose issues
      Serial.println("DEBUG: Captive Portal LED toggle - LED1=" + String(ledAlternateState ? "ON" : "OFF") + 
                     ", LED2=" + String(ledAlternateState ? "OFF" : "ON") + 
                     " (PIN_RELAY1_LED=" + String(digitalRead(PIN_RELAY1_LED)) + 
                     ", PIN_RELAY2_LED=" + String(digitalRead(PIN_RELAY2_LED)) + ")");
      
      sendDebugMessage("Captive Portal LED alternating - LED1=" + String(ledAlternateState ? "ON" : "OFF") + 
                       ", LED2=" + String(ledAlternateState ? "OFF" : "ON"));
    }
  }
  else
  {
    // Debug message to confirm captive portal is not active (only occasionally)
    static unsigned long lastCaptiveDebug = 0;
    if (currentTime - lastCaptiveDebug >= 30000) // Every 30 seconds
    {
      lastCaptiveDebug = currentTime;
      Serial.println("DEBUG: Captive Portal NOT active - normal LED operation");
    }
  }

  // Button 1 handling (skip if captive portal is active)
  if (!captivePortalActive && digitalRead(PIN_BUTTON1) == HIGH && (currentTime - lastButton1Time) > debounceDelay)
  {
    lastButton1Time = currentTime;
    relay1State = !relay1State;
    digitalWrite(PIN_RELAY1, relay1State ? HIGH : LOW);
    digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
    preferences.begin("outlet", false);
    preferences.putBool("output1", relay1State);
    preferences.end();

    // Broadcast state change
    String msg = "{\"type\":\"state\",\"output1State\":" + String(relay1State ? "true" : "false") +
                 ",\"output2State\":" + String(relay2State ? "true" : "false") +
                 ",\"label1\":\"" + String(label1Text) + "\",\"label2\":\"" + String(label2Text) + "\"}";
    ws.textAll(msg);

    Serial.printf("Button 1 pressed: Relay 1 %s\n", relay1State ? "ON" : "OFF");
  }

  // Button 2 handling (skip if captive portal is active)
  if (!captivePortalActive && digitalRead(PIN_BUTTON2) == HIGH && (currentTime - lastButton2Time) > debounceDelay)
  {
    lastButton2Time = currentTime;
    relay2State = !relay2State;
    digitalWrite(PIN_RELAY2, relay2State ? HIGH : LOW);
    digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);
    preferences.begin("outlet", false);
    preferences.putBool("output2", relay2State);
    preferences.end();

    // Broadcast state change
    String msg = "{\"type\":\"state\",\"output1State\":" + String(relay1State ? "true" : "false") +
                 ",\"output2State\":" + String(relay2State ? "true" : "false") +
                 ",\"label1\":\"" + String(label1Text) + "\",\"label2\":\"" + String(label2Text) + "\"}";
    ws.textAll(msg);

    Serial.printf("Button 2 pressed: Relay 2 %s\n", relay2State ? "ON" : "OFF");
  }

  // Periodic sensor reading and status broadcast
  static unsigned long lastSensorRead = 0;
  static unsigned long lastStatusBroadcast = 0;

  if (currentTime - lastSensorRead >= SENSOR_UPDATE_INTERVAL_MS)
  {
    lastSensorRead = currentTime;

    // Read sensors and broadcast status
    float voltage = hlw.getVoltage();
    float current = hlw.getCurrent();
    float power = hlw.getActivePower();

    // Read light sensor (LUX) - convert ADC reading to voltage
    uint16_t luxRaw = analogRead(PIN_LUX_ADC);
    float luxVoltage = (luxRaw / 4095.0) * 3.3; // Convert to voltage (0-3.3V)

    // Convert to lux value
    float lux = luxVoltage * (1000.0 / 3.3); // Simple conversion, adjust as needed

    // Get current time
    struct tm timeinfo;
    char timeStr[64];
    if (getLocalTime(&timeinfo))
    {
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    }
    else
    {
      strcpy(timeStr, "TIME_NOT_SET");
    }

    // Create comprehensive status message
    String statusMsg = "{\"type\":\"status\","
                       "\"volts\":" +
                       String(voltage, 1) + ","
                                            "\"amps\":" +
                       String(current, 2) + ","
                                            "\"watts\":" +
                       String(power, 0) + ","
                                          "\"lux\":" +
                       String(lux, 1) + ","
                                        "\"timestamp\":\"" +
                       String(timeStr) + "\","
                                         "\"output1State\":" +
                       String(relay1State ? "true" : "false") + ","
                                                                "\"output2State\":" +
                       String(relay2State ? "true" : "false") + ","
                                                                "\"label1\":\"" +
                       String(label1Text) + "\","
                                            "\"label2\":\"" +
                       String(label2Text) + "\","
                                            "\"civServerConnected\":" +
                       String(wsClientConnected ? "true" : "false") + ","
                                                                      "\"civServerEverConnected\":" +
                       String(wsClientEverConnected ? "true" : "false") + ","
                                                                          "\"civServerIP\":\"" +
                       connectedServerIP + "\","
                                           "\"civServerPort\":" +
                       String(connectedServerPort) + ","
                                                     "\"uptime\":\"" +
                       getUptime() + "\","
                                     "\"chipId\":\"" +
                       getChipID() + "\","
                                     "\"chipRevision\":" +
                       String(getChipRevision()) + ","
                                                   "\"version\":\"" +
                       String(VERSION) + "\","
                                         "\"udpPort\":" +
                       String(UDP_PORT) + ","
                                          "\"flashTotal\":" +
                       String(getFlashSize()) + ","
                                                "\"psramSize\":" +
                       String(getPsramSize()) + ","
                                                "\"cpuFreq\":" +
                       String(getCpuFrequency()) + ","
                                                   "\"freeHeap\":" +
                       String(getFreeHeap()) + ","
                                               "\"memUsed\":" +
                       String(getTotalHeap() - getFreeHeap()) + ","
                                                                "\"memTotal\":" +
                       String(getTotalHeap()) + ","
                                                "\"temperatureC\":" +
                       String(readInternalTemperature(), 1) + ","
                                                              "\"temperatureF\":" +
                       String(readInternalTemperature() * 9.0 / 5.0 + 32.0, 1) + ","
                                                                                 "\"rebootCount\":" +
                       String(rebootCounter) + ","
                                               "\"deviceId\":" +
                       String(deviceId) + ","
                                          "\"civAddress\":\"" +
                       civAddress + "\"}";

    ws.textAll(statusMsg);

    Serial.printf("Status: %.1fV %.2fA %.0fW %.1flux %s\n",
                  voltage, current, power, lux, timeStr);
  }

  // Small delay to prevent overwhelming the system
  delay(50);
}

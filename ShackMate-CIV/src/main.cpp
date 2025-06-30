#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <stdint.h>
#include <string>
bool allowOta = false;
String latestFwVersion = "";
String otaStatusMsg = "No updates found";
#define OTA_VERSION_URL "https://ristola.github.io/ShackMate/ShackMate-CIV/firmware/version.json"
#define OTA_FW_BASE_URL "https://ristola.github.io/ShackMate/ShackMate-CIV/firmware/"
#include <stdint.h>
// -------------------------------------------------------------------------
// Discovery packet timestamp (for clearing discovery info after timeout)
static unsigned long lastDiscoveryPacket = 0;

// Project Name and Version from PlatformIO, fallback defaults if not set
#ifndef NAME
#define NAME "ShackMate-CIV"
#endif
#ifndef VERSION
#define VERSION "1.0.0"
#endif

// Communications statistics
volatile uint32_t stat_serial1_frames = 0;
volatile uint32_t stat_serial1_valid = 0;
volatile uint32_t stat_serial1_invalid = 0;
volatile uint32_t stat_serial1_broadcast = 0;
volatile uint32_t stat_serial2_frames = 0;
volatile uint32_t stat_serial2_valid = 0;
volatile uint32_t stat_serial2_invalid = 0;
volatile uint32_t stat_serial2_broadcast = 0;
volatile uint32_t stat_ws_rx = 0;
volatile uint32_t stat_ws_tx = 0;
volatile uint32_t stat_ws_dup = 0;

/* N4LDR ShackMate CI-V Controller, copyright (c) 2025 Half Baked Circuits */

/*
This project uses Serial1 and Serial2 for CI-V communication with the following pin assignments:
UART#       Usual Pins    Typical Use    Can Remap?
            RX      TX
Serial 0   GPIO1, GPIO3    USB/Debug     No (USB used)
Serial 1   GPIO22, GPIO23  Unused/Free   Yes
Serial 2   GPIO21, GPIO25  User / Free   Yes (I2C pins)
*/

/*
  • Purple:  AP Mode (WiFi Config Portal)
  • Green (blinking):  Attempting WiFi Connection
  • Green:  WiFi Connected (not WebSocket)
  • Blue:  WebSocket Connected
  • White (blinking):  OTA Uploading
  • Red:  WiFi Lost after being connected
  • Orange:  Erasing WiFi Credentials (after 5s button press)
*/

// -------------------------------------------------------------------------
// Debug Macros (must be defined before use)
// -------------------------------------------------------------------------
#define DEBUG_SERIAL // Comment this line out to disable debug prints
#ifdef DEBUG_SERIAL
#define DBG_PRINT(...) Serial.print(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_PRINT(...) ((void)0)
#define DBG_PRINTLN(...) ((void)0)
#define DBG_PRINTF(...) ((void)0)
#endif
// -------------------------------------------------------------------------
// Includes
// -------------------------------------------------------------------------
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <time.h>          // NTP and time functions
#include <ArduinoOTA.h>    // OTA Over-the-Air updates
#include <esp_system.h>    // Chip revision, efuse, etc.
#include <esp_spi_flash.h> // Chip flash size
#if defined(M5ATOM_S3) || defined(ARDUINO_M5Stack_ATOMS3)
#include <M5AtomS3.h>
#include <Adafruit_NeoPixel.h>
#else
#include <M5Atom.h>
#include <FastLED.h>
#endif
// -------------------------------------------------------------------------
#include <WebSocketsClient.h>
#include <freertos/semphr.h>
#include <ESPAsyncWebServer.h>
#include "esp_task_wdt.h"
#include <deque>
struct MsgCacheEntry
{
  String hex;
  unsigned long timestamp;
};
std::deque<MsgCacheEntry> msgCache;
const unsigned long CACHE_WINDOW_MS = 1000;
const int CACHE_MAX_SIZE = 32;

bool isDuplicateMessage(const String &hex)
{
  unsigned long now = millis();
  // Purge old entries
  while (!msgCache.empty() && now - msgCache.front().timestamp > CACHE_WINDOW_MS)
  {
    msgCache.pop_front();
  }
  for (const auto &entry : msgCache)
  {
    if (entry.hex == hex)
      return true;
  }
  return false;
}

void addMessageToCache(const String &hex)
{
  if ((int)msgCache.size() >= CACHE_MAX_SIZE)
    msgCache.pop_front();
  msgCache.push_back({hex, millis()});
}
// HTTP/WebSocket server (browser dashboard) on port 80
AsyncWebServer httpServer(80);
AsyncWebSocket wsServer("/ws");

void checkAndAutoOta() {
  if (!allowOta) return;

  HTTPClient http;
  http.setTimeout(4000);
  http.begin(OTA_VERSION_URL);

  int httpCode = http.GET();
  if (httpCode != 200) {
    otaStatusMsg = "No updates found";
    latestFwVersion.clear();
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    otaStatusMsg = "No updates found";
    latestFwVersion.clear();
    return;
  }

  latestFwVersion = doc["version"] | "";
  String fwFile = doc["firmware_filename"] | "";
  String fsFile = doc["filesystem_filename"] | "";

  if (latestFwVersion.isEmpty() || fwFile.isEmpty()) {
    otaStatusMsg = "No updates found";
    return;
  }

  otaStatusMsg = "Latest Firmware: " + latestFwVersion;

  if (String(VERSION) == latestFwVersion) {
    // Already up to date
    return;
  }

  WiFiClient client;

  // Filesystem OTA is not supported via httpUpdate; skip this step.
  if (!fsFile.isEmpty()) {
    DBG_PRINTLN("Filesystem OTA update requested, but not supported via httpUpdate. Skipping...");
    // Optionally set otaStatusMsg = "Filesystem OTA not supported";
    // Or ignore completely.
  }

  otaStatusMsg = "Updating Firmware...";
  t_httpUpdate_return fwRet = httpUpdate.update(client, OTA_FW_BASE_URL + fwFile);
  if (fwRet == HTTP_UPDATE_OK) {
    // Update success, device will reboot automatically
  } else {
    otaStatusMsg = "Firmware update failed";
  }
}
// Helper to get number of connected /ws WebSocket clients
size_t getWsClientCount()
{
  return wsServer.count();
}
// Raw TCP server for custom clients (e.g. Python), port 4000
WiFiServer tcpServer(4000);
// -------------------------------------------------------------------------
// Forward declaration for setRgb (must be after includes, before any use)
// -------------------------------------------------------------------------
// Forward declaration for setRgb (must be after includes, before any use)
void setRgb(uint8_t r, uint8_t g, uint8_t b);

// -------------------------------------------------------------------------
// User-definable Serial port pin assignments and CI-V settings
#if defined(M5ATOM_S3) || defined(ARDUINO_M5Stack_ATOMS3)
// AtomS3 valid UART pins (adjust as needed for your wiring)
#define MY_RX1 5
#define MY_TX1 7
#define MY_RX2 39
#define MY_TX2 38
#define LED_PIN 35            // AtomS3 LED is GPIO35 (G35)
#define WIFI_RESET_BTN_PIN 41 // AtomS3 button is GPIO41 (G41)
#else
#define MY_RX1 22
#define MY_TX1 23
#define MY_RX2 21
#define MY_TX2 25
#define LED_PIN 27            // M5Atom LED is GPIO27 (G27)
#define WIFI_RESET_BTN_PIN 39 // M5Atom button is GPIO39
#endif
#define CIV_ADDRESS 0xC0 // CI-V controller address
// -------------------------------------------------------------------------
// setRgb implementation for Atom and AtomS3
void setRgb(uint8_t r, uint8_t g, uint8_t b)
{
#if defined(M5ATOM_S3) || defined(ARDUINO_M5Stack_ATOMS3)
  static Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);
  static bool initialized = false;
  if (!initialized)
  {
    pixel.begin();
    pixel.show();
    initialized = true;
  }
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
#else
  M5.dis.fillpix(CRGB(r, g, b));
#endif
}

// -------------------------------------------------------------------------
// Project Constants stored in flash (PROGMEM)
// -------------------------------------------------------------------------
const char AUTHOR_PROGMEM[] PROGMEM = "Half Baked Circuits";
const char MDNS_NAME_PROGMEM[] PROGMEM = "shackmate-civ";

// UDP Port definition
#define UDP_PORT 4210

// Undefine debug macros if previously defined
#ifdef DBG_PRINT
#undef DBG_PRINT
#endif
#ifdef DBG_PRINTLN
#undef DBG_PRINTLN
#endif
#ifdef DBG_PRINTF
#undef DBG_PRINTF
#endif
// -------------------------------------------------------------------------
// Debug Macros (must be defined before use)
// -------------------------------------------------------------------------
#define DEBUG_SERIAL // Comment this line out to disable debug prints
#ifdef DEBUG_SERIAL
#define DBG_PRINT(...) Serial.print(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_PRINT(...) ((void)0)
#define DBG_PRINTLN(...) ((void)0)
#define DBG_PRINTF(...) ((void)0)
#endif
// -------------------------------------------------------------------------
// Forward declaration for setRgb (must be before any use)
void setRgb(uint8_t r, uint8_t g, uint8_t b);

// -------------------------------------------------------------------------
// Global Objects & Variables
// -------------------------------------------------------------------------
Preferences preferences;
WiFiUDP udp;
String deviceIP = "";
String civBaud = "19200";

// WebSocket connection attempt flag
bool wsConnectPending = false;
unsigned long bootTime = 0;
// --- Serial2 CI-V frame buffer (fixed size) ---
#define MAX_CIV_FRAME 64
static char serial2Buf[MAX_CIV_FRAME];
static size_t serial2Len = 0;
static bool serial2FrameActive = false;

WebSocketsClient webClient;

bool otaInProgress = false;

// Mutex for protecting shared serial message strings
portMUX_TYPE serialMsgMutex = portMUX_INITIALIZER_UNLOCKED;

// Independent message tracking for each serial port to prevent cross-forwarding
String lastSerial1OutMsg = "";
String lastSerial2OutMsg = "";

// WebSocket duplicate message prevention
String lastWsInMsg = "";
unsigned long lastWsInTime = 0;

// -------------------------------------------------------------------------
// Connection state machine for discovery and WebSocket management
enum ConnState
{
  DISCOVERING,
  CONNECTING,
  CONNECTED
};
ConnState connectionState = DISCOVERING;
String lastDiscoveredIP = "";
String lastDiscoveredPort = "";
unsigned long lastDiscoveryAttempt = 0;

// -------------------------------------------------------------------------
// Function Prototypes
// -------------------------------------------------------------------------
String toHexUpper(const char *data, int len);
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

// Task function for CI-V/UDP/TCP processing (runs on Core 1)
// myTaskDebug will be run on Core 1 (CI-V/UDP only)
void myTaskDebug(void *parameter);
// core0Task (system monitoring, if needed) runs on Core 0 (optional)
void core0Task(void *parameter);
// core1UdpOtpTask: remains on Core 1 if needed (optional)
void core1UdpOtpTask(void *parameter);

// -------------------------------------------------------------------------
// Function Definitions
// -------------------------------------------------------------------------

// Converts a byte buffer into an uppercase hex string (with spaces)
String toHexUpper(const char *data, int len)
{
  char hexStr[3 * 64] = {0}; // up to 64 bytes, each "XX ", plus null
  int idx = 0;
  for (int i = 0; i < len; i++)
  {
    if (idx + 3 < (int)sizeof(hexStr))
    {
      sprintf(&hexStr[idx], "%02X ", (uint8_t)data[i]);
      idx += 3;
    }
  }
  return String(hexStr);
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

// Broadcast the dashboard status JSON to all /ws clients
void broadcastStatus()
{
  String status = "{";
  status += "\"ip\":\"" + deviceIP + "\",";
  status += "\"ws_status\":\"" +
            String(
                (connectionState == CONNECTED && lastDiscoveredIP.length() && lastDiscoveredPort.length())
                    ? "connected"
                    : "disconnected") +
            "\",";
  status += "\"ws_status_clients\":" + String(getWsClientCount()) + ",";
  status += "\"version\":\"" + String(VERSION) + "\",";
  status += "\"uptime\":\"" + getUptime() + "\",";
  status += "\"chip_id\":\"" + getChipID() + "\",";
  status += "\"cpu_freq\":\"" + String(getCpuFrequency()) + "\",";
  status += "\"free_heap\":\"" + String(getFreeHeap() / 1024) + "\",";
  status += "\"civ_baud\":\"" + civBaud + "\",";
  status += "\"civ_addr\":\"0x" + String(CIV_ADDRESS, HEX) + "\",";
  status += "\"serial1\":\"RX=" + String(MY_RX1) + " TX=" + String(MY_TX1) + "\",";
  status += "\"serial2\":\"RX=" + String(MY_RX2) + " TX=" + String(MY_TX2) + "\",";
  // Communications statistics
  status += "\"serial1_frames\":" + String(stat_serial1_frames) + ",";
  status += "\"serial1_valid\":" + String(stat_serial1_valid) + ",";
  status += "\"serial1_invalid\":" + String(stat_serial1_invalid) + ",";
  status += "\"serial1_broadcast\":" + String(stat_serial1_broadcast) + ",";
  status += "\"serial2_frames\":" + String(stat_serial2_frames) + ",";
  status += "\"serial2_valid\":" + String(stat_serial2_valid) + ",";
  status += "\"serial2_invalid\":" + String(stat_serial2_invalid) + ",";
  status += "\"serial2_broadcast\":" + String(stat_serial2_broadcast) + ",";
  status += "\"ws_rx\":" + String(stat_ws_rx) + ",";
  status += "\"ws_tx\":" + String(stat_ws_tx) + ",";
  status += "\"ws_dup\":" + String(stat_ws_dup) + ",";
  status += "\"lastDiscoveredIP\":\"" + lastDiscoveredIP + "\",";
  status += "\"lastDiscoveredPort\":\"" + lastDiscoveredPort + "\",";
  status += "\"ws_status_updated\":" + String(millis());
  status += "}";
  wsServer.textAll(status);
}

// -------------------------------------------------------------------------
// WebSocket Client Event Handler (for discovered remote)
void webSocketClientEvent(WStype_t type, uint8_t *payload, size_t length)
{
  if (type == WStype_CONNECTED)
  {
    wsConnectPending = false;
    Serial.print("WebSocket client connected to ");
    Serial.print(lastDiscoveredIP);
    Serial.print(":");
    Serial.println(lastDiscoveredPort);
    setRgb(0, 0, 64); // BLUE on websocket connect
    connectionState = CONNECTED;
    broadcastStatus();
    // Schedule a second broadcast after 100ms for instant UI update
    static TimerHandle_t wsStatusTimer = NULL;
    if (!wsStatusTimer)
    {
      wsStatusTimer = xTimerCreate("wsStatusTimer", pdMS_TO_TICKS(100), pdFALSE, 0, [](TimerHandle_t xTimer)
                                   { broadcastStatus(); });
    }
    if (wsStatusTimer)
    {
      xTimerStart(wsStatusTimer, 0);
    }
    return;
  }
  if (type == WStype_DISCONNECTED)
  {
    wsConnectPending = false;
    Serial.println("WebSocket client disconnected. Returning to discovery.");
    if (WiFi.isConnected())
    {
      setRgb(0, 64, 0); // GREEN if still on WiFi
    }
    else
    {
      setRgb(255, 0, 0); // RED if WiFi lost
    }
    connectionState = DISCOVERING;
    // Do NOT clear lastDiscoveredIP or lastDiscoveredPort here!
    // lastDiscoveredIP = "";
    // lastDiscoveredPort = "";
    // lastDiscoveryPacket = 0;
    broadcastStatus();
    return;
  }
  if (type != WStype_TEXT)
    return;

  // Get hex string
  String msg((char *)payload, length);
  msg.trim();

  // Validate hex format (optional)
  bool isHex = true;
  for (size_t i = 0; i < msg.length(); i++)
  {
    char c = msg.charAt(i);
    if (!((c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'F') ||
          (c >= 'a' && c <= 'f') ||
          c == ' '))
    {
      isHex = false;
      break;
    }
  }
  if (!isHex || msg.length() < 4)
    return;

  // Count RX if valid message
  stat_ws_rx++;

  // Convert hex to bytes
  msg.toUpperCase();
  msg.replace(" ", "");
  int byteCount = msg.length() / 2;
  uint8_t buffer[64];
  for (int i = 0; i < byteCount; i++)
    buffer[i] = (uint8_t)strtol(msg.substring(i * 2, i * 2 + 2).c_str(), NULL, 16);

  // Forward to both serial ports
  Serial1.write(buffer, byteCount);
  Serial1.flush();
  Serial2.write(buffer, byteCount);
  Serial2.flush();

  Serial.printf("WebSocket -> Serial1 & Serial2: %s\n", msg.c_str());
}

// -------------------------------------------------------------------------
// CI-V Task Function (runs on Core 1)
// -------------------------------------------------------------------------

// Validate CI-V frame (must be at least FE FE XX XX XX FD)
bool isValidCivFrame(const char *buf, size_t len)
{
  // Must be at least FE FE XX XX XX FD (min 5 bytes)
  if (len < 5)
    return false;
  // Must start with FE FE and end with FD
  if ((uint8_t)buf[0] != 0xFE || (uint8_t)buf[1] != 0xFE)
    return false;
  if ((uint8_t)buf[len - 1] != 0xFD)
    return false;
  return true;
}

// myTaskDebug: Handles CI-V serial/UDP processing ONLY, runs on Core 1.
void myTaskDebug(void *parameter)
{
  DBG_PRINTLN("ciV_UDP_Task started");
  esp_task_wdt_add(NULL); // Add this task to watchdog
  // --- Serial1 CI-V frame buffer (fixed size) ---
  static char serial1Buf[MAX_CIV_FRAME];
  static size_t serial1Len = 0;
  static bool serial1FrameActive = false;

  for (;;)
  {
    esp_task_wdt_reset(); // Feed watchdog for this task
    // --- Serial1 framing (fixed buffer, overflow safe) ---

    static int serial1FECount = 0;
    while (Serial1.available())
    {
      char c = Serial1.read();
      if (!serial1FrameActive)
      {
        if ((uint8_t)c == 0xFE)
        {
          serial1FECount++;
          if (serial1FECount == 2)
          {
            serial1FrameActive = true;
            serial1Len = 2;
            serial1Buf[0] = 0xFE;
            serial1Buf[1] = 0xFE;
            serial1FECount = 0;
          }
        }
        else
        {
          serial1FECount = 0;
        }
      }
      else if (serial1Len < MAX_CIV_FRAME)
      {
        serial1Buf[serial1Len++] = c;
        if ((uint8_t)c == 0xFD && serial1Len >= 5)
        {
          // On every frame, increment frames counter
          stat_serial1_frames++;
          // Validate CI-V frame before forwarding
          if (isValidCivFrame(serial1Buf, serial1Len))
          {
            stat_serial1_valid++;
            String hex;
            for (size_t i = 0; i < serial1Len; ++i)
            {
              char b[4];
              sprintf(b, "%02X ", (uint8_t)serial1Buf[i]);
              hex += b;
            }
            hex.trim();

            // Forward to WebSocket client (NO Serial debug print)
            if (webClient.isConnected())
            {
              if (!isDuplicateMessage(hex))
              {
                webClient.sendTXT(hex);
                stat_ws_tx++;
                addMessageToCache(hex);
              }
              else
              {
                stat_ws_dup++;
              }
            }
          }
          else
          {
            stat_serial1_invalid++;
            // Log invalid frame to WebSocket as JSON
            if (webClient.isConnected())
            {
              String logMsg = "{\"event\":\"civ_frame_invalid\",\"src\":\"serial1\",\"hex\":\"";
              for (size_t i = 0; i < serial1Len; ++i)
              {
                char b[4];
                sprintf(b, "%02X ", (uint8_t)serial1Buf[i]);
                logMsg += b;
              }
              logMsg.trim();
              logMsg += "\"}";
              webClient.sendTXT(logMsg);
              stat_ws_tx++;
            }
          }

          // --- BEGIN: Automatic reply to CI-V broadcast addressed to 00 ---
          if (serial1Len >= 6)
          { // At least FE FE 00 XX XX FD
            uint8_t toAddr = (uint8_t)serial1Buf[2];
            uint8_t fromAddr = (uint8_t)serial1Buf[3];
            // If the message is a broadcast (toAddr == 0x00) and not sent from us, respond as our device
            if (toAddr == 0x00 && fromAddr != CIV_ADDRESS)
            {
              stat_serial1_broadcast++;
              uint8_t cmd = (uint8_t)serial1Buf[4];
              uint8_t param = (serial1Len > 5) ? (uint8_t)serial1Buf[5] : 0x00;

              // Prepare base reply header
              uint8_t reply[MAX_CIV_FRAME] = {0xFE, 0xFE, fromAddr, CIV_ADDRESS, cmd};
              size_t replyLen = 5;

              // Echo subcommand if present
              if ((cmd == 0x19) && (serial1Len > 5))
              {
                reply[replyLen++] = param;

                // For command 19 01, append IP address as 4 bytes
                if (param == 0x01)
                {
                  IPAddress ip = WiFi.localIP();
                  reply[replyLen++] = ip[0];
                  reply[replyLen++] = ip[1];
                  reply[replyLen++] = ip[2];
                  reply[replyLen++] = ip[3];
                }
                else if (param == 0x00)
                {
                  // For 19 00, append our CI-V address
                  reply[replyLen++] = CIV_ADDRESS;
                }
              }

              reply[replyLen++] = 0xFD; // Terminator

              Serial1.write(reply, replyLen); // Use Serial1 for reply
              Serial1.flush();

              // --- WebSocket JSON log for broadcast reply ---
              if (webClient.isConnected())
              {
                String logMsg = "{\"event\":\"civ_broadcast_reply\",\"from\":\"0x00\",\"to\":\"0x";
                logMsg += String(CIV_ADDRESS, HEX);
                logMsg += "\",\"cmd\":\"19 01\",\"ip\":\"";
                logMsg += WiFi.localIP().toString();
                logMsg += "\"}";
                webClient.sendTXT(logMsg);
                stat_ws_tx++;
              }
            }
          }
          // --- END: Automatic reply to CI-V broadcast addressed to 00 ---

          serial1FrameActive = false;
          serial1Len = 0;
        }
        if (serial1Len >= MAX_CIV_FRAME)
        {
          // Overflow: drop frame
          serial1FrameActive = false;
          serial1Len = 0;
        }
      }
    }

    // --- Serial2 framing (fixed buffer, overflow safe) ---

    static int serial2FECount = 0;
    while (Serial2.available())
    {
      char c = Serial2.read();
      if (!serial2FrameActive)
      {
        if ((uint8_t)c == 0xFE)
        {
          serial2FECount++;
          if (serial2FECount == 2)
          {
            serial2FrameActive = true;
            serial2Len = 2;
            serial2Buf[0] = 0xFE;
            serial2Buf[1] = 0xFE;
            serial2FECount = 0;
          }
        }
        else
        {
          serial2FECount = 0;
        }
      }
      else if (serial2Len < MAX_CIV_FRAME)
      {
        serial2Buf[serial2Len++] = c;
        if ((uint8_t)c == 0xFD && serial2Len >= 5)
        {
          // On every frame, increment frames counter
          stat_serial2_frames++;
          // Validate CI-V frame before forwarding
          if (isValidCivFrame(serial2Buf, serial2Len))
          {
            stat_serial2_valid++;
            String hex;
            for (size_t i = 0; i < serial2Len; ++i)
            {
              char b[4];
              sprintf(b, "%02X ", (uint8_t)serial2Buf[i]);
              hex += b;
            }
            hex.trim();

            // Forward to WebSocket client (NO Serial debug print)
            if (webClient.isConnected())
            {
              if (!isDuplicateMessage(hex))
              {
                webClient.sendTXT(hex);
                stat_ws_tx++;
                addMessageToCache(hex);
              }
              else
              {
                stat_ws_dup++;
              }
            }
          }
          else
          {
            stat_serial2_invalid++;
            // Log invalid frame to WebSocket as JSON
            if (webClient.isConnected())
            {
              String logMsg = "{\"event\":\"civ_frame_invalid\",\"src\":\"serial2\",\"hex\":\"";
              for (size_t i = 0; i < serial2Len; ++i)
              {
                char b[4];
                sprintf(b, "%02X ", (uint8_t)serial2Buf[i]);
                logMsg += b;
              }
              logMsg.trim();
              logMsg += "\"}";
              webClient.sendTXT(logMsg);
              stat_ws_tx++;
            }
          }

          // --- BEGIN: Automatic reply to CI-V broadcast addressed to 00 ---
          if (serial2Len >= 6)
          { // At least FE FE 00 XX XX FD
            uint8_t toAddr = (uint8_t)serial2Buf[2];
            uint8_t fromAddr = (uint8_t)serial2Buf[3];
            // If the message is a broadcast (toAddr == 0x00) and not sent from us, respond as our device
            if (toAddr == 0x00 && fromAddr != CIV_ADDRESS)
            {
              stat_serial2_broadcast++;
              uint8_t cmd = (uint8_t)serial2Buf[4];
              uint8_t param = (serial2Len > 5) ? (uint8_t)serial2Buf[5] : 0x00;

              // Prepare base reply header
              uint8_t reply[MAX_CIV_FRAME] = {0xFE, 0xFE, fromAddr, CIV_ADDRESS, cmd};
              size_t replyLen = 5;

              // Echo subcommand if present
              if ((cmd == 0x19) && (serial2Len > 5))
              {
                reply[replyLen++] = param;

                // For command 19 01, append IP address as 4 bytes
                if (param == 0x01)
                {
                  IPAddress ip = WiFi.localIP();
                  reply[replyLen++] = ip[0];
                  reply[replyLen++] = ip[1];
                  reply[replyLen++] = ip[2];
                  reply[replyLen++] = ip[3];
                }
                else if (param == 0x00)
                {
                  // For 19 00, append our CI-V address
                  reply[replyLen++] = CIV_ADDRESS;
                }
              }

              reply[replyLen++] = 0xFD; // Terminator

              Serial2.write(reply, replyLen); // Use Serial2 for reply
              Serial2.flush();

              // --- WebSocket JSON log for broadcast reply ---
              if (webClient.isConnected())
              {
                String logMsg = "{\"event\":\"civ_broadcast_reply\",\"from\":\"0x00\",\"to\":\"0x";
                logMsg += String(CIV_ADDRESS, HEX);
                logMsg += "\",\"cmd\":\"19 01\",\"ip\":\"";
                logMsg += WiFi.localIP().toString();
                logMsg += "\"}";
                webClient.sendTXT(logMsg);
                stat_ws_tx++;
              }
            }
          }
          // --- END: Automatic reply to CI-V broadcast addressed to 00 ---

          serial2FrameActive = false;
          serial2Len = 0;
        }
        if (serial2Len >= MAX_CIV_FRAME)
        {
          // Overflow: drop frame
          serial2FrameActive = false;
          serial2Len = 0;
        }
      }
    }

    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

// -------------------------------------------------------------------------
// Core 0 task stub (system monitoring, optional)
// -------------------------------------------------------------------------
// If system monitoring or other tasks are needed on Core 0, use this.
void core0Task(void *parameter)
{
  esp_task_wdt_add(NULL); // Add this task to watchdog
  for (;;)
  {
    esp_task_wdt_reset(); // Feed watchdog for this task
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// -------------------------------------------------------------------------
// Minimal core1UdpOtpTask implementation (empty loop, feeds watchdog)
// -------------------------------------------------------------------------
void core1UdpOtpTask(void *parameter)
{
  esp_task_wdt_add(NULL); // Add this task to watchdog
  while (1)
  {
    esp_task_wdt_reset(); // Feed watchdog for this task
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// -------------------------------------------------------------------------
// Generate Info Page Function
// -------------------------------------------------------------------------
String generateInfoPage()
{
  String wsServerField = (lastDiscoveredIP.length() > 0 ? lastDiscoveredIP + ":" + lastDiscoveredPort : "Not discovered");
  String html = R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>)" + String(NAME) +
                R"( - Status</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: #333; min-height: 100vh; padding: 20px;
        }
        .container {
            max-width: 1000px; margin: 0 auto;
            background: rgba(255, 255, 255, 0.95);
            border-radius: 15px; padding: 30px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.1);
            backdrop-filter: blur(10px);
        }
        .header {
            text-align: center; margin-bottom: 30px;
            border-bottom: 2px solid #e0e0e0; padding-bottom: 20px;
        }
        .header h1 {
            color: #2c3e50; font-size: 2.5em; font-weight: 300; margin-bottom: 10px;
        }
        .version { color: #7f8c8d; font-size: 1.1em; }
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .card {
            background: #f8f9fa;
            border-radius: 10px;
            padding: 20px;
            border-left: 4px solid #3498db;
            box-shadow: 0 2px 8px rgba(52,152,219,0.06);
            margin-bottom: 0;
        }
        .card h3 {
            color: #2c3e50;
            margin-bottom: 15px;
            font-size: 1.3em;
            font-weight: 700;
            display: flex;
            align-items: center;
            gap: 0.5em;
            letter-spacing: 0.01em;
        }
        .info-row {
            display: flex; justify-content: space-between; margin-bottom: 8px;
            padding: 5px 0; border-bottom: 1px solid #ecf0f1;
        }
        .info-label { font-weight: 600; color: #34495e; }
        .info-value { color: #2c3e50; font-family: monospace; }
        .status {
            padding: 4px 8px; border-radius: 4px; font-size: 0.9em; font-weight: bold;
        }
        .status.connected { background: #d4edda; color: #155724; }
        .status.disconnected { background: #f8d7da; color: #721c24; }
        .status.listening { background: #fff3cd; color: #856404; }
        .footer {
            display: none;
        }
        .footer-centered {
            text-align: center;
            color: #666;
            font-size: 0.5em;
            margin: 40px 0 10px 0;
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        }
        .auto-refresh {
            position: fixed; top: 20px; right: 20px;
            background: rgba(0, 0, 0, 0.7); color: white;
            padding: 10px 15px; border-radius: 20px; font-size: 0.9em;
        }
        .stat-label {
            font-weight: 700;
            font-size: 1.4em;
            color: #2c3e50;
            min-width: 160px;
            display: inline-block;
            margin-right: 16px;
        }
        .stat-value {
            font-size: 1.18em;
            font-weight: 500;
            color: #222b38;
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            letter-spacing: 0.02em;
        }
        .comm-small {
            max-width: 350px;
            min-width: 0;
            margin-bottom: 0;
        }
    </style>
</style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>)" +
                String(NAME) + R"(</h1>
            <div class="version">Version <span id="version">)" +
                String(VERSION) + R"(</span></div>
        </div>
        
        
        <div class="grid">
            <div class="card">
                <h3><span style="font-size:1.5em;vertical-align:middle;display:inline-block;line-height:1;">📶</span> Network Status</h3>
                <div class="info-row">
                    <span class="info-label">IP Address:</span>
                    <span class="info-value" id="ip-address">)" +
                deviceIP + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">UDP Discovery:</span>
                    <span class="status listening">Listening on port )" +
                String(UDP_PORT) + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">WebSocket Server:</span>
                    <span class="info-value" id="ws-server-field">)" +
                wsServerField + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">WS Connection:</span>
                    <span class="status )" +
                String(
                    (connectionState == CONNECTED && lastDiscoveredIP.length() && lastDiscoveredPort.length())
                        ? "connected"
                        : "disconnected") +
                R"(" id="ws-connection">)" +
                String(
                    (connectionState == CONNECTED && lastDiscoveredIP.length() && lastDiscoveredPort.length())
                        ? "Connected"
                        : "Disconnected") +
                R"(</span>
                </div>
            </div>
            
            <div class="card">
                <h3><span style="font-size:1.5em;vertical-align:middle;display:inline-block;line-height:1;">🖥️</span> System Information</h3>
                <div class="info-row">
                    <span class="info-label">Chip ID:</span>
                    <span class="info-value" id="chip-id">)" +
                getChipID() + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">CPU Frequency:</span>
                    <span class="info-value" id="cpu-freq">)" +
                String(getCpuFrequency()) + R"( MHz</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Free Heap:</span>
                    <span class="info-value" id="free-heap">)" +
                String(getFreeHeap() / 1024) + R"( KB</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Uptime:</span>
                    <span class="info-value" id="uptime">)" +
                getUptime() + R"(</span>
                </div>
            </div>
            
            <div class="card">
                <h3><span style="font-size:1.5em;vertical-align:middle;display:inline-block;line-height:1;">🔧</span> CI-V Configuration</h3>
                <div class="info-row">
                    <span class="info-label">Baud Rate:</span>
                    <span class="info-value" id="civ-baud">)" +
                civBaud + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">CI-V Address:</span>
                    <span class="info-value" id="civ-addr">0x)" +
                String(CIV_ADDRESS, HEX) + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Serial1 (A):</span>
                    <span class="info-value" id="serial1">RX=)" +
                String(MY_RX1) + R"( TX=)" + String(MY_TX1) + R"(</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Serial2 (B):</span>
                    <span class="info-value" id="serial2">RX=)" +
                String(MY_RX2) + R"( TX=)" + String(MY_TX2) + R"(</span>
                </div>
            </div>
            
            <div class="card">
                <h3><span style="font-size:1.5em;vertical-align:middle;display:inline-block;line-height:1;">💾</span> Memory & Storage</h3>
                <div class="info-row">
                    <span class="info-label">Flash Size:</span>
                    <span class="info-value">)" +
                String(getFlashSize() / 1024) + R"( KB</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Sketch Size:</span>
                    <span class="info-value">)" +
                String(getSketchSize() / 1024) + R"( KB</span>
                </div>
                <div class="info-row">
                    <span class="info-label">Free Space:</span>
                    <span class="info-value">)" +
                String(getFreeSketchSpace() / 1024) + R"( KB</span>
                </div>
            </div>
        <div class="card comm-card comm-small">
            <h3><span style="font-size:1.5em;vertical-align:middle;display:inline-block;line-height:1;">📡</span> Communications</h3>
            <div class="info-row">
                <span class="info-label">Serial1:</span>
                <span class="info-value" id="serial1-stats-values">&#x2705; 0 / &#x274C; 0 &nbsp;|&nbsp; &#x1F4E2; 0</span>
            </div>
            <div class="info-row">
                <span class="info-label">Serial2:</span>
                <span class="info-value" id="serial2-stats-values">&#x2705; 0 / &#x274C; 0 &nbsp;|&nbsp; &#x1F4E2; 0</span>
            </div>
            <div class="info-row">
                <span class="info-label">WS:</span>
                <span class="info-value" id="ws-stats-values">RX 0 / TX 0</span>
            </div>
            <div class="info-row">
                <span class="info-label">WS: Duplicates Filtered</span>
                <span class="info-value" id="ws-dup">0</span>
            </div>
        </div>
        <div class="card">
          <h3>
            <span id="otaIcon" style="font-size:1.5em;vertical-align:middle;display:inline-block;line-height:1;cursor:pointer;" title="Click to check and update firmware">🔄</span>
            Automatic Updates
          </h3>
          <div class="info-row">
            <span class="info-label">Allow Automatic Updates:</span>
            <span class="info-value">
              <input type="checkbox" id="autoUpdateCb">
            </span>
          </div>
          <div id="otaStatus" style="margin-top:10px;color:#006;">
            Loading update status...
          </div>
        </div>
        </div>
    <div class="footer-centered">
      Half Baked Circuits &bull; Last updated: <span class="timestamp"></span>
    </div>
    </div>
</script>
<script>
document.addEventListener('DOMContentLoaded', function() {
    // Automatic OTA checkbox (no popup logic)
    fetch('/get_auto_update').then(r => r.text()).then(val => {
        document.getElementById('autoUpdateCb').checked = val.trim()=="1";
    });
    document.getElementById('autoUpdateCb').addEventListener('change', function() {
        fetch('/set_auto_update', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: 'enable='+(this.checked?1:0)
        });
    });
    function pad2(n){ return n<10 ? '0'+n : n; }
    function getTimeString() {
        let d = new Date();
        return pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
    }
    function loadOtaStatus() {
        fetch('/ota_status')
            .then(r => r.text())
            .then(val => {
                let msg = val.trim();
                let ts = getTimeString();
                let line = "";
                // If version present, show "Firmware: Ver: x.x.x @ timestamp"
                let verMatch = msg.match(/Latest Firmware: ([^\s]+)/i);
                if (verMatch && verMatch[1]) {
                    line = "Firmware: Ver: " + verMatch[1] + " @ " + ts;
                } else if (/Ver: ([0-9.]+)/i.test(msg)) {
                    // e.g. "Firmware: Ver: 1.2.3"
                    let m = msg.match(/Ver: ([0-9.]+)/i);
                    line = "Firmware: Ver: " + (m ? m[1] : "") + " @ " + ts;
                } else {
                    line = "Firmware check @ " + ts;
                }
                document.getElementById('otaStatus').innerText = line;
            });
    }
    loadOtaStatus();
    setInterval(loadOtaStatus, 30000);
    // Update timestamp every second
    function updateTimestamp() {
        let tsElem = document.querySelector('.timestamp');
        if (tsElem) {
            let d = new Date();
            tsElem.textContent = d.toLocaleTimeString();
        }
    }
    setInterval(updateTimestamp, 1000);
    updateTimestamp();
    // Add event handler for OTA icon
    document.getElementById('otaIcon').addEventListener('click', function() {
        var icon = this;
        icon.style.opacity = "0.6";
        icon.style.pointerEvents = "none";
        document.getElementById('otaStatus').innerText = "Checking for updates...";
        fetch('/ota_trigger', {method: 'POST'})
            .then(r => r.text())
            .then(msg => {
                document.getElementById('otaStatus').innerText = msg;
            })
            .catch(() => {
                document.getElementById('otaStatus').innerText = "Update failed or not allowed";
            })
            .finally(() => {
                icon.style.opacity = "";
                icon.style.pointerEvents = "";
            });
    });
});

function updateWsStatus(status) {
    var wsElem = document.getElementById('ws-connection');
    if (!wsElem) return;
    wsElem.classList.remove('connected', 'disconnected');
    if (status === 'connected') {
        wsElem.classList.add('connected');
        wsElem.textContent = 'Connected';
    } else {
        wsElem.classList.add('disconnected');
        wsElem.textContent = 'Disconnected';
    }
}

let ws;
function connectWS() {
    ws = new WebSocket('ws://' + location.hostname + '/ws');
    ws.onopen = function() {
        updateWsStatus('connected');
    };
    ws.onclose = function() {
        updateWsStatus('disconnected');
        setTimeout(connectWS, 5000);
    };
    ws.onerror = function(err) {
        updateWsStatus('disconnected');
    };
    ws.onmessage = function(event) {
        // Handle status JSON from server
        try {
            let data = JSON.parse(event.data);
            if (data.ws_status) {
                updateWsStatus(data.ws_status);
            }
            // Always update WebSocket Server field with lastDiscoveredIP:Port or Not discovered
            let wsServerElem = document.getElementById('ws-server-field');
            if (wsServerElem) {
                if (data.lastDiscoveredIP && data.lastDiscoveredPort) {
                    wsServerElem.textContent = data.lastDiscoveredIP + ':' + data.lastDiscoveredPort;
                } else {
                    wsServerElem.textContent = 'Not discovered';
                }
            }
            // Update stat fields
            if (data.serial1_frames !== undefined && data.serial1_valid !== undefined && data.serial1_invalid !== undefined && data.serial1_broadcast !== undefined) {
                document.getElementById('serial1-stats-values').innerHTML =
                    `&#x2705; ${data.serial1_valid} / &#x274C; ${data.serial1_invalid} &nbsp;|&nbsp; &#x1F4E2; ${data.serial1_broadcast}`;
            }
            if (data.serial2_frames !== undefined && data.serial2_valid !== undefined && data.serial2_invalid !== undefined && data.serial2_broadcast !== undefined) {
                document.getElementById('serial2-stats-values').innerHTML =
                    `&#x2705; ${data.serial2_valid} / &#x274C; ${data.serial2_invalid} &nbsp;|&nbsp; &#x1F4E2; ${data.serial2_broadcast}`;
            }
            if (data.ws_rx !== undefined && data.ws_tx !== undefined) {
                document.getElementById('ws-stats-values').textContent = `RX ${data.ws_rx} / TX ${data.ws_tx}`;
            }
            if (data.ws_dup !== undefined) {
                document.getElementById('ws-dup').textContent = data.ws_dup;
            }
        } catch (e) {
            // Not a JSON status message, ignore
        }
    };
}
connectWS();
setInterval(function() {
    fetch(window.location.href, {cache: "no-store"}).then(res => {
        if (res.status == 200 && document.hidden) location.reload();
    }).catch(()=>{});
}, 5000);
</script>
</body>
</html>)";

  return html;
}

// -------------------------------------------------------------------------
// Template Processor Function for AsyncWebServer
// -------------------------------------------------------------------------
String processTemplate(const String &var)
{
  if (var == "PROJECT_NAME")
    return String(NAME);
  if (var == "TIME")
    return "--:--"; // Placeholder, or implement getCurrentTime()
  if (var == "IP")
    return WiFi.localIP().toString();
  if (var == "WEBSOCKET_PORT")
    return "4000";
  if (var == "UDP_PORT")
    return String(UDP_PORT);
  if (var == "CIV_BAUD")
    return civBaud;
  if (var == "VERSION")
    return String(VERSION);
  if (var == "UPTIME")
    return getUptime();
  if (var == "CHIP_ID")
    return getChipID();
  if (var == "CHIP_REV")
    return String(getChipRevision());
  if (var == "FLASH_TOTAL")
    return String(getFlashSize());
  if (var == "PSRAM_SIZE")
    return String(getPsramSize());
  if (var == "CPU_FREQ")
    return String(getCpuFrequency());
  if (var == "FREE_HEAP")
    return String(getFreeHeap());
  if (var == "MEM_USED")
    return String(ESP.getHeapSize() - ESP.getFreeHeap());
  if (var == "MEM_TOTAL")
    return String(getTotalHeap());
  if (var == "SKETCH_USED")
    return String(ESP.getSketchSize());
  if (var == "SKETCH_TOTAL")
    return String(getSketchSize());
  if (var == "TEMPERATURE_C")
    return String(readInternalTemperature(), 2);
  if (var == "TEMPERATURE_F")
    return String(readInternalTemperature() * 9.0 / 5.0 + 32.0, 2);
  return "--";
}

// -------------------------------------------------------------------------
// Setup Function
// -------------------------------------------------------------------------
void setup()
{
  // Reset all stat counters to zero on boot
  stat_serial1_frames = 0;
  stat_serial1_valid = 0;
  stat_serial1_invalid = 0;
  stat_serial1_broadcast = 0;
  stat_serial2_frames = 0;
  stat_serial2_valid = 0;
  stat_serial2_invalid = 0;
  stat_serial2_broadcast = 0;
  stat_ws_rx = 0;
  stat_ws_tx = 0;
  stat_ws_dup = 0;
  Serial.begin(115200);
  DBG_PRINTLN("ShackMate CI-V: Booting...");
  DBG_PRINTF("Reset reason: %d\n", esp_reset_reason());
  delay(1000);
  bootTime = millis();
#if defined(M5ATOM_S3) || defined(ARDUINO_M5Stack_ATOMS3)
  M5.begin(); // AtomS3: use default config
#else
  M5.begin(true, false, true); // Atom: LCD, Serial, I2C as needed
#endif
  setRgb(0, 0, 64); // BLUE for disconnected

  pinMode(WIFI_RESET_BTN_PIN, INPUT);

  WiFiManager wifiManager;
  wifiManager.setAPCallback([&](WiFiManager *myWiFiManager)
                            {
    setRgb(64, 0, 64); // Purple (AP mode)
    DBG_PRINT("Entered configuration mode (AP mode): ");
    DBG_PRINTLN(NAME);
    IPAddress apIP = WiFi.softAPIP();
    DBG_PRINT("AP IP: ");
    DBG_PRINTLN(apIP.toString());
    yield(); });

  String storedCivBaud = "19200";
  preferences.begin("config", false);
  storedCivBaud = preferences.getString("civ_baud", "19200");
  preferences.end();

  WiFiManagerParameter civBaudParam("civbaud", "CI-V Baud Rate", storedCivBaud.c_str(), 6);
  wifiManager.addParameter(&civBaudParam);

  // Blinking green while trying to connect
  unsigned long blinkTimer = millis();
  bool connected = false;
  for (int i = 0; i < 30; ++i)
  { // Up to ~15 sec
    setRgb(0, 32, 0);
    delay(250);
    setRgb(0, 0, 0);
    delay(250);
    if (WiFi.isConnected())
    {
      connected = true;
      break;
    }
  }
  // Attempt to connect automatically; if fails, start config portal and halt
  if (!wifiManager.autoConnect(NAME) && !connected)
  {
    DBG_PRINTLN("Failed to connect, starting AP portal");
    while (true)
    {
      delay(1000);
    }
  }
  // At this point, WiFi is connected and you can proceed
  setRgb(0, 64, 0); // Green on successful WiFi connection
  // Ensure RGB is green when connected to WiFi and not yet connected to websocket
  deviceIP = WiFi.localIP().toString();
  DBG_PRINTLN("Connected, IP address: " + deviceIP);
  broadcastStatus();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    DBG_PRINTLN("Failed to obtain time");
  }
  else
  {
    DBG_PRINTLN("Time synchronized");
  }

  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                {
    String html = generateInfoPage();
    request->send(200, "text/html", html); });
  httpServer.on("/get_auto_update", HTTP_GET, [](AsyncWebServerRequest *req)
                { req->send(200, "text/plain", allowOta ? "1" : "0"); });
  httpServer.on("/set_auto_update", HTTP_POST, [](AsyncWebServerRequest *req)
                {
      if (req->hasArg("enable")) {
          allowOta = req->arg("enable") == "1";
          preferences.begin("config", false);
          preferences.putBool("allow_ota", allowOta);
          preferences.end();
          req->send(200, "text/plain", allowOta ? "1" : "0");
      } else {
          req->send(400, "text/plain", "Missing param");
      } });
  httpServer.on("/ota_status", HTTP_GET, [](AsyncWebServerRequest *req)
                { req->send(200, "text/plain", otaStatusMsg); });
  httpServer.on("/ota_trigger", HTTP_POST, [](AsyncWebServerRequest *req)
  {
      if (!allowOta) {
          req->send(403, "text/plain", "Automatic updates not enabled");
          return;
      }
      otaStatusMsg = "Checking for updates...";
      checkAndAutoOta();
      req->send(200, "text/plain", otaStatusMsg);
  });
  httpServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
                { request->send(204); });
  httpServer.addHandler(&wsServer);
  httpServer.begin();
  DBG_PRINTLN("HTTP server started on port 80");

  if (!MDNS.begin(MDNS_NAME_PROGMEM))
  {
    DBG_PRINTLN("Error setting up mDNS responder!");
  }
  else
  {
    DBG_PRINT("mDNS responder started: http://");
    DBG_PRINT(FPSTR(MDNS_NAME_PROGMEM));
    DBG_PRINTLN(".local");
  }

  udp.begin(UDP_PORT);

  preferences.begin("config", false);
  civBaud = civBaudParam.getValue();
  preferences.putString("civ_baud", civBaud);
  // Load Allow Automatic Updates
  allowOta = preferences.getBool("allow_ota", false);
  preferences.end();
  broadcastStatus();
  static bool hasCheckedFwUpdate = false;
  if (allowOta && !hasCheckedFwUpdate) {
    checkAndAutoOta();
    hasCheckedFwUpdate = true;
  }

  // Register WebSocket client event handler before any use/begin
  webClient.onEvent(webSocketClientEvent);

  ArduinoOTA.onStart([&]()
                     {
                       DBG_PRINTLN("OTA update starting...");
                       esp_task_wdt_delete(NULL); // Disable the watchdog for the current task during OTA
                       otaInProgress = true;
                       setRgb(64, 64, 64); // Ensure LED is white on start OTA
                     });
  ArduinoOTA.onEnd([&]()
                   {
                     DBG_PRINTLN("\nOTA update complete");
                     otaInProgress = false;
                     esp_task_wdt_init(10, true); // Re-enable watchdog after OTA
                     esp_task_wdt_add(NULL);      // Add current (loop) task to WDT
                     setRgb(0, 64, 0); // Green after OTA done
                     broadcastStatus(); });
  ArduinoOTA.onProgress([&](unsigned int progress, unsigned int total)
                        {
    static unsigned long lastBlink = 0;
    static bool ledState = false;
    if (millis() - lastBlink > 200) { // Blink every 200ms
      ledState = !ledState;
      setRgb(ledState ? 64 : 0, ledState ? 64 : 0, ledState ? 64 : 0); // Blink white
      lastBlink = millis();
    }
    DBG_PRINTF("OTA Progress: %u%%\r", (progress * 100) / total); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    DBG_PRINTF("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) DBG_PRINTLN("Authentication Failed");
    else if (error == OTA_BEGIN_ERROR) DBG_PRINTLN("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) DBG_PRINTLN("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) DBG_PRINTLN("Receive Failed");
    else if (error == OTA_END_ERROR) DBG_PRINTLN("End Failed"); });
  ArduinoOTA.begin();
  DBG_PRINTLN("OTA update service started");

  tcpServer.begin();
  DBG_PRINTLN("Raw TCP server started on port 4000");

  // Enable internal pull-up resistors on Serial RX pins to avoid floating input
  pinMode(MY_RX1, INPUT_PULLUP); // Serial1 RX (GPIO22)
  pinMode(MY_RX2, INPUT_PULLUP); // Serial2 RX (GPIO21)

  int baud = civBaud.toInt();
  if (baud <= 0)
    baud = 19200;
  Serial1.setRxBufferSize(1024);
  Serial1.setTxBufferSize(1024);
  Serial1.begin(baud, SERIAL_8N1, MY_RX1, MY_TX1);
  DBG_PRINTF("Serial1 (CI-V (A)) started at %d baud on RX=%d TX=%d (1KB buffers)\n", baud, MY_RX1, MY_TX1);
  Serial2.setRxBufferSize(1024);
  Serial2.setTxBufferSize(1024);
  Serial2.begin(baud, SERIAL_8N1, MY_RX2, MY_TX2);
  DBG_PRINTF("Serial2 (CI-V (B)) started at %d baud on RX=%d TX=%d (1KB buffers)\n", baud, MY_RX2, MY_TX2);

  // -------------------------------------------------------------------------
  // TASK/CORE ALLOCATION:
  //   Core 0: Arduino loop() (networking, OTA, web server, TCP, async, etc.)
  //           [Optional] core0Task for system monitoring (if needed)
  //   Core 1: myTaskDebug (CI-V serial/UDP processing only)
  //           core1UdpOtpTask (UDP/OTP broadcast, if needed)
  // -------------------------------------------------------------------------
  // Create a separate task for CI-V/UDP processing on Core 1
  BaseType_t result = xTaskCreatePinnedToCore(myTaskDebug, "ciV_UDP_Task", 4096, NULL, 1, NULL, 1);
  if (result != pdPASS)
  {
    DBG_PRINTLN("Failed to create ciV_UDP_Task!");
  }
  // [Optional] Start core0Task for system monitoring on Core 0
  // xTaskCreatePinnedToCore(core0Task, "core0Task", 2048, NULL, 1, NULL, 0);
  // Start core 1 UDP/OTP broadcast task (remains on Core 1 if needed)
  xTaskCreatePinnedToCore(core1UdpOtpTask, "core1UdpOtpTask", 6144, NULL, 1, NULL, 1);

  // Enable 10-second watchdog for loop() task (runs on Core 0)
  esp_task_wdt_init(10, true); // 10s timeout, panic if not fed
  esp_task_wdt_add(NULL);      // Add current (loop) task to WDT
}

// -------------------------------------------------------------------------
// Main Loop (runs on Core 0: networking, OTA, web server, TCP, async, etc.)
// -------------------------------------------------------------------------
void loop()
{
  esp_task_wdt_reset(); // Feed the watchdog at the start

  ArduinoOTA.handle();
  esp_task_wdt_reset(); // Feed after OTA

  webClient.loop();
  esp_task_wdt_reset(); // Feed after WebSocket

  // Raw TCP server on port 4000 (for custom clients, not browsers)
  WiFiClient tcpClient = tcpServer.available();
  if (tcpClient)
  {
    // Echo a basic dashboard status as JSON, one per connection, including ws_status_updated
    String tcpStatus = "{";
    tcpStatus += "\"ip\":\"" + deviceIP + "\",";
    tcpStatus += "\"ws_status\":\"" + String((connectionState == CONNECTED) ? "connected" : "disconnected") + "\",";
    tcpStatus += "\"ws_status_clients\":" + String(getWsClientCount()) + ",";
    tcpStatus += "\"version\":\"" + String(VERSION) + "\",";
    tcpStatus += "\"uptime\":\"" + getUptime() + "\",";
    tcpStatus += "\"chip_id\":\"" + getChipID() + "\",";
    tcpStatus += "\"cpu_freq\":\"" + String(getCpuFrequency()) + "\",";
    tcpStatus += "\"free_heap\":\"" + String(getFreeHeap() / 1024) + "\",";
    tcpStatus += "\"civ_baud\":\"" + civBaud + "\",";
    tcpStatus += "\"civ_addr\":\"0x" + String(CIV_ADDRESS, HEX) + "\",";
    tcpStatus += "\"serial1\":\"RX=" + String(MY_RX1) + " TX=" + String(MY_TX1) + "\",";
    tcpStatus += "\"serial2\":\"RX=" + String(MY_RX2) + " TX=" + String(MY_TX2) + "\",";
    tcpStatus += "\"ws_status_updated\":" + String(millis());
    tcpStatus += "}\n";
    tcpClient.print(tcpStatus);
    // Simple: close connection after sending
    tcpClient.stop();
  }
  esp_task_wdt_reset(); // Feed after TCP client processing

  // --- Optimized Discovery and Connection State Updates ---
  // Track last broadcasted state to avoid redundant updates
  static ConnState lastBroadcastedState = DISCOVERING;
  static String lastBroadcastedIP = "";
  static String lastBroadcastedPort = "";

  unsigned long now = millis();

  // Discovery state
  if (connectionState == DISCOVERING)
  {
    if (now - lastDiscoveryAttempt > 2000)
    {
      lastDiscoveryAttempt = now;
      int packetSize = udp.parsePacket();
      if (packetSize > 0)
      {
        char packetBuffer[128];
        int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
        if (len > 0)
        {
          packetBuffer[len] = '\0';
          String msg = String(packetBuffer);
          if (msg.indexOf("ShackMate") >= 0)
          {
            int firstComma = msg.indexOf(',');
            int secondComma = msg.indexOf(',', firstComma + 1);
            String ip = msg.substring(firstComma + 1, secondComma);
            String port = msg.substring(secondComma + 1);
            Serial.print("Discovered ShackMate IP: ");
            Serial.print(ip);
            Serial.print(" Port: ");
            Serial.println(port);
            lastDiscoveredIP = ip;
            lastDiscoveredPort = port;
            lastDiscoveryPacket = now;
            connectionState = CONNECTING;
            broadcastStatus();
          }
        }
      }
    }
  }
  // Timeout: clear discovery info ONLY if not connected
  if (lastDiscoveryPacket && (now - lastDiscoveryPacket > 6000))
  {
    if (connectionState != CONNECTED)
    {
      lastDiscoveredIP = "";
      lastDiscoveredPort = "";
      lastDiscoveryPacket = 0;
      if (connectionState != lastBroadcastedState || lastDiscoveredIP != lastBroadcastedIP || lastDiscoveredPort != lastBroadcastedPort)
      {
        broadcastStatus();
        lastBroadcastedState = connectionState;
        lastBroadcastedIP = lastDiscoveredIP;
        lastBroadcastedPort = lastDiscoveredPort;
      }
    }
    // If connected, do not clear lastDiscoveredIP/Port
  }

  // Connecting state
  if (connectionState == CONNECTING && lastDiscoveredIP.length() && lastDiscoveredPort.length() && !wsConnectPending)
  {
    Serial.print("Attempting WebSocket connection to ");
    Serial.print(lastDiscoveredIP);
    Serial.print(":");
    Serial.println(lastDiscoveredPort);
    webClient.begin(lastDiscoveredIP, lastDiscoveredPort.toInt(), "/");
    wsConnectPending = true;
    if (connectionState != lastBroadcastedState || lastDiscoveredIP != lastBroadcastedIP || lastDiscoveredPort != lastBroadcastedPort)
    {
      broadcastStatus();
      lastBroadcastedState = connectionState;
      lastBroadcastedIP = lastDiscoveredIP;
      lastBroadcastedPort = lastDiscoveredPort;
    }
  }
  esp_task_wdt_reset(); // Feed after discovery/connect

  // --- Periodic dashboard status broadcast to all /ws clients ---
  static unsigned long lastDashboardBroadcast = 0;
  if (now - lastDashboardBroadcast > 2000)
  { // every 2 seconds
    broadcastStatus();
    lastDashboardBroadcast = now;
  }

  // --- WiFi credential reset with 5 second button hold ---
  static unsigned long wifiResetPressStart = 0;
  static bool wifiResetActive = false;
  bool buttonPressed = digitalRead(WIFI_RESET_BTN_PIN) == 0; // Button on Atom is active LOW

  if (buttonPressed)
  {
    if (!wifiResetActive)
    {
      wifiResetPressStart = millis();
      wifiResetActive = true;
    }
    else if (millis() - wifiResetPressStart >= 5000)
    {
      // Erase WiFi creds after 5 second hold
      preferences.begin("wifi", false);
      preferences.clear();
      preferences.end();
      WiFi.disconnect(true, true);
      Serial.println("WiFi credentials erased! Rebooting in 2 seconds...");
      setRgb(255, 140, 0); // Orange for erase event
      broadcastStatus();
      delay(2000);
      ESP.restart();
    }
  }
  else
  {
    wifiResetActive = false;
  }
  esp_task_wdt_reset(); // Feed after button check
  vTaskDelay(1 / portTICK_PERIOD_MS);
}
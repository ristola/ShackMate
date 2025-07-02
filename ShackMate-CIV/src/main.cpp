#include <stdint.h>
// -------------------------------------------------------------------------
// CI-V Controller Specific Configuration
// -------------------------------------------------------------------------
#include "../include/civ_config.h"

// -------------------------------------------------------------------------
// ShackMateCore Library Includes
// -------------------------------------------------------------------------
#include "../lib/ShackMateCore/logger.h"
#include "../lib/ShackMateCore/device_state.h"
#include "../lib/ShackMateCore/network_manager.h"
#include "../lib/ShackMateCore/json_builder.h"

// -------------------------------------------------------------------------
// Additional JSON Library for Direct JSON Handling
// -------------------------------------------------------------------------
#include <ArduinoJson.h>

// -------------------------------------------------------------------------
// Function Declarations
// -------------------------------------------------------------------------
bool isValidHexMessage(const String &msg);
void validateConfiguration();

// -------------------------------------------------------------------------
// Helper Function Implementations
// -------------------------------------------------------------------------
bool isValidHexMessage(const String &msg)
{
  if (msg.length() < 4)
    return false;

  for (size_t i = 0; i < msg.length(); i++)
  {
    char c = msg.charAt(i);
    if (!((c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'F') ||
          (c >= 'a' && c <= 'f') ||
          c == ' '))
    {
      return false;
    }
  }
  return true;
}

void validateConfiguration()
{
  Logger::info("Validating configuration...");

  // Check CI-V address is in valid range
  if (CIV_ADDRESS < 0x01 || CIV_ADDRESS > 0xDF)
  {
    Logger::error("Invalid CI-V address: 0x" + String(CIV_ADDRESS, HEX));
  }

  // Check serial pins are not conflicting
  if (MY_RX1 == MY_RX2 || MY_TX1 == MY_TX2 || MY_RX1 == MY_TX2 || MY_TX1 == MY_RX2)
  {
    Logger::error("Serial pin conflict detected!");
  }

  // Check UDP port is in valid range
  if (UDP_PORT < 1024 || UDP_PORT > 65535)
  {
    Logger::warning("UDP port " + String(UDP_PORT) + " may require elevated privileges");
  }

  Logger::info("Configuration validation complete");
}

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
/*
This project uses Serial1 and Serial2 for CI-V communication with the following pin assignments:

{ATOM-LITE}
UART#       Usual Pins    Typical Use    Can Remap?
            RX      TX
Serial 0   GPIO1, GPIO3    USB/Debug     No (USB used)
Serial 1   GPIO22, GPIO23  Unused/Free   Yes
Serial 2   GPIO21, GPIO25  User / Free   Yes (I2C pins)

{M5STACK-ATOMS3-LITE}
UART#       Usual Pins    Typical Use    Can Remap?
            RX      TX
Serial 0   GPIO01, GPIO03  USB/Debug     No (USB used)
Serial 1   GPIO05, GPIO07  Unused/Free   Yes
Serial 2   GPIO39, GPIO38  User / Free   Yes (I2C pins)

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
// Logging System - Using ShackMateCore Logger
// -------------------------------------------------------------------------
// Replace debug macros with ShackMateCore Logger calls
#define DBG_PRINT(msg) Logger::debug(String(msg))
#define DBG_PRINTLN(msg) Logger::info(String(msg))
#define DBG_PRINTF(fmt, ...) Logger::info(String("DBG: ") + String(fmt))
// -------------------------------------------------------------------------
// Standard Arduino and ESP32 Libraries
// -------------------------------------------------------------------------
#include <WiFi.h>
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
#include "LittleFS.h"

// WiFiManager includes last to avoid HTTP method conflicts
// #include <WiFiManager.h>  // Temporarily disabled due to HTTP method conflicts

struct MsgCacheEntry
{
  String hex;
  unsigned long timestamp;
};
std::deque<MsgCacheEntry> msgCache;

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
void setRgb(uint8_t r, uint8_t g, uint8_t b);

// -------------------------------------------------------------------------
// User-definable Serial port pin assignments and CI-V settings are now in civ_config.h
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
// Project Constants now defined in civ_config.h
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// Constants
// -------------------------------------------------------------------------
const unsigned long DISCOVERY_INTERVAL_MS = 2000;
const unsigned long WIFI_RESET_HOLD_TIME_MS = 5000;
const unsigned long OTA_BLINK_INTERVAL_MS = 200;
const int WIFI_CONNECTION_ATTEMPTS = 30;
const int WIFI_BLINK_DELAY_MS = 250;
const size_t TCP_PACKET_BUFFER_SIZE = 128;
const int WATCHDOG_TIMEOUT_SECONDS = 10;

// -------------------------------------------------------------------------
// Global Objects & Variables
// -------------------------------------------------------------------------
Preferences preferences;
WiFiUDP udp;
String deviceIP = "";
String civBaud = "19200";

// DeviceState will handle uptime tracking
// bootTime removed, managed by DeviceState now
// --- Serial2 CI-V frame buffer (fixed size) ---
#define MAX_CIV_FRAME 64
static char serial2Buf[MAX_CIV_FRAME];
static size_t serial2Len = 0;
static bool serial2FrameActive = false;

WebSocketsClient webClient;

bool otaInProgress = false;

// Mutex for protecting shared serial message strings (for potential future use)
portMUX_TYPE serialMsgMutex = portMUX_INITIALIZER_UNLOCKED;

// These variables were unused and have been removed to clean up the code

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
bool wsConnectPending = false;

// -------------------------------------------------------------------------
// Function Prototypes
// -------------------------------------------------------------------------
String toHexUpper(const char *data, int len);
// Use DeviceState system info functions instead of manual ones
// String getUptime(); - now use DeviceState::getUptime()
// System info now managed by DeviceState::getSystemInfo()

// Task function for CI-V/UDP/TCP processing (runs on Core 1)
// myTaskDebug will be run on Core 1 (CI-V/UDP only)
void myTaskDebug(void *parameter);
// core1UdpOtpTask: remains on Core 1 if needed (optional)
void core1UdpOtpTask(void *parameter);

// -------------------------------------------------------------------------
// Helper Function Implementations
// -------------------------------------------------------------------------
void initFileSystem()
{
  if (!LittleFS.begin())
  {
    Logger::error("Failed to mount LittleFS filesystem");
    return;
  }
  Logger::info("LittleFS filesystem mounted successfully");
}

void resetAllStats()
{
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
  Logger::info("All statistics reset to zero");
}

bool isValidBaudRate(const String &baud)
{
  int baudInt = baud.toInt();
  return (baudInt == 1200 || baudInt == 2400 || baudInt == 4800 ||
          baudInt == 9600 || baudInt == 19200 || baudInt == 38400 ||
          baudInt == 57600 || baudInt == 115200);
}

void checkMemoryHealth()
{
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();

  if (freeHeap < 10240)
  { // Less than 10KB free
    Logger::warning("Low memory warning: " + String(freeHeap) + " bytes free");
  }

  if (minFreeHeap < 5120)
  { // Minimum ever was less than 5KB
    Logger::error("Critical memory condition detected: min " + String(minFreeHeap) + " bytes");
  }
}

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
// getUptime() function removed - now using DeviceState::getUptime()

// System info functions replaced by ShackMateCore DeviceState where possible

String getChipID()
{
  uint64_t chipid = ESP.getEfuseMac();
  char idString[17];
  sprintf(idString, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
  return String(idString);
}

int getCpuFrequency()
{
  return ESP.getCpuFreqMHz();
}

uint32_t getFreeHeap()
{
  return ESP.getFreeHeap();
}

uint32_t getFlashSize()
{
  return ESP.getFlashChipSize();
}

uint32_t getSketchSize()
{
  return ESP.getSketchSize();
}

uint32_t getFreeSketchSpace()
{
  return ESP.getFreeSketchSpace();
}

// Broadcast the dashboard status JSON to all /ws clients using ShackMateCore
void broadcastStatus()
{
  DynamicJsonDocument doc(1024);
  doc["ip"] = deviceIP;
  doc["ws_status"] = (connectionState == CONNECTED) ? "connected" : "disconnected";
  doc["ws_status_clients"] = getWsClientCount();
  doc["ws_server_ip"] = lastDiscoveredIP.length() > 0 ? lastDiscoveredIP : "Not discovered";
  doc["ws_server_port"] = lastDiscoveredPort.length() > 0 ? lastDiscoveredPort : "";
  doc["version"] = String(VERSION);
  doc["uptime"] = DeviceState::getUptime();
  // Use ShackMateCore system info where possible
  doc["chip_id"] = getChipID();
  doc["cpu_freq"] = String(getCpuFrequency());
  doc["free_heap"] = String(getFreeHeap() / 1024);
  doc["civ_baud"] = civBaud;
  doc["civ_addr"] = "0x" + String(CIV_ADDRESS, HEX);
  doc["serial1"] = "RX=" + String(MY_RX1) + " TX=" + String(MY_TX1);
  doc["serial2"] = "RX=" + String(MY_RX2) + " TX=" + String(MY_TX2);
  // Communications statistics
  doc["serial1_frames"] = stat_serial1_frames;
  doc["serial1_valid"] = stat_serial1_valid;
  doc["serial1_invalid"] = stat_serial1_invalid;
  doc["serial1_broadcast"] = stat_serial1_broadcast;
  doc["serial2_frames"] = stat_serial2_frames;
  doc["serial2_valid"] = stat_serial2_valid;
  doc["serial2_invalid"] = stat_serial2_invalid;
  doc["serial2_broadcast"] = stat_serial2_broadcast;
  doc["ws_rx"] = stat_ws_rx;
  doc["ws_tx"] = stat_ws_tx;
  doc["ws_dup"] = stat_ws_dup;
  doc["ws_status_updated"] = millis();

  String status;
  serializeJson(doc, status);
  wsServer.textAll(status);
}

// -------------------------------------------------------------------------
// WebSocket Client Event Handler (for discovered remote)
void webSocketClientEvent(WStype_t type, uint8_t *payload, size_t length)
{
  if (type == WStype_CONNECTED)
  {
    wsConnectPending = false;
    Logger::info("WebSocket client connected to " + lastDiscoveredIP + ":" + lastDiscoveredPort);
    setRgb(0, 0, 64); // BLUE on websocket connect
    connectionState = CONNECTED;
    broadcastStatus();
    return;
  }
  if (type == WStype_DISCONNECTED)
  {
    wsConnectPending = false;
    Logger::info("WebSocket client disconnected. Returning to discovery.");
    if (WiFi.isConnected())
    {
      setRgb(0, 64, 0); // GREEN if still on WiFi
    }
    else
    {
      setRgb(255, 0, 0); // RED if WiFi lost
    }
    connectionState = DISCOVERING;
    broadcastStatus();
    return;
  }
  if (type != WStype_TEXT)
    return;

  // Get hex string
  String msg((char *)payload, length);
  msg.trim();

  // Validate hex format
  if (!isValidHexMessage(msg))
  {
    Logger::warning("Invalid hex message received: " + msg);
    return;
  }

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

  Logger::debug("WebSocket -> Serial1 & Serial2: " + msg);
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
                broadcastStatus();
                addMessageToCache(hex);
              }
              else
              {
                stat_ws_dup++;
                broadcastStatus();
              }
            }
          }
          else
          {
            stat_serial1_invalid++;
            broadcastStatus();
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
                broadcastStatus();
                addMessageToCache(hex);
              }
              else
              {
                stat_ws_dup++;
                broadcastStatus();
              }
            }
          }
          else
          {
            stat_serial2_invalid++;
            broadcastStatus();
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
// REMOVED: generateInfoPage function - now serving static files from /data
// -------------------------------------------------------------------------

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

  // Initialize ShackMateCore Logger system
  Logger::init(LogLevel::INFO);
  Logger::enableSerial(true);

  // Initialize ShackMateCore DeviceState management
  DeviceState::init();
  DeviceState::setBootTime(millis());

  LOG_INFO("================================================");
  LOG_INFO("        SHACKMATE CI-V CONTROLLER STARTING");
  LOG_INFO("================================================");
  LOG_INFO("Version: " + String(VERSION));
  LOG_INFO("Boot time: " + String(DeviceState::getBootTime()) + "ms");
  LOG_INFO("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
  LOG_INFO("Reset reason: " + String(esp_reset_reason()));

  // Validate configuration before proceeding
  validateConfiguration();

  delay(1000);
#if defined(M5ATOM_S3) || defined(ARDUINO_M5Stack_ATOMS3)
  M5.begin(); // AtomS3: use default config
#else
  M5.begin(true, false, true); // Atom: LCD, Serial, I2C as needed
#endif
  setRgb(0, 0, 64); // BLUE for disconnected

  pinMode(WIFI_RESET_BTN_PIN, INPUT);

  // Simple WiFi connection (replaces WiFiManager to avoid HTTP conflicts)
  Logger::info("Starting WiFi connection...");
  WiFi.mode(WIFI_STA);

  // Try to connect to saved WiFi credentials
  WiFi.begin();

  String storedCivBaud = "19200";
  preferences.begin("config", false);
  storedCivBaud = preferences.getString("civ_baud", "19200");
  preferences.end();

  // Blinking green while trying to connect
  unsigned long blinkTimer = millis();
  bool connected = false;
  for (int i = 0; i < WIFI_CONNECTION_ATTEMPTS; ++i)
  { // Up to ~15 sec
    setRgb(0, 32, 0);
    delay(WIFI_BLINK_DELAY_MS);
    setRgb(0, 0, 0);
    delay(WIFI_BLINK_DELAY_MS);
    if (WiFi.isConnected())
    {
      connected = true;
      break;
    }
  }

  // If connection failed, start AP mode for configuration
  if (!connected)
  {
    Logger::warning("WiFi connection failed, starting AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ShackMate CI-V AP");
    setRgb(64, 0, 64); // Purple (AP mode)
    Logger::info("AP started: ShackMate CI-V AP");
    Logger::info("AP IP: " + WiFi.softAPIP().toString());
  }
  else
  {
    Logger::info("WiFi connected successfully");
    Logger::info("IP address: " + WiFi.localIP().toString());
  }

  // At this point, WiFi is connected and you can proceed
  setRgb(0, 64, 0); // Green on successful WiFi connection
  // Ensure RGB is green when connected to WiFi and not yet connected to websocket
  deviceIP = WiFi.localIP().toString();
  Logger::info("Connected, IP address: " + deviceIP);
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

  // Initialize file system and serve static files
  initFileSystem();

  // Serve main page with template replacement
  httpServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                {
    if (!LittleFS.exists("/index.html")) {
      request->send(404, "text/plain", "index.html not found");
      return;
    }
    
    File file = LittleFS.open("/index.html", "r");
    String html = file.readString();
    file.close();
    
    // Replace template variables
    html.replace("%PROJECT_NAME%", NAME);
    html.replace("%VERSION%", VERSION);
    html.replace("%IP%", deviceIP);
    html.replace("%UDP_PORT%", String(UDP_PORT));
    html.replace("%CHIP_ID%", getChipID());
    html.replace("%CPU_FREQ%", String(getCpuFrequency()));
    html.replace("%FREE_HEAP%", String(getFreeHeap() / 1024));
    html.replace("%UPTIME%", DeviceState::getUptime());
    html.replace("%CIV_BAUD%", civBaud);
    html.replace("%CIV_ADDR%", "0x" + String(CIV_ADDRESS, HEX));
    html.replace("%SERIAL1%", "RX=" + String(MY_RX1) + " TX=" + String(MY_TX1));
    html.replace("%SERIAL2%", "RX=" + String(MY_RX2) + " TX=" + String(MY_TX2));
    html.replace("%FLASH_TOTAL%", String(getFlashSize() / 1024));
    html.replace("%SKETCH_USED%", String(getSketchSize() / 1024));
    html.replace("%SKETCH_FREE%", String(getFreeSketchSpace() / 1024));
    
    // Replace WebSocket server info with current discovered values
    String wsServerInfo = "Not discovered";
    if (lastDiscoveredIP.length() > 0) {
      wsServerInfo = lastDiscoveredIP;
      if (lastDiscoveredPort.length() > 0) {
        wsServerInfo += ":" + lastDiscoveredPort;
      }
    }
    html.replace("Not discovered", wsServerInfo);
    
    request->send(200, "text/html", html); });

  // Serve static CSS and JS files
  httpServer.serveStatic("/style.css", LittleFS, "/style.css");
  httpServer.serveStatic("/app.js", LittleFS, "/app.js");

  httpServer.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
                {
                  request->send(204); // No Content
                });
  httpServer.on("/reset_stats", HTTP_POST, [](AsyncWebServerRequest *req)
                {
      resetAllStats();
      DynamicJsonDocument doc(128);
      doc["status"] = "ok";
      String response;
      serializeJson(doc, response);
      req->send(200, "application/json", response);
      broadcastStatus(); });
  httpServer.addHandler(&wsServer);
  httpServer.begin();
  DBG_PRINTLN("HTTP server started on port 80");

  if (!MDNS.begin(MDNS_NAME))
  {
    DBG_PRINTLN("Error setting up mDNS responder!");
  }
  else
  {
    DBG_PRINT("mDNS responder started: http://");
    Logger::debug("mDNS name: " + String(MDNS_NAME));
    DBG_PRINTLN(".local");
  }

  udp.begin(UDP_PORT);

  preferences.begin("config", false);
  civBaud = storedCivBaud;

  // Validate baud rate
  if (!isValidBaudRate(civBaud))
  {
    Logger::warning("Invalid baud rate specified: " + civBaud + ", using default 19200");
    civBaud = "19200";
  }

  preferences.putString("civ_baud", civBaud);
  preferences.end();
  broadcastStatus();

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
    if (millis() - lastBlink > OTA_BLINK_INTERVAL_MS) { // Blink every 200ms
      ledState = !ledState;
      setRgb(ledState ? 64 : 0, ledState ? 64 : 0, ledState ? 64 : 0); // Blink white
      lastBlink = millis();
    }
    Logger::info("OTA Progress: " + String((progress * 100) / total) + "%"); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    Logger::error("OTA Error: " + String(error));
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
  Logger::info("Serial1 (CI-V (A)) started at " + String(baud) + " baud on RX=" + String(MY_RX1) + " TX=" + String(MY_TX1) + " (1KB buffers)");
  Serial2.setRxBufferSize(1024);
  Serial2.setTxBufferSize(1024);
  Serial2.begin(baud, SERIAL_8N1, MY_RX2, MY_TX2);
  Logger::info("Serial2 (CI-V (B)) started at " + String(baud) + " baud on RX=" + String(MY_RX2) + " TX=" + String(MY_TX2) + " (1KB buffers)");

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
    Logger::error("Failed to create ciV_UDP_Task!");
    ESP.restart(); // Restart if critical task creation fails
  }

  // Start core 1 UDP/OTP broadcast task (remains on Core 1 if needed)
  result = xTaskCreatePinnedToCore(core1UdpOtpTask, "core1UdpOtpTask", 6144, NULL, 1, NULL, 1);
  if (result != pdPASS)
  {
    Logger::warning("Failed to create core1UdpOtpTask - continuing without it");
  }

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
    DynamicJsonDocument tcpDoc(1024);
    tcpDoc["ip"] = deviceIP;
    tcpDoc["ws_status"] = (connectionState == CONNECTED) ? "connected" : "disconnected";
    tcpDoc["ws_status_clients"] = getWsClientCount();
    tcpDoc["version"] = String(VERSION);
    tcpDoc["uptime"] = DeviceState::getUptime();
    tcpDoc["chip_id"] = getChipID();
    tcpDoc["cpu_freq"] = String(getCpuFrequency());
    tcpDoc["free_heap"] = String(getFreeHeap() / 1024);
    tcpDoc["civ_baud"] = civBaud;
    tcpDoc["civ_addr"] = "0x" + String(CIV_ADDRESS, HEX);
    tcpDoc["serial1"] = "RX=" + String(MY_RX1) + " TX=" + String(MY_TX1);
    tcpDoc["serial2"] = "RX=" + String(MY_RX2) + " TX=" + String(MY_TX2);
    tcpDoc["ws_status_updated"] = millis();

    String tcpStatus;
    serializeJson(tcpDoc, tcpStatus);
    tcpStatus += "\n";
    tcpClient.print(tcpStatus);
    // Simple: close connection after sending
    tcpClient.stop();
  }
  esp_task_wdt_reset(); // Feed after TCP client processing

  unsigned long now = millis();

  // Auto discovery and connection management
  if (connectionState == DISCOVERING)
  {
    if (now - lastDiscoveryAttempt > DISCOVERY_INTERVAL_MS)
    {
      lastDiscoveryAttempt = now;
      int packetSize = udp.parsePacket();
      if (packetSize > 0)
      {
        char packetBuffer[TCP_PACKET_BUFFER_SIZE];
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
            Logger::info("Discovered ShackMate IP: " + ip + " Port: " + port);
            lastDiscoveredIP = ip;
            lastDiscoveredPort = port;
            connectionState = CONNECTING;
            broadcastStatus(); // Immediately update web UI with discovered server info
          }
        }
      }
    }
  }

  if (connectionState == CONNECTING && lastDiscoveredIP.length() && lastDiscoveredPort.length() && !wsConnectPending)
  {
    Logger::info("Attempting WebSocket connection to " + lastDiscoveredIP + ":" + lastDiscoveredPort);
    webClient.begin(lastDiscoveredIP, lastDiscoveredPort.toInt(), "/");
    wsConnectPending = true;
    // Wait for CONNECTED/DISCONNECTED event
  }
  esp_task_wdt_reset(); // Feed after discovery/connect

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
    else if (millis() - wifiResetPressStart >= WIFI_RESET_HOLD_TIME_MS)
    {
      // Erase WiFi creds after 5 second hold
      preferences.begin("wifi", false);
      preferences.clear();
      preferences.end();
      WiFi.disconnect(true, true);
      Logger::warning("WiFi credentials erased! Rebooting in 2 seconds...");
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

  // Periodic memory health check (every 30 seconds)
  static unsigned long lastMemoryCheck = 0;
  if (now - lastMemoryCheck > 30000)
  {
    checkMemoryHealth();
    lastMemoryCheck = now;
  }

  // Periodic status update (every 2 seconds) to keep uptime current
  static unsigned long lastStatusUpdate = 0;
  if (now - lastStatusUpdate > 2000)
  {
    broadcastStatus();
    lastStatusUpdate = now;
  }

  esp_task_wdt_reset(); // Feed after status push

  vTaskDelay(1 / portTICK_PERIOD_MS);
}

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
// -------------------------------------------------------------------------
// ShackMateCore Library Includes
// -------------------------------------------------------------------------
#include <civ_config.h>
#include <logger.h>
#include <device_state.h>
#include <network_manager.h>
#include <json_builder.h>
#include <civ_handler.h>
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

// CI-V Serial Port Handlers
CivHandler::SerialHandler serial1Handler(Serial1, "Serial1");
CivHandler::SerialHandler serial2Handler(Serial2, "Serial2");
volatile uint32_t stat_ws_rx = 0;
volatile uint32_t stat_ws_tx = 0;
volatile uint32_t stat_ws_dup = 0;
// Reboot counter (persisted in NVS Preferences)
uint32_t reboot_counter = 0;

// Flag to signal that status update is needed (non-blocking communication between cores)
volatile bool statusUpdatePending = false;

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
// Task Priority Definitions (higher number = higher priority)
// -------------------------------------------------------------------------
#define PRIORITY_CIV_PROCESSING 4 // HIGHEST - Real-time CI-V serial processing
#define PRIORITY_NETWORK 2        // MEDIUM - Network I/O, WebSocket, HTTP
#define PRIORITY_MONITORING 1     // LOW - Background tasks, memory checks
#define PRIORITY_IDLE 0           // LOWEST - Idle tasks only
#define PRIORITY_WEBUI_EVENTS 1   // LOW - WebUI event handling

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

// Use constants from config.h instead of redefining
// WS_PING_INTERVAL_MS, WS_PING_TIMEOUT_MS, etc. are now in config.h
const unsigned long WS_CONNECTION_TIMEOUT_MS = 15000; // Connection establishment timeout

// WebSocket Connection Quality Metrics
// Message rate limiting
unsigned long last_message_time = 0;
uint32_t messages_this_second = 0;
unsigned long rate_limit_window_start = 0;

// -------------------------------------------------------------------------
// Global Objects & Variables
// -------------------------------------------------------------------------
Preferences preferences;
WiFiUDP udp;
String deviceIP = "";
String civBaud = "19200";

// DeviceState will handle uptime tracking
// bootTime removed, managed by DeviceState now

WebSocketsClient webClient;

bool otaInProgress = false;

// Mutex for protecting shared serial message strings (for potential future use)
portMUX_TYPE serialMsgMutex = portMUX_INITIALIZER_UNLOCKED;

// --- CPU Usage Monitoring ---
volatile uint32_t idle0_ticks = 0;
volatile uint32_t idle1_ticks = 0;
uint8_t cpu0_usage = 0;
uint8_t cpu1_usage = 0;
uint32_t last_idle0 = 0, last_idle1 = 0;
uint64_t lastCpuSample = 0;

// --- RTOS Event-Driven WebUI Updates ---
EventGroupHandle_t webui_events;

// Event bits for different update types
#define EVENT_STATUS_UPDATE (1 << 0)    // General status update
#define EVENT_SERIAL_STATS (1 << 1)     // Serial statistics changed
#define EVENT_WS_STATUS (1 << 2)        // WebSocket status changed
#define EVENT_CPU_USAGE (1 << 3)        // CPU usage updated
#define EVENT_MEMORY_UPDATE (1 << 4)    // Memory info changed
#define EVENT_CONFIG_CHANGE (1 << 5)    // Configuration changed
#define EVENT_DISCOVERY_UPDATE (1 << 6) // Discovery status changed

// Trigger functions for events
void triggerStatusUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_STATUS_UPDATE);
  }
}

void triggerSerialStatsUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_SERIAL_STATS);
  }
}

void triggerWebSocketStatusUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_WS_STATUS);
  }
}

void triggerCpuUsageUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_CPU_USAGE);
  }
}

void triggerMemoryUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_MEMORY_UPDATE);
  }
}

void triggerDiscoveryUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_DISCOVERY_UPDATE);
  }
}

void triggerConfigUpdate()
{
  if (webui_events)
  {
    xEventGroupSetBits(webui_events, EVENT_CONFIG_CHANGE);
  }
}

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
// CPU idle tasks for usage monitoring
void cpu0IdleTask(void *parameter);
void cpu1IdleTask(void *parameter);
// WebUI event handler task
void webuiEventTask(void *parameter);

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
  serial1Handler.resetStats();
  serial2Handler.resetStats();
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
// WebSocket Reliability Functions
// -------------------------------------------------------------------------
bool isMessageRateLimited()
{
  unsigned long now = millis();

  // Reset counter every second
  if (now - rate_limit_window_start >= 1000)
  {
    rate_limit_window_start = now;
    messages_this_second = 0;
  }

  if (messages_this_second >= WS_MESSAGE_RATE_LIMIT)
  {
    auto &ws_metrics = DeviceState::getWebSocketMetrics();
    ws_metrics.messages_rate_limited++;
    DeviceState::updateWebSocketMetrics(ws_metrics);
    return true;
  }

  messages_this_second++;
  return false;
}

void sendWebSocketPing()
{
  auto &ws_metrics = DeviceState::getWebSocketMetrics();
  if (webClient.isConnected() && !ws_metrics.ping_pending)
  {
    ws_metrics.last_ping_sent = millis();
    ws_metrics.ping_pending = true;
    DeviceState::updateWebSocketMetrics(ws_metrics);
    webClient.sendPing();
    Logger::debug("WebSocket ping sent");
  }
}

void calculateConnectionQuality()
{
  auto &ws_metrics = DeviceState::getWebSocketMetrics();
  unsigned long now = millis();
  unsigned long uptime = now - ws_metrics.last_pong_received;

  if (ws_metrics.total_disconnects == 0 && ws_metrics.ping_rtt < 1000)
  {
    ws_metrics.connection_quality = 100;
  }
  else if (ws_metrics.total_disconnects < 5 && ws_metrics.ping_rtt < 2000)
  {
    ws_metrics.connection_quality = 80;
  }
  else if (ws_metrics.total_disconnects < 10 && ws_metrics.ping_rtt < 5000)
  {
    ws_metrics.connection_quality = 60;
  }
  else
  {
    ws_metrics.connection_quality = 40;
  }

  // Reduce quality if connection is stale
  if (uptime > 60000)
  { // More than 1 minute since last pong
    ws_metrics.connection_quality = min(ws_metrics.connection_quality, (uint8_t)20);
  }

  DeviceState::updateWebSocketMetrics(ws_metrics);
}

void attemptWebSocketReconnection()
{
  auto &ws_metrics = DeviceState::getWebSocketMetrics();
  if (ws_metrics.reconnect_attempts < WS_MAX_RECONNECT_ATTEMPTS)
  {
    unsigned long delay_ms = WS_RECONNECT_DELAY_MS * (1 + ws_metrics.reconnect_attempts);
    Logger::info("Attempting WebSocket reconnection in " + String(delay_ms) + "ms (attempt " +
                 String(ws_metrics.reconnect_attempts + 1) + "/" + String(WS_MAX_RECONNECT_ATTEMPTS) + ")");

    delay(delay_ms);

    if (lastDiscoveredIP.length() > 0 && lastDiscoveredPort.length() > 0)
    {
      webClient.begin(lastDiscoveredIP, lastDiscoveredPort.toInt(), "/");
      ws_metrics.reconnect_attempts++;
      DeviceState::updateWebSocketMetrics(ws_metrics);
    }
  }
  else
  {
    Logger::warning("Max WebSocket reconnection attempts reached, falling back to discovery");
    connectionState = DISCOVERING;
    auto &ws_metrics = DeviceState::getWebSocketMetrics();
    ws_metrics.reconnect_attempts = 0;
    DeviceState::updateWebSocketMetrics(ws_metrics);
  }
}

// -------------------------------------------------------------------------
// WebSocket forwarding helper function for CI-V frames
// -------------------------------------------------------------------------

void forwardFrameToWebSocket(const char *frameData, size_t frameLen)
{
  if (!webClient.isConnected())
  {
    return; // No WebSocket connection
  }

  // Convert frame to hex string (without serial port prefix)
  String hex;
  for (size_t i = 0; i < frameLen; ++i)
  {
    char b[4];
    sprintf(b, "%02X ", (uint8_t)frameData[i]);
    hex += b;
  }
  hex.trim();

  // Check for duplicates
  if (!isDuplicateMessage(hex))
  {
    if (!isMessageRateLimited())
    {
      webClient.sendTXT(hex);
      addMessageToCache(hex);
      stat_ws_tx++;
      auto &ws_metrics = DeviceState::getWebSocketMetrics();
      ws_metrics.messages_sent++;
      DeviceState::updateWebSocketMetrics(ws_metrics);
      triggerSerialStatsUpdate(); // Event-driven update for TX
    }
    else
    {
      Logger::warning("WebSocket message rate limited");
    }
  }
  else
  {
    stat_ws_dup++;
    triggerSerialStatsUpdate(); // Event-driven update for duplicate
  }
}

// Serial port specific callback functions
void forwardSerial1FrameToWebSocket(const char *frameData, size_t frameLen)
{
  forwardFrameToWebSocket(frameData, frameLen);
}

void forwardSerial2FrameToWebSocket(const char *frameData, size_t frameLen)
{
  forwardFrameToWebSocket(frameData, frameLen);
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
  // Check if there are any connected WebSocket clients first
  if (getWsClientCount() == 0)
  {
    return; // No clients, skip broadcast
  }

  DynamicJsonDocument doc(1024);
  doc["ip"] = deviceIP;
  doc["ws_status"] = (connectionState == CONNECTED) ? "connected" : "disconnected";
  doc["ws_status_clients"] = getWsClientCount();
  doc["ws_server_ip"] = lastDiscoveredIP.length() > 0 ? lastDiscoveredIP : "Not discovered";
  doc["ws_server_port"] = lastDiscoveredPort.length() > 0 ? lastDiscoveredPort : "";
  doc["version"] = String(VERSION);
  doc["uptime"] = DeviceState::getUptime();
  doc["reboots"] = reboot_counter;
  // Use ShackMateCore system info where possible
  doc["chip_id"] = getChipID();
  doc["cpu_freq"] = String(getCpuFrequency());
  doc["free_heap"] = String(getFreeHeap() / 1024);
  doc["civ_baud"] = civBaud;
  doc["civ_addr"] = "0x" + String(CIV_ADDRESS, HEX);
  doc["serial1"] = "RX=" + String(MY_RX1) + " TX=" + String(MY_TX1);
  doc["serial2"] = "RX=" + String(MY_RX2) + " TX=" + String(MY_TX2);
  // Communications statistics from CI-V handlers
  const auto &serial1Stats = serial1Handler.getStats();
  const auto &serial2Stats = serial2Handler.getStats();

  doc["serial1_frames"] = serial1Stats.totalFrames;
  doc["serial1_valid"] = serial1Stats.validFrames;
  doc["serial1_invalid"] = serial1Stats.totalFrames - serial1Stats.validFrames;
  doc["serial1_corrupted"] = serial1Stats.corruptFrames;
  doc["serial1_broadcast"] = serial1Stats.broadcastFrames;
  doc["serial2_frames"] = serial2Stats.totalFrames;
  doc["serial2_valid"] = serial2Stats.validFrames;
  doc["serial2_invalid"] = serial2Stats.totalFrames - serial2Stats.validFrames;
  doc["serial2_corrupted"] = serial2Stats.corruptFrames;
  doc["serial2_broadcast"] = serial2Stats.broadcastFrames;
  doc["ws_rx"] = stat_ws_rx;
  doc["ws_tx"] = stat_ws_tx;
  doc["ws_dup"] = stat_ws_dup;

  // WebSocket reliability metrics
  const auto &ws_metrics = DeviceState::getWebSocketMetrics();
  doc["ws_ping_rtt"] = ws_metrics.ping_rtt;
  doc["ws_connection_quality"] = ws_metrics.connection_quality;
  doc["ws_total_disconnects"] = ws_metrics.total_disconnects;
  doc["ws_messages_sent"] = ws_metrics.messages_sent;
  doc["ws_rate_limited"] = ws_metrics.messages_rate_limited;
  doc["ws_reconnect_attempts"] = ws_metrics.reconnect_attempts;

  doc["ws_status_updated"] = millis();

  // Task performance metrics
  doc["civ_task_priority"] = PRIORITY_CIV_PROCESSING;
  doc["loop_task_priority"] = uxTaskPriorityGet(NULL); // Current task (loop) priority

  // CPU usage
  doc["cpu0_usage"] = cpu0_usage;
  doc["cpu1_usage"] = cpu1_usage;

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
    auto &ws_metrics = DeviceState::getWebSocketMetrics();
    ws_metrics.reconnect_attempts = 0;        // Reset reconnection counter on successful connect
    ws_metrics.last_pong_received = millis(); // Initialize pong timestamp
    DeviceState::updateWebSocketMetrics(ws_metrics);
    Logger::info("WebSocket client connected to " + lastDiscoveredIP + ":" + lastDiscoveredPort);
    setRgb(0, 0, 64); // BLUE on websocket connect
    connectionState = CONNECTED;
    triggerWebSocketStatusUpdate(); // Event-driven update

    // Configure connection settings for reliability
    webClient.setReconnectInterval(WS_RECONNECT_DELAY_MS);
    webClient.enableHeartbeat(WS_PING_INTERVAL_MS, WS_PING_TIMEOUT_MS, 2);
    return;
  }

  if (type == WStype_DISCONNECTED)
  {
    wsConnectPending = false;
    auto &ws_metrics = DeviceState::getWebSocketMetrics();
    ws_metrics.total_disconnects++;
    ws_metrics.ping_pending = false;
    DeviceState::updateWebSocketMetrics(ws_metrics);
    Logger::warning("WebSocket client disconnected (total: " + String(ws_metrics.total_disconnects) + ")");

    if (WiFi.isConnected())
    {
      setRgb(0, 64, 0); // GREEN if still on WiFi
      // Attempt fast reconnection instead of full discovery
      connectionState = CONNECTING;
      attemptWebSocketReconnection();
    }
    else
    {
      setRgb(255, 0, 0); // RED if WiFi lost
      connectionState = DISCOVERING;
    }
    triggerWebSocketStatusUpdate(); // Event-driven update
    return;
  }

  if (type == WStype_PONG)
  {
    auto &ws_metrics = DeviceState::getWebSocketMetrics();
    if (ws_metrics.ping_pending)
    {
      ws_metrics.ping_rtt = millis() - ws_metrics.last_ping_sent;
      ws_metrics.last_pong_received = millis();
      ws_metrics.ping_pending = false;
      DeviceState::updateWebSocketMetrics(ws_metrics);
      calculateConnectionQuality();
      Logger::debug("WebSocket pong received (RTT: " + String(ws_metrics.ping_rtt) + "ms)");
    }
    return;
  }

  if (type == WStype_PING)
  {
    Logger::debug("WebSocket ping received, pong sent automatically");
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

  // Filter broadcast commands: Only allow broadcast (toAddr = 0x00) if from management address (0xEE)
  if (byteCount >= 4) // Need at least FE FE toAddr fromAddr
  {
    uint8_t toAddr = buffer[2];
    uint8_t fromAddr = buffer[3];
    
    if (toAddr == 0x00 && fromAddr != 0xEE)
    {
      Logger::warning("Filtered broadcast command from non-management address 0x" + String(fromAddr, HEX) + " - dropping");
      return; // Drop the command
    }
  }

  // Forward to both serial ports (legacy behavior for non-broadcast or EE commands)
  Serial1.write(buffer, byteCount);
  Serial1.flush();
  Serial2.write(buffer, byteCount);
  Serial2.flush();

  Logger::debug("WebSocket -> Serial1 & Serial2: " + msg);
}

// -------------------------------------------------------------------------
// CI-V Task Function (runs on Core 1)
// -------------------------------------------------------------------------

// myTaskDebug: Handles CI-V serial/UDP processing ONLY, runs on Core 1.
void myTaskDebug(void *parameter)
{
  DBG_PRINTLN("CI-V task started with new modular handlers");
  esp_task_wdt_add(NULL); // Add this task to watchdog

  for (;;)
  {
    esp_task_wdt_reset(); // Feed watchdog for this task

    // Process incoming data on both serial ports using handlers
    bool serial1Activity = serial1Handler.processIncoming();
    bool serial2Activity = serial2Handler.processIncoming();

    // Trigger WebUI updates if there was any activity
    if (serial1Activity || serial2Activity)
    {
      triggerSerialStatsUpdate();
    }

    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

// -------------------------------------------------------------------------
// WebUI Event Handler Task (runs on Core 0)
// Waits for events and sends targeted updates to WebSocket clients
// -------------------------------------------------------------------------
void webuiEventTask(void *parameter)
{
  esp_task_wdt_add(NULL); // Add this task to watchdog

  const EventBits_t ALL_EVENTS = EVENT_STATUS_UPDATE | EVENT_SERIAL_STATS |
                                 EVENT_WS_STATUS | EVENT_CPU_USAGE |
                                 EVENT_MEMORY_UPDATE | EVENT_CONFIG_CHANGE |
                                 EVENT_DISCOVERY_UPDATE;

  while (1)
  {
    esp_task_wdt_reset(); // Feed watchdog

    // Wait for any event with 1000ms timeout
    EventBits_t eventBits = xEventGroupWaitBits(
        webui_events,       // Event group handle
        ALL_EVENTS,         // Bits to wait for
        pdTRUE,             // Clear bits on exit
        pdFALSE,            // Wait for ANY bit (not all)
        pdMS_TO_TICKS(1000) // 1 second timeout
    );

    // Skip if no WebSocket clients connected
    if (getWsClientCount() == 0)
    {
      continue;
    }

    // Handle specific events with targeted updates
    if (eventBits & EVENT_CPU_USAGE)
    {
      DynamicJsonDocument doc(256);
      doc["cpu0_usage"] = cpu0_usage;
      doc["cpu1_usage"] = cpu1_usage;
      String json;
      serializeJson(doc, json);
      wsServer.textAll(json);
    }

    if (eventBits & EVENT_MEMORY_UPDATE)
    {
      DynamicJsonDocument doc(256);
      doc["free_heap"] = String(getFreeHeap() / 1024);
      String json;
      serializeJson(doc, json);
      wsServer.textAll(json);
    }

    if (eventBits & EVENT_SERIAL_STATS)
    {
      DynamicJsonDocument doc(768); // Increased size for WebSocket metrics
      const auto &serial1Stats = serial1Handler.getStats();
      const auto &serial2Stats = serial2Handler.getStats();

      doc["serial1_valid"] = serial1Stats.validFrames;
      doc["serial1_invalid"] = serial1Stats.totalFrames - serial1Stats.validFrames;
      doc["serial1_broadcast"] = serial1Stats.broadcastFrames;
      doc["serial2_valid"] = serial2Stats.validFrames;
      doc["serial2_invalid"] = serial2Stats.totalFrames - serial2Stats.validFrames;
      doc["serial2_broadcast"] = serial2Stats.broadcastFrames;
      doc["ws_rx"] = stat_ws_rx;
      doc["ws_tx"] = stat_ws_tx;
      doc["ws_dup"] = stat_ws_dup;

      // WebSocket reliability metrics
      const auto &ws_metrics = DeviceState::getWebSocketMetrics();
      doc["ws_ping_rtt"] = ws_metrics.ping_rtt;
      doc["ws_connection_quality"] = ws_metrics.connection_quality;
      doc["ws_total_disconnects"] = ws_metrics.total_disconnects;
      doc["ws_messages_sent"] = ws_metrics.messages_sent;
      doc["ws_rate_limited"] = ws_metrics.messages_rate_limited;
      doc["ws_reconnect_attempts"] = ws_metrics.reconnect_attempts;

      String json;
      serializeJson(doc, json);
      wsServer.textAll(json);
    }

    if (eventBits & EVENT_WS_STATUS)
    {
      const auto &ws_metrics = DeviceState::getWebSocketMetrics();
      DynamicJsonDocument doc(512); // Increased size for additional metrics
      doc["ws_status"] = (connectionState == CONNECTED) ? "connected" : "disconnected";
      doc["ws_server_ip"] = lastDiscoveredIP.length() > 0 ? lastDiscoveredIP : "Not discovered";
      doc["ws_server_port"] = lastDiscoveredPort.length() > 0 ? lastDiscoveredPort : "";

      // Include connection quality in status updates
      doc["ws_ping_rtt"] = ws_metrics.ping_rtt;
      doc["ws_connection_quality"] = ws_metrics.connection_quality;
      doc["ws_total_disconnects"] = ws_metrics.total_disconnects;
      doc["ws_messages_sent"] = ws_metrics.messages_sent;
      doc["ws_rate_limited"] = ws_metrics.messages_rate_limited;
      doc["ws_reconnect_attempts"] = ws_metrics.reconnect_attempts;

      String json;
      serializeJson(doc, json);
      wsServer.textAll(json);
    }

    if (eventBits & EVENT_DISCOVERY_UPDATE)
    {
      triggerWebSocketStatusUpdate(); // Chain to WS status update
    }

    if (eventBits & EVENT_STATUS_UPDATE)
    {
      // Full status update - fallback for compatibility
      broadcastStatus();
    }

    // Memory cleanup and small delay
    vTaskDelay(pdMS_TO_TICKS(10));
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
  // Reset WebSocket statistics to zero on boot (CI-V stats handled by handlers)
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

  // Initialize RTOS event group for WebUI updates
  webui_events = xEventGroupCreate();
  if (webui_events == NULL)
  {
    Logger::error("Failed to create WebUI event group!");
    ESP.restart();
  }
  Logger::info("WebUI event group created successfully");

  // Load and increment reboot counter
  Preferences rebootPrefs;
  rebootPrefs.begin("sys", false);
  reboot_counter = rebootPrefs.getUInt("reboots", 0) + 1;
  rebootPrefs.putUInt("reboots", reboot_counter);
  rebootPrefs.end();
  LOG_INFO("Reboot count: " + String(reboot_counter));

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
  triggerStatusUpdate(); // Event-driven status update after WiFi connection

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
                  triggerSerialStatsUpdate(); // Event-driven stats update after reset
                  triggerStatusUpdate();      // Also trigger status update for completeness
                });
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
  triggerConfigUpdate(); // Event-driven config update after baud rate change

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
                     setRgb(0, 64, 0);            // Green after OTA done
                     triggerStatusUpdate();       // Event-driven status update after OTA completion
                   });
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

  // Initialize CI-V handlers with buffer configuration
  Serial1.setRxBufferSize(2048); // Increased from 1024 to 2048
  Serial1.setTxBufferSize(2048); // Increased from 1024 to 2048
  Serial2.setRxBufferSize(2048); // Increased from 1024 to 2048
  Serial2.setTxBufferSize(2048); // Increased from 1024 to 2048

  serial1Handler.begin(baud, MY_RX1, MY_TX1);
  serial2Handler.begin(baud, MY_RX2, MY_TX2);

  // Set up WebSocket forwarding callbacks for CI-V handlers
  serial1Handler.setFrameCallback(forwardSerial1FrameToWebSocket);
  serial2Handler.setFrameCallback(forwardSerial2FrameToWebSocket);

  // -------------------------------------------------------------------------
  // TASK/CORE ALLOCATION:
  //   Core 0: Arduino loop() (networking, OTA, web server, TCP, async, etc.)
  //           [Optional] core0Task for system monitoring (if needed)
  //   Core 1: myTaskDebug (CI-V serial/UDP processing only)
  //           core1UdpOtpTask (UDP/OTP broadcast, if needed)
  // -------------------------------------------------------------------------
  // Create a separate task for CI-V/UDP processing on Core 1 with HIGHEST priority
  BaseType_t result = xTaskCreatePinnedToCore(myTaskDebug, "ciV_UDP_Task", 4096, NULL, PRIORITY_CIV_PROCESSING, NULL, 1);
  if (result != pdPASS)
  {
    Logger::error("Failed to create ciV_UDP_Task!");
    ESP.restart(); // Restart if critical task creation fails
  }
  Logger::info("CI-V task created with HIGHEST priority (" + String(PRIORITY_CIV_PROCESSING) + ") on Core 1");

  // Start core 1 UDP/OTP broadcast task with lower priority
  result = xTaskCreatePinnedToCore(core1UdpOtpTask, "core1UdpOtpTask", 6144, NULL, PRIORITY_MONITORING, NULL, 1);
  if (result != pdPASS)
  {
    Logger::warning("Failed to create core1UdpOtpTask - continuing without it");
  }

  // Start CPU idle tasks for usage monitoring
  xTaskCreatePinnedToCore(cpu0IdleTask, "cpu0IdleTask", 2048, NULL, 0, NULL, 0); // Core 0
  xTaskCreatePinnedToCore(cpu1IdleTask, "cpu1IdleTask", 2048, NULL, 0, NULL, 1); // Core 1

  // Start WebUI event handler task on Core 0
  result = xTaskCreatePinnedToCore(webuiEventTask, "webuiEventTask", 4096, NULL, PRIORITY_WEBUI_EVENTS, NULL, 0);
  if (result != pdPASS)
  {
    Logger::warning("Failed to create webuiEventTask - using fallback polling");
  }
  else
  {
    Logger::info("WebUI event task created on Core 0");
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
            triggerDiscoveryUpdate(); // Event-driven update for discovery
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
      setRgb(255, 140, 0);   // Orange for erase event
      triggerStatusUpdate(); // Event-driven status update before reboot
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

  // Event-driven memory and uptime updates (every 2 seconds)
  static unsigned long lastMemoryUpdate = 0;
  if (now - lastMemoryUpdate > 2000)
  {
    triggerMemoryUpdate(); // Event-driven memory status update
    lastMemoryUpdate = now;
  }

  // CPU usage monitoring
  uint64_t now_us = esp_timer_get_time();
  if (now_us - lastCpuSample > 2000000)
  { // 2 seconds in microseconds
    uint32_t cur_idle0 = idle0_ticks, cur_idle1 = idle1_ticks;
    uint32_t idle0_delta = cur_idle0 - last_idle0;
    uint32_t idle1_delta = cur_idle1 - last_idle1;
    // 2s / 1ms per vTaskDelay(1) = 2000 expected ticks if fully idle
    cpu0_usage = 100 - ((idle0_delta * 100) / 2000);
    cpu1_usage = 100 - ((idle1_delta * 100) / 2000);
    if (cpu0_usage > 100)
      cpu0_usage = 100;
    if (cpu1_usage > 100)
      cpu1_usage = 100;
    last_idle0 = cur_idle0;
    last_idle1 = cur_idle1;
    lastCpuSample = now_us;

    // Trigger CPU usage update event
    triggerCpuUsageUpdate();
  }

  esp_task_wdt_reset(); // Feed after status push

  // -------------------------------------------------------------------------
  // WebSocket health monitoring and maintenance
  // -------------------------------------------------------------------------
  static unsigned long last_ping_check = 0;
  static unsigned long last_quality_check = 0;

  if (webClient.isConnected())
  {
    // Send periodic pings to maintain connection
    if (now - last_ping_check > WS_PING_INTERVAL_MS)
    {
      sendWebSocketPing();
      last_ping_check = now;
    }

    // Check for stale connections (ping timeout)
    auto &ws_metrics = DeviceState::getWebSocketMetrics();
    if (ws_metrics.ping_pending &&
        (now - ws_metrics.last_ping_sent > WS_PING_TIMEOUT_MS))
    {
      Logger::warning("WebSocket ping timeout, forcing reconnection");
      webClient.disconnect();
      ws_metrics.ping_pending = false;
      DeviceState::updateWebSocketMetrics(ws_metrics);
    }

    // Update connection quality metrics
    if (now - last_quality_check > 5000)
    { // Every 5 seconds
      calculateConnectionQuality();
      last_quality_check = now;
    }
  }

  esp_task_wdt_reset(); // Feed after WebSocket health checks

  vTaskDelay(1 / portTICK_PERIOD_MS);
}

// -------------------------------------------------------------------------
// CPU Idle Tasks for Usage Monitoring
// -------------------------------------------------------------------------
void cpu0IdleTask(void *parameter)
{
  while (1)
  {
    idle0_ticks++;
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void cpu1IdleTask(void *parameter)
{
  while (1)
  {
    idle1_ticks++;
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
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
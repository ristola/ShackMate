/*
ShackMate Power Outlet - WebSocket Control Interface for the Wyze Outdoor Outlet Model: WLPPO1

Control Output 1 and Output 2 via WebSocket Port 4000
Valid Commands:
  { "command": "output1", "value": true }
  { "command": "output1", "value": false }
  { "command": "output2", "value": true }
  { "command": "output2", "value": false }

Debug/Maintenance Commands:
  { "command": "resetRebootCounter" }  // Resets the boot counter to 0
  { "command": "testCaptivePortal", "enable": true }   // Enable captive portal status LED test mode
  { "command": "testCaptivePortal", "enable": false }  // Disable captive portal status LED test mode
  { "command": "testStatusLED" }                       // Toggle status LED once for testing
  { "command": "testLEDHardware" }                     // Comprehensive LED hardware test
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
#include <esp_task_wdt.h>     // For watchdog timer control during OTA
#include <WebSocketsClient.h> // For client WebSocket connections
#include "SMCIV.h"            // For CI-V message parsing and handling
#include <vector>             // For CI-V message data storage

// New modular components (now in ShackMateCore library)
#include <config.h>
#include <logger.h>
#include <device_state.h>
#include <hardware_controller.h>
#include <json_builder.h>
#include <network_manager.h>

// Hardware timer for LED blinking
hw_timer_t *ledTimer = nullptr;
volatile bool timerTriggered = false;
volatile uint32_t timerInterruptCount = 0;

// Timer interrupt handler for LED blinking
void IRAM_ATTR onLedTimer()
{
  timerTriggered = true;
  timerInterruptCount++; // Count interrupts for debugging
}

// Initialize LED timer for captive portal blinking
void initLedTimer()
{
  ledTimer = timerBegin(0, 80, true);                // Timer 0, prescaler 80 (1MHz), count up
  timerAttachInterrupt(ledTimer, &onLedTimer, true); // Edge interrupt
  timerAlarmWrite(ledTimer, 250000, true);           // 250ms interval (250,000 microseconds)
  Serial.println("LED timer initialized for captive portal blinking");
}

// Start LED blinking timer
void startLedBlinking()
{
  if (ledTimer)
  {
    timerAlarmEnable(ledTimer);
    Serial.println("LED blinking timer started");
  }
}

// Stop LED blinking timer
void stopLedBlinking()
{
  if (ledTimer)
  {
    timerAlarmDisable(ledTimer);
    Serial.println("LED blinking timer stopped");
  }
}

// -------------------------------------------------------------------------
// Project Constants (now using config.h)
// -------------------------------------------------------------------------
// Note: Most constants now come from config.h

// Define total available RAM (adjust if needed)
#define TOTAL_RAM 327680

// Sensor pins and calibration (now using config.h)
// Note: Pin definitions and calibration constants now come from config.h

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

// Pin Definitions (now using config.h)
// Note: All pin definitions now come from config.h

// -------------------------------------------------------------------------
// Global Objects & Variables
// -------------------------------------------------------------------------
DeviceState deviceState;     // Centralized state management
HardwareController hardware; // Hardware abstraction layer
Preferences preferences;
AsyncWebServer httpServer(80);
AsyncWebServer wsServer(4000);
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

uint32_t rebootCounter = 0;
unsigned long lastSensorUpdate = 0;
uint8_t deviceId = 1;
String civAddress = "B0";
bool relay1State = false;
bool relay2State = false;
char label1Text[32];
char label2Text[32];
char deviceName[64] = "ShackMate Power Outlet";
float voltageCalibrationFactor = 1.0f;
bool voltageCalibrated = false;
float currentCalibrationFactor = 1.0f;
bool currentCalibrated = false;

// Captive Portal mode flag and status LED variables
bool captivePortalActive = false;
unsigned long statusLedLastToggle = 0;
bool statusLedState = false;

// Button debounce globals - TODO: Move to HardwareController
volatile bool button1Pressed = false;
volatile bool button2Pressed = false;
unsigned long lastButton1Time = 0;
unsigned long lastButton2Time = 0;
const unsigned long debounceDelay = 50;
bool lastButton1State = false;
bool lastButton2State = false;
bool button1StateStable = false;
bool button2StateStable = false;

// -------------------------------------------------------------------------
// Power Reading Validation Functions
// -------------------------------------------------------------------------

/**
 * @brief Get validated and calibrated current reading from HLW8012
 *
 * @return float Validated and calibrated current reading in amperes
 */
float getValidatedCurrent()
{
  float rawCurrent = hlw.getCurrent();
  float calibratedCurrent = rawCurrent * currentCalibrationFactor;

  // Apply basic validation - negative current doesn't make sense
  if (calibratedCurrent < 0.0f)
  {
    return 0.0f;
  }

  // Cap at reasonable maximum (20A for household outlet)
  const float MAX_REASONABLE_CURRENT = 20.0f;
  if (calibratedCurrent > MAX_REASONABLE_CURRENT)
  {
    Serial.printf("WARNING: Detected excessive current reading: %.3fA - capping at %.1fA\n",
                  calibratedCurrent, MAX_REASONABLE_CURRENT);
    return MAX_REASONABLE_CURRENT;
  }

  return calibratedCurrent;
}

/**
 * @brief Get validated power reading from HLW8012
 *
 * The HLW8012 chip can return spurious power readings (like 43,488W) when
 * current is very low or zero. This function validates the power reading
 * against the current to filter out these anomalies.
 *
 * @return float Validated power reading in watts
 */
float getValidatedPower()
{
  float current = getValidatedCurrent(); // Use calibrated current
  float rawPower = hlw.getActivePower();

  // Define thresholds for validation
  const float MIN_CURRENT_THRESHOLD = 0.05f;  // 50mA minimum for valid power reading
  const float MAX_REASONABLE_POWER = 2000.0f; // 2000W maximum reasonable power for this device

  // If current is below threshold, power should be zero or very small
  if (current < MIN_CURRENT_THRESHOLD)
  {
    return 0.0f;
  }

  // If power reading seems unreasonable compared to current, recalculate or zero it
  if (rawPower > MAX_REASONABLE_POWER)
  {
    Serial.printf("WARNING: Detected spurious power reading: %.1fW with %.3fA - setting to 0W\n", rawPower, current);
    return 0.0f;
  }

  // Basic power factor validation (power shouldn't exceed voltage * current significantly)
  float voltage = hlw.getVoltage() * voltageCalibrationFactor;
  float apparentPower = voltage * current;

  // If power is significantly higher than apparent power, it's likely spurious
  if (rawPower > (apparentPower * 1.2f)) // Allow for some power factor variation
  {
    Serial.printf("WARNING: Power reading %.1fW exceeds apparent power %.1fW (V=%.1f, I=%.3f) - setting to 0W\n",
                  rawPower, apparentPower, voltage, current);
    return 0.0f;
  }

  return rawPower;
}

// Helper function to get CI-V address as byte value
uint8_t getCivAddressByte()
{
  return DeviceState::getCivAddressByte();
}

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

// -------------------------------------------------------------------------
// Debug Helper Function - sends debug messages to WebSocket and Logger
// -------------------------------------------------------------------------
void sendDebugMessage(String message)
{
  // Log to serial via Logger system
  LOG_DEBUG(message);

  // Also send to WebSocket clients for real-time debugging
  String debugJson = "{\"type\":\"debug\",\"message\":\"" + message + "\"}";
  NetworkManager::broadcastToWebClients(debugJson);
}

// -------------------------------------------------------------------------
// CI-V Message Handling
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
  // Check maximum length to prevent excessive memory allocation
  if (cleanHex.length() < 12 || cleanHex.length() % 2 != 0 || cleanHex.length() > 128)
  {
    sendDebugMessage("CI-V: Invalid message length: " + String(cleanHex.length()) + " (min: 12, max: 128)");
    return msg;
  }

  // Convert hex string to bytes with memory bounds checking
  std::vector<uint8_t> bytes;
  bytes.reserve(cleanHex.length() / 2); // Pre-allocate to avoid reallocations

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

    // Extract data portion (after command, before terminator) with bounds checking
    if (bytes.size() > 6)
    {
      for (size_t i = 5; i < bytes.size() - 1 && msg.data.size() < 16; i++)
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

      // Extract data portion (after subcommand, before terminator) with bounds checking
      if (bytes.size() > 7)
      {
        for (size_t i = 6; i < bytes.size() - 1 && msg.data.size() < 16; i++)
        {
          msg.data.push_back(bytes[i]);
        }
      }
    }
  }

  msg.valid = true;

  // Debug output with memory optimization
  char debugBuffer[128];
  snprintf(debugBuffer, sizeof(debugBuffer),
           "CI-V: Parsed - TO:%02X FROM:%02X CMD:%02X SUB:%02X",
           msg.toAddr, msg.fromAddr, msg.command, msg.subCommand);
  sendDebugMessage(String(debugBuffer));

  return msg;
}

// Check if CI-V message is addressed to us
bool isCivMessageForUs(const CivMessage &msg)
{
  uint8_t ourAddress = getCivAddressByte();
  bool isBroadcast = (msg.toAddr == 0x00);
  bool isAddressedToUs = (msg.toAddr == ourAddress);

  // Always log broadcast messages for debugging
  if (isBroadcast)
  {
    sendDebugMessage("CI-V: BROADCAST message received - Our addr: 0x" + String(ourAddress, HEX) + " (should be B3=0xB3), FROM: 0x" + String(msg.fromAddr, HEX));
  }

  if (isBroadcast || isAddressedToUs)
  {
    String addrType = isBroadcast ? "broadcast" : "direct";
    sendDebugMessage("CI-V: Message for us (" + addrType + ") - Our addr: 0x" + String(ourAddress, HEX) + ", TO: 0x" + String(msg.toAddr, HEX));
    return true;
  }

  // Message not for us - only log if it's not a broadcast to reduce spam
  if (!isBroadcast)
  {
    sendDebugMessage("CI-V: Message not for us - Our addr: 0x" + String(ourAddress, HEX) + ", TO: 0x" + String(msg.toAddr, HEX));
  }
  return false;
}

// Process CI-V message and generate appropriate response
// Returns true if message was handled internally, false if it should be forwarded to physical CI-V port
bool processCivMessage(const CivMessage &msg)
{
  uint8_t ourCivAddr = getCivAddressByte();

  // Provide a clear summary of what command we're processing
  String commandSummary = "CI-V: Processing ";
  if (msg.command == 0x19 && msg.subCommand == 0x00)
  {
    commandSummary += "19 00 (Echo - asking for our CI-V address)";
  }
  else if (msg.command == 0x19 && msg.subCommand == 0x01)
  {
    commandSummary += "19 01 (Model ID - asking for our IP address in hex)";
  }
  else if (msg.command == 0x34)
  {
    commandSummary += "34 (Read Model - asking what type of device we are)";
  }
  else if (msg.command == 0x35 && msg.data.size() == 0)
  {
    commandSummary += "35 (Read Outlet Status - asking what outlets are on/off)";
  }
  else if (msg.command == 0x35 && msg.data.size() == 1)
  {
    commandSummary += "35 " + String(msg.data[0], HEX) + " (Set Outlet Status - telling us what outlets to turn on/off)";
  }
  else
  {
    commandSummary += String(msg.command, HEX) + " " + String(msg.subCommand, HEX) + " (Other command)";
  }
  sendDebugMessage(commandSummary);

  sendDebugMessage("CI-V: Processing command " + String(msg.command, HEX) + " subcommand " + String(msg.subCommand, HEX) + " with our address 0x" + String(ourCivAddr, HEX));

  // Convert message back to hex string for SMCIV library processing
  char hexBuffer[256];
  int hexPos = 0;

  // Use snprintf for memory-safe hex string construction
  hexPos += snprintf(hexBuffer + hexPos, sizeof(hexBuffer) - hexPos, "FE FE %02X %02X %02X",
                     msg.toAddr, msg.fromAddr, msg.command);

  if (msg.command != 0x35)
  {
    hexPos += snprintf(hexBuffer + hexPos, sizeof(hexBuffer) - hexPos, " %02X", msg.subCommand);
  }

  for (uint8_t dataByte : msg.data)
  {
    hexPos += snprintf(hexBuffer + hexPos, sizeof(hexBuffer) - hexPos, " %02X", dataByte);
  }

  hexPos += snprintf(hexBuffer + hexPos, sizeof(hexBuffer) - hexPos, " FD");

  String hexString = String(hexBuffer);
  sendDebugMessage("CI-V: Forwarding to SMCIV library: " + hexString);

  // Handle specific CI-V commands directly BEFORE forwarding to SMCIV library
  sendDebugMessage("CI-V: Checking command 0x" + String(msg.command, HEX) + " with subcommand 0x" + String(msg.subCommand, HEX));

  // Check for commands we handle directly (19 00 and 19 01)
  if (msg.command == 0x19 && (msg.subCommand == 0x00 || msg.subCommand == 0x01))
  {
    sendDebugMessage("CI-V: Handling command 19 directly - WILL NOT call SMCIV library");

    if (msg.subCommand == 0x00)
    {
      sendDebugMessage("CI-V: 19 00 - Echo request (asking for our CI-V address) - responding with 0x" + String(getCivAddressByte(), HEX));

      // Create echo response using memory-safe buffer
      char responseBuffer[32];
      snprintf(responseBuffer, sizeof(responseBuffer), "FE FE %02X %02X 19 00 %02X FD",
               msg.fromAddr, getCivAddressByte(), getCivAddressByte());
      String echoResponse = String(responseBuffer);

      sendDebugMessage("<<< CI-V OUTGOING: Echo Response (19 00) - " + echoResponse);
      sendDebugMessage("    Purpose: Confirming our CI-V address (0x" + String(getCivAddressByte(), HEX) + ") to sender (0x" + String(msg.fromAddr, HEX) + ")");

      // Send response back via remote WebSocket client only (not to web clients)
      if (NetworkManager::isClientConnected())
      {
        NetworkManager::sendToServer(echoResponse);
        sendDebugMessage("✓ CI-V: Echo response transmitted via remote WebSocket");
      }
      else
      {
        sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, echo response NOT sent");
      }
    }
    else if (msg.subCommand == 0x01)
    {
      sendDebugMessage("CI-V: 19 01 - Model ID request (asking for our IP address in hex) - responding with IP as hex");

      // Create ModelID response with IP address using memory-safe buffer
      IPAddress ip = WiFi.localIP();
      char responseBuffer[48];
      snprintf(responseBuffer, sizeof(responseBuffer),
               "FE FE %02X %02X 19 01 %02X %02X %02X %02X FD",
               msg.fromAddr, getCivAddressByte(), ip[0], ip[1], ip[2], ip[3]);
      String modelResponse = String(responseBuffer);

      // Create readable hex representation of IP
      String ipHex = String(ip[0], HEX) + " " + String(ip[1], HEX) + " " + String(ip[2], HEX) + " " + String(ip[3], HEX);
      ipHex.toUpperCase();

      sendDebugMessage("<<< CI-V OUTGOING: Model ID Response (19 01) - " + modelResponse);
      sendDebugMessage("    Purpose: Sending our IP address in hex format");
      sendDebugMessage("    IP Address: " + ip.toString() + " -> Hex: " + ipHex);

      // Send response back via remote WebSocket client only (not to web clients)
      if (NetworkManager::isClientConnected())
      {
        NetworkManager::sendToServer(modelResponse);
        sendDebugMessage("✓ CI-V: Model ID response transmitted via remote WebSocket");
      }
      else
      {
        sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, Model ID response NOT sent");
      }
    }

    sendDebugMessage("CI-V: Command 19 handled directly - exiting without calling SMCIV library");
    return true; // Message was handled internally - do NOT forward to physical CI-V port
  }

  // Handle Command 34 - Read Model
  if (msg.command == 0x34 && msg.data.size() == 0)
  {
    sendDebugMessage("CI-V: 34 - Read Model request (asking what type of device we are) - responding with model type");

    // Get the model type from configuration
    uint8_t modelType = DEFAULT_CIV_MODEL_TYPE;
    String modelDescription;
    switch (modelType)
    {
    case CIV_MODEL_ATOM_POWER_OUTLET:
      modelDescription = "ATOM Power Outlet";
      break;
    case CIV_MODEL_WYZE_OUTDOOR_OUTLET:
      modelDescription = "Wyze Outdoor Power Outlet";
      break;
    default:
      modelDescription = "Unknown Model";
      break;
    }

    char responseBuffer[24];
    snprintf(responseBuffer, sizeof(responseBuffer), "FE FE %02X %02X 34 %02X FD",
             msg.fromAddr, getCivAddressByte(), modelType);
    String modelResponse = String(responseBuffer);

    sendDebugMessage("<<< CI-V OUTGOING: Read Model Response (34 " + String(modelType, HEX) + ") - " + modelResponse);
    sendDebugMessage("    Purpose: Responding with device model type (" + String(modelType, HEX) + " = " + modelDescription + ")");

    // Send response back via remote WebSocket client only (not to web clients)
    if (NetworkManager::isClientConnected())
    {
      NetworkManager::sendToServer(modelResponse);
      sendDebugMessage("✓ CI-V: Read Model response transmitted via remote WebSocket");
    }
    else
    {
      sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, Read Model response NOT sent");
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
        sendDebugMessage("<<< CI-V OUTGOING: NAK Response (FA) - " + nakResponse + " - Invalid broadcast value 0x" + String(setValue, HEX) + " to address 0x" + String(msg.fromAddr, HEX));

        // Send response back via remote WebSocket client only (not to web clients)
        if (NetworkManager::isClientConnected())
        {
          NetworkManager::sendToServer(nakResponse);
          sendDebugMessage("✓ CI-V: NAK response transmitted via remote WebSocket");
        }
        else
        {
          sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, NAK response NOT sent");
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
      sendDebugMessage("CI-V: 35 - Read Outlet Status request (asking what outlets are on/off)");

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

      String statusDescription = "";
      switch (currentStatus)
      {
      case 0x00:
        statusDescription = "Both outlets OFF";
        break;
      case 0x01:
        statusDescription = "Outlet 1 ON, Outlet 2 OFF";
        break;
      case 0x02:
        statusDescription = "Outlet 1 OFF, Outlet 2 ON";
        break;
      case 0x03:
        statusDescription = "Both outlets ON";
        break;
      }

      sendDebugMessage("<<< CI-V OUTGOING: Outlet Status Response (35 " + statusHex + ") - " + statusResponse + " - " + statusDescription + " to address 0x" + String(msg.fromAddr, HEX));

      // Send response back via remote WebSocket client only (not to web clients)
      if (NetworkManager::isClientConnected())
      {
        NetworkManager::sendToServer(statusResponse);
        sendDebugMessage("✓ CI-V: Outlet status response transmitted via remote WebSocket");
      }
      else
      {
        sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, outlet status response NOT sent");
      }
    }
    else if (msg.data.size() == 1)
    {
      // Set outlet status - validate value first
      uint8_t newStatus = msg.data[0];

      String commandDescription = "";
      switch (newStatus)
      {
      case 0x00:
        commandDescription = "Set both outlets OFF";
        break;
      case 0x01:
        commandDescription = "Set Outlet 1 ON, Outlet 2 OFF";
        break;
      case 0x02:
        commandDescription = "Set Outlet 1 OFF, Outlet 2 ON";
        break;
      case 0x03:
        commandDescription = "Set both outlets ON";
        break;
      default:
        commandDescription = "Set outlets to INVALID value (0x" + String(newStatus, HEX) + ")";
        break;
      }

      sendDebugMessage("CI-V: 35 " + String(newStatus, HEX) + " - " + commandDescription);

      // Check if value is valid (00-03)
      if (newStatus > 0x03)
      {
        // Invalid value - respond with FA (NAK)
        sendDebugMessage("CI-V: Invalid outlet status: 0x" + String(newStatus, HEX) + " - responding with FA (NAK)");

        String nakResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " FA FD";
        sendDebugMessage("<<< CI-V OUTGOING: NAK Response (FA) - " + nakResponse + " - Invalid outlet status value 0x" + String(newStatus, HEX) + " to address 0x" + String(msg.fromAddr, HEX));

        // Send response back via remote WebSocket client only (not to web clients)
        if (NetworkManager::isClientConnected())
        {
          NetworkManager::sendToServer(nakResponse);
          sendDebugMessage("✓ CI-V: NAK response transmitted via remote WebSocket");
        }
        else
        {
          sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, NAK response NOT sent");
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

      // Update hardware using HardwareController
      hardware.setRelay(1, relay1State);
      hardware.setRelay(2, relay2State);

      // Save to DeviceState
      deviceState.setRelayState(relay1State, relay2State);

      // Send updated status response (acknowledge the set command)
      String statusHex = String(newStatus, HEX);
      statusHex.toUpperCase();
      if (statusHex.length() == 1)
        statusHex = "0" + statusHex;

      String statusResponse = "FE FE " + toAddrHex + " " + fromAddrHex + " 35 " + statusHex + " FD";

      sendDebugMessage("<<< CI-V OUTGOING: Outlet Status Set ACK (35 " + statusHex + ") - " + statusResponse + " - Acknowledging outlet state change to address 0x" + String(msg.fromAddr, HEX));

      // Send response back via remote WebSocket client only (not to web clients)
      if (NetworkManager::isClientConnected())
      {
        NetworkManager::sendToServer(statusResponse);
        sendDebugMessage("✓ CI-V: Outlet status update ACK transmitted via remote WebSocket");
      }
      else
      {
        sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, outlet status update ACK NOT sent");
      }

      // Broadcast state change to web clients
      String stateMsg = JsonBuilder::buildStateResponse();
      NetworkManager::broadcastToWebClients(stateMsg);
      sendDebugMessage("CI-V: Broadcasted state change to web clients");
    }

    return true; // Message handled internally
  }

  // For all other commands (that we don't handle directly), forward to SMCIV library
  switch (msg.command)
  {
  case 0x30: // Device type/model
    sendDebugMessage("CI-V: Device type request received - forwarding to SMCIV library");
    break;

  case 0x31: // Antenna/port selection
    if (msg.subCommand == 0x00)
    {
      sendDebugMessage("CI-V: Antenna port read request - forwarding to SMCIV library");
    }
    else if (msg.subCommand >= 1 && msg.subCommand <= 8)
    {
      sendDebugMessage("CI-V: Antenna port set to " + String(msg.subCommand) + " - forwarding to SMCIV library");
    }
    break;

  default:
    sendDebugMessage("CI-V: Unknown command " + String(msg.command, HEX) + " - forwarding to SMCIV library");
    break;
  }

  // Use SMCIV library to handle messages we don't handle directly
  sendDebugMessage("CI-V: About to call SMCIV library with: " + hexString);
  civHandler.handleIncomingWsMessage(hexString);
  sendDebugMessage("CI-V: SMCIV library call completed");

  return false; // Message was forwarded to SMCIV library - also forward to physical CI-V port
}

// -------------------------------------------------------------------------
// CI-V Message Handler for WebSocket Client
// -------------------------------------------------------------------------
void handleReceivedCivMessage(const String &message)
{
  sendDebugMessage("=== CI-V MESSAGE RECEIVED FROM WEBSOCKET CLIENT ===");
  sendDebugMessage("Raw message from remote server: '" + message + "'");
  sendDebugMessage("Message length: " + String(message.length()) + " characters");

  // Check if it looks like a CI-V hex message
  if (message.length() >= 12 && message.indexOf("FE") != -1)
  {
    sendDebugMessage("Message appears to be CI-V format - processing...");

    // Parse and validate CI-V message
    CivMessage civMsg = parseCivMessage(message);

    if (civMsg.valid)
    {
      sendDebugMessage("CI-V message parsed successfully");

      // Check if message is addressed to us
      if (isCivMessageForUs(civMsg))
      {
        sendDebugMessage("CI-V message IS addressed to us - processing...");

        // Process the CI-V message
        bool handledInternally = processCivMessage(civMsg);

        if (handledInternally)
        {
          sendDebugMessage("CI-V message handled internally - no further action needed");
        }
        else
        {
          sendDebugMessage("CI-V message processed but may need physical port forwarding");
        }
      }
      else
      {
        sendDebugMessage("CI-V message not addressed to us - ignoring");
      }
    }
    else
    {
      sendDebugMessage("CI-V message parsing failed - invalid format");
    }
  }
  else
  {
    sendDebugMessage("Message does not appear to be CI-V format (too short or no FE preamble)");
  }

  sendDebugMessage("=== CI-V MESSAGE PROCESSING COMPLETE ===");
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
    sendDebugMessage("Device started at: " + String(DeviceState::getBootTime()) + "ms");
    sendDebugMessage("Current uptime: " + String(millis()) + "ms");
    sendDebugMessage("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
    sendDebugMessage("CPU frequency: " + String(ESP.getCpuFreqMHz()) + "MHz");
    sendDebugMessage("Device IP: " + deviceIP);
    sendDebugMessage("Web client connected! Debug messages enabled.");
    sendDebugMessage("UDP listener active on port 4210. Waiting for 'ShackMate,IP,Port' messages...");
    if (NetworkManager::getConnectedServerIP().length() > 0)
    {
      sendDebugMessage("Last discovered IP: " + NetworkManager::getConnectedServerIP() + ":" + String(NetworkManager::getConnectedServerPort()));
      if (NetworkManager::isClientConnected())
      {
        sendDebugMessage("WebSocket client status: CONNECTED to " + NetworkManager::getConnectedServerIP());
      }
      else
      {
        sendDebugMessage("WebSocket client status: DISCONNECTED (last target: " + NetworkManager::getConnectedServerIP() + ")");
      }
    }
    else
    {
      sendDebugMessage("No ShackMate devices discovered yet.");
    }
    sendDebugMessage("=== END REBOOT BANNER ===");

    // Update sensor data in DeviceState before sending responses
    float lux = analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
    float amps = getValidatedCurrent();
    float rawV = hlw.getVoltage();
    float volts = rawV * voltageCalibrationFactor;
    float watts = getValidatedPower();
    DeviceState::updateSensorData(lux, volts, amps, watts);

    // 1) Send current state (relay + labels)
    {
      String stateMsg = JsonBuilder::buildStateResponse();
      client->text(stateMsg);
    }
    // 2) Immediately send full status (including sensors & uptime)
    {
      String statusMsg = JsonBuilder::buildStatusResponse();
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
          if (strcmp(cmd, "output1") == 0)
          {
            relay1State = value;
            hardware.setRelay(1, relay1State);
            deviceState.setRelayState(relay1State, relay2State);
          }
          else if (strcmp(cmd, "output2") == 0)
          {
            relay2State = value;
            hardware.setRelay(2, relay2State);
            deviceState.setRelayState(relay1State, relay2State);
          }

          // Immediately broadcast updated status
          {
            // Uptime
            String uptimeStr = getUptime();

            // Sensor readings
            float lux = analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
            float amps = getValidatedCurrent();
            float rawV = hlw.getVoltage();
            float volts = rawV * voltageCalibrationFactor;
            float watts = getValidatedPower();

            // Update sensor data and send status response
            DeviceState::updateSensorData(lux, volts, amps, watts);
            String payload = JsonBuilder::buildStatusResponse();
            NetworkManager::broadcastToWebClients(payload);
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
          // Apply relay state using HardwareController
          hardware.setRelay(r, onCmd);
          // Save state to DeviceState
          if (r == 1)
          {
            relay1State = onCmd;
            deviceState.setRelayState(relay1State, relay2State);
          }
          else
          {
            relay2State = onCmd;
            deviceState.setRelayState(relay1State, relay2State);
          }
          break;
        }
        // Handle label-change commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "setLabel") == 0)
        {
          int outlet = j["outlet"] | 0;
          const char *newText = j["text"] | "";
          if (outlet == 1)
          {
            deviceState.setRelayLabel(1, newText);
            strncpy(label1Text, newText, sizeof(label1Text));
          }
          else
          {
            deviceState.setRelayLabel(2, newText);
            strncpy(label2Text, newText, sizeof(label2Text));
          }

          String out = JsonBuilder::buildLabelResponse(outlet, newText);
          NetworkManager::broadcastToWebClients(out);
          break;
        }

        // Handle device name change commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "setDeviceName") == 0)
        {
          const char *newName = j["text"] | "";
          if (strlen(newName) > 0 && strlen(newName) < sizeof(deviceName))
          {
            deviceState.setDeviceName(newName);

            strncpy(deviceName, newName, sizeof(deviceName) - 1);
            deviceName[sizeof(deviceName) - 1] = '\0';

            String out = JsonBuilder::buildDeviceNameResponse(newName);
            NetworkManager::broadcastToWebClients(out);
          }
          break;
        }

        // Handle reboot and restore commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "reboot") == 0)
        {
          client->text(JsonBuilder::buildInfoResponse("Rebooting device..."));
          delay(250);
          ESP.restart();
          break;
        }
        if (j.containsKey("command") && strcmp(j["command"] | "", "restore") == 0)
        {
          client->text(JsonBuilder::buildInfoResponse("Erasing WiFi credentials completely..."));

          // Turn off LED before erasing credentials to ensure clean state
          digitalWrite(PIN_STATUS_LED, HIGH); // Turn OFF (inverted logic)
          Serial.println("Status LED turned OFF before WiFi reset");
          delay(100);

          // Complete WiFi reset to ensure captive portal activation
          WiFiManager wm;
          wm.resetSettings();          // Clear WiFiManager's saved credentials
          WiFi.disconnect(true, true); // Erase WiFi creds and reset
          WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
          delay(100);
          WiFi.mode(WIFI_STA); // Set to station mode
          sendDebugMessage("WiFi credentials and WiFiManager settings completely erased");
          sendDebugMessage("Captive portal WILL activate on next boot");
          delay(500);
          ESP.restart();
          break;
        }
        if (j.containsKey("command") && strcmp(j["command"] | "", "resetRebootCounter") == 0)
        {
          // Reset reboot counter manually - need to access preferences directly for now
          preferences.begin("system", false);
          preferences.putUInt("rebootCount", 0);
          preferences.end();
          deviceState.getDeviceConfig().rebootCounter = 0;
          rebootCounter = 0;
          client->text(JsonBuilder::buildInfoResponse("Reboot counter reset to 0"));
          sendDebugMessage("Reboot counter manually reset to 0");
          break;
        }

        // Handle test captive portal status LED blinking
        if (j.containsKey("command") && strcmp(j["command"] | "", "testCaptivePortal") == 0)
        {
          bool enable = j["enable"] | false;
          hardware.setCaptivePortalMode(enable);

          String statusMsg = enable ? "ENABLED" : "DISABLED";
          client->text(JsonBuilder::buildInfoResponse("Captive Portal status LED test mode " + statusMsg));
          sendDebugMessage("Captive Portal status LED test mode " + statusMsg);
          break;
        }

        // Test status LED manually (toggle once)
        if (j.containsKey("command") && strcmp(j["command"] | "", "testStatusLED") == 0)
        {
          static bool testLedState = false;
          testLedState = !testLedState;
          hardware.setStatusLED(testLedState);
          client->text(JsonBuilder::buildInfoResponse("Status LED toggled to " + String(testLedState ? "ON" : "OFF")));
          LOG_INFO("Manual Status LED test: " + String(testLedState ? "ON" : "OFF"));
          break;
        }

        // Comprehensive LED hardware test
        if (j.containsKey("command") && strcmp(j["command"] | "", "testLEDHardware") == 0)
        {
          client->text(JsonBuilder::buildInfoResponse("Starting comprehensive LED hardware test..."));
          LOG_INFO("=== LED HARDWARE TEST START ===");

          // Use HardwareController for comprehensive LED test
          hardware.testStatusLED(); // Test blinks for comprehensive test

          LOG_INFO("=== LED HARDWARE TEST COMPLETE ===");
          client->text(JsonBuilder::buildInfoResponse("LED hardware test complete - check serial output"));
          break;
        }

        // Handle Device ID configuration
        if (j.containsKey("command") && strcmp(j["command"] | "", "setDeviceId") == 0)
        {
          uint8_t newDeviceId = j["deviceId"] | 1;
          if (newDeviceId >= 1 && newDeviceId <= 4)
          {
            const auto &currentConfig = DeviceState::getDeviceConfig();

            // Only update if the device ID is actually changing
            if (currentConfig.deviceId != newDeviceId)
            {
              // Update DeviceState (this will handle persistence)
              deviceState.setDeviceId(newDeviceId);

              // Update legacy globals for backward compatibility
              deviceId = newDeviceId;
              civAddress = "B" + String(deviceId - 1);

              // Update CI-V handler with new address
              uint8_t civAddrByte = getCivAddressByte();
              sendDebugMessage("CI-V: Re-initializing SMCIV library with new address: 0x" + String(civAddrByte, HEX) + " (decimal " + String(civAddrByte) + ")");
              civHandler.begin(&NetworkManager::getWebSocketClient(), &civAddrByte);

              client->text(JsonBuilder::buildInfoResponse("Device ID set to " + String(deviceId) + " (CIV: " + civAddress + ")"));
              sendDebugMessage("Device ID changed to " + String(deviceId) + ", CIV Address: " + civAddress + " (0x" + String(civAddrByte, HEX) + ")");
            }
            else
            {
              // Device ID unchanged - just acknowledge
              client->text(JsonBuilder::buildInfoResponse("Device ID confirmed as " + String(deviceId) + " (CIV: " + civAddress + ")"));
            }
          }
          else
          {
            client->text(JsonBuilder::buildErrorResponse("Invalid Device ID. Must be 1-4."));
            sendDebugMessage("Invalid Device ID received: " + String(newDeviceId));
          }
          break;
        }

        // Handle voltage calibration commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "calibrateVoltage") == 0)
        {
          float expectedVoltage = j["expectedVoltage"] | 0.0f;
          if (expectedVoltage > 0.0f && expectedVoltage <= 300.0f) // Reasonable voltage range
          {
            float rawVoltage = hlw.getVoltage(); // Get uncalibrated reading
            if (rawVoltage > 0.0f)
            {
              float newCalibrationFactor = expectedVoltage / rawVoltage;
              voltageCalibrationFactor = newCalibrationFactor;
              voltageCalibrated = true;

              // Save calibration to preferences
              preferences.begin("calibration", false);
              preferences.putFloat("voltageFactor", voltageCalibrationFactor);
              preferences.putBool("voltageCalibrated", voltageCalibrated);
              preferences.end();

              client->text(JsonBuilder::buildInfoResponse("Voltage calibrated: factor=" + String(voltageCalibrationFactor, 4) +
                                                          " (raw=" + String(rawVoltage, 1) + "V, expected=" + String(expectedVoltage, 1) + "V)"));
              sendDebugMessage("Voltage calibration set: factor=" + String(voltageCalibrationFactor, 4) +
                               " raw=" + String(rawVoltage, 1) + "V -> " + String(expectedVoltage, 1) + "V");
            }
            else
            {
              client->text(JsonBuilder::buildErrorResponse("Cannot calibrate: no voltage reading available"));
              sendDebugMessage("Voltage calibration failed: raw voltage reading is 0");
            }
          }
          else
          {
            client->text(JsonBuilder::buildErrorResponse("Invalid expected voltage. Must be 1-300V."));
            sendDebugMessage("Invalid voltage calibration value: " + String(expectedVoltage));
          }
          break;
        }

        // Handle reset voltage calibration
        if (j.containsKey("command") && strcmp(j["command"] | "", "resetVoltageCalibration") == 0)
        {
          voltageCalibrationFactor = 1.0f;
          voltageCalibrated = false;

          // Clear calibration from preferences
          preferences.begin("calibration", false);
          preferences.remove("voltageFactor");
          preferences.remove("voltageCalibrated");
          preferences.end();

          client->text(JsonBuilder::buildInfoResponse("Voltage calibration reset to default (factor=1.0)"));
          sendDebugMessage("Voltage calibration reset to default");
          break;
        }

        // Handle get voltage calibration info
        if (j.containsKey("command") && strcmp(j["command"] | "", "getVoltageCalibration") == 0)
        {
          float rawVoltage = hlw.getVoltage();
          float calibratedVoltage = rawVoltage * voltageCalibrationFactor;

          String response = "{\"type\":\"voltageCalibration\","
                            "\"calibrationFactor\":" +
                            String(voltageCalibrationFactor, 4) + ","
                                                                  "\"calibrated\":" +
                            String(voltageCalibrated ? "true" : "false") + ","
                                                                           "\"rawVoltage\":" +
                            String(rawVoltage, 2) + ","
                                                    "\"calibratedVoltage\":" +
                            String(calibratedVoltage, 2) + "}";
          client->text(response);
          sendDebugMessage("Voltage calibration info sent: factor=" + String(voltageCalibrationFactor, 4) +
                           " raw=" + String(rawVoltage, 1) + "V cal=" + String(calibratedVoltage, 1) + "V");
          break;
        }

        // Handle current calibration commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "calibrateCurrent") == 0)
        {
          float expectedCurrent = j["expectedCurrent"] | 0.0f;
          if (expectedCurrent > 0.0f && expectedCurrent <= 20.0f) // Reasonable current range
          {
            float rawCurrent = hlw.getCurrent(); // Get uncalibrated reading
            if (rawCurrent > 0.0f)
            {
              float newCalibrationFactor = expectedCurrent / rawCurrent;
              currentCalibrationFactor = newCalibrationFactor;
              currentCalibrated = true;

              // Save calibration to preferences
              preferences.begin("calibration", false);
              preferences.putFloat("currentFactor", currentCalibrationFactor);
              preferences.putBool("currentCalibrated", currentCalibrated);
              preferences.end();

              client->text(JsonBuilder::buildInfoResponse("Current calibrated: factor=" + String(currentCalibrationFactor, 4) +
                                                          " (raw=" + String(rawCurrent, 3) + "A, expected=" + String(expectedCurrent, 3) + "A)"));
              sendDebugMessage("Current calibration set: factor=" + String(currentCalibrationFactor, 4) +
                               " raw=" + String(rawCurrent, 3) + "A -> " + String(expectedCurrent, 3) + "A");
            }
            else
            {
              client->text(JsonBuilder::buildErrorResponse("Cannot calibrate: no current reading available"));
              sendDebugMessage("Current calibration failed: raw current reading is 0");
            }
          }
          else
          {
            client->text(JsonBuilder::buildErrorResponse("Invalid expected current. Must be 0.001-20A."));
            sendDebugMessage("Invalid current calibration value: " + String(expectedCurrent));
          }
          break;
        }

        // Handle reset current calibration
        if (j.containsKey("command") && strcmp(j["command"] | "", "resetCurrentCalibration") == 0)
        {
          currentCalibrationFactor = 1.0f;
          currentCalibrated = false;

          // Clear calibration from preferences
          preferences.begin("calibration", false);
          preferences.remove("currentFactor");
          preferences.remove("currentCalibrated");
          preferences.end();

          client->text(JsonBuilder::buildInfoResponse("Current calibration reset to default (factor=1.0)"));
          sendDebugMessage("Current calibration reset to default");
          break;
        }

        // Handle get current calibration info
        if (j.containsKey("command") && strcmp(j["command"] | "", "getCurrentCalibration") == 0)
        {
          float rawCurrent = hlw.getCurrent();
          float calibratedCurrent = rawCurrent * currentCalibrationFactor;

          String response = "{\"type\":\"currentCalibration\","
                            "\"calibrationFactor\":" +
                            String(currentCalibrationFactor, 4) + ","
                                                                  "\"calibrated\":" +
                            String(currentCalibrated ? "true" : "false") + ","
                                                                           "\"rawCurrent\":" +
                            String(rawCurrent, 3) + ","
                                                    "\"calibratedCurrent\":" +
                            String(calibratedCurrent, 3) + "}";
          client->text(response);
          sendDebugMessage("Current calibration info sent: factor=" + String(currentCalibrationFactor, 4) +
                           " raw=" + String(rawCurrent, 3) + "A cal=" + String(calibratedCurrent, 3) + "A");
          break;
        }

        // Handle ping/pong for connection testing
        if (j.containsKey("type"))
        {
          const char *msgType = j["type"] | "";
          if (strcmp(msgType, "ping") == 0)
          {
            // Respond to ping with pong using JsonBuilder
            client->text(JsonBuilder::buildPongResponse(millis()));
            sendDebugMessage("Received ping from client, sent pong response");
            break;
          }
          else if (strcmp(msgType, "pong") == 0)
          {
            // Received pong response - connection is healthy
            break;
          }
        }
      }
    }
    // Handle CI-V hex messages (must be hex format, not JSON)
    else if (!msg.startsWith("{") && msg.length() > 0)
    {
      sendDebugMessage("CI-V: Received hex message: " + msg);
      sendDebugMessage("CI-V: Message length: " + String(msg.length()) + " characters");

      // Check if this looks like the echo broadcast we're expecting
      if (msg.indexOf("FE FE 00") != -1 && msg.indexOf("19 00") != -1)
      {
        sendDebugMessage("CI-V: *** This looks like a broadcast echo request! ***");
      }

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
  float amps = getValidatedCurrent();
  float volts = hlw.getVoltage();
  float watts = getValidatedPower();
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
  wm.resetSettings();          // Clear WiFiManager's saved credentials
  WiFi.disconnect(true, true); // Erase WiFi creds and reset
  WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
  delay(100);
  WiFi.mode(WIFI_STA); // Set to station mode
  request->send(200, "text/html", "<html><body><h1>WiFi Completely Erased</h1><p>Captive portal WILL activate on reboot.</p></body></html>");
  delay(2000);
  ESP.restart();
}

// -------------------------------------------------------------------------
// UDP Discovery and WebSocket Client Functions
// -------------------------------------------------------------------------
// NOTE: UDP discovery is now handled by NetworkManager
// NOTE: connectToShackMateServer is now handled by NetworkManager::connectToShackMateServer()
// NOTE: onWebSocketClientEvent is now handled by NetworkManager::onWebSocketClientEvent()
// NOTE: disconnectWebSocketClient is now handled by NetworkManager::disconnectFromServer()

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

  // Initialize the new logging system
  Logger::init(LogLevel::INFO);

  // Initialize device state management
  deviceState.init();

  DeviceState::setBootTime(millis());

  LOG_INFO("================================================");
  LOG_INFO("        DEVICE REBOOT DETECTED");
  LOG_INFO("================================================");
  LOG_INFO("=== ShackMate Outlet Starting ===");
  LOG_INFO("Version: " + String(VERSION));
  LOG_INFO("Boot time: " + String(DeviceState::getBootTime()) + "ms");
  LOG_INFO("Free heap: " + String(ESP.getFreeHeap()) + " bytes");

  // Debug: Show device configuration and CI-V address
  LOG_INFO("Device ID: " + String(DeviceState::getDeviceConfig().deviceId));
  LOG_INFO("CI-V Address: 0x" + String(getCivAddressByte(), HEX) + " (decimal " + String(getCivAddressByte()) + ")");

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
  LOG_INFO("Reset reason: " + resetReasonStr);
  LOG_INFO("Chip model: " + String(ESP.getChipModel()));
  LOG_INFO("Chip revision: " + String(ESP.getChipRevision()));
  LOG_INFO("CPU frequency: " + String(ESP.getCpuFreqMHz()) + "MHz");
  LOG_INFO("Flash size: " + String(ESP.getFlashChipSize()) + " bytes");
  LOG_INFO("================================================");

  // Initialize hardware controller
  hardware.init();
  LOG_INFO("Hardware controller initialized");

  // Test the status LED during startup using hardware controller
  LOG_INFO("Testing Status LED - 3 blinks...");
  hardware.testStatusLED();
  Serial.println("Status LED test complete");

  // Initialize LED timer for captive portal blinking
  initLedTimer();

  // Configure LUX ADC pin as input
  pinMode(PIN_LUX_ADC, INPUT);

  // Set ADC resolution to 12-bit (0-4095 range)
  analogSetAttenuation(ADC_11db);                 // Global attenuation setting
  analogSetPinAttenuation(PIN_LUX_ADC, ADC_11db); // Pin-specific attenuation
  analogReadResolution(12);                       // Set resolution to 12-bit (0-4095)

  // Initialize HLW8012 power monitoring chip
  hlw.begin(
      PIN_HLW_CF,
      PIN_HLW_CF1,
      PIN_HLW_SEL,
      HIGH,  // Current mode when SEL pin is HIGH
      true,  // Use interrupts for better accuracy
      500000 // Pulse timeout in microseconds
  );

  // Set up interrupts for pulse counting
  setInterrupts();

  // Configure hardware resistor values for 770:1 voltage divider
  // For 770:1 divider: upstream = 770kΩ, downstream = 1kΩ
  double voltage_upstream = VOLTAGE_DIVIDER * 1000.0; // 770kΩ
  double voltage_downstream = 1000.0;                 // 1kΩ
  hlw.setResistors(CURRENT_RESISTOR, voltage_upstream, voltage_downstream);

  Serial.println("HLW8012 power monitoring initialized");
  Serial.println("CF Pin: " + String(PIN_HLW_CF));
  Serial.println("CF1 Pin: " + String(PIN_HLW_CF1));
  Serial.println("SEL Pin: " + String(PIN_HLW_SEL));
  Serial.println("Current Resistor: " + String(CURRENT_RESISTOR, 6) + " ohms");
  Serial.println("Voltage Divider Ratio: " + String(VOLTAGE_DIVIDER, 1) + ":1");
  Serial.println("Voltage Upstream: " + String(voltage_upstream, 0) + " ohms");
  Serial.println("Voltage Downstream: " + String(voltage_downstream, 0) + " ohms");

  // Load calibration multipliers from preferences (if available)
  preferences.begin("calibration", true);
  float storedCurrentMultiplier = preferences.getFloat("currentMultiplier", 0.0f);
  float storedVoltageMultiplier = preferences.getFloat("voltageMultiplier", 0.0f);
  float storedPowerMultiplier = preferences.getFloat("powerMultiplier", 0.0f);

  // Load our voltage calibration factor
  voltageCalibrationFactor = preferences.getFloat("voltageFactor", 1.0f);
  voltageCalibrated = preferences.getBool("voltageCalibrated", false);

  // Load our current calibration factor
  currentCalibrationFactor = preferences.getFloat("currentFactor", 1.0f);
  currentCalibrated = preferences.getBool("currentCalibrated", false);
  preferences.end();

  if (storedCurrentMultiplier > 0 && storedVoltageMultiplier > 0 && storedPowerMultiplier > 0)
  {
    hlw.setCurrentMultiplier(storedCurrentMultiplier);
    hlw.setVoltageMultiplier(storedVoltageMultiplier);
    hlw.setPowerMultiplier(storedPowerMultiplier);
    Serial.println("Loaded HLW8012 calibration multipliers from preferences.");
  }

  if (voltageCalibrated)
  {
    Serial.println("Loaded voltage calibration factor: " + String(voltageCalibrationFactor, 4));
  }
  else
  {
    Serial.println("No voltage calibration found - using default factor: " + String(voltageCalibrationFactor, 4));
  }

  if (currentCalibrated)
  {
    Serial.println("Loaded current calibration factor: " + String(currentCalibrationFactor, 4));
  }
  else
  {
    Serial.println("No current calibration found - using default factor: " + String(currentCalibrationFactor, 4));
  }

  // Enable HLW8012 pulse interrupts for CF and CF1
  setInterrupts();
  Serial.println("HLW8012 interrupts enabled for power monitoring");

  // Configure hardware buttons
  pinMode(PIN_BUTTON1, INPUT_PULLDOWN);
  pinMode(PIN_BUTTON2, INPUT_PULLDOWN);

  // Initialize button state variables
  lastButton1State = digitalRead(PIN_BUTTON1) == HIGH;
  lastButton2State = digitalRead(PIN_BUTTON2) == HIGH;
  button1StateStable = lastButton1State;
  button2StateStable = lastButton2State;
  lastButton1Time = millis();
  lastButton2Time = millis();
  Serial.println("Hardware buttons configured with debouncing initialized");

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

  // Load persisted relay states using DeviceState
  relay1State = deviceState.getRelayState().relay1;
  relay2State = deviceState.getRelayState().relay2;
  // Apply saved states using HardwareController
  hardware.setRelay(1, relay1State);
  hardware.setRelay(2, relay2State);

  // Load labels and device name using DeviceState
  String s1 = String(deviceState.getRelayState().label1);
  String s2 = String(deviceState.getRelayState().label2);
  String deviceNameStr = String(deviceState.getDeviceConfig().deviceName);

  // Copy strings to char arrays
  strncpy(label1Text, s1.c_str(), sizeof(label1Text) - 1);
  strncpy(label2Text, s2.c_str(), sizeof(label2Text) - 1);
  strncpy(deviceName, deviceNameStr.c_str(), sizeof(deviceName) - 1);
  label1Text[sizeof(label1Text) - 1] = '\0';
  label2Text[sizeof(label2Text) - 1] = '\0';
  deviceName[sizeof(deviceName) - 1] = '\0';

  // Load and increment reboot counter using DeviceState
  deviceState.incrementRebootCounter();
  rebootCounter = deviceState.getDeviceConfig().rebootCounter;
  Serial.println("Reboot counter: " + String(rebootCounter));

  // Load Device ID and CIV Address using DeviceState
  deviceId = deviceState.getDeviceConfig().deviceId;
  civAddress = deviceState.getDeviceConfig().civAddress;
  Serial.println("Device ID: " + String(deviceId) + ", CIV Address: " + civAddress);

  // Initialize CI-V handler with NetworkManager's WebSocket client and Device ID
  uint8_t civAddrByte = getCivAddressByte();
  sendDebugMessage("CI-V: Initializing SMCIV library with address: 0x" + String(civAddrByte, HEX) + " (decimal " + String(civAddrByte) + ")");
  civHandler.begin(&NetworkManager::getWebSocketClient(), &civAddrByte);
  Serial.println("CI-V handler initialized with address: 0x" + String(civAddrByte, HEX));

  Serial.println("Device Name: " + String(deviceName));

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
    captivePortalActive = true; // Set flag for status LED blinking
    statusLedLastToggle = millis(); // Initialize timing for LED blinking
    statusLedState = false; // Start with LED off state
    Serial.println("Captive Portal activated - Status LED will blink");
    Serial.println("DEBUG: WiFiManager AP mode - captivePortalActive = true");
    
    // Start the hardware timer for LED blinking
    startLedBlinking(); });

  String customHTML = "<div style='text-align:left;'>"
                      "<p><strong>Additional Configuration</strong></p>"
                      "<p>WebSocket Port: <input type='text' name='port' value='%PORT%'></p>"
                      "</div>";
  wifiManager.setCustomMenuHTML(customHTML.c_str());
  wifiManager.addParameter(&customPortParam);

  // Set config portal timeout to allow for longer testing
  wifiManager.setConfigPortalTimeout(180); // 3 minutes timeout

  // Remove the unreliable web server callback and use a different approach
  Serial.println("About to start WiFiManager autoConnect - LED should blink if entering captive portal mode");

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

  // Clear captive portal flag
  captivePortalActive = false;

  // Stop LED blinking timer
  stopLedBlinking();

  Serial.println("WiFi connected - Captive Portal deactivated");

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

  // Initialize network manager (WebSocket server/client, UDP discovery)
  NetworkManager::init();
  NetworkManager::setWebSocketEventHandler(onWsEvent);
  LOG_INFO("Network manager initialized");

  preferences.begin("config", false);
  tcpPort = customPortParam.getValue();
  preferences.putString("tcp_port", tcpPort);
  preferences.end();

  wsPortStr = tcpPort;

  wsPort = tcpPort.toInt();
  if (wsPort <= 0)
    wsPort = 4000;

  // Attach NetworkManager's WebSocket to HTTP server
  httpServer.addHandler(&NetworkManager::getWebSocket());
  Serial.println("WebSocket handler attached to HTTP server");

  // Root
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/saveConfig", HTTP_POST, handleSaveConfig);
  httpServer.on("/restoreConfig", HTTP_POST, handleRestoreConfig);
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

  // WebSocket server on port 4000 is now handled by NetworkManager
  wsServer.addHandler(&NetworkManager::getWebSocket());
  wsServer.begin();
  Serial.println("WebSocket server started on port 4000");
  Serial.println("DEBUG: WebSocket should be accessible at: ws://" + deviceIP + ":4000/ws");

  ArduinoOTA.onStart([]()
                     { 
                       Serial.println("OTA update starting...");
                       // Disable watchdog timer during OTA to prevent resets
                       esp_task_wdt_deinit();
                       Serial.println("Watchdog timer disabled for OTA"); });
  ArduinoOTA.onEnd([]()
                   { 
                     Serial.println("\nOTA update complete");
                     // Re-enable watchdog timer after OTA with  30 second timeout
                     esp_task_wdt_init(30, true);
                     Serial.println("Watchdog timer re-enabled after OTA"); });
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
  // Update status LED using hardware controller
  hardware.update();

  // Debug output when in captive portal mode
  static unsigned long lastLoopDebug = 0;
  if (captivePortalActive && millis() - lastLoopDebug >= 1000)
  {
    Serial.println("DEBUG: Main loop running in captive portal mode");
    lastLoopDebug = millis();
  }

  // Handle OTA updates
  ArduinoOTA.handle();

  // Handle WebSocket events and other network tasks
  NetworkManager::getWebSocket().cleanupClients();

  // Handle network management (UDP discovery, WebSocket client, connection health)
  NetworkManager::update();

  // Track network status changes (only log when status actually changes)
  static bool lastWsClientConnected = false;
  static String lastConnectedServerIP = "";
  static int lastWebClientCount = -1;

  bool currentWsClientConnected = NetworkManager::isClientConnected();
  String currentConnectedServerIP = NetworkManager::getConnectedServerIP();
  int currentWebClientCount = NetworkManager::getWebSocket().count();

  // Only log WebSocket client status changes
  if (currentWsClientConnected != lastWsClientConnected || currentConnectedServerIP != lastConnectedServerIP)
  {
    if (currentWsClientConnected)
    {
      sendDebugMessage("Network Status: WebSocket client CONNECTED to " +
                       currentConnectedServerIP + ":" +
                       String(NetworkManager::getConnectedServerPort()));
    }
    else
    {
      if (NetworkManager::hasEverConnected())
      {
        sendDebugMessage("Network Status: WebSocket client DISCONNECTED (last target: " +
                         NetworkManager::getConnectedServerIP() + ":" +
                         String(NetworkManager::getConnectedServerPort()) + ")");
      }
      else
      {
        sendDebugMessage("Network Status: No WebSocket connections established yet - listening for UDP discovery");
      }
    }
    lastWsClientConnected = currentWsClientConnected;
    lastConnectedServerIP = currentConnectedServerIP;
  }

  // Only log web client count changes
  if (currentWebClientCount != lastWebClientCount)
  {
    sendDebugMessage("Web UI Status: " + String(currentWebClientCount) + " client(s) connected to web interface");
    lastWebClientCount = currentWebClientCount;
  }

  // Handle CI-V message processing
  civHandler.loop();

  // Handle hardware button presses with debouncing
  unsigned long currentTime = millis();

  // Button 1 handling with proper debouncing
  {
    bool currentButton1Reading = digitalRead(PIN_BUTTON1) == HIGH;

    // If button reading changed, reset the debounce timer
    if (currentButton1Reading != lastButton1State)
    {
      lastButton1Time = currentTime;
    }

    // If reading has been stable for debounce delay
    if ((currentTime - lastButton1Time) > debounceDelay)
    {
      // If the button state has actually changed from stable state
      if (currentButton1Reading != button1StateStable)
      {
        button1StateStable = currentButton1Reading;

        // Only trigger on button press (LOW to HIGH transition)
        if (button1StateStable)
        {
          relay1State = !relay1State;
          digitalWrite(PIN_RELAY1, relay1State ? HIGH : LOW);
          digitalWrite(PIN_RELAY1_LED, relay1State ? LOW : HIGH);
          preferences.begin("outlet", false);
          preferences.putBool("output1", relay1State);
          preferences.end();

          // Broadcast state change
          String msg = "{\"type\":\"state\",\"output1State\":" + String(relay1State ? "true" : "false") +
                       ",\"output2State\":" + String(relay2State ? "true" : "false") +
                       ",\"label1\":\"" + String(label1Text) + "\",\"label2\":\"" + String(label2Text) +
                       "\",\"deviceName\":\"" + String(deviceName) + "\"}";
          NetworkManager::broadcastToWebClients(msg);

          Serial.printf("Button 1 pressed: Relay 1 %s\n", relay1State ? "ON" : "OFF");
        }
      }
    }

    lastButton1State = currentButton1Reading;
  }

  // Button 2 handling with proper debouncing
  {
    bool currentButton2Reading = digitalRead(PIN_BUTTON2) == HIGH;

    // If button reading changed, reset the debounce timer
    if (currentButton2Reading != lastButton2State)
    {
      lastButton2Time = currentTime;
    }

    // If reading has been stable for debounce delay
    if ((currentTime - lastButton2Time) > debounceDelay)
    {
      // If the button state has actually changed from stable state
      if (currentButton2Reading != button2StateStable)
      {
        button2StateStable = currentButton2Reading;

        // Only trigger on button press (LOW to HIGH transition)
        if (button2StateStable)
        {
          relay2State = !relay2State;
          digitalWrite(PIN_RELAY2, relay2State ? HIGH : LOW);
          digitalWrite(PIN_RELAY2_LED, relay2State ? LOW : HIGH);
          preferences.begin("outlet", false);
          preferences.putBool("output2", relay2State);
          preferences.end();

          // Broadcast state change
          String msg = "{\"type\":\"state\",\"output1State\":" + String(relay1State ? "true" : "false") +
                       ",\"output2State\":" + String(relay2State ? "true" : "false") +
                       ",\"label1\":\"" + String(label1Text) + "\",\"label2\":\"" + String(label2Text) +
                       "\",\"deviceName\":\"" + String(deviceName) + "\"}";
          NetworkManager::broadcastToWebClients(msg);

          Serial.printf("Button 2 pressed: Relay 2 %s\n", relay2State ? "ON" : "OFF");
        }
      }
    }

    lastButton2State = currentButton2Reading;
  }

  // Periodic sensor reading and status broadcast
  static unsigned long lastSensorRead = 0;
  static unsigned long lastStatusBroadcast = 0;

  if (currentTime - lastSensorRead >= SENSOR_UPDATE_INTERVAL_MS)
  {
    lastSensorRead = currentTime;

    // Read sensors and broadcast status
    float voltage = hlw.getVoltage();
    float current = getValidatedCurrent();
    float power = getValidatedPower();

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
                       String(NetworkManager::isClientConnected() ? "true" : "false") + ","
                                                                                        "\"civServerEverConnected\":" +
                       String(NetworkManager::hasEverConnected() ? "true" : "false") + ","
                                                                                       "\"civServerIP\":\"" +
                       NetworkManager::getConnectedServerIP() + "\","
                                                                "\"civServerPort\":" +
                       String(NetworkManager::getConnectedServerPort()) + ","
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
                       civAddress + "\","
                                    "\"deviceName\":\"" +
                       String(deviceName) + "\"}";

    NetworkManager::broadcastToWebClients(statusMsg);

    Serial.printf("Status: %.1fV %.2fA %.0fW %.1flux %s\n",
                  voltage, current, power, lux, timeStr);
  }

  // Small delay to prevent overwhelming the system
  delay(50);
}

// -------------------------------------------------------------------------
// System Info Functions
// -------------------------------------------------------------------------
String getUptime()
{
  return DeviceState::getUptime();
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
  // Use uptime instead of timestamp
  String uptimeStr = getUptime();

  tmpl.replace("%PROJECT_NAME%", String(NAME));
  tmpl.replace("%TIME%", uptimeStr);
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

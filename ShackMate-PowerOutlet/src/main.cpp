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
#include <vector>             // For CI-V message data storage
#include <driver/gpio.h>      // For explicit GPIO control

// New modular components (now in ShackMateCore library)
#include <config.h>
#include <logger.h>
#include <device_state.h>
#include <hardware_controller.h>
#include <json_builder.h>
#include <network_manager.h>
#include <civ_handler.h>

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
CivHandler civHandler;       // CI-V protocol handler
Preferences preferences;
AsyncWebServer httpServer(80);
AsyncWebServer wsServer(4000);
HLW8012 hlw;

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
float powerCalibrationFactor = 1.0f;
bool powerCalibrated = false;

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

  // Check voltage first - if no voltage, there should be no current
  float rawVoltage = hlw.getVoltage();
  if (rawVoltage < 1.0f) // If voltage is below 1V, treat as no power/no current
  {
    return 0.0f;
  }

  // Define minimum threshold for meaningful current reading (before calibration)
  const float MIN_RAW_CURRENT_THRESHOLD = 0.001f; // 1mA minimum raw reading

  // If raw current is below threshold, treat as zero (no load)
  if (rawCurrent < MIN_RAW_CURRENT_THRESHOLD)
  {
    return 0.0f;
  }

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
 * @brief Get validated and calibrated voltage reading from HLW8012
 *
 * @return float Validated and calibrated voltage reading in volts
 */
float getValidatedVoltage()
{
  float rawVoltage = hlw.getVoltage();
  float calibratedVoltage = rawVoltage * voltageCalibrationFactor;

  // Apply basic validation - negative voltage doesn't make sense
  if (calibratedVoltage < 0.0f)
  {
    return 0.0f;
  }

  // Cap at reasonable maximum (300V for safety)
  const float MAX_REASONABLE_VOLTAGE = 300.0f;
  if (calibratedVoltage > MAX_REASONABLE_VOLTAGE)
  {
    Serial.printf("WARNING: Detected excessive voltage reading: %.1fV - capping at %.1fV\n",
                  calibratedVoltage, MAX_REASONABLE_VOLTAGE);
    return MAX_REASONABLE_VOLTAGE;
  }

  return calibratedVoltage;
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
  const float MIN_CURRENT_THRESHOLD = 0.001f; // 1mA minimum for valid power reading (same as current validation)
  const float MAX_REASONABLE_POWER = 2000.0f; // 2000W maximum reasonable power for this device

  // Debug output every 10 seconds
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 10000)
  {
    float voltage = getValidatedVoltage(); // Use calibrated voltage
    float apparentPower = voltage * current;
    float powerFactor = (apparentPower > 0.1f) ? (rawPower / apparentPower) : 0.0f;
    Serial.printf("POWER DEBUG: Current=%.3fA, RawPower=%.1fW, Voltage=%.1fV, ApparentPower=%.1fW, PowerFactor=%.2f, MinThreshold=%.3fA\n",
                  current, rawPower, voltage, apparentPower, powerFactor, MIN_CURRENT_THRESHOLD);

    // Additional debugging for power accuracy
    if (current > 0.1f) // Only show when there's significant current
    {
      Serial.printf("POWER ANALYSIS: Expected~%.1fW (if resistive), Actual=%.1fW, Difference=%.1fW (%.1f%%)\n",
                    apparentPower, rawPower, rawPower - apparentPower,
                    ((rawPower - apparentPower) / apparentPower) * 100.0f);
    }
    lastDebug = millis();
  }

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
  float voltage = getValidatedVoltage(); // Use calibrated voltage
  float apparentPower = voltage * current;

  // If power is significantly higher than apparent power, it's likely spurious
  // Increased tolerance from 1.2 to 2.0 to account for measurement variations and reactive loads
  if (rawPower > (apparentPower * 2.0f))
  {
    Serial.printf("WARNING: Power reading %.1fW exceeds apparent power %.1fW * 2.0 = %.1fW (V=%.1f, I=%.3f) - setting to 0W\n",
                  rawPower, apparentPower, apparentPower * 2.0f, voltage, current);
    return 0.0f;
  }

  // Debug output when power is valid
  if (millis() - lastDebug > 9000) // Show slightly before regular debug
  {
    float calibratedPower = rawPower * powerCalibrationFactor;
    Serial.printf("POWER VALID: Raw=%.1fW, Calibrated=%.1fW (factor=%.3f), V=%.1f, I=%.3f, Apparent=%.1fW, Factor=%.2f\n",
                  rawPower, calibratedPower, powerCalibrationFactor, voltage, current, apparentPower, rawPower / apparentPower);
  }

  // Apply power calibration factor
  return rawPower * powerCalibrationFactor;
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
float getValidatedVoltage();
float getValidatedCurrent();
float getValidatedPower();

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

    // Parse the CI-V message using our new handler
    CivHandler::CivMessage civMsg = civHandler.parseMessage(message);

    if (civMsg.valid)
    {
      sendDebugMessage("CI-V message parsed successfully");

      // Check if message is addressed to us
      if (civHandler.isMessageForUs(civMsg))
      {
        sendDebugMessage("CI-V message IS addressed to us - processing...");

        // Process the CI-V message and get response
        bool newRelay1State, newRelay2State;
        String response = civHandler.processMessage(civMsg, relay1State, relay2State, newRelay1State, newRelay2State);

        // Apply any relay state changes
        if (newRelay1State != relay1State || newRelay2State != relay2State)
        {
          sendDebugMessage("CI-V: Applying relay state changes - Relay1: " + String(newRelay1State ? "ON" : "OFF") +
                           ", Relay2: " + String(newRelay2State ? "ON" : "OFF"));

          relay1State = newRelay1State;
          relay2State = newRelay2State;

          // Update hardware using HardwareController
          hardware.setRelay(1, relay1State);
          hardware.setRelay(2, relay2State);

          // Save to DeviceState
          deviceState.setRelayState(relay1State, relay2State);

          // Broadcast state change to web clients
          String stateMsg = JsonBuilder::buildStateResponse();
          NetworkManager::broadcastToWebClients(stateMsg);
          sendDebugMessage("CI-V: Broadcasted state change to web clients");
        }

        // Send response if one was generated
        if (response.length() > 0)
        {
          if (NetworkManager::isClientConnected())
          {
            NetworkManager::sendToServer(response);
            sendDebugMessage("✓ CI-V: Response transmitted via remote WebSocket");
          }
          else
          {
            sendDebugMessage("✗ CI-V: WARNING - Remote WebSocket not connected, response NOT sent");
          }
        }
        else
        {
          sendDebugMessage("CI-V: No response generated for this message");
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
    // Use independent lux reading - no interference with power measurements
    float lux = HardwareController::readLuxSensor();

    // Debug for websocket connect lux reading
    Serial.printf("WS CONNECT LUX DEBUG: Independent Lux=%.3f\n", lux);

    float amps = getValidatedCurrent();
    float volts = getValidatedVoltage();
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
            // Debug lux before relay change
            uint16_t luxBefore = analogRead(PIN_LUX_ADC);

            relay1State = value;
            hardware.setRelay(1, relay1State);
            deviceState.setRelayState(relay1State, relay2State);

            // Debug lux after relay change
            delay(10); // Small delay to let circuit settle
            uint16_t luxAfter = analogRead(PIN_LUX_ADC);
            Serial.printf("RELAY1 CHANGE: %s -> %s, Lux Before=%d, After=%d (Change=%d)\n",
                          relay1State ? "OFF" : "ON", relay1State ? "ON" : "OFF",
                          luxBefore, luxAfter, (int)luxAfter - (int)luxBefore);
          }
          else if (strcmp(cmd, "output2") == 0)
          {
            // Debug lux before relay change
            uint16_t luxBefore = analogRead(PIN_LUX_ADC);

            relay2State = value;
            hardware.setRelay(2, relay2State);
            deviceState.setRelayState(relay1State, relay2State);

            // Debug lux after relay change
            delay(10); // Small delay to let circuit settle
            uint16_t luxAfter = analogRead(PIN_LUX_ADC);
            Serial.printf("RELAY2 CHANGE: %s -> %s, Lux Before=%d, After=%d (Change=%d)\n",
                          relay2State ? "OFF" : "ON", relay2State ? "ON" : "OFF",
                          luxBefore, luxAfter, (int)luxAfter - (int)luxBefore);
          }

          // Immediately broadcast updated status
          {
            // Uptime
            String uptimeStr = getUptime();

            // Sensor readings
            uint16_t luxRaw = analogRead(PIN_LUX_ADC);
            float lux = luxRaw * (3.3f / 4095.0f);

            // Debug for outlet control lux reading
            Serial.printf("OUTLET CONTROL LUX DEBUG: Raw ADC=%d, Lux=%.3f, Relay1=%s, Relay2=%s\n",
                          luxRaw, lux,
                          relay1State ? "ON" : "OFF",
                          relay2State ? "ON" : "OFF");

            float amps = getValidatedCurrent();
            float volts = getValidatedVoltage();
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
              civHandler.setDeviceAddress(civAddrByte);
              sendDebugMessage("CI-V: Updated handler with new address: 0x" + String(civAddrByte, HEX) + " (decimal " + String(civAddrByte) + ")");

              client->text(JsonBuilder::buildInfoResponse("Device ID set to " + String(deviceId) + " (CIV: " + civAddress + ")"));
              sendDebugMessage("Device ID changed to " + String(deviceId) + ", CIV Address: " + civAddress + " (0x" + String(getCivAddressByte(), HEX) + ")");
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

        // Handle power calibration commands
        if (j.containsKey("command") && strcmp(j["command"] | "", "calibratePower") == 0)
        {
          float expectedPower = j["expectedPower"] | 0.0f;
          if (expectedPower > 0.0f && expectedPower <= 2000.0f) // Reasonable power range
          {
            float rawPower = hlw.getActivePower(); // Get uncalibrated reading
            if (rawPower > 0.0f)
            {
              float newCalibrationFactor = expectedPower / rawPower;
              powerCalibrationFactor = newCalibrationFactor;
              powerCalibrated = true;

              // Save calibration to preferences
              preferences.begin("calibration", false);
              preferences.putFloat("powerFactor", powerCalibrationFactor);
              preferences.putBool("powerCalibrated", powerCalibrated);
              preferences.end();

              client->text(JsonBuilder::buildInfoResponse("Power calibrated: factor=" + String(powerCalibrationFactor, 4) +
                                                          " (raw=" + String(rawPower, 1) + "W, expected=" + String(expectedPower, 1) + "W)"));
              sendDebugMessage("Power calibration set: factor=" + String(powerCalibrationFactor, 4) +
                               " raw=" + String(rawPower, 1) + "W -> " + String(expectedPower, 1) + "W");
            }
            else
            {
              client->text(JsonBuilder::buildErrorResponse("Cannot calibrate: no power reading available"));
              sendDebugMessage("Power calibration failed: raw power reading is 0");
            }
          }
          else
          {
            client->text(JsonBuilder::buildErrorResponse("Invalid expected power. Must be 1-2000W."));
            sendDebugMessage("Invalid power calibration value: " + String(expectedPower));
          }
          break;
        }

        // Handle reset power calibration
        if (j.containsKey("command") && strcmp(j["command"] | "", "resetPowerCalibration") == 0)
        {
          powerCalibrationFactor = 1.0f;
          powerCalibrated = false;

          // Clear calibration from preferences
          preferences.begin("calibration", false);
          preferences.remove("powerFactor");
          preferences.remove("powerCalibrated");
          preferences.end();

          client->text(JsonBuilder::buildInfoResponse("Power calibration reset to default (factor=1.0)"));
          sendDebugMessage("Power calibration reset to default");
          break;
        }

        // Handle get power calibration info
        if (j.containsKey("command") && strcmp(j["command"] | "", "getPowerCalibration") == 0)
        {
          float rawPower = hlw.getActivePower();
          float calibratedPower = rawPower * powerCalibrationFactor;

          String response = "{\"type\":\"powerCalibration\","
                            "\"calibrationFactor\":" +
                            String(powerCalibrationFactor, 4) + ","
                                                                "\"calibrated\":" +
                            String(powerCalibrated ? "true" : "false") + ","
                                                                         "\"rawPower\":" +
                            String(rawPower, 2) + ","
                                                  "\"calibratedPower\":" +
                            String(calibratedPower, 2) + "}";
          client->text(response);
          sendDebugMessage("Power calibration info sent: factor=" + String(powerCalibrationFactor, 4) +
                           " raw=" + String(rawPower, 1) + "W cal=" + String(calibratedPower, 1) + "W");
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
      // Use the existing unified CI-V message handler
      handleReceivedCivMessage(msg);
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
  // Use independent lux reading - no interference with power measurements
  float lux = HardwareController::readLuxSensor();

  // Debug for web interface lux reading
  Serial.printf("WEB LUX DEBUG: Independent Lux=%.3f\n", lux);

  float amps = getValidatedCurrent();
  float volts = getValidatedVoltage(); // Use calibrated voltage
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

  // Initialize CI-V handler with current device address
  uint8_t civAddrByte = getCivAddressByte();
  civHandler.init(civAddrByte);
  LOG_INFO("CI-V handler initialized with address: 0x" + String(civAddrByte, HEX));

  // Test the status LED during startup using hardware controller
  LOG_INFO("Testing Status LED - 3 blinks...");
  hardware.testStatusLED();
  Serial.println("Status LED test complete");

  // Initialize LED timer for captive portal blinking
  initLedTimer();

  // Configure LUX ADC pin as input with explicit no pull-up/pull-down
  pinMode(PIN_LUX_ADC, INPUT);
  gpio_pullup_dis((gpio_num_t)PIN_LUX_ADC);   // Explicitly disable pull-up
  gpio_pulldown_dis((gpio_num_t)PIN_LUX_ADC); // Explicitly disable pull-down

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

  // Load our power calibration factor
  powerCalibrationFactor = preferences.getFloat("powerFactor", 1.0f);
  powerCalibrated = preferences.getBool("powerCalibrated", false);
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

  if (powerCalibrated)
  {
    Serial.println("Loaded power calibration factor: " + String(powerCalibrationFactor, 4));
  }
  else
  {
    Serial.println("No power calibration found - using default factor: " + String(powerCalibrationFactor, 4));
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
  static unsigned long lastLuxRead = 0;
  static float lastLuxValue = 0.0f;

  // Independent lux sensor reading - polls frequently and independently of power measurements
  const unsigned long LUX_UPDATE_INTERVAL_MS = 1000; // Read lux every 1 second, independent of power
  if (currentTime - lastLuxRead >= LUX_UPDATE_INTERVAL_MS)
  {
    lastLuxRead = currentTime;
    lastLuxValue = HardwareController::readLuxSensor();

    // Debug lux sensor reading every 10 seconds
    static unsigned long lastLuxDebug = 0;
    if (millis() - lastLuxDebug > 10000)
    {
      Serial.printf("LUX INDEPENDENT DEBUG: Lux=%.3fV, Relay1=%s, Relay2=%s\n",
                    lastLuxValue, relay1State ? "ON" : "OFF", relay2State ? "ON" : "OFF");
      lastLuxDebug = millis();
    }
  }

  // Power sensor reading - completely separate from lux
  if (currentTime - lastSensorRead >= SENSOR_UPDATE_INTERVAL_MS)
  {
    lastSensorRead = currentTime;

    // Read power sensors only (no lux interference)
    float voltage = getValidatedVoltage(); // Use calibrated voltage
    float current = getValidatedCurrent();
    float power = getValidatedPower();

    // Use the independently-read lux value from above
    float lux = lastLuxValue;

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

// -------------------------------------------------------------------------
// Advanced Lux Sensor Debugging Functions
// -------------------------------------------------------------------------

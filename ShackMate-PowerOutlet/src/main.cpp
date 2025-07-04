/*
================================================================================
ShackMate Power Outlet - WebSocket Control Interface
Model: Wyze Outdoor Outlet (WLPPO1)
================================================================================

Control Output 1 and Output 2 via WebSocket Port 4000

Valid Commands:
  { "command": "output1", "value": true }
  { "command": "output1", "value": false }
  { "command": "output2", "value": true }
  { "command": "output2", "value": false }
  { "command": "setDeviceId", "deviceId": 1-4 }
  { "command": "reboot" }

Features:
- Persistent device configuration in NVS
- CI-V protocol support for ham radio integration
- Real-time sensor monitoring (voltage, current, power, light)
- Event-driven web interface updates
- WiFi configuration portal
- OTA firmware updates
- Hardware button control
================================================================================
*/

// ========================= SYSTEM INCLUDES =========================
#include <WiFi.h>
#include <WiFiManager.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <time.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>
#include <HLW8012.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_spi_flash.h>
#include <esp_task_wdt.h>
#include <WebSocketsClient.h>
#include <vector>

// ========================= SHACKMATE CORE LIBRARY =========================
#include <config.h>
#include <logger.h>
#include <device_state.h>
#include <hardware_controller.h>
#include <json_builder.h>
#include <network_manager.h>
#include <sensor_manager.h>
#include <event_manager.h>
#include <system_utils.h>
#include <web_server_manager.h>
#include <civ_handler.h>
#include <rate_limiter.h>

// ========================= EVENT-DRIVEN UPDATE SYSTEM =========================

// Sensor change detection (avoid update spam)
static float lastVoltage = 0.0f;
static float lastCurrent = 0.0f;
static float lastPower = 0.0f;
static float lastLux = 0.0f;
static bool lastRelay1State = false;
static bool lastRelay2State = false;
static bool lastCivConnected = false;

// Change detection thresholds
#define VOLTAGE_CHANGE_THRESHOLD 1.0f  // 1V
#define CURRENT_CHANGE_THRESHOLD 0.05f // 50mA
#define POWER_CHANGE_THRESHOLD 5.0f    // 5W
#define LUX_CHANGE_THRESHOLD 10.0f     // 10 lux units

// ========================= HARDWARE SENSOR SETUP =========================

// HLW8012 interrupt handlers for power monitoring
void IRAM_ATTR hlw8012_cf_interrupt();
void IRAM_ATTR hlw8012_cf1_interrupt();

void setInterrupts()
{
  attachInterrupt(digitalPinToInterrupt(PIN_HLW_CF1), hlw8012_cf1_interrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_HLW_CF), hlw8012_cf_interrupt, FALLING);
}

// ========================= GLOBAL OBJECTS =========================

// Core system objects
DeviceState deviceState;
HardwareController hardware;
Preferences preferences;
AsyncWebServer httpServer(80);
AsyncWebServer wsServer(4000);
HLW8012 hlw;

// HLW8012 interrupt handlers (must be defined after hlw object)
void IRAM_ATTR hlw8012_cf_interrupt()
{
  hlw.cf_interrupt();
}

void IRAM_ATTR hlw8012_cf1_interrupt()
{
  hlw.cf1_interrupt();
}

// ========================= GLOBAL STATE VARIABLES =========================

// Network and device configuration
String deviceIP = "";
String tcpPort = "4000";
String wsPortStr = tcpPort;

// Device persistent state
uint32_t rebootCounter = 0;
uint8_t deviceId = 1;
String civAddress = "B0";
bool relay1State = false;
bool relay2State = false;
char label1Text[32];
char label2Text[32];
char deviceName[64] = "ShackMate Power Outlet";

// Sensor calibration
float voltageCalibrationFactor = 1.0f;
bool voltageCalibrated = false;
float currentCalibrationFactor = 1.0f;
bool currentCalibrated = false;
unsigned long lastSensorUpdate = 0;

// WiFi captive portal state
bool captivePortalActive = false;
unsigned long statusLedLastToggle = 0;
bool statusLedState = false;

// Hardware button state (TODO: Move to HardwareController)
volatile bool button1Pressed = false;
volatile bool button2Pressed = false;
unsigned long lastButton1Time = 0;
unsigned long lastButton2Time = 0;
const unsigned long debounceDelay = 50;
bool lastButton1State = false;
bool lastButton2State = false;
bool button1StateStable = false;
bool button2StateStable = false;

// ========================= FORWARD DECLARATIONS =========================

// ========================= FUNCTION PROTOTYPES =========================

// LED Timer Management
void initLedTimer();
void startLedBlinking();
void stopLedBlinking();

// Event-Driven Update System
void initEventDrivenUpdates();
void checkSensorChanges();
void checkRelayStateChanges();
void checkCivConnectionChanges();
void processWebUpdateEvents();
void triggerRelayStateChangeEvent();
void triggerCivMessageEvent(const String &messageInfo = "");
void triggerCalibrationChangeEvent(const String &calibrationInfo = "");

bool getNextWebUpdateEvent(WebUpdateEvent *event);
bool hasWebUpdateEvents();

// Device State Management
void syncRelayStatesWithDeviceState();
uint8_t getCivAddressByte();

// Sensor Reading Functions
float getValidatedCurrent();
float getValidatedPower();

// WebSocket and Network
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, unsigned char *data, size_t len);
void sendDebugMessage(const String &message);

// HTTP Server Handlers
void handleRoot(AsyncWebServerRequest *request);
void handleDataJson(AsyncWebServerRequest *request);
void handleSaveConfig(AsyncWebServerRequest *request);
void handleRestoreConfig(AsyncWebServerRequest *request);

// File and Template Processing
String loadFile(const char *path);
String processTemplate(String tmpl);

// System Utility Functions
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

// OTA Task
void otaTask(void *pvParameters);

// ========================= SENSOR VALIDATION FUNCTIONS =========================

/**
 * @brief Get validated and calibrated current reading from HLW8012
 *
 * Applies calibration factor and validates against reasonable limits
 * to prevent spurious readings that can occur with power monitoring chips.
 *
 * @return float Validated and calibrated current reading in amperes
 */
float getValidatedCurrent()
{
  float rawCurrent = hlw.getCurrent();
  float calibratedCurrent = rawCurrent * currentCalibrationFactor;

  // Negative current doesn't make physical sense
  if (calibratedCurrent < 0.0f)
  {
    return 0.0f;
  }

  // Cap at reasonable maximum for household outlet (20A)
  const float MAX_REASONABLE_CURRENT = 20.0f;
  if (calibratedCurrent > MAX_REASONABLE_CURRENT)
  {
    Serial.printf("WARNING: Excessive current reading: %.3fA - capping at %.1fA\n",
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
  float current = getValidatedCurrent();
  float rawPower = hlw.getActivePower();

  // Validation thresholds
  const float MIN_CURRENT_THRESHOLD = 0.05f;  // 50mA minimum for valid power
  const float MAX_REASONABLE_POWER = 2000.0f; // 2000W maximum for this device

  // If current is below threshold, power should be zero
  if (current < MIN_CURRENT_THRESHOLD)
  {
    return 0.0f;
  }

  // Filter out spurious high power readings
  if (rawPower > MAX_REASONABLE_POWER)
  {
    Serial.printf("WARNING: Spurious power reading: %.1fW with %.3fA - setting to 0W\n",
                  rawPower, current);
    return 0.0f;
  }

  // Basic power factor validation (power shouldn't exceed V*I significantly)
  float voltage = hlw.getVoltage() * voltageCalibrationFactor;
  float apparentPower = voltage * current;

  // Power factor validation - real power shouldn't exceed apparent power
  if (rawPower > apparentPower * 1.1f)
  { // Allow 10% margin for measurement error
    Serial.printf("WARNING: Power %.1fW exceeds apparent power %.1fW (V=%.1f, I=%.3f) - setting to 0W\n",
                  rawPower, apparentPower, voltage, current);
    return 0.0f;
  }

  return rawPower;
}

/**
 * @brief Get CI-V address as byte value for current device ID
 *
 * @return uint8_t CI-V address byte (0xB0 + deviceId - 1)
 */
uint8_t getCivAddressByte()
{
  return civHandler.getCivAddressByte();
}

// ========================= DEBUG HELPER FUNCTIONS =========================

/**
 * @brief Send debug message to both serial console and WebSocket clients
 *
 * This function provides unified debug output that appears both in the serial
 * console (via Logger) and in the web interface debug window.
 *
 * @param message Debug message to send
 */
void sendDebugMessage(const String &message)
{
  // Log to serial console via Logger system
  LOG_DEBUG(message);

  // Send to WebSocket clients for real-time debugging in web interface
  String debugJson = "{\"type\":\"debug\",\"message\":\"" + message + "\"}";
  NetworkManager::broadcastToWebClients(debugJson);
}

// ========================= CI-V MESSAGE HANDLING =========================

/**
 * @brief Handle received CI-V message (wrapper for modular CI-V handler)
 * @param message Raw CI-V message string
 */
void handleReceivedCivMessage(const String &message)
{
  // Use the modular CI-V handler
  civHandler.handleReceivedCivMessage(message);
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

  // Configure watchdog timer for heavy CI-V traffic handling
  // 60 second timeout to prevent lockups during intensive message processing
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL); // Add current task to watchdog
  LOG_INFO("Watchdog timer configured: 60s timeout for heavy CI-V traffic");

  // Initialize the new logging system
  Logger::init(LogLevel::INFO);

  // Initialize device state management
  deviceState.init();

  // Note: Device ID is now loaded from NVS storage in deviceState.init()
  // No need to hardcode it here - let users set it via web interface
  LOG_INFO("Device ID loaded from storage: " + String(DeviceState::getDeviceConfig().deviceId));

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
  EventManager::initLedTimer();

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

  Serial.println("STARTUP: Loaded deviceId=" + String(deviceId) + " from DeviceState");
  Serial.println("STARTUP: Loaded civAddress=" + civAddress + " from DeviceState");

  // Ensure civAddress is correct for the current deviceId
  uint8_t calculatedCivAddr = getCivAddressByte();
  String calculatedCivAddrStr = String(calculatedCivAddr, HEX);
  calculatedCivAddrStr.toUpperCase();

  // Update civAddress if it doesn't match the calculated value
  if (civAddress != calculatedCivAddrStr)
  {
    Serial.println("CI-V address mismatch detected. Stored: " + civAddress + ", Calculated: " + calculatedCivAddrStr);
    civAddress = calculatedCivAddrStr;
    DeviceState::getDeviceConfig().civAddress = civAddress;
    Serial.println("CI-V address corrected to: " + civAddress);
  }

  Serial.println("Device ID: " + String(deviceId) + ", CIV Address: " + civAddress);

  // Initialize modular CI-V handler
  civHandler.setDebugCallback(sendDebugMessage);
  civHandler.init(deviceId);
  uint8_t civAddrByte = civHandler.getCivAddressByte();
  sendDebugMessage("CI-V: Modular CI-V handler initialized with address: 0x" + String(civAddrByte, HEX) + " (decimal " + String(civAddrByte) + ")");
  Serial.println("CI-V handler initialized with address: 0x" + String(civAddrByte, HEX));

  // Sync current relay states with CI-V handler
  civHandler.setRelayStates(relay1State, relay2State);

  // Log CI-V filtering configuration
  Serial.println("CI-V Address Filtering Configuration:");
  Serial.println("  Our CIV Address: 0x" + String(civAddrByte, HEX));
  Serial.println("  Broadcast Filtering: " + String(CIV_ENABLE_BROADCAST_FILTERING ? "ENABLED" : "DISABLED"));
  if (CIV_ENABLE_BROADCAST_FILTERING)
  {
    Serial.println("  Allowed Broadcast Source: 0x" + String(CIV_ALLOWED_BROADCAST_SOURCE, HEX));
    Serial.println("  Will ONLY respond to broadcasts FROM 0x" + String(CIV_ALLOWED_BROADCAST_SOURCE, HEX));
  }
  else
  {
    Serial.println("  Will respond to broadcasts from ANY source");
  }
  Serial.println("  Will respond to direct messages TO 0x" + String(civAddrByte, HEX));

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
    EventManager::startLedBlinking(); });

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

  // *** CI-V READY NOTIFICATION ***
  Serial.println("");
  Serial.println("================================================");
  Serial.println("        CI-V DEVICE READY FOR CONNECTION");
  Serial.println("================================================");
  Serial.println("Device IP: " + deviceIP);
  Serial.println("Device ID: " + String(DeviceState::getDeviceConfig().deviceId));
  Serial.println("CI-V Address: 0x" + String(getCivAddressByte(), HEX));
  Serial.println("UDP Discovery Port: " + String(UDP_PORT));
  Serial.println("Ready to receive 'ShackMate,IP,Port' discovery");
  Serial.println("================================================");
  Serial.println("");

  // Check if we were in captive portal mode and need to reboot for proper web server initialization
  bool wasInCaptivePortal = captivePortalActive;

  // Clear captive portal flag
  captivePortalActive = false;

  // Stop LED blinking timer
  EventManager::stopLedBlinking();

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

  // Test debug message to verify WebSocket debug window is working
  sendDebugMessage("=== DEBUG WINDOW TEST MESSAGE ===");
  sendDebugMessage("If you can see this, the debug WebSocket is working!");
  sendDebugMessage("Device IP: " + deviceIP);
  sendDebugMessage("Current Device ID: " + String(DeviceState::getDeviceConfig().deviceId));

  // *** ENHANCED DEBUG OUTPUT ***
  Serial.println("");
  Serial.println("████████████████████████████████████████████████");
  Serial.println("██ SHACKMATE OUTLET READY FOR CI-V COMMANDS  ██");
  Serial.println("████████████████████████████████████████████████");
  Serial.println("Device IP: " + deviceIP);
  Serial.println("Device ID: " + String(DeviceState::getDeviceConfig().deviceId));
  Serial.println("CI-V Address: 0x" + String(getCivAddressByte(), HEX));
  Serial.println("UDP Discovery Port: " + String(UDP_PORT));
  Serial.println("Broadcast Filtering: " + String(CIV_ENABLE_BROADCAST_FILTERING ? "ENABLED" : "DISABLED"));
  Serial.println("WebSocket Client Status: " + String(NetworkManager::isClientConnected() ? "CONNECTED" : "DISCONNECTED"));
  Serial.println("████████████████████████████████████████████████");
  Serial.println("");

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
                     // Re-enable watchdog timer after OTA with 60 second timeout for heavy CI-V traffic
                     esp_task_wdt_init(60, true);
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

  // -------------------------------------------------------------------------
  // Initialize Event-Driven Webpage Update System
  // -------------------------------------------------------------------------
  EventManager::init();
  Serial.println("Event-driven webpage update system initialized");

  // Initialize sensor baseline values for change detection
  lastVoltage = hlw.getVoltage() * voltageCalibrationFactor;
  lastCurrent = getValidatedCurrent();
  lastPower = getValidatedPower();
  lastLux = (analogRead(PIN_LUX_ADC) / 4095.0f) * 3.3f * (1000.0f / 3.3f);
  lastRelay1State = relay1State;
  lastRelay2State = relay2State;
  lastCivConnected = NetworkManager::isClientConnected();

  Serial.println("Sensor baseline values initialized for event-driven updates");
}

// Timer interrupt handlers
void IRAM_ATTR onSensorUpdateTimer();
void IRAM_ATTR onSystemStatusTimer();

// -------------------------------------------------------------------------
// Event-Driven Update Functions
// -------------------------------------------------------------------------

// Initialize event-driven update system
void initEventDrivenUpdates()
{
  EventManager::init();
}

// Check for significant sensor changes and queue events
void checkSensorChanges()
{
  float voltage = hlw.getVoltage() * voltageCalibrationFactor;
  float current = getValidatedCurrent();
  float power = getValidatedPower();

  // Read light sensor (LUX)
  uint16_t luxRaw = analogRead(PIN_LUX_ADC);
  float lux = (luxRaw / 4095.0f) * 3.3f * (1000.0f / 3.3f);

  bool significantChange = false;
  String changeDescription = "";

  // Check for significant voltage change
  if (abs(voltage - lastVoltage) >= VOLTAGE_CHANGE_THRESHOLD)
  {
    significantChange = true;
    changeDescription += "Voltage: " + String(lastVoltage, 1) + "V → " + String(voltage, 1) + "V ";
    lastVoltage = voltage;
  }

  // Check for significant current change
  if (abs(current - lastCurrent) >= CURRENT_CHANGE_THRESHOLD)
  {
    significantChange = true;
    changeDescription += "Current: " + String(lastCurrent, 3) + "A → " + String(current, 3) + "A ";
    lastCurrent = current;
  }

  // Check for significant power change
  if (abs(power - lastPower) >= POWER_CHANGE_THRESHOLD)
  {
    significantChange = true;
    changeDescription += "Power: " + String(lastPower, 1) + "W → " + String(power, 1) + "W ";
    lastPower = power;
  }

  // Check for significant lux change
  if (abs(lux - lastLux) >= LUX_CHANGE_THRESHOLD)
  {
    significantChange = true;
    changeDescription += "Lux: " + String(lastLux, 1) + " → " + String(lux, 1) + " ";
    lastLux = lux;
  }

  if (significantChange)
  {
    sendDebugMessage("Event: Significant sensor change detected - " + changeDescription);
    EventManager::queueEvent(WEB_EVENT_SENSOR_UPDATE, "");
  }

  // Always update DeviceState for consistency
  DeviceState::updateSensorData(lux, voltage, current, power);
}

// Check for relay state changes and queue events
void checkRelayStateChanges()
{
  if (relay1State != lastRelay1State || relay2State != lastRelay2State)
  {
    String stateChange = "Relay states changed: ";
    if (relay1State != lastRelay1State)
    {
      stateChange += "Output1: " + String(lastRelay1State ? "ON" : "OFF") + " → " + String(relay1State ? "ON" : "OFF") + " ";
    }
    if (relay2State != lastRelay2State)
    {
      stateChange += "Output2: " + String(lastRelay2State ? "ON" : "OFF") + " → " + String(relay2State ? "ON" : "OFF") + " ";
    }

    sendDebugMessage("Event: " + stateChange);
    EventManager::queueEvent(WEB_EVENT_RELAY_STATE_CHANGE, stateChange);

    lastRelay1State = relay1State;
    lastRelay2State = relay2State;
  }
}

// Check for CI-V connection status changes
void checkCivConnectionChanges()
{
  bool currentCivConnected = NetworkManager::isClientConnected();
  if (currentCivConnected != lastCivConnected)
  {
    String statusChange = "CI-V connection: " + String(lastCivConnected ? "CONNECTED" : "DISCONNECTED") +
                          " → " + String(currentCivConnected ? "CONNECTED" : "DISCONNECTED");

    sendDebugMessage("Event: " + statusChange);
    EventManager::queueEvent(WEB_EVENT_CONNECTION_STATUS_CHANGE, statusChange);

    lastCivConnected = currentCivConnected;
  }
}

// Process events and send updates to webpage
void processWebUpdateEvents()
{
  EventManager::processEvents();
}

// Trigger relay state change event (call this whenever relays change)
void triggerRelayStateChangeEvent()
{
  EventManager::queueEvent(WEB_EVENT_RELAY_STATE_CHANGE, "");
}

// Trigger CI-V message event
void triggerCivMessageEvent(const String &messageInfo)
{
  EventManager::queueEvent(WEB_EVENT_CIV_MESSAGE, messageInfo);
}

// Trigger calibration change event
void triggerCalibrationChangeEvent(const String &calibrationInfo)
{
  EventManager::queueEvent(WEB_EVENT_CALIBRATION_CHANGE, calibrationInfo);
}

// -------------------------------------------------------------------------
// WebSocket Event Handler
// -------------------------------------------------------------------------
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, unsigned char *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
  {
    Serial.printf("WebSocket client #%u connected\n", client->id());

    // Send a test debug message to verify the debug window is working
    sendDebugMessage("*** WebSocket client connected - Debug window is working! ***");
    sendDebugMessage("Current device ID: " + String(DeviceState::getDeviceConfig().deviceId));
    sendDebugMessage("Current CI-V address: 0x" + String(getCivAddressByte(), HEX));

    // Send initial state and status to newly connected client
    float lux = analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
    float amps = getValidatedCurrent();
    float rawV = hlw.getVoltage();
    float volts = rawV * voltageCalibrationFactor;
    float watts = getValidatedPower();
    DeviceState::updateSensorData(lux, volts, amps, watts);

    // Send current state (relay + labels)
    String stateMsg = JsonBuilder::buildStateResponse();
    client->text(stateMsg);

    // Send full status (including sensors & uptime)
    String statusMsg = JsonBuilder::buildStatusResponse();
    client->text(statusMsg);

    // Trigger connection status change event
    triggerRelayStateChangeEvent();
  }
  break;

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
    msg.trim();

    // Debug: Log all incoming WebSocket messages
    sendDebugMessage("WebSocket: Received message: '" + msg + "'");

    // Handle JSON commands
    if (msg.startsWith("{"))
    {
      sendDebugMessage("WebSocket: Processing JSON command...");
      DynamicJsonDocument j(128);
      DeserializationError error = deserializeJson(j, msg);

      if (error)
      {
        sendDebugMessage("WebSocket: JSON parse error: " + String(error.c_str()));
      }
      else
      {
        sendDebugMessage("WebSocket: JSON parsed successfully");

        // Debug: Show all JSON keys and values
        sendDebugMessage("WebSocket: JSON keys found:");
        for (JsonPair pair : j.as<JsonObject>())
        {
          sendDebugMessage("  Key: '" + String(pair.key().c_str()) + "', Value: '" + String(pair.value().as<String>()) + "'");
        }

        // Handle single-output commands
        if (j.containsKey("command") && j.containsKey("value"))
        {
          const char *cmd = j["command"] | "";
          sendDebugMessage("WebSocket: Command detected: '" + String(cmd) + "'");
          bool value = j["value"] | false;

          // Apply and persist new output state
          if (strcmp(cmd, "output1") == 0)
          {
            relay1State = value;
            hardware.setRelay(1, relay1State);
            syncRelayStatesWithDeviceState(); // Ensure sync after hardware update
            triggerRelayStateChangeEvent();
          }
          else if (strcmp(cmd, "output2") == 0)
          {
            relay2State = value;
            hardware.setRelay(2, relay2State);
            syncRelayStatesWithDeviceState(); // Ensure sync after hardware update
            triggerRelayStateChangeEvent();
          }
          break;
        }

        // Handle device ID change command - accept both "value" and "deviceId" formats
        if (j.containsKey("command") && strcmp(j["command"] | "", "setDeviceId") == 0)
        {
          uint8_t newDeviceId = 1; // Default fallback

          // Check for "value" format: { "command": "setDeviceId", "value": 3 }
          if (j.containsKey("value"))
          {
            newDeviceId = j["value"] | 1;
            sendDebugMessage("WebSocket: setDeviceId command received with 'value' key, newDeviceId=" + String(newDeviceId));
          }
          // Check for "deviceId" format: { "command": "setDeviceId", "deviceId": 3, "civAddress": "B2" }
          else if (j.containsKey("deviceId"))
          {
            newDeviceId = j["deviceId"] | 1;
            sendDebugMessage("WebSocket: setDeviceId command received with 'deviceId' key, newDeviceId=" + String(newDeviceId));
          }
          else
          {
            sendDebugMessage("WebSocket: setDeviceId command missing both 'value' and 'deviceId' keys - ignoring");
            break;
          }

          sendDebugMessage("WebSocket: Valid range is " + String(MIN_DEVICE_ID) + " to " + String(MAX_DEVICE_ID));

          if (newDeviceId >= MIN_DEVICE_ID && newDeviceId <= MAX_DEVICE_ID)
          {
            sendDebugMessage("WebSocket: Changing device ID from " + String(deviceId) + " to " + String(newDeviceId));

            // Update device ID and CI-V address
            DeviceState::setDeviceId(newDeviceId);
            deviceId = newDeviceId;

            // Update local civAddress variable
            uint8_t newCivAddr = getCivAddressByte();
            civAddress = String(newCivAddr, HEX);
            civAddress.toUpperCase();

            sendDebugMessage("WebSocket: Device ID updated to " + String(deviceId) + ", CI-V address now: 0x" + civAddress);

            // Send confirmation response
            String response = "Device ID changed to " + String(newDeviceId) + ", CI-V address: 0x" + civAddress + ". Change is effective immediately.";
            client->text(JsonBuilder::buildInfoResponse(response));

            // Trigger status update to refresh web clients with new device info
            triggerRelayStateChangeEvent();
          }
          else
          {
            String errorMsg = "Invalid device ID " + String(newDeviceId) + ". Must be between " + String(MIN_DEVICE_ID) + " and " + String(MAX_DEVICE_ID);
            sendDebugMessage("WebSocket: " + errorMsg);
            client->text(JsonBuilder::buildInfoResponse(errorMsg));
          }
          break;
        }

        // Handle reboot command
        if (j.containsKey("command") && strcmp(j["command"] | "", "reboot") == 0)
        {
          sendDebugMessage("WebSocket: Reboot command received");
          client->text(JsonBuilder::buildInfoResponse("Rebooting device..."));
          delay(250);
          ESP.restart();
          break;
        }

        // Handle commands with just "command" key (no "value") - catch-all for unhandled commands
        if (j.containsKey("command") && !j.containsKey("value"))
        {
          const char *cmd = j["command"] | "";
          sendDebugMessage("WebSocket: Unhandled command (no value) detected: '" + String(cmd) + "'");
        }
      }
    }
    // Handle CI-V hex messages (non-JSON)
    else if (!msg.startsWith("{") && msg.length() > 0)
    {
      sendDebugMessage("WebSocket: Processing CI-V hex message: " + msg);
      handleReceivedCivMessage(msg);
    }
  }
  break;

  default:
    break;
  }
}

// -------------------------------------------------------------------------
// Main Loop - Event-Driven Processing
// -------------------------------------------------------------------------

void loop()
{
  // Handle OTA updates
  ArduinoOTA.handle();

  // Reset watchdog timer to prevent resets during normal operation
  esp_task_wdt_reset();

  // Performance optimization: Yield to other tasks before heavy network processing
  yield();

  // Update network manager (handles WebSocket client, UDP discovery) - CRITICAL!
  NetworkManager::update();

  // Yield after network processing to prevent blocking
  yield();

  // Process event-driven sensor updates (triggered by hardware timer)
  if (EventManager::isSensorUpdateTriggered())
  {
    EventManager::clearSensorUpdateFlag();
    checkSensorChanges();
    checkRelayStateChanges();
    checkCivConnectionChanges();

    // Yield after sensor processing
    yield();
  }

  // Process event-driven system status updates (triggered by hardware timer)
  if (EventManager::isSystemStatusTriggered())
  {
    EventManager::clearSystemStatusFlag();
    EventManager::queueEvent(WEB_EVENT_SYSTEM_STATUS, "");
  }

  // Enhanced debug logging for WebSocket connection status (reduced frequency when connected)
  static unsigned long lastWebSocketDebug = 0;
  static bool lastConnectionState = false;
  bool isConnected = NetworkManager::isClientConnected();

  // Log immediately when connection state changes, or periodically if disconnected
  bool shouldLog = (isConnected != lastConnectionState) ||
                   (!isConnected && (millis() - lastWebSocketDebug >= 30000)) ||
                   (isConnected && (millis() - lastWebSocketDebug >= 120000)); // 2 minutes when connected

  if (shouldLog)
  {
    sendDebugMessage("=== WebSocket Connection Status ===");
    sendDebugMessage("Connected to server: " + String(isConnected ? "YES" : "NO"));
    if (isConnected)
    {
      sendDebugMessage("Ready to receive CI-V commands at address 0x" + String(getCivAddressByte(), HEX));
    }
    else
    {
      sendDebugMessage("WARNING: Not connected to CI-V server - will not receive commands");
    }
    sendDebugMessage("Device IP: " + WiFi.localIP().toString());
    sendDebugMessage("================================");
    lastWebSocketDebug = millis();
    lastConnectionState = isConnected;
  }

  // Network discovery status - only log when disconnected or occasionally when connected
  static unsigned long lastNetworkDebug = 0;
  if ((!isConnected && (millis() - lastNetworkDebug >= 30000)) ||
      (isConnected && (millis() - lastNetworkDebug >= 300000))) // 5 minutes when connected
  {
    if (!isConnected)
    {
      sendDebugMessage("=== Network Discovery Status ===");
      sendDebugMessage("Listening for UDP discovery on port " + String(UDP_PORT));
      sendDebugMessage("Looking for 'ShackMate,IP,Port' messages");
      sendDebugMessage("Will auto-connect to discovered CI-V server");
      sendDebugMessage("==============================");
    }
    lastNetworkDebug = millis();
  }

  // Heap monitoring during heavy CI-V traffic
  static unsigned long lastHeapCheck = 0;
  static uint32_t minHeap = ESP.getFreeHeap();
  unsigned long currentTime = millis();    // Define currentTime for heap monitoring
  if (currentTime - lastHeapCheck > 30000) // Every 30 seconds
  {
    uint32_t currentHeap = ESP.getFreeHeap();
    if (currentHeap < minHeap)
    {
      minHeap = currentHeap;
    }

    if (currentHeap < 10000) // Less than 10KB free
    {
      sendDebugMessage("WARNING: Low heap memory: " + String(currentHeap) + " bytes free");
      sendDebugMessage("Minimum heap seen: " + String(minHeap) + " bytes");
    }

    lastHeapCheck = currentTime;
  }

  // Check for button presses using hardware controller
  if (hardware.checkButton1Pressed())
  {
    relay1State = !relay1State; // Toggle relay 1
    hardware.setRelay(1, relay1State);
    syncRelayStatesWithDeviceState(); // Ensure sync after hardware update
    sendDebugMessage("Button 1 pressed - toggled Outlet 1 to " + String(relay1State ? "ON" : "OFF"));
    triggerRelayStateChangeEvent();
  }

  if (hardware.checkButton2Pressed())
  {
    relay2State = !relay2State; // Toggle relay 2
    hardware.setRelay(2, relay2State);
    syncRelayStatesWithDeviceState(); // Ensure sync after hardware update
    sendDebugMessage("Button 2 pressed - toggled Outlet 2 to " + String(relay2State ? "ON" : "OFF"));
    triggerRelayStateChangeEvent();
  }

  // Process web update events and broadcast to clients
  // Process events using EventManager
  EventManager::processEvents();

  // Yield before delay to ensure other tasks can run
  yield();

  // Increased delay for heavy CI-V traffic handling - prevents CPU starvation
  delay(20); // Increased from 10ms to 20ms for better stability
}

// -------------------------------------------------------------------------
// File Loading and Template Processing Functions
// -------------------------------------------------------------------------

String loadFile(const char *path)
{
  File file = SPIFFS.open(path, "r");
  if (!file)
  {
    Serial.println("Failed to open file: " + String(path));
    return "";
  }

  String content = file.readString();
  file.close();

  Serial.println("Loaded file: " + String(path) + " (" + String(content.length()) + " bytes)");
  return content;
}

String processTemplate(String tmpl)
{
  // Replace template variables with actual values
  tmpl.replace("%PROJECT_NAME%", "ShackMate Outlet");
  tmpl.replace("%DEVICE_NAME%", String(deviceName));
  tmpl.replace("%DEVICE_IP%", deviceIP);
  tmpl.replace("%WEBSOCKET_PORT%", wsPortStr);
  tmpl.replace("%UPTIME%", getUptime());
  tmpl.replace("%REBOOT_COUNT%", String(rebootCounter));
  tmpl.replace("%CHIP_ID%", getChipID());
  tmpl.replace("%FREE_HEAP%", String(ESP.getFreeHeap()));
  tmpl.replace("%VERSION%", String(VERSION));
  tmpl.replace("%LABEL1%", String(label1Text));
  tmpl.replace("%LABEL2%", String(label2Text));
  tmpl.replace("%RELAY1_STATE%", relay1State ? "true" : "false");
  tmpl.replace("%RELAY2_STATE%", relay2State ? "true" : "false");
  tmpl.replace("%CIV_ADDRESS%", civAddress);
  tmpl.replace("%DEVICE_ID%", String(deviceId));

  return tmpl;
}

// -------------------------------------------------------------------------
// Utility Functions
// -------------------------------------------------------------------------

String getUptime()
{
  unsigned long uptimeMs = millis();
  unsigned long seconds = uptimeMs / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;

  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  String uptime = "";
  if (days > 0)
    uptime += String(days) + "d ";
  if (hours > 0)
    uptime += String(hours) + "h ";
  if (minutes > 0)
    uptime += String(minutes) + "m ";
  uptime += String(seconds) + "s";

  return uptime;
}

String getChipID()
{
  uint64_t chipid = ESP.getEfuseMac();
  return String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
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
  return ESP.getPsramSize();
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
  return ESP.getHeapSize();
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
  // ESP32 internal temperature sensor (approximate)
  return temperatureRead();
}

// -------------------------------------------------------------------------
// Relay State Synchronization Helper Function
// -------------------------------------------------------------------------

/**
 * @brief Synchronize global relay state variables with DeviceState
 *
 * This ensures that the global relay1State and relay2State variables
 * are always in sync with the DeviceState. Should be called after
 * any operation that might modify DeviceState independently.
 */
void syncRelayStatesWithDeviceState()
{
  const auto &relayState = DeviceState::getRelayState();
  relay1State = relayState.relay1;
  relay2State = relayState.relay2;

  // Sync with CI-V handler as well
  civHandler.setRelayStates(relay1State, relay2State);
}

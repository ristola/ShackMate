#pragma once

// -------------------------------------------------------------------------
// Project Configuration
// -------------------------------------------------------------------------
#define NAME "ShackMate - Outlet"
#define VERSION "2.1.0"
#define AUTHOR "Half Baked Circuits"
#define MDNS_NAME "ShackMate-PowerOutlet"

// Network Configuration
#define UDP_PORT 4210
#define WEBSOCKET_PORT 4000
#define HTTP_PORT 80

// Hardware Pin Definitions
#define PIN_RELAY1 15
#define PIN_RELAY2 32
#define PIN_RELAY1_LED 19
#define PIN_RELAY2_LED 16
#define PIN_BUTTON1 18
#define PIN_BUTTON2 17
#define PIN_STATUS_LED 5 // Inverted logic (LOW=ON, HIGH=OFF)

// Sensor Pin Definitions
#define PIN_LUX_ADC 34
#define PIN_HLW_CF 27
#define PIN_HLW_CF1 26
#define PIN_HLW_SEL 25

// Hardware Calibration Constants
static constexpr float CURRENT_RESISTOR = 0.001f;
static constexpr float VOLTAGE_DIVIDER = 770.0f;

// Timing Constants
static constexpr uint32_t SENSOR_UPDATE_INTERVAL_MS = 10000;  // 10 seconds
static constexpr uint32_t STATUS_LED_BLINK_INTERVAL_MS = 250; // 250ms
static constexpr unsigned long DEBOUNCE_DELAY_MS = 50;
static constexpr unsigned long CONNECTION_COOLDOWN_MS = 10000;
static constexpr unsigned long WEBSOCKET_TIMEOUT_MS = 60000;
static constexpr unsigned long PING_INTERVAL_MS = 30000;

// Memory Constants
static constexpr uint32_t CRITICAL_HEAP_THRESHOLD = 30000;
static constexpr size_t MAX_DEVICE_NAME_LENGTH = 64;
static constexpr size_t MAX_LABEL_LENGTH = 32;
static constexpr size_t MAX_CIV_MESSAGE_LENGTH = 128;

// Sensor Change Detection Thresholds
static constexpr float VOLTAGE_CHANGE_THRESHOLD = 1.0f;  // 1V
static constexpr float CURRENT_CHANGE_THRESHOLD = 0.05f; // 50mA
static constexpr float POWER_CHANGE_THRESHOLD = 5.0f;    // 5W
static constexpr float LUX_CHANGE_THRESHOLD = 10.0f;     // 10 lux units

// Device Configuration
#define MIN_DEVICE_ID 1
#define MAX_DEVICE_ID 4
#define DEFAULT_DEVICE_ID 1
#define DEFAULT_CIV_ADDRESS "B0"
#define DEFAULT_DEVICE_NAME "ShackMate PowerOutlet"

// CI-V Model Type Constants
#define CIV_MODEL_ATOM_POWER_OUTLET 0x00   // 00 = ATOM Power Outlet
#define CIV_MODEL_WYZE_OUTDOOR_OUTLET 0x01 // 01 = Wyze Outdoor Power Outlet
#define DEFAULT_CIV_MODEL_TYPE CIV_MODEL_WYZE_OUTDOOR_OUTLET

// CI-V Address Filtering Configuration
#define CIV_ALLOWED_BROADCAST_SOURCE 0xEE    // Only accept broadcast messages from this address
#define CIV_ENABLE_BROADCAST_FILTERING false // Set to false to disable broadcast filtering

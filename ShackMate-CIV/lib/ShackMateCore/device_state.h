#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

// -------------------------------------------------------------------------
// Device State Management
// -------------------------------------------------------------------------
struct RelayState
{
    bool relay1 = false;
    bool relay2 = false;
    char label1[MAX_LABEL_LENGTH] = "Output 1";
    char label2[MAX_LABEL_LENGTH] = "Output 2";
};

struct DeviceConfig
{
    uint8_t deviceId = DEFAULT_DEVICE_ID;
    String civAddress = DEFAULT_CIV_ADDRESS;
    char deviceName[MAX_DEVICE_NAME_LENGTH] = DEFAULT_DEVICE_NAME;
    String tcpPort = "4000";
    uint32_t rebootCounter = 0;
};

struct CalibrationData
{
    float voltageCalibrationFactor = 1.0f;
    float currentMultiplier = 0.0f;
    float voltageMultiplier = 0.0f;
    float powerMultiplier = 0.0f;
    bool isCalibrated = false;
};

struct SensorData
{
    float lux = 0.0f;
    float voltage = 0.0f;
    float current = 0.0f;
    float power = 0.0f;
    unsigned long lastUpdate = 0;
};

struct ConnectionState
{
    bool wsClientConnected = false;
    bool wsClientEverConnected = false;
    String connectedServerIP = "";
    uint16_t connectedServerPort = 0;
    unsigned long lastConnectionAttempt = 0;
    unsigned long lastWebSocketActivity = 0;
    unsigned long lastPingSent = 0;
};

struct WebSocketMetrics
{
    uint32_t disconnects = 0;
    uint32_t reconnects = 0;
    uint32_t reconnect_attempts = 0;
    uint32_t rate_limited_messages = 0;
    uint32_t messages_sent = 0;
    uint32_t messages_rate_limited = 0; // Add missing field
    uint32_t total_disconnects = 0;     // Add missing field
    uint32_t ping_rtt = 0;
    uint8_t connection_quality = 0;
    bool ping_pending = false; // Add missing field
    unsigned long last_ping_sent = 0;
    unsigned long last_ping_received = 0;
    unsigned long last_pong_received = 0; // Add missing field
};

class DeviceState
{
private:
    static RelayState relayState;
    static DeviceConfig deviceConfig;
    static CalibrationData calibrationData;
    static SensorData sensorData;
    static ConnectionState connectionState;
    static WebSocketMetrics webSocketMetrics;
    static unsigned long bootTime;

public:
    static void init();
    static void loadFromPreferences();
    static void saveToPreferences();

    // Relay State
    static RelayState &getRelayState() { return relayState; }
    static void setRelayState(bool relay1, bool relay2);
    static void setRelayLabel(int relayNum, const String &label);

    // Device Config
    static DeviceConfig &getDeviceConfig() { return deviceConfig; }
    static void setDeviceId(uint8_t id);
    static void setDeviceName(const String &name);
    static uint8_t getCivAddressByte();
    static void incrementRebootCounter();

    // Calibration
    static CalibrationData &getCalibrationData() { return calibrationData; }
    static void setCalibration(float current, float voltage, float power);

    // Sensor Data
    static SensorData &getSensorData() { return sensorData; }
    static void updateSensorData(float lux, float voltage, float current, float power);

    // Connection State
    static ConnectionState &getConnectionState() { return connectionState; }
    static void setConnectionState(bool connected, const String &ip, uint16_t port);

    // WebSocket Metrics
    static WebSocketMetrics &getWebSocketMetrics() { return webSocketMetrics; }
    static void updateWebSocketMetrics(const WebSocketMetrics &metrics);

    // System Info
    static void setBootTime(unsigned long time) { bootTime = time; }
    static unsigned long getBootTime() { return bootTime; }
    static String getUptime();
    static String getSystemInfo();
};

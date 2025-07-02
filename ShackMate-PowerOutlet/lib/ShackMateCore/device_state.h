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

class DeviceState
{
private:
    static RelayState relayState;
    static DeviceConfig deviceConfig;
    static CalibrationData calibrationData;
    static SensorData sensorData;
    static ConnectionState connectionState;
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

    // System Info
    static void setBootTime(unsigned long time) { bootTime = time; }
    static unsigned long getBootTime() { return bootTime; }
    static String getUptime();
    static String getSystemInfo();
};

#include "device_state.h"
#include "logger.h"
#include <esp_system.h>

// Static member definitions
RelayState DeviceState::relayState;
DeviceConfig DeviceState::deviceConfig;
CalibrationData DeviceState::calibrationData;
SensorData DeviceState::sensorData;
ConnectionState DeviceState::connectionState;
unsigned long DeviceState::bootTime = 0;

void DeviceState::init()
{
    bootTime = millis();
    loadFromPreferences();
    incrementRebootCounter();
    LOG_INFO("Device state initialized");
}

void DeviceState::loadFromPreferences()
{
    // Load relay states
    Preferences prefs;
    prefs.begin("outlet", true);
    relayState.relay1 = prefs.getBool("output1", false);
    relayState.relay2 = prefs.getBool("output2", false);
    prefs.end();

    // Load labels and device name
    prefs.begin("labels", true);
    prefs.getString("label1", relayState.label1, sizeof(relayState.label1));
    prefs.getString("label2", relayState.label2, sizeof(relayState.label2));
    prefs.getString("deviceName", deviceConfig.deviceName, sizeof(deviceConfig.deviceName));
    prefs.end();

    // Load device configuration
    prefs.begin("config", true);
    deviceConfig.deviceId = prefs.getUChar("deviceId", DEFAULT_DEVICE_ID);
    deviceConfig.civAddress = prefs.getString("civAddress", DEFAULT_CIV_ADDRESS);
    deviceConfig.tcpPort = prefs.getString("tcp_port", "4000");
    prefs.end();

    Serial.println("NVS LOAD: deviceId=" + String(deviceConfig.deviceId) + ", civAddress=" + deviceConfig.civAddress);
    Serial.println("NVS LOAD: DEFAULT_DEVICE_ID=" + String(DEFAULT_DEVICE_ID) + ", DEFAULT_CIV_ADDRESS=" + String(DEFAULT_CIV_ADDRESS));

    // Load system data
    prefs.begin("system", true);
    deviceConfig.rebootCounter = prefs.getUInt("rebootCount", 0);
    prefs.end();

    // Load calibration data
    prefs.begin("calibration", true);
    calibrationData.currentMultiplier = prefs.getFloat("currentMultiplier", 0.0f);
    calibrationData.voltageMultiplier = prefs.getFloat("voltageMultiplier", 0.0f);
    calibrationData.powerMultiplier = prefs.getFloat("powerMultiplier", 0.0f);
    calibrationData.isCalibrated = (calibrationData.currentMultiplier > 0 &&
                                    calibrationData.voltageMultiplier > 0 &&
                                    calibrationData.powerMultiplier > 0);
    prefs.end();

    LOG_INFO("Preferences loaded successfully");
}

void DeviceState::saveToPreferences()
{
    Preferences prefs;

    // Save relay states
    prefs.begin("outlet", false);
    prefs.putBool("output1", relayState.relay1);
    prefs.putBool("output2", relayState.relay2);
    prefs.end();

    // Save labels and device name
    prefs.begin("labels", false);
    prefs.putString("label1", relayState.label1);
    prefs.putString("label2", relayState.label2);
    prefs.putString("deviceName", deviceConfig.deviceName);
    prefs.end();

    // Save device configuration
    prefs.begin("config", false);
    prefs.putUChar("deviceId", deviceConfig.deviceId);
    prefs.putString("civAddress", deviceConfig.civAddress);
    prefs.putString("tcp_port", deviceConfig.tcpPort);
    prefs.end();

    // Save system data
    prefs.begin("system", false);
    prefs.putUInt("rebootCount", deviceConfig.rebootCounter);
    prefs.end();
}

void DeviceState::setRelayState(bool relay1, bool relay2)
{
    relayState.relay1 = relay1;
    relayState.relay2 = relay2;

    Preferences prefs;
    prefs.begin("outlet", false);
    prefs.putBool("output1", relay1);
    prefs.putBool("output2", relay2);
    prefs.end();
}

void DeviceState::setRelayLabel(int relayNum, const String &label)
{
    Preferences prefs;
    prefs.begin("labels", false);

    if (relayNum == 1)
    {
        strncpy(relayState.label1, label.c_str(), sizeof(relayState.label1) - 1);
        relayState.label1[sizeof(relayState.label1) - 1] = '\0';
        prefs.putString("label1", relayState.label1);
    }
    else if (relayNum == 2)
    {
        strncpy(relayState.label2, label.c_str(), sizeof(relayState.label2) - 1);
        relayState.label2[sizeof(relayState.label2) - 1] = '\0';
        prefs.putString("label2", relayState.label2);
    }

    prefs.end();
}

void DeviceState::setDeviceId(uint8_t id)
{
    Serial.println("DeviceState::setDeviceId() called with id=" + String(id));
    Serial.println("Valid range: " + String(MIN_DEVICE_ID) + " to " + String(MAX_DEVICE_ID));

    if (id >= MIN_DEVICE_ID && id <= MAX_DEVICE_ID)
    {
        Serial.println("ID " + String(id) + " is within valid range, updating...");
        deviceConfig.deviceId = id;

        // Calculate the correct CI-V address: 0xB0 + (deviceId - 1)
        uint8_t civAddrByte = 0xB0 + (id - 1);
        deviceConfig.civAddress = String(civAddrByte, HEX);
        deviceConfig.civAddress.toUpperCase();

        Serial.println("Calculated CI-V address: 0x" + deviceConfig.civAddress);

        Preferences prefs;
        bool prefsOpened = prefs.begin("config", false);
        Serial.println("Preferences.begin('config', false) returned: " + String(prefsOpened ? "SUCCESS" : "FAILED"));

        if (prefsOpened)
        {
            size_t written1 = prefs.putUChar("deviceId", id);
            size_t written2 = prefs.putString("civAddress", deviceConfig.civAddress);
            prefs.end();

            Serial.println("NVS write results: deviceId=" + String(written1) + " bytes, civAddress=" + String(written2) + " bytes");

            // Verify the write by reading it back
            prefs.begin("config", true);
            uint8_t readBackId = prefs.getUChar("deviceId", 0);
            String readBackAddr = prefs.getString("civAddress", "");
            prefs.end();

            Serial.println("Verification read: deviceId=" + String(readBackId) + ", civAddress=" + readBackAddr);
        }
        else
        {
            Serial.println("ERROR: Failed to open NVS preferences for writing!");
        }

        LOG_INFO("Device ID set to " + String(id) + " (CIV: 0x" + deviceConfig.civAddress + ")");
    }
    else
    {
        Serial.println("ERROR: Device ID " + String(id) + " is outside valid range " + String(MIN_DEVICE_ID) + "-" + String(MAX_DEVICE_ID));
    }
}

void DeviceState::setDeviceName(const String &name)
{
    if (name.length() > 0 && name.length() < sizeof(deviceConfig.deviceName))
    {
        strncpy(deviceConfig.deviceName, name.c_str(), sizeof(deviceConfig.deviceName) - 1);
        deviceConfig.deviceName[sizeof(deviceConfig.deviceName) - 1] = '\0';

        Preferences prefs;
        prefs.begin("labels", false);
        prefs.putString("deviceName", deviceConfig.deviceName);
        prefs.end();

        LOG_INFO("Device name set to: " + name);
    }
}

uint8_t DeviceState::getCivAddressByte()
{
    return 0xB0 + (deviceConfig.deviceId - 1);
}

void DeviceState::incrementRebootCounter()
{
    deviceConfig.rebootCounter++;
    Preferences prefs;
    prefs.begin("system", false);
    prefs.putUInt("rebootCount", deviceConfig.rebootCounter);
    prefs.end();
}

void DeviceState::setCalibration(float current, float voltage, float power)
{
    calibrationData.currentMultiplier = current;
    calibrationData.voltageMultiplier = voltage;
    calibrationData.powerMultiplier = power;
    calibrationData.isCalibrated = true;

    Preferences prefs;
    prefs.begin("calibration", false);
    prefs.putFloat("currentMultiplier", current);
    prefs.putFloat("voltageMultiplier", voltage);
    prefs.putFloat("powerMultiplier", power);
    prefs.end();
}

void DeviceState::updateSensorData(float lux, float voltage, float current, float power)
{
    sensorData.lux = lux;
    sensorData.voltage = voltage;
    sensorData.current = current;
    sensorData.power = power;
    sensorData.lastUpdate = millis();
}

void DeviceState::setConnectionState(bool connected, const String &ip, uint16_t port)
{
    connectionState.wsClientConnected = connected;
    if (connected)
    {
        connectionState.wsClientEverConnected = true;
        connectionState.connectedServerIP = ip;
        connectionState.connectedServerPort = port;
        connectionState.lastWebSocketActivity = millis();
    }
}

String DeviceState::getUptime()
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

String DeviceState::getSystemInfo()
{
    String info = "=== System Information ===\n";
    info += "Version: " + String(VERSION) + "\n";
    info += "Uptime: " + getUptime() + "\n";
    info += "Reboot Count: " + String(deviceConfig.rebootCounter) + "\n";
    info += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    info += "CPU Freq: " + String(ESP.getCpuFreqMHz()) + "MHz\n";
    info += "Flash Size: " + String(ESP.getFlashChipSize()) + " bytes\n";
    return info;
}

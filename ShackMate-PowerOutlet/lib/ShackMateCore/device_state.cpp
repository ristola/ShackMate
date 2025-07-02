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
    if (id >= MIN_DEVICE_ID && id <= MAX_DEVICE_ID)
    {
        deviceConfig.deviceId = id;
        deviceConfig.civAddress = "B" + String(id - 1);

        Preferences prefs;
        prefs.begin("config", false);
        prefs.putUChar("deviceId", id);
        prefs.putString("civAddress", deviceConfig.civAddress);
        prefs.end();

        LOG_INFO("Device ID set to " + String(id) + " (CIV: " + deviceConfig.civAddress + ")");
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

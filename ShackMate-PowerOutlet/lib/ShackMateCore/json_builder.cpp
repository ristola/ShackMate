#include "json_builder.h"
#include "logger.h"
#include <esp_system.h>

String JsonBuilder::buildStateResponse()
{
    DynamicJsonDocument doc(STATE_JSON_SIZE);
    const auto &relayState = DeviceState::getRelayState();
    const auto &deviceConfig = DeviceState::getDeviceConfig();

    doc["type"] = "state";
    doc["output1State"] = relayState.relay1;
    doc["output2State"] = relayState.relay2;
    doc["label1"] = relayState.label1;
    doc["label2"] = relayState.label2;
    doc["deviceName"] = deviceConfig.deviceName;

    String result;
    if (serializeJson(doc, result) == 0)
    {
        LOG_ERROR("Failed to serialize state JSON");
        return "{}";
    }

    return result;
}

String JsonBuilder::buildStatusResponse()
{
    DynamicJsonDocument doc(STATUS_JSON_SIZE);
    const auto &relayState = DeviceState::getRelayState();
    const auto &deviceConfig = DeviceState::getDeviceConfig();
    const auto &sensorData = DeviceState::getSensorData();

    doc["type"] = "status";
    doc["uptime"] = DeviceState::getUptime();
    doc["output1State"] = relayState.relay1;
    doc["output2State"] = relayState.relay2;
    doc["label1"] = relayState.label1;
    doc["label2"] = relayState.label2;
    doc["deviceName"] = deviceConfig.deviceName;

    // Add sensor data
    addSensorInfo(doc);

    // Add connection info
    addConnectionInfo(doc);

    // Add system info
    addSystemInfo(doc);

    String result;
    if (serializeJson(doc, result) == 0)
    {
        LOG_ERROR("Failed to serialize status JSON");
        return "{}";
    }

    return result;
}

String JsonBuilder::buildInfoResponse(const String &message)
{
    DynamicJsonDocument doc(RESPONSE_JSON_SIZE);
    doc["type"] = "info";
    doc["msg"] = message;

    String result;
    serializeJson(doc, result);
    return result;
}

String JsonBuilder::buildErrorResponse(const String &message)
{
    DynamicJsonDocument doc(RESPONSE_JSON_SIZE);
    doc["type"] = "error";
    doc["msg"] = message;

    String result;
    serializeJson(doc, result);
    return result;
}

String JsonBuilder::buildLabelResponse(int outlet, const String &text)
{
    DynamicJsonDocument doc(RESPONSE_JSON_SIZE);
    doc["type"] = "labels";
    doc["outlet"] = outlet;
    doc["text"] = text;

    String result;
    serializeJson(doc, result);
    return result;
}

String JsonBuilder::buildDeviceNameResponse(const String &name)
{
    DynamicJsonDocument doc(RESPONSE_JSON_SIZE);
    doc["type"] = "deviceName";
    doc["text"] = name;

    String result;
    serializeJson(doc, result);
    return result;
}

String JsonBuilder::buildPongResponse(unsigned long timestamp)
{
    DynamicJsonDocument doc(RESPONSE_JSON_SIZE);
    doc["type"] = "pong";
    doc["timestamp"] = timestamp;

    String result;
    serializeJson(doc, result);
    return result;
}

String JsonBuilder::buildSensorDataResponse(float lux, float amps, float volts, float watts)
{
    DynamicJsonDocument doc(RESPONSE_JSON_SIZE);
    doc["lux"] = round(lux * 10) / 10.0;     // 1 decimal place
    doc["amps"] = round(amps * 100) / 100.0; // 2 decimal places
    doc["volts"] = round(volts * 10) / 10.0; // 1 decimal place
    doc["watts"] = round(watts);             // No decimal places

    String result;
    serializeJson(doc, result);
    return result;
}

void JsonBuilder::addConnectionInfo(JsonDocument &doc)
{
    const auto &connectionState = DeviceState::getConnectionState();
    const auto &deviceConfig = DeviceState::getDeviceConfig();

    doc["civServerConnected"] = connectionState.wsClientConnected;
    doc["civServerEverConnected"] = connectionState.wsClientEverConnected;
    doc["civServerIP"] = connectionState.connectedServerIP;
    doc["civServerPort"] = connectionState.connectedServerPort;
    doc["deviceId"] = deviceConfig.deviceId;
    doc["civAddress"] = deviceConfig.civAddress;
}

void JsonBuilder::addSystemInfo(JsonDocument &doc)
{
    const auto &deviceConfig = DeviceState::getDeviceConfig();

    doc["udpPort"] = UDP_PORT;
    doc["psramSize"] = psramFound() ? ESP.getPsramSize() : 0;
    doc["version"] = VERSION;
    doc["chipId"] = ESP.getEfuseMac();
    doc["chipRevision"] = ESP.getChipRevision();
    doc["cpuFreq"] = ESP.getCpuFreqMHz();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["totalHeap"] = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["rebootCount"] = deviceConfig.rebootCounter;
}

void JsonBuilder::addSensorInfo(JsonDocument &doc)
{
    const auto &sensorData = DeviceState::getSensorData();
    const auto &calibrationData = DeviceState::getCalibrationData();

    doc["lux"] = round(sensorData.lux * 10) / 10.0;
    doc["amps"] = round(sensorData.current * 100) / 100.0;
    doc["volts"] = round(sensorData.voltage * 10) / 10.0;
    doc["watts"] = round(sensorData.power);

    if (calibrationData.isCalibrated)
    {
        doc["currentMultiplier"] = calibrationData.currentMultiplier;
        doc["voltageMultiplier"] = calibrationData.voltageMultiplier;
        doc["powerMultiplier"] = calibrationData.powerMultiplier;
    }
}

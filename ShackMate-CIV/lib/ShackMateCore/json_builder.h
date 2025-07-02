#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "device_state.h"

// -------------------------------------------------------------------------
// JSON Response Builder
// -------------------------------------------------------------------------
class JsonBuilder
{
private:
    static constexpr size_t STATE_JSON_SIZE = 256;
    static constexpr size_t STATUS_JSON_SIZE = 512;
    static constexpr size_t RESPONSE_JSON_SIZE = 128;

public:
    // Build state response (relay states, labels, device name)
    static String buildStateResponse();

    // Build full status response (includes sensors, uptime, connection info)
    static String buildStatusResponse();

    // Build simple response messages
    static String buildInfoResponse(const String &message);
    static String buildErrorResponse(const String &message);
    static String buildLabelResponse(int outlet, const String &text);
    static String buildDeviceNameResponse(const String &name);
    static String buildPongResponse(unsigned long timestamp);

    // Build sensor data response
    static String buildSensorDataResponse(float lux, float amps, float volts, float watts);

private:
    static void addConnectionInfo(JsonDocument &doc);
    static void addSystemInfo(JsonDocument &doc);
    static void addSensorInfo(JsonDocument &doc);
};

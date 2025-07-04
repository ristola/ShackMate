#include "device_state.h"
#include "logger.h"
#include <esp_system.h>

// Static member definitions
WebSocketMetrics DeviceState::webSocketMetrics;
ConnectionState DeviceState::connectionState;
unsigned long DeviceState::bootTime = 0;
uint32_t DeviceState::rebootCounter = 0;

void DeviceState::init()
{
    bootTime = millis();

    // Load reboot counter from preferences
    Preferences prefs;
    prefs.begin("system", true);
    rebootCounter = prefs.getUInt("rebootCount", 0);
    prefs.end();

    // Increment and save reboot counter
    incrementRebootCounter();

    Logger::info("CI-V Device state initialized");
}

void DeviceState::incrementRebootCounter()
{
    rebootCounter++;
    Preferences prefs;
    prefs.begin("system", false);
    prefs.putUInt("rebootCount", rebootCounter);
    prefs.end();
}

void DeviceState::updateWebSocketMetrics(const WebSocketMetrics &metrics)
{
    webSocketMetrics = metrics;
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
    String info = "=== CI-V Controller System Information ===\n";
    info += "Version: " + String(VERSION) + "\n";
    info += "Uptime: " + getUptime() + "\n";
    info += "Reboot Count: " + String(rebootCounter) + "\n";
    info += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    info += "CPU Freq: " + String(ESP.getCpuFreqMHz()) + "MHz\n";
    info += "Flash Size: " + String(ESP.getFlashChipSize()) + " bytes\n";
    return info;
}

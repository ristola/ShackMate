#include "system_utils.h"
#include "config.h"
#include "device_state.h"
#include <SPIFFS.h>
#include <esp_system.h>

String SystemUtils::getUptime()
{
    unsigned long now = millis();
    unsigned long secs = (now - DeviceState::getBootTime()) / 1000;
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

String SystemUtils::getChipID()
{
    uint64_t chipid = ESP.getEfuseMac();
    return String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
}

int SystemUtils::getChipRevision()
{
    return ESP.getChipRevision();
}

uint32_t SystemUtils::getFlashSize()
{
    return ESP.getFlashChipSize();
}

uint32_t SystemUtils::getPsramSize()
{
    return ESP.getPsramSize();
}

int SystemUtils::getCpuFrequency()
{
    return ESP.getCpuFreqMHz();
}

uint32_t SystemUtils::getFreeHeap()
{
    return ESP.getFreeHeap();
}

uint32_t SystemUtils::getTotalHeap()
{
    return ESP.getHeapSize();
}

uint32_t SystemUtils::getSketchSize()
{
    return ESP.getSketchSize();
}

uint32_t SystemUtils::getFreeSketchSpace()
{
    return ESP.getFreeSketchSpace();
}

float SystemUtils::readInternalTemperature()
{
    // Note: This is a placeholder - ESP32 doesn't have a built-in temperature sensor
    // You would need an external sensor or use a different method
    return 25.0f; // Default room temperature
}

String SystemUtils::loadFile(const char *path)
{
    File file = SPIFFS.open(path, "r");
    if (!file)
    {
        Serial.println("Failed to open file: " + String(path));
        return "";
    }

    String content = file.readString();
    file.close();
    return content;
}

String SystemUtils::processTemplate(String tmpl)
{
    // Replace template variables
    tmpl.replace("%DEVICE_NAME%", String(DeviceState::getDeviceConfig().deviceName));
    tmpl.replace("%PROJECT_NAME%", NAME);
    tmpl.replace("%VERSION%", VERSION);
    tmpl.replace("%DEVICE_ID%", String(DeviceState::getDeviceConfig().deviceId));
    tmpl.replace("%CIV_ADDRESS%", DeviceState::getDeviceConfig().civAddress);
    tmpl.replace("%UPTIME%", getUptime());
    tmpl.replace("%FREE_HEAP%", String(getFreeHeap()));
    tmpl.replace("%CHIP_ID%", getChipID());

    return tmpl;
}

bool SystemUtils::isLowMemory()
{
    return getFreeHeap() < CRITICAL_HEAP_THRESHOLD;
}

void SystemUtils::printMemoryInfo()
{
    Serial.println("=== Memory Information ===");
    Serial.println("Free Heap: " + String(getFreeHeap()) + " bytes");
    Serial.println("Total Heap: " + String(getTotalHeap()) + " bytes");
    Serial.println("Free Sketch Space: " + String(getFreeSketchSpace()) + " bytes");
    Serial.println("Sketch Size: " + String(getSketchSize()) + " bytes");
    Serial.println("Flash Size: " + String(getFlashSize()) + " bytes");
    if (getPsramSize() > 0)
    {
        Serial.println("PSRAM Size: " + String(getPsramSize()) + " bytes");
    }
    Serial.println("==========================");
}

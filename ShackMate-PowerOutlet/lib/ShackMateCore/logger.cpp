#include "logger.h"
#include "config.h"
#include <esp_system.h>

LogLevel Logger::currentLevel = LogLevel::INFO;
bool Logger::serialEnabled = true;
bool Logger::webSocketEnabled = false;

void Logger::init(LogLevel level)
{
    currentLevel = level;
    serialEnabled = true;
    webSocketEnabled = false;
}

void Logger::setLevel(LogLevel level)
{
    currentLevel = level;
}

void Logger::enableSerial(bool enable)
{
    serialEnabled = enable;
}

void Logger::enableWebSocket(bool enable)
{
    webSocketEnabled = enable;
}

void Logger::debug(const String &message)
{
    log(LogLevel::DEBUG, message);
}

void Logger::info(const String &message)
{
    log(LogLevel::INFO, message);
}

void Logger::warning(const String &message)
{
    log(LogLevel::WARNING, message);
}

void Logger::error(const String &message)
{
    log(LogLevel::ERROR, message);
}

void Logger::critical(const String &message)
{
    log(LogLevel::CRITICAL, message);
}

void Logger::checkHeapMemory()
{
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < CRITICAL_HEAP_THRESHOLD)
    {
        critical("Very low heap memory: " + String(freeHeap) + " bytes");
    }
}

void Logger::log(LogLevel level, const String &message)
{
    if (level < currentLevel)
        return;

    checkHeapMemory();

    String logMessage = "[" + levelToString(level) + "] " + message;

    if (serialEnabled)
    {
        Serial.println(logMessage);
    }

    // TODO: Add WebSocket logging when needed
    // if (webSocketEnabled && wsConnected) {
    //     // Send to WebSocket clients
    // }
}

String Logger::levelToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARNING:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::CRITICAL:
        return "CRITICAL";
    default:
        return "UNKNOWN";
    }
}

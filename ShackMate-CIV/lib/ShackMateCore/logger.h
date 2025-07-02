#pragma once

#include <Arduino.h>

// -------------------------------------------------------------------------
// Logging System
// -------------------------------------------------------------------------
enum class LogLevel
{
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    CRITICAL = 4
};

class Logger
{
private:
    static LogLevel currentLevel;
    static bool serialEnabled;
    static bool webSocketEnabled;

public:
    static void init(LogLevel level = LogLevel::INFO);
    static void setLevel(LogLevel level);
    static void enableSerial(bool enable);
    static void enableWebSocket(bool enable);

    static void debug(const String &message);
    static void info(const String &message);
    static void warning(const String &message);
    static void error(const String &message);
    static void critical(const String &message);

    static void checkHeapMemory();

private:
    static void log(LogLevel level, const String &message);
    static String levelToString(LogLevel level);
};

// Convenience macros
#define LOG_DEBUG(msg) Logger::debug(msg)
#define LOG_INFO(msg) Logger::info(msg)
#define LOG_WARNING(msg) Logger::warning(msg)
#define LOG_ERROR(msg) Logger::error(msg)
#define LOG_CRITICAL(msg) Logger::critical(msg)

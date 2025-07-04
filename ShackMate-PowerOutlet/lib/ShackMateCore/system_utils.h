#pragma once

#include <Arduino.h>

// -------------------------------------------------------------------------
// System Utilities Module
// -------------------------------------------------------------------------

class SystemUtils
{
public:
    // System information functions
    static String getUptime();
    static String getChipID();
    static int getChipRevision();
    static uint32_t getFlashSize();
    static uint32_t getPsramSize();
    static int getCpuFrequency();
    static uint32_t getFreeHeap();
    static uint32_t getTotalHeap();
    static uint32_t getSketchSize();
    static uint32_t getFreeSketchSpace();
    static float readInternalTemperature();

    // File system utilities
    static String loadFile(const char *path);
    static String processTemplate(String tmpl);

    // Memory monitoring
    static bool isLowMemory();
    static void printMemoryInfo();
};

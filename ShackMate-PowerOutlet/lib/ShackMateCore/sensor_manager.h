#pragma once

#include <Arduino.h>
#include <HLW8012.h>
#include "config.h"

// -------------------------------------------------------------------------
// Sensor Management Module
// -------------------------------------------------------------------------

class SensorManager
{
private:
    static HLW8012 *hlw8012Instance;
    static float voltageCalibrationFactor;
    static float currentCalibrationFactor;
    static bool voltageCalibrated;
    static bool currentCalibrated;

    // Last sensor values for change detection
    static float lastVoltage;
    static float lastCurrent;
    static float lastPower;
    static float lastLux;

public:
    // Initialization
    static void init(HLW8012 *hlwInstance);
    static void loadCalibrationFromPreferences();

    // Sensor reading functions
    static float getValidatedCurrent();
    static float getValidatedPower();
    static float getValidatedVoltage();
    static float getLuxReading();

    // Calibration functions
    static void setVoltageCalibration(float factor);
    static void setCurrentCalibration(float factor);
    static float getVoltageCalibrationFactor() { return voltageCalibrationFactor; }
    static float getCurrentCalibrationFactor() { return currentCalibrationFactor; }
    static bool isVoltageCalibrated() { return voltageCalibrated; }
    static bool isCurrentCalibrated() { return currentCalibrated; }

    // Change detection
    static bool hasSignificantSensorChange();
    static String getSensorChangeDescription();
    static void updateLastSensorValues();

    // Interrupt handlers
    static void attachInterrupts();
    static void IRAM_ATTR hlw8012CfInterrupt();
    static void IRAM_ATTR hlw8012Cf1Interrupt();
};

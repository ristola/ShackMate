#include "sensor_manager.h"
#include "logger.h"
#include <Preferences.h>

// Static member definitions
HLW8012 *SensorManager::hlw8012Instance = nullptr;
float SensorManager::voltageCalibrationFactor = 1.0f;
float SensorManager::currentCalibrationFactor = 1.0f;
bool SensorManager::voltageCalibrated = false;
bool SensorManager::currentCalibrated = false;
float SensorManager::lastVoltage = 0.0f;
float SensorManager::lastCurrent = 0.0f;
float SensorManager::lastPower = 0.0f;
float SensorManager::lastLux = 0.0f;

void SensorManager::init(HLW8012 *hlwInstance)
{
    hlw8012Instance = hlwInstance;
    loadCalibrationFromPreferences();
    LOG_INFO("Sensor manager initialized");
}

void SensorManager::loadCalibrationFromPreferences()
{
    Preferences prefs;
    prefs.begin("calibration", true);

    voltageCalibrationFactor = prefs.getFloat("voltageFactor", 1.0f);
    voltageCalibrated = prefs.getBool("voltageCalibrated", false);

    currentCalibrationFactor = prefs.getFloat("currentFactor", 1.0f);
    currentCalibrated = prefs.getBool("currentCalibrated", false);

    prefs.end();

    if (voltageCalibrated)
    {
        LOG_INFO("Loaded voltage calibration factor: " + String(voltageCalibrationFactor, 4));
    }

    if (currentCalibrated)
    {
        LOG_INFO("Loaded current calibration factor: " + String(currentCalibrationFactor, 4));
    }
}

float SensorManager::getValidatedCurrent()
{
    if (!hlw8012Instance)
        return 0.0f;

    float rawCurrent = hlw8012Instance->getCurrent();
    float calibratedCurrent = rawCurrent * currentCalibrationFactor;

    // Negative current doesn't make physical sense
    if (calibratedCurrent < 0.0f)
    {
        return 0.0f;
    }

    // Cap at reasonable maximum for household outlet (20A)
    const float MAX_REASONABLE_CURRENT = 20.0f;
    if (calibratedCurrent > MAX_REASONABLE_CURRENT)
    {
        LOG_WARNING("Excessive current reading: " + String(calibratedCurrent, 3) + "A - capping at " + String(MAX_REASONABLE_CURRENT, 1) + "A");
        return MAX_REASONABLE_CURRENT;
    }

    return calibratedCurrent;
}

float SensorManager::getValidatedPower()
{
    if (!hlw8012Instance)
        return 0.0f;

    float current = getValidatedCurrent();
    float rawPower = hlw8012Instance->getActivePower();

    // Validation thresholds
    const float MIN_CURRENT_THRESHOLD = 0.05f;  // 50mA minimum for valid power
    const float MAX_REASONABLE_POWER = 2000.0f; // 2000W maximum for this device

    // If current is below threshold, power should be zero
    if (current < MIN_CURRENT_THRESHOLD)
    {
        return 0.0f;
    }

    // Filter out spurious high power readings
    if (rawPower > MAX_REASONABLE_POWER)
    {
        LOG_WARNING("Spurious power reading: " + String(rawPower, 1) + "W with " + String(current, 3) + "A - setting to 0W");
        return 0.0f;
    }

    // Basic power factor validation (power shouldn't exceed V*I significantly)
    float voltage = getValidatedVoltage();
    float apparentPower = voltage * current;

    // Power factor validation - real power shouldn't exceed apparent power
    if (rawPower > apparentPower * 1.1f)
    { // Allow 10% margin for measurement error
        LOG_WARNING("Power " + String(rawPower, 1) + "W exceeds apparent power " + String(apparentPower, 1) + "W - setting to 0W");
        return 0.0f;
    }

    return rawPower;
}

float SensorManager::getValidatedVoltage()
{
    if (!hlw8012Instance)
        return 0.0f;

    float rawVoltage = hlw8012Instance->getVoltage();
    return rawVoltage * voltageCalibrationFactor;
}

float SensorManager::getLuxReading()
{
    return analogRead(PIN_LUX_ADC) * (3.3f / 4095.0f);
}

void SensorManager::setVoltageCalibration(float factor)
{
    voltageCalibrationFactor = factor;
    voltageCalibrated = true;

    Preferences prefs;
    prefs.begin("calibration", false);
    prefs.putFloat("voltageFactor", factor);
    prefs.putBool("voltageCalibrated", true);
    prefs.end();

    LOG_INFO("Voltage calibration factor set to: " + String(factor, 4));
}

void SensorManager::setCurrentCalibration(float factor)
{
    currentCalibrationFactor = factor;
    currentCalibrated = true;

    Preferences prefs;
    prefs.begin("calibration", false);
    prefs.putFloat("currentFactor", factor);
    prefs.putBool("currentCalibrated", true);
    prefs.end();

    LOG_INFO("Current calibration factor set to: " + String(factor, 4));
}

bool SensorManager::hasSignificantSensorChange()
{
    float voltage = getValidatedVoltage();
    float current = getValidatedCurrent();
    float power = getValidatedPower();
    float lux = getLuxReading();

    // Check for significant changes using thresholds
    return (abs(voltage - lastVoltage) >= VOLTAGE_CHANGE_THRESHOLD) ||
           (abs(current - lastCurrent) >= CURRENT_CHANGE_THRESHOLD) ||
           (abs(power - lastPower) >= POWER_CHANGE_THRESHOLD) ||
           (abs(lux - lastLux) >= LUX_CHANGE_THRESHOLD);
}

String SensorManager::getSensorChangeDescription()
{
    float voltage = getValidatedVoltage();
    float current = getValidatedCurrent();
    float power = getValidatedPower();
    float lux = getLuxReading();

    String description = "";

    if (abs(voltage - lastVoltage) >= VOLTAGE_CHANGE_THRESHOLD)
    {
        description += "Voltage: " + String(lastVoltage, 1) + "V → " + String(voltage, 1) + "V ";
    }

    if (abs(current - lastCurrent) >= CURRENT_CHANGE_THRESHOLD)
    {
        description += "Current: " + String(lastCurrent, 3) + "A → " + String(current, 3) + "A ";
    }

    if (abs(power - lastPower) >= POWER_CHANGE_THRESHOLD)
    {
        description += "Power: " + String(lastPower, 1) + "W → " + String(power, 1) + "W ";
    }

    if (abs(lux - lastLux) >= LUX_CHANGE_THRESHOLD)
    {
        description += "Lux: " + String(lastLux, 1) + " → " + String(lux, 1) + " ";
    }

    description.trim();
    return description;
}

void SensorManager::updateLastSensorValues()
{
    lastVoltage = getValidatedVoltage();
    lastCurrent = getValidatedCurrent();
    lastPower = getValidatedPower();
    lastLux = getLuxReading();
}

void SensorManager::attachInterrupts()
{
    if (hlw8012Instance)
    {
        attachInterrupt(digitalPinToInterrupt(PIN_HLW_CF1), hlw8012Cf1Interrupt, FALLING);
        attachInterrupt(digitalPinToInterrupt(PIN_HLW_CF), hlw8012CfInterrupt, FALLING);
        LOG_INFO("HLW8012 interrupts attached");
    }
}

void IRAM_ATTR SensorManager::hlw8012CfInterrupt()
{
    if (hlw8012Instance)
    {
        hlw8012Instance->cf_interrupt();
    }
}

void IRAM_ATTR SensorManager::hlw8012Cf1Interrupt()
{
    if (hlw8012Instance)
    {
        hlw8012Instance->cf1_interrupt();
    }
}

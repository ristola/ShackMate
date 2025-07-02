#pragma once

#include <Arduino.h>
#include "config.h"
#include "logger.h"

// -------------------------------------------------------------------------
// Hardware Control Module
// -------------------------------------------------------------------------
class HardwareController
{
private:
    static bool statusLedState;
    static unsigned long lastStatusLedToggle;
    static hw_timer_t *ledTimer;
    static volatile bool timerTriggered;

    // Button debouncing
    static unsigned long lastButton1Time;
    static unsigned long lastButton2Time;
    static bool lastButton1State;
    static bool lastButton2State;
    static bool button1StateStable;
    static bool button2StateStable;

public:
    static void init();
    static void update();

    // LED Control
    static void setStatusLED(bool on);
    static void startStatusLedBlinking();
    static void stopStatusLedBlinking();
    static void testStatusLED();
    static void performLEDHardwareTest();

    // Relay Control
    static void setRelay(int relayNum, bool state);
    static void updateRelayHardware();

    // Button Handling
    static bool checkButton1Pressed();
    static bool checkButton2Pressed();

    // Sensor Reading
    static float readLuxSensor();

    // Captive Portal Mode
    static void setCaptivePortalMode(bool active);
    static bool isCaptivePortalActive();

private:
    static bool captivePortalActive;
    static void IRAM_ATTR onLedTimer();
    static void setupLedTimer();
    static void updateStatusLedLogic();
    static bool debounceButton(int pin, unsigned long &lastTime, bool &lastState, bool &stableState);
};

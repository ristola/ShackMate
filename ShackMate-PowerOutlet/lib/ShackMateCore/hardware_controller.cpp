#include "hardware_controller.h"
#include "device_state.h"

// Static member definitions
bool HardwareController::statusLedState = false;
unsigned long HardwareController::lastStatusLedToggle = 0;
hw_timer_t *HardwareController::ledTimer = nullptr;
volatile bool HardwareController::timerTriggered = false;
bool HardwareController::captivePortalActive = false;

unsigned long HardwareController::lastButton1Time = 0;
unsigned long HardwareController::lastButton2Time = 0;
bool HardwareController::lastButton1State = false;
bool HardwareController::lastButton2State = false;
bool HardwareController::button1StateStable = false;
bool HardwareController::button2StateStable = false;

void HardwareController::init()
{
    LOG_INFO("Initializing hardware controller");

    // Configure relay GPIOs
    pinMode(PIN_RELAY1, OUTPUT);
    pinMode(PIN_RELAY2, OUTPUT);
    pinMode(PIN_RELAY1_LED, OUTPUT);
    pinMode(PIN_RELAY2_LED, OUTPUT);

    // Configure status LED
    pinMode(PIN_STATUS_LED, OUTPUT);
    setStatusLED(true); // Turn on initially

    // Test the status LED during startup
    LOG_INFO("Testing Status LED - 3 blinks");
    for (int i = 0; i < 3; i++)
    {
        setStatusLED(false);
        delay(200);
        setStatusLED(true);
        delay(200);
    }

    // Configure sensor pins
    pinMode(PIN_LUX_ADC, INPUT);
    analogSetAttenuation(ADC_11db);
    analogSetPinAttenuation(PIN_LUX_ADC, ADC_11db);
    analogReadResolution(12);

    // Configure buttons
    pinMode(PIN_BUTTON1, INPUT_PULLDOWN);
    pinMode(PIN_BUTTON2, INPUT_PULLDOWN);

    // Initialize button states
    lastButton1State = digitalRead(PIN_BUTTON1) == HIGH;
    lastButton2State = digitalRead(PIN_BUTTON2) == HIGH;
    button1StateStable = lastButton1State;
    button2StateStable = lastButton2State;
    lastButton1Time = millis();
    lastButton2Time = millis();

    // Setup LED timer
    setupLedTimer();

    // Update relay hardware to match stored state
    updateRelayHardware();

    LOG_INFO("Hardware controller initialized successfully");
}

void HardwareController::update()
{
    updateStatusLedLogic();
}

void HardwareController::setStatusLED(bool on)
{
    statusLedState = on;
    digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH); // Inverted logic
}

void HardwareController::startStatusLedBlinking()
{
    if (ledTimer)
    {
        timerAlarmEnable(ledTimer);
        LOG_DEBUG("Status LED blinking started");
    }
}

void HardwareController::stopStatusLedBlinking()
{
    if (ledTimer)
    {
        timerAlarmDisable(ledTimer);
        LOG_DEBUG("Status LED blinking stopped");
    }
}

void HardwareController::testStatusLED()
{
    static bool testState = false;
    testState = !testState;
    setStatusLED(testState);
    LOG_INFO("Status LED test - toggled to " + String(testState ? "ON" : "OFF"));
}

void HardwareController::performLEDHardwareTest()
{
    LOG_INFO("Starting comprehensive LED hardware test");

    // Test OFF state
    setStatusLED(false);
    LOG_INFO("Test 1: LED OFF");
    delay(1000);

    // Test ON state
    setStatusLED(true);
    LOG_INFO("Test 2: LED ON");
    delay(1000);

    // Rapid blinking test
    LOG_INFO("Test 3: Rapid blinking for 5 seconds");
    for (int i = 0; i < 10; i++)
    {
        setStatusLED(false);
        delay(250);
        setStatusLED(true);
        delay(250);
        LOG_DEBUG("Blink " + String(i + 1));
    }

    // Return to normal state
    setStatusLED(true);
    LOG_INFO("LED hardware test complete");
}

void HardwareController::setRelay(int relayNum, bool state)
{
    LOG_INFO("HardwareController::setRelay called - Relay " + String(relayNum) + " = " + String(state ? "ON" : "OFF"));

    // Get current relay states
    const auto &currentRelayState = DeviceState::getRelayState();
    bool relay1State = currentRelayState.relay1;
    bool relay2State = currentRelayState.relay2;

    // Update the specific relay state
    if (relayNum == 1)
    {
        relay1State = state;
        LOG_DEBUG("Setting relay1 to " + String(state ? "ON" : "OFF"));
    }
    else if (relayNum == 2)
    {
        relay2State = state;
        LOG_DEBUG("Setting relay2 to " + String(state ? "ON" : "OFF"));
    }

    // Use the proper DeviceState method to update both states
    DeviceState::setRelayState(relay1State, relay2State);
    LOG_DEBUG("DeviceState updated via setRelayState() method");

    // Update hardware GPIOs
    updateRelayHardware();
    LOG_INFO("HardwareController::setRelay completed for relay " + String(relayNum));
}

void HardwareController::updateRelayHardware()
{
    const auto &relayState = DeviceState::getRelayState();

    LOG_INFO("updateRelayHardware: Setting GPIO pins - Relay1=" + String(relayState.relay1 ? "HIGH" : "LOW") + " (pin " + String(PIN_RELAY1) + "), Relay2=" + String(relayState.relay2 ? "HIGH" : "LOW") + " (pin " + String(PIN_RELAY2) + ")");

    // More detailed GPIO logging
    int relay1Value = relayState.relay1 ? HIGH : LOW;
    int relay1LedValue = relayState.relay1 ? LOW : HIGH; // Inverted
    int relay2Value = relayState.relay2 ? HIGH : LOW;
    int relay2LedValue = relayState.relay2 ? LOW : HIGH; // Inverted

    LOG_INFO("updateRelayHardware: Writing GPIO values - PIN_RELAY1(" + String(PIN_RELAY1) + ")=" + String(relay1Value) + ", PIN_RELAY1_LED(" + String(PIN_RELAY1_LED) + ")=" + String(relay1LedValue) + ", PIN_RELAY2(" + String(PIN_RELAY2) + ")=" + String(relay2Value) + ", PIN_RELAY2_LED(" + String(PIN_RELAY2_LED) + ")=" + String(relay2LedValue));

    digitalWrite(PIN_RELAY1, relay1Value);
    digitalWrite(PIN_RELAY1_LED, relay1LedValue);
    digitalWrite(PIN_RELAY2, relay2Value);
    digitalWrite(PIN_RELAY2_LED, relay2LedValue);

    LOG_INFO("updateRelayHardware: GPIO calls completed - Physical pins should now be updated");
}

bool HardwareController::checkButton1Pressed()
{
    return debounceButton(PIN_BUTTON1, lastButton1Time, lastButton1State, button1StateStable);
}

bool HardwareController::checkButton2Pressed()
{
    return debounceButton(PIN_BUTTON2, lastButton2Time, lastButton2State, button2StateStable);
}

float HardwareController::readLuxSensor()
{
    int rawValue = analogRead(PIN_LUX_ADC);
    return rawValue * (3.3f / 4095.0f);
}

void IRAM_ATTR HardwareController::onLedTimer()
{
    timerTriggered = true;
}

void HardwareController::setupLedTimer()
{
    ledTimer = timerBegin(0, 80, true); // Timer 0, prescaler 80 (1MHz), count up
    timerAttachInterrupt(ledTimer, &onLedTimer, true);
    timerAlarmWrite(ledTimer, STATUS_LED_BLINK_INTERVAL_MS * 1000, true); // Convert to microseconds
    LOG_DEBUG("LED timer initialized");
}

void HardwareController::updateStatusLedLogic()
{
    unsigned long currentTime = millis();

    if (captivePortalActive)
    {
        // Handle timer-based LED blinking for captive portal mode
        if (timerTriggered)
        {
            timerTriggered = false; // Reset the flag
            statusLedState = !statusLedState;
            setStatusLED(statusLedState);
            lastStatusLedToggle = currentTime;

            // Debug output occasionally
            static int toggleCount = 0;
            toggleCount++;
            if (toggleCount % 8 == 0) // Every 2 seconds (8 toggles * 250ms)
            {
                LOG_DEBUG("Status LED blinking in captive portal mode - toggle " + String(toggleCount));
            }
        }
    }
    else
    {
        // LED ON when code is running normally
        setStatusLED(true);

        // Debug output occasionally when not in captive portal mode
        static unsigned long lastNormalDebug = 0;
        if (currentTime - lastNormalDebug >= 10000) // Every 10 seconds
        {
            LOG_DEBUG("Status LED ON (normal mode)");
            lastNormalDebug = currentTime;
        }
    }
}

bool HardwareController::debounceButton(int pin, unsigned long &lastTime, bool &lastState, bool &stableState)
{
    bool currentReading = digitalRead(pin) == HIGH;
    unsigned long currentTime = millis();

    if (currentReading != lastState)
    {
        lastTime = currentTime;
        lastState = currentReading;
    }

    if ((currentTime - lastTime) > DEBOUNCE_DELAY_MS)
    {
        if (currentReading != stableState)
        {
            stableState = currentReading;
            if (stableState)
            { // Button pressed (rising edge)
                return true;
            }
        }
    }

    return false;
}

void HardwareController::setCaptivePortalMode(bool active)
{
    captivePortalActive = active;
    if (active)
    {
        LOG_INFO("Captive portal mode activated - LED will blink");
        startStatusLedBlinking();
    }
    else
    {
        LOG_INFO("Captive portal mode deactivated - LED will be solid");
        stopStatusLedBlinking();
        setStatusLED(true); // Ensure LED is on when not in captive portal mode
    }
}

bool HardwareController::isCaptivePortalActive()
{
    return captivePortalActive;
}

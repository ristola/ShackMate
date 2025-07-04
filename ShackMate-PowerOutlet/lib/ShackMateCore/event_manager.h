#pragma once

#include <Arduino.h>

// -------------------------------------------------------------------------
// Event Manager Module
// -------------------------------------------------------------------------

// Event types for real-time webpage updates
enum WebUpdateEventType
{
    WEB_EVENT_SENSOR_UPDATE,
    WEB_EVENT_RELAY_STATE_CHANGE,
    WEB_EVENT_CONNECTION_STATUS_CHANGE,
    WEB_EVENT_CIV_MESSAGE,
    WEB_EVENT_SYSTEM_STATUS,
    WEB_EVENT_CALIBRATION_CHANGE
};

// Event queue structure for efficient web updates
struct WebUpdateEvent
{
    WebUpdateEventType type;
    uint32_t timestamp;
    bool hasData;
    String data;
};

class EventManager
{
private:
    static const uint8_t EVENT_QUEUE_SIZE = 10;
    static WebUpdateEvent eventQueue[EVENT_QUEUE_SIZE];
    static volatile uint8_t queueHead;
    static volatile uint8_t queueTail;
    static volatile bool queueOverflow;

    // Timer-related variables
    static hw_timer_t *sensorUpdateTimer;
    static hw_timer_t *systemStatusTimer;
    static hw_timer_t *ledTimer;

    // Timer interrupt flags
    static volatile bool sensorUpdateTriggered;
    static volatile bool systemStatusUpdateTriggered;
    static volatile bool timerTriggered;
    static volatile uint32_t timerInterruptCount;

public:
    // Initialization
    static void init();

    // Timer management
    static void initTimers();
    static void startSensorUpdateTimer();
    static void startSystemStatusTimer();
    static void initLedTimer();
    static void startLedBlinking();
    static void stopLedBlinking();

    // Event queue management
    static void queueEvent(WebUpdateEventType type, const String &data = "");
    static bool getNextEvent(WebUpdateEvent *event);
    static bool hasEvents();
    static void processEvents();

    // Event triggers
    static void triggerRelayStateChange();
    static void triggerCivMessage(const String &messageInfo = "");
    static void triggerCalibrationChange(const String &calibrationInfo = "");
    static void triggerSensorUpdate();
    static void triggerSystemStatus();

    // Timer interrupt handlers
    static void IRAM_ATTR onLedTimer();
    static void IRAM_ATTR onSensorUpdateTimer();
    static void IRAM_ATTR onSystemStatusTimer();

    // Status checking
    static bool isSensorUpdateTriggered() { return sensorUpdateTriggered; }
    static bool isSystemStatusTriggered() { return systemStatusUpdateTriggered; }
    static bool isLedTimerTriggered() { return timerTriggered; }
    static void clearSensorUpdateFlag() { sensorUpdateTriggered = false; }
    static void clearSystemStatusFlag() { systemStatusUpdateTriggered = false; }
    static void clearLedTimerFlag() { timerTriggered = false; }
};

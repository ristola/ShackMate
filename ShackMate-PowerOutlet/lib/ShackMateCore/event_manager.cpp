#include "event_manager.h"
#include "config.h"
#include "logger.h"
#include "network_manager.h"
#include "json_builder.h"
#include "sensor_manager.h"
#include "device_state.h"

// Static member definitions
WebUpdateEvent EventManager::eventQueue[EVENT_QUEUE_SIZE];
volatile uint8_t EventManager::queueHead = 0;
volatile uint8_t EventManager::queueTail = 0;
volatile bool EventManager::queueOverflow = false;

hw_timer_t *EventManager::sensorUpdateTimer = nullptr;
hw_timer_t *EventManager::systemStatusTimer = nullptr;
hw_timer_t *EventManager::ledTimer = nullptr;

volatile bool EventManager::sensorUpdateTriggered = false;
volatile bool EventManager::systemStatusUpdateTriggered = false;
volatile bool EventManager::timerTriggered = false;
volatile uint32_t EventManager::timerInterruptCount = 0;

void EventManager::init()
{
    initTimers();
    LOG_INFO("Event manager initialized");
}

void EventManager::initTimers()
{
    // Initialize sensor update timer (2 seconds)
    sensorUpdateTimer = timerBegin(1, 80, true); // Timer 1, prescaler 80 (1MHz), count up
    timerAttachInterrupt(sensorUpdateTimer, &onSensorUpdateTimer, true);
    timerAlarmWrite(sensorUpdateTimer, 2000000, true); // 2 seconds (2,000,000 microseconds)
    timerAlarmEnable(sensorUpdateTimer);

    // Initialize system status timer (30 seconds)
    systemStatusTimer = timerBegin(2, 80, true); // Timer 2, prescaler 80 (1MHz), count up
    timerAttachInterrupt(systemStatusTimer, &onSystemStatusTimer, true);
    timerAlarmWrite(systemStatusTimer, 30000000, true); // 30 seconds (30,000,000 microseconds)
    timerAlarmEnable(systemStatusTimer);

    LOG_INFO("Event-driven timers initialized (sensor: 2s, status: 30s)");
}

void EventManager::initLedTimer()
{
    ledTimer = timerBegin(0, 80, true);                // Timer 0, prescaler 80 (1MHz), count up
    timerAttachInterrupt(ledTimer, &onLedTimer, true); // Edge interrupt
    timerAlarmWrite(ledTimer, 250000, true);           // 250ms interval (250,000 microseconds)
    LOG_INFO("LED timer initialized for captive portal blinking");
}

void EventManager::startLedBlinking()
{
    if (ledTimer)
    {
        timerAlarmEnable(ledTimer);
        LOG_INFO("LED blinking timer started");
    }
}

void EventManager::stopLedBlinking()
{
    if (ledTimer)
    {
        timerAlarmDisable(ledTimer);
        LOG_INFO("LED blinking timer stopped");
    }
}

void EventManager::queueEvent(WebUpdateEventType type, const String &data)
{
    uint8_t nextHead = (queueHead + 1) % EVENT_QUEUE_SIZE;

    if (nextHead == queueTail)
    {
        // Queue is full - drop oldest event
        queueTail = (queueTail + 1) % EVENT_QUEUE_SIZE;
        queueOverflow = true;
    }

    eventQueue[queueHead].type = type;
    eventQueue[queueHead].timestamp = millis();
    eventQueue[queueHead].hasData = !data.isEmpty();
    eventQueue[queueHead].data = data;

    queueHead = nextHead;
}

bool EventManager::getNextEvent(WebUpdateEvent *event)
{
    if (queueTail == queueHead)
    {
        return false; // Queue is empty
    }

    *event = eventQueue[queueTail];
    queueTail = (queueTail + 1) % EVENT_QUEUE_SIZE;

    return true;
}

bool EventManager::hasEvents()
{
    return queueTail != queueHead;
}

void EventManager::processEvents()
{
    WebUpdateEvent event;

    while (getNextEvent(&event))
    {
        String jsonMessage;

        switch (event.type)
        {
        case WEB_EVENT_SENSOR_UPDATE:
            // Send current sensor readings
            jsonMessage = JsonBuilder::buildStatusResponse();
            break;

        case WEB_EVENT_RELAY_STATE_CHANGE:
            // Send current relay states and device info
            jsonMessage = JsonBuilder::buildStateResponse();
            break;

        case WEB_EVENT_SYSTEM_STATUS:
            // Send comprehensive status update
            jsonMessage = JsonBuilder::buildStatusResponse();
            break;

        case WEB_EVENT_CIV_MESSAGE:
            // Send CI-V message as info response
            jsonMessage = JsonBuilder::buildInfoResponse("CIV: " + event.data);
            break;

        case WEB_EVENT_CALIBRATION_CHANGE:
            // Send calibration change as info response
            jsonMessage = JsonBuilder::buildInfoResponse("Calibration: " + event.data);
            break;

        default:
            continue; // Skip unknown event types
        }

        // Broadcast to all connected WebSocket clients
        if (!jsonMessage.isEmpty())
        {
            NetworkManager::broadcastToWebClients(jsonMessage);
        }
    }

    if (queueOverflow)
    {
        LOG_WARNING("Event queue overflow detected - some events were dropped");
        queueOverflow = false;
    }
}

void EventManager::triggerRelayStateChange()
{
    queueEvent(WEB_EVENT_RELAY_STATE_CHANGE);
}

void EventManager::triggerCivMessage(const String &messageInfo)
{
    queueEvent(WEB_EVENT_CIV_MESSAGE, messageInfo);
}

void EventManager::triggerCalibrationChange(const String &calibrationInfo)
{
    queueEvent(WEB_EVENT_CALIBRATION_CHANGE, calibrationInfo);
}

void EventManager::triggerSensorUpdate()
{
    queueEvent(WEB_EVENT_SENSOR_UPDATE);
}

void EventManager::triggerSystemStatus()
{
    queueEvent(WEB_EVENT_SYSTEM_STATUS);
}

// Timer interrupt handlers
void IRAM_ATTR EventManager::onLedTimer()
{
    timerTriggered = true;
    timerInterruptCount++;
}

void IRAM_ATTR EventManager::onSensorUpdateTimer()
{
    sensorUpdateTriggered = true;
}

void IRAM_ATTR EventManager::onSystemStatusTimer()
{
    systemStatusUpdateTriggered = true;
}

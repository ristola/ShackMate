#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

// -------------------------------------------------------------------------
// CI-V Controller Device State Management
// -------------------------------------------------------------------------

// WebSocket metrics for connection monitoring
struct WebSocketMetrics
{
    uint32_t disconnects = 0;
    uint32_t reconnects = 0;
    uint32_t reconnect_attempts = 0;
    uint32_t rate_limited_messages = 0;
    uint32_t messages_sent = 0;
    uint32_t messages_rate_limited = 0;
    uint32_t total_disconnects = 0;
    uint32_t ping_rtt = 0;
    uint8_t connection_quality = 0;
    bool ping_pending = false;
    unsigned long last_ping_sent = 0;
    unsigned long last_ping_received = 0;
    unsigned long last_pong_received = 0;
};

// WebSocket connection state
struct ConnectionState
{
    bool wsClientConnected = false;
    bool wsClientEverConnected = false;
    String connectedServerIP = "";
    uint16_t connectedServerPort = 0;
    unsigned long lastWebSocketActivity = 0;
};

class DeviceState
{
private:
    static WebSocketMetrics webSocketMetrics;
    static ConnectionState connectionState;
    static unsigned long bootTime;
    static uint32_t rebootCounter;

public:
    // Initialization
    static void init();

    // WebSocket metrics
    static WebSocketMetrics &getWebSocketMetrics() { return webSocketMetrics; }
    static void updateWebSocketMetrics(const WebSocketMetrics &metrics);

    // Connection state
    static ConnectionState &getConnectionState() { return connectionState; }
    static void setConnectionState(bool connected, const String &ip, uint16_t port);

    // System information
    static String getUptime();
    static String getSystemInfo();

    // Reboot counter
    static uint32_t getRebootCounter() { return rebootCounter; }
    static void incrementRebootCounter();
};

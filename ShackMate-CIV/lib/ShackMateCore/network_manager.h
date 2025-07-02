#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "logger.h"
#include "device_state.h"

// -------------------------------------------------------------------------
// Network and WebSocket Management Module
// -------------------------------------------------------------------------
class NetworkManager
{
private:
    static WebSocketsClient wsClient;
    static WiFiUDP udpListener;
    static AsyncWebSocket webSocket;

    // Connection state tracking
    static bool wsClientConnected;
    static bool wsClientEverConnected;
    static String connectedServerIP;
    static uint16_t connectedServerPort;
    static unsigned long lastConnectionAttempt;
    static unsigned long lastWebSocketActivity;
    static unsigned long lastPingSent;

    // Connection constants
    static constexpr unsigned long CONNECTION_COOLDOWN = 10000; // 10 seconds
    static constexpr unsigned long WEBSOCKET_TIMEOUT = 60000;   // 60 seconds
    static constexpr unsigned long PING_INTERVAL = 30000;       // 30 seconds

public:
    static void init();
    static void update();

    // WebSocket Server Management
    static AsyncWebSocket &getWebSocket() { return webSocket; }
    static void broadcastToWebClients(const String &message);
    static void setWebSocketEventHandler(AwsEventHandler handler);

    // WebSocket Client Management
    static bool isClientConnected() { return wsClientConnected; }
    static bool hasEverConnected() { return wsClientEverConnected; }
    static String getConnectedServerIP() { return connectedServerIP; }
    static uint16_t getConnectedServerPort() { return connectedServerPort; }
    static WebSocketsClient &getWebSocketClient() { return wsClient; }
    static void sendToServer(const String &message);
    static void disconnectFromServer();

    // UDP Discovery
    static void handleUdpDiscovery();
    static void connectToShackMateServer(const String &ip, uint16_t port);

    // Event Handlers
    static void onWebSocketClientEvent(WStype_t type, uint8_t *payload, size_t length);

    // Connection Health Monitoring
    static void checkConnectionHealth();
    static void updateConnectionState(bool connected, const String &ip = "", uint16_t port = 0);

private:
    static void setupUdpListener();
    static void processUdpMessage(const String &message);
    static bool shouldAttemptConnection(const String &ip, uint16_t port);
};

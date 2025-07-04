#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// -------------------------------------------------------------------------
// Web Server Module
// -------------------------------------------------------------------------

class WebServerManager
{
private:
    static AsyncWebServer *httpServer;
    static bool initialized;

public:
    // Initialization
    static void init(AsyncWebServer *server);

    // HTTP request handlers
    static void handleRoot(AsyncWebServerRequest *request);
    static void handleDataJson(AsyncWebServerRequest *request);
    static void handleSaveConfig(AsyncWebServerRequest *request);
    static void handleRestoreConfig(AsyncWebServerRequest *request);
    static void handleReboot(AsyncWebServerRequest *request);
    static void handleFactoryReset(AsyncWebServerRequest *request);
    static void handleFavicon(AsyncWebServerRequest *request);
    static void handleTest(AsyncWebServerRequest *request);

    // WebSocket message handlers
    static void handleWebSocketMessage(AsyncWebSocketClient *client, const String &message);
    static void handleJsonCommand(AsyncWebSocketClient *client, const JsonDocument &json);
    static void handleCivMessage(const String &message);

    // Route setup
    static void setupRoutes();

    // Utility functions
    static void sendJsonResponse(AsyncWebSocketClient *client, const String &response);
    static void sendErrorResponse(AsyncWebSocketClient *client, const String &error);
};

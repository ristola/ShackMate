#include "web_server_manager.h"
#include "config.h"
#include "logger.h"
#include "device_state.h"
#include "hardware_controller.h"
#include "system_utils.h"
#include "sensor_manager.h"
#include "event_manager.h"
#include "network_manager.h"
#include "json_builder.h"
#include "civ_handler.h"
#include <SPIFFS.h>

// Static member definitions
AsyncWebServer *WebServerManager::httpServer = nullptr;
bool WebServerManager::initialized = false;

void WebServerManager::init(AsyncWebServer *server)
{
    httpServer = server;
    setupRoutes();
    initialized = true;
    LOG_INFO("Web server manager initialized");
}

void WebServerManager::setupRoutes()
{
    if (!httpServer)
        return;

    // Main routes
    httpServer->on("/", HTTP_GET, handleRoot);
    httpServer->on("^/index/data/?$", HTTP_GET, handleDataJson);
    httpServer->on("/saveConfig", HTTP_POST, handleSaveConfig);
    httpServer->on("/restoreConfig", HTTP_POST, handleRestoreConfig);
    httpServer->on("/reboot", HTTP_POST, handleReboot);
    httpServer->on("/restore", HTTP_POST, handleFactoryReset);
    httpServer->on("/favicon.ico", HTTP_GET, handleFavicon);
    httpServer->on("/test", HTTP_GET, handleTest);

    LOG_INFO("Web server routes configured");
}

void WebServerManager::handleRoot(AsyncWebServerRequest *request)
{
    String page = SystemUtils::loadFile("/index.html");
    if (page.isEmpty())
    {
        request->send(500, "text/plain", "Error loading page");
        return;
    }

    page = SystemUtils::processTemplate(page);
    request->send(200, "text/html", page);
}

void WebServerManager::handleDataJson(AsyncWebServerRequest *request)
{
    String json = JsonBuilder::buildStatusResponse();
    request->send(200, "application/json", json);
}

void WebServerManager::handleSaveConfig(AsyncWebServerRequest *request)
{
    if (request->hasArg("tcpPort"))
    {
        String tcpPort = request->arg("tcpPort");
        // Update device configuration
        DeviceState::getDeviceConfig().tcpPort = tcpPort;
        DeviceState::saveToPreferences();
    }

    request->send(200, "text/html",
                  "<html><body><h1>Configuration Saved</h1><p>The device will now reboot.</p></body></html>");

    delay(2000);
    ESP.restart();
}

void WebServerManager::handleRestoreConfig(AsyncWebServerRequest *request)
{
    // Complete WiFi reset to ensure captive portal activation
    WiFi.disconnect(true, true); // Erase WiFi creds and reset
    WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
    delay(100);
    WiFi.mode(WIFI_STA); // Set to station mode

    request->send(200, "text/html",
                  "<html><body><h1>WiFi Completely Erased</h1><p>Captive portal WILL activate on reboot.</p></body></html>");

    delay(2000);
    ESP.restart();
}

void WebServerManager::handleReboot(AsyncWebServerRequest *request)
{
    LOG_INFO("Reboot requested via HTTP");
    request->send(200, "text/plain", "Rebooting device...");
    delay(250);
    ESP.restart();
}

void WebServerManager::handleFactoryReset(AsyncWebServerRequest *request)
{
    request->send(200, "text/plain", "Completely erasing WiFi credentials...");

    // Complete WiFi reset to ensure captive portal activation
    WiFi.disconnect(true, true); // Erase WiFi creds and reset
    WiFi.mode(WIFI_OFF);         // Turn off WiFi completely
    delay(100);
    WiFi.mode(WIFI_STA); // Set to station mode
    delay(500);
    ESP.restart();
}

void WebServerManager::handleFavicon(AsyncWebServerRequest *request)
{
    // Try to serve favicon from SPIFFS, or return 404
    if (SPIFFS.exists("/favicon.ico"))
    {
        request->send(SPIFFS, "/favicon.ico", "image/x-icon");
    }
    else
    {
        request->send(404, "text/plain", "Favicon not found");
    }
}

void WebServerManager::handleTest(AsyncWebServerRequest *request)
{
    String response = "ShackMate PowerOutlet Test\n";
    response += "Version: " + String(VERSION) + "\n";
    response += "Device ID: " + String(DeviceState::getDeviceConfig().deviceId) + "\n";
    response += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
    response += "Uptime: " + SystemUtils::getUptime() + "\n";

    request->send(200, "text/plain", response);
}

void WebServerManager::handleWebSocketMessage(AsyncWebSocketClient *client, const String &message)
{
    LOG_DEBUG("WebSocket message received: " + message);

    // Try to parse as JSON first
    if (message.startsWith("{"))
    {
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, message);

        if (error)
        {
            LOG_WARNING("JSON parsing failed: " + String(error.c_str()));
            sendErrorResponse(client, "Invalid JSON format");
            return;
        }

        handleJsonCommand(client, doc);
    }
    // Handle CI-V hex messages (non-JSON)
    else if (!message.startsWith("{") && message.length() > 0)
    {
        LOG_DEBUG("Processing CI-V hex message: " + message);
        handleCivMessage(message);
    }
}

void WebServerManager::handleJsonCommand(AsyncWebSocketClient *client, const JsonDocument &json)
{
    if (!json.containsKey("command"))
    {
        sendErrorResponse(client, "Missing command field");
        return;
    }

    const char *cmd = json["command"];
    LOG_DEBUG("Processing WebSocket command: " + String(cmd));

    // Handle relay control commands
    if (json.containsKey("value") && (strcmp(cmd, "output1") == 0 || strcmp(cmd, "output2") == 0))
    {
        bool value = json["value"];
        int relayNum = (strcmp(cmd, "output1") == 0) ? 1 : 2;

        // Update hardware and state
        HardwareController::setRelay(relayNum, value);

        if (relayNum == 1)
        {
            DeviceState::getRelayState().relay1 = value;
        }
        else
        {
            DeviceState::getRelayState().relay2 = value;
        }

        DeviceState::saveToPreferences();
        EventManager::triggerRelayStateChange();

        LOG_INFO("Relay " + String(relayNum) + " set to " + String(value ? "ON" : "OFF"));
    }
    // Handle device ID change command
    else if (strcmp(cmd, "setDeviceId") == 0)
    {
        uint8_t newDeviceId = 1; // Default fallback

        // Check for "value" format: { "command": "setDeviceId", "value": 3 }
        if (json.containsKey("value"))
        {
            newDeviceId = json["value"];
        }
        // Check for "deviceId" format: { "command": "setDeviceId", "deviceId": 3 }
        else if (json.containsKey("deviceId"))
        {
            newDeviceId = json["deviceId"];
        }
        else
        {
            sendErrorResponse(client, "Missing deviceId or value field");
            return;
        }

        if (newDeviceId >= MIN_DEVICE_ID && newDeviceId <= MAX_DEVICE_ID)
        {
            LOG_INFO("Changing device ID from " + String(DeviceState::getDeviceConfig().deviceId) +
                     " to " + String(newDeviceId));

            DeviceState::setDeviceId(newDeviceId);

            String response = "Device ID changed to " + String(newDeviceId) +
                              ", CI-V address: 0x" + DeviceState::getDeviceConfig().civAddress +
                              ". Change is effective immediately.";
            sendJsonResponse(client, JsonBuilder::buildInfoResponse(response));

            EventManager::triggerRelayStateChange();
        }
        else
        {
            String errorMsg = "Invalid device ID " + String(newDeviceId) +
                              ". Must be between " + String(MIN_DEVICE_ID) +
                              " and " + String(MAX_DEVICE_ID);
            sendErrorResponse(client, errorMsg);
        }
    }
    // Handle reboot command
    else if (strcmp(cmd, "reboot") == 0)
    {
        LOG_INFO("Reboot command received via WebSocket");
        sendJsonResponse(client, JsonBuilder::buildInfoResponse("Rebooting device..."));
        delay(250);
        ESP.restart();
    }
    else
    {
        LOG_WARNING("Unknown WebSocket command: " + String(cmd));
        sendErrorResponse(client, "Unknown command: " + String(cmd));
    }
}

void WebServerManager::handleCivMessage(const String &message)
{
    LOG_DEBUG("=== CI-V MESSAGE RECEIVED FROM WEBSOCKET CLIENT ===");
    LOG_DEBUG("Raw message from remote server: '" + message + "'");

    // Process the CI-V message using the CIV handler
    // This will be implemented when CIV handler is refactored
    EventManager::triggerCivMessage("Received: " + message);
}

void WebServerManager::sendJsonResponse(AsyncWebSocketClient *client, const String &response)
{
    if (client && client->status() == WS_CONNECTED)
    {
        client->text(response);
    }
}

void WebServerManager::sendErrorResponse(AsyncWebSocketClient *client, const String &error)
{
    sendJsonResponse(client, JsonBuilder::buildErrorResponse(error));
}

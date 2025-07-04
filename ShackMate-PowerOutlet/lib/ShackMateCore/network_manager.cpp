#include "network_manager.h"
#include "json_builder.h"

// Static member definitions
WebSocketsClient NetworkManager::wsClient;
WiFiUDP NetworkManager::udpListener;
AsyncWebSocket NetworkManager::webSocket("/ws");

bool NetworkManager::wsClientConnected = false;
bool NetworkManager::wsClientEverConnected = false;
String NetworkManager::connectedServerIP = "";
uint16_t NetworkManager::connectedServerPort = 0;
unsigned long NetworkManager::lastConnectionAttempt = 0;
unsigned long NetworkManager::lastWebSocketActivity = 0;
unsigned long NetworkManager::lastPingSent = 0;

void NetworkManager::init()
{
    LOG_INFO("Initializing network manager");

    // Setup UDP listener for device discovery
    setupUdpListener();

    // Configure WebSocket client event handler
    wsClient.onEvent(onWebSocketClientEvent);
    wsClient.setReconnectInterval(10000);
    wsClient.enableHeartbeat(15000, 3000, 2);

    LOG_INFO("Network manager initialized successfully");
}

void NetworkManager::update()
{
    // Process WebSocket client events - CRITICAL for connection establishment
    wsClient.loop();

    // Handle UDP discovery
    handleUdpDiscovery();

    // Check WebSocket client connection health
    checkConnectionHealth();

    // Periodic UDP status debug (every 30 seconds)
    static unsigned long lastUdpStatusDebug = 0;
    if (millis() - lastUdpStatusDebug >= 30000)
    {
        LOG_DEBUG("UDP listener status: port " + String(UDP_PORT) + " - waiting for 'ShackMate,IP,Port' messages");
        lastUdpStatusDebug = millis();
    }
}

void NetworkManager::broadcastToWebClients(const String &message)
{
    webSocket.textAll(message);
}

void NetworkManager::setWebSocketEventHandler(AwsEventHandler handler)
{
    webSocket.onEvent(handler);
}

void NetworkManager::sendToServer(const String &message)
{
    if (wsClientConnected)
    {
        // Enhanced debug for CI-V messages being sent
        if (message.length() >= 12 && message.indexOf("FE") != -1)
        {
            // Analyze what type of CI-V response we're sending
            if (message.indexOf("19 00") != -1)
            {
                LOG_INFO(">>> TRANSMITTING CI-V: Echo Response (19 00) - " + message);
                LOG_INFO("    Confirming our CI-V address (B3) to remote server");
            }
            else if (message.indexOf("19 01") != -1)
            {
                LOG_INFO(">>> TRANSMITTING CI-V: Model ID Response (19 01) - " + message);
                LOG_INFO("    Sending our IP address in hex format");
            }
            else if (message.indexOf(" 34 ") != -1)
            {
                // Extract model type from message for better debugging
                String modelType = "??";
                if (message.indexOf(" 34 00 ") != -1)
                {
                    modelType = "00 (ATOM Power Outlet)";
                }
                else if (message.indexOf(" 34 01 ") != -1)
                {
                    modelType = "01 (Wyze Outdoor Power Outlet)";
                }
                LOG_INFO(">>> TRANSMITTING CI-V: Model Response (34 " + modelType + ") - " + message);
            }
            else if (message.indexOf(" 35 ") != -1)
            {
                LOG_INFO(">>> TRANSMITTING CI-V: Outlet Status Response (35) - " + message);
            }
            else if (message.indexOf(" FA ") != -1)
            {
                LOG_INFO(">>> TRANSMITTING CI-V: NAK Response (FA) - Invalid command - " + message);
            }
            else
            {
                LOG_INFO(">>> TRANSMITTING CI-V: " + message + " -> " + connectedServerIP + ":" + String(connectedServerPort));
            }
        }
        else
        {
            LOG_DEBUG("Sending message to server: " + message);
        }

        String msgCopy = message; // WebSocketsClient needs non-const reference
        wsClient.sendTXT(msgCopy);
        lastWebSocketActivity = millis();
    }
    else
    {
        LOG_WARNING("Cannot send message - WebSocket client not connected");
        if (message.length() >= 12 && message.indexOf("FE") != -1)
        {
            LOG_ERROR("FAILED TO TRANSMIT CI-V: " + message + " (WebSocket disconnected)");
        }
    }
}

void NetworkManager::disconnectFromServer()
{
    if (wsClientConnected)
    {
        LOG_INFO("Disconnecting from WebSocket server");
        wsClient.disconnect();
        wsClientConnected = false;
    }
}

void NetworkManager::handleUdpDiscovery()
{
    int packetSize = udpListener.parsePacket();
    if (packetSize)
    {
        char packetBuffer[256];
        int len = udpListener.read(packetBuffer, sizeof(packetBuffer) - 1);
        packetBuffer[len] = '\0';

        String message = String(packetBuffer);
        LOG_INFO("UDP packet received (size: " + String(packetSize) + "): '" + message + "'");

        processUdpMessage(message);
    }
}

void NetworkManager::connectToShackMateServer(const String &ip, uint16_t port)
{
    if (!shouldAttemptConnection(ip, port))
    {
        return;
    }

    LOG_INFO("Connecting to ShackMate server at " + ip + ":" + String(port));

    // Disconnect any existing connection
    if (wsClientConnected)
    {
        LOG_INFO("Disconnecting existing WebSocket connection");
        wsClient.disconnect();
        wsClientConnected = false;
        delay(500);
    }

    try
    {
        LOG_DEBUG("Setting up WebSocket client for: ws://" + ip + ":" + String(port) + "/ws");

        // Set authentication (if required by server)
        // wsClient.setAuthorization("user", "password"); // Uncomment if server requires auth

        // Enable additional debugging
        wsClient.setReconnectInterval(5000);
        wsClient.enableHeartbeat(15000, 3000, 2);

        LOG_DEBUG("WebSocket client configuration:");
        LOG_DEBUG("  - Target: ws://" + ip + ":" + String(port) + "/ws");
        LOG_DEBUG("  - Reconnect interval: 5000ms");
        LOG_DEBUG("  - Heartbeat enabled: 15s interval, 3s timeout, 2 retries");

        wsClient.begin(ip, port, "/ws");

        // Update connection info
        connectedServerIP = ip;
        connectedServerPort = port;
        lastConnectionAttempt = millis();

        // Update DeviceState
        DeviceState::setConnectionState(false, ip, port);

        // Broadcast discovery status to web clients
        String statusMsg = JsonBuilder::buildStatusResponse();
        broadcastToWebClients(statusMsg);

        LOG_INFO("WebSocket client setup complete for: " + ip + ":" + String(port) + " - waiting for connection event");
        LOG_INFO("Connection attempt initiated at: " + String(lastConnectionAttempt) + "ms");
    }
    catch (...)
    {
        LOG_ERROR("Exception during WebSocket client connection setup");
    }
}

void NetworkManager::onWebSocketClientEvent(WStype_t type, uint8_t *payload, size_t length)
{
    try
    {
        switch (type)
        {
        case WStype_DISCONNECTED:
            LOG_INFO("WebSocket client DISCONNECTED from " + connectedServerIP + ":" + String(connectedServerPort));
            LOG_DEBUG("Disconnect event details - payload length: " + String(length));
            updateConnectionState(false);
            break;

        case WStype_CONNECTED:
            LOG_INFO("WebSocket client CONNECTED to " + connectedServerIP + ":" + String(connectedServerPort));
            LOG_INFO("Connected to URL: " + String((char *)payload));
            updateConnectionState(true, connectedServerIP, connectedServerPort);
            break;

        case WStype_ERROR:
            LOG_ERROR("WebSocket client ERROR occurred - connection failed");
            LOG_ERROR("Error details - payload length: " + String(length));
            if (length > 0 && payload != nullptr)
            {
                String errorMsg = String((char *)payload);
                LOG_ERROR("Error message: " + errorMsg);
            }
            wsClientConnected = false;
            break;

        case WStype_PING:
            LOG_DEBUG("WebSocket client received PING");
            lastWebSocketActivity = millis();
            break;

        case WStype_PONG:
            LOG_DEBUG("WebSocket client received PONG");
            lastWebSocketActivity = millis();
            break;

        case WStype_TEXT:
        {
            lastWebSocketActivity = millis();
            String message = String((char *)payload);

            // Performance optimization: Reduce verbose logging during heavy traffic
            static unsigned long lastCivLogTime = 0;
            static uint32_t messageCount = 0;
            unsigned long currentTime = millis();

            messageCount++;
            bool verboseLogging = (currentTime - lastCivLogTime > 5000); // Every 5 seconds

            if (verboseLogging)
            {
                LOG_DEBUG("WebSocket received message #" + String(messageCount) + ": " + message);
                lastCivLogTime = currentTime;
            }

            // Enhanced CI-V debug logging for specific commands (only when verbose)
            if (verboseLogging && message.length() >= 12 && message.indexOf("FE") != -1)
            {
                // Check for CI-V commands we're interested in
                if (message.indexOf("19 00") != -1)
                {
                    LOG_INFO("CI-V: Echo Request (19 00) received");
                }
                else if (message.indexOf("19 01") != -1)
                {
                    LOG_INFO("CI-V: Model ID Request (19 01) received");
                }
                else if (message.indexOf(" 34 ") != -1)
                {
                    LOG_INFO("CI-V: Read Model Request (34) received");
                }
                else if (message.indexOf(" 35 ") != -1 || message.indexOf(" 35") == message.length() - 3)
                {
                    LOG_INFO("CI-V: Outlet Control (35) received");
                }
                else if (message.indexOf("FE FE B3") != -1)
                {
                    LOG_INFO("CI-V: Direct message to our address received");
                }
                else if (message.indexOf("FE FE 00") != -1)
                {
                    LOG_INFO("CI-V: Broadcast message received");
                }
            }

            // Forward message to main application for processing
            extern void handleReceivedCivMessage(const String &message);
            handleReceivedCivMessage(message);

            // Yield after processing to prevent blocking
            yield();
            break;
        }

        case WStype_BIN:
            LOG_DEBUG("WebSocket client received binary data (length: " + String(length) + ")");
            lastWebSocketActivity = millis();
            break;

        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
            LOG_DEBUG("WebSocket client received fragment (type: " + String(type) + ")");
            break;

        default:
            LOG_DEBUG("WebSocket client unknown event type: " + String(type));
            break;
        }
    }
    catch (...)
    {
        LOG_ERROR("Exception in WebSocket client event handler");
    }
}

void NetworkManager::checkConnectionHealth()
{
    unsigned long currentTime = millis();

    // Check for connection timeout if connected
    if (wsClientConnected && currentTime - lastWebSocketActivity > WEBSOCKET_TIMEOUT)
    {
        LOG_WARNING("WebSocket connection timeout - disconnecting");
        disconnectFromServer();
        return;
    }

    // Check for connection attempt timeout (if we're trying to connect but no response)
    if (!wsClientConnected && lastConnectionAttempt > 0 &&
        currentTime - lastConnectionAttempt > 15000) // 15 second timeout for connection attempts
    {
        LOG_ERROR("WebSocket connection attempt timed out after 15 seconds");
        LOG_ERROR("Server " + connectedServerIP + ":" + String(connectedServerPort) + " may not be responding");
        LOG_ERROR("Resetting connection attempt timer - will retry on next UDP discovery");
        lastConnectionAttempt = 0; // Reset to prevent spam

        // Force disconnect to clean up any partial connection state
        wsClient.disconnect();
    }

    // Periodic connection status debug (every 10 seconds when attempting to connect)
    static unsigned long lastConnectionDebug = 0;
    if (!wsClientConnected && lastConnectionAttempt > 0 &&
        currentTime - lastConnectionDebug >= 10000)
    {
        unsigned long attemptAge = currentTime - lastConnectionAttempt;
        LOG_DEBUG("Connection attempt status:");
        LOG_DEBUG("  - Target: " + connectedServerIP + ":" + String(connectedServerPort));
        LOG_DEBUG("  - Attempt started: " + String(attemptAge) + "ms ago");
        LOG_DEBUG("  - Connected: " + String(wsClientConnected ? "YES" : "NO"));
        LOG_DEBUG("  - Client state: " + String(wsClient.isConnected() ? "Connected" : "Disconnected"));
        lastConnectionDebug = currentTime;
    }

    // Send periodic ping if connected
    if (wsClientConnected && currentTime - lastPingSent > PING_INTERVAL)
    {
        // Note: WebSocketsClient library handles pings automatically
        // This is just for our internal tracking
        lastPingSent = currentTime;
        LOG_DEBUG("Heartbeat interval - last activity: " + String(currentTime - lastWebSocketActivity) + "ms ago");
    }
}

void NetworkManager::updateConnectionState(bool connected, const String &ip, uint16_t port)
{
    wsClientConnected = connected;
    if (connected)
    {
        wsClientEverConnected = true;
        lastWebSocketActivity = millis();
        lastPingSent = millis();

        if (!ip.isEmpty())
        {
            connectedServerIP = ip;
            connectedServerPort = port;
        }
    }

    // Update DeviceState
    DeviceState::setConnectionState(connected, connectedServerIP, connectedServerPort);

    // Broadcast status update to web clients
    String statusMsg = JsonBuilder::buildStatusResponse();
    broadcastToWebClients(statusMsg);

    String statusText = connected ? "CONNECTED" : "DISCONNECTED";
    LOG_INFO("Broadcasted " + statusText + " status to web clients");
}

void NetworkManager::setupUdpListener()
{
    if (udpListener.begin(UDP_PORT))
    {
        LOG_INFO("UDP listener started on port " + String(UDP_PORT));
    }
    else
    {
        LOG_ERROR("Failed to start UDP listener on port " + String(UDP_PORT));
    }
}

void NetworkManager::processUdpMessage(const String &message)
{
    LOG_DEBUG("Processing UDP message: '" + message + "'");

    // Parse message format: 'ShackMate,IP,Port'
    if (message.indexOf("ShackMate") >= 0)
    {
        LOG_DEBUG("Message contains 'ShackMate' - parsing...");

        int firstComma = message.indexOf(',');
        int secondComma = message.indexOf(',', firstComma + 1);

        LOG_DEBUG("Comma positions: first=" + String(firstComma) + ", second=" + String(secondComma));

        if (firstComma > 0 && secondComma > firstComma)
        {
            String remoteIP = message.substring(firstComma + 1, secondComma);
            String remotePort = message.substring(secondComma + 1);

            remoteIP.trim();
            remotePort.trim();

            LOG_DEBUG("Parsed IP: '" + remoteIP + "', Port: '" + remotePort + "'");

            if (remoteIP.length() > 0 && remotePort.length() > 0)
            {
                uint16_t port = remotePort.toInt();

                // Don't connect to ourselves
                if (remoteIP != WiFi.localIP().toString())
                {
                    LOG_INFO("ShackMate server discovered: " + remoteIP + ":" + String(port));

                    // Check current connection status
                    if (wsClientConnected)
                    {
                        LOG_DEBUG("Already connected to: " + connectedServerIP + ":" + String(connectedServerPort));
                        if (connectedServerIP == remoteIP && connectedServerPort == port)
                        {
                            LOG_DEBUG("Discovery matches current connection - ignoring");
                        }
                        else
                        {
                            LOG_DEBUG("Discovery for different server - considering reconnect");
                        }
                        return;
                    }

                    // Check if we're in the middle of a connection attempt
                    unsigned long currentTime = millis();
                    if (lastConnectionAttempt > 0 && currentTime - lastConnectionAttempt < 15000)
                    {
                        LOG_DEBUG("Connection attempt in progress (" + String(currentTime - lastConnectionAttempt) + "ms ago) - ignoring discovery");
                        return;
                    }

                    // Check connection cooldown
                    if (currentTime - lastConnectionAttempt >= CONNECTION_COOLDOWN)
                    {
                        LOG_INFO("Initiating connection to discovered server: " + remoteIP + ":" + String(port));
                        connectToShackMateServer(remoteIP, port);
                    }
                    else
                    {
                        LOG_DEBUG("Connection attempt in cooldown - waiting " + String(CONNECTION_COOLDOWN - (currentTime - lastConnectionAttempt)) + "ms");
                    }
                }
                else
                {
                    LOG_DEBUG("Ignoring UDP discovery from self: " + remoteIP);
                }
            }
            else
            {
                LOG_DEBUG("Empty IP or port after parsing");
            }
        }
        else
        {
            LOG_DEBUG("Invalid comma positions in ShackMate message");
        }
    }
    else
    {
        LOG_DEBUG("Message does not contain 'ShackMate' - ignoring");
    }
}

bool NetworkManager::shouldAttemptConnection(const String &ip, uint16_t port)
{
    // Check if we're already connected to this server
    if (wsClientConnected && connectedServerIP == ip && connectedServerPort == port)
    {
        LOG_DEBUG("Already connected to " + ip + ":" + String(port) + " - skipping");
        return false;
    }

    // Check connection cooldown
    unsigned long currentTime = millis();
    if (currentTime - lastConnectionAttempt < CONNECTION_COOLDOWN)
    {
        LOG_DEBUG("Connection cooldown active - skipping connection attempt");
        return false;
    }

    // Don't connect to ourselves
    if (ip == WiFi.localIP().toString())
    {
        LOG_DEBUG("Skipping connection to self: " + ip);
        return false;
    }

    return true;
}

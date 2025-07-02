#include "civ_handler.h"
#include "config.h"
#include "logger.h"
#include <WiFi.h>

void CivHandler::init(uint8_t deviceAddress)
{
    deviceAddr = deviceAddress;
    lastBroadcastMsg = "";
    lastBroadcastTime = 0;
    LOG_INFO("CI-V Handler initialized with address: 0x" + String(deviceAddress, HEX));
}

CivHandler::CivMessage CivHandler::parseMessage(const String &hexMsg)
{
    CivMessage msg = {false, 0, 0, 0, 0, {}};

    LOG_DEBUG("CI-V: Parsing message: '" + hexMsg + "'");

    // Remove spaces and convert to upper case
    String cleanHex = hexMsg;
    cleanHex.replace(" ", "");
    cleanHex.toUpperCase();

    LOG_DEBUG("CI-V: Clean hex string: '" + cleanHex + "' (length: " + String(cleanHex.length()) + ")");

    // Check minimum length (FE FE TO FROM CMD FD = 12 hex chars = 6 bytes minimum)
    // Check maximum length to prevent excessive memory allocation
    if (cleanHex.length() < 12 || cleanHex.length() % 2 != 0 || cleanHex.length() > 128)
    {
        LOG_DEBUG("CI-V: Invalid message length: " + String(cleanHex.length()) + " (min: 12, max: 128)");
        return msg;
    }

    // Convert hex string to bytes with memory bounds checking
    std::vector<uint8_t> bytes;
    bytes.reserve(cleanHex.length() / 2); // Pre-allocate to avoid reallocations

    for (int i = 0; i < cleanHex.length(); i += 2)
    {
        String byteStr = cleanHex.substring(i, i + 2);
        uint8_t byte = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
        bytes.push_back(byte);
    }

    // Validate CI-V message format
    if (bytes.size() < 6)
    {
        LOG_DEBUG("CI-V: Message too short: " + String(bytes.size()) + " bytes");
        return msg;
    }

    // Check preamble (FE FE)
    if (bytes[0] != 0xFE || bytes[1] != 0xFE)
    {
        LOG_DEBUG("CI-V: Invalid preamble - expected FE FE, got " + String(bytes[0], HEX) + " " + String(bytes[1], HEX));
        return msg;
    }

    // Check terminator (FD)
    if (bytes[bytes.size() - 1] != 0xFD)
    {
        String invalidHex = String(bytes[bytes.size() - 1], HEX);
        invalidHex.toUpperCase();
        if (invalidHex.length() == 1)
            invalidHex = "0" + invalidHex;
        LOG_DEBUG("CI-V: Invalid terminator - expected FD, got " + invalidHex);
        return msg;
    }

    // Extract message components
    msg.toAddr = bytes[2];
    msg.fromAddr = bytes[3];
    msg.command = bytes[4];

    // For command 35 (outlet status), there is no subcommand - data comes directly after command
    if (msg.command == 0x35)
    {
        msg.subCommand = 0x00; // No subcommand for command 35

        // Extract data portion (after command, before terminator) with bounds checking
        if (bytes.size() > 6)
        {
            for (size_t i = 5; i < bytes.size() - 1 && msg.data.size() < 16; i++)
            {
                msg.data.push_back(bytes[i]);
            }
        }
    }
    else
    {
        // For other commands: FE FE TO FROM CMD [SUB] [DATA...] FD
        if (bytes.size() == 6)
        {
            // Basic command, no subcommand
            msg.subCommand = 0x00;
        }
        else if (bytes.size() >= 7)
        {
            // Command with subcommand and optional data
            msg.subCommand = bytes[5];

            // Extract data portion (after subcommand, before terminator) with bounds checking
            if (bytes.size() > 7)
            {
                for (size_t i = 6; i < bytes.size() - 1 && msg.data.size() < 16; i++)
                {
                    msg.data.push_back(bytes[i]);
                }
            }
        }
    }

    msg.valid = true;

    // Debug output
    LOG_DEBUG("CI-V: Parsed - TO:0x" + String(msg.toAddr, HEX) + " FROM:0x" + String(msg.fromAddr, HEX) +
              " CMD:0x" + String(msg.command, HEX) + " SUB:0x" + String(msg.subCommand, HEX));

    return msg;
}

bool CivHandler::isMessageForUs(const CivMessage &msg)
{
    // First check: ignore messages that originated FROM us (to prevent echo loops)
    if (msg.fromAddr == deviceAddr)
    {
        LOG_DEBUG("CI-V: Ignoring message FROM us (0x" + String(msg.fromAddr, HEX) + ") - TO: 0x" + String(msg.toAddr, HEX) + " to prevent echo loop");
        return false;
    }

    bool isBroadcast = (msg.toAddr == 0x00);
    bool isAddressedToUs = (msg.toAddr == deviceAddr);

    // For broadcast messages, check for duplicates to prevent multiple responses
    if (isBroadcast)
    {
        // Reconstruct the original message string for deduplication
        String msgStr = "FE FE 00 " + String(msg.fromAddr, HEX) + " " + String(msg.command, HEX);
        if (msg.command != 0x35 && msg.subCommand != 0x00)
        {
            msgStr += " " + String(msg.subCommand, HEX);
        }
        for (uint8_t dataByte : msg.data)
        {
            msgStr += " " + String(dataByte, HEX);
        }
        msgStr += " FD";
        msgStr.toUpperCase();

        if (isDuplicateBroadcast(msgStr))
        {
            LOG_DEBUG("CI-V: Ignoring duplicate broadcast within " + String(BROADCAST_DEDUP_WINDOW_MS) + "ms window");
            return false;
        }

        LOG_DEBUG("CI-V: BROADCAST message received - Our addr: 0x" + String(deviceAddr, HEX) +
                  ", FROM: 0x" + String(msg.fromAddr, HEX));
    }

    if (isBroadcast || isAddressedToUs)
    {
        String addrType = isBroadcast ? "broadcast" : "direct";
        LOG_DEBUG("CI-V: Message for us (" + addrType + ") - Our addr: 0x" + String(deviceAddr, HEX) +
                  ", TO: 0x" + String(msg.toAddr, HEX) + ", FROM: 0x" + String(msg.fromAddr, HEX));
        return true;
    }

    // Message not for us - only log if it's not a broadcast to reduce spam
    if (!isBroadcast)
    {
        LOG_DEBUG("CI-V: Message not for us - Our addr: 0x" + String(deviceAddr, HEX) +
                  ", TO: 0x" + String(msg.toAddr, HEX) + ", FROM: 0x" + String(msg.fromAddr, HEX));
    }
    return false;
}

String CivHandler::processMessage(const CivMessage &msg, bool relay1State, bool relay2State,
                                  bool &newRelay1State, bool &newRelay2State)
{
    // Initialize output parameters with current states
    newRelay1State = relay1State;
    newRelay2State = relay2State;

    // Provide a clear summary of what command we're processing
    String commandSummary = "CI-V: Processing ";
    if (msg.command == 0x19 && msg.subCommand == 0x00)
    {
        commandSummary += "19 00 (Echo - asking for our CI-V address)";
    }
    else if (msg.command == 0x19 && msg.subCommand == 0x01)
    {
        commandSummary += "19 01 (Model ID - asking for our IP address in hex)";
    }
    else if (msg.command == 0x34)
    {
        commandSummary += "34 (Read Model - asking what type of device we are)";
    }
    else if (msg.command == 0x35 && msg.data.size() == 0)
    {
        commandSummary += "35 (Read Outlet Status - asking what outlets are on/off)";
    }
    else if (msg.command == 0x35 && msg.data.size() == 1)
    {
        commandSummary += "35 " + String(msg.data[0], HEX) + " (Set Outlet Status - telling us what outlets to turn on/off)";
    }
    else
    {
        commandSummary += String(msg.command, HEX) + " " + String(msg.subCommand, HEX) + " (Unsupported command)";
    }
    LOG_DEBUG(commandSummary);

    // Handle specific commands
    switch (msg.command)
    {
    case 0x19:
        if (msg.subCommand == 0x00)
            return handleEchoRequest(msg);
        else if (msg.subCommand == 0x01)
            return handleModelIdRequest(msg);
        break;

    case 0x34:
        return handleReadModelRequest(msg);

    case 0x35:
        return handleOutletStatusCommand(msg, relay1State, relay2State, newRelay1State, newRelay2State);

    default:
        LOG_DEBUG("CI-V: Unsupported command 0x" + String(msg.command, HEX));
        break;
    }

    return ""; // No response for unsupported commands
}

String CivHandler::handleEchoRequest(const CivMessage &msg)
{
    LOG_DEBUG("CI-V: 19 00 - Echo request (asking for our CI-V address) - responding with 0x" + String(deviceAddr, HEX));

    // Create echo response: FE FE FROM_ADDR OUR_ADDR 19 00 OUR_ADDR FD
    uint8_t response[] = {0xFE, 0xFE, msg.fromAddr, deviceAddr, 0x19, 0x00, deviceAddr, 0xFD};
    String echoResponse = formatHexString(response, sizeof(response));

    LOG_DEBUG("<<< CI-V OUTGOING: Echo Response (19 00) - " + echoResponse);
    LOG_DEBUG("    Purpose: Confirming our CI-V address (0x" + String(deviceAddr, HEX) + ") to sender (0x" + String(msg.fromAddr, HEX) + ")");

    return echoResponse;
}

String CivHandler::handleModelIdRequest(const CivMessage &msg)
{
    LOG_DEBUG("CI-V: 19 01 - Model ID request (asking for our IP address in hex) - responding with IP as hex");

    // Get current IP address
    IPAddress ip = WiFi.localIP();

    // Create ModelID response: FE FE FROM_ADDR OUR_ADDR 19 01 IP[0] IP[1] IP[2] IP[3] FD
    uint8_t response[] = {0xFE, 0xFE, msg.fromAddr, deviceAddr, 0x19, 0x01, ip[0], ip[1], ip[2], ip[3], 0xFD};
    String modelResponse = formatHexString(response, sizeof(response));

    // Create readable hex representation of IP
    String ipHex = String(ip[0], HEX) + " " + String(ip[1], HEX) + " " + String(ip[2], HEX) + " " + String(ip[3], HEX);
    ipHex.toUpperCase();

    LOG_DEBUG("<<< CI-V OUTGOING: Model ID Response (19 01) - " + modelResponse);
    LOG_DEBUG("    Purpose: Sending our IP address in hex format");
    LOG_DEBUG("    IP Address: " + ip.toString() + " -> Hex: " + ipHex);

    return modelResponse;
}

String CivHandler::handleReadModelRequest(const CivMessage &msg)
{
    LOG_DEBUG("CI-V: 34 - Read Model request (asking what type of device we are) - responding with model type");

    // Get the model type from configuration
    uint8_t modelType = DEFAULT_CIV_MODEL_TYPE;
    String modelDescription;
    switch (modelType)
    {
    case CIV_MODEL_ATOM_POWER_OUTLET:
        modelDescription = "ATOM Power Outlet";
        break;
    case CIV_MODEL_WYZE_OUTDOOR_OUTLET:
        modelDescription = "Wyze Outdoor Power Outlet";
        break;
    default:
        modelDescription = "Unknown Model";
        break;
    }

    // Create model response: FE FE FROM_ADDR OUR_ADDR 34 MODEL_TYPE FD
    uint8_t response[] = {0xFE, 0xFE, msg.fromAddr, deviceAddr, 0x34, modelType, 0xFD};
    String modelResponse = formatHexString(response, sizeof(response));

    LOG_DEBUG("<<< CI-V OUTGOING: Read Model Response (34 " + String(modelType, HEX) + ") - " + modelResponse);
    LOG_DEBUG("    Purpose: Responding with device model type (" + String(modelType, HEX) + " = " + modelDescription + ")");

    return modelResponse;
}

String CivHandler::handleOutletStatusCommand(const CivMessage &msg, bool relay1State, bool relay2State,
                                             bool &newRelay1State, bool &newRelay2State)
{
    // For SET operations (with data), check if it's on broadcast address (00)
    // For READ operations (no data), respond to both direct addresses and broadcast
    if (msg.data.size() > 0 && msg.toAddr == 0x00)
    {
        // Check if the value is invalid (> 0x03) and respond with FA if so
        uint8_t setValue = msg.data[0];
        if (setValue > 0x03)
        {
            LOG_DEBUG("CI-V: Command 35 SET operation on broadcast with invalid value 0x" + String(setValue, HEX) + " - responding with invalid command NAK");
            return createInvalidCommandNakResponse(msg);
        }
        else
        {
            LOG_DEBUG("CI-V: Command 35 SET operation received on broadcast address (00) with valid value - responding with invalid broadcast NAK");
            // For valid values on broadcast, create a special NAK that shows the broadcast was invalid
            return createInvalidCommandNakResponse(msg);
        }
    }

    if (msg.data.size() == 0)
    {
        // Read current outlet status
        LOG_DEBUG("CI-V: 35 - Read Outlet Status request (asking what outlets are on/off)");

        // Calculate current status based on relay states
        uint8_t currentStatus = relayStatesToStatus(relay1State, relay2State);

        // Create status response: FE FE FROM_ADDR OUR_ADDR 35 STATUS FD
        uint8_t response[] = {0xFE, 0xFE, msg.fromAddr, deviceAddr, 0x35, currentStatus, 0xFD};
        String statusResponse = formatHexString(response, sizeof(response));

        String statusDescription = "";
        switch (currentStatus)
        {
        case 0x00:
            statusDescription = "Both outlets OFF";
            break;
        case 0x01:
            statusDescription = "Outlet 1 ON, Outlet 2 OFF";
            break;
        case 0x02:
            statusDescription = "Outlet 1 OFF, Outlet 2 ON";
            break;
        case 0x03:
            statusDescription = "Both outlets ON";
            break;
        }

        LOG_DEBUG("<<< CI-V OUTGOING: Outlet Status Response (35 " + String(currentStatus, HEX) + ") - " +
                  statusResponse + " - " + statusDescription + " to address 0x" + String(msg.fromAddr, HEX));

        return statusResponse;
    }
    else if (msg.data.size() == 1)
    {
        // Set outlet status - validate value first
        uint8_t newStatus = msg.data[0];

        String commandDescription = "";
        switch (newStatus)
        {
        case 0x00:
            commandDescription = "Set both outlets OFF";
            break;
        case 0x01:
            commandDescription = "Set Outlet 1 ON, Outlet 2 OFF";
            break;
        case 0x02:
            commandDescription = "Set Outlet 1 OFF, Outlet 2 ON";
            break;
        case 0x03:
            commandDescription = "Set both outlets ON";
            break;
        default:
            commandDescription = "Set outlets to INVALID value (0x" + String(newStatus, HEX) + ")";
            break;
        }

        LOG_DEBUG("CI-V: 35 " + String(newStatus, HEX) + " - " + commandDescription);

        // Check if value is valid (00-03)
        if (newStatus > 0x03)
        {
            LOG_DEBUG("CI-V: Invalid outlet status: 0x" + String(newStatus, HEX) + " - responding with invalid command NAK");
            return createInvalidCommandNakResponse(msg);
        }

        // Convert status to relay states
        if (!statusToRelayStates(newStatus, newRelay1State, newRelay2State))
        {
            LOG_DEBUG("CI-V: Failed to convert status to relay states");
            return createNakResponse(msg.fromAddr);
        }

        LOG_DEBUG("CI-V: Setting relays - Relay1: " + String(newRelay1State ? "ON" : "OFF") +
                  ", Relay2: " + String(newRelay2State ? "ON" : "OFF"));

        // Send updated status response (acknowledge the set command)
        uint8_t response[] = {0xFE, 0xFE, msg.fromAddr, deviceAddr, 0x35, newStatus, 0xFD};
        String statusResponse = formatHexString(response, sizeof(response));

        LOG_DEBUG("<<< CI-V OUTGOING: Outlet Status Set ACK (35 " + String(newStatus, HEX) + ") - " +
                  statusResponse + " - Acknowledging outlet state change to address 0x" + String(msg.fromAddr, HEX));

        return statusResponse;
    }

    return ""; // Invalid data size
}

String CivHandler::createNakResponse(uint8_t toAddr)
{
    // Create NAK response: FE FE TO_ADDR OUR_ADDR FA FD
    uint8_t response[] = {0xFE, 0xFE, toAddr, deviceAddr, 0xFA, 0xFD};
    String nakResponse = formatHexString(response, sizeof(response));

    LOG_DEBUG("<<< CI-V OUTGOING: NAK Response (FA) - " + nakResponse + " to address 0x" + String(toAddr, HEX));

    return nakResponse;
}

String CivHandler::createInvalidCommandNakResponse(const CivMessage &msg)
{
    // Create NAK response that echoes back the invalid command: FE FE FROM_ADDR OUR_ADDR [COMMAND] [SUBCOMMAND] [DATA] FA FD
    std::vector<uint8_t> response;

    // Standard preamble and addressing
    response.push_back(0xFE);
    response.push_back(0xFE);
    response.push_back(msg.fromAddr); // TO: original sender
    response.push_back(deviceAddr);   // FROM: us

    // Echo back the command
    response.push_back(msg.command);

    // For command 35, data comes directly after command (no subcommand)
    if (msg.command == 0x35)
    {
        // Add the invalid data that caused the error
        for (uint8_t dataByte : msg.data)
        {
            response.push_back(dataByte);
        }
    }
    else
    {
        // For other commands, add subcommand if present
        if (msg.subCommand != 0x00)
        {
            response.push_back(msg.subCommand);
        }

        // Add data if present
        for (uint8_t dataByte : msg.data)
        {
            response.push_back(dataByte);
        }
    }

    // Add NAK and terminator
    response.push_back(0xFA); // NAK
    response.push_back(0xFD); // Terminator

    String nakResponse = formatHexString(response.data(), response.size());

    LOG_DEBUG("<<< CI-V OUTGOING: Invalid Command NAK Response - " + nakResponse + " (echoing invalid command to address 0x" + String(msg.fromAddr, HEX) + ")");

    return nakResponse;
}

String CivHandler::formatHexString(const uint8_t *data, size_t length)
{
    String result = "";
    for (size_t i = 0; i < length; i++)
    {
        if (i > 0)
            result += " ";
        if (data[i] < 0x10)
            result += "0";
        result += String(data[i], HEX);
    }
    result.toUpperCase();
    return result;
}

uint8_t CivHandler::relayStatesToStatus(bool relay1, bool relay2)
{
    if (relay1 && relay2)
        return 0x03; // Both ON
    else if (relay1 && !relay2)
        return 0x01; // Outlet 1 ON, Outlet 2 OFF
    else if (!relay1 && relay2)
        return 0x02; // Outlet 1 OFF, Outlet 2 ON
    else
        return 0x00; // Both OFF
}

bool CivHandler::statusToRelayStates(uint8_t status, bool &relay1, bool &relay2)
{
    switch (status)
    {
    case 0x00: // Both outlets OFF
        relay1 = false;
        relay2 = false;
        return true;
    case 0x01: // Outlet 1 ON, Outlet 2 OFF
        relay1 = true;
        relay2 = false;
        return true;
    case 0x02: // Outlet 1 OFF, Outlet 2 ON
        relay1 = false;
        relay2 = true;
        return true;
    case 0x03: // Both outlets ON
        relay1 = true;
        relay2 = true;
        return true;
    default:
        return false; // Invalid status
    }
}

bool CivHandler::isDuplicateBroadcast(const String &hexMsg)
{
    unsigned long currentTime = millis();

    // Check if this is the same message as the last broadcast
    if (hexMsg == lastBroadcastMsg)
    {
        // Check if we're still within the deduplication window
        if (currentTime - lastBroadcastTime < BROADCAST_DEDUP_WINDOW_MS)
        {
            LOG_DEBUG("CI-V: Duplicate broadcast detected - same message within " + String(currentTime - lastBroadcastTime) + "ms");
            return true; // This is a duplicate
        }
    }

    // Update the last broadcast tracking
    lastBroadcastMsg = hexMsg;
    lastBroadcastTime = currentTime;

    LOG_DEBUG("CI-V: New broadcast message recorded for deduplication tracking");
    return false; // This is not a duplicate
}

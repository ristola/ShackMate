#pragma once

#include <Arduino.h>
#include <vector>
#include <WiFi.h>

/**
 * @brief CI-V Protocol Handler for ShackMate Power Outlet
 *
 * This class handles the CI-V (Computer Interface V) protocol used by Icom
 * and other ham radio manufacturers. It implements only the specific commands
 * needed for the ShackMate Power Outlet device.
 *
 * Supported Commands:
 * - 19 00: Echo Request (returns device CI-V address)
 * - 19 01: MODEL IP ADDRESS Request (returns device IP address in hex)
 * - 34: Read Model (returns device model type.  Should always be 01 for this device)
 * - 35: Read/Set Outlet Status (outlet control)
 */
class CivHandler
{
public:
    /**
     * @brief Structure to hold parsed CI-V message
     */
    struct CivMessage
    {
        bool valid;
        uint8_t toAddr;
        uint8_t fromAddr;
        uint8_t command;
        uint8_t subCommand;
        std::vector<uint8_t> data;
    };

    /**
     * @brief Initialize the CI-V handler
     * @param deviceAddress The CI-V address for this device (0xB0-0xB3)
     */
    void init(uint8_t deviceAddress);

    /**
     * @brief Parse a CI-V message from hex string
     * @param hexMsg The hex string representation of the CI-V message
     * @return Parsed CI-V message structure
     */
    CivMessage parseMessage(const String &hexMsg);

    /**
     * @brief Check if a CI-V message is addressed to this device
     * @param msg The parsed CI-V message
     * @return True if message is for this device (direct or broadcast)
     */
    bool isMessageForUs(const CivMessage &msg);

    /**
     * @brief Process a CI-V message and generate response
     * @param msg The parsed CI-V message
     * @param relay1State Current state of relay 1
     * @param relay2State Current state of relay 2
     * @param newRelay1State Output parameter for new relay 1 state
     * @param newRelay2State Output parameter for new relay 2 state
     * @return Response message (empty if no response needed)
     */
    String processMessage(const CivMessage &msg, bool relay1State, bool relay2State,
                          bool &newRelay1State, bool &newRelay2State);

    /**
     * @brief Get the current device CI-V address
     * @return CI-V address byte
     */
    uint8_t getDeviceAddress() const { return deviceAddr; }

    /**
     * @brief Set the device CI-V address
     * @param address New CI-V address (0xB0-0xB3)
     */
    void setDeviceAddress(uint8_t address) { deviceAddr = address; }

private:
    uint8_t deviceAddr; ///< CI-V address for this device

    // Broadcast deduplication to prevent multiple responses to the same broadcast
    String lastBroadcastMsg;                                     ///< Last broadcast message processed
    unsigned long lastBroadcastTime;                             ///< Time when last broadcast was processed (millis)
    static const unsigned long BROADCAST_DEDUP_WINDOW_MS = 1000; ///< Deduplication window in milliseconds

    /**
     * @brief Handle Echo Request (19 00)
     * @param msg The CI-V message
     * @return Response string
     */
    String handleEchoRequest(const CivMessage &msg);

    /**
     * @brief Handle Model ID Request (19 01)
     * @param msg The CI-V message
     * @return Response string
     */
    String handleModelIdRequest(const CivMessage &msg);

    /**
     * @brief Handle Read Model Request (34)
     * @param msg The CI-V message
     * @return Response string
     */
    String handleReadModelRequest(const CivMessage &msg);

    /**
     * @brief Handle Outlet Status Request/Set (35)
     * @param msg The CI-V message
     * @param relay1State Current state of relay 1
     * @param relay2State Current state of relay 2
     * @param newRelay1State Output parameter for new relay 1 state
     * @param newRelay2State Output parameter for new relay 2 state
     * @return Response string
     */
    String handleOutletStatusCommand(const CivMessage &msg, bool relay1State, bool relay2State,
                                     bool &newRelay1State, bool &newRelay2State);

    /**
     * @brief Create a NAK (negative acknowledgment) response
     * @param toAddr Address to send NAK to
     * @return NAK response string
     */
    String createNakResponse(uint8_t toAddr);

    /**
     * @brief Create a NAK response that echoes back the invalid command
     * @param msg The original CI-V message that was invalid
     * @return NAK response string with echoed command
     */
    String createInvalidCommandNakResponse(const CivMessage &msg);

    /**
     * @brief Format a hex string with proper spacing
     * @param data Raw bytes
     * @param length Number of bytes
     * @return Formatted hex string
     */
    String formatHexString(const uint8_t *data, size_t length);

    /**
     * @brief Convert outlet states to CI-V status byte
     * @param relay1 State of relay 1
     * @param relay2 State of relay 2
     * @return Status byte (0x00-0x03)
     */
    uint8_t relayStatesToStatus(bool relay1, bool relay2);

    /**
     * @brief Convert CI-V status byte to relay states
     * @param status Status byte (0x00-0x03)
     * @param relay1 Output for relay 1 state
     * @param relay2 Output for relay 2 state
     * @return True if status is valid (0x00-0x03)
     */
    bool statusToRelayStates(uint8_t status, bool &relay1, bool &relay2);

    /**
     * @brief Check if broadcast message is a duplicate within the deduplication window
     * @param hexMsg The original hex message string
     * @return True if this is a duplicate broadcast that should be ignored
     */
    bool isDuplicateBroadcast(const String &hexMsg);
};

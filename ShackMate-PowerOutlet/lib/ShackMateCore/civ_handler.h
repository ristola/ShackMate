/**
 * @file civ_handler.h
 * @brief CI-V Protocol Handler for ShackMate Power Outlet
 *
 * This module handles CI-V (Computer Interface V) protocol communication
 * for ham radio integration with the ShackMate Power Outlet device.
 *
 * Features:
 * - CI-V message parsing and validation
 * - Address filtering (broadcast and direct messages)
 * - Command processing (echo, model ID, outlet control)
 * - Rate limiting for high-traffic scenarios
 * - Performance optimizations for heavy CI-V traffic
 *
 * Supported CI-V Commands:
 * - 0x19/0x00: Echo Request
 * - 0x19/0x01: Model ID Request
 * - 0x34: Read Model
 * - 0x35: Outlet Control (read/write status)
 *
 * @author ShackMate Project
 * @version 1.0.0
 */

#ifndef CIV_HANDLER_H
#define CIV_HANDLER_H

#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <config.h>

/**
 * @brief Debug callback function type
 * Used to send debug messages from CI-V handler to main application
 */
typedef void (*DebugCallback)(const String &message);

/**
 * @brief CI-V Message Structure
 *
 * Represents a parsed CI-V protocol message with all components
 * extracted from the raw hex string format.
 */
struct CivMessage
{
    bool valid;                ///< Message validation flag
    uint8_t toAddr;            ///< Destination address
    uint8_t fromAddr;          ///< Source address
    uint8_t command;           ///< Primary command byte
    uint8_t subCommand;        ///< Sub-command byte (if applicable)
    std::vector<uint8_t> data; ///< Additional data payload
};

/**
 * @brief CI-V Protocol Handler Class
 *
 * Handles all CI-V protocol operations including message parsing,
 * validation, filtering, and command processing.
 */
class CivHandler
{
public:
    /**
     * @brief Constructor
     * @param debugCallback Optional debug callback function
     */
    CivHandler(DebugCallback debugCallback = nullptr);

    /**
     * @brief Set debug callback function
     * @param debugCallback Debug callback function to use
     */
    void setDebugCallback(DebugCallback debugCallback);

    /**
     * @brief Initialize the CI-V handler
     * @param deviceId Device ID for CI-V address calculation
     */
    void init(uint8_t deviceId);

    /**
     * @brief Parse CI-V message from hex string
     * @param hexMsg Raw CI-V message in hex string format
     * @return Parsed CivMessage structure
     */
    CivMessage parseCivMessage(const String &hexMsg);

    /**
     * @brief Check if CI-V message is addressed to us
     * @param msg Parsed CI-V message
     * @return true if message is for this device
     */
    bool isCivMessageForUs(const CivMessage &msg);

    /**
     * @brief Process CI-V message and generate appropriate response
     * @param msg Parsed CI-V message
     * @return true if message was handled internally
     */
    bool processCivMessage(const CivMessage &msg);

    /**
     * @brief Handle received CI-V message (main entry point)
     * @param message Raw CI-V message string
     */
    void handleReceivedCivMessage(const String &message);

    /**
     * @brief Get current CI-V address for this device
     * @return CI-V address byte
     */
    uint8_t getCivAddressByte() const;

    /**
     * @brief Set relay states (called by CI-V outlet control commands)
     * @param relay1 State for relay 1
     * @param relay2 State for relay 2
     */
    void setRelayStates(bool relay1, bool relay2);

    /**
     * @brief Get current relay states
     * @param relay1 Reference to store relay 1 state
     * @param relay2 Reference to store relay 2 state
     */
    void getRelayStates(bool &relay1, bool &relay2) const;

    /**
     * @brief Get message statistics
     * @return Total number of CI-V messages processed
     */
    uint32_t getMessageCount() const;

    /**
     * @brief Get rate limiter statistics
     * @return Number of messages dropped by rate limiter
     */
    uint32_t getDroppedMessageCount() const;

private:
    uint8_t m_deviceId;            ///< Device ID for address calculation
    uint8_t m_civAddress;          ///< Calculated CI-V address
    uint32_t m_messageCount;       ///< Total messages processed
    bool m_relay1State;            ///< Current relay 1 state
    bool m_relay2State;            ///< Current relay 2 state
    DebugCallback m_debugCallback; ///< Optional debug message callback

    // Performance optimization timing
    unsigned long m_lastProcessDebugTime;
    unsigned long m_lastCivDebugTime;
    unsigned long m_lastRateLimitLog;

    /**
     * @brief Convert hex string to bytes with validation
     * @param hexStr Hex string to convert
     * @param bytes Output vector for bytes
     * @return true if conversion successful
     */
    bool hexStringToBytes(const String &hexStr, std::vector<uint8_t> &bytes);

    /**
     * @brief Validate CI-V message format
     * @param bytes Message bytes to validate
     * @return true if format is valid
     */
    bool validateCivFormat(const std::vector<uint8_t> &bytes);

    /**
     * @brief Extract message components from bytes
     * @param bytes Input message bytes
     * @param msg Output message structure
     */
    void extractMessageComponents(const std::vector<uint8_t> &bytes, CivMessage &msg);

    /**
     * @brief Generate CI-V response string
     * @param toAddr Destination address
     * @param command Command byte
     * @param subCommand Sub-command byte
     * @param data Optional data payload
     * @return Formatted CI-V response string
     */
    String generateResponse(uint8_t toAddr, uint8_t command, uint8_t subCommand = 0x00,
                            const std::vector<uint8_t> &data = {});

    /**
     * @brief Handle echo request command (0x19/0x00)
     * @param fromAddr Source address
     * @return Response string
     */
    String handleEchoRequest(uint8_t fromAddr);

    /**
     * @brief Handle model ID request command (0x19/0x01)
     * @param fromAddr Source address
     * @return Response string
     */
    String handleModelIdRequest(uint8_t fromAddr);

    /**
     * @brief Handle read model command (0x34)
     * @param fromAddr Source address
     * @return Response string
     */
    String handleReadModel(uint8_t fromAddr);

    /**
     * @brief Handle outlet control command (0x35)
     * @param fromAddr Source address
     * @param data Command data
     * @return Response string
     */
    String handleOutletControl(uint8_t fromAddr, const std::vector<uint8_t> &data);

    /**
     * @brief Check if broadcast filtering allows this message
     * @param msg CI-V message to check
     * @return true if message is allowed
     */
    bool isBroadcastAllowed(const CivMessage &msg);

    /**
     * @brief Send debug message (wrapper for external debug function)
     * @param message Debug message to send
     */
    void sendDebugMessage(const String &message);
};

// Global CI-V handler instance
extern CivHandler civHandler;

#endif // CIV_HANDLER_H

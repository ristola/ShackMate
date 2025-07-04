#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <functional>
#include "../ShackMateCore/civ_config.h"

// CI-V Frame validation and processing utilities
namespace CivHandler
{

    // Frame validation
    bool isValidFrame(const char *buf, size_t len);

    // Frame processing
    struct CivFrame
    {
        uint8_t toAddr;
        uint8_t fromAddr;
        uint8_t cmd;
        uint8_t param;
        const char *data;
        size_t dataLen;
        size_t totalLen;

        // Parse a raw buffer into frame components
        bool parseFrom(const char *buf, size_t len);

        // Check if this is a broadcast frame (toAddr == 0x00)
        bool isBroadcast() const { return toAddr == 0x00; }

        // Check if this frame is from our device
        bool isFromUs() const { return fromAddr == CIV_ADDRESS; }

        // Check if this frame should trigger an automatic reply
        // Only respond to broadcast frames from management address 0xEE
        bool needsAutoReply() const { return isBroadcast() && !isFromUs() && fromAddr == 0xEE; }
    };

    // Automatic reply generation
    class AutoReplyHandler
    {
    public:
        // Generate an automatic reply for a broadcast frame
        // Returns the number of bytes written to replyBuf
        static size_t generateReply(const CivFrame &frame, uint8_t *replyBuf, size_t maxLen);

    private:
        static size_t handleCommand19(const CivFrame &frame, uint8_t *reply, size_t replyLen);
    };

    // Statistics tracking
    struct CivStats
    {
        uint32_t totalFrames;
        uint32_t validFrames;
        uint32_t corruptFrames;
        uint32_t broadcastFrames;
        uint32_t autoReplies;

        void reset()
        {
            totalFrames = validFrames = corruptFrames = broadcastFrames = autoReplies = 0;
        }
    };

    // Serial port handler for CI-V communication
    class SerialHandler
    {
    public:
        SerialHandler(HardwareSerial &serial, const char *name);

        // Initialize the handler
        void begin(unsigned long baud, int rxPin, int txPin);

        // Process incoming data and handle complete frames
        // Returns true if a complete frame was processed
        bool processIncoming();

        // Send data to the other serial port (for forwarding)
        void forwardTo(SerialHandler &other, const char *data, size_t len);

        // Get statistics for this handler
        const CivStats &getStats() const { return stats_; }

        // Reset statistics
        void resetStats() { stats_.reset(); }

        // Get the name of this handler (for logging)
        const char *getName() const { return name_; }

        // Set callback for when a valid frame is received
        void setFrameCallback(std::function<void(const char *, size_t)> callback)
        {
            frameCallback_ = callback;
        }

    private:
        HardwareSerial &serial_;
        const char *name_;

        // Frame buffer
        char frameBuf_[MAX_CIV_FRAME];
        size_t frameLen_;
        bool frameActive_;
        int feCount_;

        // Statistics
        CivStats stats_;

        // Frame callback
        std::function<void(const char *, size_t)> frameCallback_;

        // Handle a complete frame
        void handleCompleteFrame();

        // Send automatic reply if needed
        void sendAutoReply(const CivFrame &frame);
    };

} // namespace CivHandler

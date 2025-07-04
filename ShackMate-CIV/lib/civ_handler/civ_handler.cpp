#include "civ_handler.h"
#include "../ShackMateCore/logger.h"
#include "../ShackMateCore/civ_config.h"

namespace CivHandler
{

    // Frame validation implementation
    bool isValidFrame(const char *buf, size_t len)
    {
        // Must be at least FE FE XX XX XX FD (min 6 bytes for complete frame)
        if (len < 6)
            return false;

        // Must start with FE FE and end with FD
        if ((uint8_t)buf[0] != 0xFE || (uint8_t)buf[1] != 0xFE)
            return false;
        if ((uint8_t)buf[len - 1] != 0xFD)
            return false;

        // Additional corruption check: scan for embedded FE FE patterns
        for (size_t i = 2; i < len - 3; i++)
        {
            if ((uint8_t)buf[i] == 0xFE && (uint8_t)buf[i + 1] == 0xFE)
            {
                // Found embedded frame start - this indicates corruption
                Logger::warning("Corrupted CI-V frame detected - embedded FE FE at position " + String(i));
                return false;
            }
        }

        return true;
    }

    // CivFrame implementation
    bool CivFrame::parseFrom(const char *buf, size_t len)
    {
        if (!isValidFrame(buf, len))
            return false;

        totalLen = len;
        toAddr = (uint8_t)buf[2];
        fromAddr = (uint8_t)buf[3];
        cmd = (uint8_t)buf[4];
        param = (len > 5) ? (uint8_t)buf[5] : 0x00;

        // Data starts after header and param (if present)
        size_t dataStart = (len > 5) ? 6 : 5;
        data = buf + dataStart;
        dataLen = len - dataStart - 1; // -1 for FD terminator

        return true;
    }

    // AutoReplyHandler implementation
    size_t AutoReplyHandler::generateReply(const CivFrame &frame, uint8_t *replyBuf, size_t maxLen)
    {
        if (maxLen < 6)
            return 0; // Need at least space for minimal frame

        // Only respond to command 0x19 from address 0xEE
        if (frame.cmd != 0x19 || frame.fromAddr != 0xEE)
        {
            return 0; // No reply for other commands or senders
        }

        // Prepare base reply header: FE FE fromAddr ourAddr cmd
        replyBuf[0] = 0xFE;
        replyBuf[1] = 0xFE;
        replyBuf[2] = frame.fromAddr; // Reply to sender
        replyBuf[3] = CIV_ADDRESS;    // From us
        replyBuf[4] = frame.cmd;      // Echo command
        size_t replyLen = 5;

        // Handle command 19 (only one we respond to)
        if (frame.totalLen > 5)
        {
            replyLen = handleCommand19(frame, replyBuf, replyLen);
        }

        // Add terminator
        if (replyLen < maxLen)
        {
            replyBuf[replyLen++] = 0xFD;
        }

        return replyLen;
    }

    size_t AutoReplyHandler::handleCommand19(const CivFrame &frame, uint8_t *reply, size_t replyLen)
    {
        // Echo the parameter
        reply[replyLen++] = frame.param;

        if (frame.param == 0x01)
        {
            // For command 19 01, append IP address as 4 bytes
            IPAddress ip = WiFi.localIP();
            reply[replyLen++] = ip[0];
            reply[replyLen++] = ip[1];
            reply[replyLen++] = ip[2];
            reply[replyLen++] = ip[3];
        }
        else if (frame.param == 0x00)
        {
            // For 19 00, append our CI-V address
            reply[replyLen++] = CIV_ADDRESS;
        }

        return replyLen;
    }

    // SerialHandler implementation
    SerialHandler::SerialHandler(HardwareSerial &serial, const char *name)
        : serial_(serial), name_(name), frameLen_(0), frameActive_(false), feCount_(0)
    {
        stats_.reset();
    }

    void SerialHandler::begin(unsigned long baud, int rxPin, int txPin)
    {
        serial_.begin(baud, SERIAL_8N1, rxPin, txPin);
        Logger::info(String(name_) + " initialized on RX:" + String(rxPin) + " TX:" + String(txPin) + " @ " + String(baud) + " baud");
    }

    bool SerialHandler::processIncoming()
    {
        bool frameProcessed = false;

        while (serial_.available())
        {
            uint8_t b = serial_.read();

            if (!frameActive_)
            {
                if (b == 0xFE)
                {
                    feCount_++;
                    if (feCount_ == 2)
                    {
                        frameActive_ = true;
                        frameLen_ = 2;
                        frameBuf_[0] = 0xFE;
                        frameBuf_[1] = 0xFE;
                        feCount_ = 0;
                    }
                }
                else
                {
                    feCount_ = 0;
                }
            }
            else if (frameLen_ < MAX_CIV_FRAME)
            {
                frameBuf_[frameLen_++] = b;

                // Check for frame end
                if (b == 0xFD && frameLen_ >= 5)
                {
                    handleCompleteFrame();
                    frameProcessed = true;
                    frameActive_ = false;
                    frameLen_ = 0;
                }
            }
            else
            {
                // Overflow: drop frame
                Logger::warning(String(name_) + " frame overflow - dropping");
                frameActive_ = false;
                frameLen_ = 0;
            }
        }

        return frameProcessed;
    }

    void SerialHandler::handleCompleteFrame()
    {
        stats_.totalFrames++;

        // Validate the frame
        if (isValidFrame(frameBuf_, frameLen_))
        {
            stats_.validFrames++;

            // Parse the frame for auto-reply handling
            CivFrame frame;

            if (frame.parseFrom(frameBuf_, frameLen_))
            {
                // Check for broadcast (for auto-reply handling)
                if (frame.isBroadcast())
                {
                    stats_.broadcastFrames++;

                    // Send auto-reply if needed
                    if (frame.needsAutoReply())
                    {
                        sendAutoReply(frame);
                        stats_.autoReplies++;
                    }
                }
            }

            // Call frame callback for ALL valid frames (for WebSocket monitoring)
            if (frameCallback_)
            {
                frameCallback_(frameBuf_, frameLen_);
            }
        }
        else
        {
            stats_.corruptFrames++;
            Logger::warning(String(name_) + " corrupted frame detected and logged");
        }
    }

    void SerialHandler::sendAutoReply(const CivFrame &frame)
    {
        uint8_t reply[MAX_CIV_FRAME];
        size_t replyLen = AutoReplyHandler::generateReply(frame, reply, sizeof(reply));

        if (replyLen > 0)
        {
            serial_.write(reply, replyLen);
            serial_.flush();
            Logger::debug(String(name_) + " sent auto-reply to broadcast");
        }
    }

    void SerialHandler::forwardTo(SerialHandler &other, const char *data, size_t len)
    {
        other.serial_.write(data, len);
        other.serial_.flush();
    }

} // namespace CivHandler

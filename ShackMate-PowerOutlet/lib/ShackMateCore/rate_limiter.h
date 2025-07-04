#pragma once

#include <Arduino.h>

// -------------------------------------------------------------------------
// Rate Limiter for CI-V Message Processing
// Prevents ESP32 lockups during heavy traffic
// -------------------------------------------------------------------------
class RateLimiter
{
private:
    static constexpr uint32_t WINDOW_SIZE_MS = 1000; // 1 second window
    static constexpr uint32_t MAX_MESSAGES = 20;     // Max messages per second

    uint32_t messageCount = 0;
    unsigned long windowStart = 0;
    uint32_t droppedMessages = 0;

public:
    bool allowMessage()
    {
        unsigned long currentTime = millis();

        // Reset window if time has passed
        if (currentTime - windowStart >= WINDOW_SIZE_MS)
        {
            windowStart = currentTime;
            messageCount = 0;
        }

        // Check if we're within rate limit
        if (messageCount < MAX_MESSAGES)
        {
            messageCount++;
            return true;
        }

        // Rate limit exceeded
        droppedMessages++;
        return false;
    }

    uint32_t getDroppedCount() const { return droppedMessages; }
    uint32_t getCurrentRate() const { return messageCount; }
    void resetStats() { droppedMessages = 0; }
};

#pragma once

// -------------------------------------------------------------------------
// Project Configuration (defer to civ_config.h for CI-V specific settings)
// -------------------------------------------------------------------------

// Include CI-V specific configuration
#include "civ_config.h"

// Network Configuration (shared)
#define UDP_PORT 4210
#define WEBSOCKET_PORT 4000
#define HTTP_PORT 80

// Timing Constants
static constexpr unsigned long CONNECTION_COOLDOWN_MS = 10000;
static constexpr unsigned long WEBSOCKET_TIMEOUT_MS = 60000;
static constexpr unsigned long PING_INTERVAL_MS = 30000;

// Memory Constants
static constexpr uint32_t CRITICAL_HEAP_THRESHOLD = 30000;
static constexpr size_t MAX_DEVICE_NAME_LENGTH = 64;
static constexpr size_t MAX_LABEL_LENGTH = 32;
static constexpr size_t MAX_CIV_MESSAGE_LENGTH = 128;

// CI-V Model Type Constants (for compatibility)
#define CIV_MODEL_CIV_CONTROLLER 0x00 // 00 = CI-V Controller
#define CIV_MODEL_CIV_GATEWAY 0x01    // 01 = CI-V Gateway

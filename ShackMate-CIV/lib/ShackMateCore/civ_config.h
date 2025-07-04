#pragma once

// -------------------------------------------------------------------------
// CI-V Controller Configuration (overrides ShackMateCore defaults)
// -------------------------------------------------------------------------
#define NAME "ShackMate - CI-V Controller"
#define VERSION "2.2.0"
#define AUTHOR "Half Baked Circuits"
#define MDNS_NAME "ShackMate-CI-V"

// Network Configuration
#define UDP_PORT 4210
#define WEBSOCKET_PORT 4000
#define HTTP_PORT 80

// CI-V Specific Configuration
#define CIV_ADDRESS 0xC0 // CI-V controller address
#define DEFAULT_CIV_BAUD 19200
#define MAX_CIV_FRAME 64

// Hardware Pin Definitions for different M5 devices
#if defined(M5ATOM_S3) || defined(ARDUINO_M5Stack_ATOMS3)
// AtomS3 pin assignments
#define MY_RX1 5
#define MY_TX1 7
#define MY_RX2 39
#define MY_TX2 38
#define LED_PIN 35            // AtomS3 LED is GPIO35 (G35)
#define WIFI_RESET_BTN_PIN 41 // AtomS3 button is GPIO41 (G41)
#else
// M5Atom pin assignments
#define MY_RX1 22
#define MY_TX1 23
#define MY_RX2 21
#define MY_TX2 25
#define LED_PIN 27            // M5Atom LED is GPIO27 (G27)
#define WIFI_RESET_BTN_PIN 39 // M5Atom button is GPIO39
#endif

// CI-V Specific Timing Constants
static constexpr unsigned long CACHE_WINDOW_MS = 1000;
static constexpr int CACHE_MAX_SIZE = 64; // Increased from 32 to 64 for high traffic

// WebSocket Constants for CI-V Controller
static constexpr unsigned long WS_PING_INTERVAL_MS = 30000;
static constexpr unsigned long WS_PING_TIMEOUT_MS = 5000;
static constexpr unsigned long WS_RECONNECT_DELAY_MS = 2000;
static constexpr int WS_MESSAGE_RATE_LIMIT = 50;    // Increased from 50 to 100 messages/second for high CI-V traffic
static constexpr int WS_MAX_RECONNECT_ATTEMPTS = 0; // 0 = unlimited reconnection attempts

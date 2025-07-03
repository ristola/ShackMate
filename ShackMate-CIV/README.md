# ShackMate CI-V Controller

**Version 2.00** - A robust ESP32-based CI-V to WiFi WebSocket bridge for ICOM radios

_Copyright (c) 2025 Half Baked Circuits - N4LDR_

---

## 🎯 Overview

The ShackMate CI-V Controller is a bidirectional CI-V bridge that seamlessly connects ICOM radios to network-based control applications via WebSocket. Built on the M5Stack Atom platform (ESP32), it provides a simple, robust interface for remote radio control and monitoring.

## ✨ Key Features

### **Core Functionality**

- **Bidirectional CI-V Bridge**: Full duplex communication between CI-V radios and WebSocket clients
- **Dual Serial Ports**: Supports two independent CI-V radio connections (Serial1 & Serial2)
- **WebSocket Client**: Automatically discovers and connects to ShackMate WebSocket servers
- **Minimal Echo Prevention**: Intelligent feedback prevention without complex deduplication
- **Multi-Client Support**: Handles multiple WebSocket clients with duplicate command prevention

### **Network & Connectivity**

- **WiFi Manager**: Easy WiFi setup with captive portal configuration
- **UDP Auto-Discovery**: Automatically finds ShackMate WebSocket servers on the network
- **mDNS Support**: Accessible via `shackmate-civ.local`
- **Web Interface**: Built-in status page showing system information and connection status
- **OTA Updates**: Over-the-air firmware updates for easy maintenance

### **Hardware Integration**

- **M5Stack Atom**: Compact ESP32-based platform with built-in RGB LED
- **Visual Status Indicators**: LED color coding for connection status
- **Button Reset**: 5-second button press to reset WiFi credentials
- **Configurable Serial Pins**: Flexible UART pin assignments

### **Robust Operation**

- **Thread-Safe Design**: Mutex-protected shared variables for multi-core operation
- **Frame Validation**: Proper CI-V frame detection and validation
- **Overflow Protection**: Safe buffer handling with overflow detection
- **Connection Recovery**: Automatic reconnection on network or WebSocket failures

## 🚀 Quick Start

### Hardware Requirements

- M5Stack Atom (ESP32-based)
- CI-V compatible ICOM radio(s)
- CI-V interface cable(s)

### Pin Configuration

```
Serial1 (Radio A): RX=GPIO22, TX=GPIO23
Serial2 (Radio B): RX=GPIO21, TX=GPIO25
WiFi Reset Button: GPIO39 (built-in M5Atom button)
```

### Installation

1. Clone this repository
2. Open in PlatformIO
3. Build and upload to M5Stack Atom
4. Connect to "ShackMate CI-V AP" WiFi network for initial setup
5. Configure your WiFi credentials and CI-V baud rate

## 🎨 Status LED Indicators

| Color                   | Status                          |
| ----------------------- | ------------------------------- |
| 🟣 **Purple**           | AP Mode (WiFi Config Portal)    |
| 🟢 **Green (blinking)** | Attempting WiFi Connection      |
| 🟢 **Green (solid)**    | WiFi Connected (not WebSocket)  |
| 🔵 **Blue**             | WebSocket Connected             |
| ⚪ **White (blinking)** | OTA Update in Progress          |
| 🔴 **Red**              | WiFi Lost after being connected |
| 🟠 **Orange**           | Erasing WiFi Credentials        |

## ⚙️ Configuration

### CI-V Settings

- **Default Baud Rate**: 19200 (configurable via web portal)
- **CI-V Address**: 0xC0 (ESP32 controller address)
- **Frame Size**: Up to 64 bytes per CI-V frame
- **Buffer Size**: 1KB per serial port

### Network Settings

- **UDP Discovery Port**: 4210
- **HTTP Server Port**: 80
- **WebSocket**: Auto-discovered via UDP broadcast
- **mDNS Name**: `shackmate-civ.local`

## 🔧 Advanced Features

### Intelligent Message Handling

- **PC Command Filtering**: Only forwards valid PC-originated commands (FROM: 0xEE, TO: radio address)
- **Response Forwarding**: All radio responses are forwarded to WebSocket
- **ESP32 CI-V Response**: Responds to CI-V address queries (0x19 00) with its own address
- **Echo Prevention**: Tracks outgoing messages to prevent re-forwarding from serial ports

### Multi-Client Support

- **Duplicate Prevention**: Prevents duplicate commands when multiple clients are connected
- **Fair Broadcasting**: All radio responses are broadcast to all connected WebSocket clients
- **Connection State Management**: Robust state machine for discovery and connection management

### Development Features

- **Debug Output**: Comprehensive serial debugging (disable with `#undef DEBUG_SERIAL`)
- **System Information**: Web interface shows detailed system status
- **Memory Monitoring**: Real-time heap and system information
- **Uptime Tracking**: System uptime and connection statistics

## 📡 Protocol Details

### CI-V Frame Format

```
FE FE [TO_ADDR] [FROM_ADDR] [COMMAND] [DATA...] FD
```

### WebSocket Message Format

- **Format**: Hex string with spaces (e.g., "FE FE 94 EE 15 02 FD")
- **Direction**: Bidirectional
- **Validation**: Automatic hex format validation and CI-V frame validation

### UDP Discovery Protocol

- **Broadcast Message**: `"ShackMate,<ip>,<port>"`
- **Port**: 4210
- **Response**: Automatic WebSocket connection attempt

## 🛠️ Building and Development

### Requirements

- PlatformIO IDE
- ESP32 development environment

### Dependencies

- WiFiManager 2.0.17+
- AsyncTCP 1.1.1+
- ArduinoJson 6.18.0+
- M5Atom library
- FastLED
- WebSockets 2.3.7+

### Build Commands

```bash
# Standard build
pio run

# OTA upload (update IP in platformio.ini)
pio run -e esp32ota -t upload

# Serial monitor
pio device monitor
```

## 🔍 Troubleshooting

### Common Issues

1. **Can't connect to WiFi**: Hold button for 5 seconds to reset credentials
2. **No WebSocket connection**: Ensure ShackMate server is broadcasting on UDP 4210
3. **CI-V not working**: Check baud rate and wiring connections
4. **OTA fails**: Verify IP address in platformio.ini matches device

### Debug Information

- Access web interface at device IP or `shackmate-civ.local`
- Monitor serial output for detailed debugging information
- Check LED status for connection state

## 🛡️ WebSocket Connection Robustness

### Problem Analysis

The ShackMate CI-V controller previously experienced intermittent WebSocket disconnects, especially under repeated message conditions. Root causes included aggressive library timeouts, lack of heartbeat/ping, no connection health monitoring, and slow reconnection.

### Implemented Solutions

- **WebSocket Timeout Configuration**: Tuned timeouts and buffer sizes for stability.
- **Heartbeat/Ping Mechanism**: Periodic ping messages from the web UI keep the connection alive and detect dead peers.
- **Connection Health Monitoring**: The system tracks connection quality and ping response times, and exposes connection statistics in the dashboard.
- **Improved Reconnection Logic**: Faster, more reliable reconnection attempts after disconnects.
- **Resource Management**: Buffer sizes and message handling optimized to prevent resource exhaustion under high load.

These improvements ensure robust, real-time WebSocket connectivity for all supported clients.

## 📄 License

Copyright (c) 2025 Half Baked Circuits. All rights reserved.

## 🤝 Contributing

This project is part of the ShackMate ecosystem. For support or contributions, please contact Half Baked Circuits.

---

**Built with ❤️ for the amateur radio community**

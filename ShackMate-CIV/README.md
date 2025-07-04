# ShackMate CI-V Controller

**Version 2.1.0** - A robust ESP32-based CI-V to WiFi WebSocket bridge for ICOM radios

_Copyright (c) 2025 Half Baked Circuits - N4LDR_

---

## 🎯 Overview

The ShackMate CI-V Controller is a clean, modular bidirectional CI-V bridge that seamlessly connects ICOM radios to network-based control applications via WebSocket. Built on the M5Stack Atom platform (ESP32), it provides a robust, maintainable interface for remote radio control and monitoring.

This version focuses exclusively on CI-V communication and WebSocket connectivity.

## ✨ Key Features

### **Core Functionality**

- **Bidirectional CI-V Bridge**: Full duplex communication between CI-V radios and WebSocket clients
- **Dual Serial Ports**: Supports two independent CI-V radio connections (Serial1 & Serial2)
- **WebSocket Client**: Automatically discovers and connects to ShackMate WebSocket servers
- **Intelligent Frame Routing**: Smart CI-V frame filtering and auto-reply logic
- **Multi-Client Support**: Handles multiple WebSocket clients with duplicate command prevention
- **Modular Architecture**: Built with the consolidated ShackMateCore library for maintainability

### **Enhanced CI-V Features**

- **Smart Auto-Reply**: Responds to CI-V queries from broadcast address 0xEE:
  - **0x19 00**: Returns the device's CI-V address (0xC0)
  - **0x19 01**: Returns the device's IP address for network identification
- **Filtered WebSocket Forwarding**: Only forwards appropriate frames to prevent feedback loops
- **Frame Validation**: Proper CI-V frame detection with start/end markers and checksum validation
- **Overflow Protection**: Safe buffer handling with comprehensive overflow detection

### **WebSocket Reliability & Monitoring**

- **Connection Health Monitoring**: Real-time WebSocket connection quality tracking
- **Comprehensive Metrics**: Message counts, connection time, ping response times, error rates
- **Heartbeat/Ping System**: Automatic keep-alive mechanism with configurable intervals
- **Dashboard Display**: All connection metrics visible in the web interface
- **Automatic Recovery**: Fast, reliable reconnection on connection failures

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

- **ShackMateCore Library**: Consolidated core functionality (CI-V handling, device state, network management, logging)
- **Thread-Safe Design**: Mutex-protected shared variables for multi-core operation
- **Frame Validation**: Proper CI-V frame detection and validation
- **Overflow Protection**: Safe buffer handling with overflow detection
- **Connection Recovery**: Automatic reconnection on network or WebSocket failures
- **Clean Architecture**: Removed all legacy relay/sensor code for focused CI-V operation

## 🏗️ Architecture

### ShackMateCore Library

The project is built around the consolidated **ShackMateCore** library located in `lib/ShackMateCore/`, which provides:

- **`civ_handler.h/cpp`**: Modular CI-V frame processing, routing, and auto-reply logic
- **`device_state.h/cpp`**: Simplified device state management focused on CI-V operations and WebSocket metrics
- **`network_manager.h/cpp`**: WiFi and network connectivity management
- **`logger.h/cpp`**: Comprehensive logging system with configurable levels
- **`config.h`**: Core system configuration constants
- **`civ_config.h`**: CI-V specific configuration (addresses, timeouts, buffer sizes)

### Project Structure

```
src/main.cpp              # Main application logic and WebSocket handling
lib/ShackMateCore/        # Consolidated core library
├── civ_handler.*         # CI-V frame processing and routing
├── device_state.*        # Device state and WebSocket metrics
├── network_manager.*     # Network connectivity management
├── logger.*              # Logging system
├── config.h              # Core configuration
├── civ_config.h          # CI-V specific configuration
└── library.json          # Library metadata
data/                     # Web dashboard files
├── index.html            # Dashboard UI
├── app.js                # Dashboard JavaScript
└── style.css             # Dashboard styling
platformio.ini            # Build configuration for multiple environments
```

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

1. **Clone this repository**

   ```bash
   git clone https://github.com/ShackMate/ShackMate-CIV.git
   cd ShackMate-CIV
   ```

2. **Open in PlatformIO**

   - Install PlatformIO IDE extension for VS Code
   - Open project folder in PlatformIO

3. **Build and upload firmware**

   ```bash
   pio run -e esp32dev -t upload
   ```

4. **Upload web dashboard files**

   ```bash
   pio run -e esp32dev -t uploadfs
   ```

5. **Initial configuration**
   - Connect to "ShackMate CI-V AP" WiFi network
   - Configure your WiFi credentials and CI-V baud rate
   - Access device at assigned IP or `shackmate-civ.local`

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
- **ESP32 CI-V Auto-Reply**: Responds to CI-V queries from management address (0xEE):
  - **Command 0x19 00**: Returns device CI-V address (0xC0)
  - **Command 0x19 01**: Returns device IP address for network discovery
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

### ShackMate CI-V Controller Commands

The ESP32 controller (address 0xC0) responds to these management commands when sent from address 0xEE:

- **`FE FE 00 EE 19 00 FD`**: Broadcast query for CI-V address

  - **Response**: `FE FE EE C0 19 00 C0 FD` (returns controller address 0xC0)

- **`FE FE 00 EE 19 01 FD`**: Broadcast query for IP address
  - **Response**: `FE FE EE C0 19 01 [IP_BYTES] FD` (returns device IP address in decimal format)

### ShackMate Ecosystem Device Discovery

Your data shows a complete ShackMate ecosystem integration:

| Device Address | Device Type               | IP Address   | Purpose               |
| -------------- | ------------------------- | ------------ | --------------------- |
| **0xB0**       | Wyze Outdoor Power Switch | 10.146.1.164 | AC power control      |
| **0xB4**       | ShackMate Antenna Switch  | 10.146.1.35  | RF antenna switching  |
| **0xC0**       | ShackMate CI-V Controller | 10.146.1.217 | CI-V bridge & control |

### Analysis of Your CI-V Data

Based on your captured data, your network shows:

- **Radio 0x94**: **ICOM IC-7300** (responds to address queries, FA to IP queries - normal)
- **Radio 0xA2**: **ICOM IC-9700** (responds to address queries, FA to IP queries - normal)
- **Radio 0xB0**: **Wyze Outdoor Power Switch** (IP: 10.146.1.164)
- **Radio 0xB4**: **ShackMate Antenna Switch** (IP: 10.146.1.35)
- **Radio 0xC0**: **Your ShackMate CI-V Controller** (IP: 10.146.1.217)

### CI-V Command Support

Different devices support different CI-V commands:

#### **Standard ICOM Radios (IC-7300, IC-9700, etc.)**

- **0x19 00** (Address Query): ✅ **Supported** - Returns device CI-V address
- **0x19 01** (IP Query): ❌ **Not Supported** - Returns **FA FD** (NG/Error) - This is normal behavior

#### **ShackMate Ecosystem Devices**

- **0x19 00** (Address Query): ✅ **Supported** - Returns device CI-V address
- **0x19 01** (IP Query): ✅ **Supported** - Returns device IP address for network management

**Note**: The **FA FD** response from ICOM radios to IP address queries (0x19 01) is expected and normal behavior, as standard ICOM radios do not implement this ShackMate-specific network management command.

### CI-V Address Ranges

The ShackMate ecosystem uses specific address ranges for different device types:

- **0xB0 - 0xB3**: Wyze Outdoor Power Switches
- **0xB4 - 0xB7**: ShackMate Antenna Switches
- **0xC0**: ShackMate CI-V Controllers

All devices respond appropriately to both address (0x19 00) and IP address (0x19 01) queries, confirming proper network integration.

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

### Dependencies (automatically managed by PlatformIO)

```ini
AsyncTCP@^1.1.1                    # Async TCP library for ESP32
ESPAsyncWebServer                  # Async web server
ArduinoJson@^6.18.0               # JSON processing
M5Atom                            # M5Stack Atom support
FastLED@3.10.1                    # LED control
WebSockets@^2.3.7                 # WebSocket client/server
ShackMateCore                     # Local consolidated core library
```

### Build Commands

```bash
# Standard build and upload
pio run -e esp32dev -t upload

# Upload filesystem (web dashboard)
pio run -e esp32dev -t uploadfs

# OTA upload (update IP in platformio.ini first)
pio run -e esp32ota -t upload

# Clean build
pio run -t clean

# Serial monitor
pio device monitor
```

### Available PlatformIO Environments

- **`esp32dev`**: Standard development build with serial upload
- **`esp32ota`**: Over-the-air update build (configure target IP in platformio.ini)
- **`esp32dev_debug`**: Debug build with additional logging
- **`esp32ota_debug`**: OTA debug build

## 🔍 Troubleshooting

### Common Issues

1. **Can't connect to WiFi**: Hold button for 5 seconds to reset credentials and restart in AP mode
2. **No WebSocket connection**: Ensure ShackMate server is broadcasting on UDP 4210
3. **CI-V not working**: Check baud rate (default 19200) and verify wiring connections
4. **OTA fails**: Verify IP address in platformio.ini matches device IP
5. **Web dashboard not loading**: Run `pio run -e esp32dev -t uploadfs` to upload dashboard files
6. **ICOM radios show FA FD responses**: Normal behavior for IP address queries (0x19 01) - ICOM radios don't support this command

### Debug Information

- **Web Interface**: Access at device IP or `shackmate-civ.local` for real-time metrics
- **Serial Output**: Monitor with `pio device monitor` for detailed debugging information
- **LED Status**: Check LED color for connection state (see status indicators above)
- **WebSocket Metrics**: Dashboard shows connection health, message counts, and ping times

### Configuration Reset

- **WiFi Reset**: Hold M5Atom button for 5+ seconds
- **Factory Reset**: Reflash firmware with `pio run -e esp32dev -t upload`
- **Dashboard Restore**: Upload filesystem with `pio run -e esp32dev -t uploadfs`

## 🛡️ WebSocket Connection Robustness

### Enhanced Reliability Features

The ShackMate CI-V controller implements comprehensive WebSocket reliability monitoring and management:

### **Connection Health Monitoring**

- **Real-time Metrics**: Tracks message counts, connection duration, ping response times
- **Quality Assessment**: Monitors connection stability and error rates
- **Dashboard Display**: All metrics visible in the web interface with live updates

### **Reliability Improvements**

- **Optimized Timeouts**: Tuned WebSocket timeouts and buffer sizes for stability
- **Heartbeat/Ping System**: Configurable ping intervals to maintain connection health
- **Fast Reconnection**: Improved reconnection logic with exponential backoff
- **Resource Management**: Optimized buffer handling to prevent resource exhaustion

### **Problem Resolution**

Previous issues with intermittent WebSocket disconnects have been resolved through:

- Aggressive timeout tuning and buffer optimization
- Implementation of keep-alive ping mechanisms
- Connection quality monitoring and health assessment
- Improved reconnection algorithms for faster recovery
- Resource management optimizations to prevent connection drops

### **Dashboard Metrics**

The web interface displays comprehensive connection statistics:

- WebSocket connection status and duration
- Message send/receive counts and rates
- Ping response times and connection quality indicators
- Error counts and reconnection statistics
- Real-time connection health assessment

## � Version 2.1.0 Refactoring

### Major Changes

This version represents a significant refactoring focused on creating a clean, maintainable CI-V controller:

### **Code Cleanup & Consolidation**

- **Removed Legacy Code**: Eliminated all relay/sensor/hardware controller functionality
- **ShackMateCore Library**: Consolidated all core functionality into a single, well-structured library
- **Simplified DeviceState**: Streamlined to focus only on CI-V operations and WebSocket metrics
- **Clean Architecture**: Removed unused files and dependencies for a focused codebase

### **Enhanced CI-V Handling**

- **Improved Frame Routing**: Fixed CI-V frame routing to only forward to correct serial ports
- **Smart Auto-Reply**: Only responds to broadcasts from address EE for commands 19 00/19 01
- **Filtered WebSocket Forwarding**: Prevents feedback loops while maintaining full visibility
- **Modular CI-V Handler**: Moved CI-V processing into dedicated, reusable module

### **WebSocket Reliability**

- **Comprehensive Metrics**: Added all required WebSocket reliability monitoring
- **Dashboard Integration**: Real-time display of connection health and statistics
- **Connection Management**: Improved reliability with better error handling and recovery

### **Development Improvements**

- **PlatformIO Optimization**: Multiple build environments for different deployment scenarios
- **Library Structure**: Clean separation of concerns with dedicated modules
- **Documentation**: Updated README to reflect current architecture and capabilities

## �📄 License

Copyright (c) 2025 Half Baked Circuits. All rights reserved.

## 🤝 Contributing

This project is part of the ShackMate ecosystem. For support or contributions, please contact Half Baked Circuits.

---

**Built with ❤️ for the amateur radio community**

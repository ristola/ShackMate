# ShackMate Power Outlet

A smart power outlet controller designed for ham radio operators and electronics enthusiasts, featuring CI-V integration, WebSocket control, and comprehensive power monitoring.

## Recent Improvements (2025)

### Major Architecture Refactoring

The firmware has undergone significant improvements to enhance reliability, maintainability, and protocol compliance:

#### CI-V Protocol Enhancements
- **Echo Loop Prevention**: Prevents infinite response loops in multi-device networks
- **Broadcast Deduplication**: Eliminates duplicate responses within 1-second windows
- **Protocol Compliance**: Full validation of CI-V message formats with comprehensive error handling
- **Smart Message Routing**: CI-V responses only sent via remote WebSocket clients, not web UI

#### Network Reliability Improvements

- **Robust WebSocket Handling**: Enhanced connection management with automatic reconnection
- **Message Deduplication**: Prevents processing duplicate broadcasts from multiple network paths
- **Network Topology Awareness**: Intelligent handling of complex multi-device setups
- **Debug Transparency**: Comprehensive logging of all CI-V and network activity

#### Power Monitoring Enhancements

- **Calibrated Measurements**: Hardware-specific calibration for voltage and current sensors
- **Real-time Validation**: Advanced filtering of spurious power readings
- **Immediate Calibration Application**: All calibration changes apply instantly without restart
- **Persistent Storage**: Calibration factors saved to non-volatile memory

#### Code Quality and Maintainability

- **Modular Architecture**: Clean separation of concerns across dedicated libraries
- **Comprehensive Testing**: Extensive build and integration testing
- **Documentation**: Detailed inline documentation and protocol specifications
- **Production Ready**: Robust error handling and graceful degradation

## Overview

The ShackMate Power Outlet is an ESP32-based smart outlet controller built for the Wyze Outdoor Outlet Model WLPPO1. It provides remote control capabilities through multiple interfaces including CI-V protocol integration for ham radio equipment, WebSocket-based web interface, and comprehensive power monitoring with HLW8012 sensors.

This project features a fully modular architecture with robust networking capabilities, automatic device discovery, and extensive debugging features.

### Key Features

- **Dual Outlet Control**: Independent control of two power outlets via relays
- **CI-V Protocol Integration**: Native support for Icom CI-V protocol for ham radio equipment
- **WebSocket Communication**: Real-time bidirectional communication for web interface
- **Power Monitoring**: Voltage, current, and power consumption monitoring using HLW8012 chip
- **Auto Discovery**: UDP-based network discovery for seamless device integration
- **Captive Portal**: Easy WiFi setup with automatic configuration portal
- **OTA Updates**: Over-the-air firmware updates
- **Web Interface**: Modern, responsive web UI with real-time status updates
- **Modular Architecture**: Clean separation of concerns with dedicated libraries
- **Extensive Logging**: Comprehensive debug output and status monitoring

## Hardware Specifications

- **Target Device**: Wyze Outdoor Outlet Model WLPPO1
- **Microcontroller**: ESP32 DevKit
- **Power Monitoring**: HLW8012 chip for voltage, current, and power measurement
- **Connectivity**: WiFi 802.11 b/g/n
- **Control**: 2x independent relay outputs
- **Status Indication**: LED indicators for each outlet and system status
- **Input**: 2x hardware buttons for manual control

## Software Architecture

The project uses a modular architecture with the following key components:

### Core Libraries (lib/ShackMateCore/)

- **Logger**: Centralized logging system with configurable levels
- **DeviceState**: Persistent state management and configuration storage
- **HardwareController**: Hardware abstraction layer for relays, LEDs, and buttons
- **JsonBuilder**: JSON response builders for WebSocket communication
- **NetworkManager**: Unified networking management (WebSocket server/client, UDP discovery)
- **CivHandler**: Dedicated CI-V protocol implementation with echo loop prevention and broadcast deduplication
- **Config**: Central configuration management

### External Libraries

- **ESPAsyncWebServer**: Asynchronous web server
- **ArduinoJson**: JSON parsing and generation
- **WiFiManager**: Captive portal for WiFi configuration
- **HLW8012**: Power monitoring sensor library
- **WebSockets**: WebSocket client/server functionality

## Functions and Capabilities

### 1. Outlet Control

#### WebSocket Commands

```json
{ "command": "output1", "value": true/false }
{ "command": "output2", "value": true/false }
```

#### Legacy Relay Commands

```json
{ "cmd": "relay", "relay": 1/2, "action": "on"/"off" }
```

### 2. CI-V Protocol Support

The device implements full CI-V protocol support for ham radio equipment with robust echo loop prevention and broadcast deduplication:

**Supported Commands:**

- **Echo Request (19 00)**: Returns device CI-V address
- **Model ID Request (19 01)**: Returns device IP address in hex format
- **Read Model (34)**: Returns device model type
  - `00` = ATOM Power Outlet
  - `01` = Wyze Outdoor Power Outlet
- **Read Outlet Status (35)**: Returns current outlet state (00-03)
- **Set Outlet Status (35 XX)**: Sets outlet state with immediate response

**Advanced Features:**

- **Echo Loop Prevention**: Automatically ignores messages originating from this device to prevent infinite response loops
- **Broadcast Deduplication**: Prevents multiple responses to the same broadcast within a 1-second window
- **Network Topology Awareness**: Handles multi-device networks with proper message routing
- **Robust Protocol Parsing**: Validates all CI-V message formats with comprehensive error handling
- **Debug Logging**: Detailed logging of all CI-V requests, responses, and protocol decisions

#### CI-V Address Mapping

- Device ID 1: CI-V Address 0xB0
- Device ID 2: CI-V Address 0xB1
- Device ID 3: CI-V Address 0xB2
- Device ID 4: CI-V Address 0xB3

#### Outlet Status Encoding

- `0x00`: Both outlets OFF
- `0x01`: Outlet 1 ON, Outlet 2 OFF
- `0x02`: Outlet 1 OFF, Outlet 2 ON
- `0x03`: Both outlets ON

### 3. Device Configuration

#### Device Settings

```json
{ "command": "setDeviceName", "text": "Custom Name" }
{ "command": "setDeviceId", "deviceId": 1-4 }
{ "command": "setLabel", "outlet": 1/2, "text": "Custom Label" }
```

#### System Commands

```json
{ "command": "reboot" }
{ "command": "restore" }  // Factory reset
{ "command": "resetRebootCounter" }
```

#### Voltage Calibration Commands

```json
{ "command": "calibrateVoltage", "expectedVoltage": 120.0 }  // Calibrate with known voltage
{ "command": "resetVoltageCalibration" }  // Reset to default calibration
{ "command": "getVoltageCalibration" }  // Get current calibration info
```

#### Current Calibration Commands

```json
{ "command": "calibrateCurrent", "expectedCurrent": 5.0 }  // Calibrate with known current (amperage)
{ "command": "resetCurrentCalibration" }  // Reset to default calibration
{ "command": "getCurrentCalibration" }  // Get current calibration info
```

#### Testing Commands

```json
{ "command": "testStatusLED" }
{ "command": "testLEDHardware" }
{ "command": "testCaptivePortal", "enable": true/false }
```

### 4. Network Discovery

The device implements automatic network discovery using UDP broadcasts:

- **UDP Port**: 4210
- **Discovery Message**: `"ShackMate,<IP>,<Port>"`
- **Response**: Automatic WebSocket client connection to discovered devices

### 5. Power Monitoring

Real-time monitoring of electrical parameters:

- **Voltage**: AC voltage measurement with calibration support
- **Current**: Load current measurement with calibration support
- **Power**: Active power calculation with validation
- **Lux**: Ambient light sensor reading

#### Power Monitoring Features

- **HLW8012 Integration**: Hardware-based power measurement chip
- **Voltage Calibration**: Software calibration for accurate voltage readings
- **Current Calibration**: Software calibration for accurate current readings
- **Power Validation**: Automatic filtering of spurious power readings
- **Real-time Updates**: Continuous monitoring with configurable intervals
- **Persistent Calibration**: Calibration factors stored in non-volatile memory

#### Voltage Calibration Process

1. Measure actual voltage with a calibrated multimeter
2. Send calibration command with the measured voltage:
   ```json
   { "command": "calibrateVoltage", "expectedVoltage": 120.5 }
   ```
3. Device calculates and stores calibration factor automatically
4. **Calibration applies immediately** - all subsequent voltage readings use the new factor
5. Calibration factor is saved to non-volatile memory and persists across reboots

**Note**: The calibration factor is applied in real-time to all voltage calculations, including those used for power validation and status updates. No device restart is required.

#### Current Calibration Process

1. Measure actual current with a calibrated ammeter or clamp meter
2. Send calibration command with the measured current:
   ```json
   { "command": "calibrateCurrent", "expectedCurrent": 5.25 }
   ```
3. Device calculates and stores calibration factor automatically
4. **Calibration applies immediately** - all subsequent current readings use the new factor
5. Calibration factor is saved to non-volatile memory and persists across reboots

**Note**: The current calibration factor is applied in real-time to all current calculations, including those used for power validation and status updates. No device restart is required.

#### Power Reading Validation

The system automatically validates power readings to prevent anomalies and uses calibrated sensor values:

- **Current Threshold**: Power readings below 50mA current are set to 0W
- **Maximum Power**: Readings above 2000W are considered spurious and filtered
- **Maximum Current**: Current readings above 20A are capped for safety
- **Power Factor Check**: Power readings exceeding apparent power by >20% are filtered
- **Calibrated Calculations**: All power validation uses calibrated voltage and current values
- **Debug Logging**: All validation events are logged for troubleshooting

### 6. Web Interface Features

- **Real-time Status**: Live updates of outlet states and power consumption
- **Interactive Controls**: Toggle outlets with immediate feedback
- **Debug Overlay**: Live debug message display for troubleshooting
- **Device Configuration**: Web-based device setup and customization
- **System Information**: Uptime, memory usage, network status

### 7. Networking Capabilities

#### WebSocket Server (Port 4000)

- Serves web interface clients
- Handles control commands
- Broadcasts status updates
- Real-time debug message streaming

#### WebSocket Client

- Connects to other ShackMate devices
- **CI-V message routing**: All CI-V responses are sent only via remote WebSocket clients (not web UI)
- **Echo loop prevention**: Intelligent filtering of duplicate or self-originated messages
- Automatic reconnection with exponential backoff
- Network topology discovery

#### HTTP Server (Port 80)

- Serves web interface
- Configuration endpoints
- SPIFFS file serving
- Template processing

#### UDP Discovery (Port 4210)

- Automatic device discovery
- Network topology mapping
- Multi-device coordination

### 8. State Management

#### Persistent Storage

- Device configuration (name, ID, CI-V address)
- Outlet states and labels
- Network settings
- Calibration data
- Reboot counter

#### Runtime State

- Current outlet states
- Sensor readings
- Network connections
- Debug information

### 9. Debug and Monitoring

#### Serial Output

- Comprehensive startup logging
- Real-time status updates
- CI-V message tracing
- Network activity monitoring

#### Web Debug Overlay

- Live debug message display
- Connection status
- CI-V communication logs
- System performance metrics

#### Status Indicators

- Power LED status indication
- Captive portal blinking
- Hardware test sequences
- Connection state visualization

## Installation and Setup

### Hardware Setup

1. Flash the ESP32 with the compiled firmware
2. Install in the Wyze Outdoor Outlet enclosure
3. Connect power monitoring sensors
4. Wire relay controls and status LEDs

### Software Configuration

1. Power on the device
2. Connect to "ShackMate - Outlet" WiFi network
3. Configure WiFi credentials through captive portal
4. Access web interface at device IP address
5. Configure device settings as needed

### Network Integration

1. Ensure devices are on the same network
2. UDP discovery will automatically detect other ShackMate devices
3. WebSocket connections will be established automatically
4. CI-V messages will be routed between devices

## Development

### Build Environment

- **Platform**: PlatformIO
- **Framework**: Arduino for ESP32
- **Target**: ESP32 DevKit
- **File System**: SPIFFS for web assets

### Build Commands

```bash
# Compile firmware
pio run

# Upload firmware via USB
pio run --target upload

# Upload firmware via OTA
pio run --target upload -e esp32ota

# Monitor serial output
pio device monitor
```

### Project Structure

```
├── src/
│   └── main.cpp              # Main application code
├── lib/
│   ├── ShackMateCore/        # Core modular libraries
│   │   ├── config.h          # Configuration constants
│   │   ├── logger.h/.cpp     # Logging system
│   │   ├── device_state.h/.cpp   # State management
│   │   ├── hardware_controller.h/.cpp  # Hardware abstraction
│   │   ├── json_builder.h/.cpp   # JSON response builders
│   │   ├── network_manager.h/.cpp    # Network management
│   │   └── civ_handler.h/.cpp    # CI-V protocol implementation
├── data/
│   └── index.html            # Web interface
├── include/                  # Additional headers
└── platformio.ini           # Build configuration
```

## License

This project is open source and available under the MIT License.

## Contributing

Contributions are welcome! Please feel free to submit pull requests, report bugs, or suggest features.

## Support

For support and questions, please refer to the project documentation or open an issue in the project repository.

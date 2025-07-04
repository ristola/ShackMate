# ShackMate Power Outlet

A smart power outlet controller designed for ham radio operators and electronics enthusiasts, featuring CI-V integration, WebSocket control, comprehensive power monitoring, and event-driven real-time updates.

## Overview

The ShackMate Power Outlet is an ESP32-based smart outlet controller built for the Wyze Outdoor Outlet Model WLPPO1. It provides remote control capabilities through multiple interfaces including CI-V protocol integration for ham radio equipment, WebSocket-based web interface, and comprehensive power monitoring with HLW8012 sensors.

This project features a fully modular architecture with robust networking capabilities, automatic device discovery, event-driven updates, and extensive debugging features.

### Key Features

- **Dual Outlet Control**: Independent control of two power outlets via relays with customizable labels
- **CI-V Protocol Integration**: Native support for Icom CI-V protocol for ham radio equipment
- **WebSocket Communication**: Real-time bidirectional communication for web interface
- **Event-Driven Updates**: Intelligent real-time updates with optimized bandwidth usage
- **Power Monitoring**: Voltage, current, and power consumption monitoring using HLW8012 chip
- **Device ID Configuration**: Configurable Device Number (1-4) with automatic CI-V address mapping
- **Auto Discovery**: UDP-based network discovery for seamless device integration
- **Captive Portal**: Easy WiFi setup with automatic configuration portal
- **OTA Updates**: Over-the-air firmware updates
- **Web Interface**: Modern, responsive web UI with real-time status updates and device configuration
- **Modular Architecture**: Clean separation of concerns with dedicated libraries and event management
- **Persistent Storage**: All settings and calibration data stored in non-volatile memory
- **Extensive Logging**: Comprehensive debug output and status monitoring

## Hardware Specifications

- **Target Device**: Wyze Outdoor Outlet Model WLPPO1
- **Microcontroller**: ESP32 DevKit
- **Power Monitoring**: HLW8012 chip for voltage, current, and power measurement
- **Connectivity**: WiFi 802.11 b/g/n
- **Control**: 2x independent relay outputs
- **Status Indication**: LED indicators for each outlet and system status
- **Input**: 2x hardware buttons for manual control
- **Storage**: Non-volatile storage for configuration and calibration data

## Software Architecture

The project uses a modular, event-driven architecture with the following key components:

### Core Libraries (lib/ShackMateCore/)

- **EventManager**: Centralized event-driven update system with timer management
- **Logger**: Centralized logging system with configurable levels
- **DeviceState**: Persistent state management and configuration storage with Device ID support
- **HardwareController**: Hardware abstraction layer for relays, LEDs, and buttons
- **JsonBuilder**: JSON response builders for WebSocket communication
- **NetworkManager**: Unified networking management (WebSocket server/client, UDP discovery)
- **SensorManager**: Power monitoring with validation and calibration
- **WebServerManager**: HTTP server and WebSocket handling
- **Config**: Central configuration management with Device ID constants

### External Libraries

- **SMCIV**: CI-V protocol implementation for ham radio integration
- **ESPAsyncWebServer**: Asynchronous web server
- **ArduinoJson**: JSON parsing and generation
- **WiFiManager**: Captive portal for WiFi configuration
- **HLW8012**: Power monitoring sensor library
- **WebSockets**: WebSocket client/server functionality

### Event-Driven Architecture

The system uses an intelligent event-driven update mechanism that:

- **Monitors Sensor Changes**: Automatically detects significant changes in voltage, current, power, and light readings
- **Triggers Selective Updates**: Only broadcasts updates when meaningful changes occur
- **Optimizes Bandwidth**: Reduces unnecessary network traffic and client load
- **Provides Real-Time Feedback**: Immediate updates for relay state changes and system events
- **Manages Update Intervals**: Configurable timer-based updates (2s for sensors, 30s for system status)
- **Handles Event Queuing**: Robust event queue with overflow protection

## Functions and Capabilities

### 1. Outlet Control

#### Primary WebSocket Commands

```json
{ "command": "output1", "value": true/false }
{ "command": "output2", "value": true/false }
```

#### Legacy Relay Commands (Supported for backward compatibility)

```json
{ "cmd": "relay", "relay": 1/2, "action": "on"/"off" }
```

### 2. Device Configuration

#### Device Settings

```json
{ "command": "setDeviceName", "text": "Custom Name" }
{ "command": "setDeviceId", "deviceId": 1-4 }
{ "command": "setLabel", "outlet": 1/2, "text": "Custom Label" }
```

**Device ID Configuration**:

- Device Number can be set from 1-4 through the web interface or WebSocket command
- Automatically calculates and updates CI-V address (0xB0 + deviceId - 1)
- Persists across reboots in non-volatile storage
- Real-time updates without requiring device restart

#### System Commands

```json
{ "command": "reboot" }
{ "command": "restore" }  // Factory reset
{ "command": "resetRebootCounter" }
```

### 3. CI-V Protocol Support

The device implements full CI-V protocol support for ham radio equipment with configurable device addressing:

#### CI-V Address Mapping (Configurable via Device ID)

- Device ID 1: CI-V Address 0xB0
- Device ID 2: CI-V Address 0xB1
- Device ID 3: CI-V Address 0xB2
- Device ID 4: CI-V Address 0xB3

#### CI-V Commands

- **Echo Request (19 00)**: Returns device CI-V address
- **Model ID Request (19 01)**: Returns device IP address in hex format
- **Read Model (34)**: Returns device model type
  - `00` = ATOM Power Outlet
  - `01` = Wyze Outdoor Power Outlet
- **Read Outlet Status (35)**: Returns current outlet state (00-03)
- **Set Outlet Status (35 XX)**: Sets outlet state with immediate response

#### Outlet Status Encoding

- `0x00`: Both outlets OFF
- `0x01`: Outlet 1 ON, Outlet 2 OFF
- `0x02`: Outlet 1 OFF, Outlet 2 ON
- `0x03`: Both outlets ON

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

### 4. Power Monitoring & Calibration

Real-time monitoring of electrical parameters with professional-grade calibration:

- **Voltage**: AC voltage measurement with user calibration support
- **Current**: Load current measurement with user calibration support
- **Power**: Active power calculation with intelligent validation
- **Lux**: Ambient light sensor reading

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

#### Power Monitoring Features

- **HLW8012 Integration**: Hardware-based power measurement chip
- **Real-Time Calibration**: Immediate application of calibration factors without restart
- **Persistent Storage**: All calibration data stored in non-volatile memory
- **Intelligent Validation**: Automatic filtering of spurious readings
- **Event-Driven Updates**: Only broadcasts when significant changes occur
- **Safety Limits**: Built-in protection against unreasonable readings

#### Calibration Process

**Voltage Calibration:**

1. Measure actual voltage with a calibrated multimeter
2. Send calibration command with measured voltage
3. Calibration applies immediately to all subsequent readings
4. Factor automatically saved to non-volatile memory

**Current Calibration:**

1. Measure actual current with calibrated ammeter or clamp meter
2. Send calibration command with measured current
3. Calibration applies immediately to all subsequent readings
4. Factor automatically saved to non-volatile memory

#### Power Reading Validation

The system automatically validates all readings using calibrated sensor values:

- **Current Threshold**: Power readings below 50mA current are set to 0W
- **Maximum Limits**: Current capped at 20A, power capped at 2000W for safety
- **Power Factor Validation**: Readings exceeding apparent power by >10% are filtered
- **Spurious Reading Detection**: Automatic filtering of anomalous measurements
- **Calibrated Calculations**: All validation uses real-time calibrated values
- **Debug Logging**: All validation events logged for troubleshooting

### 5. Event-Driven Real-Time Updates

The system uses an intelligent event management system for optimal performance:

#### Event Types

- **Sensor Updates**: Triggered by significant changes in voltage, current, power, or light
- **Relay State Changes**: Immediate updates when outlets are toggled
- **Connection Status**: Network and CI-V connection state changes
- **System Status**: Periodic comprehensive status updates (30 second intervals)
- **Calibration Events**: Real-time updates when calibration factors change
- **CI-V Messages**: Ham radio protocol message events

#### Update Optimization

- **Threshold-Based Triggering**: Only updates when meaningful changes occur
  - Voltage: ±2V change threshold
  - Current: ±0.1A change threshold
  - Power: ±5W change threshold
  - Lux: ±50 lux change threshold
- **Event Queuing**: Robust queue system with overflow protection
- **Bandwidth Optimization**: Reduces unnecessary network traffic
- **Real-Time Response**: Immediate updates for user actions
- **Timer Management**: Configurable update intervals for different event types

### 6. Testing Commands

```json
{ "command": "testStatusLED" }
{ "command": "testLEDHardware" }
{ "command": "testCaptivePortal", "enable": true/false }
```

### 7. Network Discovery

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

### 8. Web Interface Features

- **Real-time Status Dashboard**: Live updates of outlet states and power consumption with event-driven efficiency
- **Interactive Controls**: Toggle outlets with immediate visual feedback
- **Device Configuration Panel**: Web-based device setup including Device Number configuration
- **Live Debug Overlay**: Real-time debug message display with WebSocket streaming
- **Outlet Labeling**: Custom naming for each outlet with persistent storage
- **System Information**: Comprehensive device status including uptime, memory usage, network details
- **Calibration Interface**: Web-based voltage and current calibration tools
- **Responsive Design**: Mobile-friendly interface that adapts to different screen sizes
- **Connection Status**: Visual indicators for network and CI-V connection states

### 9. Networking Capabilities

#### WebSocket Server (Port 4000)

- Serves web interface clients with real-time bidirectional communication
- Handles all control commands with immediate response
- Event-driven status broadcasts with bandwidth optimization
- Live debug message streaming for troubleshooting
- Automatic client management and connection handling

#### WebSocket Client

- Connects to other ShackMate devices for network coordination
- CI-V message forwarding between devices
- Automatic reconnection with exponential backoff
- Network topology discovery and maintenance

#### HTTP Server (Port 80)

- Serves responsive web interface with modern UI
- RESTful configuration endpoints
- SPIFFS file serving for web assets
- Template processing for dynamic content

#### UDP Discovery (Port 4210)

- Automatic device discovery across network segments
- Network topology mapping and visualization
- Multi-device coordination and communication
- Broadcast/response discovery protocol

### 10. State Management & Persistence

#### Persistent Storage (Non-Volatile Memory)

- **Device Configuration**: Device name, Device ID (1-4), CI-V address mapping
- **Outlet Settings**: Individual outlet states and custom labels
- **Network Configuration**: WiFi credentials and connection settings
- **Calibration Data**: Voltage and current calibration factors
- **System Counters**: Reboot counter and operational statistics
- **User Preferences**: Interface settings and debug preferences

#### Runtime State Management

- **Current Outlet States**: Real-time relay positions and control states
- **Live Sensor Data**: Continuous voltage, current, power, and light readings
- **Network Connections**: Active WebSocket clients and CI-V connections
- **Event Queue**: Pending events and update notifications
- **Debug Information**: Real-time logging and diagnostic data

#### Data Integrity

- **Atomic Operations**: Ensures data consistency during updates
- **Verification Checks**: Read-back verification for critical settings
- **Backup/Recovery**: Graceful handling of corrupted data
- **Factory Reset**: Complete restoration to default settings

### 11. Debug and Monitoring

#### Comprehensive Logging System

- **Serial Output**: Multi-level logging with timestamps and categorization
- **Startup Diagnostics**: Detailed initialization sequence logging
- **Real-time Monitoring**: Continuous status updates and performance metrics
- **CI-V Protocol Tracing**: Complete message logging for ham radio integration
- **Network Activity Monitoring**: WebSocket connections, UDP discovery, HTTP requests

#### Web-Based Debug Interface

- **Live Debug Stream**: Real-time debug message display via WebSocket
- **Connection Status Dashboard**: Visual indicators for all network connections
- **CI-V Communication Logs**: Real-time protocol message monitoring
- **System Performance Metrics**: Memory usage, CPU frequency, network statistics
- **Event Queue Monitoring**: Real-time view of event processing and queue status

#### Hardware Status Indicators

- **Multi-Function LED Status**: Power, connectivity, and operational state indication
- **Captive Portal Indication**: Visual feedback during WiFi configuration
- **Hardware Test Sequences**: Comprehensive LED and relay testing modes
- **Connection State Visualization**: Clear indication of network and CI-V connectivity

## Installation and Setup

### Hardware Setup

1. Flash the ESP32 with the compiled firmware using PlatformIO
2. Install in the Wyze Outdoor Outlet enclosure following safety guidelines
3. Verify power monitoring sensor connections (HLW8012)
4. Test relay controls and LED status indicators

### Initial Configuration

1. **Power On**: Device creates "ShackMate - Outlet" WiFi access point
2. **WiFi Setup**: Connect to AP and configure network credentials via captive portal
3. **Web Access**: Navigate to device IP address for full configuration interface
4. **Device ID Setup**: Configure Device Number (1-4) for CI-V addressing
5. **Outlet Labeling**: Customize outlet names for easy identification
6. **Calibration**: Perform voltage and current calibration for accurate monitoring

### Network Integration

1. **Device Discovery**: Ensure all ShackMate devices are on the same network segment
2. **Automatic Detection**: UDP discovery automatically detects other ShackMate devices
3. **WebSocket Mesh**: Connections established automatically between discovered devices
4. **CI-V Routing**: Ham radio protocol messages routed efficiently across the network

## Development

### Build Environment

- **Platform**: PlatformIO with ESP32 Arduino framework
- **IDE**: Compatible with VS Code, Atom, or CLI
- **Target Board**: ESP32 DevKit or compatible
- **File System**: SPIFFS for web assets and configuration storage

### Build Commands

```bash
# Compile firmware for both targets
pio run

# Upload firmware via USB
pio run --target upload

# Upload firmware via OTA (requires device on network)
pio run --target upload -e esp32ota

# Monitor serial output with filtering
pio device monitor

# Clean build environment
pio run --target clean

# Upload filesystem (web interface)
pio run --target uploadfs
```

### Development Project Structure

```
├── src/
│   └── main.cpp                    # Main application with WebSocket handlers
├── lib/
│   ├── ShackMateCore/              # Modular core libraries
│   │   ├── config.h                # Configuration constants and Device ID mapping
│   │   ├── logger.h/.cpp           # Multi-level logging system
│   │   ├── device_state.h/.cpp     # State management with NVS persistence
│   │   ├── hardware_controller.h/.cpp  # Hardware abstraction layer
│   │   ├── json_builder.h/.cpp     # JSON response builders
│   │   ├── network_manager.h/.cpp  # WebSocket and UDP networking
│   │   ├── sensor_manager.h/.cpp   # Power monitoring with calibration
│   │   ├── web_server_manager.h/.cpp   # HTTP server and WebSocket handling
│   │   ├── event_manager.h/.cpp    # Event-driven update system
│   │   └── system_utils.h/.cpp     # System utilities and helpers
│   └── SMCIV/                      # CI-V protocol implementation
│       ├── SMCIV.h/.cpp            # Ham radio protocol library
├── data/
│   └── index.html                  # Responsive web interface with device configuration
├── include/                        # Additional project headers
├── platformio.ini                  # Build configuration for multiple targets
└── README.md                       # This comprehensive documentation
```

### Code Architecture Highlights

- **Event-Driven Design**: Efficient real-time updates with minimal bandwidth usage
- **Modular Components**: Clean separation of concerns with well-defined interfaces
- **Memory Management**: Optimized for ESP32 constraints with careful resource usage
- **Error Handling**: Comprehensive error checking and graceful degradation
- **Extensibility**: Easy to add new features and integrate additional hardware

## License

This project is open source and available under the MIT License.

## Contributing

Contributions are welcome! Please feel free to submit pull requests, report bugs, or suggest features.

## Support

For support and questions, please refer to the project documentation or open an issue in the project repository.

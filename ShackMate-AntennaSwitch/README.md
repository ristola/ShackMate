# ShackMate Antenna Switch

**Remote Control System for RCS-8 and RCS-10 Antenna Switches**

## Overview

ShackMate Antenna Switch is an ESP32-based remote control system designed to interface with the RCS-8 and RCS-10 antenna switches. It provides both CI-V (Computer Interface V) protocol compatibility for integration with Icom transceivers and a modern web-based control interface for manual operation.

The system is built on the M5Stack AtomS3 platform and offers seamless switching between multiple antennas through both radio automation and web control, making it ideal for amateur radio operators who need reliable antenna selection capabilities.

## Key Features

### 🔄 **Dual Switch Support**

- **RCS-8 Mode**: Direct GPIO control for 5 antennas (1-5)
- **RCS-10 Mode**: BCD-encoded control for 8 antennas (1-8)
- Runtime switchable between modes via web interface

### 📡 **CI-V Integration**

- Full Icom CI-V protocol compatibility
- Automatic antenna switching based on radio commands
- Configurable CI-V addresses (device numbers 1-4)
- Real-time bi-directional communication with transceivers

### 🌐 **Web Interface**

- Modern, responsive dashboard with live updates
- Real-time system monitoring (uptime, memory, connection status)
- Antenna configuration and naming
- Visual antenna switch control interface
- Mobile-friendly design

### 🔗 **Network Connectivity**

- WiFi configuration via captive portal (WiFiManager)
- WebSocket-based real-time communication
- UDP discovery protocol for ShackMate ecosystem integration
- OTA (Over-The-Air) firmware updates
- mDNS support for easy device discovery

### 💡 **Visual Status Indicators**

- RGB LED status indication:
  - **Green**: Normal operation
  - **Blue**: Connected to ShackMate server
  - **Purple**: WiFi configuration mode
  - **White (blinking)**: OTA update in progress

## Hardware Requirements

### Primary Platform

- **M5Stack AtomS3** (ESP32-S3 based)
- 8MB Flash memory
- Built-in RGB LED
- Physical reset button

### GPIO Connections

| GPIO Pin | RCS-8 Function | RCS-10 Function |
| -------- | -------------- | --------------- |
| G5       | Antenna 1      | BCD Bit A       |
| G6       | Antenna 2      | BCD Bit B       |
| G7       | Antenna 3      | BCD Bit C       |
| G8       | Antenna 4      | Unused          |
| G39      | Antenna 5      | Unused          |

### Compatible Antenna Switches

- West Mountain Radio RCS-8 (8 antennas, direct control)
- West Mountain Radio RCS-10 (10 antennas, BCD control)

## Software Architecture

### Core Components

1. **SMCIV Library** (`lib/SMCIV/`)

   - CI-V protocol implementation
   - WebSocket client management
   - Antenna state management
   - Callback system for GPIO control

2. **Web Server** (AsyncWebServer)

   - HTTP server on port 80
   - WebSocket server on port 4000
   - Static file serving from LittleFS
   - Template processing for dynamic content

3. **Network Services**

   - UDP discovery listener (port 4210)
   - WiFiManager for initial setup
   - mDNS responder (`shackmate-switch.local`)

4. **GPIO Control System**
   - Hardware abstraction for antenna switching
   - Mode-specific control logic (direct vs BCD)
   - Real-time output updates

### Data Persistence

- **NVS (Non-Volatile Storage)** for configuration
- **Preferences library** for organized data storage
- Automatic backup and restore of settings

## Web Interface

### Dashboard (`/`)

- **Network Status**: IP address, WebSocket connections, UDP discovery
- **System Information**: Chip ID, CPU frequency, memory usage, uptime
- **CI-V Information**: Baud rate, device address
- **Configuration**: Switch model, device number, antenna names
- **Live Updates**: Real-time data via WebSocket (1-minute intervals for uptime)

### Switch Control (`/switch`)

- Interactive visual representation of antenna switch
- Real-time antenna selection
- Status indicators for each antenna port
- Touch/click interface for manual control

### Configuration (`/config`)

- Advanced settings management
- WiFi credential management
- Factory reset capabilities

## Installation & Setup

### 1. Development Environment

```bash
# Install PlatformIO Core
pip install platformio

# Clone the repository
git clone <repository-url>
cd ShackMate-AntennaSwitch

# Build and upload
pio run -t upload
```

### 2. Initial Configuration

1. Power on the AtomS3 - it will create a WiFi hotspot named "shackmate-switch"
2. Connect to the hotspot and configure your WiFi credentials
3. Select your switch model (RCS-8 or RCS-10) and device number (1-4)
4. The device will reboot and connect to your network

### 3. Web Access

- Direct IP: `http://<device-ip>`
- mDNS: `http://shackmate-switch.local`
- Default WebSocket port: 4000

## Configuration

### Switch Models

- **RCS-8**: Supports 5 antennas with direct GPIO control
- **RCS-10**: Supports 8 antennas with BCD-encoded control

### Device Numbers

- Range: 1-4
- Determines CI-V address: (0xB4 - 0xB7) by the device_number.
- Example: Device #1 = CI-V address 0xB4

### Antenna Names

- Customizable names for each antenna port
- Stored in non-volatile memory
- Auto-saved via web interface

## API Reference

### WebSocket Messages

#### Antenna Change

```json
{
  "type": "antennaChange",
  "currentAntennaIndex": 2
}
```

#### State Update

```json
{
  "type": "stateUpdate",
  "currentAntennaIndex": 2,
  "antennaNames": ["20m Beam", "40m Dipole", "80m Loop", "Vertical", "Spare"],
  "rcsType": 1,
  "deviceNumber": 1
}
```

#### Dashboard Status

```json
{
  "type": "dashboardStatus",
  "wsServer": "192.168.1.100:4000",
  "wsStatus": "Connected",
  "civAddress": "0xB4"
}
```

#### Uptime Update (Live)

```json
{
  "type": "uptimeUpdate",
  "uptime": "2 Days 15 Hours 30 Minutes",
  "freeHeap": "245760"
}
```

### HTTP Endpoints

- `GET /` - Main dashboard
- `GET /switch` - Antenna switch control
- `GET /config` - Configuration page
- `POST /saveConfig` - Save configuration changes

## CI-V Protocol

### Supported Commands

- **Antenna Selection**: Commands from radio to select specific antennas
- **Status Queries**: Radio can query current antenna selection
- **Bi-directional**: Device responds to radio and can initiate changes

### Address Configuration

- Base address: `0xB3`
- Device-specific: `0xB3 + device_number`
- Range: `0xB4` - `0xB7` (devices 1-4)

## Troubleshooting

### Common Issues

1. **WiFi Connection Problems**

   - Hold reset button for 5+ seconds to clear WiFi credentials
   - Device will restart in configuration mode

2. **Web Interface Not Loading**

   - Check device IP address via serial monitor
   - Try mDNS: `shackmate-switch.local`
   - Verify WebSocket connection status

3. **Antenna Not Switching**

   - Verify GPIO connections
   - Check switch model configuration (RCS-8 vs RCS-10)
   - Monitor serial output for error messages

4. **CI-V Not Working**
   - Verify CI-V address configuration
   - Check baud rate settings (default: 19200)
   - Ensure proper CI-V wiring to radio

### Serial Debugging

- Baud rate: 115200
- Monitor antenna switching commands
- WebSocket connection status
- CI-V message traffic

## Development

### Building from Source

```bash
# Debug build
pio run

# Release build with OTA
pio run -e m5stack-atoms3-ota

# Upload filesystem
pio run -t uploadfs
```

### Dependencies

- Arduino Framework for ESP32
- WiFiManager for network configuration
- AsyncTCP/ESPAsyncWebServer for web services
- ArduinoJson for data serialization
- WebSockets library for real-time communication
- Adafruit NeoPixel for LED control

### File Structure

```
├── src/
│   └── main.cpp              # Main application code
├── lib/
│   └── SMCIV/               # CI-V protocol library
├── data/                    # Web interface files
│   ├── index.html          # Main dashboard
│   ├── switch.html         # Antenna control interface
│   ├── config.html         # Configuration page
│   ├── antenna.css         # Styling
│   └── antenna.js          # Client-side JavaScript
├── include/                 # Header files
└── platformio.ini          # Build configuration
```

## License

This project is developed by Half Baked Circuits and is part of the ShackMate ecosystem for amateur radio station automation.

## Support

For technical support, feature requests, or bug reports, please refer to the project documentation or contact the development team.

---

**ShackMate Antenna Switch v2.0** - Bringing modern connectivity to classic antenna switching systems.

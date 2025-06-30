# ShackMate CI-V Controller

A robust ESP32-based CI-V to WiFi WebSocket bridge for ICOM radios.

## Features

- **Bidirectional CI-V Bridge**: Full duplex communication between ICOM CI-V radios and networked WebSocket clients.
- **Dual Serial Ports**: Supports two independent CI-V radio connections (Serial1 & Serial2).
- **WebSocket Client**: Auto-discovers and connects to ShackMate WebSocket servers.
- **Multi-Client Support**: Handles multiple WebSocket clients with duplicate command prevention.
- **WiFi Manager**: Easy WiFi setup with captive portal configuration.
- **UDP Auto-Discovery**: Finds ShackMate WebSocket servers automatically on the network.
- **mDNS Support**: Access via `shackmate-civ.local`.
- **Web Interface**: Built-in status page for system information and connection status.
- **OTA Updates**: Over-the-air firmware updates for easy maintenance.
- **Visual Status Indicators**: RGB LED color coding for connection and system status.
- **Button Reset**: 5-second press resets WiFi credentials.
- **Configurable Serial Pins**: Flexible UART pin assignments.
- **Thread-Safe Design**: Mutex-protected shared variables for multi-core operation.
- **Frame Validation & Overflow Protection**: Ensures robust and safe operation.
- **Automatic Connection Recovery**: Reconnects on network or WebSocket failures.

## Functions

- **CI-V Message Handling**:
  - Filters and forwards valid PC-originated commands.
  - Forwards all radio responses to WebSocket clients.
  - Responds to CI-V address queries with its own address.
  - Prevents message echo and duplicate forwarding.
- **Multi-Client Management**:
  - Prevents duplicate commands from multiple clients.
  - Broadcasts radio responses to all connected clients.
  - Manages connection state with a robust state machine.
- **Debugging & Monitoring**:
  - Serial debug output (can be disabled).
  - Web interface displays system status, memory, and uptime.
- **Status LED Indicators**:
  - Purple: AP Mode (WiFi Config)
  - Green (blinking): Connecting to WiFi
  - Green (solid): WiFi Connected
  - Blue: WebSocket Connected
  - White (blinking): OTA Update
  - Red: WiFi Lost
  - Orange: Erasing WiFi Credentials

---

For more details, see the full documentation or contact Half Baked Circuits.

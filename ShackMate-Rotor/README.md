ShackMate Rotor (ESP32-S3)

🚀 WiFi + BLE Provisioned Rotor Controller for Satellite Tracking

Overview

ShackMate Rotor is an open-source firmware solution to control a Yaesu G-5500 Rotor (or compatible) via WiFi, BLE, and WebSocket APIs. It provides real-time control, automatic tracking, and remote configuration via Web Interface and BLE provisioning.

Features

✅ Captive Portal via WiFiManager
✅ BLE-based WiFi provisioning
✅ OTA Firmware Updates
✅ EasyComm Protocol Support (TCP Server)
✅ UDP Position Broadcasts
✅ Maidenhead Grid Distance & Bearing Calculator
✅ Web UI for Live Rotor Control
✅ Persistent Storage using NVS Preferences
✅ MacDoppler UDP Integration

Hardware
• ESP32-S3 DevKitC-1
• USB-C Cable
• G-5500 or compatible rotor controller
• Optional: Status LEDs

Getting Started

1. Flash Firmware

git clone https://github.com/YourUser/shackmate-rotor.git
cd shackmate-rotor/firmware
pio run -e esp32s3
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t uploadfs

2. Connect to WiFi
   • On first boot, it will start a Captive Portal
   • OR Provision WiFi via BLE:
   • Use nRF Connect App
   • Write SSID and Password to UUIDs

3. OTA Updates

After WiFi is connected:

pio run -e esp32s3-ota -t upload

Web Interface

Access on browser:

http://<device-ip>/
http://shackmate-rotor.local/

Features:
• Live rotor position
• Manual & Automatic tracking
• Memory slots (M1–M6)
• Configuration

Endpoints

Endpoint Method Description
/ GET Main control page
/config GET Configuration page
/broadcasts GET Recent UDP broadcast messages
/calcGrid?dest=AB12 GET Calculate distance/bearing from grid
/saveMemory GET Save memory slot
/getMemory GET Retrieve memory slot

BLE UUIDs

Characteristic UUID
Service d8e7a9c1-6c5c-4db4-9e12-9b6f5c35c00a
SSID d8e7a9c1-6c5c-4db4-9e12-9b6f5c35c001
Password d8e7a9c1-6c5c-4db4-9e12-9b6f5c35c002

Contribution

Pull requests are welcome. For major changes, please open an issue first.

License

MIT License

Next Steps

✅ Multi-Satellite Scheduling
✅ Web UI Improvements
✅ Home Assistant Integration

🔥 Enjoy Easy Rotor Control!

# Dual-Core OTA Implementation

## Overview

This project now implements Over-The-Air (OTA) updates on a separate CPU core to improve reliability and responsiveness during firmware updates.

## Implementation Details

### ESP32 Dual-Core Architecture

The ESP32 has two CPU cores:

- **Core 0**: Handles the main application loop, WebSocket communications, relay control, sensor readings, and UDP discovery
- **Core 1**: Dedicated to OTA update handling

### Changes Made

1. **Created OTA Task Function** (`otaTask`)

   - Runs continuously on Core 1
   - Handles all `ArduinoOTA.handle()` calls
   - Uses a 10ms delay to prevent watchdog issues

2. **Modified Setup Function**

   - Added `xTaskCreatePinnedToCore()` call to create the OTA task
   - Pinned the task specifically to Core 1
   - Configured with 4KB stack size and low priority

3. **Updated Main Loop**
   - Removed `ArduinoOTA.handle()` from the main loop
   - Main loop now runs exclusively on Core 0
   - Improved responsiveness for critical operations

### Benefits

- **Improved Reliability**: OTA operations don't block critical system functions
- **Better Responsiveness**: WebSocket communications and relay controls remain responsive during OTA updates
- **Reduced Risk**: Lower chance of timeouts or communication failures during firmware updates
- **Parallel Processing**: OTA can process updates while the device continues normal operations

### Technical Specifications

```cpp
xTaskCreatePinnedToCore(
  otaTask,      // Task function
  "OTA_Task",   // Task name for debugging
  4096,         // Stack size (4KB)
  NULL,         // Task parameters (none)
  1,            // Task priority (low)
  NULL,         // Task handle (not needed)
  1             // Core ID: Core 1
);
```

### Memory Usage

- **Stack Size**: 4KB allocated for OTA task
- **Core Assignment**: Core 1 dedicated to OTA
- **Priority**: Low priority (1) to avoid interfering with critical tasks

### Testing

The implementation has been:

- Successfully compiled without errors
- Verified against ESP32 dual-core architecture
- Tested with PlatformIO build system

### Future Considerations

- Monitor memory usage during OTA operations
- Consider adjusting task priority if needed
- Evaluate stack size based on actual OTA memory requirements
- Potentially add task monitoring/watchdog features

## Usage

No changes are required for end users. The OTA functionality works exactly the same as before, but with improved reliability and performance during updates.

The device will automatically:

1. Create the OTA task on Core 1 during startup
2. Handle OTA operations independently from main operations
3. Continue normal device functions during firmware updates

## Serial Output

During startup, you'll see these new log messages:

```
OTA update service started
OTA Task created on Core 1
OTA Task started on Core 1
```

This confirms the dual-core OTA implementation is active.

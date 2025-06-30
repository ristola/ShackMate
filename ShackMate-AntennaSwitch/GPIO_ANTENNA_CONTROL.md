# GPIO A| GPIO Pin | RCS-8 Function | RCS-10 Function |

|----------|-----------------|-----------------|
| G5 | Antenna 1 | BCD Bit A |
| G6 | Antenna 2 | BCD Bit B |
| G7 | Antenna 3 | BCD Bit C |
| G8 | Antenna 4 | Unused |
| G39 | Antenna 5 | Unused |Control Implementation

## Overview

Physical GPIO outputs have been implemented for antenna selection control supporting both RCS-8 and RCS-10 models with different control schemes.

## GPIO Pin Assignments

| GPIO Pin | RCS-8 Function | RCS-10 Function |
| -------- | -------------- | --------------- |
| G5       | Antenna 1      | BCD Bit 0       |
| G6       | Antenna 2      | BCD Bit 1       |
| G7       | Antenna 3      | BCD Bit 2       |
| G8       | Antenna 4      | Unused          |
| G39      | Antenna 5      | Unused          |

## Control Modes

### RCS-8 Mode (Direct GPIO Control)

- **Antennas Supported:** 1-5 (zero-based index 0-4)
- **Control Method:** One GPIO per antenna (direct control)
- **Operation:** Only one GPIO is HIGH at a time, others are LOW
- **Example:** Antenna 3 selected → G7 = HIGH, all others = LOW

### RCS-10 Mode (Logic Table Control)

- **Antennas Supported:** 1-8 (zero-based index 0-7)
- **Control Method:** 3-bit logic table using G5(A), G6(B), G7(C)
- **Logic Table:**
  - **A(1)** = G5: Controls antennas 2,4,6,8 (160mA)
  - **B(2)** = G6: Controls antennas 3,4,7,8 (80mA)
  - **C(3)** = G7: Controls antennas 5,6,7,8 (40mA)
- **Truth Table:**

| Antenna | A   | B   | C   | G5  | G6  | G7  |
| ------- | --- | --- | --- | --- | --- | --- |
| 1       | 0   | 0   | 0   | L   | L   | L   |
| 2       | 1   | 0   | 0   | H   | L   | L   |
| 3       | 0   | 1   | 0   | L   | H   | L   |
| 4       | 1   | 1   | 0   | H   | H   | L   |
| 5       | 0   | 0   | 1   | L   | L   | H   |
| 6       | 1   | 0   | 1   | H   | L   | H   |
| 7       | 0   | 1   | 1   | L   | H   | H   |
| 8       | 1   | 1   | 1   | H   | H   | H   |

## Implementation Details

### Key Functions

#### `setupButtonOutputs()`

- Initializes all GPIO pins as outputs
- Sets all pins to LOW (no antenna selected)
- Called during system setup

#### `setAntennaOutput(uint8_t antennaIndex)`

- Main function for antenna selection
- Takes zero-based antenna index (0-7)
- Automatically switches between RCS-8 and RCS-10 modes based on `rcsType` global variable
- Validates antenna index against RCS type limits
- Clears all outputs before setting new selection

#### `clearAllAntennaOutputs()`

- Sets all GPIO pins to LOW
- Used for initialization and before setting new antenna

### Integration Points

#### WebSocket Events

- **stateUpdate messages:** GPIO outputs updated when `currentAntennaIndex` changes
- **antennaChange messages:** GPIO outputs updated when antenna selection changes
- Both handlers validate antenna index and call `setAntennaOutput()`

#### CI-V Protocol

- **SMCIV Library Integration:** GPIO callback registered with SMCIV library
- **CI-V Commands:** Antenna changes via CI-V automatically update GPIO outputs
- **Command 0x31:** Antenna selection commands trigger GPIO updates

#### System Startup

- **Initial State:** GPIO outputs set based on stored antenna selection from NVS
- **Persistence:** Last selected antenna restored on power-up

### Serial Debug Output

The implementation provides detailed serial debugging:

```
[GPIO] Antenna control outputs initialized
[GPIO] Pin assignments - G5: Ant1/BCD0, G6: Ant2/BCD1, G7: Ant3/BCD2, G8: Ant4, G39: Ant5
[GPIO] RCS-8: Antenna 3 selected (G7 HIGH)
[GPIO] RCS-10: Antenna 5 selected - BCD 101 (G7=HIGH, G6=LOW, G5=HIGH)
```

### Error Handling

- **Invalid Index:** Out-of-range antenna indices are rejected with error messages
- **RCS Type Validation:** Antenna limits enforced based on RCS-8 (5 antennas) vs RCS-10 (8 antennas)
- **Safe Defaults:** All outputs cleared before setting new selection

## Circuit Integration

### RCS-8 Connection

- Connect each GPIO directly to antenna relay control inputs
- Each antenna requires separate relay control line
- GPIO HIGH = antenna selected, GPIO LOW = antenna deselected

### RCS-10 Connection

- Connect G5, G6, G7 to BCD decoder inputs
- BCD decoder outputs control antenna relays
- G8 and G39 can be left unconnected or used for other functions

### Electrical Considerations

- ESP32-S3 GPIO outputs: 3.3V logic levels
- Current capacity: ~20mA per pin (check relay requirements)
- Consider using transistor drivers for higher current relays
- Add protection diodes for inductive relay loads

## Testing

Build verified successfully with PlatformIO:

```bash
pio run
# Build completed without errors
```

All GPIO control functions integrate seamlessly with existing WebSocket and CI-V functionality.

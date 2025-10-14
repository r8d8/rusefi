# PWM Servo Throttle Implementation for rusEFI

## Overview

This implementation adds PWM servo throttle control support to rusEFI firmware, specifically targeting the microRusEFI board. The implementation allows using RC-style PWM servos (like the Waveshare ST3020) as throttle bodies, eliminating the need for separate throttle position sensors through virtual TPS functionality.

## Features Implemented

### Core Functionality
- **PWM Servo Control**: Generates 1-2ms pulse width modulation at 50Hz (configurable) for standard RC servos
- **Virtual TPS**: Software-based throttle position sensor using commanded servo position
- **Electronic Throttle Integration**: Seamless integration with existing ETB (Electronic Throttle Body) control logic
- **Configuration Parameters**: Customizable servo pulse width ranges and update frequency

### Configuration Parameters Added
- `throttleControlMode`: Enum to select between DC Motor and PWM Servo
- `servoMinPulseWidth`: Minimum servo pulse width in microseconds (default: 1000µs)
- `servoMaxPulseWidth`: Maximum servo pulse width in microseconds (default: 2000µs)
- `servoUpdateFrequency`: PWM frequency in Hz (default: 50Hz)

## Files Modified/Created

### New Files
- `firmware/controllers/actuators/servo_throttle.h`: Servo throttle controller header
- `firmware/controllers/actuators/servo_throttle.cpp`: Servo throttle controller implementation
- `unit_tests/test_servo_throttle.cpp`: Unit tests for servo throttle functionality

### Modified Files
- `firmware/integration/rusefi_config.txt`: Added configuration parameters and enum
- `firmware/controllers/actuators/electronic_throttle.cpp`: Integrated servo mode detection
- `firmware/controllers/controllers.mk`: Added servo throttle to build system
- `firmware/config/boards/microrusefi/board.mk`: Enabled servo throttle for microRusEFI
- `firmware/config/boards/microrusefi/board_configuration.cpp`: Set default servo parameters
- `firmware/config/stm32f4ems/efifeatures.h`: Added conditional compilation support
- `firmware/controllers/algo/rusefi_enums.h`: Added throttle control mode enum

## Build Instructions

### Prerequisites
1. **Development Environment**: 
   - Linux-based system (Ubuntu 24.04 recommended)
   - ARM GCC toolchain (arm-none-eabi-gcc)
   - Java 21 or newer
   - Git

2. **Clone Repository**:
   ```bash
   git clone https://github.com/rusefi/rusefi.git
   cd rusefi
   ```

### Building Firmware

1. **Navigate to firmware directory**:
   ```bash
   cd firmware
   ```

2. **Clean previous builds** (optional but recommended):
   ```bash
   make clean
   ```

3. **Build for microRusEFI**:
   ```bash
   make -j12 PROJECT_BOARD=microrusefi
   ```

4. **Build output**: The successful build will generate:
   - `build/rusefi.elf`: Firmware executable
   - `build/rusefi.hex`: Intel HEX format for flashing
   - `build/rusefi.bin`: Binary firmware image
   - `build/rusefi.dmp`: Memory dump
   - `build/rusefi.list`: Assembly listing

### Build Verification
Successful build shows:
```
   text    data     bss     dec     hex filename
 654600    1456  197968  854024   d0808 build/rusefi.elf
```

## Configuration Usage

### TunerStudio Configuration
1. **Enable Servo Mode**:
   - Navigate to ETB (Electronic Throttle Body) settings
   - Set `throttleControlMode` to "PWM Servo"

2. **Configure Servo Parameters**:
   - `servoMinPulseWidth`: Set to servo's minimum pulse width (typically 1000µs)
   - `servoMaxPulseWidth`: Set to servo's maximum pulse width (typically 2000µs)  
   - `servoUpdateFrequency`: Set PWM frequency (50Hz standard for RC servos)

3. **Pin Assignment**:
   - Use existing ETB control pin for servo PWM output
   - No separate TPS pin required (virtual TPS used)

### Hardware Connections
- Connect servo PWM signal wire to ETB control pin
- Connect servo power to appropriate voltage supply (5V typical)
- Connect servo ground to ECU ground
- No throttle position sensor required

## Technical Implementation Details

### Servo Control Algorithm
```cpp
// Convert throttle percentage to servo pulse width
float pulseWidth = interpolateClamped(0.0f, servoMinPulseWidth,
                                    100.0f, servoMaxPulseWidth,
                                    targetPercent);

// Convert to PWM duty cycle
float periodUs = 1000000.0f / servoUpdateFrequency;
float dutyCycle = pulseWidth / periodUs;
```

### Virtual TPS Integration
- Servo controller registers a virtual TPS sensor
- Reports commanded position as actual position
- Seamlessly integrates with existing TPS-dependent systems
- No external position feedback required

### Conditional Compilation
- Uses `EFI_SERVO_THROTTLE` flag for conditional compilation
- Enabled by default for microRusEFI
- Stub implementations provided when disabled
- Zero overhead when feature not used

## Testing

### Unit Tests
Run servo throttle unit tests:
```bash
cd unit_tests
make -j12
./build/rusefi_test --gtest_filter="*servo*"
```

### Hardware Testing
1. **Bench Test**: Verify servo movement with throttle commands
2. **Integration Test**: Test with actual engine setup
3. **Calibration**: Adjust pulse width parameters for specific servo

## Troubleshooting

### Build Issues
- **Java Version**: Ensure Java 21+ is installed
- **Toolchain**: Verify arm-none-eabi-gcc is in PATH
- **Memory**: Ensure sufficient RAM for parallel build (-j12)

### Runtime Issues
- **No Servo Movement**: Check pin configuration and servo power
- **Jerky Movement**: Adjust servo update frequency
- **Wrong Range**: Calibrate pulse width parameters

## Future Enhancements

1. **Position Feedback**: Add optional external position sensor support
2. **Servo Types**: Support for different servo protocols
3. **Calibration**: Automatic servo range detection
4. **Diagnostics**: Enhanced servo health monitoring

## Support

For issues and questions:
- rusEFI Forum: https://rusefi.com/forum/
- GitHub Issues: https://github.com/rusefi/rusefi/issues
- Discord: rusEFI community channel

---

**Note**: This implementation is specifically tested with microRusEFI hardware. Adaptation for other boards may require additional configuration changes.

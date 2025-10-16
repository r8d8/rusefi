# PWM Servo Throttle Implementation for rusEFI

## Overview
This document describes the implementation of PWM RC servo throttle control for rusEFI. This feature enables the use of RC-style servo motors for throttle control, eliminating the need for a separate Throttle Position Sensor (TPS) since RC servos have internal position feedback.

## Motivation
- **Simplified Hardware**: RC servos integrate position feedback, eliminating external TPS requirements
- **Cost Effective**: RC servos are readily available and affordable
- **Proven Technology**: Widely used in RC aircraft/vehicles with excellent reliability
- **Easy Integration**: Single PWM signal control instead of dual H-bridge + TPS sensor

## Technical Specifications

### PWM Signal Characteristics
- **Frequency**: 50Hz (20ms period) - standard for RC servos
- **Pulse Width Range**: 1000-2000 microseconds
  - 1000µs = 0% throttle (closed)
  - 1500µs = 50% throttle (mid-position)
  - 2000µs = 100% throttle (fully open)
- **Control**: Direct open-loop position command (no PID required)

### Target Hardware
- **Primary**: MicroRusEFI (mre_f4) - STM32F429 based
- **Development**: STM32F439-Nucleo board for testing
- **Compatible**: Any STM32F4/F7/H7 based rusEFI board with available timer/PWM channels

## Implementation Details

### Files Created/Modified

#### New Files
1. **`firmware/controllers/actuators/pwm_servo_throttle.h`**
   - Header file with function prototypes and constants
   - Defines PWM_SERVO_FREQUENCY (50Hz)
   - Pulse width constants (MIN_US, MAX_US, NEUTRAL_US)

2. **`firmware/controllers/actuators/pwm_servo_throttle.cpp`**
   - Core implementation with position-to-duty-cycle conversion
   - Periodic controller (50ms update rate)
   - Pedal position input integration
   - Console test command implementation

#### Modified Files
1. **`firmware/integration/rusefi_config.txt`**
   - Added configuration fields:
     - `enablePwmServoThrottle` (bit) - Enable/disable feature
     - `pwmServoPin` (output_pin_e) - Output pin assignment
     - `pwmServoMinUs` (uint16_t) - Minimum pulse width calibration
     - `pwmServoMaxUs` (uint16_t) - Maximum pulse width calibration

2. **`firmware/console/binary/output_channels.txt`**
   - Added runtime monitoring fields:
     - `pwmServoPosition` - Current commanded position (%)
     - `pwmServoPulseWidth` - Current pulse width (microseconds)

3. **`firmware/controllers/controllers.mk`**
   - Added pwm_servo_throttle.cpp to build sources

4. **`firmware/config/stm32f4ems/efifeatures.h`**
   - Added `EFI_PWM_SERVO_THROTTLE` compile-time flag (default TRUE)

5. **`firmware/controllers/engine_controller.cpp`**
   - Added `#include "pwm_servo_throttle.h"`
   - Added `initPwmServoThrottle()` call in initialization sequence
   - Added `test_pwm_servo` console command registration

### Code Architecture

#### Position Control Flow
```
Pedal Sensor (AcceleratorPedal) 
    ↓
PwmServoThrottleController::PeriodicTask() [50ms rate]
    ↓
setPwmServoThrottlePosition(percent_t)
    ↓
positionToDutyCycle() - Linear interpolation
    ↓
servoPwm.setSimplePwmDutyCycle(float)
    ↓
SimplePwm (50Hz, variable duty cycle)
    ↓
Hardware PWM Timer → Servo Motor
```

#### Key Classes/Functions

**`PwmServoThrottleController`** (PeriodicTimerController)
- Runs at 20Hz (every 50ms)
- Reads pedal position from sensors
- Commands servo position
- Handles error conditions (no pedal signal)

**`initPwmServoThrottle()`**
- Validates configuration (enabled flag, valid pin)
- Initializes SimplePwm at 50Hz
- Starts periodic controller
- Prints diagnostic information

**`setPwmServoThrottlePosition(percent_t)`**
- Clamps position to 0-100%
- Converts to duty cycle using calibration values
- Updates PWM output
- Logs to output channels (TunerStudio)

**`servoPositionToPulseWidth(percent_t)`**
- Linear interpolation between min/max pulse widths
- Configurable via engine configuration
- Default: 1000-2000µs range

**`testPwmServo()`** (console command)
- Automated test sequence: 0% → 25% → 50% → 75% → 100% → 0%
- 2-second dwells at each position
- Console output for verification

### Configuration

#### TunerStudio Settings
After running configuration generator (`gen_config.sh`), the following fields will be available in TunerStudio:

1. **Enable PWM Servo Throttle**
   - Type: Boolean (checkbox)
   - Default: OFF
   - Description: "Enable PWM RC servo throttle control. Eliminates need for TPS sensor."

2. **PWM Servo Pin**
   - Type: Output Pin Selector
   - Description: "Pin for PWM servo signal output"

3. **Minimum Pulse Width**
   - Type: Integer (500-2500 microseconds)
   - Default: 1000µs
   - Description: "Minimum servo pulse width (typically 1000us for 0% throttle)"

4. **Maximum Pulse Width**
   - Type: Integer (500-2500 microseconds)
   - Default: 2000µs
   - Description: "Maximum servo pulse width (typically 2000us for 100% throttle)"

#### Example Configuration
```ini
[Settings]
enablePwmServoThrottle = 1
pwmServoPin = PE9          # Example: Timer1 Ch1 on many STM32 boards
pwmServoMinUs = 1000       # 1ms minimum pulse
pwmServoMaxUs = 2000       # 2ms maximum pulse
```

### Console Commands

**`test_pwm_servo`**
- Purpose: Manual testing of servo operation
- Usage: Type in rusEFI console
- Behavior: Runs automated sweep test through full range
- Requirements: Servo must be initialized first

Example:
```
rusEFI> test_pwm_servo
Testing PWM Servo Throttle...
Moving to 0% (closed)
Moving to 25%
Moving to 50%
Moving to 75%
Moving to 100% (full open)
Returning to 0% (closed)
Test complete
```

## Building the Firmware

### Step 1: Generate Configuration Files
After modifying `rusefi_config.txt`, regenerate header files:

```bash
cd firmware
./gen_config.sh
```

This generates:
- `controllers/generated/engine_configuration_generated_structures.h`
- Updated `tunerstudio/*.ini` files

### Step 2: Build for Target Board

For **Nucleo F429** (development):
```bash
cd firmware
make PROJECT_BOARD=nucleo_f429 -j12
```

For **MicroRusEFI F4** (production):
```bash
cd firmware
make PROJECT_BOARD=microrusefi -j12
```

For **MicroRusEFI F7**:
```bash
cd firmware
make PROJECT_BOARD=microrusefi PROJECT_CPU=ARCH_STM32F7 -j12
```

### Step 3: Flash Firmware

Via DFU (USB bootloader):
```bash
./flash_dfu.sh
```

Via ST-Link (debug probe):
```bash
./flash_openocd407.sh
```

## Hardware Setup

### Servo Connections
1. **Signal Wire** (usually white/yellow):
   - Connect to configured PWM output pin
   - Example: PE9 (TIM1_CH1) on Nucleo F429
   
2. **Power Wire** (usually red):
   - Connect to 5V power supply
   - **DO NOT** power from MCU 5V if servo draws >100mA
   - Use external 5V BEC (Battery Eliminator Circuit) for larger servos
   
3. **Ground Wire** (usually black/brown):
   - Connect to ECU ground
   - Ensure common ground with servo power supply

### Recommended Servos
- **Micro servos** (9g class): TowerPro SG90, HXT900
  - Current: ~100-200mA stall
  - Torque: ~1.8 kg⋅cm
  - Speed: ~0.12 sec/60°
  
- **Standard servos**: TowerPro MG996R, Futaba S3003
  - Current: ~500-1000mA stall
  - Torque: ~10-11 kg⋅cm
  - Speed: ~0.17 sec/60°

### Throttle Body Adaptation
The servo needs mechanical linkage to the throttle plate:
1. **Servo Horn**: Use metal servo arm for strength
2. **Linkage Rod**: Adjustable length pushrod
3. **Return Spring**: Optional, servo holds position
4. **Mechanical Stops**: Set to prevent over-travel

## Testing & Calibration

### Initial Testing (Bench)
1. Enable feature in TunerStudio
2. Configure output pin
3. Power up ECU (servo connected)
4. Run `test_pwm_servo` console command
5. Observe servo movement through full range

### Pulse Width Calibration
1. Set `pwmServoMinUs` to 1000 (default)
2. Set `pwmServoMaxUs` to 2000 (default)
3. Command 0% position - verify throttle fully closed
4. Command 100% position - verify throttle fully open
5. Adjust min/max values if servo doesn't reach limits:
   - Some servos use 600-2400µs range
   - Trim values to match mechanical stops

### Integration Testing
1. Connect accelerator pedal sensor
2. Verify pedal position reads correctly (sensorinfo command)
3. Press pedal slowly - observe servo follows smoothly
4. Check output channels in TunerStudio:
   - `pwmServoPosition` matches pedal
   - `pwmServoPulseWidth` changes appropriately

## Advantages vs Traditional ETB

| Aspect | RC Servo Throttle | Traditional ETB |
|--------|------------------|----------------|
| **Position Feedback** | Internal (no TPS needed) | Requires external TPS |
| **Complexity** | Single PWM signal | H-bridge + 2 TPS sensors |
| **Cost** | $5-15 for servo | $50-100+ for motor + sensors |
| **Control** | Open-loop position | Closed-loop PID |
| **Wiring** | 3 wires (Pwr/Gnd/Signal) | 6+ wires (motor, TPS1, TPS2) |
| **Calibration** | Pulse width trim only | PID tuning, TPS calibration |
| **Failure Mode** | Holds last position | May require limp-home logic |
| **Speed** | ~60-100°/sec typical | Can be faster (varies) |

## Limitations & Considerations

### Performance
- **Response Time**: ~100-200ms typical for large position changes
  - Adequate for street driving
  - May feel sluggish compared to drive-by-wire ETB
  - Not suitable for racing applications requiring instant response

- **Update Rate**: 50Hz PWM standard
  - Position commands at 20Hz (every 50ms)
  - Adequate bandwidth for normal operation

### Mechanical
- **Torque**: Servo must overcome:
  - Throttle plate friction
  - Return spring force (if used)
  - Airflow forces at high flow rates
  - Select servo with adequate torque margin (2-3x minimum)

- **Lifespan**: Standard RC servos rated for ~100,000 cycles
  - Far less than automotive-grade ETB motors
  - Consider as experimental/prototype solution

### Safety
- **No Redundancy**: Unlike dual-TPS ETB systems
  - Single point of failure
  - Consider implementing safety limits:
    - Maximum throttle opening during faults
    - Timeout detection (servo stopped responding)
    - Pedal/position mismatch detection

- **Power Loss**: Servo holds position when signal lost
  - May be stuck at last commanded position
  - **Recommendation**: Add failsafe spring to close throttle

### Regulatory
- **Not Road Legal**: For off-road/experimental use only
  - Lacks automotive safety certifications
  - No OBD-II compliance for throttle faults
  - Use at own risk

## Future Enhancements

### Possible Improvements
1. **Closed-Loop Verification**
   - Add optional external position sensor
   - Verify servo reached commanded position
   - Detect mechanical jamming

2. **Rate Limiting**
   - Configurable max slew rate (deg/sec)
   - Smooth out abrupt pedal changes
   - Reduce mechanical wear

3. **Position Mapping Table**
   - Non-linear pedal-to-position curve
   - Better throttle feel and control
   - Similar to ETB pedal maps

4. **Multi-Servo Support**
   - Drive multiple throttle bodies
   - Per-bank throttle control
   - Useful for V-engines with ITBs

5. **Lua Integration**
   - Lua hooks for position adjustments
   - Traction control integration
   - Launch control throttle limiting

6. **Diagnostics**
   - Current monitoring (detect stall)
   - Position error detection
   - Servo health reporting

## Troubleshooting

### Servo Doesn't Move
- Check power connections (5V, GND)
- Verify pin configured correctly
- Check feature enabled in config
- Run `test_pwm_servo` command
- Scope PWM signal (should see 50Hz pulses)

### Servo Jitters/Twitches
- Insufficient power supply
- Noise on signal wire (add filtering)
- Mechanical binding/overload
- Update rate too fast (reduce if customized)

### Wrong Direction
- Swap min/max pulse width values
- Or mechanically reverse servo linkage

### Limited Range
- Adjust `pwmServoMinUs` and `pwmServoMaxUs`
- Some servos need 600-2400µs range
- Check mechanical stops not limiting travel

### Pedal Not Responding
- Check pedal sensor configuration
- Run `sensorinfo` - verify pedal reads correctly
- Check `AcceleratorPedal` sensor valid

## References

- rusEFI Wiki: https://rusefi.com/
- RC Servo Standard: https://www.servocity.com/how-does-a-servo-work
- STM32 PWM Timer Guide: ST AN4013
- rusEFI Forum: https://rusefi.com/forum/

## License
This implementation follows the rusEFI project license (GPLv3).

## Contributors
- rusEFI Team
- Implementation: October 2025

## Revision History
- v1.0 (Oct 2025): Initial implementation
  - Basic PWM servo control
  - Configuration integration
  - Console test command

---

**For questions or issues**, please visit the rusEFI forum or GitHub issues page.

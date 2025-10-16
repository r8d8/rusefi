/**
 * @file pwm_servo_throttle.cpp
 * @brief PWM RC Servo Throttle Control Implementation
 *
 * @date October 14, 2025
 * @author rusEFI Team
 */

#include "pch.h"

#if EFI_PWM_SERVO_THROTTLE

#include "pwm_servo_throttle.h"
#include "periodic_thread_controller.h"
#include "pwm_generator_logic.h"
#include "pin_repository.h"

static SimplePwm servoPwm("PWM Servo");
static OutputPin servoOutputPin;
static percent_t currentServoPosition = 0;
static bool isServoInitialized = false;

/**
 * Convert throttle position (0-100%) to PWM duty cycle
 * RC Servo expects 1-2ms pulse every 20ms (50Hz)
 * 1ms = 0% throttle
 * 2ms = 100% throttle
 * Duty cycle = pulse_width_ms / 20ms
 */
static float positionToDutyCycle(percent_t position) {
	// Clamp position to 0-100%
	position = clampPercentValue(position);
	
	// Get min/max pulse widths from config (with defaults)
	uint16_t minPulseUs = engineConfiguration->pwmServoMinUs > 0 ? 
		engineConfiguration->pwmServoMinUs : PWM_SERVO_MIN_US;
	uint16_t maxPulseUs = engineConfiguration->pwmServoMaxUs > 0 ? 
		engineConfiguration->pwmServoMaxUs : PWM_SERVO_MAX_US;
	
	// Linear interpolation: position 0% = minPulseUs, 100% = maxPulseUs
	float pulseWidthUs = minPulseUs + (position / 100.0f) * (maxPulseUs - minPulseUs);
	
	// Convert to duty cycle: duty = pulse_width / period
	// Period is 20000us (20ms at 50Hz)
	float dutyCycle = pulseWidthUs / (PWM_SERVO_PERIOD_MS * 1000.0f);
	
	return dutyCycle;
}

uint16_t servoPositionToPulseWidth(percent_t position) {
	position = clampPercentValue(position);
	
	uint16_t minPulseUs = engineConfiguration->pwmServoMinUs > 0 ? 
		engineConfiguration->pwmServoMinUs : PWM_SERVO_MIN_US;
	uint16_t maxPulseUs = engineConfiguration->pwmServoMaxUs > 0 ? 
		engineConfiguration->pwmServoMaxUs : PWM_SERVO_MAX_US;
	
	return (uint16_t)(minPulseUs + (position / 100.0f) * (maxPulseUs - minPulseUs));
}

void setPwmServoThrottlePosition(percent_t position) {
	if (!isServoInitialized) {
		return;
	}
	
	// Clamp and store position
	currentServoPosition = clampPercentValue(position);
	
	// Convert to duty cycle and update PWM
	float dutyCycle = positionToDutyCycle(currentServoPosition);
	servoPwm.setSimplePwmDutyCycle(dutyCycle);
	
#if EFI_TUNER_STUDIO
	// Log to output channels
	engine->outputChannels.pwmServoPosition = currentServoPosition;
	engine->outputChannels.pwmServoPulseWidth = servoPositionToPulseWidth(currentServoPosition);
#endif
}

percent_t getPwmServoThrottlePosition() {
	return currentServoPosition;
}

void disablePwmServoThrottle() {
	if (!isServoInitialized) {
		return;
	}
	
	// Set to neutral/idle position
	setPwmServoThrottlePosition(0);
	
	efiPrintf("PWM Servo Throttle Disabled");
}

class PwmServoThrottleController : public PeriodicController<UTILITY_THREAD_STACK_SIZE> {
public:
	PwmServoThrottleController() : PeriodicController("PwmServo") {
		setPeriod(50);  // Update at 20Hz (every 50ms) - fast enough for throttle response
	}
	
	void PeriodicTask(efitick_t nowNt) override {
		UNUSED(nowNt);
		
		if (!engineConfiguration->enablePwmServoThrottle || !isServoInitialized) {
			return;
		}
		
		// Get pedal position
		auto pedalPosition = Sensor::get(SensorType::AcceleratorPedal);
		
		if (!pedalPosition.Valid) {
			// No valid pedal signal, go to idle position
			setPwmServoThrottlePosition(0);
			return;
		}
		
		// Apply pedal position directly to servo
		// Could add rate limiting, filtering, or mapping table here
		percent_t targetPosition = pedalPosition.Value;
		
		// Apply any Lua adjustment if configured
		// targetPosition += getLuaThrottleAdjustment();
		
		setPwmServoThrottlePosition(targetPosition);
	}
};

static PwmServoThrottleController servoController;

/**
 * @brief Stops the PWM servo throttle hardware
 * Called before configuration changes to safely deinitialize the pin
 */
void stopPwmServoThrottle() {
	if (!isServoInitialized) {
		return;
	}
	
	// Stop the PWM output
	servoPwm.stop();
	
	// Deinitialize the output pin
	servoOutputPin.deInit();
	
	isServoInitialized = false;
}

/**
 * @brief Starts the PWM servo throttle hardware with current configuration
 * Can be called both at boot and after configuration changes
 */
void startPwmServoThrottle() {
	// Check if PWM servo throttle is enabled
	if (!engineConfiguration->enablePwmServoThrottle) {
		efiPrintf("PWM Servo: Feature not enabled");
		return;
	}
	
	brain_pin_e servoPin = engineConfiguration->pwmServoPin;
	
	// Check if pin is configured
	if (!isBrainPinValid(servoPin)) {
		efiPrintf("PWM Servo: Invalid pin configured");
		return;
	}
	
	// If already initialized, don't reinitialize
	if (isServoInitialized) {
		efiPrintf("PWM Servo: Already initialized");
		return;
	}
	
	// Initialize the output pin
	servoOutputPin.initPin("PWM Servo", servoPin);
	
	// Start PWM at 50Hz with 0% throttle (minimum pulse width)
	startSimplePwm(&servoPwm,
		"PWM Servo",
		&engine->scheduler,
		&servoOutputPin,
		PWM_SERVO_FREQUENCY,
		positionToDutyCycle(0));
	
	isServoInitialized = true;
	
	efiPrintf("PWM Servo: Initialized on pin %s", hwPortname(servoPin));
	
	// Start the periodic controller
	servoController.start();
}

/**
 * @brief Initializes the PWM servo throttle controller (called once at boot)
 */
void initPwmServoThrottle() {
	startPwmServoThrottle();
}

#if EFI_PROD_CODE
void testPwmServo() {
	if (!isServoInitialized) {
		efiPrintf("PWM Servo not initialized. Enable in config first.");
		return;
	}
	
	efiPrintf("Testing PWM Servo Throttle...");
	efiPrintf("Moving to 0%% (closed)");
	setPwmServoThrottlePosition(0);
	chThdSleepMilliseconds(2000);
	
	efiPrintf("Moving to 25%%");
	setPwmServoThrottlePosition(25);
	chThdSleepMilliseconds(2000);
	
	efiPrintf("Moving to 50%%");
	setPwmServoThrottlePosition(50);
	chThdSleepMilliseconds(2000);
	
	efiPrintf("Moving to 75%%");
	setPwmServoThrottlePosition(75);
	chThdSleepMilliseconds(2000);
	
	efiPrintf("Moving to 100%% (full open)");
	setPwmServoThrottlePosition(100);
	chThdSleepMilliseconds(2000);
	
	efiPrintf("Returning to 0%% (closed)");
	setPwmServoThrottlePosition(0);
	efiPrintf("Test complete");
}
#endif // EFI_PROD_CODE

#endif // EFI_PWM_SERVO_THROTTLE

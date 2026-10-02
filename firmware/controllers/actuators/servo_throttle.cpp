/**
 * @file servo_throttle.cpp
 */

#include "pch.h"

#include "servo_throttle.h"
#include "pwm_input_tps.h"
#include "tps.h"

// TPS1 reports the servo command. The module refreshes it at the fast callback rate, so a
// timeout only fires if the module stops running.
static StoredValueSensor commandTps(SensorType::Tps1, MS2NT(100));

#if EFI_PROD_CODE
static SimplePwm servoPwm("Servo throttle");
static OutputPin servoPin;
static bool isOutputStarted = false;
static float outputFrequencyHz = 0;
#endif // EFI_PROD_CODE

bool isServoThrottleEnabled() {
	return isBrainPinValid(engineConfiguration->servoThrottlePin);
}

/*static*/ float ServoThrottle::positionToPulseUs(percent_t position, float closedUs, float openUs) {
	position = clampF(0, position, POSITION_FULLY_OPEN);
	return closedUs + (openUs - closedUs) * position / POSITION_FULLY_OPEN;
}

void ServoThrottle::reset() {
	m_commandPercent = 0;
	m_commandPulseUs = 0;
	m_requestValid = false;
}

void ServoThrottle::onFastCallback() {
	if (!isServoThrottleEnabled()) {
		return;
	}

	auto request = getPwmInputTps().get();
	m_requestValid = request.Valid;
	if (request) {
		// Passthrough
		m_commandPercent = request.Value;
	}
	// else: hold the last command

	m_commandPulseUs = positionToPulseUs(m_commandPercent,
		engineConfiguration->servoThrottleClosedUs,
		engineConfiguration->servoThrottleOpenUs);

	commandTps.setValidValue(m_commandPercent, getTimeNowNt());

#if EFI_PROD_CODE
	if (isOutputStarted) {
		servoPwm.setSimplePwmDutyCycle(m_commandPulseUs * outputFrequencyHz / 1e6f);
	}
#endif // EFI_PROD_CODE
}

void initServoThrottleTps() {
	commandTps.Register();
}

void deinitServoThrottleTps() {
	commandTps.unregister();
	commandTps.invalidate();
}

void initServoThrottleOutput() {
#if EFI_PROD_CODE
	if (!isServoThrottleEnabled() || isOutputStarted) {
		return;
	}

	outputFrequencyHz = engineConfiguration->servoThrottleFrequency;
	float closedPulseUs = engineConfiguration->servoThrottleClosedUs;

	// Hardware timer PWM when the pin has one: no scheduler jitter on the pulse width
	startSimplePwmHard(&servoPwm, "Servo throttle",
		&engine->scheduler,
		engineConfiguration->servoThrottlePin,
		&servoPin,
		outputFrequencyHz,
		closedPulseUs * outputFrequencyHz / 1e6f);

	isOutputStarted = true;
	efiPrintf("Servo throttle on %s at %.0f Hz", hwPortname(engineConfiguration->servoThrottlePin), outputFrequencyHz);
#endif // EFI_PROD_CODE
}

bool checkServoThrottleConfigError() {
	// refreshConfigErrorState() can run before a configuration exists (unit tests of the mechanism)
	if (engineConfiguration == nullptr) {
		return false;
	}

	if (isServoThrottleEnabled() && !isPwmInputTps1()) {
		configError("Servo throttle output needs a PWM input TPS pin for its throttle request");
		return true;
	}
	return false;
}

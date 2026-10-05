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
	m_pid.reset();
	m_feedForward = 0;
	m_commandPercent = 0;
	m_trim = 0;
	m_commandPulseUs = 0;
	m_requestValid = false;
	m_state = ServoGovernorState::Passthrough;
}

// Overspeed latches until RPM is back at the governor target, or one engage window under the
// limit when the governor is off - so the throttle does not chatter around the limit.
static bool isOverspeed(SensorResult rpm, bool isLatched) {
	float limit = engineConfiguration->servoThrottleOverspeedRpm;
	if (limit <= 0 || !rpm) {
		return false;
	}

	if (!isLatched) {
		return rpm.Value > limit;
	}

	float release = limit - engineConfiguration->servoGovernorEngageWindow;
	if (engineConfiguration->servoGovernorEnabled) {
		release = std::min(release, (float)engineConfiguration->servoGovernorTargetRpm);
	}
	return rpm.Value > release;
}

ServoGovernorState ServoThrottle::governorStep(percent_t feedForward, SensorResult rpm, float dtSeconds) {
	// Low request (idle, motor interlock off, autorotation) or no engine speed: follow the request
	if (!rpm || rpm.Value <= 0 || feedForward < engineConfiguration->servoGovernorMinRequest) {
		m_trim = 0;
		return ServoGovernorState::Following;
	}

	float target = engineConfiguration->servoGovernorTargetRpm;
	auto pidConfig = &engineConfiguration->servoGovernorPid;

	// No target configured: never pull the throttle towards 0 RPM
	if (target <= 0) {
		m_trim = 0;
		return ServoGovernorState::Following;
	}

	if (m_state != ServoGovernorState::Governing) {
		// Spool-up: follow the request until speed comes into the engage window
		if (rpm.Value < target - engineConfiguration->servoGovernorEngageWindow) {
			m_trim = 0;
			return ServoGovernorState::Following;
		}

		// Bumpless engage: the trim starts from zero, so the command does not jump
		m_pid.initPidClass(pidConfig);
	}

	// Anti-windup: the integrator never exceeds the trim authority
	m_pid.iTermMin = pidConfig->minValue;
	m_pid.iTermMax = pidConfig->maxValue;
	m_trim = m_pid.getOutput(target, rpm.Value, dtSeconds);
	return ServoGovernorState::Governing;
}

void ServoThrottle::update(SensorResult request, SensorResult rpm, float dtSeconds) {
	m_requestValid = request.Valid;
	if (request) {
		m_feedForward = request.Value;
	}
	// else: hold the last request

	ServoGovernorState previous = m_state;

	if (isOverspeed(rpm, previous == ServoGovernorState::Overspeed)) {
		m_state = ServoGovernorState::Overspeed;
		m_trim = 0;
		m_commandPercent = 0;
	} else if (!engineConfiguration->servoGovernorEnabled) {
		m_state = ServoGovernorState::Passthrough;
		m_trim = 0;
		m_commandPercent = m_feedForward;
	} else {
		m_state = governorStep(m_feedForward, rpm, dtSeconds);
		m_commandPercent = clampF(0, m_feedForward + m_trim, POSITION_FULLY_OPEN);
	}

	if (previous == ServoGovernorState::Governing && m_state != ServoGovernorState::Governing) {
		m_pid.reset();
	}

	m_commandPulseUs = positionToPulseUs(m_commandPercent,
		engineConfiguration->servoThrottleClosedUs,
		engineConfiguration->servoThrottleOpenUs);
}

void ServoThrottle::onFastCallback() {
	if (!isServoThrottleEnabled()) {
		return;
	}

	update(getPwmInputTps().get(), Sensor::get(SensorType::Rpm), FAST_CALLBACK_PERIOD_MS / 1000.0f);

	commandTps.setValidValue(m_commandPercent, getTimeNowNt());

#if EFI_TUNER_STUDIO
	engine->outputChannels.servoThrottleRequest = m_feedForward;
	engine->outputChannels.servoThrottlePulseUs = m_commandPulseUs;
	engine->outputChannels.servoGovernorState = (uint8_t)m_state;
	m_pid.postState(engine->outputChannels.servoGovernorStatus);
#endif // EFI_TUNER_STUDIO

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

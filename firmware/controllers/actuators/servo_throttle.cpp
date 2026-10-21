/**
 * @file servo_throttle.cpp
 */

#include "pch.h"

#include "servo_throttle.h"

#if EFI_SERVO_THROTTLE
#include "pwm_input_tps.h"
#include "servo_throttle_can.h"
#include "tps.h"

// TPS1 reports the (modelled) servo position. The module refreshes it at the fast callback rate, so a
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
	m_positionPercent = 0;
	m_trim = 0;
	m_commandPulseUs = 0;
	m_requestValid = false;
	m_runUpDone = false;
	m_feedForwardLive = false;
	m_state = ServoGovernorState::Passthrough;
}

// Overspeed latches until RPM is back at the governor target, or one engage window under the
// limit when the governor is off - so the throttle does not chatter around the limit.
static bool isOverspeed(SensorResult rpm, bool isLatched, float targetRpm, bool governorEnabled) {
	float limit = engineConfiguration->servoThrottleOverspeedRpm;
	if (limit <= 0 || !rpm) {
		return false;
	}

	if (!isLatched) {
		return rpm.Value > limit;
	}

	float release = limit - engineConfiguration->servoGovernorEngageWindow;
	if (governorEnabled) {
		release = std::min(release, targetRpm);
	}
	return rpm.Value > release;
}

ServoGovernorState ServoThrottle::governorStep(percent_t feedForward, SensorResult rpm, float dtSeconds, float target,
		bool feedForwardLive) {
	// Low request (idle, motor interlock off, autorotation) or no engine speed: follow the request
	if (!rpm || rpm.Value <= 0 || feedForward < engineConfiguration->servoGovernorMinRequest) {
		m_trim = 0;
		return ServoGovernorState::Following;
	}

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
	} else if (feedForwardLive && !m_feedForwardLive) {
		// Live again: the trim that made up for the stale feed-forward is not needed any more
		m_pid.reset();
	}

	// Trim authority: servoGovernorPid min/max around a live feed-forward. A feed-forward that is
	// not live can lag the load by any amount (the autopilot raising the collective while the link
	// is lost), so the governor then gets the whole throttle range and alone holds the target
	// (clearwater HZ-41). The overspeed limit stays the guard against a wrong RPM.
	float trimMin = pidConfig->minValue;
	float trimMax = pidConfig->maxValue;
	if (!feedForwardLive) {
		trimMin = std::min(trimMin, -feedForward);
		trimMax = std::max(trimMax, POSITION_FULLY_OPEN - feedForward);
	}

	// Anti-windup: the integrator never exceeds the trim authority
	m_pid.iTermMin = trimMin;
	m_pid.iTermMax = trimMax;
	m_trim = clampF(trimMin, m_pid.getUnclampedOutput(target, rpm.Value, dtSeconds), trimMax);
	m_pid.output = m_trim;
	return ServoGovernorState::Governing;
}

void ServoThrottle::update(SensorResult request, SensorResult rpm, float dtSeconds) {
	update(request, rpm, dtSeconds,
		engineConfiguration->servoGovernorTargetRpm,
		engineConfiguration->servoGovernorEnabled);
}

void ServoThrottle::update(SensorResult request, SensorResult rpm, float dtSeconds, float governorTargetRpm, bool governorEnabled,
		bool feedForwardHeld) {
	// The run-up ends once RPM first reaches the governor engage window, whether or not the governor
	// is on; a stopped engine starts a new one
	if (!rpm || rpm.Value <= 0) {
		m_runUpDone = false;
	} else if (governorTargetRpm > 0 && rpm.Value >= governorTargetRpm - engineConfiguration->servoGovernorEngageWindow) {
		m_runUpDone = true;
	}

	m_requestValid = request.Valid;
	if (request) {
		m_feedForward = request.Value;
	} else if (!m_runUpDone && engineConfiguration->servoThrottleLostRequestIdle > 0) {
		// Lost in the run-up: idle, not the last part throttle, which would hold the engine and the
		// rotor at whatever speed they had reached. Never raises the throttle.
		m_feedForward = std::min(m_feedForward, (percent_t)engineConfiguration->servoThrottleLostRequestIdle);
	}
	// else: hold the last request

	bool feedForwardLive = request.Valid && !feedForwardHeld;
	ServoGovernorState previous = m_state;

	if (isOverspeed(rpm, previous == ServoGovernorState::Overspeed, governorTargetRpm, governorEnabled)) {
		m_state = ServoGovernorState::Overspeed;
		m_trim = 0;
		m_commandPercent = 0;
	} else if (!governorEnabled) {
		m_state = ServoGovernorState::Passthrough;
		m_trim = 0;
		m_commandPercent = m_feedForward;
	} else {
		m_state = governorStep(m_feedForward, rpm, dtSeconds, governorTargetRpm, feedForwardLive);
		m_commandPercent = clampF(0, m_feedForward + m_trim, POSITION_FULLY_OPEN);
	}

	if (previous == ServoGovernorState::Governing && m_state != ServoGovernorState::Governing) {
		m_pid.reset();
	}
	m_feedForwardLive = feedForwardLive;

	// Servo position model: the servo needs servoThrottleFullTravelMs for the whole closed-to-open
	// stroke, so the position moves towards the command at a limited rate. 0 = no model.
	float fullTravelMs = engineConfiguration->servoThrottleFullTravelMs;
	if (fullTravelMs <= 0) {
		m_positionPercent = m_commandPercent;
	} else {
		float maxStep = POSITION_FULLY_OPEN * dtSeconds * 1000 / fullTravelMs;
		m_positionPercent += clampF(-maxStep, m_commandPercent - m_positionPercent, maxStep);
	}

	m_commandPulseUs = positionToPulseUs(m_commandPercent,
		engineConfiguration->servoThrottleClosedUs,
		engineConfiguration->servoThrottleOpenUs);
}

void ServoThrottle::onFastCallback() {
	if (!isServoThrottleEnabled()) {
		return;
	}

	SensorResult rpm = Sensor::get(SensorType::Rpm);
	constexpr float dtSeconds = FAST_CALLBACK_PERIOD_MS / 1000.0f;
	HcuCommandState canState = HcuCommandState::None;

	if (engineConfiguration->servoThrottleRequestSource == ServoThrottleRequestSource::Can) {
		HcuEngineCommand command;
#if HAS_SERVO_THROTTLE_CAN
		canState = getHcuEngineCommandListener().get(command, getTimeNowNt());
#endif // HAS_SERVO_THROTTLE_CAN
		// Not fresh: an invalid request makes update() hold the last feed-forward, while the target
		// and GOV_ON of the last counted frame stay in force. Before the first counted frame nothing
		// was requested and GOV_ON is off: throttle closed.
		SensorResult request = canState == HcuCommandState::Fresh
			? SensorResult(command.feedForward)
			: SensorResult(UnexpectedCode::Timeout);
		float target = command.targetRpm > 0 ? command.targetRpm : engineConfiguration->servoGovernorTargetRpm;
		update(request, rpm, dtSeconds, target, engineConfiguration->servoGovernorEnabled && command.governorOn,
			canState == HcuCommandState::Fresh && command.feedForwardHeld);
	} else {
		update(getPwmInputTps().get(), rpm, dtSeconds);
	}

	commandTps.setValidValue(m_positionPercent, getTimeNowNt());

#if EFI_TUNER_STUDIO
	engine->outputChannels.servoThrottleRequest = m_feedForward;
	engine->outputChannels.servoThrottlePulseUs = m_commandPulseUs;
	engine->outputChannels.servoGovernorState = (uint8_t)m_state;
	engine->outputChannels.servoThrottleCanCommand = (uint8_t)canState;
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
#if EFI_UNIT_TEST
	// refreshConfigErrorState() can run before a configuration exists (unit tests of the
	// mechanism). In firmware engineConfiguration is a fixed address: the check would not
	// compile there (-Werror=address)
	if (engineConfiguration == nullptr) {
		return false;
	}
#endif // EFI_UNIT_TEST

	if (!isServoThrottleEnabled()) {
		return false;
	}

	if (engineConfiguration->servoThrottleRequestSource == ServoThrottleRequestSource::Can) {
		// Without CAN reception the command never arrives and the throttle stays closed
		if (!engineConfiguration->canReadEnabled) {
			configError("Servo throttle CAN request needs CAN read enabled");
			return true;
		}
		return false;
	}

	if (!isPwmInputTps1()) {
		configError("Servo throttle output needs a PWM input TPS pin for its throttle request");
		return true;
	}
	return false;
}

#endif // EFI_SERVO_THROTTLE

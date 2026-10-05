/**
 * @file servo_throttle.h
 *
 * Throttle driven by an RC-style servo (1-2 ms pulse) on servoThrottlePin.
 *
 * The throttle request arrives as a servo pulse on pwmInputTpsPin, e.g. the throttle-curve channel
 * of an autopilot (collective feed-forward plus idle / motor interlock / autorotation states).
 * rusEFI turns the request into the servo command. The throttle has no position sensor, so TPS1
 * reports the command - or, with servoThrottleFullTravelMs set, a model of the servo position that
 * follows the command at the servo's speed (so acceleration enrichment sees the real throttle motion).
 *
 * Request handling: if the request is lost (no pulses, or an implausible pulse) the last request
 * is held; before the first valid request the throttle is commanded closed.
 *
 * Modes:
 * - passthrough (governor disabled): command = request.
 * - governor: the request is feed-forward and a PID trim holds servoGovernorTargetRpm. The
 *   governor engages once RPM comes within servoGovernorEngageWindow of the target, and is off
 *   (command follows the request) while the request is below servoGovernorMinRequest - that is how
 *   idle, motor interlock off and autorotation reach the throttle. A lost request keeps the
 *   governor holding speed on the last feed-forward.
 * Overspeed (servoThrottleOverspeedRpm, both modes): the throttle is closed until RPM falls back.
 *
 * The output is started once at boot (hardware PWM timer channels are never released), so a pin
 * or frame-rate change needs a power cycle. Pulse-width calibration and governor settings apply live.
 */

#pragma once

#include "engine_module.h"
#include "rusefi_types.h"
#include "efi_pid.h"

enum class ServoGovernorState : uint8_t {
	// Governor disabled in the configuration: command = request
	Passthrough = 0,
	// Governor enabled but not governing (spool-up, low request, no RPM): command = request
	Following = 1,
	// Command = request + PID trim
	Governing = 2,
	// Above the overspeed limit: throttle closed
	Overspeed = 3,
};

class ServoThrottle : public EngineModule {
public:
	void onFastCallback() override;

	// One control step: request (may be invalid), engine RPM (may be invalid), time step
	void update(SensorResult request, SensorResult rpm, float dtSeconds);

	// Output pulse width for a throttle position: closedUs at 0%, openUs at 100%
	static float positionToPulseUs(percent_t position, float closedUs, float openUs);

	percent_t getCommandPercent() const {
		return m_commandPercent;
	}

	// Modelled servo position, reported as TPS1
	percent_t getPositionPercent() const {
		return m_positionPercent;
	}

	float getCommandPulseUs() const {
		return m_commandPulseUs;
	}

	bool isRequestValid() const {
		return m_requestValid;
	}

	ServoGovernorState getState() const {
		return m_state;
	}

	percent_t getTrim() const {
		return m_trim;
	}

	// Back to "closed, no request seen yet"
	void reset();

private:
	ServoGovernorState governorStep(percent_t feedForward, SensorResult rpm, float dtSeconds);

	Pid m_pid;
	percent_t m_feedForward = 0;
	percent_t m_commandPercent = 0;
	percent_t m_positionPercent = 0;
	percent_t m_trim = 0;
	float m_commandPulseUs = 0;
	bool m_requestValid = false;
	ServoGovernorState m_state = ServoGovernorState::Passthrough;
};

bool isServoThrottleEnabled();

// TPS1 as the servo command; called from initTps()/deinitTps()
void initServoThrottleTps();
void deinitServoThrottleTps();

// Starts the servo output; called once at boot
void initServoThrottleOutput();

// refreshConfigErrorState() producer: raises a config error and returns true while the servo
// output is configured without a request input
bool checkServoThrottleConfigError();

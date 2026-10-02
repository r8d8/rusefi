/**
 * @file servo_throttle.h
 *
 * Throttle driven by an RC-style servo (1-2 ms pulse) on servoThrottlePin.
 *
 * The throttle request arrives as a servo pulse on pwmInputTpsPin, e.g. the throttle channel of
 * an autopilot. rusEFI turns the request into the servo command and TPS1 reports that command,
 * because the throttle has no position sensor.
 *
 * Passthrough is the only mode so far: the command follows the request. If the request is lost
 * (no pulses, or an implausible pulse) the last command is held; before the first valid request
 * the throttle is commanded closed.
 *
 * The output is started once at boot (hardware PWM timer channels are never released), so a pin
 * or frame-rate change needs a power cycle. Pulse-width calibration applies live.
 */

#pragma once

#include "engine_module.h"
#include "rusefi_types.h"

class ServoThrottle : public EngineModule {
public:
	void onFastCallback() override;

	// Output pulse width for a throttle position: closedUs at 0%, openUs at 100%
	static float positionToPulseUs(percent_t position, float closedUs, float openUs);

	percent_t getCommandPercent() const {
		return m_commandPercent;
	}

	float getCommandPulseUs() const {
		return m_commandPulseUs;
	}

	bool isRequestValid() const {
		return m_requestValid;
	}

	// Back to "closed, no request seen yet"
	void reset();

private:
	percent_t m_commandPercent = 0;
	float m_commandPulseUs = 0;
	bool m_requestValid = false;
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

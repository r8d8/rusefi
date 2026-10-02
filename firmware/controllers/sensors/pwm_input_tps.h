/**
 * @file pwm_input_tps.h
 *
 * Throttle position read from an RC-servo-style PWM signal on a digital input. The pulse width is
 * mapped linearly from pwmInputTpsMinUs (0%) to pwmInputTpsMaxUs (100%); min > max reverses the
 * direction. The value times out when pulses stop.
 *
 * Two uses:
 * - on its own it replaces the analog TPS1 (the pulse drives the throttle servo elsewhere);
 * - with a servo throttle output configured it is the throttle request, and TPS1 reports the servo
 *   command instead (servo_throttle.h).
 */

#pragma once

#include "stored_value_sensor.h"

// Servo pulses outside this window are not a valid servo command
#define PWM_INPUT_TPS_PLAUSIBLE_MIN_US 500
#define PWM_INPUT_TPS_PLAUSIBLE_MAX_US 2500
// Five frames at the slowest common servo frame rate (50 Hz)
#define PWM_INPUT_TPS_TIMEOUT_MS 100

class PwmInputTps : public StoredValueSensor {
public:
	PwmInputTps() : StoredValueSensor(SensorType::Tps1, MS2NT(PWM_INPUT_TPS_TIMEOUT_MS)) { }

	// Called on both edges; isHigh is the pin level after the edge
	void onEdge(efitick_t nowNt, bool isHigh);

	// Deliberately not exposed through getRaw()/hasRaw(): callers of those expect volts
	// (check engine light range checks, TPS "grab closed/open" calibration).
	float getPulseWidthUs() const {
		return m_lastPulseWidthUs;
	}

	void showInfo(const char* sensorName) const override;

	// Forget the stored value and any pending rising edge
	void reset() {
		invalidate();
		m_hasRisingEdge = false;
		m_lastPulseWidthUs = 0;
	}

	// Pure conversion: pulse width to throttle percent, or an error code
	static SensorResult convert(float pulseWidthUs, float minUs, float maxUs);

private:
	efitick_t m_risingEdgeNt = 0;
	bool m_hasRisingEdge = false;
	float m_lastPulseWidthUs = 0;
};

bool isPwmInputTps1();
// registerAsTps1: false when the pulse is a throttle request for the servo throttle, which then
// owns TPS1 (see servo_throttle.h)
void initPwmInputTps(bool registerAsTps1);
void deinitPwmInputTps();

PwmInputTps& getPwmInputTps();

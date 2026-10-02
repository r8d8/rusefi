/**
 * @file pwm_input_tps.cpp
 */

#include "pch.h"

#include "pwm_input_tps.h"

#include "digital_input_exti.h"
#include "tps.h"

/*static*/ SensorResult PwmInputTps::convert(float pulseWidthUs, float minUs, float maxUs) {
	if (pulseWidthUs < PWM_INPUT_TPS_PLAUSIBLE_MIN_US) {
		return UnexpectedCode::Low;
	}

	if (pulseWidthUs > PWM_INPUT_TPS_PLAUSIBLE_MAX_US) {
		return UnexpectedCode::High;
	}

	if (minUs == maxUs) {
		return UnexpectedCode::Configuration;
	}

	float percent = POSITION_FULLY_OPEN * (pulseWidthUs - minUs) / (maxUs - minUs);
	return clampF(0, percent, POSITION_FULLY_OPEN);
}

void PwmInputTps::onEdge(efitick_t nowNt, bool isHigh) {
	if (isHigh) {
		m_risingEdgeNt = nowNt;
		m_hasRisingEdge = true;
		return;
	}

	// A falling edge without a preceding rising edge (start-up, or a missed edge) carries no width
	if (!m_hasRisingEdge) {
		return;
	}
	m_hasRisingEdge = false;

	m_lastPulseWidthUs = NT2US(nowNt - m_risingEdgeNt);

	auto result = convert(m_lastPulseWidthUs,
		engineConfiguration->pwmInputTpsMinUs,
		engineConfiguration->pwmInputTpsMaxUs);

	if (result) {
		setValidValue(result.Value, nowNt);
	} else {
		invalidate(result.Code);
	}
}

void PwmInputTps::showInfo(const char* sensorName) const {
	const auto value = get();
	efiPrintf("PWM input TPS \"%s\": pulse %.1f us, valid: %s, value: %.2f",
		sensorName, m_lastPulseWidthUs, boolToString(value.Valid), value.Value);
}

static PwmInputTps pwmInputTps;

bool isPwmInputTps1() {
	return isBrainPinValid(engineConfiguration->pwmInputTpsPin);
}

#if EFI_PROD_CODE
static Gpio pwmInputTpsPin = Gpio::Unassigned;

static void pwmInputTpsExtiCallback(void*, efitick_t nowNt) {
	pwmInputTps.onEdge(nowNt, efiReadPin(pwmInputTpsPin));
}
#endif // EFI_PROD_CODE

void initPwmInputTps() {
#if EFI_PROD_CODE
	if (efiExtiEnablePin("PWM input TPS", engineConfiguration->pwmInputTpsPin,
			PAL_EVENT_MODE_BOTH_EDGES, pwmInputTpsExtiCallback, nullptr) < 0) {
		return;
	}
	pwmInputTpsPin = engineConfiguration->pwmInputTpsPin;
#endif // EFI_PROD_CODE

	pwmInputTps.Register();
}

void deinitPwmInputTps() {
	pwmInputTps.unregister();
	pwmInputTps.reset();

#if EFI_PROD_CODE
	if (!isBrainPinValid(pwmInputTpsPin)) {
		return;
	}

	efiExtiDisablePin(pwmInputTpsPin);
	pwmInputTpsPin = Gpio::Unassigned;
#endif // EFI_PROD_CODE
}

#if EFI_UNIT_TEST
PwmInputTps& getPwmInputTpsForUnitTest() {
	return pwmInputTps;
}
#endif // EFI_UNIT_TEST

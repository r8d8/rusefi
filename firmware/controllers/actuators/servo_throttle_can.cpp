/**
 * @file servo_throttle_can.cpp
 *
 * HCU_ENGINE_CMD decoder, see servo_throttle_can.h
 */

#include "pch.h"

#include "servo_throttle_can.h"

#if EFI_SERVO_THROTTLE

#if HAS_SERVO_THROTTLE_CAN
#include "pwm_input_tps.h"

uint8_t hcuEngineCmdCrc(const uint8_t* data, size_t length) {
	// crc8() takes a uint8_t length; HCU_ENGINE_CMD covers 7 bytes
	return crc8(data, static_cast<uint8_t>(length));
}

static HcuEngineCommandListener hcuEngineCommandListener;

HcuEngineCommandListener& getHcuEngineCommandListener() {
	return hcuEngineCommandListener;
}

// Any bus: the frame is identified by its ID
bool HcuEngineCommandListener::acceptFrame(const size_t, const CANRxFrame& frame) const {
	if (engineConfiguration->servoThrottleRequestSource != ServoThrottleRequestSource::Can) {
		return false;
	}

	int expectedId = engineConfiguration->servoThrottleCanId;
	if (expectedId == 0) {
		expectedId = HCU_ENGINE_CMD_DEFAULT_ID;
	}

	return !CAN_ISX(frame)
		&& !CAN_ISRTR(frame)
		&& (int)CAN_SID(frame) == expectedId
		&& frame.DLC == HCU_ENGINE_CMD_DLC;
}

void HcuEngineCommandListener::decodeFrame(const CANRxFrame& frame, efitick_t nowNt) {
	const uint8_t* payload = frame.data8;

	if (hcuEngineCmdCrc(payload, 7) != payload[7] || (payload[1] >> 4) != HCU_ENGINE_CMD_VERSION) {
		return;
	}

	uint16_t feedForward = (payload[4] << 8) | payload[5];
	if (feedForward > HCU_ENGINE_CMD_FF_MAX) {
		return;
	}

	// A sender stuck on one frame repeats its counter: that must not keep the command fresh
	uint8_t frameCounter = payload[1] & 0x0F;
	if (m_hasCounter && frameCounter == m_lastCounter) {
		return;
	}
	m_hasCounter = true;
	m_lastCounter = frameCounter;

	HcuEngineCommand received;
	received.feedForward = feedForward / 100.0f;
	received.targetRpm = (payload[2] << 8) | payload[3];
	received.governorOn = (payload[0] & 0x02) != 0;

	{
		chibios_rt::CriticalSectionLocker csl;
		m_command = received;
		m_lastCountedNt = nowNt;
		m_hasCommand = true;
	}

	if (payload[0] & 0x01) {
		// RUN: re-arm the stop
		m_runZeroCount = 0;
		return;
	}

	// Stop once, on the frame that completes the run of RUN = 0 frames (see header)
	if (m_runZeroCount < HCU_ENGINE_CMD_STOP_FRAMES) {
		m_runZeroCount++;
		if (m_runZeroCount == HCU_ENGINE_CMD_STOP_FRAMES) {
			doScheduleStopEngine(StopRequestedReason::CanCommand);
		}
	}
}

HcuCommandState HcuEngineCommandListener::get(HcuEngineCommand& command, efitick_t nowNt) const {
	efitick_t lastCountedNt;
	bool hasCommand;
	{
		chibios_rt::CriticalSectionLocker csl;
		command = m_command;
		lastCountedNt = m_lastCountedNt;
		hasCommand = m_hasCommand;
	}

	if (!hasCommand) {
		return HcuCommandState::None;
	}

	return nowNt - lastCountedNt < MS2NT(PWM_INPUT_TPS_TIMEOUT_MS) ? HcuCommandState::Fresh : HcuCommandState::Held;
}

void HcuEngineCommandListener::reset() {
	chibios_rt::CriticalSectionLocker csl;
	m_command = HcuEngineCommand();
	m_lastCountedNt = 0;
	m_hasCommand = false;
	m_hasCounter = false;
	m_lastCounter = 0;
	m_runZeroCount = 0;
}
#endif // HAS_SERVO_THROTTLE_CAN

void initServoThrottleCan() {
#if EFI_CAN_SUPPORT
	// Registered once and for good: acceptFrame() checks the request source and the ID on every
	// frame, so a configuration change needs no (unlocked) listener list surgery
	registerCanListener(hcuEngineCommandListener);
#endif // EFI_CAN_SUPPORT
}

#endif // EFI_SERVO_THROTTLE

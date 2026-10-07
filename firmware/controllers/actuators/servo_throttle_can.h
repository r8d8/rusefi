/**
 * @file servo_throttle_can.h
 *
 * HCU_ENGINE_CMD: the hybrid controller's (HCU) engine command, the servo throttle request when
 * servoThrottleRequestSource is CAN. The frame is specified by the HCU - do not change it here:
 * 11-bit ID servoThrottleCanId (0x1A0 by default), DLC 8, sent every 10 ms, fields big-endian.
 *   byte 0     bit 0 RUN (1 = run, 0 = stop request), bit 1 GOV_ON (1 = governor on), bits 2-7 reserved
 *   byte 1     bits 0-3 COUNTER (+1 per frame, mod 16), bits 4-7 VERSION = 1
 *   bytes 2-3  TARGET_RPM: engine RPM, 1 rpm/bit, 0 = use servoGovernorTargetRpm
 *   bytes 4-5  FF: throttle feed-forward, 0.01 %/bit, 0..10000
 *   byte 6     reserved
 *   byte 7     CRC-8/SAE-J1850 over bytes 0-6
 *
 * A frame counts only with the configured standard ID, DLC 8, a correct CRC, VERSION 1, FF at most
 * 100% and a COUNTER different from the last counted frame's (a stuck sender must not keep the
 * command fresh). The first frame after boot counts. Reserved bits are not checked.
 *
 * The command is fresh while the last counted frame is under PWM_INPUT_TPS_TIMEOUT_MS old. When it
 * is not, ServoThrottle holds the last feed-forward, target and GOV_ON and keeps governing, exactly
 * like a lost pulse request: a lost command never stops the engine. Before the first counted frame
 * the throttle is closed.
 *
 * Stop: RUN = 0 in HCU_ENGINE_CMD_STOP_FRAMES consecutive counted frames schedules one engine stop
 * (doScheduleStopEngine, StopRequestedReason::CanCommand), and a counted RUN = 1 frame re-arms it.
 * The stop is issued once per run-to-stop transition, not on every frame while RUN stays 0: each
 * doScheduleStopEngine() restarts the engineShutDownPeriod window and prints to the console, so at
 * 100 Hz it would flood the console and keep the window open for as long as RUN = 0. After the
 * window the engine stays stopped because nothing cranks it - cranking is the HCU's job.
 *
 * Threads: frames are decoded in the CAN RX thread and the command is read by the 200 Hz fast
 * callback (which the trigger ISR also runs once when RPM appears). The command and its timestamp
 * are copied in and out under chibios_rt::CriticalSectionLocker - a few words, and that lock is
 * legal in thread and ISR context alike (KnockController and MapAverager use it the same way). The
 * counter and stop state belong to the CAN RX thread alone. The stop is scheduled from the CAN RX
 * thread, as the existing ECU_CAN_BUS_USER_CONTROL TS_STOP_ENGINE path already does.
 */

#pragma once

#include "rusefi_types.h"

#define HCU_ENGINE_CMD_DEFAULT_ID 0x1A0
#define HCU_ENGINE_CMD_DLC 8
#define HCU_ENGINE_CMD_VERSION 1
// 100% at 0.01 %/bit
#define HCU_ENGINE_CMD_FF_MAX 10000
// Consecutive counted RUN = 0 frames that stop the engine
#define HCU_ENGINE_CMD_STOP_FRAMES 3

// The decoder exists in firmware with CAN support, and in unit tests (frames injected directly)
#define HAS_SERVO_THROTTLE_CAN (EFI_SERVO_THROTTLE && (EFI_CAN_SUPPORT || EFI_UNIT_TEST))

// Command of the last counted frame
struct HcuEngineCommand {
	percent_t feedForward = 0;
	// 0 = use servoGovernorTargetRpm
	uint16_t targetRpm = 0;
	bool governorOn = false;
};

// Also the servoThrottleCanCommand output channel
enum class HcuCommandState : uint8_t {
	// No counted frame since boot: throttle closed
	None = 0,
	Fresh = 1,
	// Last counted frame too old: the last command is held
	Held = 2,
};

void initServoThrottleCan();

#if HAS_SERVO_THROTTLE_CAN
#include "can_listener.h"

// CRC-8/SAE-J1850: poly 0x1D, init 0xFF, no reflection, xor-out 0xFF, check value 0x4B. This is
// the CRC libfirmware's crc8() computes, reused so the decoder adds no second CRC table to flash.
uint8_t hcuEngineCmdCrc(const uint8_t* data, size_t length);

class HcuEngineCommandListener : public CanListener {
public:
	// The ID is configurable: acceptFrame() checks it, the base class ID is unused
	HcuEngineCommandListener() : CanListener(0) { }

	bool acceptFrame(const size_t busIndex, const CANRxFrame& frame) const override;

	// Copies the last counted command (fast callback side)
	HcuCommandState get(HcuEngineCommand& command, efitick_t nowNt) const;

	// Back to "no frame since boot"
	void reset();

protected:
	void decodeFrame(const CANRxFrame& frame, efitick_t nowNt) override;

private:
	// Shared with the fast callback: written and read under a critical section
	HcuEngineCommand m_command;
	efitick_t m_lastCountedNt = 0;
	bool m_hasCommand = false;

	// CAN RX thread only
	bool m_hasCounter = false;
	uint8_t m_lastCounter = 0;
	uint8_t m_runZeroCount = 0;
};

HcuEngineCommandListener& getHcuEngineCommandListener();
#endif // HAS_SERVO_THROTTLE_CAN

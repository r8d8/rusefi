/**
 * @file test_servo_throttle_can.cpp
 *
 * Servo throttle request from the hybrid controller's HCU_ENGINE_CMD CAN frame
 * (servo_throttle_can.h): frame validation, freshness and hold-on-loss, governor target and
 * GOV_ON, and the RUN = 0 engine stop.
 */

#include "pch.h"

#include "pwm_input_tps.h"
#include "servo_throttle.h"
#include "servo_throttle_can.h"

namespace {

ServoThrottle& servo() {
	return *engine->module<ServoThrottle>();
}

HcuEngineCommandListener& listener() {
	return getHcuEngineCommandListener();
}

struct HcuCmd {
	bool run = true;
	bool governorOn = true;
	uint16_t targetRpm = 0;
	// 0.01 %/bit
	uint16_t feedForward = 0;
	uint8_t version = HCU_ENGINE_CMD_VERSION;
};

// RUN = 1
HcuCmd runCmd(uint16_t feedForward, uint16_t targetRpm = 0, bool governorOn = true) {
	HcuCmd cmd;
	cmd.feedForward = feedForward;
	cmd.targetRpm = targetRpm;
	cmd.governorOn = governorOn;
	return cmd;
}

// RUN = 0
HcuCmd stopCmd(uint16_t feedForward) {
	HcuCmd cmd = runCmd(feedForward);
	cmd.run = false;
	return cmd;
}

CANRxFrame makeFrame(const HcuCmd& cmd, uint8_t counter) {
	CANRxFrame frame{};
	frame.IDE = CAN_IDE_STD;
	frame.RTR = CAN_RTR_DATA;
	frame.SID = HCU_ENGINE_CMD_DEFAULT_ID;
	frame.DLC = 8;
	frame.data8[0] = (cmd.run ? 0x01 : 0) | (cmd.governorOn ? 0x02 : 0);
	frame.data8[1] = (counter & 0x0F) | (cmd.version << 4);
	frame.data8[2] = cmd.targetRpm >> 8;
	frame.data8[3] = cmd.targetRpm & 0xFF;
	frame.data8[4] = cmd.feedForward >> 8;
	frame.data8[5] = cmd.feedForward & 0xFF;
	frame.data8[6] = 0;
	frame.data8[7] = hcuEngineCmdCrc(frame.data8, 7);
	return frame;
}

void receive(const CANRxFrame& frame) {
	listener().processFrame(/*busIndex*/0, frame, getTimeNowNt());
}

HcuCommandState lastCommand(HcuEngineCommand& command) {
	return listener().get(command, getTimeNowNt());
}

} // namespace

class ServoThrottleCan : public ::testing::Test {
protected:
	EngineTestHelper eth{engine_type_e::TEST_ENGINE};
	uint8_t counter = 0;

	void SetUp() override {
		engineConfiguration->servoThrottlePin = Gpio::B8;
		engineConfiguration->servoThrottleRequestSource = ServoThrottleRequestSource::Can;
		engineConfiguration->servoThrottleCanId = HCU_ENGINE_CMD_DEFAULT_ID;
		engineConfiguration->engineShutDownPeriod = 3;

		engineConfiguration->servoGovernorEnabled = true;
		engineConfiguration->servoGovernorTargetRpm = 6000;
		engineConfiguration->servoGovernorEngageWindow = 300;
		engineConfiguration->servoGovernorMinRequest = 10;
		engineConfiguration->servoGovernorPid.pFactor = 0.01;
		engineConfiguration->servoGovernorPid.iFactor = 0.02;
		engineConfiguration->servoGovernorPid.dFactor = 0;
		engineConfiguration->servoGovernorPid.offset = 0;
		engineConfiguration->servoGovernorPid.minValue = -20;
		engineConfiguration->servoGovernorPid.maxValue = 20;

		listener().reset();
		servo().reset();
	}

	void TearDown() override {
		listener().reset();
		Sensor::resetMockValue(SensorType::Rpm);
	}

	// A valid frame with the next counter value
	void send(const HcuCmd& cmd) {
		receive(makeFrame(cmd, counter++));
	}

	bool isEngineStopRequested() {
		return getLimpManager()->shutdownController.isEngineStop(getTimeNowNt());
	}
};

// ---- Frame validation ----

TEST(ServoThrottleCanCrc, checkValue) {
	// CRC-8/SAE-J1850 check value
	const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
	EXPECT_EQ(0x4B, hcuEngineCmdCrc(check, sizeof(check)));
}

TEST_F(ServoThrottleCan, validFrameSetsCommand) {
	HcuEngineCommand command;
	EXPECT_EQ(HcuCommandState::None, lastCommand(command));

	send(runCmd(4250, 6200, true));

	EXPECT_EQ(HcuCommandState::Fresh, lastCommand(command));
	EXPECT_NEAR(42.5, command.feedForward, EPS4D);
	EXPECT_EQ(6200, command.targetRpm);
	EXPECT_TRUE(command.governorOn);

	send(runCmd(10000, 0, false));
	EXPECT_EQ(HcuCommandState::Fresh, lastCommand(command));
	EXPECT_NEAR(100, command.feedForward, EPS4D);
	EXPECT_EQ(0, command.targetRpm);
	EXPECT_FALSE(command.governorOn);
}

TEST_F(ServoThrottleCan, invalidFramesAreRejected) {
	send(runCmd(3000));
	HcuEngineCommand command;
	ASSERT_EQ(HcuCommandState::Fresh, lastCommand(command));
	ASSERT_NEAR(30, command.feedForward, EPS4D);

	auto expectStillThirtyPercent = [](const char* what) {
		HcuEngineCommand held;
		lastCommand(held);
		EXPECT_NEAR(30, held.feedForward, EPS4D) << what;
	};

	const HcuCmd eighty = runCmd(8000);

	auto badCrc = makeFrame(eighty, counter++);
	badCrc.data8[7] ^= 0x01;
	receive(badCrc);
	expectStillThirtyPercent("bad CRC");

	auto shortFrame = makeFrame(eighty, counter++);
	shortFrame.DLC = 7;
	receive(shortFrame);
	expectStillThirtyPercent("DLC 7");

	HcuCmd version2 = runCmd(8000);
	version2.version = 2;
	receive(makeFrame(version2, counter++));
	expectStillThirtyPercent("VERSION 2");

	receive(makeFrame(runCmd(10001), counter++));
	expectStillThirtyPercent("FF above 100%");

	auto extended = makeFrame(eighty, counter++);
	extended.IDE = CAN_IDE_EXT;
	extended.EID = HCU_ENGINE_CMD_DEFAULT_ID;
	receive(extended);
	expectStillThirtyPercent("29-bit ID");

	auto otherId = makeFrame(eighty, counter++);
	otherId.SID = HCU_ENGINE_CMD_DEFAULT_ID + 1;
	receive(otherId);
	expectStillThirtyPercent("other ID");

	// Valid again
	send(eighty);
	lastCommand(command);
	EXPECT_NEAR(80, command.feedForward, EPS4D);
}

TEST_F(ServoThrottleCan, repeatedCounterIsNotCounted) {
	receive(makeFrame(runCmd(3000), 5));
	// A stuck sender: same counter, whatever the content
	receive(makeFrame(runCmd(8000), 5));
	HcuEngineCommand command;
	lastCommand(command);
	EXPECT_NEAR(30, command.feedForward, EPS4D);

	receive(makeFrame(runCmd(8000), 6));
	lastCommand(command);
	EXPECT_NEAR(80, command.feedForward, EPS4D);

	// A repeated frame does not keep the command fresh either
	eth.moveTimeForwardMs(60);
	receive(makeFrame(runCmd(8000), 6));
	eth.moveTimeForwardMs(60);
	EXPECT_EQ(HcuCommandState::Held, lastCommand(command));
}

TEST_F(ServoThrottleCan, configuredIdAndSource) {
	engineConfiguration->servoThrottleCanId = 0x123;

	receive(makeFrame(runCmd(3000), counter++));
	HcuEngineCommand command;
	EXPECT_EQ(HcuCommandState::None, lastCommand(command));

	auto frame = makeFrame(runCmd(3000), counter++);
	frame.SID = 0x123;
	receive(frame);
	EXPECT_EQ(HcuCommandState::Fresh, lastCommand(command));

	// 0 reads as the default ID
	listener().reset();
	engineConfiguration->servoThrottleCanId = 0;
	send(runCmd(3000));
	EXPECT_EQ(HcuCommandState::Fresh, lastCommand(command));

	// PWM request source: frames are ignored
	listener().reset();
	engineConfiguration->servoThrottleRequestSource = ServoThrottleRequestSource::PwmInput;
	send(runCmd(3000));
	EXPECT_EQ(HcuCommandState::None, lastCommand(command));
}

// ---- Servo throttle with the CAN request ----

TEST_F(ServoThrottleCan, closedBeforeFirstFrame) {
	Sensor::setMockValue(SensorType::Rpm, 0);
	servo().onFastCallback();

	EXPECT_FALSE(servo().isRequestValid());
	EXPECT_NEAR(0, servo().getCommandPercent(), EPS4D);
	EXPECT_NEAR(1000, servo().getCommandPulseUs(), EPS4D);
	EXPECT_EQ((uint8_t)HcuCommandState::None, engine->outputChannels.servoThrottleCanCommand);
}

TEST_F(ServoThrottleCan, governorOffIsPassthrough) {
	Sensor::setMockValue(SensorType::Rpm, 5900);
	send(runCmd(4000, 6000, false));
	servo().onFastCallback();

	EXPECT_TRUE(servo().isRequestValid());
	EXPECT_EQ(ServoGovernorState::Passthrough, servo().getState());
	EXPECT_NEAR(40, servo().getCommandPercent(), EPS4D);
	EXPECT_EQ((uint8_t)HcuCommandState::Fresh, engine->outputChannels.servoThrottleCanCommand);
}

TEST_F(ServoThrottleCan, governorNeedsTheConfigurationToo) {
	engineConfiguration->servoGovernorEnabled = false;
	Sensor::setMockValue(SensorType::Rpm, 5900);
	send(runCmd(4000, 6000, true));
	servo().onFastCallback();

	EXPECT_EQ(ServoGovernorState::Passthrough, servo().getState());
	EXPECT_NEAR(40, servo().getCommandPercent(), EPS4D);
}

TEST_F(ServoThrottleCan, governsOnTheFrameTarget) {
	// 4800 RPM is inside the engage window of the frame's 5000, far below the configured 6000
	Sensor::setMockValue(SensorType::Rpm, 4800);
	send(runCmd(5000, 5000, true));
	servo().onFastCallback();

	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	// First step: P = 0.01 * 200 = 2%, I = 0.02 * 0.005 * 200 = 0.02%
	EXPECT_NEAR(2.02, servo().getTrim(), 1e-3);
	EXPECT_NEAR(52.02, servo().getCommandPercent(), 1e-3);
}

TEST_F(ServoThrottleCan, zeroTargetUsesConfiguredTarget) {
	// 5800 RPM: inside the engage window of the configured 6000
	Sensor::setMockValue(SensorType::Rpm, 5800);
	send(runCmd(5000, 0, true));
	servo().onFastCallback();

	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_NEAR(2.02, servo().getTrim(), 1e-3);
}

TEST_F(ServoThrottleCan, lostCommandHoldsAndKeepsGoverning) {
	Sensor::setMockValue(SensorType::Rpm, 4950);
	send(runCmd(5000, 5000, true));
	servo().onFastCallback();
	ASSERT_EQ(ServoGovernorState::Governing, servo().getState());

	// Still fresh just under the timeout
	eth.moveTimeForwardMs(PWM_INPUT_TPS_TIMEOUT_MS - 1);
	servo().onFastCallback();
	EXPECT_TRUE(servo().isRequestValid());

	// Link lost: last feed-forward, target and GOV_ON held, the governor keeps holding speed
	eth.moveTimeForwardMs(50);
	Sensor::setMockValue(SensorType::Rpm, 4800);
	servo().onFastCallback();
	EXPECT_FALSE(servo().isRequestValid());
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_NEAR(50, engine->outputChannels.servoThrottleRequest, 0.1);
	// Underspeed against the held 5000 RPM target: positive trim on the held 50%
	EXPECT_GT(servo().getCommandPercent(), 51);
	EXPECT_EQ((uint8_t)HcuCommandState::Held, engine->outputChannels.servoThrottleCanCommand);

	// A lost command never stops the engine
	eth.moveTimeForwardMs(5000);
	servo().onFastCallback();
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_FALSE(isEngineStopRequested());

	// Back on the next frame
	send(runCmd(3000, 5000, false));
	servo().onFastCallback();
	EXPECT_TRUE(servo().isRequestValid());
	EXPECT_EQ(ServoGovernorState::Passthrough, servo().getState());
	EXPECT_NEAR(30, servo().getCommandPercent(), EPS4D);
}

// ---- RUN = 0: engine stop ----

TEST_F(ServoThrottleCan, stopAfterThreeConsecutiveRunZeroFrames) {
	const HcuCmd stop = stopCmd(1200);
	const HcuCmd run = runCmd(1200);

	send(stop);
	send(stop);
	EXPECT_FALSE(isEngineStopRequested());

	// RUN = 1 starts the count again
	send(run);
	send(stop);
	send(stop);
	EXPECT_FALSE(isEngineStopRequested());

	// Repeated counters do not count
	auto repeated = makeFrame(stop, counter - 1);
	receive(repeated);
	receive(repeated);
	EXPECT_FALSE(isEngineStopRequested());

	send(stop);
	EXPECT_TRUE(isEngineStopRequested());
	EXPECT_EQ((uint8_t)StopRequestedReason::CanCommand, engine->outputChannels.stopEngineCode);
}

TEST_F(ServoThrottleCan, stopIsRequestedOncePerTransition) {
	const HcuCmd stop = stopCmd(1200);
	const HcuCmd run = runCmd(1200);

	send(stop);
	send(stop);
	send(stop);
	ASSERT_TRUE(isEngineStopRequested());

	// RUN stays 0: no new stop request, so the stop window runs out
	engine->outputChannels.stopEngineCode = 0;
	for (int i = 0; i < 400; i++) {
		eth.moveTimeForwardMs(10);
		send(stop);
	}
	EXPECT_EQ(0, engine->outputChannels.stopEngineCode);
	EXPECT_FALSE(isEngineStopRequested());

	// RUN = 1 re-arms: the next run of three RUN = 0 frames stops again
	send(run);
	send(stop);
	send(stop);
	EXPECT_EQ(0, engine->outputChannels.stopEngineCode);
	send(stop);
	EXPECT_EQ((uint8_t)StopRequestedReason::CanCommand, engine->outputChannels.stopEngineCode);
	EXPECT_TRUE(isEngineStopRequested());
}

// ---- Config error ----

TEST_F(ServoThrottleCan, canSourceNeedsNoPwmInputPin) {
	resetConfigErrorStateForUnitTest();
	ASSERT_FALSE(isBrainPinValid(engineConfiguration->pwmInputTpsPin));

	engineConfiguration->canReadEnabled = true;
	EXPECT_FALSE(checkServoThrottleConfigError());

	engineConfiguration->canReadEnabled = false;
	EXPECT_TRUE(checkServoThrottleConfigError());
	EXPECT_TRUE(hasConfigError());

	resetConfigErrorStateForUnitTest();
}

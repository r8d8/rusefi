#include "pch.h"

#include "init.h"
#include "pwm_input_tps.h"
#include "servo_throttle.h"

static ServoThrottle& servo() {
	return *engine->module<ServoThrottle>();
}

static void sendRequestPulse(EngineTestHelper& eth, int widthUs) {
	auto& input = getPwmInputTps();
	input.onEdge(getTimeNowNt(), true);
	eth.moveTimeForwardUs(widthUs);
	input.onEdge(getTimeNowNt(), false);
	eth.moveTimeForwardUs(20000 - widthUs);
}

static void configureServoThrottle() {
	deinitTps();
	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	engineConfiguration->servoThrottlePin = Gpio::B8;
	initTps();
	servo().reset();
}

TEST(ServoThrottle, positionToPulseUs) {
	EXPECT_NEAR(1000, ServoThrottle::positionToPulseUs(0, 1000, 2000), EPS4D);
	EXPECT_NEAR(1500, ServoThrottle::positionToPulseUs(50, 1000, 2000), EPS4D);
	EXPECT_NEAR(2000, ServoThrottle::positionToPulseUs(100, 1000, 2000), EPS4D);

	// Clamped to the calibrated span
	EXPECT_NEAR(2000, ServoThrottle::positionToPulseUs(120, 1000, 2000), EPS4D);
	EXPECT_NEAR(1000, ServoThrottle::positionToPulseUs(-5, 1000, 2000), EPS4D);

	// Reversed servo
	EXPECT_NEAR(1750, ServoThrottle::positionToPulseUs(25, 2000, 1000), EPS4D);
}

TEST(ServoThrottle, defaults) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);

	EXPECT_FALSE(isServoThrottleEnabled());
	EXPECT_EQ(50, engineConfiguration->servoThrottleFrequency);
	EXPECT_EQ(1000, engineConfiguration->servoThrottleClosedUs);
	EXPECT_EQ(2000, engineConfiguration->servoThrottleOpenUs);
}

TEST(ServoThrottle, disabledDoesNothing) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	servo().reset();

	servo().onFastCallback();
	EXPECT_EQ(0, servo().getCommandPulseUs());
	EXPECT_FALSE(checkServoThrottleConfigError());
}

TEST(ServoThrottle, tps1IsTheCommand) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureServoThrottle();

	auto tps1 = Sensor::getSensorOfType(SensorType::Tps1);
	ASSERT_NE(nullptr, tps1);
	// The pulse input is the request, not TPS1
	EXPECT_NE(&getPwmInputTps(), tps1);

	deinitTps();
}

TEST(ServoThrottle, closedBeforeFirstRequest) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureServoThrottle();

	servo().onFastCallback();
	EXPECT_FALSE(servo().isRequestValid());
	EXPECT_NEAR(0, servo().getCommandPercent(), EPS4D);
	EXPECT_NEAR(1000, servo().getCommandPulseUs(), EPS4D);

	auto tps1 = Sensor::get(SensorType::Tps1);
	EXPECT_TRUE(tps1.Valid);
	EXPECT_NEAR(0, tps1.Value, EPS4D);

	deinitTps();
}

TEST(ServoThrottle, passthrough) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureServoThrottle();

	sendRequestPulse(eth, 1500);
	servo().onFastCallback();
	EXPECT_TRUE(servo().isRequestValid());
	EXPECT_NEAR(50, servo().getCommandPercent(), 0.1);
	EXPECT_NEAR(1500, servo().getCommandPulseUs(), 0.1);
	EXPECT_NEAR(50, Sensor::get(SensorType::Tps1).Value, 0.1);

	sendRequestPulse(eth, 1900);
	servo().onFastCallback();
	EXPECT_NEAR(90, servo().getCommandPercent(), 0.1);
	EXPECT_NEAR(1900, servo().getCommandPulseUs(), 0.1);

	deinitTps();
}

TEST(ServoThrottle, outputCalibrationIndependentOfInput) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureServoThrottle();
	// Throttle linkage only uses part of the servo travel
	engineConfiguration->servoThrottleClosedUs = 1100;
	engineConfiguration->servoThrottleOpenUs = 1900;

	sendRequestPulse(eth, 1250);
	servo().onFastCallback();
	EXPECT_NEAR(25, servo().getCommandPercent(), 0.1);
	EXPECT_NEAR(1300, servo().getCommandPulseUs(), 0.1);

	deinitTps();
}

TEST(ServoThrottle, holdsLastCommandWhenRequestLost) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureServoThrottle();

	sendRequestPulse(eth, 1600);
	servo().onFastCallback();
	ASSERT_NEAR(60, servo().getCommandPercent(), 0.1);

	// Request times out
	eth.moveTimeForwardUs(MS2US(PWM_INPUT_TPS_TIMEOUT_MS) + 10000);
	servo().onFastCallback();
	EXPECT_FALSE(servo().isRequestValid());
	EXPECT_NEAR(60, servo().getCommandPercent(), 0.1);
	EXPECT_NEAR(1600, servo().getCommandPulseUs(), 0.1);

	// TPS1 still reports the (held) command
	auto tps1 = Sensor::get(SensorType::Tps1);
	EXPECT_TRUE(tps1.Valid);
	EXPECT_NEAR(60, tps1.Value, 0.1);

	// Implausible request: still held
	sendRequestPulse(eth, 2800);
	servo().onFastCallback();
	EXPECT_FALSE(servo().isRequestValid());
	EXPECT_NEAR(60, servo().getCommandPercent(), 0.1);

	// Recovers on the next good request
	sendRequestPulse(eth, 1200);
	servo().onFastCallback();
	EXPECT_TRUE(servo().isRequestValid());
	EXPECT_NEAR(20, servo().getCommandPercent(), 0.1);

	deinitTps();
}

TEST(ServoThrottle, configErrorWithoutRequestInput) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	resetConfigErrorStateForUnitTest();

	engineConfiguration->servoThrottlePin = Gpio::B8;
	EXPECT_TRUE(checkServoThrottleConfigError());
	EXPECT_TRUE(hasConfigError());

	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	EXPECT_FALSE(checkServoThrottleConfigError());

	resetConfigErrorStateForUnitTest();
}

// ---- Governor ----

static constexpr float dt = 0.005f;

static SensorResult rpmOf(float rpm) {
	return rpm;
}

static void configureGovernor() {
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
	servo().reset();
}

TEST(ServoGovernor, defaults) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);

	EXPECT_FALSE(engineConfiguration->servoGovernorEnabled);
	EXPECT_EQ(300, engineConfiguration->servoGovernorEngageWindow);
	EXPECT_EQ(10, engineConfiguration->servoGovernorMinRequest);
	EXPECT_EQ(0, engineConfiguration->servoThrottleOverspeedRpm);
	EXPECT_EQ(-20, engineConfiguration->servoGovernorPid.minValue);
	EXPECT_EQ(20, engineConfiguration->servoGovernorPid.maxValue);
}

TEST(ServoGovernor, disabledIsPassthrough) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();
	engineConfiguration->servoGovernorEnabled = false;

	servo().update(40.0f, rpmOf(5000), dt);
	EXPECT_EQ(ServoGovernorState::Passthrough, servo().getState());
	EXPECT_NEAR(40, servo().getCommandPercent(), EPS4D);
}

TEST(ServoGovernor, followsRequestDuringSpoolUp) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	servo().update(50.0f, rpmOf(3000), dt);
	EXPECT_EQ(ServoGovernorState::Following, servo().getState());
	EXPECT_NEAR(50, servo().getCommandPercent(), EPS4D);

	// Still one RPM short of the engage window
	servo().update(50.0f, rpmOf(5699), dt);
	EXPECT_EQ(ServoGovernorState::Following, servo().getState());
}

TEST(ServoGovernor, engagesBumplessInsideWindow) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	servo().update(50.0f, rpmOf(5000), dt);
	servo().update(50.0f, rpmOf(5800), dt);
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	// First step: P = 0.01 * 200 = 2%, I = 0.02 * 0.005 * 200 = 0.02%
	EXPECT_NEAR(2.02, servo().getTrim(), 1e-3);
	EXPECT_NEAR(52.02, servo().getCommandPercent(), 1e-3);
}

TEST(ServoGovernor, onTargetCommandEqualsRequest) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	servo().update(45.0f, rpmOf(6000), dt);
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_NEAR(45, servo().getCommandPercent(), EPS4D);
}

TEST(ServoGovernor, sustainedUnderspeedIntegratesToTheAuthorityLimit) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	for (int i = 0; i < 10000; i++) {
		servo().update(50.0f, rpmOf(5800), dt);
	}
	EXPECT_NEAR(20, servo().getTrim(), EPS4D);
	EXPECT_NEAR(70, servo().getCommandPercent(), EPS4D);

	// Anti-windup: the integrator stopped at the 20% limit, so the first overspeed step already
	// pulls the trim down (P -2%). A wound-up integrator would keep it pinned at 20%.
	servo().update(50.0f, rpmOf(6200), dt);
	EXPECT_NEAR(17.98, servo().getTrim(), 1e-3);
}

TEST(ServoGovernor, overspeedTrimsDown) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	servo().update(50.0f, rpmOf(6000), dt);
	servo().update(50.0f, rpmOf(6200), dt);
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_LT(servo().getTrim(), -1.9);
	EXPECT_LT(servo().getCommandPercent(), 48.1);
}

TEST(ServoGovernor, commandClampedToThrottleRange) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	for (int i = 0; i < 10000; i++) {
		servo().update(95.0f, rpmOf(5800), dt);
	}
	EXPECT_NEAR(100, servo().getCommandPercent(), EPS4D);
}

TEST(ServoGovernor, lowRequestTurnsGovernorOff) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	for (int i = 0; i < 100; i++) {
		servo().update(50.0f, rpmOf(5800), dt);
	}
	ASSERT_EQ(ServoGovernorState::Governing, servo().getState());

	// Motor interlock off / autorotation: the autopilot requests idle
	servo().update(5.0f, rpmOf(5800), dt);
	EXPECT_EQ(ServoGovernorState::Following, servo().getState());
	EXPECT_NEAR(5, servo().getCommandPercent(), EPS4D);
	EXPECT_NEAR(0, servo().getTrim(), EPS4D);

	// Re-engages from a fresh integrator
	servo().update(50.0f, rpmOf(6000), dt);
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_NEAR(50, servo().getCommandPercent(), EPS4D);
}

TEST(ServoGovernor, noRpmFollowsRequest) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	servo().update(50.0f, rpmOf(6000), dt);
	ASSERT_EQ(ServoGovernorState::Governing, servo().getState());

	servo().update(50.0f, UnexpectedCode::Timeout, dt);
	EXPECT_EQ(ServoGovernorState::Following, servo().getState());
	EXPECT_NEAR(50, servo().getCommandPercent(), EPS4D);

	servo().update(50.0f, rpmOf(0), dt);
	EXPECT_EQ(ServoGovernorState::Following, servo().getState());
}

TEST(ServoGovernor, zeroTargetNeverGoverns) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();
	engineConfiguration->servoGovernorTargetRpm = 0;

	servo().update(50.0f, rpmOf(6000), dt);
	EXPECT_EQ(ServoGovernorState::Following, servo().getState());
	EXPECT_NEAR(50, servo().getCommandPercent(), EPS4D);
}

TEST(ServoGovernor, lostRequestKeepsGoverningOnLastFeedForward) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();

	servo().update(50.0f, rpmOf(6000), dt);
	ASSERT_EQ(ServoGovernorState::Governing, servo().getState());

	servo().update(UnexpectedCode::Timeout, rpmOf(5900), dt);
	EXPECT_FALSE(servo().isRequestValid());
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	// Feed-forward held at 50%, governor adds a positive trim for the underspeed
	EXPECT_GT(servo().getCommandPercent(), 50.9);
}

TEST(ServoGovernor, overspeedClosesThrottleUntilBackOnTarget) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();
	engineConfiguration->servoThrottleOverspeedRpm = 7000;

	servo().update(50.0f, rpmOf(6000), dt);
	servo().update(50.0f, rpmOf(7100), dt);
	EXPECT_EQ(ServoGovernorState::Overspeed, servo().getState());
	EXPECT_NEAR(0, servo().getCommandPercent(), EPS4D);

	// Latched until the governor target
	servo().update(50.0f, rpmOf(6500), dt);
	EXPECT_EQ(ServoGovernorState::Overspeed, servo().getState());

	servo().update(50.0f, rpmOf(5990), dt);
	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_GT(servo().getCommandPercent(), 50);
}

TEST(ServoGovernor, overspeedProtectsPassthroughToo) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureGovernor();
	engineConfiguration->servoGovernorEnabled = false;
	engineConfiguration->servoThrottleOverspeedRpm = 7000;

	servo().update(60.0f, rpmOf(7100), dt);
	EXPECT_EQ(ServoGovernorState::Overspeed, servo().getState());
	EXPECT_NEAR(0, servo().getCommandPercent(), EPS4D);

	// Released one engage window below the limit
	servo().update(60.0f, rpmOf(6750), dt);
	EXPECT_EQ(ServoGovernorState::Overspeed, servo().getState());
	servo().update(60.0f, rpmOf(6650), dt);
	EXPECT_EQ(ServoGovernorState::Passthrough, servo().getState());
	EXPECT_NEAR(60, servo().getCommandPercent(), EPS4D);
}

TEST(ServoGovernor, fastCallbackUsesRpmSensorAndPostsLiveData) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	configureServoThrottle();
	configureGovernor();

	Sensor::setMockValue(SensorType::Rpm, 5800);
	sendRequestPulse(eth, 1500);
	servo().onFastCallback();

	EXPECT_EQ(ServoGovernorState::Governing, servo().getState());
	EXPECT_NEAR(servo().getCommandPercent(), Sensor::get(SensorType::Tps1).Value, 1e-3);
	EXPECT_EQ((uint8_t)ServoGovernorState::Governing, engine->outputChannels.servoGovernorState);
	EXPECT_NEAR(50, engine->outputChannels.servoThrottleRequest, 0.1);
	EXPECT_NEAR(servo().getCommandPulseUs(), engine->outputChannels.servoThrottlePulseUs, 1);

	Sensor::resetMockValue(SensorType::Rpm);
	deinitTps();
}

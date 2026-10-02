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

#include "pch.h"

#include "init.h"
#include "pwm_input_tps.h"

TEST(PwmInputTps, convertStandardRange) {
	EXPECT_NEAR(0, PwmInputTps::convert(1000, 1000, 2000).Value, EPS4D);
	EXPECT_NEAR(25, PwmInputTps::convert(1250, 1000, 2000).Value, EPS4D);
	EXPECT_NEAR(50, PwmInputTps::convert(1500, 1000, 2000).Value, EPS4D);
	EXPECT_NEAR(100, PwmInputTps::convert(2000, 1000, 2000).Value, EPS4D);
}

TEST(PwmInputTps, convertClampsBeyondCalibration) {
	// Plausible servo pulses outside the calibrated span clamp instead of failing
	auto below = PwmInputTps::convert(900, 1000, 2000);
	ASSERT_TRUE(below.Valid);
	EXPECT_NEAR(0, below.Value, EPS4D);

	auto above = PwmInputTps::convert(2100, 1000, 2000);
	ASSERT_TRUE(above.Valid);
	EXPECT_NEAR(100, above.Value, EPS4D);
}

TEST(PwmInputTps, convertReversedDirection) {
	EXPECT_NEAR(0, PwmInputTps::convert(2000, 2000, 1000).Value, EPS4D);
	EXPECT_NEAR(75, PwmInputTps::convert(1250, 2000, 1000).Value, EPS4D);
	EXPECT_NEAR(100, PwmInputTps::convert(1000, 2000, 1000).Value, EPS4D);
}

TEST(PwmInputTps, convertRejectsImplausiblePulses) {
	auto tooShort = PwmInputTps::convert(PWM_INPUT_TPS_PLAUSIBLE_MIN_US - 1, 1000, 2000);
	EXPECT_FALSE(tooShort.Valid);
	EXPECT_EQ(UnexpectedCode::Low, tooShort.Code);

	auto tooLong = PwmInputTps::convert(PWM_INPUT_TPS_PLAUSIBLE_MAX_US + 1, 1000, 2000);
	EXPECT_FALSE(tooLong.Valid);
	EXPECT_EQ(UnexpectedCode::High, tooLong.Code);
}

TEST(PwmInputTps, convertRejectsZeroSpan) {
	auto result = PwmInputTps::convert(1500, 1500, 1500);
	EXPECT_FALSE(result.Valid);
	EXPECT_EQ(UnexpectedCode::Configuration, result.Code);
}

TEST(PwmInputTps, defaultsAreStandardServoRange) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);

	EXPECT_EQ(Gpio::Unassigned, engineConfiguration->pwmInputTpsPin);
	EXPECT_EQ(1000, engineConfiguration->pwmInputTpsMinUs);
	EXPECT_EQ(2000, engineConfiguration->pwmInputTpsMaxUs);
}

TEST(PwmInputTps, notRegisteredWithoutPin) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	deinitTps();
	initTps();

	EXPECT_NE(&getPwmInputTpsForUnitTest(), Sensor::getSensorOfType(SensorType::Tps1));
}

static void sendPulse(EngineTestHelper& eth, int widthUs, int periodUs = 20000) {
	auto& sensor = getPwmInputTpsForUnitTest();
	sensor.onEdge(getTimeNowNt(), true);
	eth.moveTimeForwardUs(widthUs);
	sensor.onEdge(getTimeNowNt(), false);
	eth.moveTimeForwardUs(periodUs - widthUs);
}

TEST(PwmInputTps, replacesAnalogTps1) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	deinitTps();
	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	initTps();

	EXPECT_EQ(&getPwmInputTpsForUnitTest(), Sensor::getSensorOfType(SensorType::Tps1));
	EXPECT_EQ(nullptr, Sensor::getSensorOfType(SensorType::Tps1Primary));

	// No pulse yet
	EXPECT_FALSE(Sensor::get(SensorType::Tps1).Valid);

	sendPulse(eth, 1500);
	EXPECT_NEAR(50, Sensor::get(SensorType::Tps1).Value, 0.1);
	EXPECT_NEAR(1500, getPwmInputTpsForUnitTest().getPulseWidthUs(), 0.1);

	sendPulse(eth, 1800);
	EXPECT_NEAR(80, Sensor::get(SensorType::Tps1).Value, 0.1);

	// 333 Hz digital servo frame rate
	sendPulse(eth, 1100, 3000);
	EXPECT_NEAR(10, Sensor::get(SensorType::Tps1).Value, 0.1);

	deinitTps();
}

TEST(PwmInputTps, ignoresFallingEdgeWithoutRisingEdge) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	deinitTps();
	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	initTps();

	// Signal already high at start-up: the first edge seen is a falling one
	eth.moveTimeForwardUs(700);
	getPwmInputTpsForUnitTest().onEdge(getTimeNowNt(), false);
	EXPECT_FALSE(Sensor::get(SensorType::Tps1).Valid);

	eth.moveTimeForwardUs(18000);
	sendPulse(eth, 1250);
	EXPECT_NEAR(25, Sensor::get(SensorType::Tps1).Value, 0.1);

	deinitTps();
}

TEST(PwmInputTps, implausiblePulseInvalidates) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	deinitTps();
	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	initTps();

	sendPulse(eth, 1500);
	ASSERT_TRUE(Sensor::get(SensorType::Tps1).Valid);

	sendPulse(eth, 3000);
	auto result = Sensor::get(SensorType::Tps1);
	EXPECT_FALSE(result.Valid);
	EXPECT_EQ(UnexpectedCode::High, result.Code);

	// Recovers on the next good pulse
	sendPulse(eth, 1500);
	EXPECT_TRUE(Sensor::get(SensorType::Tps1).Valid);

	deinitTps();
}

TEST(PwmInputTps, timesOutWhenPulsesStop) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	deinitTps();
	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	initTps();

	// The value is stamped at the falling edge
	auto& sensor = getPwmInputTpsForUnitTest();
	sensor.onEdge(getTimeNowNt(), true);
	eth.moveTimeForwardUs(1500);
	sensor.onEdge(getTimeNowNt(), false);
	ASSERT_TRUE(Sensor::get(SensorType::Tps1).Valid);

	eth.moveTimeForwardUs(MS2US(PWM_INPUT_TPS_TIMEOUT_MS) - 1000);
	EXPECT_TRUE(Sensor::get(SensorType::Tps1).Valid);

	eth.moveTimeForwardUs(2000);
	auto result = Sensor::get(SensorType::Tps1);
	EXPECT_FALSE(result.Valid);
	EXPECT_EQ(UnexpectedCode::Timeout, result.Code);

	deinitTps();
}

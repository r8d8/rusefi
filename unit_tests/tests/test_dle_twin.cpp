#include "pch.h"

#include "dle_twin.h"

// One narrow magnet pulse per revolution
static void runAtRpm(EngineTestHelper& eth, float rpm, int revolutions) {
	float periodMs = 60000 / rpm;
	float pulseMs = 0.3;
	for (int i = 0; i < revolutions; i++) {
		eth.smartFireRise(periodMs - pulseMs);
		eth.smartFireFall(pulseMs);
	}
}

template<typename TValue, int TSize>
static void expectStrictlyAscending(const TValue (&bins)[TSize]) {
	for (int i = 1; i < TSize; i++) {
		EXPECT_GT(bins[i], bins[i - 1]) << "index " << i;
	}
}

static void expectDleTwinCommon() {
	EXPECT_TRUE(engineConfiguration->twoStroke);
	EXPECT_EQ(1, engineConfiguration->cylindersCount);
	EXPECT_EQ(trigger_type_e::TT_NARROW_SINGLE_TOOTH, engineConfiguration->trigger.type);
	EXPECT_EQ(engine_load_mode_e::LM_ALPHA_N, engineConfiguration->fuelAlgorithm);
	EXPECT_TRUE(engineConfiguration->alphaNUseBaro);
	EXPECT_TRUE(engineConfiguration->alphaNUseIat);
	EXPECT_EQ(IM_ONE_COIL, engineConfiguration->ignitionMode);
	EXPECT_NEAR(1, engineConfiguration->tachPulsePerRev, EPS4D);

	expectStrictlyAscending(config->veLoadBins);
	expectStrictlyAscending(config->veRpmBins);
	expectStrictlyAscending(config->ignitionLoadBins);
	expectStrictlyAscending(config->ignitionRpmBins);
	expectStrictlyAscending(config->lambdaLoadBins);
	EXPECT_EQ(0, config->veLoadBins[0]);
	EXPECT_EQ(100, config->veLoadBins[efi::size(config->veLoadBins) - 1]);

	// Rich towards WOT for an air-cooled two-stroke
	EXPECT_NEAR(1.0, config->lambdaTable[0][0], 0.01);
	EXPECT_NEAR(0.85, config->lambdaTable[efi::size(config->lambdaLoadBins) - 1][0], 0.01);

	// Overspeed closes the throttle before the hard rev limit cuts
	EXPECT_LT(engineConfiguration->servoThrottleOverspeedRpm, engineConfiguration->rpmHardLimit);
}

// EngineTestHelper swaps fuelAlgorithm for a mock airmass model after applying the engine type,
// so configuration checks apply the preset directly and running tests restore alpha-N.

TEST(DleTwin, dle60Configuration) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	expectDleTwinCommon();
	EXPECT_NEAR(0.061, engineConfiguration->displacement, EPS4D);
	EXPECT_EQ(9000, engineConfiguration->rpmHardLimit);
}

TEST(DleTwin, dle120Configuration) {
	EngineTestHelper eth(engine_type_e::DLE_120_TWIN);
	setDle120Twin();
	expectDleTwinCommon();
	EXPECT_NEAR(0.120, engineConfiguration->displacement, EPS4D);
	EXPECT_EQ(8500, engineConfiguration->rpmHardLimit);
}

// The presets leave pins to the board; give the test an injector and a coil
static void assignTestPins(engine_configuration_s* c) {
	c->injectionPins[0] = Gpio::A1;
	c->ignitionPins[0] = Gpio::A2;
}

static void expectOneSparkAndInjectionPerRevolution(engine_type_e type) {
	EngineTestHelper eth(type, assignTestPins);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	Sensor::setMockValue(SensorType::Tps1, 30);

	// Starter, then run-up
	// (a jump from standstill straight to 6000 rpm makes the spark logic skip one dwell on purpose)
	runAtRpm(eth, 1500, 20);
	runAtRpm(eth, 3000, 30);
	runAtRpm(eth, 6000, 50);
	EXPECT_NEAR(6000, Sensor::getOrZero(SensorType::Rpm), 10);
	EXPECT_EQ(0, engine->triggerCentral.triggerState.totalTriggerErrorCounter);

	auto sparks = engine->engineState.globalSparkCounter;
	auto injections = engine->engineState.fuelInjectionCounter;
	runAtRpm(eth, 6000, 100);

	EXPECT_NEAR(100, engine->engineState.globalSparkCounter - sparks, 2);
	EXPECT_NEAR(100, engine->engineState.fuelInjectionCounter - injections, 2);
	EXPECT_EQ(0, engine->engineState.sparkOutOfOrderCounter);
	EXPECT_EQ(0, engine->triggerCentral.triggerState.totalTriggerErrorCounter);
}

TEST(DleTwin, dle60RunsOneSparkAndOneInjectionPerRevolution) {
	expectOneSparkAndInjectionPerRevolution(engine_type_e::DLE_60_TWIN);
}

TEST(DleTwin, dle120RunsOneSparkAndOneInjectionPerRevolution) {
	expectOneSparkAndInjectionPerRevolution(engine_type_e::DLE_120_TWIN);
}

// The suggested injector must deliver rated-power fuel below ~85% duty, and not be so large that
// it runs at a tiny duty
static float injectorDutyAtWot(engine_type_e type, float rpm) {
	EngineTestHelper eth(type, assignTestPins);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	Sensor::setMockValue(SensorType::Tps1, 100);
	runAtRpm(eth, 1500, 20);
	runAtRpm(eth, rpm, 100);
	EXPECT_NEAR(rpm, Sensor::getOrZero(SensorType::Rpm), 20);

	// Two-stroke: one injection per revolution
	float revolutionMs = 60000 / rpm;
	return engine->engineState.injectionDuration / revolutionMs;
}

TEST(DleTwin, dle60InjectorDutyAtRatedPower) {
	float duty = injectorDutyAtWot(engine_type_e::DLE_60_TWIN, 8500);
	EXPECT_LT(duty, 0.85);
	EXPECT_GT(duty, 0.40);
}

TEST(DleTwin, dle120InjectorDutyAtRatedPower) {
	float duty = injectorDutyAtWot(engine_type_e::DLE_120_TWIN, 8000);
	EXPECT_LT(duty, 0.85);
	EXPECT_GT(duty, 0.40);
}

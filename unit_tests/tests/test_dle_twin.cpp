#include "pch.h"

#include "dle_twin.h"

#include "can_msg_tx.h"
#include "trip_odometer.h"

#include <array>
#include <map>

template<typename TValue, int TSize>
static void expectStrictlyAscending(const TValue (&bins)[TSize]) {
	for (int i = 1; i < TSize; i++) {
		EXPECT_GT(bins[i], bins[i - 1]) << "index " << i;
	}
}

static void expectDleTwinCommon() {
	EXPECT_TRUE(engineConfiguration->twoStroke);
	EXPECT_EQ(1, engineConfiguration->cylindersCount);
	EXPECT_EQ(trigger_type_e::TT_TOOTHED_WHEEL_36_1, engineConfiguration->trigger.type);
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

	// Verbose CAN broadcast = engine status for the hybrid controller
	EXPECT_TRUE(engineConfiguration->canWriteEnabled);
	EXPECT_TRUE(engineConfiguration->enableVerboseCanTx);
	EXPECT_EQ(0x200u, engineConfiguration->verboseCanBaseAddress);
	EXPECT_FALSE(engineConfiguration->rusefiVerbose29b);
	EXPECT_EQ(50, engineConfiguration->canSleepPeriodMs);

	// Throttle request, governor target and run/stop from the hybrid controller's HCU_ENGINE_CMD
	EXPECT_EQ(ServoThrottleRequestSource::Can, engineConfiguration->servoThrottleRequestSource);
	EXPECT_EQ(0x1A0, engineConfiguration->servoThrottleCanId);
	EXPECT_TRUE(engineConfiguration->canReadEnabled);
	// CAN user control: engine stop only
	EXPECT_TRUE(engineConfiguration->canUserControlStopOnly);
	// Above the HCU's 12% idle feed-forward, so idle / interlock off / autorotation drop the governor
	EXPECT_EQ(20, engineConfiguration->servoGovernorMinRequest);
	// rusEFI governs; gains from the SITL sweep, engage window wide enough for a loaded run-up
	EXPECT_TRUE(engineConfiguration->servoGovernorEnabled);
	EXPECT_NEAR(0.04, engineConfiguration->servoGovernorPid.pFactor, 1e-6);
	EXPECT_NEAR(0.1, engineConfiguration->servoGovernorPid.iFactor, 1e-6);
	EXPECT_NEAR(0, engineConfiguration->servoGovernorPid.dFactor, 1e-6);
	EXPECT_EQ(-20, engineConfiguration->servoGovernorPid.minValue);
	EXPECT_EQ(20, engineConfiguration->servoGovernorPid.maxValue);
	EXPECT_EQ(600, engineConfiguration->servoGovernorEngageWindow);
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

// Rising edge at the start of each present tooth, falling edge half a tooth pitch later. Speed ramps
// linearly from fromRpm to toRpm over the revolutions; edge times are accumulated exactly so that
// microsecond rounding does not bias the speed at high RPM.
struct WheelSpinner {
	EngineTestHelper& eth;
	int totalTeeth;
	int missingTeeth;
	double timeUs = 0;
	double firedUs = 0;

	void advance(double us) {
		timeUs += us;
		int step = (int)(timeUs - firedUs);
		eth.moveTimeForwardAndInvokeEventsUs(step);
		firedUs += step;
	}

	void spin(float fromRpm, float toRpm, int revolutions) {
		for (int r = 0; r < revolutions; r++) {
			float rpm = fromRpm + (toRpm - fromRpm) * (r + 1) / revolutions;
			double halfToothUs = 60e6 / rpm / totalTeeth / 2;
			for (int tooth = 0; tooth < totalTeeth; tooth++) {
				bool present = tooth < totalTeeth - missingTeeth;
				advance(halfToothUs);
				if (present) {
					eth.firePrimaryTriggerRise();
				}
				advance(halfToothUs);
				if (present) {
					eth.firePrimaryTriggerFall();
				}
			}
		}
	}
};

// The suggested injector must deliver rated-power fuel below ~85% duty, and not be so large that
// it runs at a tiny duty
static float injectorDutyAtWot(engine_type_e type, float rpm) {
	EngineTestHelper eth(type, assignTestPins);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	Sensor::setMockValue(SensorType::Tps1, 100);

	// The preset's 36-1 wheel
	WheelSpinner spinner{eth, 36, 1};
	spinner.spin(1000, 1000, 20);
	spinner.spin(1000, rpm, 80);
	spinner.spin(rpm, rpm, 20);
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

// ---- Crank trigger wheels: the presets' 36-1, and 60-2 as the alternative ----

// One spark and one injection per revolution (two-stroke) from starter speed to rated RPM
static void expectWheelRunsCleanly(engine_type_e type, trigger_type_e wheel, int totalTeeth, int missingTeeth, float maxRpm) {
	EngineTestHelper eth(type, assignTestPins);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	if (engineConfiguration->trigger.type != wheel) {
		eth.setTriggerType(wheel);
	}
	Sensor::setMockValue(SensorType::Tps1, 30);

	WheelSpinner spinner{eth, totalTeeth, missingTeeth};
	// Starter speed, then run-up in realistic ramps (the engine cannot triple its speed in one turn)
	spinner.spin(1000, 1000, 20);
	float previousRpm = 1000;
	for (float rpm : { 1000.0f, 3000.0f, 6000.0f, maxRpm }) {
		spinner.spin(previousRpm, rpm, 40);
		previousRpm = rpm;
		spinner.spin(rpm, rpm, 10);
		EXPECT_NEAR(rpm, Sensor::getOrZero(SensorType::Rpm), rpm * 0.002) << "at " << rpm;

		auto sparks = engine->engineState.globalSparkCounter;
		auto injections = engine->engineState.fuelInjectionCounter;
		spinner.spin(rpm, rpm, 50);
		EXPECT_NEAR(50, engine->engineState.globalSparkCounter - sparks, 1) << "at " << rpm;
		EXPECT_NEAR(50, engine->engineState.fuelInjectionCounter - injections, 1) << "at " << rpm;
	}

	EXPECT_EQ(0, engine->triggerCentral.triggerState.totalTriggerErrorCounter);
	EXPECT_EQ(0, engine->engineState.sparkOutOfOrderCounter);
}

TEST(DleTwin, dle60With36minus1Wheel) {
	expectWheelRunsCleanly(engine_type_e::DLE_60_TWIN, trigger_type_e::TT_TOOTHED_WHEEL_36_1, 36, 1, 8500);
}

TEST(DleTwin, dle60With60minus2Wheel) {
	expectWheelRunsCleanly(engine_type_e::DLE_60_TWIN, trigger_type_e::TT_TOOTHED_WHEEL_60_2, 60, 2, 8500);
}

TEST(DleTwin, dle120With36minus1Wheel) {
	expectWheelRunsCleanly(engine_type_e::DLE_120_TWIN, trigger_type_e::TT_TOOTHED_WHEEL_36_1, 36, 1, 8000);
}

TEST(DleTwin, dle120With60minus2Wheel) {
	expectWheelRunsCleanly(engine_type_e::DLE_120_TWIN, trigger_type_e::TT_TOOTHED_WHEEL_60_2, 60, 2, 8000);
}

namespace {

CANDriver verboseCan;
std::map<uint32_t, std::array<uint8_t, 8>> verboseFrames;

msg_t captureVerboseFrame(CANDriver*, canmbx_t, CANTxFrame* frame, can_sysinterval_t) {
	EXPECT_EQ(CAN_IDE_STD, frame->IDE);
	std::array<uint8_t, 8> data{};
	memcpy(data.data(), frame->data8, data.size());
	verboseFrames[CAN_ID(*frame)] = data;
	return MSG_OK;
}

int u16At(uint32_t id, int offset) {
	const auto& d = verboseFrames.at(id);
	return d[offset] | (d[offset + 1] << 8);
}

int s16At(uint32_t id, int offset) {
	return static_cast<int16_t>(u16At(id, offset));
}

} // namespace

/**
 * The hybrid controller (r8d8/hybrid_ctrl, firmware/src/rusefi_link.c) decodes these bytes of the
 * verbose broadcast and forwards them to the autopilot. A layout change here breaks it: update both.
 */
TEST(DleTwin, verboseCanLayoutForHybridController) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	engine->allowCanTx = true;
	canTransmitMock = captureVerboseFrame;
	CanTxMessage::setDevice(0, &verboseCan);
	verboseFrames.clear();

	Sensor::setMockValue(SensorType::Rpm, 7250);
	Sensor::setMockValue(SensorType::Tps1, 42.5);
	Sensor::setMockValue(SensorType::Map, 85.5);
	Sensor::setMockValue(SensorType::Clt, 152);
	Sensor::setMockValue(SensorType::Iat, 23);
	Sensor::setMockValue(SensorType::BatteryVoltage, 12.4);
	Sensor::setMockValue(SensorType::Lambda1, 0.87);
	Sensor::setMockValue(SensorType::EGT1, 610);
	Sensor::setMockValue(SensorType::EGT2, 640);
	engine->engineState.warnings.warningCounter = 3;
	engine->engineState.warnings.lastErrorCode = ObdCode::OBD_Map_Timeout;
	engine->engineState.timingAdvance[0] = 24.5;
	engine->outputChannels.actualLastInjection = 2.4;
#ifdef MODULE_ODOMETER
	engine->module<TripOdometer>()->consumeFuel(37.5, getTimeNowNt());
#endif

	void sendCanVerbose();
	sendCanVerbose();
	while (CanTxMessage::serviceOne(0)) {
	}
	CanTxMessage::removeDevice(0);
	canTransmitMock = nullptr;

	ASSERT_EQ(12u, verboseFrames.size());
	// 0x200: warning counter, last error code, bit 0 of byte 4 = fuel or spark cut active
	EXPECT_EQ(3, u16At(0x200, 0));
	EXPECT_EQ(static_cast<int>(ObdCode::OBD_Map_Timeout), u16At(0x200, 2));
	EXPECT_EQ(0, verboseFrames.at(0x200)[4] & 1);
	// 0x201: RPM, ignition timing x50
	EXPECT_EQ(7250, u16At(0x201, 0));
	EXPECT_EQ(1225, s16At(0x201, 2));
	// 0x202: TPS1 (the servo command) x100
	EXPECT_EQ(4250, s16At(0x202, 2));
	// 0x203: MAP x30, CLT and IAT +40
	EXPECT_EQ(2565, u16At(0x203, 0));
	EXPECT_EQ(192, verboseFrames.at(0x203)[2]);
	EXPECT_EQ(63, verboseFrames.at(0x203)[3]);
	// 0x204: battery x1000
	EXPECT_EQ(12400, u16At(0x204, 6));
	// 0x205: injector pulse x300
	EXPECT_EQ(720, u16At(0x205, 4));
#ifdef MODULE_ODOMETER
	// 0x206: fuel used, whole grams
	EXPECT_EQ(37, u16At(0x206, 0));
#endif
	// 0x207: lambda 1 x10000
	EXPECT_EQ(8700, u16At(0x207, 0));
	// 0x209: EGT 1 and 2 / 5
	EXPECT_EQ(122, verboseFrames.at(0x209)[0]);
	EXPECT_EQ(128, verboseFrames.at(0x209)[1]);
}

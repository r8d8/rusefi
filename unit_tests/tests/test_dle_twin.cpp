#include "pch.h"

#include "dle_twin.h"
#include "thermistor_func.h"

#include "can_msg_tx.h"
#include "trip_odometer.h"
#include "ignition_controller.h"
#include "servo_throttle.h"
#include "servo_throttle_can.h"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

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
	EXPECT_EQ(1000, engineConfiguration->servoGovernorEngageWindow);
	// The reference ramps from the engagement RPM instead of saturating the trim at once
	EXPECT_EQ(500, engineConfiguration->servoGovernorEngageRampRpmPerS);
	// A request lost in the run-up drops to the HCU's idle feed-forward (clearwater HZ-39)
	EXPECT_EQ(12, engineConfiguration->servoThrottleLostRequestIdle);

	// Injector duty cut out of reach: it releases only below 20 % duty, which this engine never
	// reaches while running, so a trip would cut fuel for the rest of the flight
	EXPECT_EQ(200, engineConfiguration->maxInjectorDutyInstant);
	EXPECT_EQ(120, engineConfiguration->maxInjectorDutySustained);
}

// EngineTestHelper swaps fuelAlgorithm for a mock airmass model after applying the engine type,
// so configuration checks apply the preset directly and running tests restore alpha-N.

TEST(DleTwin, dle60Configuration) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	expectDleTwinCommon();
	EXPECT_NEAR(0.061, engineConfiguration->displacement, EPS4D);
	EXPECT_EQ(9000, engineConfiguration->rpmHardLimit);

	// One coil output; the trailing output unused
	EXPECT_FALSE(engineConfiguration->enableTrailingSparks);
	EXPECT_EQ(100, engineConfiguration->injector.flow);

	// Stage 1.0 head temperatures: 47 kOhm B3950 on CLT and AuxTemp1, the B-equation's curve
	for (ThermistorConf* sensor : { &engineConfiguration->clt, &engineConfiguration->auxTempSensor1 }) {
		ThermistorFunc func;
		func.configure(sensor->config);
		EXPECT_NEAR(-7, func.convert(231143).Value, 0.1);
		EXPECT_NEAR(25, func.convert(47000).Value, 0.1);
		EXPECT_NEAR(150, func.convert(938.5).Value, 0.1);
		EXPECT_NEAR(240, func.convert(182.5).Value, 0.2);
		EXPECT_FALSE(func.convert(140).Valid) << "258 C: above the 250 C validation limit";
	}
}

TEST(DleTwin, dle120Configuration) {
	EngineTestHelper eth(engine_type_e::DLE_120_TWIN);
	setDle120Twin();
	expectDleTwinCommon();
	EXPECT_NEAR(0.120, engineConfiguration->displacement, EPS4D);
	EXPECT_EQ(8500, engineConfiguration->rpmHardLimit);
	// The two-tower coil on one output
	EXPECT_FALSE(engineConfiguration->enableTrailingSparks);
}

// The presets leave pins to the board; give the test an injector and a coil
static void assignTestPins(engine_configuration_s* c) {
	c->injectionPins[0] = Gpio::A1;
	c->ignitionPins[0] = Gpio::A2;
}

// The coil outputs' edges with their times (the stage 1.0 ignition tests at the end)
struct CoilEdges {
	struct Edge {
		efitimeus_t us;
		bool high;
	};

	struct Output {
		const OutputPin& pin;
		bool high;
		std::vector<Edge> edges;

		explicit Output(const OutputPin& output) : pin(output), high(output.getLogicValue()) {
		}

		void sample() {
			if (pin.getLogicValue() != high) {
				high = !high;
				edges.push_back({ getTimeNowUs(), high });
			}
		}

		int rises() const {
			return std::count_if(edges.begin(), edges.end(), [](const Edge& edge) { return edge.high; });
		}
	};

	Output coil{ enginePins.coils[0] };
	Output trailing{ enginePins.trailingCoils[0] };

	void sample() {
		coil.sample();
		trailing.sample();
	}
};

// Rising edge at the start of each present tooth, falling edge half a tooth pitch later. Speed ramps
// linearly from fromRpm to toRpm over the revolutions; edge times are accumulated exactly so that
// microsecond rounding does not bias the speed at high RPM.
struct WheelSpinner {
	EngineTestHelper& eth;
	int totalTeeth;
	int missingTeeth;
	double timeUs = 0;
	double firedUs = 0;
	// When set, the scheduled events run one at a time, each at its own time, and the coils are
	// sampled after each: every edge gets its time
	CoilEdges* coils = nullptr;
	// Stop the wheel as soon as coil 1 charges: the trigger lost mid-dwell
	bool stopInDwell = false;

	void advance(double us) {
		timeUs += us;
		int step = (int)(timeUs - firedUs);
		firedUs += step;
		if (!coils) {
			eth.moveTimeForwardAndInvokeEventsUs(step);
			return;
		}
		efitick_t targetNt = getTimeNowNt() + US2NT(step);
		while (scheduling_s* next = engine->scheduler.getHead()) {
			if (next->getMomentNt() > targetNt) {
				break;
			}
			eth.setTimeAndInvokeEventsNt(next->getMomentNt());
			coils->sample();
		}
		eth.setTimeAndInvokeEventsNt(targetNt);
		coils->sample();
	}

	bool stopped() {
		if (!coils) {
			return false;
		}
		coils->sample();
		return stopInDwell && coils->coil.high;
	}

	void spin(float fromRpm, float toRpm, int revolutions) {
		for (int r = 0; r < revolutions; r++) {
			float rpm = fromRpm + (toRpm - fromRpm) * (r + 1) / revolutions;
			double halfToothUs = 60e6 / rpm / totalTeeth / 2;
			for (int tooth = 0; tooth < totalTeeth; tooth++) {
				bool present = tooth < totalTeeth - missingTeeth;
				advance(halfToothUs);
				if (stopped()) {
					return;
				}
				if (present) {
					eth.firePrimaryTriggerRise();
				}
				if (stopped()) {
					return;
				}
				advance(halfToothUs);
				if (stopped()) {
					return;
				}
				if (present) {
					eth.firePrimaryTriggerFall();
				}
				if (stopped()) {
					return;
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

void sendCanVerbose();

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

// One verbose broadcast into verboseFrames
void captureVerboseBroadcast() {
	engine->allowCanTx = true;
	canTransmitMock = captureVerboseFrame;
	CanTxMessage::setDevice(0, &verboseCan);
	verboseFrames.clear();

	sendCanVerbose();
	while (CanTxMessage::serviceOne(0)) {
	}
	CanTxMessage::removeDevice(0);
	canTransmitMock = nullptr;
}

} // namespace

/**
 * The hybrid controller (r8d8/hybrid_ctrl, firmware/src/rusefi_link.c) decodes these bytes of the
 * verbose broadcast and forwards them to the autopilot. A layout change here breaks it: update both.
 */
TEST(DleTwin, verboseCanLayoutForHybridController) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();

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

	captureVerboseBroadcast();

	// 0x200 - 0x20B, and the fork's 0x20C with the CAN request source
	ASSERT_EQ(13u, verboseFrames.size());
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

/**
 * Clearwater HZ-43: Sensors1 carries the head temperatures (CHT 1 on CLT, CHT 2 on AuxTemp1) as a
 * uint8 + 40, saturated at 215 C, and a failed sensor (open, or above the thermistor's 250 C
 * validation limit) as 0 C. The fork's 0x20C carries them in 0.1 C with a failed mark.
 */
TEST(DleTwin, verboseCanHeadTemperatures) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();

	// 215 C is the top of the byte
	Sensor::setMockValue(SensorType::Clt, 215);
	Sensor::setMockValue(SensorType::AuxTemp1, 180);
	captureVerboseBroadcast();
	EXPECT_EQ(255, verboseFrames.at(0x203)[2]);
	EXPECT_EQ(220, verboseFrames.at(0x203)[4]);
	EXPECT_EQ(2150, s16At(0x20C, 0));
	EXPECT_EQ(1800, s16At(0x20C, 2));

	// Above it Sensors1 stays at the top of the byte; 0x20C has the temperature
	Sensor::setMockValue(SensorType::Clt, 230.04);
	captureVerboseBroadcast();
	EXPECT_EQ(255, verboseFrames.at(0x203)[2]);
	EXPECT_EQ(2300, s16At(0x20C, 0));

	// and below -40 C at the bottom
	Sensor::setMockValue(SensorType::Clt, -50.06);
	captureVerboseBroadcast();
	EXPECT_EQ(0, verboseFrames.at(0x203)[2]);
	EXPECT_EQ(-501, s16At(0x20C, 0));

	// A failed sensor goes out as 0 C
	Sensor::resetMockValue(SensorType::Clt);
	Sensor::resetMockValue(SensorType::AuxTemp1);
	ASSERT_FALSE(Sensor::get(SensorType::Clt).Valid);
	ASSERT_FALSE(Sensor::get(SensorType::AuxTemp1).Valid);
	captureVerboseBroadcast();
	EXPECT_EQ(40, verboseFrames.at(0x203)[2]);
	EXPECT_EQ(40, verboseFrames.at(0x203)[4]);
	// 0x20C marks it failed
	EXPECT_EQ(INT16_MIN, s16At(0x20C, 0));
	EXPECT_EQ(INT16_MIN, s16At(0x20C, 2));
}

/**
 * 0x20C byte 4: the HCU_ENGINE_CMD state, FF_HELD, the feed-forward live, the run-up done, the
 * ignition state and the overspeed entries modulo 4; byte 5 the governor state; bytes 6 / 7 the fuel
 * and spark cut reasons.
 */
TEST(DleTwin, hybridStatusFrame) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	getHcuEngineCommandListener().reset();
	engine->module<ServoThrottle>()->reset();

	// Nothing yet: no command, governor off, ignition off (no battery voltage seen), not cut
	captureVerboseBroadcast();
	EXPECT_EQ(0, verboseFrames.at(0x20C)[4]);
	EXPECT_EQ(0, verboseFrames.at(0x20C)[5]);
	EXPECT_EQ(0, verboseFrames.at(0x20C)[6]);
	EXPECT_EQ(0, verboseFrames.at(0x20C)[7]);

	// A fresh command with FF_HELD, the governor holding the target, ignition on
	CANRxFrame frame{};
	frame.IDE = CAN_IDE_STD;
	frame.RTR = CAN_RTR_DATA;
	frame.SID = HCU_ENGINE_CMD_DEFAULT_ID;
	frame.DLC = 8;
	frame.data8[0] = 0x01 | 0x02 | 0x04; // RUN, GOV_ON, FF_HELD
	frame.data8[1] = HCU_ENGINE_CMD_VERSION << 4;
	frame.data8[4] = 4700 >> 8;
	frame.data8[5] = 4700 & 0xFF;
	frame.data8[7] = hcuEngineCmdCrc(frame.data8, 7);
	getHcuEngineCommandListener().processFrame(/*busIndex*/0, frame, getTimeNowNt());
	engine->module<ServoThrottle>()->update(47, 7650, 0.005f, 7650, true, /*feedForwardHeld*/ true);
	ASSERT_EQ(ServoGovernorState::Governing, engine->module<ServoThrottle>()->getState());
	Sensor::setMockValue(SensorType::BatteryVoltage, 12.4);
	engine->module<IgnitionController>()->onSlowCallback();
	ASSERT_TRUE(engine->module<IgnitionController>()->getIgnState());
	captureVerboseBroadcast();
	// fresh (1), FF_HELD, not live (held), run-up done, ignition on
	EXPECT_EQ(0x01 | 0x04 | 0x10 | 0x20, verboseFrames.at(0x20C)[4]);
	EXPECT_EQ(static_cast<int>(ServoGovernorState::Governing), verboseFrames.at(0x20C)[5]);

	// Injection disabled in the settings: the fuel cut reason
	engineConfiguration->isInjectionEnabled = false;
	engine->module<LimpManager>()->updateState(7650, getTimeNowNt());
	captureVerboseBroadcast();
	EXPECT_EQ(static_cast<int>(ClearReason::Settings), verboseFrames.at(0x20C)[6]);
	EXPECT_EQ(0, verboseFrames.at(0x20C)[7]);

	// Five overspeeds (above 8700 rpm, released at the 7650 target): 5 modulo 4 in bits 6-7, and
	// reset() keeps the count, so the hybrid controller can count by difference
	for (int i = 0; i < 5; i++) {
		engine->module<ServoThrottle>()->update(47, 8800, 0.005f, 7650, true);
		ASSERT_EQ(ServoGovernorState::Overspeed, engine->module<ServoThrottle>()->getState());
		engine->module<ServoThrottle>()->update(47, 8800, 0.005f, 7650, true);
		engine->module<ServoThrottle>()->update(47, 7500, 0.005f, 7650, true);
		ASSERT_NE(ServoGovernorState::Overspeed, engine->module<ServoThrottle>()->getState());
	}
	engine->module<ServoThrottle>()->reset();
	EXPECT_EQ(5, engine->module<ServoThrottle>()->getOverspeedEntries());
	captureVerboseBroadcast();
	EXPECT_EQ(0x40, verboseFrames.at(0x20C)[4] & 0xC0);
	// The values the hybrid controller names
	static_assert(static_cast<int>(ClearReason::StopRequested) == 7);
	static_assert(static_cast<int>(ClearReason::IgnitionOff) == 14);
	static_assert(static_cast<int>(ServoGovernorState::Overspeed) == 3);
	static_assert(static_cast<int>(HcuCommandState::Held) == 2);
}

// No 0x20C without the CAN request source
TEST(DleTwin, hybridStatusFrameOnlyWithTheCanRequest) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	engineConfiguration->servoThrottleRequestSource = ServoThrottleRequestSource::PwmInput;
	captureVerboseBroadcast();
	EXPECT_EQ(12u, verboseFrames.size());
	EXPECT_EQ(0u, verboseFrames.count(0x20C));
}

/**
 * Clearwater HZ-44: what the DLE preset's held key input on uaEFI takes away. Without a key input
 * the ignition follows the battery voltage: above 6.0 V it is on; 0.2 s after the last slow callback
 * that saw more than 6 V it goes off (fuel and spark cut, ClearReason::IgnitionOff), so a dip of
 * 0.15 - 0.25 s at the 20 Hz callback can cut; it is back on at the first callback above 6 V; and a
 * failed battery-voltage reading counts as 0 V.
 */
TEST(DleTwin, ignitionFollowsBatteryVoltageWithoutAKeyInput) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	auto ignitionOn = [] {
		return engine->module<IgnitionController>()->getIgnState();
	};
	auto slowCallbackAfterMs = [&](float ms) {
		eth.moveTimeForwardMs(ms);
		engine->module<IgnitionController>()->onSlowCallback();
	};

	Sensor::setMockValue(SensorType::BatteryVoltage, 12.4);
	slowCallbackAfterMs(50);
	ASSERT_TRUE(ignitionOn());

	// 6.0 V for 0.15 s, then 6.1 V: on throughout
	Sensor::setMockValue(SensorType::BatteryVoltage, 6.0);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(50);
	EXPECT_TRUE(ignitionOn());
	Sensor::setMockValue(SensorType::BatteryVoltage, 6.1);
	slowCallbackAfterMs(50);
	EXPECT_TRUE(ignitionOn());

	// 5.9 V: off once 0.2 s have passed since the last callback above 6 V
	Sensor::setMockValue(SensorType::BatteryVoltage, 5.9);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(50);
	EXPECT_TRUE(ignitionOn());
	slowCallbackAfterMs(60);
	EXPECT_FALSE(ignitionOn());

	// back above 6 V: on at the next callback
	Sensor::setMockValue(SensorType::BatteryVoltage, 6.1);
	slowCallbackAfterMs(50);
	EXPECT_TRUE(ignitionOn());

	// A failed battery-voltage reading counts as 0 V
	Sensor::resetMockValue(SensorType::BatteryVoltage);
	ASSERT_FALSE(Sensor::get(SensorType::BatteryVoltage).Valid);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(50);
	slowCallbackAfterMs(60);
	EXPECT_FALSE(ignitionOn());

	// Unit tests are no uaEFI: the preset leaves the key input to the board here
	EXPECT_FALSE(isBrainPinValid(engineConfiguration->ignitionKeyDigitalPin));
}

/**
 * Clearwater stage 1.0 run-up (simulation 2026-10-23, SITL): on the derated DLE-60 with an 8 m/s
 * crosswind the autopilot's throttle curve settles the engine at 6881 rpm against the HCU's 7489 rpm
 * target. The governor engages only within servoGovernorEngageWindow under the target: with 600 rpm
 * it never engaged and the rotor hung at 92 %, under the autopilot's run-up gate. The DLE presets
 * engage within 1000 rpm.
 */
TEST(DleTwin, governorEngagesFromTheOpenLoopRunUp) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();
	engine->module<ServoThrottle>()->reset();

	// Just outside the window: still following the feed-forward
	for (int i = 0; i < 200; i++) {
		engine->module<ServoThrottle>()->update(51, 6488, 0.005f, 7489, true);
	}
	EXPECT_EQ(ServoGovernorState::Following, engine->module<ServoThrottle>()->getState());
	EXPECT_NEAR(51, engine->module<ServoThrottle>()->getCommandPercent(), 1e-3);

	// Where the crosswind run-up settled: governing, the trim opening the throttle towards the target
	for (int i = 0; i < 200; i++) {
		engine->module<ServoThrottle>()->update(51, 6881, 0.005f, 7489, true);
	}
	EXPECT_EQ(ServoGovernorState::Governing, engine->module<ServoThrottle>()->getState());
	EXPECT_GT(engine->module<ServoThrottle>()->getCommandPercent(), 51 + 10);
}

/**
 * Clearwater stage 1.0 starter (design 2026-10-27): a T-Motor AT4120 KV250 on 12S through 12T/97T, 8.083:1,
 * turns the DLE-60 at ~1050 rpm under load and up to ~1560 rpm free. rusEFI leaves cranking for running the first
 * time RPM reaches cranking.rpm and stays running while the engine turns: at 1000 the starter alone put
 * it on running fuel and timing before the engine had fired. The DLE-60 preset cranks up to 1700.
 */
TEST(DleTwin, dle60StarterSpeedStaysCranking) {
	EngineTestHelper eth(engine_type_e::DLE_60_TWIN);
	setDle60Twin();

	EXPECT_EQ(1700, engineConfiguration->cranking.rpm);
	engine->rpmCalculator.setRpmValue(0);
	// The starter under load, a compression slowing it, and its no-load limit: cranking throughout
	for (float rpm : { 1050.0f, 800.0f, 1500.0f, 1560.0f }) {
		engine->rpmCalculator.setRpmValue(rpm);
		EXPECT_TRUE(engine->rpmCalculator.isCranking()) << rpm;
		EXPECT_FALSE(engine->rpmCalculator.isRunning()) << rpm;
	}
	// Fired and past the starter: running, and an idle below 1700 stays running
	engine->rpmCalculator.setRpmValue(1800);
	EXPECT_TRUE(engine->rpmCalculator.isRunning());
	engine->rpmCalculator.setRpmValue(1500);
	EXPECT_TRUE(engine->rpmCalculator.isRunning());
}

// ---- Stage 1.0 ignition: a passive coil per cylinder (clearwater, user decision 2026-10-27) ----

/**
 * The DLE-60 on the preset's 36-1 wheel, both coil outputs recorded. Stage 1.0 fires each cylinder's
 * plug from its own passive single-fire coil on uaEFI's IGBTs: coil 1 on the main output, coil 2 on
 * rusEFI's trailing output at 0 degrees, which charges from the main dwell's start and fires from
 * the main spark.
 */
struct Dle60Bench {
	EngineTestHelper eth{ engine_type_e::DLE_60_TWIN, assignTestPins };
	WheelSpinner spinner{ eth, 36, 1 };

	Dle60Bench() {
		engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
		Sensor::setMockValue(SensorType::Tps1, 30);
	}

	// From starter speed in ramps the decoder follows, then steady
	void runUpTo(float rpm) {
		spinner.spin(1000, 1000, 20);
		if (rpm > 1000) {
			spinner.spin(1000, rpm, 40 * (int)std::ceil((rpm - 1000) / 2500));
			spinner.spin(rpm, rpm, 10);
		}
	}

	void record(CoilEdges& edges, float rpm, int revolutions) {
		spinner.coils = &edges;
		spinner.spin(rpm, rpm, revolutions);
		spinner.coils = nullptr;
	}
};

// rusEFI's trailing spark at 0 degrees, the way stage 1.0 uses it
static void setTrailingSparkAtZero() {
	engineConfiguration->enableTrailingSparks = true;
	setTable(config->trailingSparkTable, 0);
}

static void expectDwellUs(const CoilEdges::Output& output, float dwellUs, float rpm) {
	for (size_t i = 0; i + 1 < output.edges.size(); i++) {
		if (output.edges[i].high) {
			EXPECT_NEAR(dwellUs, output.edges[i + 1].us - output.edges[i].us, 50) << rpm << " edge " << i;
		}
	}
}

TEST(DleTwin, dle60SecondCoil) {
	// Starter speed (cranking up to 1700), the run-up and the governed speed
	for (float rpm : { 1000.0f, 3000.0f, 7500.0f }) {
		Dle60Bench bench;
		bench.runUpTo(rpm);
		CoilEdges edges;
		bench.record(edges, rpm, 20);

		// Coil 1: a spark a revolution, 6 ms of dwell cranking and 4 ms running
		EXPECT_NEAR(20, edges.coil.rises(), 1) << rpm;
		expectDwellUs(edges.coil, rpm < 1700 ? 6000 : 4000, rpm);
		// The preset drives one coil: the trailing output stays idle
		EXPECT_TRUE(edges.trailing.edges.empty()) << rpm;
	}
}

// rusEFI cuts a spark by not starting its dwell, and the trailing coil charges from the main dwell:
// every spark cut cuts both coils and leaves neither charged. The ignition-off cut (HZ-44) is not
// compiled into unit tests; it clears the same spark permission.
TEST(DleTwin, everySparkCutCutsBothCoils) {
	std::pair<const char*, void (*)()> cuts[] = {
		{ "hard rev limit", [] { engineConfiguration->rpmHardLimit = 2500; } },
		{ "engine stop", [] { doScheduleStopEngine(StopRequestedReason::Console); } },
		{ "Lua spark cut", [] { engine->engineState.lua.luaIgnCut = true; } },
	};
	for (auto [name, cut] : cuts) {
		Dle60Bench bench;
		setTrailingSparkAtZero();
		bench.runUpTo(3000);
		CoilEdges before;
		bench.record(before, 3000, 5);
		EXPECT_NEAR(5, before.coil.rises(), 1) << name;
		EXPECT_EQ(before.coil.rises(), before.trailing.rises()) << name;

		cut();
		// A dwell started before the cut still ends in its spark
		bench.spinner.spin(3000, 3000, 2);
		CoilEdges during;
		bench.record(during, 3000, 10);
		EXPECT_EQ(0, during.coil.rises()) << name;
		EXPECT_EQ(0, during.trailing.rises()) << name;
		EXPECT_FALSE(during.coil.high) << name;
		EXPECT_FALSE(during.trailing.high) << name;
	}
}

// The trigger lost while both coils charge (a sensor or wiring fault): the main spark waits for a
// tooth that never comes, rusEFI's over-dwell protection fires coil 1 at 1.5 x the dwell, and the
// trailing spark is scheduled from that firing, so coil 2 is released with it
TEST(DleTwin, trailingCoilReleasedWhenTheTriggerStopsInDwell) {
	Dle60Bench bench;
	setTrailingSparkAtZero();
	bench.runUpTo(3000);

	CoilEdges edges;
	bench.spinner.coils = &edges;
	bench.spinner.stopInDwell = true;
	bench.spinner.spin(3000, 3000, 2);
	ASSERT_TRUE(edges.coil.high);
	ASSERT_TRUE(edges.trailing.high);
	auto overDwells = engine->engineState.overDwellCanceledCounter;

	// No more teeth
	bench.spinner.advance(100'000);
	EXPECT_FALSE(edges.coil.high);
	EXPECT_FALSE(edges.trailing.high);
	EXPECT_EQ(overDwells + 1, engine->engineState.overDwellCanceledCounter);
	ASSERT_EQ(2u, edges.coil.edges.size());
	ASSERT_EQ(2u, edges.trailing.edges.size());
	EXPECT_NEAR(1.5 * 4000, edges.coil.edges[1].us - edges.coil.edges[0].us, 50);
	EXPECT_EQ(edges.coil.edges[0].us, edges.trailing.edges[0].us);
	EXPECT_EQ(edges.coil.edges[1].us, edges.trailing.edges[1].us);
}

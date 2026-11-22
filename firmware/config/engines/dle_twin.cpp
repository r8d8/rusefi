/**
 * @file dle_twin.cpp
 *
 * DLE two-stroke boxer twins (DLE-60, DLE-120) converted to fuel injection, as used on a hybrid
 * helicopter: throttle body with one injector, rusEFI-driven ignition, throttle servo driven by
 * rusEFI from the hybrid controller's CAN command (see servo_throttle.h).
 *
 * Engine model: both pistons of the boxer reach TDC together and the stock DLE "TWIN" ignition
 * fires both plugs at once from one hub magnet. For fueling and spark scheduling that is one
 * two-stroke cylinder of the full displacement: cylindersCount = 1, one spark a revolution. The
 * DLE-120 drives a two-tower (wasted-spark style) coil that fires both plugs in series from one
 * output; the DLE-60 a passive coil per plug, the second on rusEFI's trailing output at 0 degrees
 * (setDle60Twin). Confirm simultaneous firing on the actual engine with a timing light on both plugs.
 *
 * Trigger: a 36-1 crank wheel fitted to the engine, read by a Hall gear-tooth sensor
 * (TT_TOOTHED_WHEEL_36_1, two-stroke cycle = 360 degrees). 36-1 rather than 60-2: both decode
 * equally well in two-stroke mode (see test_dle_twin.cpp), and on a small wheel 36-1 gives ~1.7x
 * larger teeth, which suits the sensor's minimum tooth size and air gap. The stock hub magnet and
 * DLE ignition module are not used. One wheel serves both engines: 80 mm OD steel ring, 6 mm wide,
 * 5 mm tall teeth, 2.5 mm tooth / 4.5 mm gap at the OD, on an engine-specific adapter hub, balanced
 * for the missing tooth; read radially by a Hall gear-tooth sensor (Honeywell SNDH-T) at ~1 mm air
 * gap. The decoder syncs on one edge, so the uneven tooth/gap duty does not matter.
 *
 * Load: alpha-N (TPS = rusEFI's own servo command), with barometric and IAT correction because the
 * helicopter operates at altitude. A live barometric sensor is needed for alphaNUseBaro to help.
 *
 * CAN: the standard rusEFI verbose broadcast (rusEFI_CAN_verbose.dbc, 11-bit IDs 0x200-0x20B,
 * every 50 ms) is the engine status for the hybrid controller, which forwards it to the autopilot
 * as DroneCAN ICE status. With the CAN request source the fork adds 0x20C (can_verbose.cpp
 * HybridStatus): the head temperatures in 0.1 C with a failed mark, and the servo throttle's and
 * the cuts' state. Its byte layout is pinned by test_dle_twin.cpp. In the other direction the
 * hybrid controller sends HCU_ENGINE_CMD (0x1A0, servo_throttle_can.h): throttle feed-forward,
 * governor target and on/off, run/stop. CAN user control is limited to the engine stop.
 *
 * Everything below is a STARTING POINT, not a tune:
 * - globalTriggerAngleOffset assumes the wheel is fitted so that the first tooth after the gap
 *   passes the sensor 90 degrees BTDC. MEASURE IT with a timing light (cranking timing is fixed)
 *   before the first start - an error here goes straight into ignition timing.
 * - VE and timing tables are generic shapes; tune VE with a wideband sensor.
 * - Injector flow: the DLE-60's is clearwater stage 1.0's part (to be measured), the DLE-120's a
 *   suggested size; set the real one.
 *
 * Engine data (DLE manuals / dealer listings):
 *   DLE-60:  61 cc, bore 36 mm, stroke 30 mm, 7.0 hp @ 8500 rpm, 1400-8500 rpm, CR 7.6:1, oil 30:1
 *   DLE-120: 120 cc, bore 47 mm, stroke 35 mm, 12 hp @ 7500 rpm, 1300-8000 rpm, CR 10.5:1
 */

#include "pch.h"

#include "dle_twin.h"

#if defined(HW_HELLEN_UAEFI) || EFI_UNIT_TEST
// uaEFI's MM100_* pins (setDleTwinUaefiPins)
#include "hellen_meta.h"
#include "defaults.h"
#endif

#if EFI_SERVO_THROTTLE
#include "servo_throttle_can.h"

#define ENGINE_MAKE_DLE "DLE"

// First tooth after the gap to TDC, assumed until measured (see header comment)
#define DLE_TRIGGER_ANGLE_BTDC 90

/**
 * Quadratic TPS spacing: alpha-N needs resolution at small throttle openings, where airflow
 * changes fastest.
 */
template<typename TValue, int TSize>
static void setTpsLoadBins(TValue (&bins)[TSize]) {
	int previous = -1;
	for (int i = 0; i < TSize; i++) {
		float fraction = (float)i / (TSize - 1);
		int tps = static_cast<int>(std::round(100 * fraction * fraction));
		// Keep the axis strictly ascending where the quadratic rounds to the same integer
		tps = std::max(tps, previous + 1);
		bins[i] = tps;
		previous = tps;
	}
}

/**
 * Generic alpha-N VE shape: cylinder filling rises steeply with the first few percent of throttle
 * at low RPM and needs more throttle to saturate at high RPM. Ceiling ~90%: at rated power these
 * engines move about their swept volume per revolution (7 hp at ~540 g/kWh and AFR ~12.5 on the
 * DLE-60 is ~95% of swept air mass), so a lower ceiling would leave WOT dangerously lean.
 */
static float dleStartingVe(float tps, float rpm, float maxRpm) {
	float rpmFraction = clampF(0, (rpm - 800) / (maxRpm - 800), 1);
	float saturationTps = 10 + 30 * rpmFraction;
	return 10 + 80 * (1 - expf(-tps / saturationTps));
}

/**
 * Air-cooled two-stroke on premix: stoichiometric at part throttle, rich towards WOT for piston
 * cooling.
 */
static float dleStartingLambda(float tps) {
	return interpolateClamped(40, 1.0f, 80, 0.85f, tps);
}

/**
 * Stock CDI style RPM-only advance: retarded for idle and starting, full advance from mid range.
 */
static float dleStartingTiming(float rpm) {
	if (rpm <= 1500) {
		return 10;
	}
	if (rpm <= 3000) {
		return interpolateClamped(1500, 10, 3000, 22, rpm);
	}
	return interpolateClamped(3000, 22, 4000, 26, rpm);
}

static void setDleTwinCommon(float maxRpm) {
	strcpy(engineConfiguration->engineMake, ENGINE_MAKE_DLE);

	// Boxer twin firing both cylinders together = one two-stroke cylinder (see header comment)
	engineConfiguration->cylindersCount = 1;
	engineConfiguration->firingOrder = FO_1;
	engineConfiguration->twoStroke = true;

	engineConfiguration->trigger.type = trigger_type_e::TT_TOOTHED_WHEEL_36_1;
	engineConfiguration->globalTriggerAngleOffset = DLE_TRIGGER_ANGLE_BTDC;

	// One spark a revolution: a two-tower coil firing both plugs from one output (the DLE-60 adds a
	// second coil, see setDle60Twin)
	engineConfiguration->ignitionMode = IM_ONE_COIL;
	engineConfiguration->crankingTimingAngle = 5;

	// One injector in the throttle body, once per revolution
	engineConfiguration->injectionMode = IM_SEQUENTIAL;
	engineConfiguration->crankingInjectionMode = IM_SEQUENTIAL;

	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	engineConfiguration->alphaNUseIat = true;
	engineConfiguration->alphaNUseBaro = true;

	// One pulse per revolution for the autopilot's RPM input and the hybrid controller
	engineConfiguration->tachPulsePerRev = 1;

	// Engine status for the hybrid controller (see header comment)
	engineConfiguration->canWriteEnabled = true;
	engineConfiguration->enableVerboseCanTx = true;
	engineConfiguration->verboseCanBaseAddress = CAN_DEFAULT_BASE;
	engineConfiguration->rusefiVerbose29b = false;
	engineConfiguration->canSleepPeriodMs = 50;

#ifdef HW_HELLEN_UAEFI
	setDleTwinUaefiPins();
#endif // HW_HELLEN_UAEFI

	// Tables
	setTpsLoadBins(config->veLoadBins);
	setLinearCurve(config->veRpmBins, 800, maxRpm + 500, 1);
	for (size_t loadIndex = 0; loadIndex < efi::size(config->veLoadBins); loadIndex++) {
		for (size_t rpmIndex = 0; rpmIndex < efi::size(config->veRpmBins); rpmIndex++) {
			config->veTable[loadIndex][rpmIndex] = dleStartingVe(
				config->veLoadBins[loadIndex], config->veRpmBins[rpmIndex], maxRpm);
		}
	}

	setTpsLoadBins(config->lambdaLoadBins);
	setLinearCurve(config->lambdaRpmBins, 800, maxRpm + 500, 1);
	for (size_t loadIndex = 0; loadIndex < efi::size(config->lambdaLoadBins); loadIndex++) {
		for (size_t rpmIndex = 0; rpmIndex < efi::size(config->lambdaRpmBins); rpmIndex++) {
			config->lambdaTable[loadIndex][rpmIndex] = dleStartingLambda(config->lambdaLoadBins[loadIndex]);
		}
	}

	setTpsLoadBins(config->ignitionLoadBins);
	setLinearCurve(config->ignitionRpmBins, 800, maxRpm + 500, 1);
	for (size_t loadIndex = 0; loadIndex < efi::size(config->ignitionLoadBins); loadIndex++) {
		for (size_t rpmIndex = 0; rpmIndex < efi::size(config->ignitionRpmBins); rpmIndex++) {
			config->ignitionTable[loadIndex][rpmIndex] = dleStartingTiming(config->ignitionRpmBins[rpmIndex]);
		}
	}

	// Throttle servo: KST SV12-12 reference servo (333 Hz, ~180 ms for a 100 degree stroke at 12 V).
	// Inert until servoThrottlePin is assigned. Overspeed closes the throttle just below the rev limit.
	engineConfiguration->servoThrottleFrequency = 333;
	engineConfiguration->servoThrottleFullTravelMs = 180;

	// Throttle request, governor target and run/stop come from the hybrid controller's
	// HCU_ENGINE_CMD frame (servo_throttle_can.h); rusEFI's governor holds speed and keeps the last
	// command when the link is lost. Default ID 0x1A0.
	engineConfiguration->servoThrottleRequestSource = ServoThrottleRequestSource::Can;
	engineConfiguration->servoThrottleCanId = HCU_ENGINE_CMD_DEFAULT_ID;
	engineConfiguration->canReadEnabled = true;
	// The HCU may stop the engine over CAN user control, nothing else (no reboot, DFU, preset...)
	engineConfiguration->canUserControlStopOnly = true;
	// The HCU sends a 12% idle feed-forward (ArduPilot H_RSC_IDLE) while disarmed or with the motor
	// interlock off. The governor drops out only below this minimum request: at the default 10% it
	// would stay engaged at idle and pull the engine back up to the target in a practice
	// autorotation. 20% matches the HCU's arming level.
	engineConfiguration->servoGovernorMinRequest = 20;

	// rusEFI governs rotor speed (decision 2026-10-07); the HCU's GOV_ON bit can still switch it off.
	// Gains from the SITL sweep (45 kg full-collective lift-off at 2000 m ISA+15, 180 ms servo):
	// rotor minimum 99.3 %, overshoot 101.5 %, no limit cycle at hover. The model has no tach noise or
	// rpm quantization - check on the engine before raising them.
	engineConfiguration->servoGovernorEnabled = true;
	engineConfiguration->servoGovernorPid.pFactor = 0.04f;
	engineConfiguration->servoGovernorPid.iFactor = 0.1f;
	engineConfiguration->servoGovernorPid.dFactor = 0;
	engineConfiguration->servoGovernorPid.minValue = -20;
	engineConfiguration->servoGovernorPid.maxValue = 20;
	// Engages within 1000 rpm (~13 %) under the target. The window only sets where the governor
	// first engages (once governing it stays engaged), and the open-loop run-up settles wherever the
	// autopilot's throttle curve puts it: the default 300 never engaged with the generator loaded
	// (~560 rpm under), and 600 missed by 8 rpm on stage 1.0's derated engine in an 8 m/s crosswind
	// (6881 against 7489; clearwater simulation 2026-10-23, user decision 2026-10-24)
	engineConfiguration->servoGovernorEngageWindow = 1000;
	// Engaging ~1000 rpm under the target, the P term alone saturates the +20 % trim and carries the
	// engine through the target (stage 1.0: rotor +3.6 to +5.3 %). The reference instead ramps from the
	// engagement RPM at 500 rpm/s (~2 s to the target): +1.1 to +1.5 %, the run-up gate at most 1.4 s
	// later, a power recovery not slowed (clearwater simulation 2026-10-26, user decision)
	engineConfiguration->servoGovernorEngageRampRpmPerS = 500;
	// A request lost in the run-up drops to the HCU's 12% idle feed-forward instead of holding a part
	// throttle: the stage 1.1 helicopter's stiff in-plane rotor head must not dwell at its 3/rev and
	// 2/rev lag crossings, ~480 and ~730 rotor rpm (~3240 / ~4930 engine rpm at 6.75:1; clearwater
	// HZ-39). From the engage window on (stage 1.1: 5750 rpm, rotor ~852; stage 1.0: 6496 rpm, rotor
	// ~850 - where the hybrid controller also ends its run-up) a lost request is held, as in flight:
	// the autopilot completes its run-up at 970 rotor rpm, and no lift-off may come before that.
	engineConfiguration->servoThrottleLostRequestIdle = 12;

	// Injector duty cut at the fields' maxima, so it cannot trip in practice: it releases only below
	// 20 % duty, which this engine (one injection per revolution) never reaches while running, so a
	// single trip would cut fuel for the rest of the flight - worse for a helicopter than a brief
	// lean run (user decision, 2026-10-13; HITL bench showed the latched cut)
	engineConfiguration->maxInjectorDutyInstant = 200;
	engineConfiguration->maxInjectorDutySustained = 120;
}

/**
 * Stage 1.0 head temperatures (user decision 2026-10-21): a 47 kOhm NTC, B 3950, on each head - CHT 1
 * on CLT, CHT 2 on AuxTemp1 (its 4.7 kOhm pull-up is in the harness; CLT has the board's). Provisional
 * points from the B-equation at 0 / 100 / 200 C (the range TunerStudio takes); the final three come
 * from the bought part's R-T table. The pull-up value stays the board's and the harness's; on uaEFI
 * AuxTemp1 is D1 (setDle60TwinUaefiPins). On a 4.7 kOhm pull-up the part reads as open below about -7 C (above 0.98 x 5 V),
 * and rusEFI rejects a reading above CLT_UPPER_VALIDATION_LIMIT (250 C).
 */
static void setDle47kHeadThermistor(ThermistorConf& sensor) {
	sensor.config.tempC_1 = 0;
	sensor.config.resistance_1 = 158017;
	sensor.config.tempC_2 = 100;
	sensor.config.resistance_2 = 3278;
	sensor.config.tempC_3 = 200;
	sensor.config.resistance_3 = 350;
}

/**
 * DLE-60 Twin: 61 cc, 7.0 hp @ 8500 rpm, 1400-8500 rpm.
 * Fuel at rated power ~5.2 kW x 540 g/kWh ~ 47 g/min, ~65 cc/min.
 */
void setDle60Twin() {
	setDleTwinCommon(/*maxRpm*/ 8500);
	strcpy(engineConfiguration->engineCode, "DLE-60 Twin");

	engineConfiguration->displacement = 0.061f;
	engineConfiguration->cylinderBore = 36;

	// Stage 1.0's fuel system (clearwater, user decision 2026-10-27): the Honda Grom 125 pump unit and
	// its Keihin injector, ~80 cc/min (unverified for this part; the bench measures its flow and dead
	// time at 12 and 14 V). Short of full power: design's estimate from the rated fuel flow (~65 cc/min)
	// is ~81 % of each revolution at the governed 7500 rpm and full throttle before the dead time, ~91 -
	// 94 % with 0.8 - 1.0 ms of it, ~7 % margin. The starting VE and lambda below ask for less (~80 % at
	// 7500 and ~89 % at 8500 with 1.3 ms, test_dle_twin.cpp); the tune decides. A 110 - 120 cc/min
	// injector of the same fit (Keihin CJR-1780, Koso DB016120) for full power (clearwater HZ-28)
	engineConfiguration->injector.flow = 80;

	// Stage 1.0 ignition (clearwater, user decision 2026-10-27): a passive single-fire coil per plug
	// on uaEFI's optional IGBTs (Q7 / Q8 fitted, R20 / R21 removed - check on the delivered board's
	// revision). Coil 1 on the main output, coil 2 on rusEFI's trailing output at 0 degrees: it
	// charges from coil 1's dwell start and fires from coil 1's spark, so both plugs fire together as
	// from the stock ignition, and every spark cut and the over-dwell protection cover both
	// (test_dle_twin.cpp)
	engineConfiguration->enableTrailingSparks = true;
	setTable(config->trailingSparkTable, 0);
#ifdef HW_HELLEN_UAEFI
	setDle60TwinUaefiPins();
#endif // HW_HELLEN_UAEFI
	// Provisional dwell, rusEFI's defaults: on the coil's listing (3 ohm, 16 mH, ~4 A at 12 V) 4 ms
	// running charges ~2.1 A / ~36 mJ and 6 ms cranking ~2.7 A / ~58 mJ. The bench measures the coil
	// current and the spark
	setConstantDwell(4);
	engineConfiguration->ignitionDwellForCrankingMs = 6;

	// rusEFI leaves cranking for running the first time RPM reaches this and stays running while the
	// engine turns. Stage 1.0's starter (T-Motor AT4120 KV250 on 12S through 12T/97T, 8.083:1) turns the
	// engine at ~1050 rpm loaded and at most ~1560 free, so 1700 keeps the cranking fuel and timing
	// until the engine has fired and run past the starter; idle below it then stays running. The hybrid
	// controller counts the engine as started at 1800 (clearwater design 2026-10-27, user decision)
	engineConfiguration->cranking.rpm = 1700;
	// Cranking fuel base (clearwater HITL bench 2026-11-18, user decision): rusEFI's default, 27 mg a
	// cycle, is sized for a far larger cylinder - ~42 mg a revolution at 20 C (a failed head sensor cranks
	// as 20 C), over 8x stoichiometric for 61 cc, and on the 80 cc/min injector a pulse longer than a
	// revolution at cranking speed, so pulses ran together. 5 mg, scaled by displacement: x1.55 at 20 C
	// is ~7.8 mg (~1.6x stoichiometric), ~9 ms at 1356 rpm. A starting point: tune it on the engine
	setTable(config->crankingCycleBaseFuel, 5);
	engineConfiguration->rpmHardLimit = 9000;
	engineConfiguration->servoThrottleOverspeedRpm = 8700;
	// Fallback governor target when the HCU sends TARGET_RPM 0: 90 % of rated, as for the DLE-120
	engineConfiguration->servoGovernorTargetRpm = 7650;

	setDle47kHeadThermistor(engineConfiguration->clt);
	setDle47kHeadThermistor(engineConfiguration->auxTempSensor1);
}

/**
 * DLE-120 Twin: 120 cc, 12 hp @ 7500 rpm, 1300-8000 rpm.
 * Fuel at rated power ~8.9 kW x 540 g/kWh ~ 80 g/min. With the rich WOT target and injector dead
 * time a 150 cc/min injector reaches ~87% duty, so ~180 cc/min is suggested.
 */
void setDle120Twin() {
	setDleTwinCommon(/*maxRpm*/ 8000);
	strcpy(engineConfiguration->engineCode, "DLE-120 Twin");

	engineConfiguration->displacement = 0.120f;
	engineConfiguration->cylinderBore = 47;

	engineConfiguration->injector.flow = 180;

	engineConfiguration->cranking.rpm = 900;
	// Cranking fuel base scaled by displacement as for the DLE-60 (rusEFI's default 27 mg is over 4x
	// stoichiometric for 120 cc): x1.55 at 20 C is ~15.5 mg, ~1.6x stoichiometric. Tune on the engine
	setTable(config->crankingCycleBaseFuel, 10);
	engineConfiguration->rpmHardLimit = 8500;
	engineConfiguration->servoThrottleOverspeedRpm = 8200;
	// Fallback governor target when the HCU sends TARGET_RPM 0: 90 % of rated (7500 rpm), the engine
	// speed at the governed 1000 rpm rotor with the working 6.75:1 drive (clearwater hcu/README.md)
	engineConfiguration->servoGovernorTargetRpm = 6750;
}

#if defined(HW_HELLEN_UAEFI) || EFI_UNIT_TEST
/**
 * uaEFI's connector pins for both presets (clearwater's harness). The rest stays the board's
 * (board_configuration.cpp): injector 1 B6, fuel pump B16, main relay B9, fan B8.
 */
void setDleTwinUaefiPins() {
	// The crank's Hall sensor on C5 (HALL1): the board's trigger input is C17, VR2 through U771
	// (MAX9924), which clearwater's boards leave off. A two-stroke has no cam: the board's cam inputs on
	// C5 and C6 go
	engineConfiguration->triggerInputPins[0] = Gpio::MM100_IN_D1;
	engineConfiguration->camInputs[0] = Gpio::Unassigned;
	engineConfiguration->camInputs[1] = Gpio::Unassigned;

	// The throttle servo on B10 (OUT_IGN6, PB8 TIM4_CH3, through U103's 5 V buffer): the only free
	// push-pull output with a hardware timer. The board gives its VR2 threshold on the same timer channel
	// (PD14) up for it (uaefi_boardConfigOverrides). Without a servo TPS1 is the unwired analog D13, and
	// alpha-N takes the failed TPS as full load
	engineConfiguration->servoThrottlePin = Gpio::MM100_IGN6;

	// The hybrid controller's hardwired stop, an active-high press (clearwater HZ-27), on C9 (BUTTON3,
	// 10k pull-down on the board)
	engineConfiguration->startStopButtonPin = Gpio::MM100_IN_D4;
	engineConfiguration->startStopButtonMode = PI_DEFAULT;

	// No electronic throttle (the TLE9201s are not fitted), MAP or pedal: the servo command is TPS1 and
	// the driver's intent. The board's defaults would drive the H-bridges against the servo, and its
	// unwired MAP and pedal inputs would read as sensor faults
	engineConfiguration->etbFunctions[0] = DC_None;
	engineConfiguration->etbFunctions[1] = DC_None;
	engineConfiguration->map.sensor.hwChannel = EFI_ADC_NONE;
	setPPSInputs(EFI_ADC_NONE, EFI_ADC_NONE);

	// Clearwater HZ-44: rusEFI cuts fuel and spark (ClearReason::IgnitionOff) once its ignition input
	// has read 6 V or less for 0.2 s. Without a key input that is the board's own supply, so a supply
	// dip stops the engine in flight, and so does a failed battery-voltage reading (0 V). A key input
	// decides alone: uaEFI's FLEX input (D5) has a 10k pull-up on the board and stays unwired, so it
	// always reads "on" and only the hardware's own supply limits remain. The engine stop does not use
	// the ignition state (CAN RUN = 0; the start/stop input, which rusEFI ignores without ignition,
	// then always works). The HITL Nucleo holds its own key input (its board_configuration.cpp).
	engineConfiguration->ignitionKeyDigitalPin = Gpio::MM100_IN_VSS;
	engineConfiguration->ignitionKeyDigitalPinMode = PI_PULLUP;
}

/**
 * The DLE-60's own uaEFI pins (clearwater stage 1.0).
 */
void setDle60TwinUaefiPins() {
	// B15 Coil 1, B14 Coil 2
	engineConfiguration->ignitionPins[0] = Gpio::MM100_IGN1;
	engineConfiguration->trailingCoilPins[0] = Gpio::MM100_IGN2;

	// CHT 2 on D1 (AUX1, a 500k pull-down on the board; the harness adds the 4.7k pull-up to 5 V)
	engineConfiguration->auxTempSensor1.adcChannel = MM100_IN_O2S_ANALOG;
}
#endif // HW_HELLEN_UAEFI || EFI_UNIT_TEST

#endif // EFI_SERVO_THROTTLE

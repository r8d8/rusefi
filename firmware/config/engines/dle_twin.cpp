/**
 * @file dle_twin.cpp
 *
 * DLE two-stroke boxer twins (DLE-60, DLE-120) converted to fuel injection, as used on a hybrid
 * helicopter: throttle body with one injector, rusEFI-driven ignition, throttle servo driven by
 * rusEFI from the hybrid controller's CAN command (see servo_throttle.h).
 *
 * Engine model: both pistons of the boxer reach TDC together and the stock DLE "TWIN" ignition
 * fires both plugs at once from one hub magnet. For fueling and spark scheduling that is one
 * two-stroke cylinder of the full displacement: cylindersCount = 1, and one ignition output drives
 * a two-tower (wasted-spark style) coil that fires both plugs in series. Confirm simultaneous
 * firing on the actual engine with a timing light on both plugs.
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
 * as DroneCAN ICE status. Its byte layout is pinned by test_dle_twin.cpp. In the other direction the
 * hybrid controller sends HCU_ENGINE_CMD (0x1A0, servo_throttle_can.h): throttle feed-forward,
 * governor target and on/off, run/stop. CAN user control is limited to the engine stop.
 *
 * Everything below is a STARTING POINT, not a tune:
 * - globalTriggerAngleOffset assumes the wheel is fitted so that the first tooth after the gap
 *   passes the sensor 90 degrees BTDC. MEASURE IT with a timing light (cranking timing is fixed)
 *   before the first start - an error here goes straight into ignition timing.
 * - VE and timing tables are generic shapes; tune VE with a wideband sensor.
 * - Injector flow is for the injector size suggested for each engine; set the real one.
 *
 * Engine data (DLE manuals / dealer listings):
 *   DLE-60:  61 cc, bore 36 mm, stroke 30 mm, 7.0 hp @ 8500 rpm, 1400-8500 rpm, CR 7.6:1, oil 30:1
 *   DLE-120: 120 cc, bore 47 mm, stroke 35 mm, 12 hp @ 7500 rpm, 1300-8000 rpm, CR 10.5:1
 */

#include "pch.h"

#include "dle_twin.h"

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
		int tps = std::round(100 * fraction * fraction);
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
	return interpolateClamped(40, 1.0, 80, 0.85, tps);
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

	// One output, two-tower coil firing both plugs
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
	engineConfiguration->servoGovernorPid.pFactor = 0.04;
	engineConfiguration->servoGovernorPid.iFactor = 0.1;
	engineConfiguration->servoGovernorPid.dFactor = 0;
	engineConfiguration->servoGovernorPid.minValue = -20;
	engineConfiguration->servoGovernorPid.maxValue = 20;
	// Engages within 600 rpm (~9 %) under the target: in the SITL run-up the low-collective throttle
	// curve left the engine ~560 rpm under the target with the generator loaded, so the default
	// 300 rpm window never engaged
	engineConfiguration->servoGovernorEngageWindow = 600;
}

/**
 * DLE-60 Twin: 61 cc, 7.0 hp @ 8500 rpm, 1400-8500 rpm.
 * Fuel at rated power ~5.2 kW x 540 g/kWh ~ 47 g/min: a ~100 cc/min injector stays below ~85% duty.
 */
void setDle60Twin() {
	setDleTwinCommon(/*maxRpm*/ 8500);
	strcpy(engineConfiguration->engineCode, "DLE-60 Twin");

	engineConfiguration->displacement = 0.061;
	engineConfiguration->cylinderBore = 36;

	engineConfiguration->injector.flow = 100;

	engineConfiguration->cranking.rpm = 1000;
	engineConfiguration->rpmHardLimit = 9000;
	engineConfiguration->servoThrottleOverspeedRpm = 8700;
	// Fallback governor target when the HCU sends TARGET_RPM 0: 90 % of rated, as for the DLE-120
	engineConfiguration->servoGovernorTargetRpm = 7650;
}

/**
 * DLE-120 Twin: 120 cc, 12 hp @ 7500 rpm, 1300-8000 rpm.
 * Fuel at rated power ~8.9 kW x 540 g/kWh ~ 80 g/min. With the rich WOT target and injector dead
 * time a 150 cc/min injector reaches ~87% duty, so ~180 cc/min is suggested.
 */
void setDle120Twin() {
	setDleTwinCommon(/*maxRpm*/ 8000);
	strcpy(engineConfiguration->engineCode, "DLE-120 Twin");

	engineConfiguration->displacement = 0.120;
	engineConfiguration->cylinderBore = 47;

	engineConfiguration->injector.flow = 180;

	engineConfiguration->cranking.rpm = 900;
	engineConfiguration->rpmHardLimit = 8500;
	engineConfiguration->servoThrottleOverspeedRpm = 8200;
	// Fallback governor target when the HCU sends TARGET_RPM 0: 90 % of rated (7500 rpm), the engine
	// speed at the governed 1000 rpm rotor with the working 6.75:1 drive (clearwater hcu/README.md)
	engineConfiguration->servoGovernorTargetRpm = 6750;
}

#endif // EFI_SERVO_THROTTLE

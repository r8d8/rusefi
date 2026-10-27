/**
 * @file	can_verbose.cpp
 *
 * TODO: change 'verbose' into 'broadcast'?
 *
 * If you edit this file, please update rusEFI_CAN_verbose.dbc!
 * Kvaser Database Editor works well for this task, and is free.
 *
 * @author Matthew Kennedy, (c) 2020
 */

#include "pch.h"
#if EFI_CAN_SUPPORT || EFI_UNIT_TEST

#include "efi_scaled_channel.h"
#include "can_msg_tx.h"
#include "can.h"
#include "fuel_math.h"
#include "spark_logic.h"

#ifdef HW_HELLEN
#include "hellen_meta.h"
#endif // HW_HELLEN

#if EFI_SERVO_THROTTLE
#include "servo_throttle.h"
#include "servo_throttle_can.h"
#include "ignition_controller.h"
#endif // EFI_SERVO_THROTTLE

#define CAN_PEDAL_TPS_OFFSET 2
#define CAN_SENSOR_1_OFFSET 3

// A temperature as a uint8 with PACK_ADD_TEMPERATURE, -40 .. 215 C, saturated: a float outside
// the uint8 range converts with undefined behaviour (x86 and ARM wrap, so a 230 C head read -26 C)
static uint8_t packTemperature(float temperatureC) {
	return static_cast<uint8_t>(clampF(0, temperatureC + PACK_ADD_TEMPERATURE, UINT8_MAX));
}

struct Status {
	uint16_t warningCounter;
	uint16_t lastErrorCode;

	uint8_t revLimit : 1;
	uint8_t mainRelay : 1;
	uint8_t fuelPump : 1;
	uint8_t checkEngine : 1;
	uint8_t o2Heater : 1;
	uint8_t lambdaProtectActive : 1;

	uint8_t fan : 1;
	uint8_t fan2 : 1;

	uint8_t gear;

	uint16_t distanceTraveled;
};

void populateFrame(Status& msg) {
	msg.warningCounter = engine->engineState.warnings.warningCounter;
	msg.lastErrorCode = static_cast<uint16_t>(engine->engineState.warnings.lastErrorCode);

#if EFI_ENGINE_CONTROL
	msg.revLimit = !engine->module<LimpManager>()->allowInjection() || !engine->module<LimpManager>()->allowIgnition();
#endif // EFI_ENGINE_CONTROL

	msg.mainRelay = enginePins.mainRelay.getLogicValue();
	msg.fuelPump = enginePins.fuelPumpRelay.getLogicValue();
	msg.checkEngine = enginePins.checkEnginePin.getLogicValue();
	msg.o2Heater = enginePins.o2heater.getLogicValue();
#if EFI_SHAFT_POSITION_INPUT && EFI_ENGINE_CONTROL
	msg.lambdaProtectActive = engine->lambdaMonitor.isCut();
#endif // EFI_SHAFT_POSITION_INPUT && EFI_ENGINE_CONTROL
	msg.fan = enginePins.fanRelay.getLogicValue();
	msg.fan2 = enginePins.fanRelay2.getLogicValue();

	msg.gear = Sensor::getOrZero(SensorType::DetectedGear);

#ifdef MODULE_ODOMETER
	// scale to units of 0.1km
	msg.distanceTraveled = engine->module<TripOdometer>()->getDistanceMeters() / 100;
#endif // MODULE_ODOMETER
}

struct Status11 {
  uint8_t brakePedal : 1;
};

void populateFrame(Status11& msg) {
  msg.brakePedal = engine->engineState.brakePedalState;
}

struct Speeds {
	uint16_t rpm;
	scaled_angle timing;
	scaled_channel<uint8_t, 2> injDuty;
	scaled_channel<uint8_t, 2> coilDuty;
	scaled_channel<uint8_t> vssKph;
	uint8_t EthanolPercent;
};

void populateFrame(Speeds& msg) {
	auto rpm = Sensor::getOrZero(SensorType::Rpm);
	msg.rpm = rpm;

	auto timing = engine->engineState.timingAdvance[0];
	msg.timing = timing > 360 ? timing - 720 : timing;
#if EFI_ENGINE_CONTROL
	msg.injDuty = getInjectorDutyCycle(rpm);
	msg.coilDuty = getCoilDutyCycle(rpm);
#endif // EFI_ENGINE_CONTROL
	msg.vssKph = Sensor::getOrZero(SensorType::VehicleSpeed);

	msg.EthanolPercent = Sensor::getOrZero(SensorType::FuelEthanolPercent);
}

struct PedalAndTps {
	scaled_percent pedal;
	scaled_percent tps1;
	scaled_percent tps2;
	scaled_percent wastegate;
};

void populateFrame(PedalAndTps& msg)
{
	msg.pedal = Sensor::get(SensorType::AcceleratorPedal).value_or(-1);
	msg.tps1 = Sensor::get(SensorType::Tps1).value_or(-1);
	msg.tps2 = Sensor::get(SensorType::Tps2).value_or(-1);
	msg.wastegate = Sensor::get(SensorType::WastegatePosition).value_or(-1);
}

struct Sensors1 {
	scaled_pressure map;
	scaled_channel<uint8_t> clt;
	scaled_channel<uint8_t> iat;
	scaled_channel<uint8_t> aux1;
	scaled_channel<uint8_t> aux2;
	scaled_channel<uint8_t> mcuTemp;
	scaled_channel<uint8_t, 2> fuelLevel;
};

void populateFrame(Sensors1& msg) {
	msg.map = Sensor::getOrZero(SensorType::Map);

	msg.clt = packTemperature(Sensor::getOrZero(SensorType::Clt));
	msg.iat = packTemperature(Sensor::getOrZero(SensorType::Iat));

	msg.aux1 = packTemperature(Sensor::getOrZero(SensorType::AuxTemp1));
	msg.aux2 = packTemperature(Sensor::getOrZero(SensorType::AuxTemp2));

#if	HAL_USE_ADC
	msg.mcuTemp = packTemperature(getMCUInternalTemperature());
#endif

	msg.fuelLevel = Sensor::getOrZero(SensorType::FuelLevel);
}

struct Sensors2 {
	uint8_t pad[2];

	scaled_pressure oilPressure;
	uint8_t oilTemp;
	uint8_t fuelTemp;
	scaled_voltage vbatt;
};

void populateFrame(Sensors2& msg) {
	msg.oilPressure = Sensor::get(SensorType::OilPressure).value_or(-1);
	msg.oilTemp = packTemperature(Sensor::getOrZero(SensorType::OilTemperature));
	msg.fuelTemp = packTemperature(Sensor::getOrZero(SensorType::FuelTemperature));
	msg.vbatt = Sensor::getOrZero(SensorType::BatteryVoltage);
}

struct Fueling {
	scaled_channel<uint16_t, 1000> cylAirmass;
	scaled_channel<uint16_t, 100> estAirflow;
	scaled_ms fuel_pulse;
	uint16_t knockCount;
};

void populateFrame(Fueling& msg) {
#if EFI_ENGINE_CONTROL
	msg.cylAirmass = engine->fuelComputer.sdAirMassInOneCylinder;
	msg.estAirflow = engine->engineState.airflowEstimate;
	msg.fuel_pulse = (float)engine->outputChannels.actualLastInjection;
	msg.knockCount = engine->module<KnockController>()->getKnockCount();
#endif // EFI_ENGINE_CONTROL
}

struct Fueling2 {
	scaled_channel<uint16_t> fuelConsumedGram;
	scaled_channel<uint16_t, PACK_MULT_FUEL_FLOW> fuelFlowRate;
	scaled_percent fuelTrim[2];
};

void populateFrame(Fueling2& msg) {
#ifdef MODULE_ODOMETER
	msg.fuelConsumedGram = engine->module<TripOdometer>()->getConsumedGrams();
	msg.fuelFlowRate = engine->module<TripOdometer>()->getConsumptionGramPerSecond();
#endif // MODULE_ODOMETER

	for (size_t i = 0; i < FT_BANK_COUNT; i++) {
		msg.fuelTrim[i] = 100.0f * (engine->engineState.stftCorrection[i] - 1.0f);
	}
}

struct Fueling3 {
	scaled_channel<uint16_t, 10000> Lambda;
	scaled_channel<uint16_t, 10000> Lambda2;
	scaled_channel<uint16_t, 30> FuelPressureLow;
	scaled_channel<uint16_t, 10> FuelPressureHigh;
};

void populateFrame(Fueling3& msg) {
	msg.Lambda = Sensor::getOrZero(SensorType::Lambda1);
	msg.Lambda2 = Sensor::getOrZero(SensorType::Lambda2);
	msg.FuelPressureLow = Sensor::getOrZero(SensorType::FuelPressureLow);
	msg.FuelPressureHigh = KPA2BAR(Sensor::getOrZero(SensorType::FuelPressureHigh));
}

struct PerCylinderKnock {
  int8_t knock[8];
};

void populateFrame(PerCylinderKnock& msg) {
  for (int index = 0; index<std::min(8, MAX_CYLINDER_COUNT); index++) {
	  msg.knock[index] = engine->module<KnockController>()->m_knockCyl[index];
  }
}

struct Cams {
	int8_t Bank1IntakeActual;
	int8_t Bank1IntakeTarget;
	int8_t Bank1ExhaustActual;
	int8_t Bank1ExhaustTarget;
	int8_t Bank2IntakeActual;
	int8_t Bank2IntakeTarget;
	int8_t Bank2ExhaustActual;
	int8_t Bank2ExhaustTarget;
};

void populateFrame(Cams& msg) {
#if EFI_SHAFT_POSITION_INPUT
	msg.Bank1IntakeActual  = engine->triggerCentral.getVVTPosition(0, 0);
	msg.Bank1ExhaustActual = engine->triggerCentral.getVVTPosition(0, 1);
	msg.Bank2IntakeActual  = engine->triggerCentral.getVVTPosition(1, 0);
	msg.Bank2ExhaustActual = engine->triggerCentral.getVVTPosition(1, 1);
#endif // EFI_SHAFT_POSITION_INPUT

	// TODO: maybe don't rely on outputChannels here
	msg.Bank1IntakeTarget = engine->outputChannels.vvtTargets[0];
	msg.Bank1ExhaustTarget = engine->outputChannels.vvtTargets[1];
	msg.Bank2IntakeTarget = engine->outputChannels.vvtTargets[2];
	msg.Bank2ExhaustTarget = engine->outputChannels.vvtTargets[3];
}

struct Egts {
	uint8_t egt[8];
};

void populateFrame(Egts& msg) {
	msg.egt[0] = Sensor::getOrZero(SensorType::EGT1) / 5;
	msg.egt[1] = Sensor::getOrZero(SensorType::EGT2) / 5;
	// DBC Defines signals Egt3 through Egt8 but we do not have the code
}

#if EFI_SERVO_THROTTLE
/**
 * Fork-only (r8d8/rusefi), base + 12: sent while the servo throttle takes its request from the
 * hybrid controller's HCU_ENGINE_CMD. The head temperatures without Sensors1's limits (clearwater
 * HZ-43), and the state of the servo throttle, the command and the cuts. The hybrid controller
 * (r8d8/hybrid_ctrl rusefi_link.c) decodes it; the layout is pinned by test_dle_twin.cpp.
 */
struct HybridStatus {
	// 0.1 C; HYBRID_STATUS_TEMPERATURE_INVALID for a failed or unconfigured sensor
	int16_t clt;
	int16_t auxTemp1;

	// HcuCommandState: 0 none since boot, 1 fresh, 2 held
	uint8_t commandState : 2;
	// FF_HELD in the fresh command
	uint8_t feedForwardHeld : 1;
	uint8_t feedForwardLive : 1;
	// RPM has reached the governor engage window since the engine started
	uint8_t runUpDone : 1;
	// Off: fuel and spark are cut (ClearReason::IgnitionOff)
	uint8_t ignitionOn : 1;
	// Overspeed entries since boot, modulo 4: an overspeed shorter than the 50 ms period still counts
	uint8_t overspeedEntries : 2;

	// ServoGovernorState: 0 passthrough, 1 following, 2 governing, 3 overspeed (throttle closed)
	uint8_t governorState : 2;
	// The IAT failed or unconfigured: Sensors1 then sends it as 0 C
	uint8_t iatInvalid : 1;
	uint8_t : 5;
	// ClearReason, 0 = not cut
	uint8_t fuelCutReason;
	uint8_t sparkCutReason;
};
static_assert(sizeof(HybridStatus) == 8);

#define HYBRID_STATUS_TEMPERATURE_INVALID INT16_MIN

static int16_t packTenthsC(SensorType type) {
	auto temperature = Sensor::get(type);
	if (!temperature) {
		return HYBRID_STATUS_TEMPERATURE_INVALID;
	}
	return static_cast<int16_t>(std::roundf(clampF(-3276.7f, temperature.Value, 3276.7f) * 10));
}

void populateFrame(HybridStatus& msg) {
	msg.clt = packTenthsC(SensorType::Clt);
	msg.auxTemp1 = packTenthsC(SensorType::AuxTemp1);
	msg.iatInvalid = !Sensor::get(SensorType::Iat).Valid;

#if HAS_SERVO_THROTTLE_CAN
	HcuEngineCommand command;
	auto commandState = getHcuEngineCommandListener().get(command, getTimeNowNt());
	msg.commandState = static_cast<uint8_t>(commandState);
	msg.feedForwardHeld = commandState == HcuCommandState::Fresh && command.feedForwardHeld;
#endif // HAS_SERVO_THROTTLE_CAN
	const auto& servo = *engine->module<ServoThrottle>();
	msg.feedForwardLive = servo.isFeedForwardLive();
	msg.runUpDone = servo.isRunUpDone();
	msg.governorState = static_cast<uint8_t>(servo.getState());
	msg.overspeedEntries = servo.getOverspeedEntries() & 0x3;
	msg.ignitionOn = engine->module<IgnitionController>()->getIgnState();

#if EFI_ENGINE_CONTROL
	msg.fuelCutReason = static_cast<uint8_t>(engine->module<LimpManager>()->allowInjection().reason);
	msg.sparkCutReason = static_cast<uint8_t>(engine->module<LimpManager>()->allowIgnition().reason);
#endif // EFI_ENGINE_CONTROL
}
#endif // EFI_SERVO_THROTTLE

void sendCanVerbose() {
#if HW_HELLEN && EFI_PROD_CODE
        if (!getHellenBoardEnabled()) {
            return;
        }
#endif // HW_HELLEN
	auto base = engineConfiguration->verboseCanBaseAddress;
	auto isExt = engineConfiguration->rusefiVerbose29b;
	auto canChannel = (int)engineConfiguration->canBroadcastUseChannel;

	transmitStruct<Status>		(CanCategory::VERBOSE, base + 0, isExt, canChannel);
	transmitStruct<Speeds>		(CanCategory::VERBOSE, base + 1, isExt, canChannel);
	transmitStruct<PedalAndTps>	(CanCategory::VERBOSE, base + CAN_PEDAL_TPS_OFFSET, isExt, canChannel);
	transmitStruct<Sensors1>	(CanCategory::VERBOSE, base + CAN_SENSOR_1_OFFSET, isExt, canChannel);
	transmitStruct<Sensors2>	(CanCategory::VERBOSE, base + 4, isExt, canChannel);
	transmitStruct<Fueling>		(CanCategory::VERBOSE, base + 5, isExt, canChannel);
	transmitStruct<Fueling2>	(CanCategory::VERBOSE, base + 6, isExt, canChannel);
	transmitStruct<Fueling3>	(CanCategory::VERBOSE, base + 7, isExt, canChannel);
	transmitStruct<Cams>		(CanCategory::VERBOSE, base + 8, isExt, canChannel);

	transmitStruct<Egts>	(CanCategory::VERBOSE, base + 9, isExt, canChannel);
	transmitStruct<PerCylinderKnock>	(CanCategory::VERBOSE, base + 10, isExt, canChannel);
	transmitStruct<Status11>	(CanCategory::VERBOSE, base + 11, isExt, canChannel);

#if EFI_SERVO_THROTTLE
	if (engineConfiguration->servoThrottleRequestSource == ServoThrottleRequestSource::Can) {
		transmitStruct<HybridStatus>	(CanCategory::VERBOSE, base + 12, isExt, canChannel);
	}
#endif // EFI_SERVO_THROTTLE
}

#endif // EFI_CAN_SUPPORT || EFI_UNIT_TEST

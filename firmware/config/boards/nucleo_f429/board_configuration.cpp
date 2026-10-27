#include "pch.h"
#include "board_overrides.h"

// PB14 is error LED, configured in board.mk
// Board only has 3 LEDs, so no warning LED

Gpio getCommsLedPin() {
	return Gpio::B7;
}

Gpio getRunningLedPin() {
	return Gpio::B0;
}

Gpio getWarningLedPin() {
	// this board has no warning led
	return Gpio::Unassigned;
}

static void nucleo_f429_preHalInit() {
	/* TX_EN */
	efiSetPadMode("Ethernet", Gpio::G11, PAL_MODE_ALTERNATE(0xb));
	/* TXD0 */
	efiSetPadMode("Ethernet", Gpio::G13, PAL_MODE_ALTERNATE(0xb));
	/* TXD1 */
	efiSetPadMode("Ethernet", Gpio::B13, PAL_MODE_ALTERNATE(0xb));

	/* RXD0 */
	efiSetPadMode("Ethernet",  Gpio::C4, PAL_MODE_ALTERNATE(0xb));
	/* RXD1 */
	efiSetPadMode("Ethernet",  Gpio::C5, PAL_MODE_ALTERNATE(0xb));
	/* CSR DV */
	efiSetPadMode("Ethernet",  Gpio::A7, PAL_MODE_ALTERNATE(0xb));

	/* MDIO */
	efiSetPadMode("Ethernet",  Gpio::A2, PAL_MODE_ALTERNATE(0xb));
	/* MDC */
	efiSetPadMode("Ethernet",  Gpio::C1, PAL_MODE_ALTERNATE(0xb));

	/* REF_CLK */
	efiSetPadMode("Ethernet",  Gpio::A1, PAL_MODE_ALTERNATE(0xb));
}

#if defined(HARDWARE_CI) && defined(HW_NUCLEO_F767)
static void nucleoHardwareCiConfigOverrides() {
	// MINIMAL_PINS has no MAP input, so the fast ADC would never start.
	// PC3 supports ADC2 (fast) and ADC1 (slow), and avoids Ethernet pins.
	engineConfiguration->map.sensor.hwChannel = EFI_ADC_13;
}
#endif

#if HW_NUCLEO_F429_HITL
/**
 * Hybrid controller (HCU) hardware-in-the-loop bench, see r8d8/hybrid_ctrl docs/hitl.md: this
 * Nucleo stands in for uaEFI running the DLE-120 preset, between a plant emulator (G474: crank
 * wheel, engine and rotor model) and the HCU (H755). Pins on the Zio (Arduino) header, at the
 * same positions as the HCU's Nucleo-144 signals.
 */
static void nucleoHitlDefaultConfiguration() {
	// D7 <- plant 36-1 crank signal
	engineConfiguration->triggerInputPins[0] = Gpio::F13;
	// D5 <- throttle request pulse (plant, or the autopilot's HeliRSC output). The DLE presets take
	// the request from the HCU over CAN (HCU_ENGINE_CMD); the pin stays for the PWM request source.
	engineConfiguration->pwmInputTpsPin = Gpio::E11;
	// D6 -> plant servo input; TIM1_CH1 for a hardware PWM pulse
	engineConfiguration->servoThrottlePin = Gpio::E9;
	// D3 -> HCU tach input, 1 pulse/rev (set by the preset)
	engineConfiguration->tachOutputPin = Gpio::E13;
	// D9 -> plant spark input: the engine only makes torque while this fires
	engineConfiguration->ignitionPins[0] = Gpio::D15;
	// D2 -> plant's second spark input (G474 PA1): the DLE-60 preset's coil 2, on the trailing output
	// at 0 degrees; idle under the DLE-120 preset
	engineConfiguration->trailingCoilPins[0] = Gpio::F15;
	// D10, not wired; assigned so injection is scheduled as on the engine
	engineConfiguration->injectionPins[0] = Gpio::D14;
	// D4 <- HCU ecu-stop output (active high pulse)
	engineConfiguration->startStopButtonPin = Gpio::F14;
	engineConfiguration->startStopButtonMode = PI_PULLDOWN;
	// D8, not wired: the Nucleo has no battery voltage input, and 0 V VBatt reads as ignition off
	// (fuel and spark cut). The ignition key input takes priority over VBatt; its pull-up reads "on"
	// while open, and grounding it simulates key-off.
	engineConfiguration->ignitionKeyDigitalPin = Gpio::F12;
	engineConfiguration->ignitionKeyDigitalPinMode = PI_PULLUP;
	// With VBatt at 0 V the injector dead time comes from the lowest voltage bin, lengthening every
	// pulse (and with it the injector duty, which can trip the duty-cycle fuel cut): flatten each
	// pressure row to its 14 V value so the bench injects as the engine would on a healthy supply
	for (auto& row : engineConfiguration->injector.battLagCorrTable) {
		const auto atNominal = row[efi::size(row) - 2]; // the 14 V bin of 6..15 V
		for (auto& lag : row) {
			lag = atNominal;
		}
	}
	// No MAP sensor on the bench (the DLE presets run alpha-N): averaging only floods warnings
	engineConfiguration->isMapAveragingEnabled = false;
	// D14/D15: CAN1 to the power CAN transceiver (verbose broadcast for the HCU, HCU_ENGINE_CMD from it)
	engineConfiguration->canTxPin = Gpio::B9;
	engineConfiguration->canRxPin = Gpio::B8;
}
#endif // HW_NUCLEO_F429_HITL

void setup_custom_board_overrides() {
	custom_board_preHalInit = nucleo_f429_preHalInit;
#if HW_NUCLEO_F429_HITL
	custom_board_DefaultConfiguration = nucleoHitlDefaultConfiguration;
#endif
#if defined(HARDWARE_CI) && defined(HW_NUCLEO_F767)
	custom_board_ConfigOverrides = nucleoHardwareCiConfigOverrides;
#endif
}

extern "C" {

void OpenBLT__early_init() {
	nucleo_f429_preHalInit();
}

}

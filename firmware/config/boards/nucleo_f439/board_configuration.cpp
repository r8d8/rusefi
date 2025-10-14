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

static void nucleo_f439_preHalInit() {
	// Ethernet configuration (same as F429)
	efiSetPadMode("Ethernet",  Gpio::A1, PAL_MODE_ALTERNATE(0xb));
	efiSetPadMode("Ethernet",  Gpio::A2, PAL_MODE_ALTERNATE(0xb));
	efiSetPadMode("Ethernet",  Gpio::A7, PAL_MODE_ALTERNATE(0xb));

	efiSetPadMode("Ethernet", Gpio::B13, PAL_MODE_ALTERNATE(0xb));

	efiSetPadMode("Ethernet",  Gpio::C1, PAL_MODE_ALTERNATE(0xb));
	efiSetPadMode("Ethernet",  Gpio::C4, PAL_MODE_ALTERNATE(0xb));
	efiSetPadMode("Ethernet",  Gpio::C5, PAL_MODE_ALTERNATE(0xb));

	efiSetPadMode("Ethernet", Gpio::G11, PAL_MODE_ALTERNATE(0xb));
	efiSetPadMode("Ethernet", Gpio::G13, PAL_MODE_ALTERNATE(0xb));
}

void setup_custom_board_overrides() {
	custom_board_preHalInit = nucleo_f439_preHalInit;
}

/**
 * @brief Board-specific configuration for Nucleo F439ZI
 * Based on F429 but with crypto support and enhanced features
 */
void setBoardConfigOverrides() {
	// Configure servo throttle pins for testing
	engineConfiguration->servoOutputPin = Gpio::A8;  // TIM1_CH1 - PWM capable
	
	// Configure some basic ADC inputs for testing
	engineConfiguration->tpsAdcChannel = EFI_ADC_3;   // PA3/A0
	engineConfiguration->mapAdcChannel = EFI_ADC_10;  // PC0/A1
	engineConfiguration->cltAdcChannel = EFI_ADC_13;  // PC3/A2
	engineConfiguration->iatAdcChannel = EFI_ADC_9;   // PF3/A3
	
	// Enable servo throttle mode for testing
	engineConfiguration->throttleControlMode = ETB_SERVO;
}

void setupDefaultSensorInputs() {
	// Set reasonable defaults for Nucleo F439 testing
	engineConfiguration->throttleControlMode = ETB_SERVO;
	
	// Servo configuration
	engineConfiguration->servoMinPulseWidth = 1000; // 1ms
	engineConfiguration->servoMaxPulseWidth = 2000; // 2ms
	engineConfiguration->servoUpdateFrequency = 50; // 50Hz
	engineConfiguration->servoClosedAngle = 0;      // 0 degrees closed
	engineConfiguration->servoOpenAngle = 90;       // 90 degrees open
}
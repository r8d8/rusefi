/**
 * @file pwm_servo_throttle.h
 * @brief PWM RC Servo Throttle Control
 *
 * This module controls throttle using RC-style PWM servo motors.
 * RC servos operate at 50Hz (20ms period) with pulse width between 1000-2000 microseconds.
 * Unlike traditional ETB with TPS feedback, servo position is commanded directly based on 
 * pedal input, as RC servos have internal position feedback.
 *
 * @date October 14, 2025
 * @author rusEFI Team
 *
 * This file is part of rusEfi - see http://rusefi.com
 */

#pragma once

#include "closed_loop_controller.h"
#include "rusefi_types.h"

// RC Servo PWM parameters
#define PWM_SERVO_FREQUENCY 50.0f     // 50Hz standard for RC servos
#define PWM_SERVO_PERIOD_MS 20.0f     // 20ms period (1/50Hz)
#define PWM_SERVO_MIN_US 1000         // 1000us minimum pulse width
#define PWM_SERVO_MAX_US 2000         // 2000us maximum pulse width
#define PWM_SERVO_NEUTRAL_US 1500     // 1500us neutral/center position

/**
 * Get servo position as TPS feedback (0-100%)
 * This provides virtual TPS sensor data for ETB validation
 */
float getPwmServoTps();

/**
 * Initialize PWM servo throttle control system (called once at boot)
 */
void initPwmServoThrottle();

/**
 * Start PWM servo throttle hardware (can be called after configuration change)
 */
void startPwmServoThrottle();

/**
 * Stop PWM servo throttle hardware (called before configuration change)
 */
void stopPwmServoThrottle();

/**
 * Set throttle position via PWM servo
 * @param position Target throttle position 0-100%
 */
void setPwmServoThrottlePosition(percent_t position);

/**
 * Get current commanded servo position
 * @return Current throttle position 0-100%
 */
percent_t getPwmServoThrottlePosition();

/**
 * Convert throttle position (0-100%) to pulse width in microseconds
 * @param position Throttle position 0-100%
 * @return Pulse width in microseconds
 */
uint16_t servoPositionToPulseWidth(percent_t position);

/**
 * Disable PWM servo throttle
 */
void disablePwmServoThrottle();

/**
 * Console command to test PWM servo
 */
void testPwmServo();

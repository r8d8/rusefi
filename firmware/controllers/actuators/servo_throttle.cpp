#include "pch.h"
#include "servo_throttle.h"

#if EFI_SERVO_THROTTLE

#include "pwm_generator_logic.h"
#include "electronic_throttle.h"
#include "functional_sensor.h"
#include "identity_func.h"

static ServoThrottleController instance;
static SimplePwm servoPwm("ServoThrottle");
static OutputPin servoOutputPin;

// Virtual TPS sensor for servo position
static FunctionalSensor servoTpsSensor(SensorType::Tps1, MS2NT(50));
static IdentityFunction servoTpsConverter;

ServoThrottleController* getServoThrottleController() {
    return &instance;
}

SensorResult getServoTpsPosition() {
    ServoThrottleController* controller = getServoThrottleController();
    if (!controller || !controller->isInitialized()) {
        return unexpected;
    }
    
    return controller->getCurrentPosition();
}

void ServoThrottleController::onConfigurationChange(engine_configuration_s const * previousConfig) {
    (void)previousConfig; // Suppress unused parameter warning
    
    // Reinitialize if mode changed to servo
    if (engineConfiguration->throttleControlMode == throttle_control_mode_e::PWM_SERVO) {
        if (!isInitialized()) {
            initPwmServo();
            registerVirtualTps();
        }
    } else {
        // Reset if mode changed away from servo
        if (isInitialized()) {
            reset();
        }
    }
}

void ServoThrottleController::onSlowCallback() {
    // Nothing to do on slow callback for now
    // Could be used for diagnostics or position checking
}

void ServoThrottleController::initPwmServo() {
    // Use ETB control pin for servo PWM output
    if (isBrainPinValid(engineConfiguration->etbIo[0].controlPin)) {
        // Calculate initial neutral position
        float neutralPosition = 50.0f; // Start at 50% (typically 1.5ms)
        float neutralPulse = interpolateClamped(0.0f, engineConfiguration->servoMinPulseWidth,
                                              100.0f, engineConfiguration->servoMaxPulseWidth,
                                              neutralPosition);
        float periodUs = 1000000.0f / engineConfiguration->servoUpdateFrequency;
        float neutralDuty = neutralPulse / periodUs;
        
        startSimplePwmExt(&servoPwm,
                          "Servo Throttle",
                          &engine->scheduler,
                          engineConfiguration->etbIo[0].controlPin,
                          &servoOutputPin,
                          (int)engineConfiguration->servoUpdateFrequency,
                          neutralDuty,
                          nullptr); // No callback needed
        
        m_currentPosition = neutralPosition;
        m_isInitialized = true;
        
        efiPrintf("Servo throttle initialized on pin %s at %dHz",
                 hwPortname(engineConfiguration->etbIo[0].controlPin),
                 (int)engineConfiguration->servoUpdateFrequency);
    }
}

void ServoThrottleController::registerVirtualTps() {
    // Set up converter for virtual TPS
    servoTpsSensor.setFunction(servoTpsConverter);
    
    if (!servoTpsSensor.Register()) {
        efiPrintf("Failed to register servo virtual TPS");
    } else {
        efiPrintf("Servo virtual TPS registered");
    }
}

void ServoThrottleController::unregisterVirtualTps() {
    // Note: rusEFI doesn't have per-sensor reset, only full registry reset
    // This is a limitation that could be improved in the future
}

void ServoThrottleController::setTargetPosition(float targetPercent) {
    if (!isInitialized()) {
        return;
    }
    
    // Clamp target to valid range
    targetPercent = clampF(0.0f, targetPercent, 100.0f);
    
    // Convert percentage to servo pulse width
    float pulseWidth = interpolateClamped(0.0f, engineConfiguration->servoMinPulseWidth,
                                        100.0f, engineConfiguration->servoMaxPulseWidth,
                                        targetPercent);
    
    // Convert to duty cycle
    float periodUs = 1000000.0f / engineConfiguration->servoUpdateFrequency;
    float dutyCycle = pulseWidth / periodUs;
    
    // Update PWM
    servoPwm.setSimplePwmDutyCycle(dutyCycle);
    
    // Update current position (servo has no feedback)
    m_currentPosition = targetPercent;
    
    // Update virtual TPS sensor
    servoTpsSensor.postRawValue(targetPercent, getTimeNowNt());
}

void ServoThrottleController::reset() {
    if (m_isInitialized) {
        servoPwm.stop();
        unregisterVirtualTps();
        m_isInitialized = false;
        m_currentPosition = 0.0f;
    }
}

#else // EFI_SERVO_THROTTLE

// Stub implementations when servo throttle is disabled
static ServoThrottleController stubInstance;

ServoThrottleController* getServoThrottleController() {
    return &stubInstance;
}

#endif // EFI_SERVO_THROTTLE

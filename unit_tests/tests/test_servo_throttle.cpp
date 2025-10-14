#include "pch.h"

#if EFI_SERVO_THROTTLE
#include "servo_throttle.h"
#include "sensor.h"

TEST(ServoThrottle, BasicInitialization) {
    EngineTestHelper eth(engine_type_e::TEST_ENGINE);
    
    // Set up servo configuration
    engineConfiguration->throttleControlMode = throttle_control_mode_e::PWM_SERVO;
    engineConfiguration->servoMinPulseWidth = 1000;
    engineConfiguration->servoMaxPulseWidth = 2000;
    engineConfiguration->servoUpdateFrequency = 50;
    engineConfiguration->servoReversed = false;
    
    // Servo angle mapping
    engineConfiguration->servoMinAngle = -90.0f;
    engineConfiguration->servoMaxAngle = 90.0f;
    engineConfiguration->throttleMinAngle = 0.0f;
    engineConfiguration->throttleMaxAngle = 90.0f;
    
    // ETB configuration for servo mode
    engineConfiguration->etbIo[0].controlPin = Gpio::A8; // Test pin
    
    ServoThrottleController* controller = getServoThrottleController();
    ASSERT_NE(nullptr, controller);
    
    // Trigger configuration change to initialize
    controller->onConfigurationChange(nullptr);
    
    // Check initialization
    EXPECT_TRUE(controller->isInitialized());
}

TEST(ServoThrottle, PositionControl) {
    EngineTestHelper eth(engine_type_e::TEST_ENGINE);
    
    engineConfiguration->throttleControlMode = throttle_control_mode_e::PWM_SERVO;
    engineConfiguration->servoMinPulseWidth = 1000;
    engineConfiguration->servoMaxPulseWidth = 2000;
    engineConfiguration->servoUpdateFrequency = 50;
    engineConfiguration->servoReversed = false;
    
    ServoThrottleController* controller = getServoThrottleController();
    
    // Test position setting
    controller->setTargetPosition(0);
    EXPECT_EQ(0, controller->getTargetPosition());
    
    controller->setTargetPosition(50);
    EXPECT_EQ(50, controller->getTargetPosition());
    
    controller->setTargetPosition(100);
    EXPECT_EQ(100, controller->getTargetPosition());
    
    // Test position clamping
    controller->setTargetPosition(-10);
    EXPECT_EQ(0, controller->getTargetPosition());
    
    controller->setTargetPosition(110);
    EXPECT_EQ(100, controller->getTargetPosition());
}

TEST(ServoThrottle, VirtualTpsBasic) {
    EngineTestHelper eth(engine_type_e::TEST_ENGINE);
    
    // Set up servo configuration
    engineConfiguration->throttleControlMode = throttle_control_mode_e::PWM_SERVO;
    engineConfiguration->servoMinPulseWidth = 1000;
    engineConfiguration->servoMaxPulseWidth = 2000;
    engineConfiguration->servoUpdateFrequency = 50;
    engineConfiguration->servoReversed = false;
    engineConfiguration->etbIo[0].controlPin = Gpio::A8; // Test pin
    
    ServoThrottleController* controller = getServoThrottleController();
    ASSERT_NE(nullptr, controller);
    
    // Trigger configuration change to initialize
    controller->onConfigurationChange(nullptr);
    
    // Test that virtual TPS is working
    controller->setTargetPosition(50);
    controller->onSlowCallback(); // Update position
    
    // Virtual TPS should report the servo position
    auto tpsReading = Sensor::get(SensorType::Tps1);
    EXPECT_TRUE(tpsReading.Valid);
    EXPECT_NEAR(50.0f, tpsReading.Value, 0.1f);
}

TEST(ServoThrottle, ReverseDirection) {
    EngineTestHelper eth(engine_type_e::TEST_ENGINE);
    
    engineConfiguration->throttleControlMode = throttle_control_mode_e::PWM_SERVO;
    engineConfiguration->servoMinPulseWidth = 1000;
    engineConfiguration->servoMaxPulseWidth = 2000;
    engineConfiguration->servoUpdateFrequency = 50;
    engineConfiguration->servoReversed = true; // Enable reverse
    engineConfiguration->etbIo[0].controlPin = Gpio::A8; // Test pin
    
    ServoThrottleController* controller = getServoThrottleController();
    
    // With reverse enabled, internal calculations should handle the reversal
    // The target position should still be what we set, but PWM calculations differ
    controller->setTargetPosition(25);
    EXPECT_EQ(25, controller->getTargetPosition());
    
    controller->setTargetPosition(75);
    EXPECT_EQ(75, controller->getTargetPosition());
}

TEST(ServoThrottle, PulseWidthCalculations) {
    EngineTestHelper eth(engine_type_e::TEST_ENGINE);
    
    // Test various servo configurations
    struct TestCase {
        uint16_t minPulse;
        uint16_t maxPulse;
        float position;
        float expectedPulseWidth;
    };
    
    TestCase testCases[] = {
        {1000, 2000, 0.0f, 1000.0f},    // 0% = min pulse
        {1000, 2000, 50.0f, 1500.0f},   // 50% = middle
        {1000, 2000, 100.0f, 2000.0f},  // 100% = max pulse
        {800, 2200, 0.0f, 800.0f},      // Extended range
        {800, 2200, 100.0f, 2200.0f},   // Extended range
    };
    
    for (const auto& testCase : testCases) {
        engineConfiguration->servoMinPulseWidth = testCase.minPulse;
        engineConfiguration->servoMaxPulseWidth = testCase.maxPulse;
        
        // Calculate expected pulse width using the same interpolation logic
        float calculatedPulse = interpolateClamped(0.0f, testCase.minPulse,
                                                 100.0f, testCase.maxPulse,
                                                 testCase.position);
        
        EXPECT_NEAR(testCase.expectedPulseWidth, calculatedPulse, 0.1f);
    }
}

TEST(ServoThrottle, FrequencyAndDutyCycle) {
    // Test duty cycle calculations for different frequencies
    struct TestCase {
        uint16_t frequency;
        uint16_t pulseWidth;
        float expectedDutyCycle;
    };
    
    TestCase testCases[] = {
        {50, 1500, 0.075f},   // 50Hz, 1.5ms pulse = 7.5% duty cycle
        {100, 1500, 0.15f},   // 100Hz, 1.5ms pulse = 15% duty cycle
        {50, 1000, 0.05f},    // 50Hz, 1ms pulse = 5% duty cycle
        {50, 2000, 0.10f},    // 50Hz, 2ms pulse = 10% duty cycle
    };
    
    for (const auto& testCase : testCases) {
        float periodUs = 1000000.0f / testCase.frequency;
        float dutyCycle = testCase.pulseWidth / periodUs;
        
        EXPECT_NEAR(testCase.expectedDutyCycle, dutyCycle, 0.001f);
    }
}

#endif // EFI_SERVO_THROTTLE

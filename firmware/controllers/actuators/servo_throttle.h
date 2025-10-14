#pragma once

#include "rusefi_enums.h"
#include "engine_module.h"
#include "sensor.h"

#if EFI_SERVO_THROTTLE

class ServoThrottleController : public EngineModule {
public:
    void onConfigurationChange(engine_configuration_s const * previousConfig) override;
    void onSlowCallback() override;
    
    void setTargetPosition(float targetPercent);
    float getCurrentPosition() const { return m_currentPosition; }
    float getTargetPosition() const { return m_targetPosition; }
    bool isInitialized() const { return m_isInitialized; }
    
    // Virtual TPS functionality
    void registerVirtualTps();
    void unregisterVirtualTps();
    
private:
    void initPwmServo();
    void updatePwmServo(float position);
    void reset();
    float calculateServoAngle(float position) const;
    float calculateThrottlePosition(float servoAngle) const;
    
    float m_targetPosition = 0;
    float m_currentPosition = 0;
    float m_currentServoAngle = 0;
    bool m_isInitialized = false;
    bool m_virtualTpsRegistered = false;
};

ServoThrottleController* getServoThrottleController();

// Virtual TPS sensor function
SensorResult getServoTpsPosition();

#endif // EFI_SERVO_THROTTLE

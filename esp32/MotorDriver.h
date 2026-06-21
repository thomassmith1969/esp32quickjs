// MotorDriver.h
//
// Native C++ class for controlling a bidirectional motor via an H-bridge.
// BOTH pins are PWM-capable: forwardPin_ carries the PWM signal when
// speed > 0, reversePin_ carries it when speed < 0. Direction is
// implicit in the sign of the speed. Optional quadrature encoder on
// encPinA/encPinB for position feedback.

#pragma once
#include <Arduino.h>
#include <functional>

// Forward declaration to avoid pulling QuickJS.h into this header
// (QuickJS.h includes MotorDriver.h, so a full include here would
// be circular). The JSAnalog class is the centralized LEDC channel
// allocator — MotorDriver borrows channels from it instead of
// running its own bitmap, so analogWrite() calls elsewhere in the
// sketch can't collide with the motor's PWM.
class JSAnalog;

class MotorDriver {
public:
    // Callback type for encoder position updates
    using PositionCallback = std::function<void(int position)>;

    // Wire the JSAnalog pointer. Called once from
    // ESP32QuickJS::begin() after both objects exist. Must be
    // called before begin(). Safe to leave null only if you never
    // call begin() (i.e. you only use the object as a placeholder).
    void setAnalog(JSAnalog* a) { analog = a; }

    // Initialize the motor driver
    //
    // forwardPin: PWM-capable pin driven when speed > 0
    // reversePin: PWM-capable pin driven when speed < 0
    // encPinA:    Optional encoder channel A pin (set to -1 to disable encoder)
    // encPinB:    Optional encoder channel B pin
    void begin(int forwardPin, int reversePin, int encPinA = -1, int encPinB = -1);

    // Deinitialize the motor driver and release resources
    void end();

    // Set speed in [-1.0, 1.0]. Negative = reverse, positive = forward,
    // 0 = coast. Values outside the range are clamped.
    void setSpeed(float speed);

    // Current speed (last value passed to setSpeed, after clamping).
    float getSpeed() const { return currentSpeed_; }

    // Current direction: 0 = stopped, 1 = forward, 2 = reverse.
    uint8_t getDirection() const { return currentDirection_; }

    // Set target distance to move (requires encoder)
    // Returns false if encoder is not enabled
    // distance: target distance in units (e.g., mm)
    // speed: speed to move at (units per second)
    bool setTargetDistance(int distance, float speed);

    // Get current encoder position (if encoder enabled)
    int getPosition() const;

    // Stop the motor (speed = 0)
    void stop();

    // Call this periodically from the main loop to handle non-blocking operations
    void loop();

    // Set callback for position updates
    void setPositionCallback(PositionCallback callback);

private:
    // Pointer to the centralized LEDC allocator. Set by
    // ESP32QuickJS::begin(). begin() will fail (return without
    // activating) if this is null.
    JSAnalog* analog = nullptr;

    // Pin assignments
    int forwardPin_ = -1;
    int reversePin_ = -1;
    int encPinA_ = -1;
    int encPinB_ = -1;
    float stepsPerUnit_ = 1.0f; // Steps per unit distance (e.g., steps per mm)

    // LEDC channels borrowed from JSAnalog. -1 means "not allocated".
    int forwardChannel_ = -1;
    int reverseChannel_ = -1;

    // True between successful begin() and end().
    bool active_ = false;

    // State
    float currentSpeed_ = 0.0f;
    uint8_t currentDirection_ = 0;
    int position_ = 0;
    int targetPosition_ = 0;
    float targetSpeed_ = 0.0f;
    bool distanceMode_ = false;

    // Encoder state
    bool encoderActive_ = false;
    volatile int encoderPosition_ = 0;
    uint8_t encoderState_ = 0;

    // Callback
    PositionCallback positionCallback_ = nullptr;

    // Encoder ISR handlers
    static void IRAM_ATTR isrA(void* arg);
    static void IRAM_ATTR isrB(void* arg);

    // Handle encoder transition
    void handleInterrupt(bool a);

    // Sample both encoder pins into a 2-bit value (bit 0 = A, bit 1 = B).
    uint8_t readEncoderState() const;

    // Update motor output based on current speed/direction
    void updateMotor();
};
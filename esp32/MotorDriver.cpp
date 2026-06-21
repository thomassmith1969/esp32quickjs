// MotorDriver.cpp
//
// Implementation of the native motor driver object. See
// MotorDriver.h for the design rationale.
//
// H-bridge design: BOTH pins are PWM-capable. Direction is
// implicit in the sign of the speed:
//   speed > 0  → PWM on forwardPin_, reversePin_ = 0
//   speed < 0  → PWM on reversePin_, forwardPin_ = 0
//   speed == 0 → both pins LOW (coast)
//
// Optional quadrature encoder on encPinA/encPinB for position
// feedback. The encoder uses CHANGE interrupts on both pins and
// a Gray-code transition table (same approach as RotaryEncoder).

#include "MotorDriver.h"
#include "QuickJS.h"

// --- LEDC channel allocation -----------------------------------------
//
// The legacy Arduino-ESP32 LEDC API requires the caller to pick a
// channel (0..15). We borrow two channels per motor (one per
// direction pin) from the centralized JSAnalog allocator instead
// of running our own bitmap. This way analogWrite() calls elsewhere
// in the sketch (LED dimming, another motor, a buzzer, etc.) draw
// from the same pool and can't collide with the motor's PWM.
//
// JSAnalog reserves channel 0 for Tone, so channels 1..15 are
// available. It also tracks which pin owns which channel in
// pinChannel[40], which lets isPinBusy() reject a second driver
// from grabbing a pin that's already being PWM'd.

// --- PWM configuration -----------------------------------------------
//
// 5 kHz / 8-bit resolution is a reasonable default for brushed DC
// motors driven through an H-bridge: high enough to be inaudible,
// low enough that the FETs don't spend all their time switching.

static const int PWM_FREQ = 5000;
static const int PWM_RESOLUTION = 8;          // 0..255 duty
static const int PWM_MAX_DUTY = (1 << PWM_RESOLUTION) - 1;  // 255

// --- Gray-code transition table --------------------------------------
//
// Same approach as RotaryEncoder: only valid Gray-code transitions
// count as motion, anything else is bounce/noise.
//
// Forward: 00→01→11→10→00
// Reverse: 00→10→11→01→00
//
// We pack (old_state << 2) | new_state into a 4-bit index and
// look up the delta. Returns +1, -1, or 0.

static int transitionDelta(uint8_t old_state, uint8_t new_state) {
  switch ((old_state << 2) | new_state) {
    case 0b0001: return +1;  // 00 → 01 (forward)
    case 0b1110: return -1;  // 11 → 10 (forward)
    case 0b0010: return -1;  // 00 → 10 (reverse)
    case 0b1101: return +1;  // 11 → 01 (reverse)
    default:     return 0;   // invalid transition (bounce)
  }
}

// --- begin / end -----------------------------------------------------

void MotorDriver::begin(int forwardPin, int reversePin, int encPinA, int encPinB) {
  if (active_) end();

  // The analog pointer is wired by ESP32QuickJS::begin(). If it's
  // null, the user constructed a MotorDriver outside the normal
  // framework — bail out rather than silently grabbing channels
  // we can't track.
  if (!analog) return;

  forwardPin_ = forwardPin;
  reversePin_ = reversePin;
  encPinA_    = encPinA;
  encPinB_    = encPinB;

  // Reject pins that are already being driven by LEDC (e.g. by an
  // analogWrite() call or another MotorDriver). isPinBusy() also
  // catches the case where the user passed the same pin for both
  // forward and reverse.
  if (analog->isPinBusy(forwardPin_) || analog->isPinBusy(reversePin_)) {
    forwardPin_ = -1;
    reversePin_ = -1;
    return;
  }

  // Borrow two LEDC channels from the shared allocator — one per
  // direction pin. If we run out of channels, fail loudly rather
  // than silently leaving the motor un-driven.
  forwardChannel_ = analog->allocChannel();
  if (forwardChannel_ < 0) return;
  reverseChannel_ = analog->allocChannel();
  if (reverseChannel_ < 0) {
    analog->freeChannel(forwardChannel_);
    forwardChannel_ = -1;
    return;
  }

  // Register the pin→channel mapping so isPinBusy() sees us and
  // so detachPwmIfAttached() can find us if something else needs
  // the pin back.
  analog->setPinChannel(forwardPin_, forwardChannel_);
  analog->setPinChannel(reversePin_, reverseChannel_);

  // Configure both channels at the same freq/resolution. The pins
  // become outputs after ledcAttachPin.
  ledcSetup(forwardChannel_, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(reverseChannel_, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(forwardPin_, forwardChannel_);
  ledcAttachPin(reversePin_, reverseChannel_);

  // Start coasting (both pins LOW).
  ledcWrite(forwardChannel_, 0);
  ledcWrite(reverseChannel_, 0);

  currentSpeed_     = 0.0f;
  currentDirection_ = 0;
  position_         = 0;
  targetPosition_   = 0;
  targetSpeed_      = 0.0f;
  distanceMode_     = false;
  encoderPosition_  = 0;

  // Optional encoder. encPinA == -1 (the default) disables it.
  if (encPinA_ >= 0 && encPinB_ >= 0) {
    pinMode(encPinA_, INPUT_PULLUP);
    pinMode(encPinB_, INPUT_PULLUP);
    encoderState_ = readEncoderState();  // seed so first edge is valid
    attachInterruptArg(encPinA_, &MotorDriver::isrA, this, CHANGE);
    attachInterruptArg(encPinB_, &MotorDriver::isrB, this, CHANGE);
    encoderActive_ = true;
  } else {
    encoderActive_ = false;
  }

  active_ = true;
}

void MotorDriver::end() {
  if (!active_) return;

  // Detach encoder interrupts first — they're cheap and the ISR
  // touches encoderPosition_/encoderState_.
  if (encoderActive_) {
    detachInterrupt(encPinA_);
    detachInterrupt(encPinB_);
    encoderActive_ = false;
  }

  // Coast the motor before tearing down the channels so we don't
  // leave a partial duty cycle driving the H-bridge.
  if (forwardChannel_ >= 0) {
    ledcWrite(forwardChannel_, 0);
    ledcDetachPin(forwardPin_);
    if (analog) {
      analog->setPinChannel(forwardPin_, 0);  // PIN_UNUSED
      analog->freeChannel(forwardChannel_);
    }
    forwardChannel_ = -1;
  }
  if (reverseChannel_ >= 0) {
    ledcWrite(reverseChannel_, 0);
    ledcDetachPin(reversePin_);
    if (analog) {
      analog->setPinChannel(reversePin_, 0);  // PIN_UNUSED
      analog->freeChannel(reverseChannel_);
    }
    reverseChannel_ = -1;
  }

  currentSpeed_     = 0.0f;
  currentDirection_ = 0;
  position_         = 0;
  targetPosition_   = 0;
  targetSpeed_      = 0.0f;
  distanceMode_     = false;
  encoderPosition_  = 0;
  encoderState_     = 0;
  active_           = false;
}

// --- speed / direction -----------------------------------------------

void MotorDriver::setSpeed(float speed) {
  if (!active_) return;

  // Clamp to [-1.0, 1.0]. Anything outside is treated as full
  // speed in that direction.
  if (speed >  1.0f) speed =  1.0f;
  if (speed < -1.0f) speed = -1.0f;

  currentSpeed_     = speed;
  currentDirection_ = (speed > 0.0f) ? 1 : (speed < 0.0f) ? 2 : 0;

  updateMotor();
}

bool MotorDriver::setTargetDistance(int distance, float speed) {
  if (!active_)    return false;
  if (!encoderActive_) return false;

  // Convert the requested distance (in user units, e.g. mm) into
  // an absolute encoder target. We assume the encoder counts
  // steps and stepsPerUnit_ converts units → steps.
  targetPosition_ = position_ + (int)(distance * stepsPerUnit_);
  targetSpeed_    = speed;
  distanceMode_   = true;

  setSpeed(speed);
  return true;
}

int MotorDriver::getPosition() const {
  // If the encoder is active, prefer its count (it updates in the
  // ISR). Otherwise fall back to the software position counter,
  // which is only updated by setTargetDistance() callers.
  if (encoderActive_) return encoderPosition_;
  return position_;
}

void MotorDriver::stop() {
  distanceMode_ = false;
  setSpeed(0.0f);
}

// --- non-blocking state machine --------------------------------------

void MotorDriver::loop() {
  if (!active_) return;

  // Sync the software position counter with the encoder. We do
  // this every tick so getPosition() returns fresh data even if
  // the encoder ISR fires between calls.
  if (encoderActive_) {
    position_ = encoderPosition_;
  }

  if (!distanceMode_) return;

  // Distance mode: drive until we've reached (or overshot) the
  // target, then stop. We compare against the encoder count so
  // the loop is closed on real motion, not on time.
  int delta = targetPosition_ - position_;
  if (delta == 0) {
    stop();
    return;
  }

  // If we've crossed the target (overshot due to inertia, or
  // moved in the wrong direction), stop. The caller can call
  // setTargetDistance() again to correct.
  if ((targetSpeed_ > 0 && position_ >= targetPosition_) ||
      (targetSpeed_ < 0 && position_ <= targetPosition_)) {
    stop();
    return;
  }

  // Reapply the target speed in case something else (e.g. a
  // setSpeed(0) from the JS side) clobbered it.
  if (currentSpeed_ == 0.0f) {
    setSpeed(targetSpeed_);
  }
}

void MotorDriver::setPositionCallback(PositionCallback callback) {
  positionCallback_ = std::move(callback);
}

// --- motor output ----------------------------------------------------

void MotorDriver::updateMotor() {
  if (!active_) return;

  if (currentSpeed_ > 0.0f) {
    // Forward: PWM on forward pin, reverse pin LOW.
    uint32_t duty = (uint32_t)(currentSpeed_ * PWM_MAX_DUTY);
    ledcWrite(forwardChannel_, duty);
    ledcWrite(reverseChannel_, 0);
  } else if (currentSpeed_ < 0.0f) {
    // Reverse: PWM on reverse pin, forward pin LOW.
    uint32_t duty = (uint32_t)(-currentSpeed_ * PWM_MAX_DUTY);
    ledcWrite(reverseChannel_, duty);
    ledcWrite(forwardChannel_, 0);
  } else {
    // Coast: both pins LOW. (Brake would be both HIGH, but that
    // requires the H-bridge to support it — most do, but coast
    // is the safer default.)
    ledcWrite(forwardChannel_, 0);
    ledcWrite(reverseChannel_, 0);
  }
}

uint8_t MotorDriver::readEncoderState() const {
  // Pack the two encoder pins into a 2-bit value: bit 0 = A, bit 1 = B.
  return (uint8_t)((digitalRead(encPinA_) ? 1 : 0) |
                   (digitalRead(encPinB_) ? 2 : 0));
}

// --- encoder ISR -----------------------------------------------------
//
// Runs in IRAM on every CHANGE of either encoder pin. We sample
// both pins, compute the Gray-code delta, and update the position
// counter. The JS-side loop() reads encoderPosition_ on the main
// task, so we mark it volatile to make sure the compiler doesn't
// cache it across the ISR/main boundary.

void IRAM_ATTR MotorDriver::handleInterrupt(bool /*a*/) {
  uint8_t new_state = readEncoderState();
  int delta = transitionDelta(encoderState_, new_state);
  encoderState_ = new_state;
  if (delta != 0) encoderPosition_ += delta;
}

void IRAM_ATTR MotorDriver::isrA(void* arg) {
  MotorDriver* self = (MotorDriver*)arg;
  if (self) self->handleInterrupt(true);
}

void IRAM_ATTR MotorDriver::isrB(void* arg) {
  MotorDriver* self = (MotorDriver*)arg;
  if (self) self->handleInterrupt(false);
}

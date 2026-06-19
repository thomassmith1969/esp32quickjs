// RotaryEncoder.cpp
//
// Implementation of the native rotary encoder object. See
// RotaryEncoder.h for the design rationale.

#include "RotaryEncoder.h"

RotaryEncoder::RotaryEncoder()
    : _pinA(0),
      _pinB(0),
      _last_state(0),
      _position(0),
      _active(false) {}

RotaryEncoder::~RotaryEncoder() { end(); }

int RotaryEncoder::transitionDelta(uint8_t old_state,
                                   uint8_t new_state) {
  // Gray-code transitions only — anything else is bounce/noise.
  // Forward: 00→01→11→10→00
  // Reverse: 00→10→11→01→00
  switch ((old_state << 2) | new_state) {
    case 0b0001: return +1;  // 00 → 01 (forward)
    case 0b1110: return -1;  // 11 → 10 (forward)
    case 0b0010: return -1;  // 00 → 10 (reverse)
    case 0b1101: return +1;  // 11 → 01 (reverse)
    default:     return 0;   // invalid transition (bounce)
  }
}

bool RotaryEncoder::begin(uint8_t pinA, uint8_t pinB) {
  if (_active) end();

  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);

  _pinA = pinA;
  _pinB = pinB;
  _position = 0;
  _active = true;
  // Seed last_state from the current pin levels so the first edge
  // computes a valid transition (otherwise we'd compare against 0
  // and likely get a bogus delta).
  _last_state = readState(pinA, pinB);

  // Attach CHANGE interrupts on both pins. Each ISR uses `this`
  // as its user-data arg so it can find its owning instance.
  attachInterruptArg(pinA, &RotaryEncoder::isrA, this, CHANGE);
  attachInterruptArg(pinB, &RotaryEncoder::isrB, this, CHANGE);
  return true;
}

void RotaryEncoder::end() {
  if (!_active) return;
  detachInterrupt(_pinA);
  detachInterrupt(_pinB);
  _active = false;
  _position = 0;
  _last_state = 0;
}

int32_t RotaryEncoder::position() const { return _position; }

void RotaryEncoder::position(int32_t value) { _position = value; }

void IRAM_ATTR RotaryEncoder::handleInterrupt() {
  uint8_t new_state = readState(_pinA, _pinB);
  int delta = transitionDelta(_last_state, new_state);
  _last_state = new_state;
  if (delta != 0) _position += delta;
}

void IRAM_ATTR RotaryEncoder::isrA(void* arg) {
  RotaryEncoder* self = (RotaryEncoder*)arg;
  if (self) self->handleInterrupt();
}

void IRAM_ATTR RotaryEncoder::isrB(void* arg) {
  RotaryEncoder* self = (RotaryEncoder*)arg;
  if (self) self->handleInterrupt();
}

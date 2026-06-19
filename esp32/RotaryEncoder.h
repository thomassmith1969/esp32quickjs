// RotaryEncoder.h
//
// Native (non-JS) rotary encoder object. Holds the real hardware
// state and the ISR. Multiple instances are supported — each
// instance owns its own pin pair and counter.
//
// The JS wrapper (JSRotaryEncoder) holds a pointer to one of these
// and forwards calls. This keeps the JS layer thin and lets the
// native layer be reused without QuickJS.

#pragma once

#include <Arduino.h>
#include <stdint.h>

class RotaryEncoder {
 public:
  RotaryEncoder();
  ~RotaryEncoder();

  // Allocate pins, attach CHANGE interrupts. Returns true on
  // success, false if pins are invalid or interrupts can't be
  // attached.
  bool begin(uint8_t pinA, uint8_t pinB);

  // Detach interrupts. Safe to call multiple times.
  void end();

  // Read the current position counter.
  int32_t position() const;

  // Set the position counter to an arbitrary value.
  void position(int32_t value);

  // Pin accessors (useful for diagnostics).
  uint8_t pinA() const { return _pinA; }
  uint8_t pinB() const { return _pinB; }
  bool active() const { return _active; }

 private:
  // Quadrature state transition table. Returns +1 for forward,
  // -1 for reverse, 0 for invalid/noise.
  // State encoding: bit0 = pinA, bit1 = pinB.
  static int transitionDelta(uint8_t old_state, uint8_t new_state);

  // Read both pins and pack into 2-bit state (bit0=A, bit1=B).
  static inline uint8_t readState(uint8_t pinA, uint8_t pinB) {
    return (uint8_t)((digitalRead(pinA) & 1) |
                     ((digitalRead(pinB) & 1) << 1));
  }

  // ISR trampolines. Each takes the owning RotaryEncoder* as arg
  // (passed via attachInterruptArg) so it can find its instance
  // without a static lookup table.
  static void IRAM_ATTR isrA(void* arg);
  static void IRAM_ATTR isrB(void* arg);

  // Common ISR body. Reads both pins, computes the transition
  // delta, and updates the counter.
  void IRAM_ATTR handleInterrupt();

  uint8_t _pinA;
  uint8_t _pinB;
  // Last sampled (A,B) state. Updated in the ISR.
  volatile uint8_t _last_state;
  // Current position count. Updated in the ISR.
  volatile int32_t _position;
  bool _active;
};

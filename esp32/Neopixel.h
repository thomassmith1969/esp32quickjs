#pragma once

#include <Arduino.h>
#include <FastLED_min.h>
#include <driver/rmt.h>
#include <cstring>
#include <cstdlib>

// NeoPixel wrapper using FastLED_min's CRGB pixel type.
// Uses ESP32 RMT directly for transmission (FastLED_min's template
// pin is compile-time, but JS passes pin at runtime — so we handle
// RMT ourselves while using CRGB for pixel storage and API compat).
//
// All methods except show() are synchronous buffer operations.
// show() builds RMT items from the CRGB array and calls
// rmt_write_items() — blocking while the RMT transmits. This runs
// on the JSWorker task so the main loop never stalls.
class Neopixel {
 public:
  Neopixel() {}
  ~Neopixel() { end(); }

  bool begin(uint8_t pin, uint16_t numPixels,
             rmt_channel_t channel = RMT_CHANNEL_0) {
    if (leds_) end();
    pin_ = pin;
    channel_ = channel;
    numLeds_ = numPixels;

    leds_ = (CRGB *)malloc(numLeds_ * sizeof(CRGB));
    if (!leds_) { numLeds_ = 0; return false; }

    // Build RMT config — clk_div = 4 → 20MHz → 50ns/tick
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)pin, channel);
    config.clk_div = 4;
    config.tx_config.loop_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    if (rmt_config(&config) != ESP_OK) { free(leds_); leds_ = nullptr; numLeds_ = 0; return false; }
    if (rmt_driver_install(config.channel, 0, 0) != ESP_OK) { free(leds_); leds_ = nullptr; numLeds_ = 0; return false; }

    clear();
    show();
    return true;
  }

  void end() {
    if (channel_ != RMT_CHANNEL_MAX) {
      rmt_driver_uninstall(channel_);
      channel_ = RMT_CHANNEL_MAX;
    }
    if (leds_) { free(leds_); leds_ = nullptr; }
    numLeds_ = 0;
  }

  // ---- Synchronous pixel operations ----

  void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!leds_ || index >= numLeds_) return;
    leds_[index] = CRGB(r, g, b);
  }

  void setPixelColor(uint16_t index, CRGB color) {
    if (!leds_ || index >= numLeds_) return;
    leds_[index] = color;
  }

  CRGB getPixelColor(uint16_t index) const {
    if (!leds_ || index >= numLeds_) return CRGB::Black;
    return leds_[index];
  }

  // Return as uint32 0x00RRGGBB for JS
  uint32_t getPixelColorU32(uint16_t index) const {
    if (!leds_ || index >= numLeds_) return 0;
    return ((uint32_t)leds_[index].r << 16) |
           ((uint32_t)leds_[index].g << 8)  |
           leds_[index].b;
  }

  void fill(uint8_t r, uint8_t g, uint8_t b) {
    if (!leds_) return;
    CRGB c(r, g, b);
    for (uint16_t i = 0; i < numLeds_; i++) leds_[i] = c;
  }

  void fill(CRGB color) {
    if (!leds_) return;
    for (uint16_t i = 0; i < numLeds_; i++) leds_[i] = color;
  }

  void clear() {
    if (leds_) memset(leds_, 0, numLeds_ * sizeof(CRGB));
  }

  uint16_t numPixels() const { return numLeds_; }

  // ---- Blocking operation (call from JSWorker task only) ----
  // Builds and sends WS2812 bitstream via RMT.

  void show() {
    if (!leds_ || numLeds_ == 0) return;

    // WS2812 timing at 20MHz (50ns/tick):
    //   T0H = 0.35µs = 7 ticks   T0L = 0.90µs = 18 ticks
    //   T1H = 0.90µs = 18 ticks  T1L = 0.35µs = 7 ticks
    static constexpr uint16_t T0H = 7, T0L = 18;
    static constexpr uint16_t T1H = 18, T1L = 7;

    // Allocate RMT items on each show — max ~LEDs*24 items
    uint32_t totalItems = numLeds_ * 24;
    rmt_item32_t *items = (rmt_item32_t *)malloc(totalItems * sizeof(rmt_item32_t));
    if (!items) return;

    uint32_t idx = 0;
    for (uint16_t p = 0; p < numLeds_; p++) {
      uint8_t grb[3] = { leds_[p].g, leds_[p].r, leds_[p].b };
      for (int b = 0; b < 3; b++) {
        uint8_t byte = grb[b];
        for (int bit = 7; bit >= 0; bit--) {
          if (byte & (1 << bit)) {
            items[idx].duration0 = T1H; items[idx].level0 = 1;
            items[idx].duration1 = T1L; items[idx].level1 = 0;
          } else {
            items[idx].duration0 = T0H; items[idx].level0 = 1;
            items[idx].duration1 = T0L; items[idx].level1 = 0;
          }
          idx++;
        }
      }
    }

    rmt_write_items(channel_, items, idx, true);
    free(items);
    delayMicroseconds(60); // reset pulse >50µs
  }

  CRGB *leds() { return leds_; }
  uint8_t pin() const { return pin_; }

 private:
  CRGB        *leds_    = nullptr;
  uint16_t     numLeds_ = 0;
  uint8_t      pin_     = 0;
  rmt_channel_t channel_ = RMT_CHANNEL_MAX;
};
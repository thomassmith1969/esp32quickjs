/**
 * Example module demonstrating how to create a QuickJS module.
 * 
 * To use this module:
 * 1. Register it: esp32.registerModule("myModule", "/path/to/myModule.js")
 * 2. Import it: const mod = await import("myModule")
 * 3. Access the module namespace: const ns = mod.default
 * 
 * The module must export a function named `moduleInit` that returns
 * the module namespace object.
 */

// Example: Create a simple LED controller module
// This module controls an LED connected to GPIO 2 (GPIO_NUM LED in ESP32)

// Module namespace object
const module_ns = {
  // Initialize the LED (call this once at startup)
  init: async function() {
    // Set up LED control via QuickJS's GPIO interface
    // Note: This is a placeholder - actual GPIO control would use
    // the ESP32QuickJS GPIO API if available
    console.log("LED module initialized");
    return { ledOn: true };
  },
  
  // Turn LED on
  on: async function() {
    console.log("LED turned ON");
    return { ledOn: true };
  },
  
  // Turn LED off
  off: async function() {
    console.log("LED turned OFF");
    return { ledOn: false };
  },
  
  // Toggle LED state
  toggle: async function() {
    const current = this.ledOn;
    this.ledOn = !current;
    return { ledOn: this.ledOn };
  },
  
  // Get current LED state
  getState: async function() {
    return { ledOn: this.ledOn };
  }
};

// Export the moduleInit function and namespace
module.exports = {
  moduleInit: module_ns.init,
  default: module_ns
};

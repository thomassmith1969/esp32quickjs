/**
 * Example: Using module imports with ESP32QuickJS
 * 
 * This example demonstrates how to use the module system to load
 * external JavaScript modules from the filesystem.
 * 
 * Steps:
 * 1. Register a module: esp32.registerModule("myModule", "/path/to/myModule.js")
 * 2. Import the module: const mod = await import("myModule")
 * 3. Access the module namespace: const ns = mod.default
 * 
 * Note: This example assumes ENABLE_FS is defined and the filesystem
 * is mounted.
 */

// Example 1: Using a pre-registered module
async function example1() {
  // Import the module (this will load it from the filesystem)
  const mod = await import("myModule");
  
  // Access the module namespace
  const ns = mod.default;
  
  // Initialize the module
  const result = await ns.init();
  console.log("Module initialized:", result);
  
  // Use the module's functions
  await ns.on();
  await ns.toggle();
  const state = await ns.getState();
  console.log("LED state:", state);
}

// Example 2: Using requireModule() directly
async function example2() {
  // Load a module directly (equivalent to import())
  const module_ns = esp32.requireModule("myModule");
  
  if (JS_IsException(module_ns)) {
    console.error("Failed to load module");
    return;
  }
  
  // Get the module namespace
  const ns = JS_GetProperty(ctx, module_ns, "default");
  
  // Initialize and use the module
  const result = await ns.init();
  console.log("Module initialized:", result);
}

// Example 3: Creating a custom module
async function example3() {
  // Create a simple counter module
  const counter = {
    value: 0,
    increment: async function() {
      this.value++;
      return this.value;
    },
    decrement: async function() {
      this.value--;
      return this.value;
    },
    reset: async function() {
      this.value = 0;
      return this.value;
    }
  };
  
  // Export the moduleInit function and namespace
  const mod = {
    moduleInit: async function() {
      return { counter };
    },
    default: counter
  };
  
  // Use the module
  const result = await mod.moduleInit();
  console.log("Counter initialized:", result);
  
  await result.counter.increment();
  console.log("Counter value:", await result.counter.increment());
}

// Example 4: Module with async operations
async function example4() {
  // Create a module that performs async operations
  const asyncModule = {
    fetchData: async function(url) {
      // Simulate async operation
      await new Promise(resolve => setTimeout(resolve, 100));
      return { data: "Hello from async module!" };
    }
  };
  
  const mod = {
    moduleInit: async function() {
      return { asyncModule };
    },
    default: asyncModule
  };
  
  const result = await mod.moduleInit();
  const data = await result.asyncModule.fetchData("http://example.com");
  console.log("Fetched data:", data);
}

// Example 5: Module with error handling
async function example5() {
  try {
    // Try to import a non-existent module
    const mod = await import("nonExistentModule");
    console.log("Module loaded:", mod);
  } catch (e) {
    console.error("Failed to load module:", e);
  }
}

// Run examples
async function main() {
  console.log("=== ESP32QuickJS Module Examples ===\n");
  
  await example1();
  console.log("\n---\n");
  
  await example2();
  console.log("\n---\n");
  
  await example3();
  console.log("\n---\n");
  
  await example4();
  console.log("\n---\n");
  
  await example5();
  console.log("\n---\n");
  
  console.log("All examples completed!");
}

// Export the main function
module.exports = {
  moduleInit: async function() {
    return { main };
  },
  default: { main }
};

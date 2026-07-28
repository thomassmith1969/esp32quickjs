// JSRepl.h
//
// Read-Eval-Print-Loop subsystem for the QuickJS embed. Owns:
//   - processCommand(): trim, eval (sync or async via await detection),
//     JSON.stringify result, Promise.then() unwrapping, route output
//     to activeOutputStream.
//   - evaluateCode(): wraps a code string in setTimeout(()=>{...}, 0)
//     and evals it. Used for startup scripts and other fire-and-forget
//     contexts.
//   - stringify(JSValue): JSON.stringify helper.
//   - Serial REPL byte-level read loop and multi-line statement
//     detection. Pumps input from Serial, detects complete statements
//     (balanced parens / brackets / braces / strings / escapes), and
//     calls processCommand on each complete statement.
//   - runStartupScript(path): mounts LittleFS, reads the file at the
//     given path, and evaluates its contents via evaluateCode().
//   - runMainLoop(): creates the dedicated FreeRTOS task that runs
//     the JS main loop (with a 16KB stack), then pumps Serial input
//     and calls qjs.loop() in a tight loop.
//   - tickMainLoop(): called from Arduino loop() to wake the main
//     loop task via a semaphore.
//
// The example app calls runMainLoop() once from setup() and
// tickMainLoop() from loop(). Everything else (threading, Serial
// read, multi-line statement detection, REPL eval, telnet REPL,
// pump callback, startup script loading) is owned by the library.

#pragma once

#include <Arduino.h>
#include <string>
#include "../quickjs.h"

class ESP32QuickJS;

// Callback signature for a command-line evaluator. Receives the raw
// (untrimmed) command buffer and the caller-owned buffer length. The
// callback should return true on success or false on failure. The
// returned JS source is evaluated as a single QuickJS script.
//
// Note: the example app can override this to inject extra behaviour
// (e.g. logging, history). The default identity evaluator just
// returns the input unchanged.
using JSReplEvaluatorFn = bool (*)(char* command, size_t len);

class JSRepl {
 public:
  JSRepl();

  // Wire this REPL helper to a QuickJS instance. Required before
  // any of the run/tick/processCommand calls.
  void attach(ESP32QuickJS* qjs);

  // Override the application-specific evaluator. Default identity
  // returns the command unchanged.
  static void setEvaluator(JSReplEvaluatorFn fn);

  // ---- Eval helpers (used by both Serial REPL and Telnet REPL) ----

  // Process a single fully-buffered command line. Trims, evaluates,
  // routes output to activeOutputStream. Returns true on success
  // (including empty input), false on exception. Empty or all-
  // whitespace input is a silent no-op.
  //
  // MUST be called from the same task as qjs (QuickJS is not
  // thread-safe). MUST NOT be called from inside a JS callback.
  bool processCommand(char* command);

  // Convenience: evaluate a block of JS code wrapped in
  // setTimeout(() => { ... }, 0) so that the call returns immediately
  // and the JS runs in a later microtask.
  bool evaluateCode(char* code);

  // Convenience: convert a JSValue to its JSON-stringified form.
  std::string stringify(JSValue result);

  // ---- Serial REPL ----

  // Read available bytes from Serial, buffer them, and call
  // processCommand() on each complete statement. Detects multi-line
  // statements (balanced parens/brackets/braces, string awareness,
  // escape handling). MUST be called from the JS task (not an ISR).
  void pumpSerial();

  // ---- Startup script ----

  // Mount LittleFS, read the file at `path`, and evaluate its
  // contents. Returns true if the file existed and was evaluated,
  // false otherwise. Prints status messages to Serial.
  bool runStartupScript(const char* path);

  // ---- Main loop task lifecycle ----

  // Spawn the dedicated FreeRTOS main-loop task. The task has its own
  // 16KB stack (vs Arduino loop()'s 8KB) so JS_Eval, module loading,
  // and require() have plenty of stack room. After this call,
  // tickMainLoop() should be invoked from Arduino loop() to wake the
  // main-loop task on each iteration.
  //
  // `serialBaud` is the Serial.begin() rate for the REPL's serial
  // console. Pass 0 to skip Serial.begin() (caller already did it).
  // `startupPath` is the path to a JS file to evaluate on startup;
  // pass nullptr to skip startup script evaluation.
  //
  // Returns true if the task was created successfully.
  bool runMainLoop(uint32_t serialBaud = 115200,
                   const char* startupPath = "/startup.js");

  // Wake the main-loop task. Call from Arduino loop(). The main-loop
  // task will then drain Serial input, run qjs.loop(), and yield.
  void tickMainLoop();

  // Accessor for the wired QuickJS instance. Used internally by the
  // main-loop task.
  ESP32QuickJS* getQjs() { return qjs_; }

  // Static trampoline for the dedicated FreeRTOS main-loop task.
  // xTaskCreatePinnedToCore requires a plain C function pointer, so
  // we can't pass a capturing lambda. The trampoline reads
  // g_mainLoopStartupPath (set by runMainLoop) and dispatches to
  // g_jsrepl->getQjs().
  static void mainLoopTaskTrampoline(void* arg);

 private:
  ESP32QuickJS* qjs_ = nullptr;
};

// Default singleton — referenced by ESP32QuickJS via g_jsrepl.
extern JSRepl* g_jsrepl;

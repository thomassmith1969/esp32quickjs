#pragma once

#include "quickjs.h"
#include <vector>
#include "Neopixel.h"

// JS wrapper for Neopixel (WS2812/NeoPixel addressable LED strips).
//
// Architecture:
//   - Synchronous methods (setPixelColor, fill, clear,
//     getPixelColor, numPixels) follow the JSRotaryEncoder pattern — they just
//     modify the RAM buffer and return immediately.
//   - show() follows the JSMotorDriver async pattern — it submits a
//     WorkItem to the shared JSWorker, which calls the blocking RMT
//     transmission on the worker task. The JS side gets a Promise
//     that resolves when the pixels are physically updated.
//
// Lifecycle:
//   - init() called once from ESP32QuickJS::begin()
//   - loop(ctx) called every tick from ESP32QuickJS::loop() —
//     drains completed show() operations and resolves Promises.

class JSNeopixel {
 public:
  // ---- QuickJS class identity ----
  static JSClassID js_class_id;
  static const JSClassDef js_class_def;

  // ---- JS methods (static, get opaque Neopixel* from JSValue) ----
  static JSValue js_ctor(JSContext *ctx, JSValueConst new_target,
                         int argc, JSValueConst *argv);
  static JSValue js_setPixelColor(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
  static JSValue js_getPixelColor(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
  static JSValue js_fill(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv);
  static JSValue js_clear(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv);
  static JSValue js_numPixels(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
  static JSValue js_show(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv);
  static void js_finalizer(JSRuntime *rt, JSValue obj);
  static void js_gc_mark(JSRuntime *rt, JSValueConst obj,
                            JS_MarkFunc *mark_func);

  // ---- Lifecycle (called by ESP32QuickJS) ----
  void init();
  void loop(JSContext *ctx);

  // ---- Singleton for static JS callbacks ----
  static JSNeopixel *instance;

 private:
  // Helper: extract native Neopixel* from JS opaque slot.
  static Neopixel *getNeopixel(JSContext *ctx, JSValueConst this_val);

  // ---- Async show() queue ----
  struct PendingShow {
    Neopixel *neopixel;
    JSValue resolving_funcs[2];   // [0] = resolve, [1] = reject
    void *workItem;               // JSWorker::WorkItem*, freed after done
  };
  std::vector<PendingShow> pending_;
};
// JSRotaryEncoder.h
//
// JavaScript wrapper for RotaryEncoder. Each JS instance owns a
// pointer to a real RotaryEncoder (the native object). The JS
// trampolines forward calls to the native object.
//
// Pattern follows the Worker class in quickjs-libc.c:
//   - JS_NewObjectClass allocates a typed opaque slot
//   - JS_SetOpaque stores the native pointer
//   - JS_GetOpaque retrieves it
//   - Finalizer frees the native object on GC

#pragma once

#include "RotaryEncoder.h"
#include "../quickjs.h"

class JSRotaryEncoder {
 public:
  // Class id for RotaryEncoder instances. Allocated once at setup
  // time via JS_NewClassID.
  static JSClassID js_class_id;

  // Class definition for JS_NewObjectClass.
  static const JSClassDef js_class_def;

  // Constructor: `new RotaryEncoder(pinA, pinB)` → JS object.
  static JSValue js_ctor(JSContext* ctx, JSValueConst new_target,
                         int argc, JSValueConst* argv);

  // position() → number (getter)
  // position(value) → undefined (setter)
  static JSValue js_position(JSContext* ctx, JSValueConst jsThis,
                             int argc, JSValueConst* argv);

  // Finalizer: called when the JS object is GC'd. Frees the
  // native object.
  static void js_finalizer(JSRuntime* rt, JSValueConst obj);

  // GC mark: nothing to mark — the native object isn't a JS value.
  static void js_gc_mark(JSRuntime* rt, JSValueConst obj,
                         JS_MarkFunc* mark_func);

 private:
  // Helper: extract the native pointer from a JS object. Returns
  // nullptr if the object isn't a RotaryEncoder instance.
  static RotaryEncoder* getEncoder(JSValueConst obj);
};

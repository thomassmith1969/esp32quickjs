// JSRotaryEncoder.cpp
//
// Implementation of the JavaScript wrapper for RotaryEncoder.
// Each JS instance owns a pointer to a real RotaryEncoder (the
// native object). The JS trampolines forward calls to the native
// object.

#include "JSRotaryEncoder.h"

#include <Arduino.h>

// Static member definitions.
JSClassID JSRotaryEncoder::js_class_id = 0;

const JSClassDef JSRotaryEncoder::js_class_def = {
  "RotaryEncoder",
  js_finalizer,
  js_gc_mark,
};

RotaryEncoder* JSRotaryEncoder::getEncoder(JSValueConst obj) {
  return (RotaryEncoder*)JS_GetOpaque(obj, js_class_id);
}

JSValue JSRotaryEncoder::js_ctor(JSContext* ctx, JSValueConst new_target,
                                 int argc, JSValueConst* argv) {
  (void)new_target;
  if (argc < 2) {
    return JS_ThrowTypeError(ctx,
      "RotaryEncoder: expected (pinA, pinB)");
  }
  uint32_t pinA = 0, pinB = 0;
  if (JS_ToUint32(ctx, &pinA, argv[0]) ||
      JS_ToUint32(ctx, &pinB, argv[1])) {
    return JS_EXCEPTION;
  }

  // Allocate the native object on the heap. The JS object will
  // own it via the opaque pointer; the finalizer frees it.
  RotaryEncoder* enc = new RotaryEncoder();
  if (!enc->begin((uint8_t)pinA, (uint8_t)pinB)) {
    delete enc;
    return JS_ThrowInternalError(ctx,
      "RotaryEncoder: failed to attach interrupts");
  }

  // Build the JS object. JS_NewObjectClass with our class id
  // gives us a typed opaque slot. We then store the native
  // pointer in that slot via JS_SetOpaque.
  JSValue obj = JS_NewObjectClass(ctx, js_class_id);
  if (JS_IsException(obj)) {
    delete enc;
    return obj;
  }
  JS_SetOpaque(obj, enc);
  return obj;
}

JSValue JSRotaryEncoder::js_position(JSContext* ctx, JSValueConst jsThis,
                                     int argc, JSValueConst* argv) {
  RotaryEncoder* enc = getEncoder(jsThis);
  if (!enc) {
    return JS_ThrowTypeError(ctx, "not a RotaryEncoder");
  }
  if (argc == 0) {
    // Getter: return current position.
    return JS_NewInt32(ctx, enc->position());
  }
  // Setter: position(value). Coerce to int32.
  int32_t value = 0;
  if (JS_ToInt32(ctx, &value, argv[0])) return JS_EXCEPTION;
  enc->position(value);
  return JS_UNDEFINED;
}

void JSRotaryEncoder::js_finalizer(JSRuntime* rt, JSValueConst obj) {
  (void)rt;
  RotaryEncoder* enc = getEncoder(obj);
  if (enc) {
    delete enc;
    JS_SetOpaque(obj, nullptr);
  }
}

void JSRotaryEncoder::js_gc_mark(JSRuntime* rt, JSValueConst obj,
                                 JS_MarkFunc* mark_func) {
  (void)rt; (void)obj; (void)mark_func;
}

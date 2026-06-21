// JSMotorDriver.h
// 
// JavaScript wrapper for MotorDriver. Each JS instance owns a
// pointer to a real MotorDriver (the native object).
//
// Pattern follows JSRotaryEncoder:
// - JS_NewObjectClass allocates a typed opaque slot
// - JS_SetOpaque stores the native pointer
// - JS_GetOpaque retrieves it
// - Finalizer frees the native object on GC

#pragma once
#include "MotorDriver.h"
#include "quickjs.h"
#include <vector>

class JSMotorDriver {
public:
    // Class id for MotorDriver instances
    static JSClassID js_class_id;
    
    // Class definition for JS_NewObjectClass
    static const JSClassDef js_class_def;
    
    // Constructor: `new MotorDriver(forwardPin, reversePin, encPinA?, encPinB?)`
    static JSValue js_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv);

    // Property getters/setters
    static JSValue js_speed(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv);
    static JSValue js_position(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv);

    // Methods
    static JSValue js_moveTo(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv);
    static JSValue js_stop(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv);
    
    // Finalizer: called when the JS object is GC'd
    static void js_finalizer(JSRuntime* rt, JSValueConst obj);
    
    // GC mark: nothing to mark — the native object isn't a JS value
    static void js_gc_mark(JSRuntime* rt, JSValueConst obj, JS_MarkFunc* mark_func);

    // Lifecycle hooks called from ESP32QuickJS::begin/end/loop.
    // init() wires the analog pointer (set by QuickJS.h) and
    // prepares the per-instance state. loop(ctx) pumps any
    // pending Promise resolutions for in-flight moveTo() calls.
    void init();
    void loop(JSContext* ctx);

    // Pointer to the shared LEDC allocator. Set by
    // ESP32QuickJS::begin() right before init() is called.
    JSAnalog* analog = nullptr;

    // Per-instance pending Promise resolvers. Each entry is a
    // (MotorDriver*, resolving_funcs[2]) pair captured when
    // moveTo() is called; loop() drains the queue and resolves
    // the promise once the motor reaches its target.
    struct PendingMove {
        MotorDriver* motor;
        JSValue resolving_funcs[2];
    };
    std::vector<PendingMove> pending_;

    // Singleton pointer — set by init(). Static JS callbacks
    // (js_moveTo) use this to reach the shared pending_ queue
    // because they have no this pointer.
    static JSMotorDriver* instance_;

private:
    // Helper: extract the native pointer from a JS object
    static MotorDriver* getMotor(JSValueConst obj);
};
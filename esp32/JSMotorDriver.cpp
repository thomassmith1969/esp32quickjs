// JSMotorDriver.cpp
//
// JavaScript wrapper for MotorDriver.
// H-bridge design: BOTH forwardPin and reversePin are PWM-capable.
// Direction is implicit in the sign of `speed` (positive = forward, negative = reverse).

#include "JSMotorDriver.h"
#include "QuickJS.h"
#include "quickjs.h"
#include <Arduino.h>

// Lifecycle: called from ESP32QuickJS::begin() after the analog
// pointer has been wired. Nothing to do today — the per-instance
// state is just an empty vector — but keeping the hook means we
// can add setup later without touching QuickJS.h again.
void JSMotorDriver::init() {
    pending_.clear();
    instance_ = this;
}

// Lifecycle: called from ESP32QuickJS::loop(). Drains any pending
// moveTo() promises whose motors have reached their target. We
// resolve on the main task (not in the encoder ISR) so we can
// safely call into QuickJS.
void JSMotorDriver::loop(JSContext* ctx) {
    if (pending_.empty()) return;

    // Walk the queue, resolve anything that's done, drop anything
    // whose motor has been destroyed (finalizer ran).
    size_t i = 0;
    while (i < pending_.size()) {
        PendingMove& pm = pending_[i];
        if (!pm.motor) {
            // Motor was deleted out from under us. Free the
            // resolver values and drop the entry.
            JS_FreeValue(ctx, pm.resolving_funcs[0]);
            JS_FreeValue(ctx, pm.resolving_funcs[1]);
            pending_.erase(pending_.begin() + i);
            continue;
        }

        // The motor stops itself when it reaches the target (see
        // MotorDriver::loop()). When currentSpeed_ drops to 0 in
        // distance mode, the move is complete.
        if (pm.motor->getSpeed() == 0.0f) {
            JSValue resolve = pm.resolving_funcs[0];
            JSValue reject  = pm.resolving_funcs[1];
            JSValue ret = JS_Call(ctx, resolve, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);
            pending_.erase(pending_.begin() + i);
            continue;
        }
        ++i;
    }
}

// Class id for MotorDriver instances
JSClassID JSMotorDriver::js_class_id = 0;

// Singleton pointer — set by init()
JSMotorDriver* JSMotorDriver::instance_ = nullptr;

// Class definition
const JSClassDef JSMotorDriver::js_class_def = {
    "MotorDriver",
    .finalizer = JSMotorDriver::js_finalizer,
    .gc_mark = JSMotorDriver::js_gc_mark,
};

// Helper: extract the native pointer from a JS object
MotorDriver* JSMotorDriver::getMotor(JSValueConst obj) {
    return static_cast<MotorDriver*>(JS_GetOpaque(obj, JSMotorDriver::js_class_id));
}

// Constructor: `new MotorDriver(forwardPin, reversePin, encPinA?, encPinB?)`
JSValue JSMotorDriver::js_ctor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
    if (argc < 2 || argc > 4) {
        return JS_ThrowTypeError(ctx, "MotorDriver requires 2-4 arguments: (forwardPin, reversePin, encPinA?, encPinB?)");
    }

    int forwardPin, reversePin, encPinA = -1, encPinB = -1;
    if (JS_ToUint32(ctx, (uint32_t*)&forwardPin, argv[0]) ||
        JS_ToUint32(ctx, (uint32_t*)&reversePin, argv[1])) {
        return JS_EXCEPTION;
    }
    if (argc >= 3) {
        if (JS_ToUint32(ctx, (uint32_t*)&encPinA, argv[2])) return JS_EXCEPTION;
    }
    if (argc >= 4) {
        if (JS_ToUint32(ctx, (uint32_t*)&encPinB, argv[3])) return JS_EXCEPTION;
    }

    // Allocate the native object
    MotorDriver* motor = new MotorDriver();
    motor->begin(forwardPin, reversePin, encPinA, encPinB);

    // Create the JS wrapper
    JSValue obj = JS_NewObjectClass(ctx, js_class_id);
    if (JS_IsException(obj)) {
        delete motor;
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, motor);

    // If called as a constructor (new MotorDriver(...)), set up the prototype chain
    if (!JS_IsUndefined(new_target)) {
        JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto)) {
            delete motor;
            return JS_EXCEPTION;
        }
        JS_SetPrototype(ctx, obj, proto);
        JS_FreeValue(ctx, proto);
    }

    return obj;
}

// Property: speed (getter/setter)
JSValue JSMotorDriver::js_speed(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
    MotorDriver* motor = JSMotorDriver::getMotor(jsThis);
    if (!motor) {
        return JS_ThrowTypeError(ctx, "speed: not a MotorDriver");
    }

    if (argc == 0) {
        // Getter: return current speed
        return JS_NewFloat64(ctx, motor->getSpeed());
    }

    // Setter: set speed (sign determines direction)
    double speed;
    if (JS_ToFloat64(ctx, &speed, argv[0])) {
        return JS_ThrowTypeError(ctx, "speed: invalid value");
    }
    motor->setSpeed(static_cast<float>(speed));
    return JS_UNDEFINED;
}

// Property: position (getter)
JSValue JSMotorDriver::js_position(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    MotorDriver* motor = JSMotorDriver::getMotor(jsThis);
    if (!motor) {
        return JS_ThrowTypeError(ctx, "position: not a MotorDriver");
    }
    return JS_NewInt32(ctx, motor->getPosition());
}

// Method: moveTo(distance, speed) → Promise<void>
JSValue JSMotorDriver::js_moveTo(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
    MotorDriver* motor = JSMotorDriver::getMotor(jsThis);
    if (!motor) {
        return JS_ThrowTypeError(ctx, "moveTo: not a MotorDriver");
    }

    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "moveTo requires 2 arguments: (distance, speed)");
    }

    int32_t distance;
    double speed;
    if (JS_ToInt32(ctx, &distance, argv[0]) || JS_ToFloat64(ctx, &speed, argv[1])) {
        return JS_ThrowTypeError(ctx, "moveTo: invalid arguments");
    }

    if (!motor->setTargetDistance(distance, static_cast<float>(speed))) {
        return JS_ThrowReferenceError(ctx, "moveTo: encoder not enabled");
    }

    // Return a Promise that resolves when the target is reached.
    // The resolver is parked in pending_; loop() drains it once
    // the motor stops itself.
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return JS_EXCEPTION;
    }
    PendingMove pm;
    pm.motor = motor;
    pm.resolving_funcs[0] = resolving_funcs[0];
    pm.resolving_funcs[1] = resolving_funcs[1];
    instance_->pending_.push_back(std::move(pm));
    return promise;
}

// Method: stop()
JSValue JSMotorDriver::js_stop(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
    (void)argc; (void)argv;
    MotorDriver* motor = JSMotorDriver::getMotor(jsThis);
    if (!motor) {
        return JS_ThrowTypeError(ctx, "stop: not a MotorDriver");
    }
    motor->stop();
    return JS_UNDEFINED;
}

// Finalizer: called when the JS object is GC'd
void JSMotorDriver::js_finalizer(JSRuntime* rt, JSValueConst obj) {
    (void)rt;
    MotorDriver* motor = static_cast<MotorDriver*>(JS_GetOpaque(obj, JSMotorDriver::js_class_id));
    if (motor) {
        motor->end();
        delete motor;
    }
}

// GC mark: nothing to mark
void JSMotorDriver::js_gc_mark(JSRuntime* rt, JSValueConst obj, JS_MarkFunc* mark_func) {
    (void)rt; (void)obj; (void)mark_func;
    // No JS values to mark
}

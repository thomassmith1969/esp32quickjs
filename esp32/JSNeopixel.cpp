// JSNeopixel.cpp
//
// JavaScript wrapper for Neopixel (WS2812) using ESP32 RMT directly.
//
// Architecture:
//   - Synchronous methods (setPixelColor, fill, clear, getPixelColor, numPixels)
//     follow the JSRotaryEncoder pattern — they just modify the RAM buffer.
//   - show() follows the JSMotorDriver async pattern — it submits a WorkItem
//     to the shared JSWorker so the blocking RMT transmission runs
//     on the worker task. The JS side gets a Promise resolved when done.

#include "JSNeopixel.h"
#include "QuickJS.h"
#include "quickjs.h"
#include <Arduino.h>

// ---- Process entry for JSWorker ----
// Called on the JSWorker task. Simply calls the blocking show().
static void processShow(void *entry) {
    auto *np = static_cast<Neopixel *>(entry);
    np->show();
}

// ---- Lifecycle ----

void JSNeopixel::init() {
    pending_.clear();
    instance = this;
}

void JSNeopixel::loop(JSContext *ctx) {
    if (pending_.empty()) return;

    size_t i = 0;
    while (i < pending_.size()) {
        PendingShow &ps = pending_[i];

        // Check if the worker has finished the show() call
        auto *wi = static_cast<JSWorker::WorkItem *>(ps.workItem);
        if (wi->done) {
            JSValue resolve = ps.resolving_funcs[0];
            JSValue reject  = ps.resolving_funcs[1];

            // Resolve the Promise with undefined
            JSValue ret = JS_Call(ctx, resolve, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, ret);

            // Free the resolver values
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);

            // Free the work item
            delete wi; // JSWorker::WorkItem*

            pending_.erase(pending_.begin() + i);
            continue;
        }
        ++i;
    }
}

// ---- QuickJS class plumbing ----

JSClassID JSNeopixel::js_class_id = 0;
JSNeopixel *JSNeopixel::instance = nullptr;

const JSClassDef JSNeopixel::js_class_def = {
    "Neopixel",
    .finalizer = JSNeopixel::js_finalizer,
    .gc_mark = JSNeopixel::js_gc_mark,
};

Neopixel *JSNeopixel::getNeopixel(JSContext *ctx, JSValueConst this_val) {
    return static_cast<Neopixel *>(JS_GetOpaque(this_val, js_class_id));
}

// ---- Finalizer / GC ----

void JSNeopixel::js_finalizer(JSRuntime *rt, JSValue obj) {
    (void)rt;
    auto *np = static_cast<Neopixel *>(JS_GetOpaque(obj, js_class_id));
    if (np) {
        delete np;
    }
    JS_SetOpaque(obj, nullptr);
}

void JSNeopixel::js_gc_mark(JSRuntime *rt, JSValueConst obj, JS_MarkFunc *mark_func) {
}

// ---- Constructor ----
// new Neopixel(pin, numPixels)
JSValue JSNeopixel::js_ctor(JSContext *ctx, JSValueConst new_target,
                            int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "Neopixel requires 2 arguments: (pin, numPixels)");
    }

    uint32_t pin, numPixels;
    if (JS_ToUint32(ctx, &pin, argv[0]) ||
        JS_ToUint32(ctx, &numPixels, argv[1])) {
        return JS_EXCEPTION;
    }

    auto *np = new Neopixel();
    np->begin((uint8_t)pin, (uint16_t)numPixels);

    JSValue obj = JS_NewObjectClass(ctx, js_class_id);
    if (JS_IsException(obj)) {
        delete np;
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, np);

    if (!JS_IsUndefined(new_target)) {
        JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto)) {
            delete np;
            return JS_EXCEPTION;
        }
        JS_SetPrototype(ctx, obj, proto);
        JS_FreeValue(ctx, proto);
    }

    return obj;
}

// ---- Synchronous methods ----

// setPixelColor(index, r, g, b)
JSValue JSNeopixel::js_setPixelColor(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "setPixelColor requires 4 arguments: (index, r, g, b)");
    }
    auto *np = getNeopixel(ctx, this_val);
    if (!np) return JS_ThrowTypeError(ctx, "Neopixel instance is null");

    uint32_t index, r, g, b;
    if (JS_ToUint32(ctx, &index, argv[0]) ||
        JS_ToUint32(ctx, &r, argv[1]) ||
        JS_ToUint32(ctx, &g, argv[2]) ||
        JS_ToUint32(ctx, &b, argv[3])) {
        return JS_EXCEPTION;
    }

    np->setPixelColor((uint16_t)index, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    return JS_UNDEFINED;
}

// getPixelColor(index) → color (as uint32 0x00RRGGBB)
JSValue JSNeopixel::js_getPixelColor(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "getPixelColor requires 1 argument: (index)");
    }
    auto *np = getNeopixel(ctx, this_val);
    if (!np) return JS_ThrowTypeError(ctx, "Neopixel instance is null");

    uint32_t index;
    if (JS_ToUint32(ctx, &index, argv[0])) return JS_EXCEPTION;

    return JS_NewUint32(ctx, np->getPixelColorU32((uint16_t)index));
}

// fill(r, g, b)
JSValue JSNeopixel::js_fill(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "fill requires 3 arguments: (r, g, b)");
    }
    auto *np = getNeopixel(ctx, this_val);
    if (!np) return JS_ThrowTypeError(ctx, "Neopixel instance is null");

    uint32_t r, g, b;
    if (JS_ToUint32(ctx, &r, argv[0]) ||
        JS_ToUint32(ctx, &g, argv[1]) ||
        JS_ToUint32(ctx, &b, argv[2])) {
        return JS_EXCEPTION;
    }

    np->fill((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return JS_UNDEFINED;
}

// clear()
JSValue JSNeopixel::js_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    auto *np = getNeopixel(ctx, this_val);
    if (!np) return JS_ThrowTypeError(ctx, "Neopixel instance is null");

    np->clear();
    return JS_UNDEFINED;
}

// numPixels() → number
JSValue JSNeopixel::js_numPixels(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    auto *np = getNeopixel(ctx, this_val);
    if (!np) return JS_ThrowTypeError(ctx, "Neopixel instance is null");

    return JS_NewUint32(ctx, np->numPixels());
}

// ---- Async show() ----
// show() → Promise<void>
JSValue JSNeopixel::js_show(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    auto *np = getNeopixel(ctx, this_val);
    if (!np) return JS_ThrowTypeError(ctx, "Neopixel instance is null");
    if (!JSWorker::instance) return JS_ThrowTypeError(ctx, "JSWorker not available");

    // Create a Promise
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) return JS_EXCEPTION;

    // Allocate WorkItem for the JSWorker
    // entry is the Neopixel* itself — processShow() casts it back.
    auto *wi = new JSWorker::WorkItem;
    wi->entry = np;
    wi->fn = processShow;
    wi->done = false;

    // Store in pending queue for loop() to drain
    PendingShow ps;
    ps.neopixel = np;
    ps.workItem = wi;
    ps.resolving_funcs[0] = resolving_funcs[0];
    ps.resolving_funcs[1] = resolving_funcs[1];

    instance->pending_.push_back(ps);

    // Submit to JSWorker — this triggers the blocking show() on the worker task
    JSWorker::instance->submit(wi);

    return promise;
}
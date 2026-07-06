#pragma once

#include <map>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../quickjs.h"

// JSStash - A system for safely storing and retrieving JSValues across threads
// Uses stash IDs to reference JSValues without storing them directly in C++ objects
// Values are stored on global.__stashed_objects[uint32 key] — intermediate hidden object.
class JSStash {
private:
  static SemaphoreHandle_t stashMutex_;
  static uint32_t nextId_;

  // Get or create the hidden stash object on globalThis.
  // Returns a handle with refcount owned by caller (must JS_FreeValue).
  static JSValue getStashObj(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue stashObj = JS_GetPropertyStr(ctx, global, "__stashed_objects");
    if (JS_IsUndefined(stashObj)) {
      stashObj = JS_NewObject(ctx);
      // SetProperty steals ref — stashObj now has ref=1 (local owns it).
      // We DupValue BEFORE SetProperty so the local survives.
      JS_SetPropertyStr(ctx, global, "__stashed_objects", JS_DupValue(ctx, stashObj));
    }
    JS_FreeValue(ctx, global);
    return stashObj;
  }

public:
    static void init() {
        stashMutex_ = xSemaphoreCreateMutex();
    }

    static void cleanup() {
        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        xSemaphoreGive(stashMutex_);
        vSemaphoreDelete(stashMutex_);
    }

    // Stash a JSValue on global.__stashed_objects[id] and return the id.
    // JS_SetPropertyUint32 steals the ref of 'value'.
    static uint32_t stash(JSContext* ctx, JSValue value) {
        if (JS_IsUndefined(value) || JS_IsNull(value)) return 0;

        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        uint32_t id = ++nextId_;
        JSValue stashObj = getStashObj(ctx);
        JS_SetPropertyUint32(ctx, stashObj, id, value);  // steals ref
        JS_FreeValue(ctx, stashObj);
        xSemaphoreGive(stashMutex_);
        return id;
    }

    // Get a stashed JSValue (caller must JS_FreeValue when done).
    static JSValue get(JSContext* ctx, uint32_t id) {
        if (id == 0) return JS_UNDEFINED;

        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        JSValue stashObj = getStashObj(ctx);
        JSValue value = JS_GetPropertyUint32(ctx, stashObj, id);
        JS_FreeValue(ctx, stashObj);
        xSemaphoreGive(stashMutex_);
        return value;
    }

    // Release (delete) a stashed value from global.__stashed_objects.
    static void release(JSContext* ctx, uint32_t id) {
        if (id == 0) return;

        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        JSValue stashObj = getStashObj(ctx);
        if (!JS_IsUndefined(stashObj)) {
            JSAtom atom = JS_NewAtomUInt32(ctx, id);
            JS_DeleteProperty(ctx, stashObj, atom, 0);
            JS_FreeAtom(ctx, atom);
        }
        JS_FreeValue(ctx, stashObj);
        xSemaphoreGive(stashMutex_);
    }
};

inline SemaphoreHandle_t JSStash::stashMutex_ = nullptr;
inline uint32_t JSStash::nextId_ = 0;


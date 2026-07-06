#pragma once

#include <map>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../quickjs.h"

// JSStash - A system for safely storing and retrieving JSValues across threads
// Uses stash IDs to reference JSValues without storing them directly in C++ objects
class JSStash {
private:
  static SemaphoreHandle_t stashMutex_;
     static uint32_t nextId_;
    
public:
    // Initialize the stash system
    static void init() {
        stashMutex_ = xSemaphoreCreateMutex();
        
    }
    
    // Clean up the stash system
    static void cleanup() {
        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        // actually figure out & do cleanup
        // for (auto& entry : stashMap_) {
        //     JS_FreeValue(nullptr, entry.second);
        // }
        // stashMap_.clear();
        xSemaphoreGive(stashMutex_);
        vSemaphoreDelete(stashMutex_);
    }
    
    // Stash a JSValue and return a stash ID
    static uint32_t stash(JSContext* ctx, JSValue value) {
        if (JS_IsUndefined(value) || JS_IsNull(value)) {
            return 0;
        }
        
        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        uint32_t id = ++nextId_;
        // if there is not already a hidden global value for stashed objects, create it
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue stashObj = JS_GetPropertyStr(ctx, global, "__stashed_objects");
        if (JS_IsUndefined(stashObj)) {
            stashObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, global, "__stashed_objects", stashObj);
        }

        // push the passed value into the hidden stash object
        JS_SetProperty(ctx, stashObj, id, value);     
        JS_FreeValue(ctx, value); // Free the original value after stashing it
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, stashObj);

        xSemaphoreGive(stashMutex_);
        
        return id;
    }
    
    // Get a JSValue from the stash
    static JSValue get(JSContext* ctx, uint32_t id) {
        

        // get the global object and the hidden stash object
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue stashObj = JS_GetPropertyStr(ctx, global, "__stashed_objects");
        if (JS_IsUndefined(stashObj)) {
            stashObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, global, "__stashed_objects", stashObj);
        }
        //return the value from the hidden stash object if it exists
        JSValue value = JS_GetProperty(ctx, stashObj, id);
        JS_FreeValue(ctx, stashObj);
        JS_FreeValue(ctx, global);  
        xSemaphoreGive(stashMutex_);
        
        return value;
    }
    
    // Release a stashed value
    static void release(JSContext* ctx, uint32_t id) {
        if (id == 0) return;
        
        xSemaphoreTake(stashMutex_, portMAX_DELAY);
        // remove the value from the hidden stash object
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue stashObj = JS_GetPropertyStr(ctx, global, "__stashed_objects");
        if (!JS_IsUndefined(stashObj)) {
        JS_DeleteProperty(ctx, stashObj, id, 0);
        }
        JS_FreeValue(ctx, stashObj);
        JS_FreeValue(ctx, global);  
        
        xSemaphoreGive(stashMutex_);
    }
};

// Initialize static members
inline SemaphoreHandle_t JSStash::stashMutex_ = nullptr;
inline uint32_t JSStash::nextId_ = 0;


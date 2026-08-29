#pragma once

#include <functional>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../quickjs.h"
#include "JSStash.h"

// Resolve function type: takes JSContext*, returns JSValue to resolve with
using JSPromiseResolveFunc = std::function<JSValue(JSContext*)>;

namespace {
    struct PendingOp {
        uint32_t id;
        bool isReject;
        JSPromiseResolveFunc resolveFunc;
        std::string errorMsg;
    };

    SemaphoreHandle_t g_promiseMutex = nullptr;
    std::vector<PendingOp> g_pendingOps;
    JSContext* g_promiseCtx = nullptr;

    JSValue processJobFunc(JSContext* ctx, int argc, JSValueConst* argv) {
        (void)argc; (void)argv;
        if (!g_promiseMutex) return JS_UNDEFINED;

        xSemaphoreTake(g_promiseMutex, portMAX_DELAY);
        std::vector<PendingOp> ops = std::move(g_pendingOps);
        g_pendingOps.clear();
        xSemaphoreGive(g_promiseMutex);

        for (auto& op : ops) {
            JSValue arr = JSStash_get(ctx, op.id);
            if (JS_IsUndefined(arr)) continue;

            JSValue resolve = JS_GetPropertyUint32(ctx, arr, 1);
            JSValue reject = JS_GetPropertyUint32(ctx, arr, 2);

            if (op.isReject) {
                JSValue err = JS_NewError(ctx);
                JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, op.errorMsg.c_str()));
                JS_Call(ctx, reject, JS_UNDEFINED, 1, &err);
                JS_FreeValue(ctx, err);
            } else {
                JSValue val = op.resolveFunc ? op.resolveFunc(ctx) : JS_UNDEFINED;
                JS_Call(ctx, resolve, JS_UNDEFINED, 1, &val);
                JS_FreeValue(ctx, val);
            }

            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reject);
            JS_FreeValue(ctx, arr);
            JSStash_release(ctx, op.id);
        }
        return JS_UNDEFINED;
    }
}

// 1. createPromise - creates a new Promise, stashes it, returns ID
static uint32_t createPromise(JSContext* ctx) {
    if (!g_promiseMutex) {
        g_promiseMutex = xSemaphoreCreateMutex();
        g_promiseCtx = ctx;
    }

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);

    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, promise);
    JS_SetPropertyUint32(ctx, arr, 1, resolving_funcs[0]);
    JS_SetPropertyUint32(ctx, arr, 2, resolving_funcs[1]);

    return JSStash_stash(ctx, arr);
}

// 2. getPromise - gets the stashed promise by ID
static JSValue getPromise(JSContext* ctx, uint32_t id) {
    if (id == 0) return JS_UNDEFINED;
    JSValue arr = JSStash_get(ctx, id);
    if (JS_IsUndefined(arr)) return JS_UNDEFINED;
    JSValue promise = JS_GetPropertyUint32(ctx, arr, 0);
    JS_FreeValue(ctx, arr);
    return promise;
}

// 3. queueResolve - queues resolution from any thread
static void queueResolve(uint32_t id, JSPromiseResolveFunc resolveFunc) {
    if (id == 0 || !g_promiseMutex || !g_promiseCtx) return;

    xSemaphoreTake(g_promiseMutex, portMAX_DELAY);
    g_pendingOps.push_back({id, false, std::move(resolveFunc), ""});
    xSemaphoreGive(g_promiseMutex);

    JS_EnqueueJob(g_promiseCtx, processJobFunc, 0, nullptr);
}

// 4. queueReject - queues rejection from any thread
static void queueReject(uint32_t id, const char* msg) {
    if (id == 0 || !g_promiseMutex || !g_promiseCtx) return;

    xSemaphoreTake(g_promiseMutex, portMAX_DELAY);
    g_pendingOps.push_back({id, true, nullptr, msg ? msg : ""});
    xSemaphoreGive(g_promiseMutex);

    JS_EnqueueJob(g_promiseCtx, processJobFunc, 0, nullptr);
}

#pragma once
#include "../quickjs.h"
#include <ESPAsyncWebServer.h>
#include "JSStash.h"
#include "JSQueue.h"
#include <unordered_map>

// Map from handlerId (returned by app.get/post/put/delete/patch/all) to the
// AsyncCallbackWebHandler* that server->on() produced, so app.remove(handlerId)
// can later call server->removeHandler(handler). Populated in
// makeRouteHandler, consumed in makeRemoveHandler.
struct RouteHandlerEntry {
    AsyncWebHandler *handler = nullptr;
};
static std::unordered_map<uint32_t, RouteHandlerEntry> g_routeHandlers;
static SemaphoreHandle_t g_routeHandlersMutex = xSemaphoreCreateMutex();

// Forward declare for AsyncWebRequestMethod
namespace AsyncWebRequestMethod {
    enum AsyncWebRequestMethodType : uint32_t;
}
using WebRequestMethod = AsyncWebRequestMethod::AsyncWebRequestMethodType;

// Cached atoms for the express constructor pattern.  Allocated lazily on first
// registration and reused by every instance.
static JSAtom serverInstanceAtom = JS_ATOM_NULL;
static JSAtom requestAtom = JS_ATOM_NULL;
static JSAtom responseAtom = JS_ATOM_NULL;
static JSAtom listenAtom = JS_ATOM_NULL;
static JSAtom getAtom = JS_ATOM_NULL;
static JSAtom postAtom = JS_ATOM_NULL;
static JSAtom putAtom = JS_ATOM_NULL;
static JSAtom deleteAtom = JS_ATOM_NULL;
static JSAtom patchAtom = JS_ATOM_NULL;
static JSAtom allAtom = JS_ATOM_NULL;
static JSAtom useAtom = JS_ATOM_NULL;
static JSAtom closeAtom = JS_ATOM_NULL;
static JSAtom removeAtom = JS_ATOM_NULL;
static JSAtom serveStaticAtom = JS_ATOM_NULL;
static JSAtom serveSDAtom = JS_ATOM_NULL;

// Monotonic counter for static-file route ids (returned from app.serveStatic
// and app.serveSD so the caller can pass them to app.remove()). Static routes
// have no JS handler so JSStash_stash is not used; this counter is the only
// thing that gives them a unique id.
static uint32_t g_staticRouteIdCounter = 0;


// Wrap a raw ESPAsyncWebServer request pointer in a JS object whose only property is the
// int64-encoded pointer under requestAtom.  Built on the JS thread so the
// property is a proper JSValue. Also adds methods like send(), redirect(), etc.
static JSValue wrapRequest(JSContext *ctx, AsyncWebServerRequest *req) {
    JSValue jsReq = JS_NewObject(ctx);
    JS_DefinePropertyValue(ctx, jsReq, requestAtom,
        JS_NewInt64(ctx, (int64_t)req), JS_PROP_CONFIGURABLE);
    
    // Add send(code, contentType?, content?) method
    JSValue sendFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid request object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *req = reinterpret_cast<AsyncWebServerRequest*>(reqPtr);
        
        int code = 200;
        const char *contentType = "text/plain";
        const char *content = "";
        
        if (argc >= 1) JS_ToInt32(ctx2, &code, argv[0]);
        if (argc >= 2) contentType = JS_ToCString(ctx2, argv[1]);
        if (argc >= 3) content = JS_ToCString(ctx2, argv[2]);
        
        req->send(code, contentType, content);
        
        if (argc >= 2) JS_FreeCString(ctx2, contentType);
        if (argc >= 3) JS_FreeCString(ctx2, content);
        
        return JS_UNDEFINED;
    }, "send", 3);
    JS_DefinePropertyValueStr(ctx, jsReq, "send", sendFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    
    // Add redirect(url, code?) method
    JSValue redirectFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid request object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *req = reinterpret_cast<AsyncWebServerRequest*>(reqPtr);
        
        if (argc < 1) return JS_ThrowTypeError(ctx2, "redirect requires URL");
        
        const char *url = JS_ToCString(ctx2, argv[0]);
        int code = 302;
        if (argc >= 2) JS_ToInt32(ctx2, &code, argv[1]);
        
        req->redirect(url, code);
        JS_FreeCString(ctx2, url);
        
        return JS_UNDEFINED;
    }, "redirect", 2);
    JS_DefinePropertyValueStr(ctx, jsReq, "redirect", redirectFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    
    // Add url() getter
    JSValue urlFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid request object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *req = reinterpret_cast<AsyncWebServerRequest*>(reqPtr);
        return JS_NewString(ctx2, req->url().c_str());
    }, "url", 0);
    JS_DefinePropertyValueStr(ctx, jsReq, "url", urlFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    
    // Add method() getter
    JSValue methodFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid request object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *req = reinterpret_cast<AsyncWebServerRequest*>(reqPtr);
        return JS_NewString(ctx2, req->methodToString());
    }, "method", 0);
    JS_DefinePropertyValueStr(ctx, jsReq, "method", methodFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    
    // Add contentType() getter
    JSValue contentTypeFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid request object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *req = reinterpret_cast<AsyncWebServerRequest*>(reqPtr);
        return JS_NewString(ctx2, req->contentType().c_str());
    }, "contentType", 0);
    JS_DefinePropertyValueStr(ctx, jsReq, "contentType", contentTypeFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    
    // Add body() getter - returns body as string
    JSValue bodyFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid request object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *req = reinterpret_cast<AsyncWebServerRequest*>(reqPtr);
        // Note: body is only available after the request is fully received
        // For async handlers, we need to use onBody callback
        return JS_NewString(ctx2, ""); // Placeholder
    }, "body", 0);
    JS_DefinePropertyValueStr(ctx, jsReq, "body", bodyFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    
    return jsReq;
}

// Same for ESPAsyncWebServer response - wraps BOTH the (possibly-null)
// response pointer and the request pointer, so res.send(...) can dispatch
// via req->send() (the library's response writer lives on the request, not
// on the response). Express-style handlers reach for (req, res) => res.send(...);
// so we make sure res.send exists and does the right thing.
static JSValue wrapResponse(JSContext *ctx, AsyncWebServerRequest *req, AsyncWebServerResponse *res) {
    JSValue jsRes = JS_NewObject(ctx);
    // Stash the request pointer under the same atom jsReq uses, so res.send
    // can find it without needing a second int64 atom.
    JS_DefinePropertyValue(ctx, jsRes, requestAtom,
        JS_NewInt64(ctx, (int64_t)req), JS_PROP_CONFIGURABLE);
    // Also keep the response pointer (may be null if the request is paused
    // and the user does res.send(...)). res.redirect() will need it.
    JS_DefinePropertyValue(ctx, jsRes, responseAtom,
        JS_NewInt64(ctx, (int64_t)res), JS_PROP_CONFIGURABLE);

    // res.send(code, contentType?, content?) - dispatches via req->send(...).
    JSValue sendFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid response object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *r = reinterpret_cast<AsyncWebServerRequest *>(reqPtr);

        int code = 200;
        const char *contentType = "text/plain";
        const char *content = "";
        if (argc >= 1) JS_ToInt32(ctx2, &code, argv[0]);
        if (argc >= 2) contentType = JS_ToCString(ctx2, argv[1]);
        if (argc >= 3) content = JS_ToCString(ctx2, argv[2]);

        r->send(code, contentType, content);

        if (argc >= 2) JS_FreeCString(ctx2, contentType);
        if (argc >= 3) JS_FreeCString(ctx2, content);
        return JS_UNDEFINED;
    }, "send", 3);
    JS_DefinePropertyValueStr(ctx, jsRes, "send", sendFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

    // res.redirect(url, code?) - dispatches via req->redirect(...).
    JSValue redirectFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
        int64_t reqPtr = 0;
        JSValue reqVal = JS_GetProperty(ctx2, this_obj, requestAtom);
        if (JS_ToInt64(ctx2, &reqPtr, reqVal) != 0 || reqPtr == 0) {
            JS_FreeValue(ctx2, reqVal);
            return JS_ThrowTypeError(ctx2, "Invalid response object");
        }
        JS_FreeValue(ctx2, reqVal);
        AsyncWebServerRequest *r = reinterpret_cast<AsyncWebServerRequest *>(reqPtr);

        if (argc < 1) return JS_ThrowTypeError(ctx2, "redirect requires URL");
        const char *url = JS_ToCString(ctx2, argv[0]);
        int code = 302;
        if (argc >= 2) JS_ToInt32(ctx2, &code, argv[1]);
        r->redirect(url, code);
        JS_FreeCString(ctx2, url);
        return JS_UNDEFINED;
    }, "redirect", 2);
    JS_DefinePropertyValueStr(ctx, jsRes, "redirect", redirectFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

    return jsRes;
}

static AsyncWebServer* getServer(JSContext *ctx, JSValue serverObject){
    // Look up the cached _serverInstance atom.  This is the atom-based getter.
    JSValue serverValue = JS_GetProperty(ctx, serverObject, serverInstanceAtom);
    if (JS_IsUndefined(serverValue)) {
        return nullptr;
    }
    int64_t serverPtr = 0;
    if (JS_ToInt64(ctx, &serverPtr, serverValue) != 0) {
        JS_FreeValue(ctx, serverValue);
        return nullptr;
    }
    JS_FreeValue(ctx, serverValue);
    return reinterpret_cast<AsyncWebServer*>(serverPtr);
}

// Helper: register a route for a given HTTP method
static JSValue makeRouteHandler(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv, WebRequestMethodComposite method) {
    AsyncWebServer *server = getServer(ctx, this_obj);
    if (!server) {
        return JS_ThrowTypeError(ctx, "Server instance not found");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "Expected 2 arguments: path and handler");
    }
    // get the path string from argv[0]
    size_t pathLen;
    const char *path = JS_ToCStringLen(ctx, &pathLen, argv[0]);
    if (!path) {
        return JS_ThrowTypeError(ctx, "Invalid path argument");
    }
    // get the handler function from argv[1]
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Handler must be a function");
    }
    // Stash the handler. JSStash_stash steals the caller's ref (the
    // __stashed_objects[id] property is set without DupValue — see
    // JSStash.h). To avoid the property aliasing the JS engine's argv
    // ref (which would dangle when the C function returns and the
    // engine frees argv), DupValue before passing; JSStash_stash steals
    // our dup'd ref, leaving the engine's argv ref untouched.
    uint32_t handlerId = JSStash_stash(ctx, JS_DupValue(ctx, argv[1]));

    // Register route on the AsyncWebServer. Note: ESPAsyncWebServer copies the
    // URI into an AsyncURIMatcher (String), so we can free the path immediately
    // after the call. The route lambda does NOT capture `path` because doing so
    // would dangle once JS_FreeCString() runs; the lambda uses req->url() for
    // logging instead. (Previously the lambda captured `path` and was a
    // use-after-free — caused heap corruption under multi_heap poisoning.)
    //
    // We capture the AsyncCallbackWebHandler& returned by server->on() so that
    // app.remove(handlerId) can later find the handler via the
    // g_routeHandlers map and call server->removeHandler().
    AsyncCallbackWebHandler &cbHandler = server->on(path, method, [handlerId](AsyncWebServerRequest *req) {
        Serial.printf("[WebServer] Request received: %s %s (handlerId=%u)\n",
                      req->methodToString(), req->url().c_str(), handlerId);
        // IMPORTANT: this lambda runs on AsyncTCP's task. We must not touch
        // JSValues or call JS_Call from here — only enqueue work for the JS
        // thread (see esp32/AGENTS.md "Never call JS_Call from a foreign thread").
        //
        // Also: req is owned by a shared_ptr inside the library. As soon as
        // _onData / _onAck / _onPoll returns, that local shared_ptr drops and
        // req may be destroyed on the AsyncTCP task — long before the JS thread
        // dequeues our PendingExecution. Capturing a raw `req` pointer here
        // produced a use-after-free on the JS thread (manifesting as a free of
        // bogus memory in ~AsyncWebHeader during the eventual teardown).
        // Fix: capture a shared_ptr to the request so it lives until the JS-
        // thread lambda finishes.
        std::shared_ptr<AsyncWebServerRequest> reqShared = req->shared_from_this();
        // Pause the request: the library calls _send() immediately after
        // _runMiddlewareChain() returns, but our JS handler hasn't run yet
        // (it will run later on the JS thread via pushPendingOp). Pausing
        // makes _send() short-circuit (the if (!sent && !paused) guard)
        // and prevents the library from sending its 501 'Handler did not
        // handle the request' fallback. When our JS-thread lambda later
        // calls req.send(...), send() clears _paused and invokes _send()
        // itself (see AsyncWebServerRequest::send(AsyncWebServerResponse*)),
        // which is what actually flushes the response to the wire.
        req->pause();
        pushPendingOp(PendingExecution{[handlerId, reqShared](JSContext* ctx) -> void* {
            AsyncWebServerRequest *req = reqShared.get();
            JSValue handler = JSStash_get(ctx, handlerId);
            if (!JS_IsUndefined(handler) && JS_IsFunction(ctx, handler)) {
                JSValue jsReq = wrapRequest(ctx, req);
                // For response, we need to create a wrapper that can be used to send response
                AsyncWebServerResponse *res = req->getResponse();
                JSValue jsRes = wrapResponse(ctx, req, res);
                JSValue argv[2] = { jsReq, jsRes };
                JS_Call(ctx, handler, JS_UNDEFINED, 2, argv);
                JS_FreeValue(ctx, jsReq);
                JS_FreeValue(ctx, jsRes);
            }
            JS_FreeValue(ctx, handler);
            // reqShared is released when this lambda returns.
            return nullptr;
        }});
    });
    // Record the handler pointer so app.remove(handlerId) can find it.
    if (g_routeHandlersMutex) xSemaphoreTake(g_routeHandlersMutex, portMAX_DELAY);
    RouteHandlerEntry entry;
    entry.handler = &cbHandler;
    g_routeHandlers[handlerId] = entry;
    if (g_routeHandlersMutex) xSemaphoreGive(g_routeHandlersMutex);
    // server->on() copied the URI into its AsyncURIMatcher; safe to free now.
    JS_FreeCString(ctx, path);

    return JS_NewInt32(ctx, handlerId);
}

// Helper: register middleware (use)
static JSValue makeUseHandler(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv) {
    AsyncWebServer *server = getServer(ctx, this_obj);
    if (!server) {
        return JS_ThrowTypeError(ctx, "Server instance not found");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Expected at least 1 argument: handler (path optional)");
    }
    
    // Optional path argument
    const char* path = "/";
    int argIndex = 0;
    if (argc >= 2 && JS_IsString(argv[0])) {
        size_t pathLen;
        path = JS_ToCStringLen(ctx, &pathLen, argv[0]);
        argIndex = 1;
    }
    
    if (!JS_IsFunction(ctx, argv[argIndex])) {
        if (argIndex == 0) JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "Handler must be a function");
    }

    // Stash the handler. JSStash_stash steals the caller's ref; pass a
    // DupValue'd copy so the property owns its own ref rather than
    // aliasing the JS engine's argv ref (which would dangle when the
    // engine frees argv after we return). See JSStash.h.
    uint32_t handlerId = JSStash_stash(ctx, JS_DupValue(ctx, argv[argIndex]));
    if (argIndex == 0) JS_FreeCString(ctx, path);
    
    // Add middleware - runs for all methods on this path
    // Use catchAllHandler() to add middleware that runs for all requests.
    //
    // Note: the middleware chain (AsyncMiddlewareChain::_runChain) is built to
    // run synchronously. Each `next` callable captures references to local
    // state in _runChain that disappear the moment _runChain returns. So we
    // MUST call next() on the AsyncTCP task, not defer it to the JS thread.
    // Same thread-safety rule as the route lambda: do JS work on the JS thread
    // (via pushPendingOp), but keep the synchronous chain moving.
    auto& catchAll = server->catchAllHandler();
    catchAll.addMiddleware([handlerId](AsyncWebServerRequest *req, ArMiddlewareNext next) {
        // Hold a shared_ptr until both the JS-thread dispatch and the chain
        // continuation have completed.
        std::shared_ptr<AsyncWebServerRequest> reqShared = req->shared_from_this();
        // Dispatch the JS handler on the JS thread.
        pushPendingOp(PendingExecution{[handlerId, reqShared](JSContext* ctx) -> void* {
            AsyncWebServerRequest *req = reqShared.get();
            JSValue handler = JSStash_get(ctx, handlerId);
            if (!JS_IsUndefined(handler) && JS_IsFunction(ctx, handler)) {
                JSValue jsReq = wrapRequest(ctx, req);
                AsyncWebServerResponse *res = req->getResponse();
                JSValue jsRes = wrapResponse(ctx, req, res);
                JSValue argv[2] = { jsReq, jsRes };
                JS_Call(ctx, handler, JS_UNDEFINED, 2, argv);
                JS_FreeValue(ctx, jsReq);
                JS_FreeValue(ctx, jsRes);
            }
            JS_FreeValue(ctx, handler);
            return nullptr;
        }});
        // Continue the middleware chain synchronously on AsyncTCP. This will
        // eventually reach the route handler (which itself defers JS work via
        // pushPendingOp — see makeRouteHandler).
        next();
        // reqShared released when this lambda returns; by that point both the
        // queued JS-thread op and the chain continuation are in flight.
    });
    
    return JS_UNDEFINED;
}

// Helper: close/destroy the server
static JSValue makeCloseHandler(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv) {
    AsyncWebServer *server = getServer(ctx, this_obj);
    if (!server) {
        return JS_ThrowTypeError(ctx, "Server instance not found");
    }
    server->end();  // Safe to call multiple times
    // Note: We don't delete the server here because the JS object still holds the pointer.
    // The server will be deleted when the JS object is GC'd (via a finalizer if needed).
    return JS_UNDEFINED;
}

// Helper: serve static files from a filesystem. uri is the URI prefix
// to match (e.g. "/"); fs is the filesystem instance to read from; path is
// the directory inside that filesystem (e.g. "/www"). Returns the route id
// (same id-space as app.get/post/etc.) so the caller can later app.remove(id).
// Pass nullptr for cache_control or pass a string like "max-age=3600".
static JSValue makeServeStatic(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv, fs::FS &fs) {
    AsyncWebServer *server = getServer(ctx, this_obj);
    if (!server) {
        return JS_ThrowTypeError(ctx, "Server instance not found");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "Expected 2 arguments: uri, path");
    }
    size_t uriLen, pathLen;
    const char *uri = JS_ToCStringLen(ctx, &uriLen, argv[0]);
    if (!uri) {
        return JS_ThrowTypeError(ctx, "Invalid uri argument");
    }
    const char *path = JS_ToCStringLen(ctx, &pathLen, argv[1]);
    if (!path) {
        JS_FreeCString(ctx, uri);
        return JS_ThrowTypeError(ctx, "Invalid path argument");
    }
    const char *cache_control = nullptr;
    char *cacheStr = nullptr;
    if (argc >= 3 && JS_IsString(argv[2])) {
        cacheStr = (char *)JS_ToCString(ctx, argv[2]);
        cache_control = cacheStr;
    }

    AsyncStaticWebHandler &staticHandler = server->serveStatic(uri, fs, path, cache_control);

    if (cacheStr) JS_FreeCString(ctx, cacheStr);
    JS_FreeCString(ctx, uri);
    JS_FreeCString(ctx, path);

    // Assign a fresh id so app.remove(id) works.
    uint32_t routeId = ++g_staticRouteIdCounter;
    RouteHandlerEntry entry;
    entry.handler = &staticHandler;  // AsyncStaticWebHandler -> AsyncWebHandler*
    g_routeHandlers[routeId] = entry;
    return JS_NewInt32(ctx, routeId);
}

// Helper: remove a route by its handlerId (the value returned from app.get,
// app.post, etc.). Returns true if a handler was removed, false otherwise.
static JSValue makeRemoveHandler(JSContext *ctx, JSValueConst this_obj, int argc, JSValueConst *argv) {
    AsyncWebServer *server = getServer(ctx, this_obj);
    if (!server) {
        return JS_ThrowTypeError(ctx, "Server instance not found");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Expected 1 argument: routeId");
    }
    int64_t routeId = 0;
    if (JS_ToInt64(ctx, &routeId, argv[0]) != 0) {
        return JS_ThrowTypeError(ctx, "routeId must be an integer");
    }
    if (routeId <= 0) {
        return JS_NewBool(ctx, false);
    }

    AsyncWebHandler *handler = nullptr;
    if (g_routeHandlersMutex) xSemaphoreTake(g_routeHandlersMutex, portMAX_DELAY);
    auto it = g_routeHandlers.find((uint32_t)routeId);
    if (it != g_routeHandlers.end()) {
        handler = it->second.handler;
        g_routeHandlers.erase(it);
    }
    if (g_routeHandlersMutex) xSemaphoreGive(g_routeHandlersMutex);

    if (!handler) {
        return JS_NewBool(ctx, false);
    }
    bool removed = server->removeHandler(handler);
    return JS_NewBool(ctx, removed);
}

inline void initWebServer(JSContext *ctx, JSValue globalObj){
    // Allocate atoms once for the lifetime of the runtime.
    serverInstanceAtom = JS_NewAtom(ctx, "_serverInstance");
    requestAtom = JS_NewAtom(ctx, "_req");
    responseAtom = JS_NewAtom(ctx, "_res");
    listenAtom = JS_NewAtom(ctx, "listen");
    getAtom = JS_NewAtom(ctx, "get");
    postAtom = JS_NewAtom(ctx, "post");
    putAtom = JS_NewAtom(ctx, "put");
    deleteAtom = JS_NewAtom(ctx, "delete");
    patchAtom = JS_NewAtom(ctx, "patch");
    allAtom = JS_NewAtom(ctx, "all");
    useAtom = JS_NewAtom(ctx, "use");
    closeAtom = JS_NewAtom(ctx, "close");
    removeAtom = JS_NewAtom(ctx, "remove");
    serveStaticAtom = JS_NewAtom(ctx, "serveStatic");
    serveSDAtom = JS_NewAtom(ctx, "serveSD");
    // Register "express" as a constructor: let myServer = new express();
    JSValue expressConstructor = JS_NewCFunction2(ctx, [](JSContext *ctx, JSValueConst new_target,
              int argc, JSValueConst *argv) {
                // Create a new object to hold the server instance
                JSValue new_obj = JS_NewObject(ctx);
                
                // Create ESPAsyncWebServer and store its pointer as a property on the JS object.
                // On 32-bit (ESP32) use JS_NewInt64 — the 32-bit pointer fits in int32 and round-trips via JS_ToInt64.
                // Constructor requires a port - default to 80
                int defaultPort = 80;
                if (argc > 0) {
                    JS_ToInt32(ctx, &defaultPort, argv[0]);
                }
                AsyncWebServer *server = new AsyncWebServer(defaultPort);
                JS_DefinePropertyValue(ctx, new_obj, serverInstanceAtom,
                    JS_NewInt64(ctx, (int64_t)server), JS_PROP_CONFIGURABLE);

                // listen() - start the server (port already set in constructor)
                JSValue listenFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) {
                    AsyncWebServer *server = getServer(ctx2, this_obj);
                    if (!server) {
                        return JS_ThrowTypeError(ctx2, "Server instance not found");
                    }
                    if(!server->state()){
                        Serial.println("Starting server");
                        server->begin();
                    }
                    return JS_UNDEFINED;
                }, "listen", 0);
                JS_DefinePropertyValue(ctx, new_obj, listenAtom, listenFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // close() - stop the server
                JSValue closeFunc = JS_NewCFunction(ctx, makeCloseHandler, "close", 0);
                JS_DefinePropertyValue(ctx, new_obj, closeAtom, closeFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // remove(routeId) - deregister a route by the id returned from
                // app.get/post/put/delete/patch/all/serveStatic/serveSD. Returns
                // true if a handler was removed, false if no handler was
                // registered for that id.
                JSValue removeFunc = JS_NewCFunction(ctx, makeRemoveHandler, "remove", 1);
                JS_DefinePropertyValue(ctx, new_obj, removeAtom, removeFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // serveStatic(uri, path, [cache_control]) - serve files from
                // LittleFS at the given URI prefix. Files under <path> on the
                // filesystem are mapped to <uri>/* in the URL space. Returns
                // a route id (use app.remove(id) to deregister).
                JSValue serveStaticFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeServeStatic(ctx2, this_obj, argc, argv, LittleFS);
                }, "serveStatic", 2);
                JS_DefinePropertyValue(ctx, new_obj, serveStaticAtom, serveStaticFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // serveSD(uri, path, [cache_control]) - serve files from the
                // SD card at the given URI prefix. Same signature/return as
                // serveStatic.
                JSValue serveSDFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeServeStatic(ctx2, this_obj, argc, argv, SD);
                }, "serveSD", 2);
                JS_DefinePropertyValue(ctx, new_obj, serveSDAtom, serveSDFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // get(path, handler)
                JSValue getFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeRouteHandler(ctx2, this_obj, argc, argv, AsyncWebRequestMethod::HTTP_GET);
                }, "get", 2);
                JS_DefinePropertyValue(ctx, new_obj, getAtom, getFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // post(path, handler)
                JSValue postFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeRouteHandler(ctx2, this_obj, argc, argv, AsyncWebRequestMethod::HTTP_POST);
                }, "post", 2);
                JS_DefinePropertyValue(ctx, new_obj, postAtom, postFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // put(path, handler)
                JSValue putFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeRouteHandler(ctx2, this_obj, argc, argv, AsyncWebRequestMethod::HTTP_PUT);
                }, "put", 2);
                JS_DefinePropertyValue(ctx, new_obj, putAtom, putFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // delete(path, handler) - "delete" is a reserved word, so use "del" in JS
                JSValue deleteFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeRouteHandler(ctx2, this_obj, argc, argv, AsyncWebRequestMethod::HTTP_DELETE);
                }, "delete", 2);
                JS_DefinePropertyValue(ctx, new_obj, deleteAtom, deleteFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // patch(path, handler)
                JSValue patchFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeRouteHandler(ctx2, this_obj, argc, argv, AsyncWebRequestMethod::HTTP_PATCH);
                }, "patch", 2);
                JS_DefinePropertyValue(ctx, new_obj, patchAtom, patchFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // all(path, handler) - matches any HTTP method
                JSValue allFunc = JS_NewCFunction(ctx, [](JSContext *ctx2, JSValueConst this_obj, int argc, JSValueConst *argv) -> JSValue {
                    return makeRouteHandler(ctx2, this_obj, argc, argv, AsyncWebRequestMethod::HTTP_ALL);
                }, "all", 2);
                JS_DefinePropertyValue(ctx, new_obj, allAtom, allFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                // use([path,] handler) - middleware
                JSValue useFunc = JS_NewCFunction(ctx, makeUseHandler, "use", 1);
                JS_DefinePropertyValue(ctx, new_obj, useAtom, useFunc, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);

                return new_obj;
    }, "express", 0, JS_CFUNC_constructor, 0);
    
    JS_SetPropertyStr(ctx, globalObj, "express", expressConstructor);
    Serial.println("express defined in JS global object");
}

// Process pending operations queued from async HTTP callbacks.
// Call this from the main loop (via ESP32QuickJS::loop()).
inline void loopWebServer(JSContext *ctx) {
    runPendingOps(ctx);
}

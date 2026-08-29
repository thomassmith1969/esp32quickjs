# esp32quickjs — Hard Rules (project conventions)

These are NON-NEGOTIABLE rules for any C++ work in `esp32/`. Breaking
any of these has crashed the runtime or produced subtle lifetime bugs.


## 1. Never store `JSContext*`

- `JSContext*` is a per-runtime handle that can be torn down.
- Storing it on a struct that outlives the function call (e.g. as a
  member of `JSHttpRequest`, as a member of a class instance that
  persists across JS calls) WILL crash when the runtime is freed.
- Allowed: `JSContext*` as a function parameter, or as a local.
- The ONLY safe way to call `JS_Call` from a foreign thread is via
  `JS_EnqueueJob(ctx, lambda, argc, argv)`. QuickJS invokes the
  lambda on the JS thread with a fresh JS-thread ctx as its first
  parameter — that ctx is the ONLY ctx the lambda may use.

## 2. Never call `JS_Call` from a foreign thread

- AsyncTCP callbacks run on AsyncTCP's FreeRTOS task.
- All they may do: take a mutex, copy bytes into a buffer, set
  flags. They NEVER call `JS_Call`, NEVER allocate JSValues, NEVER
  free anything.
- The JS-thread side (`ESP32QuickJS::loop()` → `JSHttpClient::loop()`
  → `driveParser()` → `parseHeaders()`) is the only place that may
  touch JSValues or call `JS_Call`. And in that path, ctx is the
  parameter passed by QuickJS into `loop()`.

## 3. Cross-thread dispatch via `JS_EnqueueJob`

Pattern (from `esp32/QuickJS.h` around line 3309):

```cpp
JSValue reqArg = JS_MKPTR(JS_TAG_INT, req);
JS_EnqueueJob(ctx, [](JSContext *ctx, int argc, JSValueConst *argv) -> JSValue {
    auto *r = (JSHttpRequest*)JS_VALUE_GET_PTR(argv[0]);
    // ctx is the JS-thread ctx from QuickJS. Use it for JS_Call.
    // NEVER store it. NEVER capture a different ctx.
    return JS_UNDEFINED;
}, 1, &reqArg);
```

## 4. JS-initiated close only

- The ONLY path that releases the underlying socket is JS calling
  `.destroy()`, `.close` (or `AbortController.abort()`, or the runtime being
  torn down).
- Network events (peer disconnect, parse error) NEVER free the
  request. They set a flag; the JS-thread loop notices and resolves
  the relevant Promise.


## 6. When in doubt

- Look at `JSTelnetServer.cpp`  . It
  is the canonical example of "do C++ work on a different task,
  call back into JS when done" and does it correctly.
-

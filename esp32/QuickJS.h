#pragma once

#if defined(WiFi_h) && !defined(ENABLE_WIFI)
#define ENABLE_WIFI
#endif

// ENABLE_FS is opt-in: define it from build_flags or before including QuickJS.h.
// LittleFS / SD are always available on Arduino-ESP32, but we keep the gate to
// stay consistent with ENABLE_WIFI.
#if !defined(ENABLE_FS) && (defined(LittleFS_h) || defined(_SD_H_) || defined(SD_H) || defined(GLOBAL_ESP32))
#define ENABLE_FS
#endif

// ENABLE_ESPNOW: there is no Arduino-style header to test against, so we
// gate on GLOBAL_ESP32 the same way ENABLE_FS does. Users can still force it
// with -DENABLE_ESPNOW.
#if !defined(ENABLE_ESPNOW) && defined(GLOBAL_ESP32)
#define ENABLE_ESPNOW
#endif

// Forward declaration of the main embed class. JSAnalog (defined
// above ESP32QuickJS in this file) needs to look up the qjs
// instance to fire its dispose() callback; the forward decl lets us
// reference ESP32QuickJS* without a circular include.
class ESP32QuickJS;

#include <Arduino.h>

#include <algorithm>
#include <vector>
#include <string>

// Optional override for console.log output. main.cpp sets this to a
// per-telnet-client Stream (or Serial) so console.log output goes to the
// REPL that initiated the command. If null, console.log falls back to
// Serial. Defined in main.cpp.
extern Stream* activeOutputStream;
// Current telnet connection id, or 0 if not running from a telnet REPL.
// Used by setTimeout/setInterval to tag timers with their owner. Defined
// in main.cpp.
extern int currentTelnetId;

#include <queue>
#include <cstdio>
#include <cstdint>
#include <cstring>

#ifdef ENABLE_WIFI
#include <HTTPClient.h>
#include <Server.h>
#include <StreamString.h>
#endif

#ifdef ENABLE_FS
#include <LittleFS.h>
#include <SD.h>
#endif

#ifdef ENABLE_ESPNOW
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

#include "../quickjs.h"

static void qjs_dump_exception_to(JSContext *ctx, JSValue v, Print* out) {
  if (!out) out = &Serial;
  if (!JS_IsUndefined(v)) {
    const char *str = JS_ToCString(ctx, v);
    if (str) {
      out->println(str);
      JS_FreeCString(ctx, str);
    } else {
      out->println("[Exception]");
    }
  }
  JSValue e = JS_GetException(ctx);
  const char *str = JS_ToCString(ctx, e);
  if (str) {
    out->println(str);
    JS_FreeCString(ctx, str);
  }
  if (JS_IsError(ctx, e)) {
    JSValue s = JS_GetPropertyStr(ctx, e, "stack");
    if (!JS_IsUndefined(s)) {
      const char *str = JS_ToCString(ctx, s);
      if (str) {
        out->println(str);
        JS_FreeCString(ctx, str);
      }
    }
    JS_FreeValue(ctx, s);
  }
  JS_FreeValue(ctx, e);
}

static void qjs_dump_exception(JSContext *ctx, JSValue v) {
  // Route to activeOutputStream if a REPL is active; otherwise Serial.
  // Note: this POPS the exception from the pending slot, so the caller
  // must not also call JS_GetException.
  qjs_dump_exception_to(ctx, v, activeOutputStream ? (Print*)activeOutputStream : nullptr);
}

// Stringify a JS value and print it to a specific Stream* using
// JSON.stringify. Used by the REPL to print Promise resolutions back
// to the originating client. The caller may pass nullptr to fall back
// to Serial. The Stream* is captured by value (not dereferenced for
// ownership); the caller is responsible for keeping it alive (or
// accepting that writes after disconnect are no-ops).
static void qjs_print_value_to(JSContext *ctx, JSValueConst v, Print* out) {
  if (!out) out = &Serial;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
  JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
  JSValue strResult = JS_Call(ctx, stringify, JS_UNDEFINED, 1, &v);
  const char *str = JS_ToCString(ctx, strResult);
  if (str) {
    out->println(str);
    JS_FreeCString(ctx, str);
  }
  JS_FreeValue(ctx, strResult);
  JS_FreeValue(ctx, stringify);
  JS_FreeValue(ctx, json);
  JS_FreeValue(ctx, global);
}

#ifdef ENABLE_WIFI
class JSHttpFetcher {
  struct Entry {
    HTTPClient *client;
    JSValue resolving_funcs[2];
    int status;
    void result(JSContext *ctx, uint32_t func, JSValue body) {
      delete client;  // dispose connection before invoke;
      JSValue r = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, r, "body", JS_DupValue(ctx, body));
      JS_SetPropertyStr(ctx, r, "status", JS_NewInt32(ctx, status));
      JS_Call(ctx, resolving_funcs[func], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, resolving_funcs[0]);
      JS_FreeValue(ctx, resolving_funcs[1]);
    }
  };
  std::vector<Entry *> queue;

 public:
  JSValue fetch(JSContext *ctx, JSValueConst jsUrl, JSValueConst options) {
    if (WiFi.status() != WL_CONNECTED) {
      return JS_EXCEPTION;
    }
    const char *url = JS_ToCString(ctx, jsUrl);
    if (!url) {
      return JS_EXCEPTION;
    }
    const char *method = nullptr, *body = nullptr;
    if (JS_IsObject(options)) {
      JSValue m = JS_GetPropertyStr(ctx, options, "method");
      if (JS_IsString(m)) {
        method = JS_ToCString(ctx, m);
      }
      JSValue b = JS_GetPropertyStr(ctx, options, "body");
      if (JS_IsString(m)) {
        body = JS_ToCString(ctx, b);
      }
    }

    Entry *ent = new Entry();
    ent->client = new HTTPClient();
    ent->client->begin(url);
    JS_FreeCString(ctx, url);

    // TODO: remove blocking calls.
    if (method) {
      ent->status = ent->client->sendRequest(method, (uint8_t *)body,
                                             body ? strlen(body) : 0);
    } else {
      ent->status = ent->client->GET();
    }
    queue.push_back(ent);

    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, body);
    return JS_NewPromiseCapability(ctx, ent->resolving_funcs);
  }

  void loop(JSContext *ctx) {
    int doneCount = 0;
    for (auto &pent : queue) {
      WiFiClient *stream = pent->client->getStreamPtr();
      if (stream == nullptr || pent->status <= 0) {
        // reject.
        pent->result(ctx, 1, JS_UNDEFINED);
        delete pent;
        pent = nullptr;
        doneCount++;
        continue;
      }
      if (stream->available()) {
        String body = pent->client->getString();
        JSValue bodyStr = JS_NewString(ctx, body.c_str());
        body.clear();
        pent->result(ctx, 0, bodyStr);
        JS_FreeValue(ctx, bodyStr);
        delete pent;
        pent = nullptr;
        doneCount++;
      }
    }

    if (doneCount > 0) {
      queue.erase(std::remove_if(queue.begin(), queue.end(),
                                 [](Entry *pent) { return pent == nullptr; }),
                  queue.end());
    }
  }
};

class JSConnection {
public:
    WiFiClient* client;
    JSContext* ctx;
    
    struct PendingOp {
        enum Type { READ, WRITE, CLOSE };
        Type type;
        JSValue resolve;
        JSValue reject;
        std::string data;
        uint32_t len;
    };
    std::vector<PendingOp*> pending;

    JSConnection(JSContext* ctx, WiFiClient* client) : ctx(ctx), client(client) {}
    ~JSConnection() {
        for (auto p : pending) {
            JS_FreeValue(ctx, p->resolve);
            JS_FreeValue(ctx, p->reject);
            delete p;
        }
        if (client) delete client;
    }

    void poll() {
        auto it = pending.begin();
        while (it != pending.end()) {
            PendingOp* op = *it;
            bool resolved = false;
            if (op->type == JSConnection::PendingOp::READ) {
                if (client->available() > 0) {
                    int available = client->available();
                    int toRead = std::min(available, (int)op->len);
                    std::vector<char> buf(toRead);
                    client->readBytes(buf.data(), toRead);
                    JSValue res = JS_NewString(ctx, buf.data());
                    JS_Call(ctx, op->resolve, JS_UNDEFINED, 1, &res);
                    JS_FreeValue(ctx, res);
                    resolved = true;
                }
            } else if (op->type == JSConnection::PendingOp::WRITE) {
                if (client->availableForWrite() > 0) {
                    client->write((const uint8_t*)op->data.c_str(), op->data.length());
                    JSValue res = JS_NewBool(ctx, true);
                    JS_Call(ctx, op->resolve, JS_UNDEFINED, 1, &res);
                    JS_FreeValue(ctx, res);
                    resolved = true;
                }
            } else if (op->type == JSConnection::PendingOp::CLOSE) {
                client->stop();
                JSValue res = JS_NewBool(ctx, true);
                JS_Call(ctx, op->resolve, JS_UNDEFINED, 1, &res);
                JS_FreeValue(ctx, res);
                resolved = true;
            }

            if (resolved) {
                JS_FreeValue(ctx, op->resolve);
                JS_FreeValue(ctx, op->reject);
                delete op;
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }
};

class JSWebServer {
    WiFiServer server;
    JSValue callback = JS_UNDEFINED;
    JSContext* ctx = nullptr;
    std::vector<JSConnection*> connections;

public:
    void serve(JSContext* ctx, uint16_t port, JSValue callback) {
        this->ctx = ctx;
        this->callback = JS_DupValue(ctx, callback);
        server.begin(port);
    }

    void loop() {
        if (!ctx) return;

        WiFiClient client = server.available();
        if (client) {
            JSConnection* conn = new JSConnection(ctx, new WiFiClient(client));
            connections.push_back(conn);
            
            JSValue jsConn = createConnection(ctx, conn);
            JS_Call(ctx, callback, JS_UNDEFINED, 1, &jsConn);
            JS_FreeValue(ctx, jsConn);
        }

        auto it = connections.begin();
        while (it != connections.end()) {
            JSConnection* conn = *it;
            if (!conn->client || !conn->client->connected()) {
                delete conn;
                it = connections.erase(it);
            } else {
                conn->poll();
                ++it;
            }
        }
    }

    static JSValue createConnection(JSContext* ctx, JSConnection* conn) {
        JSValue obj = JS_NewObject(ctx);
        JSValue ptr = JS_NewUint32(ctx, (uintptr_t)conn);
        JS_SetPropertyStr(ctx, obj, "__conn_ptr", ptr);
        JS_FreeValue(ctx, ptr);

        static const JSCFunctionListEntry conn_funcs[] = {
            {"read", 1, JS_DEF_CFUNC, 0, {func: {1, JS_CFUNC_generic, conn_read}}},
            {"write", 1, JS_DEF_CFUNC, 0, {func: {1, JS_CFUNC_generic, conn_write}}},
            {"close", 0, JS_DEF_CFUNC, 0, {func: {0, JS_CFUNC_generic, conn_close}}},
        };
        JS_SetPropertyFunctionList(ctx, obj, conn_funcs, 3);
        return obj;
    }

    static JSValue conn_read(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
        JSValue ptrVal = JS_GetPropertyStr(ctx, (JSValue)jsThis, "__conn_ptr");
        uint32_t ptr;
        JS_ToUint32(ctx, &ptr, ptrVal);
        JS_FreeValue(ctx, ptrVal);
        JSConnection* conn = (JSConnection*)ptr;
        
        uint32_t len = 1024;
        if (argc > 0) {
            JS_ToUint32(ctx, &len, argv[0]);
        }

        JSValue resolving_funcs[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
        
        JSConnection::PendingOp* op = new JSConnection::PendingOp{
            JSConnection::PendingOp::Type::READ,
            JS_DupValue(ctx, resolving_funcs[0]),
            JS_DupValue(ctx, resolving_funcs[1]),
            "",
            len
        };
        conn->pending.push_back(op);

        return promise;
    }

    static JSValue conn_write(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
        JSValue ptrVal = JS_GetPropertyStr(ctx, (JSValue)jsThis, "__conn_ptr");
        uint32_t ptr;
        JS_ToUint32(ctx, &ptr, ptrVal);
        JS_FreeValue(ctx, ptrVal);
        JSConnection* conn = (JSConnection*)ptr;
        
        const char* data = JS_ToCString(ctx, argv[0]);
        if (!data) return JS_EXCEPTION;

        JSValue resolving_funcs[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
        
        JSConnection::PendingOp* op = new JSConnection::PendingOp{
            JSConnection::PendingOp::Type::WRITE,
            JS_DupValue(ctx, resolving_funcs[0]),
            JS_DupValue(ctx, resolving_funcs[1]),
            std::string(data),
            0
        };
        conn->pending.push_back(op);

        JS_FreeCString(ctx, data);
        return promise;
    }

    static JSValue conn_close(JSContext* ctx, JSValueConst jsThis, int argc, JSValueConst* argv) {
        JSValue ptrVal = JS_GetPropertyStr(ctx, (JSValue)jsThis, "__conn_ptr");
        uint32_t ptr;
        JS_ToUint32(ctx, &ptr, ptrVal);
        JS_FreeValue(ctx, ptrVal);
        JSConnection* conn = (JSConnection*)ptr;

        JSValue resolving_funcs[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
        
        JSConnection::PendingOp* op = new JSConnection::PendingOp{
            JSConnection::PendingOp::Type::CLOSE,
            JS_DupValue(ctx, resolving_funcs[0]),
            JS_DupValue(ctx, resolving_funcs[1]),
            "",
            0
        };
        conn->pending.push_back(op);

        return promise;
    }
};
#endif

class JSTimer {
  struct TimerEntry {
    uint32_t id;
    int32_t timeout;
    int32_t interval;
    JSValue func;
    // Stream to route console.log / exceptions to while the timer
    // callback runs. Captured at registration time (the REPL that
    // called setTimeout). May be nullptr (route to Serial) or point to
    // a TelnetOut* for a per-client stream. The pointer is treated as
    // opaque here; we just write to it via Print. The caller is
    // responsible for keeping the stream alive (e.g. TelnetOut stays
    // alive until the client disconnects; on disconnect the timer
    // entry's stream may dangle, but writing to a dangling Print* is
    // no-op since AsyncClient::write checks connected()).
    Print* out;
    // Owner id. 0 means "no owner" (Serial / startup.js / unattached).
    // A non-zero id is a telnet connection id; main.cpp calls
    // RemoveTimersForOwner() on disconnect to drop every timer
    // scheduled by that client — UNLESS `nohup` is true, in which
    // case the timer outlives its owner and reroutes to Serial.
    int owner_id;
    // If true, this timer survives owner disconnect and reroutes to
    // Serial after the owner is gone.
    bool nohup;
  };
  std::vector<TimerEntry> timers;
  uint32_t id_counter = 0;

 public:
  uint32_t RegisterTimer(JSValue f, int32_t time, int32_t interval = -1,
                         Print* out = nullptr, int owner_id = 0,
                         bool nohup = false) {
    uint32_t id = ++id_counter;
    timers.push_back(TimerEntry{id, time, interval, f, out, owner_id, nohup});
    return id;
  }
  void RemoveTimer(uint32_t id) {
    timers.erase(std::remove_if(timers.begin(), timers.end(),
                                [id](TimerEntry &t) { return t.id == id; }),
                 timers.end());
  }
  // Drop every timer whose owner matches `owner_id` AND that is not
  // marked nohup. Nohup timers keep running — their `out` is
  // rerouted to Serial so console.log still goes somewhere visible.
  // Called from main.cpp's telnet disconnect handler.
  void RemoveTimersForOwner(JSContext *ctx, int owner_id) {
    for (auto it = timers.begin(); it != timers.end(); ) {
      if (it->owner_id == owner_id && !it->nohup) {
        JS_FreeValue(ctx, it->func);
        it = timers.erase(it);
      } else if (it->owner_id == owner_id && it->nohup) {
        // Detach: reroute to Serial, mark owner as 0 so this code path
        // doesn't run again if the same id is recycled.
        it->out = nullptr;  // nullptr → console_log falls back to Serial
        it->owner_id = 0;
        ++it;
      } else {
        ++it;
      }
    }
  }
  void RemoveAll(JSContext *ctx) {
    for (auto &ent : timers) {
      JS_FreeValue(ctx, ent.func);
    }
    timers.clear();
  }
  int32_t GetNextTimeout(int32_t now) {
    if (timers.empty()) {
      return -1;
    }
    std::sort(timers.begin(), timers.end(),
              [now](TimerEntry &a, TimerEntry &b) -> bool {
                return (a.timeout - now) >
                       (b.timeout - now);  // 2^32 wraparound
              });
    int next = timers.back().timeout - now;
    return max(next, 0);
  }
  bool ConsumeTimer(JSContext *ctx, int32_t now) {
    std::vector<TimerEntry> t;
    int32_t eps = 2;
    while (!timers.empty() && timers.back().timeout - now <= eps) {
      t.push_back(timers.back());
      timers.pop_back();
    }
    for (auto &ent : t) {
      // Route console.log / exceptions to the stream the REPL that
      // scheduled this timer was using. Save and restore the global
      // so a timer that itself calls setTimeout doesn't get tangled
      // up.
      Print* prev = (Print*)activeOutputStream;
      activeOutputStream = ent.out ? (Stream*)ent.out : nullptr;
      // NOTE: may update timers in this JS_Call().
      JSValue r = JS_Call(ctx, ent.func, ent.func, 0, nullptr);
      if (JS_IsException(r)) {
        qjs_dump_exception(ctx, r);
      }
      JS_FreeValue(ctx, r);
      activeOutputStream = (Stream*)prev;

      if (ent.interval >= 0) {
        ent.timeout = now + ent.interval;
        timers.push_back(ent);
      } else {
        JS_FreeValue(ctx, ent.func);
      }
    }
    return !t.empty();
  }
};

// ----------------------------------------------------------------------------
// JSAnalog: Promise-based analog I/O and touch listener.
//
// Three operations are exposed to JS (all on the `esp32` global):
//   - readAnalog(pin, [resolution=12])     → Promise<int>
//   - writeAnalog(pin, duty, [freq=5000],  → Promise<{ channel }>
//                  [resolution=8])
//   - writeAnalogStop(channel)             → Promise<void>
//   - touch(pin, threshold, callback)      → dispose() function
//
// Why promises for synchronous hardware? Two reasons:
//   1. Keeps the JS API uniform: every I/O op returns a Promise.
//   2. Allows the main loop to do all the work in one place
//      (JSAnalog::loop) so the JS engine never blocks on hardware.
//
// Touch listeners use touchAttachInterrupt under the hood. The C ISR
// runs in interrupt context (cannot safely call into QuickJS), so the
// ISR only sets a volatile flag; the JS callback is dispatched from
// JSAnalog::loop() on the main task, where it's safe.
// ----------------------------------------------------------------------------
class JSAnalog {
  // -- readAnalog: queue of pending ADC reads --
  struct ReadEntry {
    uint8_t pin;
    uint8_t resolution;  // 9..12 bits
    JSValue resolving_funcs[2];
  };
  std::vector<ReadEntry> readQueue;

  // -- writeAnalog: queue of pending PWM setup/teardown --
  struct WriteEntry {
    uint8_t pin;
    uint8_t is_stop;       // 0 = attach+write, 1 = stop
    uint8_t resolution;    // 1..16 bits
    uint8_t channel;       // LEDC channel (allocated during processing)
    uint32_t frequency;
    uint32_t duty;         // 0..2^resolution-1
    JSValue resolving_funcs[2];
  };
  std::vector<WriteEntry> writeQueue;

  // Channel allocator. The legacy Arduino-ESP32 LEDC API
  // (ledcSetup/ledcAttachPin/ledcWrite/ledcDetachPin) requires the
  // caller to specify a channel (0..15). We allocate from a bitmap
  // so the user doesn't have to. Channel 0 is reserved for
  // Arduino-internal use (Tone.cpp), so we start at 1 and allow up
  // to LEDC_CHANNELS-1.
  static const int LEDC_CHANNELS = 16;
  uint32_t channelBitmap = 0;  // bit i = channel i allocated
  int allocChannel() {
    for (int i = 1; i < LEDC_CHANNELS; i++) {
      if (!(channelBitmap & (1U << i))) {
        channelBitmap |= (1U << i);
        return i;
      }
    }
    return -1;
  }
  void freeChannel(int ch) {
    if (ch >= 0 && ch < LEDC_CHANNELS) channelBitmap &= ~(1U << ch);
  }

  // pin → channel map. Lets writeAnalogStop(pin) find the channel to
  // free. Index 0 means "no channel / not currently in use for PWM".
  // 0xFF sentinel means "invalid pin index, out of range".
  static const uint8_t PIN_UNUSED = 0;
  static const uint8_t PIN_INVALID = 0xFF;
  uint8_t pinChannel[40];
  void setPinChannel(uint8_t pin, uint8_t ch) {
    if (pin < 40) pinChannel[pin] = ch;
  }
  uint8_t getPinChannel(uint8_t pin) {
    if (pin >= 40) return PIN_INVALID;
    return pinChannel[pin];
  }

  // -- touch listeners --
  struct TouchListener {
    uint8_t pin;
    uint16_t threshold;
    // JS callback. Stays valid for the lifetime of the listener.
    // Released in removeTouch() or end().
    JSValue js_callback;
    // Set by the ISR; consumed by loop() to dispatch the JS callback.
    volatile bool touched;
    // Stored as a pointer because the C API takes `void*` we can
    // back-reference; the ISR uses this to find the listener.
    bool active;
  };
  // Index 1..MAX_TOUCH. Index 0 unused so we can store id as 0 = "no
  // listener". Capped to keep memory bounded; the user can dispose
  // old listeners to free slots.
  static const int MAX_TOUCH = 8;
  TouchListener listeners[MAX_TOUCH + 1];

  // Helper: register a touch interrupt on a free slot. Returns the
  // id (1..MAX_TOUCH) on success, 0 on failure.
  //
  // threshold semantics: 0 = auto. We sample the current (untouched)
  // reading and set the threshold to 2/3 of that, so a touch (which
  // typically drops the reading well below 2/3 of baseline) fires
  // the interrupt once. The actual interrupt also fires on the
  // *release* edge (reading crosses back up), but loop() debounces
  // by only dispatching on state transitions, so the JS callback
  // fires once per touch, not on every ISR.
  int addTouch(JSContext* ctx, uint8_t pin, uint16_t threshold,
               JSValue callback) {
    int slot = -1;
    for (int i = 1; i <= MAX_TOUCH; i++) {
      if (!listeners[i].active) { slot = i; break; }
    }
    if (slot < 0) return 0;
    int8_t pad = digitalPinToTouchChannel(pin);
    if (pad < 0) {
      Serial.printf("[analog] touch: pin %u is not a touch pad\n", pin);
      return 0;
    }
    // Read the baseline for diagnostic logging. Do NOT auto-pick a
    // threshold from it; the user-supplied threshold is authoritative
    // (touch was working, don't break it).
    uint16_t baseline = touchRead(pin);
    listeners[slot].pin = pin;
    listeners[slot].threshold = threshold;
    listeners[slot].js_callback = JS_DupValue(ctx, callback);
    listeners[slot].touched = false;
    listeners[slot].active = true;
    Serial.printf("[analog] touch: pin=%u pad=%d slot=%d thr=%u baseline=%u\n",
                  pin, (int)pad, slot, threshold, baseline);
    // Attach the C-level ISR. We use touchAttachInterruptArg so the
    // ISR can find its listener via the slot id we pass as the
    // `arg` user-data pointer. The trampoline looks up the listener
    // in the static `instance` pointer below.
    touchAttachInterruptArg(
        (uint8_t)pin,
        [](void* arg) {
          int id = (int)(intptr_t)arg;
          TouchListener* tl = JSAnalog::findListener(id);
          if (tl) tl->touched = true;
        },
        (void*)(intptr_t)slot, threshold);
    return slot;
  }

  void removeTouch(JSContext* ctx, int id) {
    if (id < 1 || id > MAX_TOUCH) return;
    TouchListener& tl = listeners[id];
    if (!tl.active) return;
    // Detach the interrupt. touchDetachInterrupt is the inverse of
    // touchAttachInterrupt. Falls back to no-op on cores that don't
    // have it (older Arduino-ESP32).
    #ifdef touchDetachInterrupt
    touchDetachInterrupt(tl.pin);
    #endif
    JS_FreeValue(ctx, tl.js_callback);
    tl.js_callback = JS_UNDEFINED;
    tl.active = false;
    tl.touched = false;
  }

  // Static lookup so the ISR (which is a free function) can find the
  // listener that owns it. We use the same `instance` pattern as
  // JSEspNow.
  static JSAnalog* instance;
  static TouchListener* findListener(int id) {
    if (!instance || id < 1 || id > MAX_TOUCH) return nullptr;
    if (!instance->listeners[id].active) return nullptr;
    return &instance->listeners[id];
  }

 public:
  // Reset all state. Called from end() and the constructor.
  void clear() {
    for (int i = 1; i <= MAX_TOUCH; i++) {
      listeners[i].active = false;
      listeners[i].touched = false;
      listeners[i].js_callback = JS_UNDEFINED;
    }
    channelBitmap = 0;
    for (int i = 0; i < 40; i++) pinChannel[i] = PIN_UNUSED;
  }

  // Called once during qjs.begin() to wire the static instance.
  void init() { instance = this; clear(); }

  // Called from qjs.end() to free everything.
  void end(JSContext* ctx) {
    for (int i = 1; i <= MAX_TOUCH; i++) {
      if (listeners[i].active) {
        removeTouch(ctx, i);
      }
    }
    // Free any in-flight read/write queues. Reject pending promises.
    for (auto& e : readQueue) {
      JSValue r = JS_UNDEFINED;
      JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, e.resolving_funcs[0]);
      JS_FreeValue(ctx, e.resolving_funcs[1]);
    }
    readQueue.clear();
    for (auto& e : writeQueue) {
      JSValue r = JS_UNDEFINED;
      JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, e.resolving_funcs[0]);
      JS_FreeValue(ctx, e.resolving_funcs[1]);
    }
    writeQueue.clear();
  }

  // Drain one item from each queue. Returns immediately if empty.
  void loop(JSContext* ctx) {
    // --- Process one readAnalog request per tick. ADC reads are
    // fast but we still rate-limit to keep the main loop snappy. ---
    if (!readQueue.empty()) {
      ReadEntry e = readQueue.front();
      readQueue.erase(readQueue.begin());
      int value = analogRead(e.pin);
      JSValue r = JS_NewInt32(ctx, value);
      JS_Call(ctx, e.resolving_funcs[0], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, e.resolving_funcs[0]);
      JS_FreeValue(ctx, e.resolving_funcs[1]);
    }
    // --- Process one writeAnalog / writeAnalogStop per tick. ---
    if (!writeQueue.empty()) {
      WriteEntry e = writeQueue.front();
      writeQueue.erase(writeQueue.begin());
      if (e.is_stop) {
        // Look up the channel that was used for this pin, free it,
        // and detach the pin. ledcWrite(pin, 0) is a no-op on the
        // legacy core; ledcDetachPin is the real detach. If the pin
        // was never allocated, freeChannel(0) is a no-op.
        uint8_t ch = getPinChannel(e.pin);
        if (ch != PIN_INVALID && ch != PIN_UNUSED) {
          ledcDetachPin(e.pin);
          freeChannel(ch);
          setPinChannel(e.pin, PIN_UNUSED);
        }
        JSValue r = JS_UNDEFINED;
        JS_Call(ctx, e.resolving_funcs[0], JS_UNDEFINED, 1, &r);
        JS_FreeValue(ctx, r);
      } else {
        int ch = allocChannel();
        if (ch >= 0) {
          // Setup the channel at the requested freq/resolution, then
          // attach the pin to it.
          ledcSetup(ch, e.frequency, e.resolution);
          ledcAttachPin(e.pin, ch);
          // duty value 0..2^resolution-1
          uint32_t maxDuty = (1UL << e.resolution) - 1;
          uint32_t duty = e.duty > maxDuty ? maxDuty : e.duty;
          ledcWrite(ch, duty);
          // Remember which channel this pin uses so writeAnalogStop
          // can free it later.
          setPinChannel(e.pin, (uint8_t)ch);
          // Resolve with { channel: ch }.
          JSValue r = JS_NewObject(ctx);
          JS_SetPropertyStr(ctx, r, "channel", JS_NewInt32(ctx, ch));
          JS_Call(ctx, e.resolving_funcs[0], JS_UNDEFINED, 1, &r);
          JS_FreeValue(ctx, r);
        } else {
          // Reject.
          JSValue r = JS_NewString(ctx, "no free LEDC channel");
          JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &r);
          JS_FreeValue(ctx, r);
        }
      }
      JS_FreeValue(ctx, e.resolving_funcs[0]);
      JS_FreeValue(ctx, e.resolving_funcs[1]);
    }
    // --- Dispatch any touch events flagged by ISRs. ---
    for (int i = 1; i <= MAX_TOUCH; i++) {
      TouchListener& tl = listeners[i];
      if (!tl.active) continue;
      if (!tl.touched) continue;
      // Atomically read+clear so a re-trigger during dispatch is
      // not lost.
      tl.touched = false;
      JSValue arg = JS_NewBool(ctx, true);
      JSValue ret = JS_Call(ctx, tl.js_callback, JS_UNDEFINED, 1, &arg);
      JS_FreeValue(ctx, arg);
      if (JS_IsException(ret)) {
        qjs_dump_exception(ctx, ret);
      }
      JS_FreeValue(ctx, ret);
    }
  }

  // -- The four JS-callable entry points. Called from the static
  // C trampolines below. --
  JSValue js_readAnalog(JSContext* ctx, int argc, JSValueConst* argv) {
    uint32_t pin, resolution = 12;
    JS_ToUint32(ctx, &pin, argv[0]);
    if (argc >= 2) JS_ToUint32(ctx, &resolution, argv[1]);
    if (resolution < 9) resolution = 9;
    if (resolution > 12) resolution = 12;
    analogSetPinAttenuation((uint8_t)pin, ADC_11db);
    ReadEntry e;
    e.pin = (uint8_t)pin;
    e.resolution = (uint8_t)resolution;
    readQueue.push_back(e);
    return JS_NewPromiseCapability(ctx, readQueue.back().resolving_funcs);
  }

  JSValue js_writeAnalog(JSContext* ctx, int argc, JSValueConst* argv) {
    uint32_t pin, duty, frequency = 5000, resolution = 8;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &duty, argv[1]);
    if (argc >= 3) JS_ToUint32(ctx, &frequency, argv[2]);
    if (argc >= 4) JS_ToUint32(ctx, &resolution, argv[3]);
    if (resolution < 1) resolution = 1;
    if (resolution > 16) resolution = 16;
    WriteEntry e;
    e.pin = (uint8_t)pin;
    e.is_stop = 0;
    e.resolution = (uint8_t)resolution;
    e.frequency = frequency;
    e.duty = duty;
    writeQueue.push_back(e);
    return JS_NewPromiseCapability(ctx, writeQueue.back().resolving_funcs);
  }

  JSValue js_writeAnalogStop(JSContext* ctx, int argc, JSValueConst* argv) {
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    WriteEntry e;
    e.pin = (uint8_t)pin;
    e.is_stop = 1;
    writeQueue.push_back(e);
    return JS_NewPromiseCapability(ctx, writeQueue.back().resolving_funcs);
  }

  // touch(pin, threshold, callback) → dispose()
  // Returns a JS function that, when called, removes the listener.
  JSValue js_touch(JSContext* ctx, int argc, JSValueConst* argv) {
    uint32_t pin, threshold = 0;
    JS_ToUint32(ctx, &pin, argv[0]);
    if (argc >= 2) JS_ToUint32(ctx, &threshold, argv[1]);
    if (!JS_IsFunction(ctx, argv[2])) {
      return JS_ThrowTypeError(ctx, "touch: callback must be a function");
    }
    int id = addTouch(ctx, (uint8_t)pin, (uint16_t)threshold, argv[2]);
    if (id == 0) {
      return JS_ThrowInternalError(ctx, "touch: no free listener slot");
    }
    // Build the dispose function via JS_NewCFunctionData, capturing
    // the slot id in the function's `magic` integer slot. QuickJS
    // gives dispose() calls the magic value as the 5th arg; the
    // 6th is the func_data pointer (we pass nullptr so it's
    // always null here). The lambda is non-capturing so it
    // converts to a plain C function pointer; we look up the
    // JSAnalog via the static `instance` pointer instead of going
    // through the (still-incomplete here) ESP32QuickJS forward
    // declaration.
    auto trampoline = [](JSContext* ctx2, JSValueConst jsThis2, int argc2,
                         JSValueConst* argv2, int magic,
                         JSValueConst* func_data) -> JSValue {
      (void)jsThis2; (void)argc2; (void)argv2; (void)func_data;
      if (JSAnalog::instance) {
        JSAnalog::instance->removeTouch(ctx2, magic);
      }
      return JS_UNDEFINED;
    };
    return JS_NewCFunctionData(ctx, trampoline, 0, id, 0, nullptr);
  }
};

// Static instance pointer for ISR → listener lookup.
JSAnalog* JSAnalog::instance = nullptr;

#ifdef ENABLE_FS
// Generic Promise-style filesystem backend. Works with any Arduino FS (LittleFS, SD, ...).
// Mirrors the JSHttpFetcher style: queue entries, poll from ESP32QuickJS::loop().
class JSFileSystem {
 public:
  enum Op {
    OP_READ,
    OP_WRITE,
    OP_REMOVE,
    OP_LIST,
  };

  struct Entry {
    Op op;
    fs::FS *fs;
    std::string path;
    std::string content;  // used for OP_WRITE
    JSValue resolving_funcs[2];
    bool done = false;
    bool ok = false;
    std::string result;  // read content or list text
    std::string error;   // error message on failure
  };

 private:
  std::vector<Entry *> queue;
  fs::FS *defaultFs = nullptr;

  static void finish(Entry *e, JSContext *ctx) {
    if (e->ok) {
      JSValue val = JS_NewString(ctx, e->result.c_str());
      JS_Call(ctx, e->resolving_funcs[0], JS_UNDEFINED, 1, &val);
      JS_FreeValue(ctx, val);
    } else {
      JSValue err = JS_NewString(ctx, e->error.c_str());
      JS_Call(ctx, e->resolving_funcs[1], JS_UNDEFINED, 1, &err);
      JS_FreeValue(ctx, err);
    }
    JS_FreeValue(ctx, e->resolving_funcs[0]);
    JS_FreeValue(ctx, e->resolving_funcs[1]);
    e->done = true;
  }

 public:
  // Bind a backing fs::FS (LittleFS, SD, ...). Use nullptr to detach.
  void bind(fs::FS *fs) { defaultFs = fs; }
  fs::FS *bound() const { return defaultFs; }

  JSValue op(JSContext *ctx, Op op_, const char *path, const char *content) {
    if (!defaultFs) {
      JSValue resolving_funcs[2];
      JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
      JSValue err = JS_NewString(ctx, "filesystem not mounted");
      JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
      JS_FreeValue(ctx, err);
      JS_FreeValue(ctx, resolving_funcs[0]);
      JS_FreeValue(ctx, resolving_funcs[1]);
      return promise;
    }
    Entry *e = new Entry();
    e->op = op_;
    e->fs = defaultFs;
    e->path = path ? path : "";
    if (content) e->content = content;
    JSValue promise = JS_NewPromiseCapability(ctx, e->resolving_funcs);
    queue.push_back(e);
    return promise;
  }

  // Process at most one entry per loop tick. Returns true if work was done.
  // Processing one at a time keeps flash wear / SD latency bounded.
  void loop(JSContext *ctx) {
    for (auto &pent : queue) {
      if (pent->done) continue;
      if (!pent->fs) {
        pent->ok = false;
        pent->error = "filesystem unbound";
        finish(pent, ctx);
        continue;
      }
      switch (pent->op) {
        case OP_READ: {
          if (!pent->fs->exists(pent->path.c_str())) {
            pent->ok = false;
            pent->error = "ENOENT: " + pent->path;
            finish(pent, ctx);
            break;
          }
          File f = pent->fs->open(pent->path.c_str(), "r");
          if (!f) {
            pent->ok = false;
            pent->error = "EOPEN: " + pent->path;
            finish(pent, ctx);
            break;
          }
          std::string out;
          // Chunk into a temporary buffer; for very large files users can
          // chunked APIs (planned extension).
          const size_t CHUNK = 256;
          uint8_t buf[CHUNK];
          while (f.available()) {
            size_t n = f.read(buf, CHUNK);
            if (!n) break;
            out.append((const char *)buf, n);
          }
          f.close();
          pent->ok = true;
          pent->result = std::move(out);
          finish(pent, ctx);
          break;
        }
        case OP_WRITE: {
          // Ensure parent directory exists for LittleFS; SD auto-creates.
          File f = pent->fs->open(pent->path.c_str(), "w");
          if (!f) {
            pent->ok = false;
            pent->error = "EOPEN: " + pent->path;
            finish(pent, ctx);
            break;
          }
          size_t written = f.print(pent->content.c_str());
          f.close();
          if (written != pent->content.size()) {
            pent->ok = false;
            pent->error = "ESHORT: " + pent->path;
            finish(pent, ctx);
            break;
          }
          pent->ok = true;
          pent->result = "wrote " + std::to_string(written) + " bytes";
          finish(pent, ctx);
          break;
        }
        case OP_REMOVE: {
          if (!pent->fs->exists(pent->path.c_str())) {
            pent->ok = false;
            pent->error = "ENOENT: " + pent->path;
            finish(pent, ctx);
            break;
          }
          if (!pent->fs->remove(pent->path.c_str())) {
            pent->ok = false;
            pent->error = "Erm: " + pent->path;
            finish(pent, ctx);
            break;
          }
          pent->ok = true;
          pent->result = "removed " + pent->path;
          finish(pent, ctx);
          break;
        }
        case OP_LIST: {
          const char *dir = pent->path.empty() ? "/" : pent->path.c_str();
          File root = pent->fs->open(dir);
          if (!root || !root.isDirectory()) {
            pent->ok = false;
            pent->error = "ENOTDIR: " + pent->path;
            finish(pent, ctx);
            break;
          }
          std::string out;
          File entry = root.openNextFile();
          while (entry) {
            if (entry.isDirectory()) {
              out += "/";
              out += entry.name();
            } else {
              out += entry.name();
              out += "\t";
              out += std::to_string(entry.size());
            }
            out += "\n";
            entry.close();
            entry = root.openNextFile();
          }
          root.close();
          pent->ok = true;
          pent->result = std::move(out);
          finish(pent, ctx);
          break;
        }
      }
    }
    // Drop completed entries.
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [](Entry *e) {
                                 if (e->done) {
                                   delete e;
                                   return true;
                                 }
                                 return false;
                               }),
                queue.end());
  }
};

class JSLittleFS {
 public:
  bool begin(bool formatOnFail = true) {
    return LittleFS.begin(formatOnFail);
  }
  void end() { LittleFS.end(); }
};

class JSSD {
 public:
  bool begin(uint8_t csPin = 5) {
    if (csPin == 0) {
      // SD library treats 0 as "no CS pin" sentinel for software chip select
      // on some cores; pass through as-is for compatibility.
      return SD.begin();
    }
    return SD.begin(csPin);
  }
  void end() { SD.end(); }
};
#endif  // ENABLE_FS

#ifdef ENABLE_ESPNOW
// JSEspNow bridges the C esp_now API to QuickJS. Goals:
//   * Hide peer lifecycle behind a friendly JS surface
//   * Make sends Promise-based: ESPNow.send() resolves when the radio reports
//     success/fail via the send-callback, not when the buffer is queued.
//   * Expose ESPNow.onReceive(cb) and ESPNow.onSend(cb) for "fire whenever"
//     events that aren't tied to a single send call.
//   * Allow MAC addresses as colon strings or 6-element number arrays.
//   * Allow payloads as JS strings (raw bytes) or number arrays (0..255).
class JSEspNow {
 public:
  JSEspNow() : ctx(nullptr), initialized(false),
               onReceiveCb(JS_UNDEFINED), onSendCb(JS_UNDEFINED) {}

  // ---- lifecycle ----
  bool init() {
    if (initialized) return true;
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(&JSEspNow::s_recv_cb);
    esp_now_register_send_cb(&JSEspNow::s_send_cb);
    initialized = true;
    return true;
  }

  void end() {
    if (!initialized) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    initialized = false;
  }

  bool isInitialized() const { return initialized; }

  // ---- peer mgmt ----
  bool addPeer(const uint8_t *mac, uint8_t channel, bool encrypt,
               const uint8_t *lmk, wifi_interface_t iface) {
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.channel = channel;
    p.encrypt = encrypt;
    p.ifidx = iface;
    if (encrypt && lmk) memcpy(p.lmk, lmk, ESP_NOW_KEY_LEN);
    esp_err_t r = esp_now_add_peer(&p);
    if (r == ESP_OK) return true;
    if (r == ESP_ERR_ESPNOW_EXIST) {
      // Re-adding the same peer should be idempotent.
      return esp_now_mod_peer(&p) == ESP_OK;
    }
    return false;
  }

  bool delPeer(const uint8_t *mac) {
    return esp_now_del_peer(mac) == ESP_OK;
  }

  bool peerExists(const uint8_t *mac) {
    return esp_now_is_peer_exist(mac);
  }

  // ---- send ----
  // Returns the queued send-id (>0) or 0 on enqueue failure.
  // The actual resolution (success/fail) happens later in loop() when the
  // matching send-callback fires.
  uint32_t send(const uint8_t *mac, const uint8_t *data, size_t len) {
    if (!initialized || len == 0 || len > ESP_NOW_MAX_DATA_LEN) return 0;
    uint32_t id = ++sendIdCounter;
    PendingSend s{};
    s.id = id;
    if (mac) memcpy(s.mac, mac, ESP_NOW_ETH_ALEN);
    else memset(s.mac, 0, ESP_NOW_ETH_ALEN);
    s.unicast = (mac != nullptr);
    pendingSends.push_back(s);

    if (esp_now_send(mac, data, len) != ESP_OK) {
      // Pop the entry and fail it immediately.
      for (auto it = pendingSends.begin(); it != pendingSends.end(); ++it) {
        if (it->id == id) {
          it->resolved = true;
          it->ok = false;
          break;
        }
      }
    }
    return id;
  }

  // ---- callbacks (set by the C bridge) ----
  void setReceiveHandler(JSContext *c, JSValue fn) {
    ctx = c;
    JS_FreeValue(ctx, onReceiveCb);
    onReceiveCb = JS_DupValue(ctx, fn);
  }
  void setSendHandler(JSContext *c, JSValue fn) {
    ctx = c;
    JS_FreeValue(ctx, onSendCb);
    onSendCb = JS_DupValue(ctx, fn);
  }
  void clearHandlers() {
    if (ctx) {
      JS_FreeValue(ctx, onReceiveCb);
      JS_FreeValue(ctx, onSendCb);
    }
    onReceiveCb = JS_UNDEFINED;
    onSendCb = JS_UNDEFINED;
  }

  // Called from ESP32QuickJS::loop() to drain completed sends.
  // Resolves any pending send-promises whose send-callback has fired and
  // fires the onReceive/onSend global handlers with the latest event.
  void loop() {
    if (!ctx) return;

    // Resolve any pending send Promises that have completed.
    for (auto &s : pendingSends) {
      if (s.promise == JS_UNDEFINED) continue;
      if (s.resolved) {
        JS_Call(ctx, s.resolvingFuncs[s.ok ? 0 : 1], JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, s.resolvingFuncs[0]);
        JS_FreeValue(ctx, s.resolvingFuncs[1]);
        JS_FreeValue(ctx, s.promise);
        s.promise = JS_UNDEFINED;
      }
    }
    pendingSends.erase(
        std::remove_if(pendingSends.begin(), pendingSends.end(),
                       [](const PendingSend &s) {
                         return s.promise == JS_UNDEFINED && s.resolved;
                       }),
        pendingSends.end());

    // Drain receive events.
    while (!recvQueue.empty()) {
      RecvEvent e = std::move(recvQueue.front());
      recvQueue.pop();
      if (JS_IsFunction(ctx, onReceiveCb)) {
        JSValue rmac = makeMacArray(ctx, e.mac);
        // Pass data as a Uint8Array-shaped number array so JS can read bytes
        // losslessly (some payloads may be non-UTF8 binary).
        JSValue rdata = JS_NewArray(ctx);
        for (size_t i = 0; i < e.data.size(); i++) {
          JS_SetPropertyUint32(ctx, rdata, i, JS_NewInt32(ctx, e.data[i]));
        }
        JSValue rssi = JS_NewInt32(ctx, e.rssi);  // 0 if unknown
        JSValue argv[3] = {rmac, rdata, rssi};
        JSValue ret = JS_Call(ctx, onReceiveCb, onReceiveCb, 3, argv);
        if (JS_IsException(ret)) qjs_dump_exception(ctx, ret);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, rmac);
        JS_FreeValue(ctx, rdata);
        JS_FreeValue(ctx, rssi);
      }
    }

    // Drain send events.
    while (!sendQueue.empty()) {
      SendEvent e = std::move(sendQueue.front());
      sendQueue.pop();
      if (JS_IsFunction(ctx, onSendCb)) {
        JSValue rmac = makeMacArray(ctx, e.mac);
        JSValue rbool = JS_NewBool(ctx, e.ok);
        JSValue argv[2] = {rmac, rbool};
        JSValue ret = JS_Call(ctx, onSendCb, onSendCb, 2, argv);
        if (JS_IsException(ret)) qjs_dump_exception(ctx, ret);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, rmac);
        JS_FreeValue(ctx, rbool);
      }
    }
  }

  // Attach a fresh Promise to a queued send. Called from the JS bridge after
  // ESPNow.send() returns its send-id.
  JSValue attachPromiseToSend(uint32_t id) {
    for (auto &s : pendingSends) {
      if (s.id == id) {
        if (s.promise != JS_UNDEFINED) {
          // Already attached. Return a dup of the existing promise.
          return JS_DupValue(ctx, s.promise);
        }
        s.promise = JS_NewPromiseCapability(ctx, s.resolvingFuncs);
        if (s.resolved) {
          // Race: send-callback already fired between the C++ call and now.
          // Resolve immediately on the next loop tick by leaving it in queue
          // and letting loop() handle it.
        }
        return JS_DupValue(ctx, s.promise);
      }
    }
    // Send id not found (already failed at enqueue). Return a rejected promise.
    JSValue resolving_funcs[2];
    JSValue p = JS_NewPromiseCapability(ctx, resolving_funcs);
    JSValue err = JS_NewString(ctx, "ESPNOW send enqueue failed");
    JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return p;
  }

  // Public for the static C callbacks. Must be set by the bridge before
  // the first callback can fire.
  JSContext *ctx;
  bool initialized;

  // Singleton pointer used by the C callbacks. The bridge sets it after
  // constructing the JSEspNow instance.
  static JSEspNow *instance;

 private:
  struct PendingSend {
    uint32_t id;
    uint8_t mac[ESP_NOW_ETH_ALEN];
    bool unicast;
    bool resolved = false;
    bool ok = false;
    JSValue promise = JS_UNDEFINED;
    JSValue resolvingFuncs[2] = {JS_UNDEFINED, JS_UNDEFINED};
  };
  struct RecvEvent {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    std::vector<uint8_t> data;
    int rssi = 0;
  };
  struct SendEvent {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    bool ok;
  };

  std::vector<PendingSend> pendingSends;
  std::queue<RecvEvent> recvQueue;
  std::queue<SendEvent> sendQueue;
  uint32_t sendIdCounter = 0;
  JSValue onReceiveCb;
  JSValue onSendCb;

  static JSValue makeMacArray(JSContext *ctx, const uint8_t *mac) {
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
      JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, mac[i]));
    }
    return arr;
  }

  // Match a fired send-callback to the most recent pending unicast or any
  // broadcast (peer_addr == NULL) send. Mark it resolved; loop() will fire
  // the promise then.
  void completeSend(const uint8_t *mac, esp_now_send_status_t status) {
    bool ok = (status == ESP_NOW_SEND_SUCCESS);
    // Prefer the most recent unicast to this mac; otherwise mark the most
    // recent broadcast.
    int unicastIdx = -1;
    int broadcastIdx = -1;
    for (size_t i = 0; i < pendingSends.size(); i++) {
      if (pendingSends[i].resolved) continue;
      if (pendingSends[i].unicast &&
          memcmp(pendingSends[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
        unicastIdx = (int)i;
        break;
      } else if (!pendingSends[i].unicast) {
        broadcastIdx = (int)i;
      }
    }
    int idx = (unicastIdx >= 0) ? unicastIdx : broadcastIdx;
    if (idx < 0) return;
    pendingSends[idx].resolved = true;
    pendingSends[idx].ok = ok;
  }

  // ---- C callback trampolines (must be static and C-linkage) ----
  static void s_recv_cb(const uint8_t *mac_addr, const uint8_t *data,
                        int data_len) {
    if (!instance) return;
    RecvEvent e{};
    if (mac_addr) memcpy(e.mac, mac_addr, ESP_NOW_ETH_ALEN);
    e.data.assign(data, data + data_len);
    instance->recvQueue.push(std::move(e));
  }
  static void s_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (!instance) return;
    SendEvent e{};
    if (mac_addr) memcpy(e.mac, mac_addr, ESP_NOW_ETH_ALEN);
    e.ok = (status == ESP_NOW_SEND_SUCCESS);
    instance->sendQueue.push(e);
    if (mac_addr) {
      instance->completeSend(mac_addr, status);
    } else {
      // Broadcast: complete the oldest unresolved broadcast.
      for (auto &s : instance->pendingSends) {
        if (!s.resolved && !s.unicast) {
          s.resolved = true;
          s.ok = e.ok;
          break;
        }
      }
    }
  }
};
JSEspNow *JSEspNow::instance = nullptr;
#endif  // ENABLE_ESPNOW

class ESP32QuickJS {
 public:
  JSRuntime *rt;
  JSContext *ctx;
  JSTimer timer;
  JSValue loop_func = JS_UNDEFINED;
  // Analog I/O and touch listeners. Always available (no feature
  // flag — uses on-chip ADC / LEDC / touch hardware).
  JSAnalog analog;
#ifdef ENABLE_WIFI
  JSHttpFetcher httpFetcher;
  JSWebServer webServer;
#endif
#ifdef ENABLE_FS
  JSFileSystem littlefs;
  JSFileSystem sd;
  bool littlefsMounted = false;
  bool sdMounted = false;
#endif
#ifdef ENABLE_ESPNOW
  JSEspNow espNow;
#endif

  void begin() {
    JSRuntime *rt = JS_NewRuntime();
    begin(rt, JS_NewContext(rt));
  }

  void begin(JSRuntime *rt, JSContext *ctx, int memoryLimit = 0) {
    this->rt = rt;
    this->ctx = ctx;
    if (memoryLimit == 0) {
      memoryLimit = ESP.getFreeHeap() >> 1;
    }
    JS_SetMemoryLimit(rt, memoryLimit);
    JS_SetGCThreshold(rt, memoryLimit >> 3);
    JSValue global = JS_GetGlobalObject(ctx);
    setup(ctx, global);
    JS_FreeValue(ctx, global);
    analog.init();
#ifdef ENABLE_ESPNOW
    // Wire the static trampoline so C callbacks can find this instance.
    JSEspNow::instance = &espNow;
    espNow.ctx = ctx;
#endif
  }

  void end() {
    timer.RemoveAll(ctx);
    analog.end(ctx);
#ifdef ENABLE_ESPNOW
    if (espNow.isInitialized()) espNow.end();
    espNow.clearHandlers();
    JSEspNow::instance = nullptr;
#endif
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
  }

  void loop(bool callLoopFn = true) {
    // async
    JSContext *c;
    int ret = JS_ExecutePendingJob(JS_GetRuntime(ctx), &c);
    if (ret < 0) {
      qjs_dump_exception(ctx, JS_UNDEFINED);
    }

    // timer
    uint32_t now = millis();
    if (timer.GetNextTimeout(now) >= 0) {
      timer.ConsumeTimer(ctx, now);
    }

    // Analog I/O + touch listeners (always available, no flag)
    analog.loop(ctx);

#ifdef ENABLE_WIFI
    httpFetcher.loop(ctx);
    webServer.loop();
#endif
#ifdef ENABLE_FS
    littlefs.loop(ctx);
    sd.loop(ctx);
#endif
#ifdef ENABLE_ESPNOW
    espNow.loop();
#endif

    // loop()
    if (callLoopFn && JS_IsFunction(ctx, loop_func)) {
      JSValue ret = JS_Call(ctx, loop_func, loop_func, 0, nullptr);
      if (JS_IsException(ret)) {
        qjs_dump_exception(ctx, ret);
      }
      JS_FreeValue(ctx, ret);
    }
  }

  void runGC() { JS_RunGC(rt); }

  bool exec(const char *code) {
    JSValue result = eval(code);
    bool ret = JS_IsException(result);
    JS_FreeValue(ctx, result);
    return ret;
  }

  JSValue eval(const char *code) {
    JSValue ret =
        JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(ret)) {
      // qjs_dump_exception now routes via activeOutputStream when set,
      // so the exception message goes to the originating REPL
      // (Serial REPL or specific telnet client) instead of always
      // to Serial.
      qjs_dump_exception(ctx, ret);
    }
    return ret;
  }

  // Async-eval: wraps the user code in an async IIFE so that
  // top-level `await` works. The returned value is a Promise; the
  // caller can chain .then() if they want a result. Used by the
  // REPL to support `await esp32.readAnalog(34)` directly.
  //
  // Note: this costs one extra function-call frame and forces the
  // code to run as `script` inside the async body. Top-level `let`
  // and `const` are function-scoped, not block-scoped, so they
  // don't leak to subsequent evals — same as the JS spec.
  JSValue evalAsync(const char *code) {
    // Wrap: "(async()=>{ <code> })()"
    size_t n = strlen(code);
    // Prefix + suffix + NUL
    const char* prefix = "(async()=>{\n";
    const char* suffix = "\n})()";
    size_t total = strlen(prefix) + n + strlen(suffix) + 1;
    char* wrapped = (char*)js_malloc(ctx, total);
    if (!wrapped) return JS_EXCEPTION;
    snprintf(wrapped, total, "%s%s%s", prefix, code, suffix);
    JSValue ret = JS_Eval(ctx, wrapped, total - 1, "<eval-async>",
                          JS_EVAL_TYPE_GLOBAL);
    js_free(ctx, wrapped);
    if (JS_IsException(ret)) {
      qjs_dump_exception(ctx, ret);
    }
    return ret;
  }

  void setLoopFunc(const char *fname) {
    JSValue global = JS_GetGlobalObject(ctx);
    setLoopFunc(JS_GetPropertyStr(ctx, global, fname));
    JS_FreeValue(ctx, global);
  }

 protected:
  void setLoopFunc(JSValue f) {
    JS_FreeValue(ctx, loop_func);
    loop_func = f;
  }

  virtual void setup(JSContext *ctx, JSValue global) {
    this->ctx = ctx;
    JS_SetContextOpaque(ctx, this);

    // setup console.log()
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, console_log, "log", 1));

    // timer
    JS_SetPropertyStr(ctx, global, "setTimeout",
                      JS_NewCFunction(ctx, set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                      JS_NewCFunction(ctx, clear_timeout, "clearTimeout", 1));    JS_SetPropertyStr(ctx, global, "setInterval",
                      JS_NewCFunction(ctx, set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                      JS_NewCFunction(ctx, clear_timeout, "clearInterval", 1));


#ifdef ENABLE_WIFI
    JSValue wifi = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "WiFi", wifi);
    
    static const JSCFunctionListEntry wifi_funcs[] = {
        JSCFunctionListEntry{"connected", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, wifi_is_connected}
                             }},
        JSCFunctionListEntry{"connect", 2, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, wifi_connect}
                             }},
        JSCFunctionListEntry{"reconnect", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, wifi_reconnect}
                             }},
        JSCFunctionListEntry{"startAP", 1, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, wifi_start_ap}
                             }},
        JSCFunctionListEntry{"stopAP", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, wifi_stop_ap}
                             }},
        JSCFunctionListEntry{"stop", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, wifi_stop}
                             }},
        JSCFunctionListEntry{"fetch", 2, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, http_fetch}
                             }},
        JSCFunctionListEntry{"ip", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, wifi_ip}
                             }},
        JSCFunctionListEntry{"serve", 2, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, wifi_serve}
                             }},
    };
    JS_SetPropertyFunctionList(ctx, wifi, wifi_funcs, sizeof(wifi_funcs) / sizeof(JSCFunctionListEntry));
    // Do not free wifi here, it is owned by the global object
#endif

#ifdef ENABLE_FS
    // FS = { LittleFS: { readFile, writeFile, removeFile, listFiles },
    //        SD:       { init, readFile, writeFile, removeFile, listFiles } }
    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "FS", fs);

    JSValue lfs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "LittleFS", lfs);

    static const JSCFunctionListEntry lfs_funcs[] = {
        JSCFunctionListEntry{"readFile", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_lfs_read}
        }},
        JSCFunctionListEntry{"writeFile", 0, JS_DEF_CFUNC, 0, {
            func : {2, JS_CFUNC_generic, fs_lfs_write}
        }},
        JSCFunctionListEntry{"removeFile", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_lfs_remove}
        }},
        JSCFunctionListEntry{"listFiles", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_lfs_list}
        }},
    };
    JS_SetPropertyFunctionList(ctx, lfs, lfs_funcs,
                               sizeof(lfs_funcs) / sizeof(JSCFunctionListEntry));

    JSValue sdfs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "SD", sdfs);

    static const JSCFunctionListEntry sd_funcs[] = {
        JSCFunctionListEntry{"init", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_sd_init}
        }},
        JSCFunctionListEntry{"readFile", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_sd_read}
        }},
        JSCFunctionListEntry{"writeFile", 0, JS_DEF_CFUNC, 0, {
            func : {2, JS_CFUNC_generic, fs_sd_write}
        }},
        JSCFunctionListEntry{"removeFile", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_sd_remove}
        }},
        JSCFunctionListEntry{"listFiles", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, fs_sd_list}
        }},
    };
    JS_SetPropertyFunctionList(ctx, sdfs, sd_funcs,
                               sizeof(sd_funcs) / sizeof(JSCFunctionListEntry));
#endif

#ifdef ENABLE_ESPNOW
    // ESPNow = { init, end, addPeer, delPeer, peerExists, send, broadcast,
    //            onReceive, onSend, BROADCAST_ADDR, MAX_DATA_LEN }
    JSValue en = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "ESPNow", en);

    // Pre-define BROADCAST_ADDR and MAX_DATA_LEN as global constants.
    {
      JSValue ba = JS_NewArray(ctx);
      const uint8_t ff[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      for (int i = 0; i < 6; i++) JS_SetPropertyUint32(ctx, ba, i, JS_NewInt32(ctx, ff[i]));
      JS_SetPropertyStr(ctx, en, "BROADCAST_ADDR", ba);
    }
    JS_SetPropertyStr(ctx, en, "MAX_DATA_LEN", JS_NewInt32(ctx, ESP_NOW_MAX_DATA_LEN));
    JS_SetPropertyStr(ctx, en, "MAX_TOTAL_PEERS", JS_NewInt32(ctx, ESP_NOW_MAX_TOTAL_PEER_NUM));
    JS_SetPropertyStr(ctx, en, "MAX_ENCRYPT_PEERS", JS_NewInt32(ctx, ESP_NOW_MAX_ENCRYPT_PEER_NUM));

    static const JSCFunctionListEntry en_funcs[] = {
        JSCFunctionListEntry{"init", 0, JS_DEF_CFUNC, 0, {
            func : {0, JS_CFUNC_generic, espnow_init}
        }},
        JSCFunctionListEntry{"end", 0, JS_DEF_CFUNC, 0, {
            func : {0, JS_CFUNC_generic, espnow_end}
        }},
        JSCFunctionListEntry{"addPeer", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, espnow_add_peer}
        }},
        JSCFunctionListEntry{"delPeer", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, espnow_del_peer}
        }},
        JSCFunctionListEntry{"peerExists", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, espnow_peer_exists}
        }},
        JSCFunctionListEntry{"send", 0, JS_DEF_CFUNC, 0, {
            func : {2, JS_CFUNC_generic, espnow_send}
        }},
        JSCFunctionListEntry{"broadcast", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, espnow_broadcast}
        }},
        JSCFunctionListEntry{"onReceive", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, espnow_on_receive}
        }},
        JSCFunctionListEntry{"onSend", 0, JS_DEF_CFUNC, 0, {
            func : {1, JS_CFUNC_generic, espnow_on_send}
        }},
    };
    JS_SetPropertyFunctionList(ctx, en, en_funcs,
                               sizeof(en_funcs) / sizeof(JSCFunctionListEntry));
#endif

    static const JSCFunctionListEntry esp32_funcs[] = {
        JSCFunctionListEntry{"millis", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, esp32_millis}
                             }},
        JSCFunctionListEntry{"pinMode", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, esp32_gpio_mode}
                             }},
        JSCFunctionListEntry{
            "digitalRead", 0, JS_DEF_CFUNC, 0, {
              func : {1, JS_CFUNC_generic, esp32_gpio_digital_read}
            }},
        JSCFunctionListEntry{
            "digitalWrite", 0, JS_DEF_CFUNC, 0, {
              func : {2, JS_CFUNC_generic, esp32_gpio_digital_write}
            }},
        JSCFunctionListEntry{"readAnalog", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, esp32_read_analog}
                             }},
        JSCFunctionListEntry{"writeAnalog", 0, JS_DEF_CFUNC, 0, {
                               func : {4, JS_CFUNC_generic, esp32_write_analog}
                             }},
        JSCFunctionListEntry{
            "writeAnalogStop", 0, JS_DEF_CFUNC, 0, {
              func : {1, JS_CFUNC_generic, esp32_write_analog_stop}
            }},
        JSCFunctionListEntry{"touch", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, esp32_touch}
                             }},
        JSCFunctionListEntry{"analogRead", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, esp32_analog_read}
                             }},
        JSCFunctionListEntry{"analogWrite", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, esp32_analog_write}
                             }},
        JSCFunctionListEntry{"deepSleep", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, esp32_deep_sleep}
                             }},
        JSCFunctionListEntry{"setLoop", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, esp32_set_loop}
                             }},
    };

#ifndef GLOBAL_ESP32
    JSModuleDef *m =
        JS_NewCModule(ctx, "esp32", [](JSContext *ctx, JSModuleDef *m) {
          return JS_SetModuleExportList(
              ctx, m, esp32_funcs,
              sizeof(esp32_funcs) / sizeof(JSCFunctionListEntry));
        });
    if (m) {
      JS_AddModuleExportList(
          ctx, m, esp32_funcs,
          sizeof(esp32_funcs) / sizeof(JSCFunctionListEntry));
    }
#else
    // import * as esp32 from "esp32";
    JSValue esp32 = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "esp32", esp32);
    JS_SetPropertyFunctionList(
        ctx, esp32, esp32_funcs,
        sizeof(esp32_funcs) / sizeof(JSCFunctionListEntry));
#endif
  }

  static JSValue console_log(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    // Route output to the REPL that initiated this command (Serial or a
    // specific telnet client). Falls back to Serial if no REPL is active.
    Stream* out = activeOutputStream ? activeOutputStream : (Stream*)&Serial;
    for (int i = 0; i < argc; i++) {
      const char *str = JS_ToCString(ctx, argv[i]);
      if (str) {
        out->println(str);
        JS_FreeCString(ctx, str);
      }
    }
    return JS_UNDEFINED;
  }

  static JSValue set_timeout(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t t;
    JS_ToUint32(ctx, &t, argv[1]);
    // Optional 3rd arg: nohup flag. If true, the timer survives its
    // owner REPL disconnecting — useful for background polling tasks
    // that should keep running and dump their output to Serial even
    // after the user closes the telnet session. Missing arg
    // (argv[2] == JS_UNDEFINED) defaults to false.
    bool nohup = (argc >= 3) && JS_ToBool(ctx, argv[2]);
    // Capture the active output stream (the REPL that called
    // setTimeout) so console.log inside the callback routes back to
    // the same REPL, not always to Serial. May be nullptr if no REPL
    // is active. Also tag the entry with the owner telnet id so the
    // disconnect handler can clean up (or reroute, for nohup timers).
    Print* out = (Print*)activeOutputStream;
    int owner_id = currentTelnetId;
    uint32_t id = qjs->timer.RegisterTimer(JS_DupValue(ctx, argv[0]),
                                            millis() + t, -1, out, owner_id,
                                            nohup);
    return JS_NewUint32(ctx, id);
  }

  static JSValue clear_timeout(JSContext *ctx, JSValueConst jsThis, int argc,
                               JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t tid;
    JS_ToUint32(ctx, &tid, argv[0]);
    qjs->timer.RemoveTimer(tid);
    return JS_UNDEFINED;
  }

  static JSValue set_interval(JSContext *ctx, JSValueConst jsThis, int argc,
                              JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t t;
    JS_ToUint32(ctx, &t, argv[1]);
    bool nohup = (argc >= 3) && JS_ToBool(ctx, argv[2]);
    Print* out = (Print*)activeOutputStream;
    int owner_id = currentTelnetId;
    uint32_t id = qjs->timer.RegisterTimer(JS_DupValue(ctx, argv[0]),
                                            millis() + t, t, out, owner_id,
                                            nohup);
    return JS_NewUint32(ctx, id);
  }

  static JSValue esp32_millis(JSContext *ctx, JSValueConst jsThis, int argc,
                              JSValueConst *argv) {
    return JS_NewUint32(ctx, millis());
  }

  static JSValue esp32_gpio_mode(JSContext *ctx, JSValueConst jsThis, int argc,
                                 JSValueConst *argv) {
    uint32_t pin, mode;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &mode, argv[1]);
    pinMode(pin, mode);
    return JS_UNDEFINED;
  }

  static JSValue esp32_gpio_digital_read(JSContext *ctx, JSValueConst jsThis,
                                         int argc, JSValueConst *argv) {
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    return JS_NewUint32(ctx, digitalRead(pin));
  }

  static JSValue esp32_gpio_digital_write(JSContext *ctx, JSValueConst jsThis,
                                          int argc, JSValueConst *argv) {
    uint32_t pin, value;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &value, argv[1]);
    digitalWrite(pin, value);
    return JS_UNDEFINED;
  }

  // --- Analog I/O + touch. Trampolines pull qjs out of the context
  // opaque pointer and delegate to the JSAnalog member. ---
  static JSValue esp32_read_analog(JSContext *ctx, JSValueConst jsThis,
                                   int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->analog.js_readAnalog(ctx, argc, argv);
  }

  static JSValue esp32_write_analog(JSContext *ctx, JSValueConst jsThis,
                                    int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->analog.js_writeAnalog(ctx, argc, argv);
  }

  static JSValue esp32_write_analog_stop(JSContext *ctx, JSValueConst jsThis,
                                         int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->analog.js_writeAnalogStop(ctx, argc, argv);
  }

  static JSValue esp32_touch(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->analog.js_touch(ctx, argc, argv);
  }

  // analogRead(pin) — Arduino-style alias for readAnalog(pin, 12).
  // Returns a Promise<number> (the ADC reading).
  static JSValue esp32_analog_read(JSContext *ctx, JSValueConst jsThis,
                                   int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->analog.js_readAnalog(ctx, argc, argv);
  }

  // analogWrite(pin, fraction, [freq=5000])
  // Arduino-style API: fraction is 0.0..1.0. Internally maps to 8-bit
  // duty and forwards to writeAnalog. Returns the same Promise<{channel}>.
  static JSValue esp32_analog_write(JSContext *ctx, JSValueConst jsThis,
                                    int argc, JSValueConst *argv) {
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "analogWrite: need (pin, fraction, [freq])");
    }
    double frac = 0;
    JS_ToFloat64(ctx, &frac, argv[1]);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    uint32_t duty = (uint32_t)(frac * 255.0 + 0.5);
    uint32_t freq = (argc >= 3) ? 0 : 5000;
    if (argc >= 3) JS_ToUint32(ctx, &freq, argv[2]);
    // Build argv for js_writeAnalog: (pin, duty, freq, resolution)
    JSValue wargv[4] = {
      JS_DupValue(ctx, argv[0]),
      JS_NewUint32(ctx, duty),
      JS_NewUint32(ctx, freq),
      JS_NewUint32(ctx, 8),
    };
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue ret = qjs->analog.js_writeAnalog(ctx, 4, (JSValueConst*)wargv);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, wargv[i]);
    return ret;
  }

  static JSValue esp32_deep_sleep(JSContext *ctx, JSValueConst jsThis, int argc,
                                  JSValueConst *argv) {
    uint32_t t;
    JS_ToUint32(ctx, &t, argv[0]);
    ESP.deepSleep(t);  // never return.
    return JS_UNDEFINED;
  }

  static JSValue esp32_set_loop(JSContext *ctx, JSValueConst jsThis, int argc,
                                JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    qjs->setLoopFunc(JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
  }

#ifdef ENABLE_WIFI
  static JSValue wifi_is_connected(JSContext *ctx, JSValueConst jsThis,
                                   int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, WiFi.status() == WL_CONNECTED);
  }

  static JSValue wifi_connect(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    const char *ssid = JS_ToCString(ctx, argv[0]);
    const char *pass = JS_ToCString(ctx, argv[1]);
    if (!ssid || !pass) return JS_EXCEPTION;
    
    bool success = WiFi.begin(ssid, pass);
    
    JS_FreeCString(ctx, ssid);
    JS_FreeCString(ctx, pass);
    return JS_NewBool(ctx, success);
  }

  static JSValue wifi_reconnect(JSContext *ctx, JSValueConst jsThis,
                                 int argc, JSValueConst *argv) {
    WiFi.disconnect();
    WiFi.begin();
    return JS_UNDEFINED;
  }

  static JSValue wifi_start_ap(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    const char *ssid = JS_ToCString(ctx, argv[0]);
    if (!ssid) return JS_EXCEPTION;
    
    bool success = WiFi.softAP(ssid);
    JS_FreeCString(ctx, ssid);
    return JS_NewBool(ctx, success);
  }

  static JSValue wifi_stop_ap(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    WiFi.softAP(NULL);
    return JS_UNDEFINED;
  }

  static JSValue wifi_stop(JSContext *ctx, JSValueConst jsThis,
                           int argc, JSValueConst *argv) {
    WiFi.disconnect();
    return JS_UNDEFINED;
  }

  static JSValue wifi_ip(JSContext *ctx, JSValueConst jsThis,
                         int argc, JSValueConst *argv) {
    IPAddress ip = WiFi.localIP();
    char buf[16];
    sprintf(buf, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return JS_NewString(ctx, buf);
  }

  static JSValue wifi_serve(JSContext *ctx, JSValueConst jsThis, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    uint32_t port;
    JS_ToUint32(ctx, &port, argv[0]);
    JSValue callback = argv[1];
    if (!JS_IsFunction(ctx, callback)) return JS_EXCEPTION;

    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    qjs->webServer.serve(ctx, (uint16_t)port, callback);
    return JS_UNDEFINED;
  }

  static JSValue http_fetch(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->httpFetcher.fetch(ctx, argv[0], argv[1]);
  }
#endif

#ifdef ENABLE_FS
  // Auto-mount LittleFS at setup if not already mounted. Mirrors the wifi
  // "always present" pattern: a global is available, no JS call required to
  // mount it, and it just works the moment JS runs.
  bool ensureLittleFS() {
    if (littlefsMounted) return true;
    if (LittleFS.begin(true)) {
      littlefs.bind(&LittleFS);
      littlefsMounted = true;
      return true;
    }
    return false;
  }

  // Auto-mount SD lazily on first FS.SD.init() call.
  bool ensureSD(uint8_t csPin) {
    if (sdMounted) return true;
    if (SD.begin(csPin)) {
      sd.bind(&SD);
      sdMounted = true;
      return true;
    }
    return false;
  }

  // FS.LittleFS.readFile(path)
  static JSValue fs_lfs_read(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->ensureLittleFS()) return JS_EXCEPTION;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    JSValue p = qjs->littlefs.op(ctx, JSFileSystem::OP_READ, path, nullptr);
    JS_FreeCString(ctx, path);
    return p;
  }

  // FS.LittleFS.writeFile(path, content)
  static JSValue fs_lfs_write(JSContext *ctx, JSValueConst jsThis, int argc,
                              JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->ensureLittleFS()) return JS_EXCEPTION;
    const char *path = JS_ToCString(ctx, argv[0]);
    const char *content = JS_ToCString(ctx, argv[1]);
    if (!path || !content) {
      if (path) JS_FreeCString(ctx, path);
      if (content) JS_FreeCString(ctx, content);
      return JS_EXCEPTION;
    }
    JSValue p = qjs->littlefs.op(ctx, JSFileSystem::OP_WRITE, path, content);
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);
    return p;
  }

  // FS.LittleFS.removeFile(path)
  static JSValue fs_lfs_remove(JSContext *ctx, JSValueConst jsThis, int argc,
                               JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->ensureLittleFS()) return JS_EXCEPTION;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    JSValue p = qjs->littlefs.op(ctx, JSFileSystem::OP_REMOVE, path, nullptr);
    JS_FreeCString(ctx, path);
    return p;
  }

  // FS.LittleFS.listFiles(path?)
  static JSValue fs_lfs_list(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->ensureLittleFS()) return JS_EXCEPTION;
    const char *path = (argc > 0) ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (argc > 0 && !path) return JS_EXCEPTION;
    JSValue p =
        qjs->littlefs.op(ctx, JSFileSystem::OP_LIST, path ? path : "/", nullptr);
    if (path) JS_FreeCString(ctx, path);
    return p;
  }

  // FS.SD.init(csPin?)  Returns true on success, throws on failure.
  static JSValue fs_sd_init(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t csPin = 5;
    if (argc > 0) JS_ToUint32(ctx, &csPin, argv[0]);
    if (!qjs->ensureSD((uint8_t)csPin)) {
      return JS_ThrowReferenceError(ctx, "SD mount failed on csPin=%u", csPin);
    }
    return JS_NewBool(ctx, true);
  }

  static JSValue fs_sd_read(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->sdMounted) {
      return JS_ThrowReferenceError(ctx, "FS.SD.init(csPin) must be called first");
    }
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    JSValue p = qjs->sd.op(ctx, JSFileSystem::OP_READ, path, nullptr);
    JS_FreeCString(ctx, path);
    return p;
  }

  static JSValue fs_sd_write(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->sdMounted) {
      return JS_ThrowReferenceError(ctx, "FS.SD.init(csPin) must be called first");
    }
    const char *path = JS_ToCString(ctx, argv[0]);
    const char *content = JS_ToCString(ctx, argv[1]);
    if (!path || !content) {
      if (path) JS_FreeCString(ctx, path);
      if (content) JS_FreeCString(ctx, content);
      return JS_EXCEPTION;
    }
    JSValue p = qjs->sd.op(ctx, JSFileSystem::OP_WRITE, path, content);
    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);
    return p;
  }

  static JSValue fs_sd_remove(JSContext *ctx, JSValueConst jsThis, int argc,
                              JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->sdMounted) {
      return JS_ThrowReferenceError(ctx, "FS.SD.init(csPin) must be called first");
    }
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    JSValue p = qjs->sd.op(ctx, JSFileSystem::OP_REMOVE, path, nullptr);
    JS_FreeCString(ctx, path);
    return p;
  }

  static JSValue fs_sd_list(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs->sdMounted) {
      return JS_ThrowReferenceError(ctx, "FS.SD.init(csPin) must be called first");
    }
    const char *path = (argc > 0) ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (argc > 0 && !path) return JS_EXCEPTION;
    JSValue p =
        qjs->sd.op(ctx, JSFileSystem::OP_LIST, path ? path : "/", nullptr);
    if (path) JS_FreeCString(ctx, path);
    return p;
  }
#endif

#ifdef ENABLE_ESPNOW
  // ---- ESPNow JS bridge helpers (static) ----
  // Decode a MAC value coming from JS: either "AA:BB:CC:DD:EE:FF" or a
  // 6-element number array. Writes 6 bytes into out_mac. Returns true on
  // success. out_valid is set true if `out_mac` is meaningful (vs. NULL=bcast).
  static bool decodeMac(JSContext *ctx, JSValueConst v, uint8_t *out_mac,
                        bool *out_valid) {
    *out_valid = false;
    if (JS_IsString(v)) {
      const char *s = JS_ToCString(ctx, v);
      if (!s) return false;
      unsigned int m[6];
      int n = sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4],
                     &m[5]);
      JS_FreeCString(ctx, s);
      if (n != 6) return false;
      for (int i = 0; i < 6; i++) out_mac[i] = (uint8_t)m[i];
      *out_valid = true;
      return true;
    }
    if (JS_IsArray(ctx, v)) {
      JSValue len = JS_GetPropertyStr(ctx, v, "length");
      int64_t l;
      JS_ToInt64(ctx, &l, len);
      JS_FreeValue(ctx, len);
      if (l != 6) return false;
      for (int i = 0; i < 6; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, v, i);
        int32_t b;
        if (JS_ToInt32(ctx, &b, el) != 0) {
          JS_FreeValue(ctx, el);
          return false;
        }
        JS_FreeValue(ctx, el);
        out_mac[i] = (uint8_t)b;
      }
      *out_valid = true;
      return true;
    }
    return false;
  }

  // Decode a payload: either a JS string (raw bytes) or a number array.
  // Returns bytes via std::vector; empty vector on error.
  static std::vector<uint8_t> decodePayload(JSContext *ctx, JSValueConst v) {
    std::vector<uint8_t> out;
    if (JS_IsString(v)) {
      const char *s = JS_ToCString(ctx, v);
      if (!s) return out;
      size_t n = strlen(s);
      if (n > ESP_NOW_MAX_DATA_LEN) n = ESP_NOW_MAX_DATA_LEN;
      out.assign((const uint8_t *)s, (const uint8_t *)s + n);
      JS_FreeCString(ctx, s);
      return out;
    }
    if (JS_IsArray(ctx, v)) {
      JSValue len = JS_GetPropertyStr(ctx, v, "length");
      int64_t l;
      JS_ToInt64(ctx, &l, len);
      JS_FreeValue(ctx, len);
      if (l < 0 || l > (int)ESP_NOW_MAX_DATA_LEN) return out;
      out.reserve((size_t)l);
      for (int64_t i = 0; i < l; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int32_t b;
        if (JS_ToInt32(ctx, &b, el) != 0) {
          JS_FreeValue(ctx, el);
          out.clear();
          return out;
        }
        JS_FreeValue(ctx, el);
        out.push_back((uint8_t)b);
      }
      return out;
    }
    return out;
  }

  static JSValue espnow_init(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return JS_NewBool(ctx, qjs->espNow.init());
  }

  static JSValue espnow_end(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    qjs->espNow.end();
    return JS_UNDEFINED;
  }

  // ESPNow.addPeer(mac, [opts]) where opts = { channel, encrypt, lmk, iface }
  static JSValue espnow_add_peer(JSContext *ctx, JSValueConst jsThis, int argc,
                                 JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint8_t mac[6];
    bool valid;
    if (!decodeMac(ctx, argv[0], mac, &valid) || !valid) {
      return JS_ThrowTypeError(ctx, "addPeer: invalid MAC address");
    }
    uint8_t channel = 0;  // 0 = use current WiFi channel
    bool encrypt = false;
    uint8_t lmk[ESP_NOW_KEY_LEN] = {0};
    wifi_interface_t iface = WIFI_IF_STA;
    if (argc > 1 && JS_IsObject(argv[1])) {
      JSValue ch = JS_GetPropertyStr(ctx, argv[1], "channel");
      if (JS_IsNumber(ch)) {
        int32_t v;
        JS_ToInt32(ctx, &v, ch);
        channel = (uint8_t)v;
      }
      JS_FreeValue(ctx, ch);
      JSValue en = JS_GetPropertyStr(ctx, argv[1], "encrypt");
      if (JS_IsBool(en)) encrypt = JS_ToBool(ctx, en);
      JS_FreeValue(ctx, en);
      JSValue ke = JS_GetPropertyStr(ctx, argv[1], "lmk");
      if (JS_IsArray(ctx, ke) || JS_IsString(ke)) {
        std::vector<uint8_t> k = decodePayload(ctx, ke);
        if (k.size() == ESP_NOW_KEY_LEN) {
          memcpy(lmk, k.data(), ESP_NOW_KEY_LEN);
          encrypt = true;
        }
      }
      JS_FreeValue(ctx, ke);
      JSValue ifc = JS_GetPropertyStr(ctx, argv[1], "iface");
      if (JS_IsString(ifc)) {
        const char *s = JS_ToCString(ctx, ifc);
        if (s) {
          if (strcmp(s, "ap") == 0 || strcmp(s, "AP") == 0)
            iface = WIFI_IF_AP;
          else
            iface = WIFI_IF_STA;
          JS_FreeCString(ctx, s);
        }
      }
      JS_FreeValue(ctx, ifc);
    }
    bool ok = qjs->espNow.addPeer(mac, channel, encrypt,
                                  encrypt ? lmk : nullptr, iface);
    return JS_NewBool(ctx, ok);
  }

  static JSValue espnow_del_peer(JSContext *ctx, JSValueConst jsThis, int argc,
                                 JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint8_t mac[6];
    bool valid;
    if (!decodeMac(ctx, argv[0], mac, &valid) || !valid) {
      return JS_ThrowTypeError(ctx, "delPeer: invalid MAC address");
    }
    return JS_NewBool(ctx, qjs->espNow.delPeer(mac));
  }

  static JSValue espnow_peer_exists(JSContext *ctx, JSValueConst jsThis,
                                    int argc, JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint8_t mac[6];
    bool valid;
    if (!decodeMac(ctx, argv[0], mac, &valid) || !valid) {
      return JS_ThrowTypeError(ctx, "peerExists: invalid MAC address");
    }
    return JS_NewBool(ctx, qjs->espNow.peerExists(mac));
  }

  // Common send path used by send/broadcast. Returns a Promise.
  static JSValue espnow_do_send(JSContext *ctx, const uint8_t *mac, // nullptr for broadcast
                                JSValueConst payload) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    std::vector<uint8_t> bytes = decodePayload(ctx, payload);
    if (bytes.empty()) {
      return JS_ThrowTypeError(ctx, "send: payload must be non-empty string or array");
    }
    if (bytes.size() > (size_t)ESP_NOW_MAX_DATA_LEN) {
      return JS_ThrowRangeError(ctx, "send: payload exceeds ESP_NOW_MAX_DATA_LEN (%d)",
                                ESP_NOW_MAX_DATA_LEN);
    }
    uint32_t id = qjs->espNow.send(mac, bytes.data(), bytes.size());
    if (id == 0) {
      return JS_ThrowInternalError(ctx, "send: enqueue failed (init? channel? peer?)");
    }
    return qjs->espNow.attachPromiseToSend(id);
  }

  static JSValue espnow_send(JSContext *ctx, JSValueConst jsThis, int argc,
                             JSValueConst *argv) {
    if (argc < 2) return JS_EXCEPTION;
    uint8_t mac[6];
    bool valid;
    if (!decodeMac(ctx, argv[0], mac, &valid) || !valid) {
      return JS_ThrowTypeError(ctx, "send: invalid MAC address");
    }
    return espnow_do_send(ctx, mac, argv[1]);
  }

  static JSValue espnow_broadcast(JSContext *ctx, JSValueConst jsThis, int argc,
                                  JSValueConst *argv) {
    if (argc < 1) return JS_EXCEPTION;
    return espnow_do_send(ctx, nullptr, argv[0]);
  }

  static JSValue espnow_on_receive(JSContext *ctx, JSValueConst jsThis, int argc,
                                   JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_EXCEPTION;
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    qjs->espNow.setReceiveHandler(ctx, argv[0]);
    return JS_UNDEFINED;
  }

  static JSValue espnow_on_send(JSContext *ctx, JSValueConst jsThis, int argc,
                                JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_EXCEPTION;
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    qjs->espNow.setSendHandler(ctx, argv[0]);
    return JS_UNDEFINED;
  }
#endif
};

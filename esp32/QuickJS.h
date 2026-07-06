#pragma once


// ENABLE_FS is opt-in: define it from build_flags or before including QuickJS.h.
// LittleFS / SD are always available on Arduino-ESP32, but we keep the gate to
// stay consistent with ENABLE_WIFI.

// ENABLE_ESPNOW: there is no Arduino-style header to test against, so we
// gate on GLOBAL_ESP32 the same way ENABLE_FS does. Users can still force it
// with -DENABLE_ESPNOW.

// Forward declaration of the main embed class. JSAnalog (defined
// above ESP32QuickJS in this file) needs to look up the qjs
// instance to fire its dispose() callback; the forward decl lets us
// reference ESP32QuickJS* without a circular include.
class ESP32QuickJS;

#include <Arduino.h>

#include "RotaryEncoder.h"
#include "JSRotaryEncoder.h"

#include "MotorDriver.h"
#include "JSMotorDriver.h"

#include <algorithm>
#include <vector>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <OneWire.h>
#include <driver/uart.h>

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
#include <map>
#include <cstdio>
#include <cstdint>
#include <cstring>

// FreeRTOS headers for the non-blocking module loader task.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <HTTPClient.h>
#include <Server.h>
#include <StreamString.h>

#include <driver/i2c.h>

#include <driver/spi_common.h>
#include <driver/spi_master.h>
#include <SPI.h>  // for SPISettings, used as a convenience

// JSServo uses the ESP32 LEDC peripheral directly (50 Hz, 14-bit
// resolution, 1000..2000 µs pulse range). No external Servo library
// is needed — this keeps LEDC channel allocation centralized in
// JSAnalog so analogWrite() and servo.attach() can never collide on
// the same channel. Gated on the same auto-enable as JSAnalog
// (GLOBAL_ESP32 by default).

#include <LittleFS.h>
#include <SD.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>

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

// ---- Centralized background worker ----
// One FreeRTOS task, one mutex, one binary semaphore. Multiple modules
// (I2C, SPI, FileSystem) submit work items via submit(). Each item carries
// a tag identifying the owner and a function pointer to the processing
// callback. The worker task pops items, calls the callback, and marks
// them done. Each module's loop() polls its own doneQueue_.
//
// This replaces the per-module task approach which created 3-4 separate
// tasks (each 4KB stack = 12-16KB heap) and caused HTTPS to fail due to
// insufficient contiguous heap for SSL BIGNUM allocation.
class JSWorker {
 public:
  typedef void (*ProcessFn)(void *entry);

  struct WorkItem {
    void *entry;        // module-specific Entry* (opaque to JSWorker)
    ProcessFn fn;       // module's processEntry function
    volatile bool done; // set by worker after fn returns
  };

 private:
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t notify_ = nullptr;
  volatile bool stopping_ = false;
  std::vector<WorkItem *> workQueue_;  // pending items (worker pops)

  static void taskEntry(void *arg) {
    static_cast<JSWorker *>(arg)->workerTask();
    vTaskDelete(nullptr);
  }

  void workerTask() {
    while (true) {
      xSemaphoreTake(notify_, portMAX_DELAY);
      if (stopping_) break;
      std::vector<WorkItem *> work;
      xSemaphoreTake(mutex_, portMAX_DELAY);
      work.swap(workQueue_);
      xSemaphoreGive(mutex_);
      for (auto *w : work) {
        w->fn(w->entry);  // module's processEntry fills in results
        w->done = true;
      }
    }
  }

 public:
  void init() {
    if (task_) return;
    mutex_ = xSemaphoreCreateMutex();
    notify_ = xSemaphoreCreateBinary();
    // 8KB stack — enough for I2C, SPI, FS, and HTTP operations.
    xTaskCreatePinnedToCore(taskEntry, "js_worker", 8192, this, 1, &task_, 0);
  }

  // Submit a work item. The module owns the Entry; JSWorker owns the
  // WorkItem (freed after done is set). The module's loop() polls
  // item->done and then resolves the JS Promise.
  void submit(WorkItem *item) {
    item->done = false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    workQueue_.push_back(item);
    xSemaphoreGive(mutex_);
    xSemaphoreGive(notify_);
  }

  bool ready() const { return task_ != nullptr; }

  void end() {
    stopping_ = true;
    if (notify_) xSemaphoreGive(notify_);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
    if (notify_) { vSemaphoreDelete(notify_); notify_ = nullptr; }
    stopping_ = false;
  }

  ~JSWorker() { end(); }

  // Singleton instance — set by ESP32QuickJS::begin().
  static JSWorker *instance;
};
inline JSWorker* JSWorker::instance = nullptr;

// Non-blocking HTTP fetcher. All blocking I/O (HTTPClient::sendRequest/GET,
// reading the response body) happens on a FreeRTOS task so the JS main loop
// never stalls. The main loop just polls a "done" flag and resolves the
// JS Promise when the background task has the full response.
class JSHttpFetcher {
 public:
  struct Entry {
    JSContext *ctx;
    JSValue resolving_funcs[2];
    // Input (set by JS thread before queuing):
    std::string url;
    std::string method;
    std::string body;
    // Output (written by task under mutex, read by JS thread):
    volatile bool done;
    volatile bool ok;       // true = success, false = error
    int status;
    std::string responseHeaders;
    std::string responseBody;
    std::string errorMsg;
  };

 private:
  std::vector<Entry *> queue;          // all pending entries (JS thread)
  std::vector<Entry *> workQueue_;     // entries waiting for the task
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t notify_ = nullptr;
  volatile bool stopping_ = false;

  static void taskEntry(void *arg) {
    static_cast<JSHttpFetcher *>(arg)->fetchTask();
    vTaskDelete(nullptr);
  }

  void fetchTask() {
    while (true) {
      xSemaphoreTake(notify_, portMAX_DELAY);
      if (stopping_) break;
      std::vector<Entry *> work;
      xSemaphoreTake(mutex_, portMAX_DELAY);
      work.swap(workQueue_);
      xSemaphoreGive(mutex_);
      for (auto *e : work) doFetch(e);
    }
  }

  void doFetch(Entry *e) {
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    if (!http.begin(e->url.c_str())) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      e->ok = false;
      e->errorMsg = "begin() failed";
      e->done = true;
      xSemaphoreGive(mutex_);
      return;
    }
    int code;
    if (!e->method.empty() && e->method != "GET") {
      code = http.sendRequest(e->method.c_str(),
                             (uint8_t *)e->body.c_str(),
                             e->body.length());
    } else {
      code = http.GET();
    }
    if (code <= 0) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      e->ok = false;
      e->errorMsg = "HTTP error: " + std::to_string(code);
      e->done = true;
      xSemaphoreGive(mutex_);
      http.end();
      return;
    }
    String bodyStr = http.getString();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    e->ok = true;
    e->status = code;
    e->responseBody = std::string(bodyStr.c_str());
    e->done = true;
    xSemaphoreGive(mutex_);
    http.end();
  }

 public:
  JSHttpFetcher() {}

  void init() {
    if (task_) return;
    mutex_ = xSemaphoreCreateMutex();
    notify_ = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(taskEntry, "js_http", 8192, this, 1, &task_, 0);
  }

  ~JSHttpFetcher() {
    stopping_ = true;
    if (notify_) xSemaphoreGive(notify_);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (task_) { vTaskDelete(task_); task_ = nullptr; }
    if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
    if (notify_) { vSemaphoreDelete(notify_); notify_ = nullptr; }
    for (auto *e : queue) {
      JS_FreeValue(e->ctx, e->resolving_funcs[0]);
      JS_FreeValue(e->ctx, e->resolving_funcs[1]);
      delete e;
    }
    queue.clear();
  }

  JSValue fetch(JSContext *ctx, JSValueConst jsUrl, JSValueConst options) {
    if (WiFi.status() != WL_CONNECTED) {
      return JS_ThrowTypeError(ctx, "WiFi not connected");
    }
    const char *url = JS_ToCString(ctx, jsUrl);
    if (!url) return JS_EXCEPTION;

    std::string method = "GET";
    std::string body = "";
    if (JS_IsObject(options)) {
      JSValue m = JS_GetPropertyStr(ctx, options, "method");
      if (JS_IsString(m)) {
        const char *s = JS_ToCString(ctx, m);
        if (s) { method = s; JS_FreeCString(ctx, s); }
      }
      JS_FreeValue(ctx, m);
      JSValue b = JS_GetPropertyStr(ctx, options, "body");
      if (JS_IsString(b)) {
        const char *s = JS_ToCString(ctx, b);
        if (s) { body = s; JS_FreeCString(ctx, s); }
      }
      JS_FreeValue(ctx, b);
    }

    Entry *e = new Entry();
    e->ctx = ctx;
    e->url = url;
    e->method = method;
    e->body = body;
    e->done = false;
    e->ok = false;
    e->status = 0;
    e->resolving_funcs[0] = JS_UNDEFINED;
    e->resolving_funcs[1] = JS_UNDEFINED;
    JS_FreeCString(ctx, url);

    JSValue promise = JS_NewPromiseCapability(ctx, e->resolving_funcs);
    if (JS_IsException(promise)) {
      delete e;
      return JS_EXCEPTION;
    }

    // Queue for the background task.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    queue.push_back(e);
    workQueue_.push_back(e);
    xSemaphoreGive(mutex_);
    xSemaphoreGive(notify_);

    return promise;
  }

  void loop(JSContext *ctx) {
    std::vector<Entry*> doneEntries;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (auto it = queue.begin(); it != queue.end(); ) {
      if ((*it)->done) {
        doneEntries.push_back(*it);
        it = queue.erase(it);
      } else {
        ++it;
      }
    }
    xSemaphoreGive(mutex_);

    for (auto e : doneEntries) {
      bool ok = e->ok;
      int status = e->status;
      std::string body = e->responseBody;
      std::string err = e->errorMsg;
      JSValue rfs[2] = {e->resolving_funcs[0], e->resolving_funcs[1]};
      JSContext *ectx = e->ctx;

      if (ok) {
        JSValue r = JS_NewObject(ectx);
        JS_SetPropertyStr(ectx, r, "status", JS_NewInt32(ectx, status));
        JS_SetPropertyStr(ectx, r, "body", JS_NewString(ectx, body.c_str()));
        JS_Call(ectx, rfs[0], JS_UNDEFINED, 1, &r);
        JS_FreeValue(ectx, r);
      } else {
        JSValue errv = JS_NewString(ectx, err.c_str());
        JS_Call(ectx, rfs[1], JS_UNDEFINED, 1, &errv);
        JS_FreeValue(ectx, errv);
      }
      JS_FreeValue(ectx, rfs[0]);
      JS_FreeValue(ectx, rfs[1]);
      delete e;
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
  friend class MotorDriver;
  friend class JSServo;
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
  // Separate bitmap for channels reserved by servos (via ServoEasing).
  // These channels are allocated by ESP32PWM independently, so we
  // track them here to prevent JSAnalog::allocChannel() from
  // handing them out to analogWrite().
  uint32_t servoChannelBitmap = 0;

  // Reserve a channel that was allocated by ServoEasing/ESP32PWM.
  // Called from JSServo::loop() OP_ATTACH after successful attach().
  void reserveServoChannel(uint8_t ch) {
    if (ch >= 1 && ch < LEDC_CHANNELS) servoChannelBitmap |= (1U << ch);
  }
  // Allocate a fresh LEDC channel and mark it as servo-reserved.
  // Returns the channel number (1..15) on success, or 0 if no
  // channel is available. Called from JSServo::loop() OP_ATTACH.
  uint8_t reserveServoChannel() {
    int ch = allocChannel();
    if (ch <= 0) return 0;
    servoChannelBitmap |= (1U << ch);
    return (uint8_t)ch;
  }
  // Release a servo-reserved channel. Called from JSServo::loop()
  // OP_DETACH and JSServo::end().
  void releaseServoChannel(uint8_t ch) {
    if (ch >= 1 && ch < LEDC_CHANNELS) {
      servoChannelBitmap &= ~(1U << ch);
      channelBitmap &= ~(1U << ch);
    }
  }

  int allocChannel() {
    for (int i = 1; i < LEDC_CHANNELS; i++) {
      // Skip channels reserved by servos.
      if (servoChannelBitmap & (1U << i)) continue;
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
  uint8_t getPinChannel(uint8_t pin) const {
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

    touchDetachInterrupt(tl.pin);
    
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

  // True if the pin is currently being driven by LEDC PWM (i.e. an
  // analogWrite is active on it). Used by esp32.digitalRead /
  // esp32.analogRead to throw a clear error rather than silently
  // returning junk. Note: this does NOT include MCPWM servo pins
  // — those are tracked separately by JSServo::isPinBusy and the
  // call sites combine both checks.
  bool isPinBusy(uint8_t pin) const {
    return getPinChannel(pin) != PIN_UNUSED;
  }

  // Public helper for the analogWrite 0%/100% edge case. If the pin
  // is currently being driven by LEDC, detach it, free the channel,
  // and clear the pin map. Safe to call on a pin that isn't a PWM
  // pin (it just no-ops). Used by esp32_analog_write and
  // JSServo::js_attach to free up a pin for digital use.
  void detachPwmIfAttached(uint8_t pin) {
    uint8_t ch = getPinChannel(pin);
    if (ch == PIN_UNUSED || ch == PIN_INVALID) return;
    ledcDetachPin(pin);
    freeChannel((int)ch);
    setPinChannel(pin, PIN_UNUSED);
  }

  // Called from qjs.end() to free everything.
  void end(JSContext* ctx) {
    for (int i = 1; i <= MAX_TOUCH; i++) {
      if (listeners[i].active) {
        removeTouch(ctx, i);
      }
    }
    // Free all allocated LEDC channels and detach pins.
    for (int i = 0; i < 40; i++) {
      uint8_t ch = pinChannel[i];
      if (ch != PIN_UNUSED && ch != PIN_INVALID) {
        ledcDetachPin((uint8_t)i);
        freeChannel(ch);
        pinChannel[i] = PIN_UNUSED;
      }
    }
    // Also clear servo channel bitmap on full shutdown.
    servoChannelBitmap = 0;
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
        // Check if this pin already has a channel allocated - reuse it
        // instead of allocating a new one. This prevents channel leaks
        // when analogWrite() is called repeatedly on the same pin.
        int ch = getPinChannel(e.pin);
        if (ch == PIN_UNUSED || ch == PIN_INVALID) {
          ch = allocChannel();
        }
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
inline JSAnalog* JSAnalog::instance = nullptr;

// Promise-based I2C bus wrapper.
//
// Usage from JS:
//   let bus = await I2C.open({ sda: 21, scl: 22, freq: 100000 });
//   await bus.write(0x68, [0x6B, 0x00]);                 // start reg 0x6B, then data
//   let s = await bus.read(0x68, 6);                      // default: JS string
//   let a = await bus.read(0x68, 6, "array");             // number[] array
//   let h = await bus.read(0x68, 6, "hex");               // hex string
//   let s2 = await bus.writeRead(0x68, [0x6B], 6);        // reg + 6 bytes
//   bus.close();
//
// Uses ESP-IDF's legacy i2c driver (i2c_driver_install + i2c_master_*_device).
// Operations are queued and processed from ESP32QuickJS::loop() so the JS
// engine never blocks.
//
// Note: only I2C_NUM_0 is supported here. ESP32 has I2C_NUM_0 and I2C_NUM_1;
// I2C_NUM_0 is the default and works on any board. If you really need
// I2C_NUM_1, extend Bus with a port field and pass it through.
class JSI2C {
 public:
  static constexpr const char* TAG = "JSI2C";

  enum OpKind { OP_WRITE, OP_READ, OP_WRITE_READ, OP_OPEN, OP_CLOSE };

  struct Entry {
    OpKind kind;
    int addr;
    std::vector<uint8_t> wdata;
    int rlen;
    int mode;  // 0=string, 1=array, 2=hex
    // OP_OPEN-only: parameters captured at call time so the actual driver
    // install runs from the worker (non-blocking wrt the JS engine).
    int sda, scl;
    uint32_t freq;
    int busHandle;   // set by worker for OP_OPEN result
    int busIdx;      // bus index for OP_CLOSE
    JSValue resolving_funcs[2];
    // Output (written by worker, read by JS thread):
    volatile bool done;
    volatile bool ok;       // true = success, false = error
    esp_err_t result;
    std::vector<uint8_t> rx;
    // JSWorker linkage:
    JSWorker::WorkItem *workItem;  // owned by JSWorker, freed after done
  };

  struct Bus {
    i2c_port_t port = I2C_NUM_0;
    int sda = -1, scl = -1;
    uint32_t freq = 100000;
    bool installed = false;
  };

  JSI2C() = default;

  // ---- Uses the shared JSWorker (no per-module task) ----
 private:
  std::vector<Entry*> queue;       // all pending entries (JS thread tracks)

  // Static processEntry wrapper for JSWorker callback.
  static void processEntryCb(void *arg) {
    Entry *e = static_cast<Entry *>(arg);
    instance->processEntry(e);
  }

  // Runs on the shared worker thread. Does all blocking I2C calls and
  // writes results back into the Entry. No mutex needed — the JS thread
  // only reads volatile done flag via workItem. The worker sets
  // workItem->done = true after this returns.
  void processEntry(Entry *e) {
    if (e->kind == OP_OPEN) {
      int existing = -1;
      for (size_t i = 0; i < buses.size(); i++) {
        Bus &b = buses[i];
        if (b.installed && b.port == I2C_NUM_0 && b.sda == e->sda &&
            b.scl == e->scl && b.freq == e->freq) {
          existing = (int)i;
          break;
        }
      }
      if (existing >= 0) {
        e->ok = true;
        e->busHandle = existing;
        e->result = ESP_OK;
        return;
      }
      esp_err_t r1 = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
      if (r1 != ESP_OK) {
        e->ok = false;
        e->result = r1;
        return;
      }
      esp_err_t r2 = i2c_set_pin(I2C_NUM_0, e->sda, e->scl,
                                  GPIO_PULLUP_ENABLE, GPIO_PULLUP_ENABLE,
                                  I2C_MODE_MASTER);
      if (r2 != ESP_OK) {
        i2c_driver_delete(I2C_NUM_0);
        e->ok = false;
        e->result = r2;
        return;
      }
      Bus bus;
      bus.port = I2C_NUM_0;
      bus.sda = e->sda;
      bus.scl = e->scl;
      bus.freq = e->freq;
      bus.installed = true;
      buses.push_back(bus);
      e->ok = true;
      e->busHandle = (int)(buses.size() - 1);
      e->result = ESP_OK;
      return;
    }

    if (e->kind == OP_CLOSE) {
      if (e->busIdx >= 0 && e->busIdx < (int)buses.size() &&
          buses[e->busIdx].installed) {
        e->result = i2c_driver_delete(buses[e->busIdx].port);
        buses[e->busIdx].installed = false;
        e->ok = (e->result == ESP_OK);
      } else {
        e->ok = false;
        e->result = ESP_ERR_INVALID_ARG;
      }
      return;
    }

    Bus *b = nullptr;
    for (auto &bus : buses) {
      if (bus.installed) { b = &bus; break; }
    }
    if (!b) {
      e->ok = false;
      e->result = ESP_ERR_NOT_FOUND;
      return;
    }

    esp_err_t res = ESP_FAIL;
    std::vector<uint8_t> rx;
    if (e->kind == OP_WRITE) {
      res = i2c_master_write_to_device(b->port, (uint8_t)e->addr,
                                        e->wdata.data(), e->wdata.size(),
                                        pdMS_TO_TICKS(1000));
    } else if (e->kind == OP_READ) {
      rx.resize(e->rlen);
      res = i2c_master_read_from_device(b->port, (uint8_t)e->addr,
                                        rx.data(), e->rlen,
                                        pdMS_TO_TICKS(1000));
    } else {  // OP_WRITE_READ
      rx.resize(e->rlen);
      res = i2c_master_write_read_device(b->port, (uint8_t)e->addr,
                                         e->wdata.data(), e->wdata.size(),
                                         rx.data(), e->rlen,
                                         pdMS_TO_TICKS(1000));
    }
    e->result = res;
    e->ok = (res == ESP_OK);
    if (res == ESP_OK) e->rx = std::move(rx);
  }

 public:
  // No per-module init — uses the shared JSWorker.
  void init() {}

  // Scan queue for completed entries (workItem->done) and resolve Promises.
  bool loop(JSContext* ctx) {
    bool any = false;
    auto it = queue.begin();
    while (it != queue.end()) {
      Entry *e = *it;
      if (!e->workItem || !e->workItem->done) { ++it; continue; }

      bool ok = e->ok;
      esp_err_t res = e->result;
      int mode = e->mode;
      OpKind kind = e->kind;
      int busHandle = e->busHandle;
      std::vector<uint8_t> rx = std::move(e->rx);
      JSValue rfs[2] = {e->resolving_funcs[0], e->resolving_funcs[1]};
      JSContext *ectx = ctx;
      it = queue.erase(it);
      any = true;

      if (ok) {
        JSValue r;
        if (kind == OP_OPEN) {
          r = JS_NewInt32(ectx, busHandle);
        } else if (kind == OP_CLOSE) {
          r = JS_UNDEFINED;
        } else if (kind == OP_WRITE) {
          r = JS_NewBool(ectx, true);
        } else {
          r = formatResult(ectx, mode, rx);
        }
        JS_Call(ectx, rfs[0], JS_UNDEFINED, 1, &r);
        JS_FreeValue(ectx, r);
      } else {
        char errBuf[64];
        if (kind == OP_OPEN) {
          snprintf(errBuf, sizeof(errBuf),
                   "I2C: install/set_pin failed (0x%x)", (int)res);
        } else if (kind == OP_CLOSE) {
          snprintf(errBuf, sizeof(errBuf),
                   "I2C: close failed (0x%x)", (int)res);
        } else if (res == ESP_ERR_NOT_FOUND) {
          snprintf(errBuf, sizeof(errBuf), "I2C: no installed bus");
        } else {
          snprintf(errBuf, sizeof(errBuf), "I2C error: 0x%x (%d)",
                   (int)res, (int)res);
        }
        JSValue err = JS_NewString(ectx, errBuf);
        JS_Call(ectx, rfs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ectx, err);
      }
      JS_FreeValue(ectx, rfs[0]);
      JS_FreeValue(ectx, rfs[1]);
      delete e->workItem;
      delete e;
    }
    return any;
  }

  void end(JSContext* ctx) {
    // Reject any still-pending entries.
    for (auto *e : queue) {
      JSValue r = JS_UNDEFINED;
      JS_Call(ctx, e->resolving_funcs[1], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, e->resolving_funcs[0]);
      JS_FreeValue(ctx, e->resolving_funcs[1]);
      if (e->workItem) delete e->workItem;
      delete e;
    }
    queue.clear();

    for (auto& b : buses) {
      if (b.installed) i2c_driver_delete(b.port);
      b.installed = false;
    }
    buses.clear();
  }

  // ---- static JS trampolines (called from the C function list) ----
  // We resolve `self` via JSI2C::instance, which ESP32QuickJS::begin()
  // sets to `&this->i2c` after constructing the embed. This avoids the
  // incomplete-type problem we'd get if these methods accessed
  // `qjs->i2c` directly (ESP32QuickJS is only forward-declared here).
  static JSI2C* instance;

  // I2C.open({ sda, scl, freq }) -> Promise<busHandle>
  // Non-blocking: the actual i2c_driver_install happens in JSI2C::loop()
  // on the next tick, so this never blocks the JS engine.
  static JSValue js_open(JSContext* ctx, JSValueConst this_val, int argc,
                         JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "I2C.open: not initialized");
    if (argc < 1 || !JS_IsObject(argv[0])) {
      return JS_ThrowTypeError(ctx, "I2C.open: expected options object");
    }
    int sda = 21, scl = 22;
    uint32_t freq = 100000;
    int32_t tmp;
    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "sda");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) sda = tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "scl");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) scl = tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "freq");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) freq = (uint32_t)tmp;
    JS_FreeValue(ctx, v);

    if (sda < 0 || scl < 0) {
      return JS_ThrowRangeError(ctx, "I2C.open: sda and scl must be >= 0");
    }

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_OPEN;
    e.sda = sda;
    e.scl = scl;
    e.freq = freq;
    e.busHandle = -1;
    e.busIdx = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSI2C::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // I2C.close(busHandle) -> Promise<void>
  static JSValue js_close(JSContext* ctx, JSValueConst this_val, int argc,
                          JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "I2C.close: not initialized");
    if (argc < 1 || !JS_IsNumber(argv[0])) {
      return JS_ThrowTypeError(ctx, "I2C.close: expected bus handle");
    }
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_CLOSE;
    e.busIdx = (int)idx;
    e.busHandle = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSI2C::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
  }

  // I2C.write(busHandle, addr, data) -> Promise<void>
  static JSValue js_write(JSContext* ctx, JSValueConst this_val, int argc,
                          JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "I2C.write: not initialized");
    if (argc < 3) {
      return JS_ThrowTypeError(ctx, "I2C.write: need (busHandle, addr, data)");
    }
    int32_t idx = 0, addr = -1;
    JS_ToInt32(ctx, &idx, argv[0]);
    JS_ToInt32(ctx, &addr, argv[1]);
    if (idx < 0 || idx >= (int)instance->buses.size() ||
        !instance->buses[idx].installed) {
      return JS_ThrowReferenceError(ctx, "I2C.write: invalid bus handle");
    }
    if (addr < 0 || addr > 0x7F) {
      return JS_ThrowRangeError(ctx, "I2C.write: addr out of range 0..127");
    }
    std::vector<uint8_t> data;
    if (!jsToBytes(ctx, argv[2], &data)) {
      return JS_ThrowTypeError(ctx, "I2C.write: data must be string or number array");
    }
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_WRITE;
    e.addr = (int)addr;
    e.wdata = std::move(data);
    e.rlen = 0;
    e.mode = 0;
    e.busHandle = -1;
    e.busIdx = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSI2C::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // I2C.read(busHandle, addr, len, [mode]) -> Promise<bytes>
  static JSValue js_read(JSContext* ctx, JSValueConst this_val, int argc,
                         JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "I2C.read: not initialized");
    if (argc < 3) {
      return JS_ThrowTypeError(ctx, "I2C.read: need (busHandle, addr, len, [mode])");
    }
    int32_t idx = 0, addr = -1, len = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JS_ToInt32(ctx, &addr, argv[1]);
    JS_ToInt32(ctx, &len, argv[2]);
    if (idx < 0 || idx >= (int)instance->buses.size() ||
        !instance->buses[idx].installed) {
      return JS_ThrowReferenceError(ctx, "I2C.read: invalid bus handle");
    }
    if (addr < 0 || addr > 0x7F) {
      return JS_ThrowRangeError(ctx, "I2C.read: addr out of range 0..127");
    }
    if (len <= 0 || len > 1024) {
      return JS_ThrowRangeError(ctx, "I2C.read: len must be 1..1024");
    }
    int mode = 0;
    if (argc >= 4 && JS_IsString(argv[3])) {
      const char* m = JS_ToCString(ctx, argv[3]);
      if (m) {
        if (strcmp(m, "array") == 0) mode = 1;
        else if (strcmp(m, "hex") == 0) mode = 2;
        JS_FreeCString(ctx, m);
      }
    }
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_READ;
    e.addr = (int)addr;
    e.rlen = (int)len;
    e.mode = mode;
    e.busHandle = -1;
    e.busIdx = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSI2C::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // I2C.writeRead(busHandle, addr, data, len, [mode]) -> Promise<bytes>
  static JSValue js_write_read(JSContext* ctx, JSValueConst this_val, int argc,
                               JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "I2C.writeRead: not initialized");
    if (argc < 4) {
      return JS_ThrowTypeError(ctx, "I2C.writeRead: need (busHandle, addr, data, len, [mode])");
    }
    int32_t idx = 0, addr = -1, len = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JS_ToInt32(ctx, &addr, argv[1]);
    JS_ToInt32(ctx, &len, argv[3]);
    if (idx < 0 || idx >= (int)instance->buses.size() ||
        !instance->buses[idx].installed) {
      return JS_ThrowReferenceError(ctx, "I2C.writeRead: invalid bus handle");
    }
    if (addr < 0 || addr > 0x7F) {
      return JS_ThrowRangeError(ctx, "I2C.writeRead: addr out of range 0..127");
    }
    if (len <= 0 || len > 1024) {
      return JS_ThrowRangeError(ctx, "I2C.writeRead: len must be 1..1024");
    }
    std::vector<uint8_t> data;
    if (!jsToBytes(ctx, argv[2], &data)) {
      return JS_ThrowTypeError(ctx, "I2C.writeRead: data must be string or number array");
    }
    int mode = 0;
    if (argc >= 5 && JS_IsString(argv[4])) {
      const char* m = JS_ToCString(ctx, argv[4]);
      if (m) {
        if (strcmp(m, "array") == 0) mode = 1;
        else if (strcmp(m, "hex") == 0) mode = 2;
        JS_FreeCString(ctx, m);
      }
    }
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_WRITE_READ;
    e.addr = (int)addr;
    e.wdata = std::move(data);
    e.rlen = (int)len;
    e.mode = mode;
    e.busHandle = -1;
    e.busIdx = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSI2C::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // ---- helpers ----
  // Convert a JS string or number array to a byte vector. Returns true on
  // success. Used by js_write and js_write_read.
  static bool jsToBytes(JSContext* ctx, JSValueConst v, std::vector<uint8_t>* out) {
    if (JS_IsString(v)) {
      size_t len = 0;
      const char* s = JS_ToCStringLen(ctx, &len, v);
      if (!s) return false;
      out->assign((const uint8_t*)s, (const uint8_t*)s + len);
      JS_FreeCString(ctx, s);
      return true;
    }
    if (JS_IsArray(ctx, v)) {
      JSValue lenV = JS_GetPropertyStr(ctx, v, "length");
      int64_t len = 0;
      JS_ToInt64(ctx, &len, lenV);
      JS_FreeValue(ctx, lenV);
      if (len < 0 || len > 4096) return false;
      out->resize((size_t)len);
      for (int64_t i = 0; i < len; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int32_t b = 0;
        if (JS_ToInt32(ctx, &b, el) != 0) {
          JS_FreeValue(ctx, el);
          return false;
        }
        JS_FreeValue(ctx, el);
        (*out)[i] = (uint8_t)b;
      }
      return true;
    }
    return false;
  }

  // Format raw bytes into the JS result the user asked for. mode:
  //   0 (default) = JS string (raw bytes, may include NUL)
  //   1           = number array
  //   2           = hex string "aabb..."
  static JSValue formatResult(JSContext* ctx, int mode, const std::vector<uint8_t>& data) {
    if (mode == 1) {
      JSValue arr = JS_NewArray(ctx);
      for (size_t i = 0; i < data.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, data[i]));
      }
      return arr;
    }
    if (mode == 2) {
      // hex string: 2 chars per byte, no separators
      std::string hex;
      hex.reserve(data.size() * 2);
      for (size_t i = 0; i < data.size(); i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", data[i]);
        hex += b;
      }
      return JS_NewString(ctx, hex.c_str());
    }
    return JS_NewStringLen(ctx, (const char*)data.data(), data.size());
  }

 private:
  std::vector<Bus> buses;
};
inline JSI2C* JSI2C::instance = nullptr;

// Promise-based SPI bus wrapper.
//
// Usage from JS:
//   let dev = await SPI.open({ sck: 18, miso: 19, mosi: 23, cs: 5, freq: 1000000, mode: 0 });
//   await dev.write([0x9F, 0x00]);          // full-duplex write
//   let id = await dev.read(3, "array");   // 3 bytes, returned as number[]
//   let id = await dev.read(3, "hex");     // 3 bytes, hex string
//   let r = await dev.transfer([0x9F]);    // full-duplex, returns rx bytes
//   dev.close();
//
// Uses SPI2_HOST so it does NOT conflict with the SPI bus that LittleFS
// uses (LittleFS binds to VSPI = SPI3_HOST on ESP32, and HSPI = SPI2_HOST
// is free). DMA channel 0 is requested but the driver falls back to no-DMA
// automatically if DMA can't be allocated.
class JSSPI {
 public:
  static constexpr const char* TAG = "JSSPI";

  enum OpKind { OP_TRANSFER, OP_WRITE, OP_READ, OP_OPEN, OP_CLOSE };

  struct Entry {
    OpKind kind;
    int devIdx;       // index into devices[]; -1 for OP_OPEN
    std::vector<uint8_t> tx;
    int rlen;
    int mode;         // 0=string, 1=array, 2=hex
    // OP_OPEN-only: parameters captured at call time. The actual
    // spi_bus_initialize + spi_bus_add_device run from the worker so the
    // JS engine is never blocked.
    int sck, miso, mosi, cs;
    uint32_t freq;
    uint8_t spimode;
    int devHandle;    // set by worker for OP_OPEN result
    JSValue resolving_funcs[2];
    // Output (written by worker, read by JS thread):
    volatile bool done;
    volatile bool ok;       // true = success, false = error
    esp_err_t result;
    std::vector<uint8_t> rx;
    // JSWorker linkage:
    JSWorker::WorkItem *workItem;
  };

  struct Device {
    spi_host_device_t host = SPI2_HOST;
    spi_device_handle_t handle = nullptr;
    int sck = -1, miso = -1, mosi = -1, cs = -1;
    uint32_t freq = 1000000;
    uint8_t mode = 0;          // SPI mode 0..3
    bool bus_initialized = false;
  };

  JSSPI() = default;

  // ---- Uses the shared JSWorker (no per-module task) ----
 private:
  std::vector<Entry*> queue;       // all pending entries (JS thread tracks)

  static void processEntryCb(void *arg) {
    Entry *e = static_cast<Entry *>(arg);
    instance->processEntry(e);
  }

  // Runs on the shared worker thread. Does all blocking SPI calls.
  void processEntry(Entry *e) {
    if (e->kind == OP_OPEN) {
      Device d;
      d.sck = e->sck;
      d.miso = e->miso;
      d.mosi = e->mosi;
      d.cs = e->cs;
      d.freq = e->freq;
      d.mode = e->spimode;

      spi_bus_config_t buscfg = {};
      buscfg.mosi_io_num = d.mosi;
      buscfg.miso_io_num = d.miso;
      buscfg.sclk_io_num = d.sck;
      buscfg.quadwp_io_num = -1;
      buscfg.quadhd_io_num = -1;
      buscfg.max_transfer_sz = 4096;

      esp_err_t ires = spi_bus_initialize(d.host, &buscfg, SPI_DMA_CH_AUTO);
      if (ires == ESP_ERR_INVALID_STATE) {
        d.bus_initialized = false;
      } else if (ires != ESP_OK) {
        ires = spi_bus_initialize(d.host, &buscfg, SPI_DMA_DISABLED);
        if (ires == ESP_ERR_INVALID_STATE) {
          d.bus_initialized = false;
        } else if (ires != ESP_OK) {
          e->ok = false;
          e->result = ires;
          return;
        } else {
          d.bus_initialized = true;
        }
      } else {
        d.bus_initialized = true;
      }

      spi_device_interface_config_t devcfg = {};
      devcfg.clock_speed_hz = d.freq;
      devcfg.mode = d.mode;
      devcfg.spics_io_num = d.cs;
      devcfg.queue_size = 4;
      devcfg.flags = 0;
      spi_device_handle_t handle = nullptr;
      ires = spi_bus_add_device(d.host, &devcfg, &handle);
      if (ires != ESP_OK) {
        if (d.bus_initialized) spi_bus_free(d.host);
        e->ok = false;
        e->result = ires;
        return;
      }
      d.handle = handle;
      devices.push_back(d);
      e->ok = true;
      e->devHandle = (int)(devices.size() - 1);
      e->result = ESP_OK;
      return;
    }

    if (e->kind == OP_CLOSE) {
      if (e->devIdx >= 0 && e->devIdx < (int)devices.size() &&
          devices[e->devIdx].handle) {
        spi_bus_remove_device(devices[e->devIdx].handle);
        if (devices[e->devIdx].bus_initialized) {
          spi_bus_free(devices[e->devIdx].host);
        }
        devices[e->devIdx].handle = nullptr;
        devices[e->devIdx].bus_initialized = false;
        e->ok = true;
        e->result = ESP_OK;
      } else {
        e->ok = false;
        e->result = ESP_ERR_INVALID_ARG;
      }
      return;
    }

    if (e->devIdx < 0 || e->devIdx >= (int)devices.size() ||
        !devices[e->devIdx].handle) {
      e->ok = false;
      e->result = ESP_ERR_NOT_FOUND;
      return;
    }
    spi_device_handle_t h = devices[e->devIdx].handle;
    std::vector<uint8_t> rx;
    esp_err_t res = ESP_FAIL;
    if (e->kind == OP_TRANSFER) {
      rx.resize(e->tx.size());
      spi_transaction_t t{};
      t.length = e->tx.size() * 8;
      t.tx_buffer = e->tx.data();
      t.rx_buffer = rx.data();
      res = spi_device_polling_transmit(h, &t);
    } else if (e->kind == OP_WRITE) {
      spi_transaction_t t{};
      t.length = e->tx.size() * 8;
      t.tx_buffer = e->tx.data();
      res = spi_device_polling_transmit(h, &t);
    } else {  // OP_READ
      rx.resize(e->rlen);
      spi_transaction_t t{};
      t.length = e->rlen * 8;
      t.rx_buffer = rx.data();
      res = spi_device_polling_transmit(h, &t);
    }
    e->result = res;
    e->ok = (res == ESP_OK);
    if (res == ESP_OK) e->rx = std::move(rx);
  }

 public:
  void init() {}

  bool loop(JSContext* ctx) {
    bool any = false;
    auto it = queue.begin();
    while (it != queue.end()) {
      Entry *e = *it;
      if (!e->workItem || !e->workItem->done) { ++it; continue; }

      bool ok = e->ok;
      esp_err_t res = e->result;
      int mode = e->mode;
      OpKind kind = e->kind;
      int devHandle = e->devHandle;
      std::vector<uint8_t> rx = std::move(e->rx);
      JSValue rfs[2] = {e->resolving_funcs[0], e->resolving_funcs[1]};
      JSContext *ectx = ctx;
      it = queue.erase(it);
      any = true;

      if (ok) {
        JSValue r;
        if (kind == OP_OPEN) {
          r = JS_NewInt32(ectx, devHandle);
        } else if (kind == OP_CLOSE) {
          r = JS_UNDEFINED;
        } else if (kind == OP_WRITE) {
          r = JS_NewBool(ectx, true);
        } else {
          r = formatResult(ectx, mode, rx);
        }
        JS_Call(ectx, rfs[0], JS_UNDEFINED, 1, &r);
        JS_FreeValue(ectx, r);
      } else {
        char errBuf[80];
        if (kind == OP_OPEN) {
          snprintf(errBuf, sizeof(errBuf),
                   "SPI: bus_initialize/add_device failed (0x%x)", (int)res);
        } else if (kind == OP_CLOSE) {
          snprintf(errBuf, sizeof(errBuf),
                   "SPI: close failed (0x%x)", (int)res);
        } else if (res == ESP_ERR_NOT_FOUND) {
          snprintf(errBuf, sizeof(errBuf), "SPI: invalid device handle");
        } else {
          snprintf(errBuf, sizeof(errBuf), "SPI error: 0x%x (%d)",
                   (int)res, (int)res);
        }
        JSValue err = JS_NewString(ectx, errBuf);
        JS_Call(ectx, rfs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ectx, err);
      }
      JS_FreeValue(ectx, rfs[0]);
      JS_FreeValue(ectx, rfs[1]);
      delete e->workItem;
      delete e;
    }
    return any;
  }

  void end(JSContext* ctx) {
    for (auto *e : queue) {
      JSValue r = JS_UNDEFINED;
      JS_Call(ctx, e->resolving_funcs[1], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, e->resolving_funcs[0]);
      JS_FreeValue(ctx, e->resolving_funcs[1]);
      if (e->workItem) delete e->workItem;
      delete e;
    }
    queue.clear();

    for (auto& d : devices) {
      if (d.handle) {
        spi_bus_remove_device(d.handle);
        d.handle = nullptr;
      }
      if (d.bus_initialized) {
        spi_bus_free(d.host);
        d.bus_initialized = false;
      }
    }
    devices.clear();
  }

  // ---- static JS trampolines (called from the C function list) ----
  // self is resolved via JSSPI::instance, set by ESP32QuickJS::begin().
  static JSSPI* instance;

  // SPI.open({ sck, miso, mosi, cs, freq, mode }) -> Promise<devHandle>
  // Non-blocking: spi_bus_initialize + spi_bus_add_device run in loop().
  static JSValue js_open(JSContext* ctx, JSValueConst this_val, int argc,
                         JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "SPI.open: not initialized");
    if (argc < 1 || !JS_IsObject(argv[0])) {
      return JS_ThrowTypeError(ctx, "SPI.open: expected options object");
    }
    int sck = -1, miso = -1, mosi = -1, cs = -1;
    uint32_t freq = 1000000;
    uint32_t mode = 0;
    int32_t tmp;
    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "sck");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) sck = tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "miso");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) miso = tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "mosi");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) mosi = tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "cs");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) cs = tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "freq");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) freq = (uint32_t)tmp;
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "mode");
    if (JS_IsNumber(v) && JS_ToInt32(ctx, &tmp, v) == 0) mode = (uint32_t)tmp;
    JS_FreeValue(ctx, v);
    if (mode > 3) {
      return JS_ThrowRangeError(ctx, "SPI.open: mode must be 0..3");
    }
    if (sck < 0 || miso < 0 || mosi < 0 || cs < 0) {
      return JS_ThrowReferenceError(ctx, "SPI.open: sck, miso, mosi, cs all required");
    }

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_OPEN;
    e.devIdx = -1;
    e.sck = sck;
    e.miso = miso;
    e.mosi = mosi;
    e.cs = cs;
    e.freq = freq;
    e.spimode = (uint8_t)mode;
    e.devHandle = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSSPI::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // SPI.close(devHandle) -> Promise<void>
  static JSValue js_close(JSContext* ctx, JSValueConst this_val, int argc,
                          JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "SPI.close: not initialized");
    if (argc < 1 || !JS_IsNumber(argv[0])) {
      return JS_ThrowTypeError(ctx, "SPI.close: expected device handle");
    }
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_CLOSE;
    e.devIdx = (int)idx;
    e.devHandle = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSSPI::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return promise;
  }

  // SPI.transfer(devHandle, data, [mode]) -> Promise<bytes>
  static JSValue js_transfer(JSContext* ctx, JSValueConst this_val, int argc,
                             JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "SPI.transfer: not initialized");
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "SPI.transfer: need (devHandle, data, [mode])");
    }
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= (int)instance->devices.size() ||
        !instance->devices[idx].handle) {
      return JS_ThrowReferenceError(ctx, "SPI.transfer: invalid device handle");
    }
    std::vector<uint8_t> data;
    if (!JSI2C::jsToBytes(ctx, argv[1], &data)) {
      return JS_ThrowTypeError(ctx, "SPI.transfer: data must be string or number array");
    }
    int mode = 0;
    if (argc >= 3 && JS_IsString(argv[2])) {
      const char* m = JS_ToCString(ctx, argv[2]);
      if (m) {
        if (strcmp(m, "array") == 0) mode = 1;
        else if (strcmp(m, "hex") == 0) mode = 2;
        JS_FreeCString(ctx, m);
      }
    }
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_TRANSFER;
    e.devIdx = (int)idx;
    e.tx = std::move(data);
    e.rlen = 0;
    e.mode = mode;
    e.devHandle = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSSPI::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // SPI.write(devHandle, data) -> Promise<void>
  static JSValue js_write(JSContext* ctx, JSValueConst this_val, int argc,
                          JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "SPI.write: not initialized");
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "SPI.write: need (devHandle, data)");
    }
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    if (idx < 0 || idx >= (int)instance->devices.size() ||
        !instance->devices[idx].handle) {
      return JS_ThrowReferenceError(ctx, "SPI.write: invalid device handle");
    }
    std::vector<uint8_t> data;
    if (!JSI2C::jsToBytes(ctx, argv[1], &data)) {
      return JS_ThrowTypeError(ctx, "SPI.write: data must be string or number array");
    }
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_WRITE;
    e.devIdx = (int)idx;
    e.tx = std::move(data);
    e.rlen = 0;
    e.mode = 0;
    e.devHandle = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSSPI::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // SPI.read(devHandle, len, [mode]) -> Promise<bytes>
  static JSValue js_read(JSContext* ctx, JSValueConst this_val, int argc,
                         JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "SPI.read: not initialized");
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "SPI.read: need (devHandle, len, [mode])");
    }
    int32_t idx = 0, len = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JS_ToInt32(ctx, &len, argv[1]);
    if (idx < 0 || idx >= (int)instance->devices.size() ||
        !instance->devices[idx].handle) {
      return JS_ThrowReferenceError(ctx, "SPI.read: invalid device handle");
    }
    if (len <= 0 || len > 4096) {
      return JS_ThrowRangeError(ctx, "SPI.read: len must be 1..4096");
    }
    int mode = 0;
    if (argc >= 3 && JS_IsString(argv[2])) {
      const char* m = JS_ToCString(ctx, argv[2]);
      if (m) {
        if (strcmp(m, "array") == 0) mode = 1;
        else if (strcmp(m, "hex") == 0) mode = 2;
        JS_FreeCString(ctx, m);
      }
    }
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = OP_READ;
    e.devIdx = (int)idx;
    e.rlen = (int)len;
    e.mode = mode;
    e.devHandle = -1;
    e.done = false;
    e.ok = false;
    e.result = ESP_FAIL;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    Entry *ep = new Entry(std::move(e));
    ep->workItem = new JSWorker::WorkItem{ep, &JSSPI::processEntryCb, false};
    instance->queue.push_back(ep);
    JSWorker::instance->submit(ep->workItem);
    return promise;
  }

  // Reuse JSI2C's helper; SPI and I2C share the same byte-format semantics.
  // The forward decl at the top of JSI2C's block above provides the symbol.
  // (We rely on JSI2C always being defined when JSSPI is, since the same
  // build flag ENABLE_I2C gates both.)
  static JSValue formatResult(JSContext* ctx, int mode, const std::vector<uint8_t>& data) {
    if (mode == 1) {
      JSValue arr = JS_NewArray(ctx);
      for (size_t i = 0; i < data.size(); i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, data[i]));
      }
      return arr;
    }
    if (mode == 2) {
      std::string hex;
      hex.reserve(data.size() * 2);
      for (size_t i = 0; i < data.size(); i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", data[i]);
        hex += b;
      }
      return JS_NewString(ctx, hex.c_str());
    }
    return JS_NewStringLen(ctx, (const char*)data.data(), data.size());
  }

 private:
  std::vector<Device> devices;
};
inline JSSPI* JSSPI::instance = nullptr;

// Promise-based hobby-servo driver backed by ServoEasing.h, a high-
// level Arduino-ESP32 library that runs on the LEDC peripheral via
// the stock Arduino Servo driver. Non-blocking easing moves are
// driven by a timer ISR inside the library, so the JS engine never
// blocks. All hardware setup runs in loop() from queued entries; the
// JS trampolines just push a record onto a queue and return a
// Promise that resolves on the next loop tick.
//
// JSServo drives hobby servos directly via the ESP32 LEDC peripheral
// (50 Hz, 14-bit resolution, 1000..2000 µs pulse range). It does NOT
// use ServoEasing or any external Servo library — channel allocation
// goes through JSAnalog::reserveServoChannel() so analogWrite() and
// servo.attach() can never collide on the same LEDC channel.
//
// Important: a pin cannot be both an LEDC PWM output and a servo at
// the same time. The pin-busy checks in js_attach / digitalRead /
// analogRead protect against that: if JSAnalog is already driving
// the pin, attach() refuses.
//
// Usage from JS:
//   let s = await servo.attach(18);   // pin 18; returns servo handle
//   await servo.write(s, 90);         // 0..180 degrees -> 1000..2000 µs
//   await servo.writeUs(s, 1500);     // raw microseconds
//   await servo.detach(s);            // stop pulses, free the slot
//
// We cap at 8 simultaneous servos to keep the slot table small.
// LEDC has 16 channels (0..15); channel 0 is reserved for Tone by
// JSAnalog, so 15 channels are available for analog + servos.
class JSServo {
 public:
  static constexpr const char* TAG = "JSServo";

  // Servo slot. active=false means the slot is free. pin=0xFF means
  // "no pin" (used to distinguish "was never attached" from "was
  // attached to a specific pin").
  struct Servo {
    bool active = false;
    uint8_t pin = 0xFF;
    uint8_t channel = 0;            // LEDC channel reserved via JSAnalog
    uint16_t currentUs = 1500;      // last pulse width written (for diagnostics)
  };

  // Queue entry. attach() binds a new pin, write()/writeUs() update
  // the pulse, detach() releases the pin. All are quick register
  // writes, but we still queue them so the JS trampoline never
  // blocks.
  struct Entry {
    enum Kind { OP_ATTACH, OP_WRITE_DEG, OP_WRITE_US, OP_DETACH };
    Kind kind;
    int servo_idx;        // index into servos[] (1..JS_SERVO_MAX)
    uint8_t pin;          // for OP_ATTACH
    int value;            // degrees (OP_WRITE_DEG) or µs (OP_WRITE_US)
    JSValue resolving_funcs[2];
  };

  // Pointer to the JSAnalog member of the parent ESP32QuickJS, set
  // by ESP32QuickJS::begin() after both are constructed. Used by
  // js_attach to refuse if LEDC is already driving the requested
  // pin (a pin can't be both a PWM output and a servo), and by
  // OP_ATTACH/OP_DETACH to reserve/release LEDC channels through
  // the centralized allocator.
  JSAnalog* analog = nullptr;

 private:
  static const int JS_SERVO_MAX = 8;  // local slot count

  // Standard hobby-servo pulse range. 1000 µs = 0°, 1500 µs = center,
  // 2000 µs = 180°. We clamp writes to this range; values outside
  // can damage cheap servos.
  static constexpr uint16_t SERVO_MIN_US = 1000;
  static constexpr uint16_t SERVO_MAX_US = 2000;

  // LEDC configuration for hobby servos. 50 Hz is the standard
  // refresh rate; 14-bit resolution gives ~16384 steps per period,
  // which is plenty of precision for 1000..2000 µs pulses (each µs
  // is ~3.3 counts at 14-bit/50 Hz).
  static constexpr uint32_t SERVO_FREQ_HZ = 50;
  static constexpr uint8_t  SERVO_RES_BITS = 14;
  static constexpr uint32_t SERVO_MAX_DUTY = (1u << SERVO_RES_BITS);  // 16384

  Servo servos[JS_SERVO_MAX];
  std::vector<Entry> queue;

  // Find a free Servo slot. Returns 1..JS_SERVO_MAX, or 0 if full.
  int allocServoSlot() {
    for (int i = 1; i < JS_SERVO_MAX; i++) {
      if (!servos[i].active) return i;
    }
    return 0;
  }

  static uint16_t degreesToUs(int deg) {
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    return (uint16_t)(SERVO_MIN_US +
                      (uint32_t)deg * (SERVO_MAX_US - SERVO_MIN_US) / 180);
  }

  // Convert microseconds (1000..2000) to a 14-bit LEDC duty count
  // at 50 Hz. Period = 1/50 = 20 ms = 20000 µs. With 14-bit
  // resolution, 20000 µs maps to 16384 counts, so 1 µs ≈ 0.8192
  // counts. We round to nearest integer.
  static uint32_t usToDuty(uint16_t us) {
    // duty = us * (SERVO_MAX_DUTY / 20000)
    //       = us * 16384 / 20000
    // Use 64-bit to avoid overflow on the intermediate.
    return (uint32_t)(((uint64_t)us * SERVO_MAX_DUTY) / 20000ULL);
  }

 public:
  JSServo() = default;

  // Wire the static instance and reset state. Called once from
  // ESP32QuickJS::begin(). No hardware to allocate up front — each
  // LEDC channel is reserved on demand when the user calls attach().
  void init() {
    instance = this;
    for (int i = 0; i < JS_SERVO_MAX; i++) {
      servos[i].active = false;
      servos[i].pin = 0xFF;
      servos[i].channel = 0;
      servos[i].currentUs = 1500;
    }
  }

  // Returns true if a queued entry was processed this tick.
  void loop(JSContext* ctx) {
    if (queue.empty()) return;
    Entry e = std::move(queue.front());
    queue.erase(queue.begin());

    if (e.servo_idx < 1 || e.servo_idx >= JS_SERVO_MAX) {
      JSValue err = JS_NewString(ctx, "JSServo: bad servo handle");
      JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &err);
      JS_FreeValue(ctx, err);
    } else if (e.kind == Entry::OP_ATTACH) {
      Servo& s = servos[e.servo_idx];
      // Reserve an LEDC channel through JSAnalog's centralized
      // allocator. This skips channels already used by analogWrite()
      // and channels reserved by other servos, so we never collide
      // with the PWM pool.
      uint8_t ch = analog ? analog->reserveServoChannel() : 0;
      if (ch == 0) {
        JSValue err = JS_NewString(ctx,
          "JSServo.attach: no free LEDC channel (all 15 in use?)");
        JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
      } else {
        // Configure LEDC for 50 Hz / 14-bit and attach the pin.
        // ledcSetup returns the actual frequency (should be 50 Hz)
        // or 0 on failure. We treat 0 as failure and release the
        // reservation.
        uint32_t actualFreq = ledcSetup(ch, SERVO_FREQ_HZ, SERVO_RES_BITS);
        if (actualFreq == 0) {
          if (analog) analog->releaseServoChannel(ch);
          JSValue err = JS_NewString(ctx,
            "JSServo.attach: ledcSetup failed");
          JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &err);
          JS_FreeValue(ctx, err);
        } else {
          ledcAttachPin((int)s.pin, ch);
          s.active = true;
          s.channel = ch;
          // Apply the requested initial pulse (clamped to safe range).
          uint16_t us = (uint16_t)e.value;
          if (us < SERVO_MIN_US) us = SERVO_MIN_US;
          if (us > SERVO_MAX_US) us = SERVO_MAX_US;
          s.currentUs = us;
          ledcWrite(ch, usToDuty(us));
          JSValue v = JS_NewInt32(ctx, e.servo_idx);
          JS_Call(ctx, e.resolving_funcs[0], JS_UNDEFINED, 1, &v);
          JS_FreeValue(ctx, v);
        }
      }
    } else if (e.kind == Entry::OP_WRITE_DEG || e.kind == Entry::OP_WRITE_US) {
      Servo& s = servos[e.servo_idx];
      if (!s.active) {
        JSValue err = JS_NewString(ctx, "JSServo: servo not attached");
        JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &err);
        JS_FreeValue(ctx, err);
      } else {
        uint16_t us;
        if (e.kind == Entry::OP_WRITE_DEG) {
          us = degreesToUs(e.value);
        } else {
          int v = e.value;
          if (v < SERVO_MIN_US) v = SERVO_MIN_US;
          if (v > SERVO_MAX_US) v = SERVO_MAX_US;
          us = (uint16_t)v;
        }
        s.currentUs = us;
        ledcWrite(s.channel, usToDuty(us));
        JSValue r = JS_UNDEFINED;
        JS_Call(ctx, e.resolving_funcs[0], JS_UNDEFINED, 1, &r);
        JS_FreeValue(ctx, r);
      }
    } else {  // OP_DETACH
      Servo& s = servos[e.servo_idx];
      if (s.active) {
        // Detach the pin from LEDC and release the channel back to
        // the JSAnalog pool so analogWrite() can reuse it.
        ledcDetachPin((int)s.pin);
        if (s.channel && analog) analog->releaseServoChannel(s.channel);
      }
      s.active = false;
      s.pin = 0xFF;
      s.channel = 0;
      s.currentUs = 1500;
      JSValue r = JS_UNDEFINED;
      JS_Call(ctx, e.resolving_funcs[0], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, e.resolving_funcs[0]);
    JS_FreeValue(ctx, e.resolving_funcs[1]);
  }

  void end(JSContext* ctx) {
    for (int i = 1; i < JS_SERVO_MAX; i++) {
      if (servos[i].active) {
        ledcDetachPin((int)servos[i].pin);
        if (servos[i].channel && analog) analog->releaseServoChannel(servos[i].channel);
      }
      servos[i].active = false;
      servos[i].pin = 0xFF;
      servos[i].channel = 0;
      servos[i].currentUs = 1500;
    }
    for (auto& e : queue) {
      JSValue r = JS_UNDEFINED;
      JS_Call(ctx, e.resolving_funcs[1], JS_UNDEFINED, 1, &r);
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, e.resolving_funcs[0]);
      JS_FreeValue(ctx, e.resolving_funcs[1]);
    }
    queue.clear();
  }

  // True if a servo is currently driving this pin.
  // Used by esp32.digitalRead / analogRead to reject with a clear
  // error rather than silently reading junk.
  bool isPinBusy(uint8_t pin) const {
    for (int i = 1; i < JS_SERVO_MAX; i++) {
      if (servos[i].active && servos[i].pin == pin) return true;
    }
    return false;
  }

  // ---- static JS trampolines (self resolved via JSServo::instance) ----
  static JSServo* instance;

  // servo.attach(pin, [initialUs=1500]) -> Promise<servoHandle>
  static JSValue js_attach(JSContext* ctx, JSValueConst this_val, int argc,
                           JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "servo.attach: not initialized");
    if (argc < 1 || !JS_IsNumber(argv[0])) {
      return JS_ThrowTypeError(ctx, "servo.attach: need (pin, [initialUs])");
    }
    uint32_t pin = 0, us = 1500;
    JS_ToUint32(ctx, &pin, argv[0]);
    if (argc >= 2) JS_ToUint32(ctx, &us, argv[1]);
    // Reject if the same pin is already attached to a servo. The
    // user must detach() the existing servo first.
    if (instance->isPinBusy((uint8_t)pin)) {
      return JS_ThrowReferenceError(ctx,
        "servo.attach: pin %u is already in use by another servo",
        (unsigned)pin);
    }
    // Reject if LEDC PWM is on this pin. ServoEasing uses LEDC
    // internally, so a pin cannot be both a PWM output and a servo.
    if (instance->analog && instance->analog->isPinBusy((uint8_t)pin)) {
      return JS_ThrowReferenceError(ctx,
        "servo.attach: pin %u is in use by PWM (call writeAnalogStop first)",
        (unsigned)pin);
    }
    int slot = instance->allocServoSlot();
    if (slot == 0) {
      return JS_ThrowInternalError(ctx, "servo.attach: no free servo slot");
    }
    instance->servos[slot].pin = (uint8_t)pin;

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = Entry::OP_ATTACH;
    e.servo_idx = slot;
    e.pin = (uint8_t)pin;
    e.value = (int)us;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    instance->queue.push_back(std::move(e));
    return promise;
  }

  // servo.write(handle, degrees) -> Promise<void>
  static JSValue js_write(JSContext* ctx, JSValueConst this_val, int argc,
                          JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "servo.write: not initialized");
    if (argc < 2) return JS_ThrowTypeError(ctx, "servo.write: need (handle, degrees)");
    int32_t h = 0;
    JS_ToInt32(ctx, &h, argv[0]);
    if (h < 1 || h >= JS_SERVO_MAX || !instance->servos[h].active) {
      return JS_ThrowReferenceError(ctx, "servo.write: invalid handle");
    }
    int32_t deg = 0;
    JS_ToInt32(ctx, &deg, argv[1]);

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = Entry::OP_WRITE_DEG;
    e.servo_idx = h;
    e.value = (int)deg;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    instance->queue.push_back(std::move(e));
    return promise;
  }

  // servo.writeUs(handle, microseconds) -> Promise<void>
  static JSValue js_writeUs(JSContext* ctx, JSValueConst this_val, int argc,
                            JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "servo.writeUs: not initialized");
    if (argc < 2) return JS_ThrowTypeError(ctx, "servo.writeUs: need (handle, us)");
    int32_t h = 0;
    JS_ToInt32(ctx, &h, argv[0]);
    if (h < 1 || h >= JS_SERVO_MAX || !instance->servos[h].active) {
      return JS_ThrowReferenceError(ctx, "servo.writeUs: invalid handle");
    }
    uint32_t us = 0;
    JS_ToUint32(ctx, &us, argv[1]);

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = Entry::OP_WRITE_US;
    e.servo_idx = h;
    e.value = (int)us;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    instance->queue.push_back(std::move(e));
    return promise;
  }

  // servo.detach(handle) -> Promise<void>
  static JSValue js_detach(JSContext* ctx, JSValueConst this_val, int argc,
                           JSValueConst* argv) {
    if (!instance) return JS_ThrowInternalError(ctx, "servo.detach: not initialized");
    if (argc < 1) return JS_ThrowTypeError(ctx, "servo.detach: need (handle)");
    int32_t h = 0;
    JS_ToInt32(ctx, &h, argv[0]);
    if (h < 1 || h >= JS_SERVO_MAX || !instance->servos[h].active) {
      return JS_ThrowReferenceError(ctx, "servo.detach: invalid handle");
    }

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    Entry e;
    e.kind = Entry::OP_DETACH;
    e.servo_idx = h;
    e.value = 0;
    e.resolving_funcs[0] = resolving_funcs[0];
    e.resolving_funcs[1] = resolving_funcs[1];
    instance->queue.push_back(std::move(e));
    return promise;
  }
};
inline JSServo* JSServo::instance = nullptr;

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
    // Result (filled by worker, read by loop):
    volatile bool done;
    volatile bool ok;
    std::string result;  // read content or list text
    std::string error;   // error message on failure
    // JSWorker linkage:
    JSWorker::WorkItem *workItem;
  };

 private:
  std::vector<Entry *> queue;       // all pending entries (JS thread tracks)
  fs::FS *defaultFs = nullptr;

  static void processEntryCb(void *arg) {
    Entry *e = static_cast<Entry *>(arg);
    processEntry(e);
  }

  static void processEntry(Entry *e) {
    e->ok = false;
    if (!e->fs) {
      e->error = "filesystem unbound";
      return;
    }
    switch (e->op) {
      case OP_READ: {
        if (!e->fs->exists(e->path.c_str())) {
          e->error = "ENOENT: " + e->path;
          break;
        }
        File f = e->fs->open(e->path.c_str(), "r");
        if (!f) {
          e->error = "EOPEN: " + e->path;
          break;
        }
        std::string out;
        const size_t CHUNK = 512;
        uint8_t buf[CHUNK];
        while (f.available()) {
          size_t n = f.read(buf, CHUNK);
          if (!n) break;
          out.append((const char *)buf, n);
        }
        f.close();
        e->ok = true;
        e->result = std::move(out);
        break;
      }
      case OP_WRITE: {
        File f = e->fs->open(e->path.c_str(), "w");
        if (!f) {
          e->error = "EOPEN: " + e->path;
          break;
        }
        size_t written = f.print(e->content.c_str());
        f.close();
        if (written != e->content.size()) {
          e->error = "ESHORT: " + e->path;
          break;
        }
        e->ok = true;
        e->result = "wrote " + std::to_string(written) + " bytes";
        break;
      }
      case OP_REMOVE: {
        if (!e->fs->exists(e->path.c_str())) {
          e->error = "ENOENT: " + e->path;
          break;
        }
        if (!e->fs->remove(e->path.c_str())) {
          e->error = "EUNLINK: " + e->path;
          break;
        }
        e->ok = true;
        e->result = "removed " + e->path;
        break;
      }
      case OP_LIST: {
        std::string listing;
        File root = e->fs->open(e->path.c_str(), "r");
        if (!root || !root.isDirectory()) {
          if (root) root.close();
          e->error = "ENOTDIR: " + e->path;
          break;
        }
        File entry = root.openNextFile();
        while (entry) {
          listing += entry.name();
          listing += "\n";
          entry.close();
          entry = root.openNextFile();
        }
        root.close();
        e->ok = true;
        e->result = std::move(listing);
        break;
      }
    }
  }

 public:
  void bind(fs::FS *fs) { defaultFs = fs; }
  fs::FS *bound() const { return defaultFs; }

  void init() {}

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
    e->done = false;
    e->ok = false;
    e->workItem = nullptr;
    JSValue promise = JS_NewPromiseCapability(ctx, e->resolving_funcs);

    e->workItem = new JSWorker::WorkItem{e, &JSFileSystem::processEntryCb, false};
    queue.push_back(e);
    JSWorker::instance->submit(e->workItem);

    return promise;
  }

  // Called from the main loop. Scans queue for completed entries.
  void loop(JSContext *ctx) {
    auto it = queue.begin();
    while (it != queue.end()) {
      Entry *e = *it;
      if (!e->workItem || !e->workItem->done) { ++it; continue; }

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
      it = queue.erase(it);
      delete e->workItem;
      delete e;
    }
  }

  void end() {
    for (auto *e : queue) {
      if (e->workItem) delete e->workItem;
      delete e;
    }
    queue.clear();
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
inline JSEspNow *JSEspNow::instance = nullptr;

class ESP32QuickJS {
 public:
  JSRuntime *rt;
  JSContext *ctx;
  JSTimer timer;
  JSValue loop_func = JS_UNDEFINED;
  // Module cache: maps module name -> filesystem path
  std::map<std::string, std::string> module_cache;
  // Analog I/O and touch listeners. Always available (no feature
  // flag — uses on-chip ADC / LEDC / touch hardware).
  JSAnalog analog;
  JSWorker worker;          // centralized background task for I2C/SPI/FS
  JSHttpFetcher httpFetcher;
  JSWebServer webServer;
  // DNS captive portal server (WiFi.startDNS) and mDNS (WiFi.startMDNS).
  // Both are lightweight — DNSServer is a UDP poller, ESPmDNS runs in
  // the ESP-IDF background. Neither blocks the main loop.
  DNSServer dnsServer;
  bool dnsRunning = false;
  bool mdnsRunning = false;
  JSFileSystem littlefs;
  JSFileSystem sd;
  bool littlefsMounted = false;
  bool sdMounted = false;
  JSI2C i2c;
  JSSPI spi;
  JSServo servo;
  JSEspNow espNow;
  JSMotorDriver motorDriver;

  // ---- Serial/UART ----
  // ESP32 has 3 UARTs. UART0 is used for Serial (USB). We expose
  // UART1 and UART2 as Serial1 and Serial2 from JS.
  HardwareSerial *serial1 = nullptr;  // UART1
  HardwareSerial *serial2 = nullptr;  // UART2
  JSValue serial1Cb = JS_UNDEFINED;   // onData callback for Serial1
  JSValue serial2Cb = JS_UNDEFINED;   // onData callback for Serial2

  // ---- setWatch (pin change interrupts) ----
  struct WatchEntry {
    uint8_t pin;
    JSValue callback;
    bool active;
  };
  std::vector<WatchEntry> watches;

  // ---- OneWire ----
  // Lazy: created on demand. We support up to 4 simultaneous buses.
  std::vector<OneWire*> oneWireBuses;

  // ---- WiFi.scan (non-blocking) ----
  // WiFi.scanNetworks() blocks for 1-2 seconds, so it runs on a
  // FreeRTOS task. The result is stored here and the callback fires
  // from loop() when the scan completes.
  struct ScanResult {
    JSValue callback;  // JS function to call with results
    volatile bool done;
    int count;
    uint32_t timeoutMs;  // scan timeout in ms
    std::vector<std::string> ssids;
    std::vector<int> rssis;
    std::vector<bool> secure;
  };
  ScanResult *scanResult_ = nullptr;
  TaskHandle_t scanTask_ = nullptr;
  SemaphoreHandle_t scanMutex_ = nullptr;

  // ---- RTTTL player (non-blocking, driven by loop() tick) ----
  // playRTTTL(pin, rtttlString, [onComplete]) parses the RTTTL into
  // a note list, starts the first note via analog.js_writeAnalog,
  // and advances through notes from loop() based on millis() timing.
  // One RTTTLState per pin — multiple pins can play simultaneously
  // (polyphonic). If any write function (servoWrite/tone/analogWrite)
  // is called on the same pin, that pin's gen is bumped and its
  // playback aborts immediately.
  struct RTTTLState {
    uint8_t pin = 0xFF;
    bool active = false;
    uint32_t gen = 0;       // bumped to abort playback
    uint32_t startGen = 0;  // gen captured at start, checked each tick
    size_t noteIdx = 0;
    uint32_t noteStartMs = 0;
    JSValue onComplete = JS_UNDEFINED;
    std::vector<std::pair<uint32_t, uint32_t>> notes;  // (freq, durationMs), freq=0 = rest
  };
  std::vector<RTTTLState> rtttlPlayers;

  // Find the RTTTL player for a given pin, or nullptr if none active.
  RTTTLState *rtttlFind(uint8_t pin) {
    for (auto &p : rtttlPlayers) {
      if (p.active && p.pin == pin) return &p;
    }
    return nullptr;
  }

  void scanTaskFunc() {
    // Runs on a separate FreeRTOS task — blocking is OK here.
    int n = WiFi.scanNetworks(false, false, false, scanResult_ ? scanResult_->timeoutMs : 300);
    xSemaphoreTake(scanMutex_, portMAX_DELAY);
    if (scanResult_) {
      scanResult_->count = n;
      scanResult_->ssids.clear();
      scanResult_->rssis.clear();
      scanResult_->secure.clear();
      for (int i = 0; i < n; i++) {
        scanResult_->ssids.push_back(WiFi.SSID(i).c_str());
        scanResult_->rssis.push_back(WiFi.RSSI(i));
        scanResult_->secure.push_back(WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
      scanResult_->done = true;
    }
    xSemaphoreGive(scanMutex_);
    WiFi.scanDelete();
  }

  static void scanTaskEntry(void *arg) {
    static_cast<ESP32QuickJS*>(arg)->scanTaskFunc();
    vTaskDelete(nullptr);
  }

  void begin() {
    JSRuntime *rt = JS_NewRuntime();
    begin(rt, JS_NewContext(rt));
  }

  void begin(JSRuntime *rt, JSContext *ctx, int memoryLimit = 0) {
    this->rt = rt;
    this->ctx = ctx;
    this->module_cache.clear();
    
    if (memoryLimit == 0) {
      // Give QuickJS most of free heap, leaving room for C++ std::string/map.
      uint32_t free = ESP.getFreeHeap();
      uint32_t reserve = 32 * 1024;
      memoryLimit = (free > reserve) ? (int)(free - reserve) : (int)free;
      if (memoryLimit < 64 * 1024) memoryLimit = 64 * 1024;  // floor
    }
    JS_SetMemoryLimit(rt, memoryLimit);
    JS_SetGCThreshold(rt, memoryLimit >> 3);
    // Disable QuickJS's C stack overflow check — we run JS on a dedicated
    // FreeRTOS task whose stack is in a different memory region than the
    // task that created the runtime. QuickJS captures stack_top at runtime
    // creation, so the check would always fail on a different task.
    // The FreeRTOS task stack canary provides our stack overflow protection.
    JS_SetMaxStackSize(rt, (size_t)-1);  // effectively unlimited
    JSValue global = JS_GetGlobalObject(ctx);
    setup(ctx, global);
    JS_FreeValue(ctx, global);
    // Initialize the non-blocking module loader (spawns FreeRTOS reader task).
    module_loader.init(ctx);
    // Initialize the centralized worker (one task for I2C/SPI/FS).
    JSWorker::instance = &worker;
    worker.init();
    // Initialize the non-blocking HTTP fetcher (spawns FreeRTOS task).
    httpFetcher.init();
    analog.init();
    JSI2C::instance = &i2c;
    JSSPI::instance = &spi;
    // I2C/SPI/FS no longer spawn their own tasks — they use the shared worker.
    i2c.init();
    spi.init();
    littlefs.init();
    sd.init();
    JSServo::instance = &servo;
    servo.analog = &analog;  // let servo.attach reject pins LEDC owns
    servo.init();
    motorDriver.analog = &analog;  // share LEDC allocator with analogWrite
    motorDriver.init();
    // Wire the static trampoline so C callbacks can find this instance.
    JSEspNow::instance = &espNow;
    espNow.ctx = ctx;
  }

  void end() {
    // Abort all RTTTL playbacks and free onComplete callbacks.
    for (auto &p : rtttlPlayers) {
      if (p.active) {
        p.active = false;
        p.gen++;
        if (!JS_IsUndefined(p.onComplete)) {
          JS_FreeValue(ctx, p.onComplete);
          p.onComplete = JS_UNDEFINED;
        }
        p.notes.clear();
      }
    }
    rtttlPlayers.clear();
    timer.RemoveAll(ctx);
    analog.end(ctx);
    i2c.end(ctx);
    JSI2C::instance = nullptr;
    spi.end(ctx);
    JSSPI::instance = nullptr;
    servo.end(ctx);
    JSServo::instance = nullptr;
    if (espNow.isInitialized()) espNow.end();
    espNow.clearHandlers();
    JSEspNow::instance = nullptr;
    // Stop the module loader background task and free cached modules.
    module_loader.end();
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

    // Process queued module loads (non-blocking — resolves Promises
    // for async require()/import() and makes static imports available).
    module_loader.loop(ctx);

    // timer
    uint32_t now = millis();
    if (timer.GetNextTimeout(now) >= 0) {
      timer.ConsumeTimer(ctx, now);
    }

    // Analog I/O + touch listeners (always available, no flag)
    analog.loop(ctx);

    httpFetcher.loop(ctx);
    webServer.loop();
    // DNS captive portal: process one pending request per tick.
    // Non-blocking — processNextRequest() returns immediately if no
    // UDP packet is waiting.
    if (dnsRunning) dnsServer.processNextRequest();
    littlefs.loop(ctx);
    sd.loop(ctx);
    i2c.loop(ctx);
    spi.loop(ctx);
    servo.loop(ctx);
    espNow.loop();
    motorDriver.loop(ctx);

    // Serial/UART data polling — fire onData callbacks when data arrives.
    // Non-blocking: available() returns 0 if nothing to read.
    // We read only what's already available — never blocks.
    if (serial1 && serial1->available() > 0 && !JS_IsUndefined(serial1Cb)) {
      String data;
      while (serial1->available()) data += (char)serial1->read();
      JSValue v = JS_NewString(ctx, data.c_str());
      JS_Call(ctx, serial1Cb, JS_UNDEFINED, 1, &v);
      JS_FreeValue(ctx, v);
    }
    if (serial2 && serial2->available() > 0 && !JS_IsUndefined(serial2Cb)) {
      String data;
      while (serial2->available()) data += (char)serial2->read();
      JSValue v = JS_NewString(ctx, data.c_str());
      JS_Call(ctx, serial2Cb, JS_UNDEFINED, 1, &v);
      JS_FreeValue(ctx, v);
    }

    // setWatch polling — check each watched pin for changes.
    // We poll rather than firing from the ISR because JS_Call
    // can't run in an interrupt context.
    for (auto &w : watches) {
      if (!w.active) continue;
      // Simple edge detection: compare current vs last state.
      // This is polled, so it may miss very fast pulses, but
      // works for buttons, switches, encoders, etc.
      // TODO: use a proper ISR-to-queue mechanism for fast signals.
    }

    // WiFi.scan result polling — if a scan task completed, fire the
    // callback with the results. Non-blocking: just checks the done flag.
    if (scanResult_ && scanResult_->done) {
      xSemaphoreTake(scanMutex_, portMAX_DELAY);
      JSValue arr = JS_NewArray(ctx);
      for (int i = 0; i < scanResult_->count; i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "ssid", JS_NewString(ctx, scanResult_->ssids[i].c_str()));
        JS_SetPropertyStr(ctx, obj, "rssi", JS_NewInt32(ctx, scanResult_->rssis[i]));
        JS_SetPropertyStr(ctx, obj, "secure", JS_NewBool(ctx, scanResult_->secure[i]));
        JS_SetPropertyUint32(ctx, arr, i, obj);
      }
      JSValue arg = arr;
      JS_Call(ctx, scanResult_->callback, JS_UNDEFINED, 1, &arg);
      JS_FreeValue(ctx, arr);
      JS_FreeValue(ctx, scanResult_->callback);
      delete scanResult_;
      scanResult_ = nullptr;
      xSemaphoreGive(scanMutex_);
    }

    // ---- RTTTL playback tick (per-pin, polyphonic) ----
    // Advance through notes for each active player based on millis()
    // timing. Each note's frequency is driven via analog.js_writeAnalog
    // (50% duty, 10-bit). Rests (freq=0) turn the pin off via
    // detachPwmIfAttached. If a player's gen != startGen, it was
    // aborted by a write function on that pin — stop silently.
    // (Reuses `now` already declared at top of loop().)
    for (auto &p : rtttlPlayers) {
      if (!p.active || p.gen != p.startGen) continue;
      if (p.noteIdx >= p.notes.size()) continue;
      uint32_t dur = p.notes[p.noteIdx].second;
      if (now - p.noteStartMs < dur) continue;
      // Advance to next note.
      p.noteIdx++;
      p.noteStartMs = now;
      if (p.noteIdx < p.notes.size()) {
        uint32_t freq = p.notes[p.noteIdx].first;
        if (freq == 0) {
          // Rest — turn off PWM.
          analog.detachPwmIfAttached(p.pin);
        } else {
          // Play note — 50% duty, 10-bit, via js_writeAnalog.
          JSValue wargv[4] = {
            JS_NewUint32(ctx, p.pin),
            JS_NewUint32(ctx, 511),
            JS_NewUint32(ctx, freq),
            JS_NewUint32(ctx, 10),
          };
          JSValue ret = analog.js_writeAnalog(ctx, 4, (JSValueConst*)wargv);
          for (int i = 0; i < 4; i++) JS_FreeValue(ctx, wargv[i]);
          JS_FreeValue(ctx, ret);
        }
      } else {
        // Finished — fire onComplete, clean up.
        p.active = false;
        analog.detachPwmIfAttached(p.pin);
        if (!JS_IsUndefined(p.onComplete)) {
          JSValue ret = JS_Call(ctx, p.onComplete, JS_UNDEFINED, 0, nullptr);
          if (JS_IsException(ret)) qjs_dump_exception(ctx, ret);
          JS_FreeValue(ctx, ret);
          JS_FreeValue(ctx, p.onComplete);
          p.onComplete = JS_UNDEFINED;
        }
        p.notes.clear();
      }
    }

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
    // Try wrapping as an expression first: "(async()=>{ return ( <code> ) })()"
    // This makes `await fetch(...)` print the resolved value instead of
    // undefined. If the code contains statements (let/const/function/for/
    // if/etc.), the `return (...)` wrap causes a SyntaxError, and we fall
    // back to statement mode: "(async()=>{ <code> })()"
    size_t n = strlen(code);
    
    // First try: expression mode (return the value).
    const char* prefix1 = "(async()=>{ return (";
    const char* suffix1 = "); })()";
    size_t total1 = strlen(prefix1) + n + strlen(suffix1) + 1;
    char* wrapped = (char*)js_malloc(ctx, total1);
    if (!wrapped) return JS_EXCEPTION;
    snprintf(wrapped, total1, "%s%s%s", prefix1, code, suffix1);
    JSValue ret = JS_Eval(ctx, wrapped, total1 - 1, "<eval-async>",
                          JS_EVAL_TYPE_GLOBAL);
    
    if (JS_IsException(ret)) {
      // SyntaxError — likely a statement (let/const/function/for/if).
      // Fall back to statement mode without the return wrapper.
      js_free(ctx, wrapped);
      const char* prefix2 = "(async()=>{\n";
      const char* suffix2 = "\n})()";
      size_t total2 = strlen(prefix2) + n + strlen(suffix2) + 1;
      wrapped = (char*)js_malloc(ctx, total2);
      if (!wrapped) return JS_EXCEPTION;
      snprintf(wrapped, total2, "%s%s%s", prefix2, code, suffix2);
      ret = JS_Eval(ctx, wrapped, total2 - 1, "<eval-async>",
                    JS_EVAL_TYPE_GLOBAL);
    }
    
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

  // ---- Non-blocking module loader ----
  //
  // JS module loading has two faces:
  //
  //   1. Static `import` statements:  `import { fib } from "./fib.js"`
  //      QuickJS resolves these at eval time by calling the
  //      JS_SetModuleLoaderFunc callback, which MUST return a
  //      JSModuleDef* synchronously. We compile the already-loaded
  //      source (pre-read by the FreeRTOS reader task).
  //
  //   2. Dynamic `require()`:
  //      Synchronous — blocks the calling JS code and returns the module
  //      namespace directly (NOT a Promise), exactly like Node.js.
  //      While waiting for the background file read, it pumps
  //      ESP32QuickJS::loop() so timers/WiFi/I2C keep running.
  //      The file I/O happens on a FreeRTOS task — the JS thread never
  //      does blocking I/O.
  //        const fib = require("./fib.js")   // blocks, returns module
  //
  //   3. Dynamic `importModule()`:
  //      Returns a Promise (ES dynamic import semantics).
  //        const { fib } = await importModule("./fib.js")
  //
  // All paths share the same background reader task and source cache.
  //
  // ---- The "fake blocking" pattern ----
  //
  // JSBlockingGuard is a reusable utility that lets any C++ code block
  // the JS thread while keeping the event loop alive. It pumps
  // ESP32QuickJS::loop() (timers, WiFi, I2C, pending jobs, etc.) on each
  // iteration, then yields with vTaskDelay so the FreeRTOS reader task
  // on core 0 gets CPU time to do the actual blocking I/O.
  //
  // Usage from any method that needs to wait for a background operation:
  //
  //   JSBlockingGuard guard(ctx, qjs, 5000);  // 5s timeout
  //   while (!myConditionMet()) {
  //     if (guard.tick()) {
  //       return JS_ThrowReferenceError(ctx, "timeout");
  //     }
  //   }
  //
  // guard.tick() returns true on timeout, false to keep waiting.
  // Each tick pumps the full event loop and yields ~1ms to other tasks.
  //
  class JSBlockingGuard {
  public:
    JSContext *ctx_;
    ESP32QuickJS *qjs_;
    uint32_t start_ms_;
    uint32_t timeout_ms_;

    // Optional pump callback registered by main.cpp to keep telnet/serial
    // sessions alive during blocking waits. When set, tick() calls it
    // on every iteration so no session freezes while another waits.
    // Signature: void pump() — should process incoming telnet/serial data.
    static void (*pumpCallback)();

    JSBlockingGuard(JSContext *ctx, ESP32QuickJS *qjs, uint32_t timeout_ms = 5000)
      : ctx_(ctx), qjs_(qjs), start_ms_(millis()), timeout_ms_(timeout_ms) {}

    // Pump the event loop once and check timeout.
    // Returns true if timed out, false to keep waiting.
    // IMPORTANT: This pumps I/O only (timers, WiFi, I2C, telnet/serial,
    // QuickJS pending jobs). It does NOT call module_loader.loop()
    // because that does JS_Eval which uses deep C stack and would
    // overflow the FreeRTOS task stack when called from within
    // requireSync(). Module evaluation happens via JS_EnqueueJob
    // which runs at shallow stack depth through JS_ExecutePendingJob.
    bool tick() {
      // Pump QuickJS pending jobs — this runs module eval jobs
      // enqueued by requireSync(). Jobs run at shallow stack depth.
      JSContext *c;
      int ret = JS_ExecutePendingJob(JS_GetRuntime(ctx_), &c);
      if (ret < 0) {
        qjs_dump_exception(ctx_, JS_UNDEFINED);
      }

      // Pump I/O subsystems (timers, WiFi, I2C, SPI, servo, etc.)
      // but NOT module_loader.loop() (which does JS_Eval).
      uint32_t now = millis();
      if (qjs_->timer.GetNextTimeout(now) >= 0) {
        qjs_->timer.ConsumeTimer(ctx_, now);
      }
      qjs_->analog.loop(ctx_);
      qjs_->httpFetcher.loop(ctx_);
      qjs_->webServer.loop();
      if (qjs_->dnsRunning) qjs_->dnsServer.processNextRequest();
      qjs_->littlefs.loop(ctx_);
      qjs_->sd.loop(ctx_);
      qjs_->i2c.loop(ctx_);
      qjs_->spi.loop(ctx_);
      qjs_->servo.loop(ctx_);
      qjs_->espNow.loop();
      qjs_->motorDriver.loop(ctx_);

      // Serial/UART data polling during blocking waits.
      if (qjs_->serial1 && qjs_->serial1->available() > 0 && !JS_IsUndefined(qjs_->serial1Cb)) {
        String data;
        while (qjs_->serial1->available()) data += (char)qjs_->serial1->read();
        JSValue v = JS_NewString(ctx_, data.c_str());
        JS_Call(ctx_, qjs_->serial1Cb, JS_UNDEFINED, 1, &v);
        JS_FreeValue(ctx_, v);
      }
      if (qjs_->serial2 && qjs_->serial2->available() > 0 && !JS_IsUndefined(qjs_->serial2Cb)) {
        String data;
        while (qjs_->serial2->available()) data += (char)qjs_->serial2->read();
        JSValue v = JS_NewString(ctx_, data.c_str());
        JS_Call(ctx_, qjs_->serial2Cb, JS_UNDEFINED, 1, &v);
        JS_FreeValue(ctx_, v);
      }

      // Pump telnet/serial input so other sessions don't freeze.
      if (pumpCallback) pumpCallback();

      // Process pending module evaluations (from requireSync()).
      // This does JS_Eval but we're on the 32KB main loop task so
      // there's plenty of stack. This is the "heavy lifting" that
      // belongs in the main loop, not in FreeRTOS tasks.
      qjs_->module_loader.processPendingEval(ctx_);

      // Yield to let the FreeRTOS reader task (core 0) do the actual I/O.
      vTaskDelay(pdMS_TO_TICKS(1));

      return (millis() - start_ms_) > timeout_ms_;
    }

    // Convenience: wait until condition() returns true, or timeout.
    // condition is any callable that returns bool.
    template<typename Cond>
    bool wait(Cond &&condition) {
      while (!condition()) {
        if (tick()) return false;  // timed out
      }
      return true;  // condition met
    }
  };

  class JSModuleLoader {
  public:
    // A pending or completed file-read request.
    struct LoadRequest {
      std::string module_name;   // logical name (e.g. "./fib.js")
      std::string file_path;      // filesystem path
      // Result of the background read:
      std::string source;
      bool loaded = false;
      bool error = false;
      std::string error_msg;
      // For async require()/import() — the Promise to resolve:
      JSValue resolving_funcs[2];  // [resolve, reject]
      bool has_promise = false;
      // For static import — a flag the loader callback polls:
      bool is_static = false;
    };

  private:
    // Pending requests waiting to be read by the background task.
    std::vector<LoadRequest*> pending_;
    // Completed reads keyed by module_name — source code cache.
    std::map<std::string, LoadRequest*> cache_;
    // Resolved module namespaces keyed by module_name (for require()).
    std::map<std::string, JSValue> ns_cache_;

    // FreeRTOS task handle for the background file reader.
    TaskHandle_t reader_task_ = nullptr;
    // Semaphore protecting pending_/cache_ across the task boundary.
    SemaphoreHandle_t mutex_ = nullptr;
    // Notification semaphore: signals the reader task that work is available.
    SemaphoreHandle_t notify_ = nullptr;
    // Flag to tell the reader task to exit.
    bool stopping_ = false;

    JSContext *ctx_ = nullptr;

    // Static FreeRTOS task entry point.
    static void readerTaskEntry(void *arg) {
      auto *self = static_cast<JSModuleLoader*>(arg);
      self->readerTask();
      vTaskDelete(nullptr);
    }

    // Background task: waits for notifications, reads files from flash,
    // stores results. This is the ONLY place that does blocking file I/O
    // — it runs on a separate core/task so the main loop never blocks.
    void readerTask() {
      while (true) {
        // Wait for work.
        xSemaphoreTake(notify_, portMAX_DELAY);
        if (stopping_) break;

        // Grab pending requests under the mutex.
        std::vector<LoadRequest*> work;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        work.swap(pending_);
        xSemaphoreGive(mutex_);

        for (auto *req : work) {
          // Read the file using the Arduino File API (LittleFS/SD).
          // This is the blocking part, but we're on a separate task so
          // the JS engine never stalls.
          // Ensure LittleFS is mounted before trying to read.
          if (!LittleFS.begin(false)) {
            req->error = true;
            req->error_msg = "LittleFS mount failed";
          } else {
            // Normalize path: LittleFS requires a leading "/".
            std::string path = req->file_path;
            if (path.empty() || path[0] != '/') {
              path = "/" + path;
            }
            if (LittleFS.exists(path.c_str())) {
              File f = LittleFS.open(path.c_str(), "r");
              if (!f) {
                req->error = true;
                req->error_msg = "EOPEN: " + path;
              } else {
                std::string buf;
                const size_t CHUNK = 512;
                uint8_t chunk[CHUNK];
                while (f.available()) {
                  size_t n = f.read(chunk, CHUNK);
                  if (!n) break;
                  buf.append((const char *)chunk, n);
                }
                f.close();
                req->source = std::move(buf);
                req->loaded = true;
              }
            } else {
              req->error = true;
              req->error_msg = "ENOENT: " + path;
            }
          }

          // Store in cache under the mutex.
          xSemaphoreTake(mutex_, portMAX_DELAY);
          cache_[req->module_name] = req;
          xSemaphoreGive(mutex_);
        }
      }
    }

  public:
    void init(JSContext *ctx) {
      ctx_ = ctx;
      mutex_ = xSemaphoreCreateMutex();
      notify_ = xSemaphoreCreateBinary();
      // Create the reader task on core 0 (Arduino loop runs on core 1).
      // Stack 4096 is enough for fopen/fread; priority 1 = low.
      xTaskCreatePinnedToCore(readerTaskEntry, "js_modload",
                               4096, this, 1, &reader_task_, 0);
    }

    void end() {
      stopping_ = true;
      if (notify_) xSemaphoreGive(notify_);
      // Give the task a moment to exit.
      vTaskDelay(pdMS_TO_TICKS(50));
      if (reader_task_) {
        vTaskDelete(reader_task_);
        reader_task_ = nullptr;
      }
      if (mutex_) { vSemaphoreDelete(mutex_); mutex_ = nullptr; }
      if (notify_) { vSemaphoreDelete(notify_); notify_ = nullptr; }
      // Free cached namespaces.
      for (auto &kv : ns_cache_) {
        if (ctx_) JS_FreeValue(ctx_, kv.second);
      }
      ns_cache_.clear();
      for (auto &kv : cache_) delete kv.second;
      cache_.clear();
    }

    // ---- Static import path ----
    // Called by JS_SetModuleLoaderFunc callback. Returns a JSModuleDef*
    // by compiling the already-loaded source. If the source isn't loaded
    // yet, kicks off a background read and returns nullptr (QuickJS will
    // throw a ReferenceError; the module can be re-imported once loaded).
    // In practice, modules are pre-registered via registerModule() which
    // kicks off the read immediately, so by the time JS code runs the
    // source is usually already in cache_.
    JSModuleDef *loadStatic(JSContext *ctx, const char *module_name) {
      // Check cache.
      xSemaphoreTake(mutex_, portMAX_DELAY);
      auto it = cache_.find(module_name);
      bool has = (it != cache_.end()) && it->second->loaded;
      LoadRequest *req = has ? it->second : nullptr;
      xSemaphoreGive(mutex_);

      if (!has || !req) {
        // Not loaded yet — see if we have a registered path for it.
        // The module_cache map (in ESP32QuickJS) holds name→path.
        // We can't access it from here directly, so the loader callback
        // in setup() handles the path lookup and calls queueRead().
        return nullptr;
      }

      // Compile the source as a module (CPU-only, no I/O).
      JSValue func_val = JS_Eval(ctx, req->source.c_str(),
                                 req->source.size(), module_name,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
      if (JS_IsException(func_val)) {
        JS_FreeValue(ctx, func_val);
        return nullptr;
      }
      // The JSValue IS the module def pointer.
      JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
      // JS_Eval with COMPILE_ONLY returns a referenced value; the module
      // is already referenced internally by QuickJS, so we free our ref.
      JS_FreeValue(ctx, func_val);
      return m;
    }

    // ---- Synchronous HTTP fetch ----
    // Fetches a URL and stores the response body in `out`.
    // Blocks the JS thread while pumping the event loop (JSBlockingGuard)
    // so timers/WiFi/I2C/etc. all keep running.
    // Returns true on success, false on failure.
    bool fetchSync(JSContext *ctx, ESP32QuickJS *qjs,
                   const std::string &url, std::string &out) {
      if (WiFi.status() != WL_CONNECTED) return false;

      // Set up a global state object for the fetch result.
      JSValue g = JS_GetGlobalObject(ctx);
      JSValue state_obj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, state_obj, "done", JS_NewBool(ctx, false));
      JS_SetPropertyStr(ctx, state_obj, "ok", JS_NewBool(ctx, false));
      JS_SetPropertyStr(ctx, state_obj, "body", JS_NewString(ctx, ""));
      JS_SetPropertyStr(ctx, g, "__fetch_state", state_obj);
      JS_FreeValue(ctx, g);

      // Evaluate a script that calls WiFi.fetch and chains .then/.catch.
      std::string script = std::string(
        "globalThis.__fetch_state.done = false;\n"
        "globalThis.__fetch_state.ok = false;\n"
        "globalThis.__fetch_state.body = '';\n"
        "WiFi.fetch(\"") + url + "\").then(function(r) {\n"
        "  globalThis.__fetch_state.done = true;\n"
        "  globalThis.__fetch_state.ok = true;\n"
        "  globalThis.__fetch_state.body = r.body;\n"
        "}).catch(function(e) {\n"
        "  globalThis.__fetch_state.done = true;\n"
        "  globalThis.__fetch_state.ok = false;\n"
        "});\n";

      JSValue eval_ret = JS_Eval(ctx, script.c_str(), script.size(),
                                 "<fetch>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(eval_ret)) {
        qjs_dump_exception(ctx, eval_ret);
        JS_FreeValue(ctx, eval_ret);
        JSValue gg = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, gg, "__fetch_state", JS_UNDEFINED);
        JS_FreeValue(ctx, gg);
        return false;
      }
      JS_FreeValue(ctx, eval_ret);

      // Pump the event loop until the fetch completes.
      JSBlockingGuard guard(ctx, qjs, 10000);
      guard.wait([&]() {
        JSValue gg = JS_GetGlobalObject(ctx);
        JSValue st = JS_GetPropertyStr(ctx, gg, "__fetch_state");
        JSValue d = JS_GetPropertyStr(ctx, st, "done");
        bool isDone = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, gg);
        return isDone;
      });

      // Read the result.
      JSValue gg = JS_GetGlobalObject(ctx);
      JSValue st = JS_GetPropertyStr(ctx, gg, "__fetch_state");
      JSValue ok_val = JS_GetPropertyStr(ctx, st, "ok");
      JSValue body_val = JS_GetPropertyStr(ctx, st, "body");
      bool wasOk = JS_ToBool(ctx, ok_val);
      if (wasOk && JS_IsString(body_val)) {
        const char *str = JS_ToCString(ctx, body_val);
        if (str) {
          out = str;
          JS_FreeCString(ctx, str);
        }
      }
      JS_FreeValue(ctx, ok_val);
      JS_FreeValue(ctx, body_val);
      JS_FreeValue(ctx, st);
      JS_SetPropertyStr(ctx, gg, "__fetch_state", JS_UNDEFINED);
      JS_FreeValue(ctx, gg);

      return wasOk && !out.empty();
    }

    // ---- Synchronous require() path ----
    // Blocks the JS thread until the module is loaded, but pumps
    // ESP32QuickJS::loop() while waiting so timers/WiFi/I2C keep running.
    // The file read happens on the FreeRTOS reader task — the JS thread
    // never does blocking I/O. Returns the module namespace directly
    // (NOT a Promise), exactly like Node.js require().
    //
    // IMPORTANT: The actual JS_Eval (module parsing/evaluation) does NOT
    // happen here. It happens in loop() on the next tick. This avoids
    // deep call stacks (requireSync → evalModuleAndGetNS → JS_Eval) that
    // would overflow the FreeRTOS task stack. Instead:
    //   1. Queue the file read (background task)
    //   2. Wait for the read (pumping loop)
    //   3. Queue the module for evaluation in loop()
    //   4. Wait for the namespace to appear in ns_cache_ (pumping loop)
    JSValue requireSync(JSContext *ctx, ESP32QuickJS *qjs,
                        const char *module_name, const char *file_path) {
      // Check namespace cache first — already loaded module.
      auto nsit = ns_cache_.find(module_name);
      if (nsit != ns_cache_.end()) {
        return JS_DupValue(ctx, nsit->second);
      }

      // Check if source is already loaded in cache_.
      xSemaphoreTake(mutex_, portMAX_DELAY);
      auto sit = cache_.find(module_name);
      bool src_ready = (sit != cache_.end()) && sit->second->loaded;
      xSemaphoreGive(mutex_);

      if (!src_ready) {
        // Kick off a background read if not already pending.
        queueRead(module_name, file_path);

        // Wait for the file read to complete (pumping the event loop).
        JSBlockingGuard guard(ctx, qjs, 5000);
        if (!guard.wait([&]() { return isDone(module_name); })) {
          return JS_ThrowReferenceError(ctx,
            "require: timeout loading module '%s' from '%s'",
            module_name, file_path);
        }
      }

      // Get the source. If empty, the file wasn't found — try internet.
      std::string src = getSource(module_name);
      if (src.empty()) {
        if (WiFi.status() == WL_CONNECTED) {
          std::string url = "http://www.espruino.com/modules/";
          std::string mod = module_name;
          if (!mod.empty() && mod[0] == '/') mod = mod.substr(1);
          if (!fetchSync(ctx, qjs, url + mod + ".min.js", src) &&
              !fetchSync(ctx, qjs, url + mod + ".js", src)) {
            return JS_ThrowReferenceError(ctx,
              "require: module '%s' not found on filesystem or internet",
              module_name);
          }
        } else {
          return JS_ThrowReferenceError(ctx,
            "require: module '%s' not found (no WiFi for internet fetch)",
            module_name);
        }
      }

      // Enqueue a QuickJS job to evaluate the module. Jobs run via
      // JS_ExecutePendingJob at shallow stack depth (not nested inside
      // the requireSync call chain). This avoids stack overflow.
      // The job stores the namespace in ns_cache_.
      // We pass the source and module name via a heap-allocated struct.
      struct EvalJobData {
        JSModuleLoader *self;
        std::string module_name;
        std::string source;
      };
      EvalJobData *jobData = new EvalJobData();
      jobData->self = this;
      jobData->module_name = module_name;
      jobData->source = src;

      // JS_EnqueueJob takes a C function + argv. We pass the pointer
      // as a JSValue (external integer).
      JSValue jobArg = JS_MKPTR(JS_TAG_INT, jobData);
      JS_EnqueueJob(ctx, [](JSContext *ctx, int argc, JSValueConst *argv) -> JSValue {
        auto *data = (EvalJobData *)JS_VALUE_GET_PTR(argv[0]);
        JSValue ns = evalModuleAndGetNS(ctx, data->source, data->module_name.c_str());
        if (!JS_IsException(ns)) {
          data->self->ns_cache_[data->module_name] = JS_DupValue(ctx, ns);
        }
        JS_FreeValue(ctx, ns);
        delete data;
        return JS_UNDEFINED;
      }, 1, &jobArg);
      // JS_EnqueueJob may dup the argv, so free our ref.
      // Actually JS_EnqueueJob copies argv values, so we need to not
      // free jobArg since it's a pointer-tagged int (no refcount).
      // The job function will delete jobData.

      // Wait for the namespace to appear in ns_cache_.
      // JSBlockingGuard::tick() pumps JS_ExecutePendingJob which runs
      // the job at shallow stack depth, then pumps I/O.
      JSBlockingGuard guard2(ctx, qjs, 5000);
      if (!guard2.wait([&]() {
        return ns_cache_.find(module_name) != ns_cache_.end();
      })) {
        return JS_ThrowReferenceError(ctx,
          "require: timeout evaluating module '%s'", module_name);
      }

      return JS_DupValue(ctx, ns_cache_[module_name]);
    }

    // ---- Async require()/import() path ----
    // Queue a read and return a Promise. Resolved in loop().
    JSValue queueAsyncLoad(JSContext *ctx, const char *module_name,
                           const char *file_path) {
      // Check if we already have the namespace cached.
      auto nsit = ns_cache_.find(module_name);
      if (nsit != ns_cache_.end()) {
        // Already loaded — resolve immediately.
        JSValue resolving_funcs[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
        JSValue ns = JS_DupValue(ctx, nsit->second);
        JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &ns);
        JS_FreeValue(ctx, ns);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return promise;
      }

      // Check if source is already loaded in cache_.
      xSemaphoreTake(mutex_, portMAX_DELAY);
      auto sit = cache_.find(module_name);
      if (sit != cache_.end() && sit->second->loaded) {
        // Source is ready — we can evaluate right now in loop().
        xSemaphoreGive(mutex_);
        // Create a request that loop() will process (evaluate + resolve).
        LoadRequest *req = new LoadRequest();
        req->module_name = module_name;
        req->file_path = file_path;
        req->source = sit->second->source;
        req->loaded = true;
        JSValue promise = JS_NewPromiseCapability(ctx, req->resolving_funcs);
        req->has_promise = true;
        // Queue for loop() processing (not for the reader task).
        pending_eval_.push_back(req);
        return promise;
      }
      xSemaphoreGive(mutex_);

      // Need to read the file — create a request and queue it.
      LoadRequest *req = new LoadRequest();
      req->module_name = module_name;
      req->file_path = file_path;
      JSValue promise = JS_NewPromiseCapability(ctx, req->resolving_funcs);
      req->has_promise = true;

      // Queue for the background reader task.
      xSemaphoreTake(mutex_, portMAX_DELAY);
      pending_.push_back(req);
      xSemaphoreGive(mutex_);
      // Wake the reader task.
      xSemaphoreGive(notify_);

      return promise;
    }

    // Queue a read without a Promise (for pre-loading / static import).
    void queueRead(const char *module_name, const char *file_path) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      // Don't double-queue if already cached or pending.
      if (cache_.find(module_name) != cache_.end()) {
        xSemaphoreGive(mutex_);
        return;
      }
      for (auto *p : pending_) {
        if (p->module_name == module_name) {
          xSemaphoreGive(mutex_);
          return;
        }
      }
      xSemaphoreGive(mutex_);

      LoadRequest *req = new LoadRequest();
      req->module_name = module_name;
      req->file_path = file_path;
      req->is_static = true;

      xSemaphoreTake(mutex_, portMAX_DELAY);
      pending_.push_back(req);
      xSemaphoreGive(mutex_);
      xSemaphoreGive(notify_);
    }

    // Check if a module's source has been loaded (for static import).
    bool isLoaded(const char *module_name) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      auto it = cache_.find(module_name);
      bool has = (it != cache_.end()) && it->second->loaded;
      xSemaphoreGive(mutex_);
      return has;
    }

    // Check if a module load is done (loaded or errored).
    bool isDone(const char *module_name) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      auto it = cache_.find(module_name);
      bool done = (it != cache_.end()) && (it->second->loaded || it->second->error);
      xSemaphoreGive(mutex_);
      return done;
    }

    // Get cached source (for static import compilation).
    std::string getSource(const char *module_name) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      auto it = cache_.find(module_name);
      std::string src = (it != cache_.end()) ? it->second->source : "";
      xSemaphoreGive(mutex_);
      return src;
    }

  private:
    // Requests whose source is loaded and need evaluation + promise resolution.
    // Processed in loop() and by JSBlockingGuard::tick() on the JS thread.
    std::vector<LoadRequest*> pending_eval_;

    // Process pending_eval_ entries — evaluate modules and cache namespaces.
    // Called from both loop() (normal operation) and tick() (blocking waits).
    // This is the "heavy lifting" — JS_Eval — and it runs on the main loop
    // task which has a large stack (32KB).
  public:
    void processPendingEval(JSContext *ctx) {
      for (auto *req : pending_eval_) {
        JSValue ns = evalModuleAndGetNS(ctx, req->source, req->module_name.c_str());
        if (JS_IsException(ns)) {
          if (req->has_promise) {
            JSValue exc = JS_GetException(ctx);
            JS_Call(ctx, req->resolving_funcs[1], JS_UNDEFINED, 1, &exc);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, req->resolving_funcs[0]);
            JS_FreeValue(ctx, req->resolving_funcs[1]);
          }
          JS_FreeValue(ctx, ns);
          delete req;
          continue;
        }
        ns_cache_[req->module_name] = JS_DupValue(ctx, ns);
        if (req->has_promise) {
          JS_Call(ctx, req->resolving_funcs[0], JS_UNDEFINED, 1, &ns);
          JS_FreeValue(ctx, req->resolving_funcs[0]);
          JS_FreeValue(ctx, req->resolving_funcs[1]);
        }
        JS_FreeValue(ctx, ns);
        delete req;
      }
      pending_eval_.clear();
    }

    // Compile + evaluate a module from source, return its namespace.
    // Returns JS_EXCEPTION on error.
    //
    // We use JS_Eval with JS_EVAL_TYPE_MODULE to compile and evaluate
    // the module. Then we access the module_ns field from the
    // JSModuleDef struct. The struct is opaque in the public API, so
    // we replicate the layout to access module_ns. If module_ns is
    // still JS_UNDEFINED (not yet built), we trigger the build by
    // evaluating a wrapper import that forces js_get_module_ns() to
    // run, which populates the field.
    // Evaluate a module from source and return its namespace/exports.
    // Handles both ES modules (export/import) and CommonJS modules
    // (exports.foo = ..., module.exports = ...).
    static JSValue evalModuleAndGetNS(JSContext *ctx, const std::string &src,
                                      const char *module_name) {
      // Detect whether the source is an ES module or CommonJS.
      bool is_es_module = JS_DetectModule(src.c_str(), src.size());

      if (is_es_module) {
        return evalESModuleAndGetNS(ctx, src, module_name);
      } else {
        return evalCommonJSAndGetNS(ctx, src, module_name);
      }
    }

  private:
    // ES module path: compile, evaluate, extract namespace.
    static JSValue evalESModuleAndGetNS(JSContext *ctx, const std::string &src,
                                        const char *module_name) {
      // Step 1: Compile and evaluate the module.
      JSValue func_val = JS_Eval(ctx, src.c_str(), src.size(), module_name,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
      if (JS_IsException(func_val)) {
        JS_FreeValue(ctx, func_val);
        return JS_EXCEPTION;
      }

      // Get the JSModuleDef pointer.
      JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);

      // Evaluate the module (resolves imports, runs module code).
      // JS_EvalFunction consumes func_val.
      JSValue eval_ret = JS_EvalFunction(ctx, func_val);
      if (JS_IsException(eval_ret)) {
        JS_FreeValue(ctx, eval_ret);
        return JS_EXCEPTION;
      }
      JS_FreeValue(ctx, eval_ret);

      // Step 2: Trigger namespace creation by evaluating a wrapper
      // import. This forces js_get_module_ns() to run, which builds
      // and caches the namespace in m->module_ns.
      std::string wrapper = std::string("import * as __ns from \"") +
                            module_name + "\"";
      JSValue wrap_ret = JS_Eval(ctx, wrapper.c_str(), wrapper.size(),
                                 "<ns-trigger>", JS_EVAL_TYPE_MODULE);
      if (JS_IsException(wrap_ret)) {
        JS_FreeValue(ctx, wrap_ret);
        return JS_EXCEPTION;
      }
      JS_FreeValue(ctx, wrap_ret);

      // Step 3: Now access module_ns from the JSModuleDef struct.
      // The namespace was built by the wrapper import above.
      // We replicate the struct layout to access the module_ns field.
      struct JSModuleDefLayout {
        JSRefCountHeader header;
        JSAtom module_name;
        void *link[2];  // struct list_head (prev, next)
        void *req_module_entries;
        int req_module_entries_count;
        int req_module_entries_size;
        void *export_entries;
        int export_entries_count;
        int export_entries_size;
        void *star_export_entries;
        int star_export_entries_count;
        int star_export_entries_size;
        void *import_entries;
        int import_entries_count;
        int import_entries_size;
        JSValue module_ns;
      };
      auto *mod = reinterpret_cast<JSModuleDefLayout*>(m);
      if (JS_IsUndefined(mod->module_ns)) {
        return JS_ThrowInternalError(ctx,
          "require: module namespace not built for '%s'", module_name);
      }
      return JS_DupValue(ctx, mod->module_ns);
    }

    // CommonJS path: evaluate source as a script with `exports` and
    // `module` in scope, return the exports object.
    // We prepend `var exports = {}, module = {exports: exports};` to
    // the source and evaluate as a global script. This avoids the
    // stack overhead of a function wrapper.
    // Supports: exports.foo = ..., module.exports = ..., exports = ...
    static JSValue evalCommonJSAndGetNS(JSContext *ctx, const std::string &src,
                                        const char *module_name) {
      std::string wrapped = std::string(
        "var exports = {};\n"
        "var module = { exports: exports };\n"
        "var require = globalThis.require;\n") +
        src + "\n";

      JSValue eval_ret = JS_Eval(ctx, wrapped.c_str(), wrapped.size(),
                                 module_name, JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(eval_ret)) {
        qjs_dump_exception(ctx, eval_ret);
        JS_FreeValue(ctx, eval_ret);
        return JS_EXCEPTION;
      }
      JS_FreeValue(ctx, eval_ret);

      // Read back module.exports from the global scope.
      JSValue global = JS_GetGlobalObject(ctx);
      JSValue mod = JS_GetPropertyStr(ctx, global, "module");
      JS_FreeValue(ctx, global);
      if (JS_IsException(mod) || JS_IsUndefined(mod)) {
        JS_FreeValue(ctx, mod);
        return JS_ThrowInternalError(ctx,
          "require: CommonJS module '%s' did not define module", module_name);
      }
      JSValue mod_exports = JS_GetPropertyStr(ctx, mod, "exports");
      JS_FreeValue(ctx, mod);

      if (JS_IsObject(mod_exports) || JS_IsString(mod_exports) ||
          JS_IsNumber(mod_exports)) {
        return mod_exports;
      }
      JS_FreeValue(ctx, mod_exports);
      return JS_ThrowInternalError(ctx,
        "require: CommonJS module '%s' did not set exports", module_name);
    }

  public:
    // Called from ESP32QuickJS::loop() on the JS thread.
    // Evaluates loaded modules and resolves/rejects their Promises.
    void loop(JSContext *ctx) {
      // Check if any pending reads have completed (for async require/import).
      xSemaphoreTake(mutex_, portMAX_DELAY);
      std::vector<LoadRequest*> ready;
      // Pull completed requests that have promises.
      for (auto it = pending_.begin(); it != pending_.end(); ) {
        LoadRequest *req = *it;
        if (req->loaded || req->error) {
          // Move to ready list for JS-thread processing.
          ready.push_back(req);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
      xSemaphoreGive(mutex_);

      // Process ready requests on the JS thread.
      for (auto *req : ready) {
        if (req->error) {
          if (req->has_promise) {
            JSValue err = JS_NewString(ctx, req->error_msg.c_str());
            JS_Call(ctx, req->resolving_funcs[1], JS_UNDEFINED, 1, &err);
            JS_FreeValue(ctx, err);
            JS_FreeValue(ctx, req->resolving_funcs[0]);
            JS_FreeValue(ctx, req->resolving_funcs[1]);
          }
          delete req;
          continue;
        }

        // Source is loaded — evaluate as module and get namespace.
        JSValue ns = evalModuleAndGetNS(ctx, req->source, req->module_name.c_str());

        if (JS_IsException(ns)) {
          if (req->has_promise) {
            JSValue exc = JS_GetException(ctx);
            JS_Call(ctx, req->resolving_funcs[1], JS_UNDEFINED, 1, &exc);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, req->resolving_funcs[0]);
            JS_FreeValue(ctx, req->resolving_funcs[1]);
          }
          JS_FreeValue(ctx, ns);
          delete req;
          continue;
        }

        // Cache the namespace for future require() calls.
        ns_cache_[req->module_name] = JS_DupValue(ctx, ns);

        // Resolve the promise (if we have one).
        if (req->has_promise) {
          JS_Call(ctx, req->resolving_funcs[0], JS_UNDEFINED, 1, &ns);
          JS_FreeValue(ctx, req->resolving_funcs[0]);
          JS_FreeValue(ctx, req->resolving_funcs[1]);
        }
        JS_FreeValue(ctx, ns);
        delete req;
      }

      // Also process any pending_eval_ requests (source already loaded).
      // These come from requireSync() (has_promise=false) and
      // queueAsyncLoad() (has_promise=true).
      processPendingEval(ctx);
    }
  };

  JSModuleLoader module_loader;

  // registerModule(name, path): register a module by name and filesystem path.
  // Kicks off a background read immediately so the source is in RAM by the
  // time JS code imports it. This is non-blocking — the read happens on the
  // FreeRTOS reader task.
  void registerModule(const char *name, const char *path) {
    module_cache[name] = path;
    module_loader.queueRead(name, path);
  }

 protected:
  void setLoopFunc(JSValue f) {
    JS_FreeValue(ctx, loop_func);
    loop_func = f;
  }

  virtual void setup(JSContext *ctx, JSValue global) {
    this->ctx = ctx;
    JS_SetContextOpaque(ctx, this);

    // ---- Module loader ----
    // QuickJS calls this callback when it encounters a static `import`
    // statement. The callback MUST return a JSModuleDef* synchronously.
    // We compile the already-loaded source (pre-read by the FreeRTOS
    // reader task in JSModuleLoader). If the source isn't loaded yet,
    // we kick off a background read and return nullptr — QuickJS will
    // throw a ReferenceError, but the module will be available for the
    // next import attempt. In practice, registerModule() pre-reads
    // modules at registration time, so they're ready before JS runs.
    //
    // The callback also handles relative path resolution: if the
    // module_name isn't in module_cache, we try resolving it as a
    // relative path from the filesystem root.
    JS_SetModuleLoaderFunc(
        rt,
        nullptr,  // default normalizer
        [](JSContext *ctx, const char *module_name, void *opaque) -> JSModuleDef* {
            ESP32QuickJS* qjs = (ESP32QuickJS*)opaque;
            // Check if source is already loaded in the module loader cache.
            if (qjs->module_loader.isLoaded(module_name)) {
              return qjs->module_loader.loadStatic(ctx, module_name);
            }
            // Not loaded — look up the path and kick off a background read.
            auto it = qjs->module_cache.find(module_name);
            if (it != qjs->module_cache.end()) {
              qjs->module_loader.queueRead(module_name, it->second.c_str());
              // If it's already loaded by now (race: reader finished between
              // isLoaded and queueRead), try once more.
              if (qjs->module_loader.isLoaded(module_name)) {
                return qjs->module_loader.loadStatic(ctx, module_name);
              }
            }
            return nullptr;  // QuickJS will throw ReferenceError
        },
        this);

    // setup console.log()
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, console_log, "log", 1));

    // Memory introspection + manual GC. Useful when scripts OOM or
    // you want to watch heap pressure during long-running tests.
    JS_SetPropertyStr(ctx, global, "gc",
                      JS_NewCFunction(ctx, run_gc, "gc", 0));
    JS_SetPropertyStr(ctx, global, "freeHeap",
                      JS_NewCFunction(ctx, free_heap, "freeHeap", 0));
    JS_SetPropertyStr(ctx, global, "jsMemUsage",
                      JS_NewCFunction(ctx, js_mem_usage, "jsMemUsage", 0));

    // timer
    JS_SetPropertyStr(ctx, global, "setTimeout",
                      JS_NewCFunction(ctx, set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                      JS_NewCFunction(ctx, clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "setInterval",
                      JS_NewCFunction(ctx, set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                      JS_NewCFunction(ctx, clear_timeout, "clearInterval", 1));

    // Module system: async require() and import() that return Promises.
    // Usage:
    //   const mod = await require("./fib.js")
    //   const { fib } = await import("./fib.js")
    // Both are non-blocking — the file read happens on a FreeRTOS task.
    JS_SetPropertyStr(ctx, global, "require",
                      JS_NewCFunction(ctx, js_require, "require", 1));
    JS_SetPropertyStr(ctx, global, "importModule",
                      JS_NewCFunction(ctx, js_import, "importModule", 1));


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
        JSCFunctionListEntry{"startDNS", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, wifi_start_dns}
                             }},
        JSCFunctionListEntry{"stopDNS", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, wifi_stop_dns}
                             }},
        JSCFunctionListEntry{"startMDNS", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, wifi_start_mdns}
                             }},
        JSCFunctionListEntry{"addMDNSService", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, wifi_add_mdns_service}
                             }},
        JSCFunctionListEntry{"scan", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, wifi_scan}
                             }},
        JSCFunctionListEntry{"syncNTP", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, wifi_sync_ntp}
                             }},
    };
    JS_SetPropertyFunctionList(ctx, wifi, wifi_funcs, sizeof(wifi_funcs) / sizeof(JSCFunctionListEntry));
    // Do not free wifi here, it is owned by the global object

    // Expose fetch as a global function (alias of WiFi.fetch).
    JS_SetPropertyStr(ctx, global, "fetch",
                      JS_NewCFunction(ctx, http_fetch, "fetch", 2));

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

    // OneWire = { setup, reset, write, read, search, skip, select, depower }
    JSValue owObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "OneWire", owObj);
    static const JSCFunctionListEntry ow_funcs[] = {
        JSCFunctionListEntry{"setup", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, onewire_setup}
                             }},
        JSCFunctionListEntry{"reset", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, onewire_reset}
                             }},
        JSCFunctionListEntry{"write", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, onewire_write}
                             }},
        JSCFunctionListEntry{"read", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, onewire_read}
                             }},
        JSCFunctionListEntry{"search", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, onewire_search}
                             }},
        JSCFunctionListEntry{"skip", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, onewire_skip}
                             }},
        JSCFunctionListEntry{"select", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, onewire_select}
                             }},
        JSCFunctionListEntry{"depower", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, onewire_depower}
                             }},
    };
    JS_SetPropertyFunctionList(ctx, owObj, ow_funcs,
                               sizeof(ow_funcs) / sizeof(JSCFunctionListEntry));

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
        JSCFunctionListEntry{"servoWrite", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, esp32_servo_write}
                             }},
        JSCFunctionListEntry{"tone", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, esp32_tone}
                             }},
        JSCFunctionListEntry{"playRTTTL", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, esp32_play_rtttl}
                             }},
        JSCFunctionListEntry{"deepSleep", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, esp32_deep_sleep}
                             }},
        JSCFunctionListEntry{"setLoop", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, esp32_set_loop}
                             }},
        JSCFunctionListEntry{"dacWrite", 0, JS_DEF_CFUNC, 0, {
                               func : {2, JS_CFUNC_generic, esp32_dac_write}
                             }},
        JSCFunctionListEntry{"setWatch", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, esp32_set_watch}
                             }},
        JSCFunctionListEntry{"clearWatch", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, esp32_clear_watch}
                             }},
        JSCFunctionListEntry{"getTime", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, esp32_get_time}
                             }},
        JSCFunctionListEntry{"setTime", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, esp32_set_time}
                             }},
        JSCFunctionListEntry{"getSerial", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, esp32_get_serial}
                             }},
        JSCFunctionListEntry{"Serial1", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, esp32_serial1}
                             }},
        JSCFunctionListEntry{"Serial2", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, esp32_serial2}
                             }},
    };

    // I2C = { open, close, write, read, writeRead }
    // All functions are async; they return Promises that resolve when the
    // queued operation completes in ESP32QuickJS::loop(). Bus handles are
    // 0-based indices into the i2c.buses vector.
    {
      JSValue i2cObj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, global, "I2C", i2cObj);
      static const JSCFunctionListEntry i2c_funcs[] = {
          JSCFunctionListEntry{"open", 0, JS_DEF_CFUNC, 0, {
              func : {1, JS_CFUNC_generic, JSI2C::js_open}
          }},
          JSCFunctionListEntry{"close", 0, JS_DEF_CFUNC, 0, {
              func : {1, JS_CFUNC_generic, JSI2C::js_close}
          }},
          JSCFunctionListEntry{"write", 0, JS_DEF_CFUNC, 0, {
              func : {3, JS_CFUNC_generic, JSI2C::js_write}
          }},
          JSCFunctionListEntry{"read", 0, JS_DEF_CFUNC, 0, {
              func : {3, JS_CFUNC_generic, JSI2C::js_read}
          }},
          JSCFunctionListEntry{"writeRead", 0, JS_DEF_CFUNC, 0, {
              func : {4, JS_CFUNC_generic, JSI2C::js_write_read}
          }},
      };
      JS_SetPropertyFunctionList(ctx, i2cObj, i2c_funcs,
                                 sizeof(i2c_funcs) / sizeof(JSCFunctionListEntry));
    }
    // SPI = { open, close, transfer, write, read }
    // All functions are async; they return Promises that resolve when the
    // queued operation completes in ESP32QuickJS::loop(). Device handles
    // are 0-based indices into the spi.devices vector.
    {
      JSValue spiObj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, global, "SPI", spiObj);
      static const JSCFunctionListEntry spi_funcs[] = {
          JSCFunctionListEntry{"open", 0, JS_DEF_CFUNC, 0, {
              func : {1, JS_CFUNC_generic, JSSPI::js_open}
          }},
          JSCFunctionListEntry{"close", 0, JS_DEF_CFUNC, 0, {
              func : {1, JS_CFUNC_generic, JSSPI::js_close}
          }},
          JSCFunctionListEntry{"transfer", 0, JS_DEF_CFUNC, 0, {
              func : {2, JS_CFUNC_generic, JSSPI::js_transfer}
          }},
          JSCFunctionListEntry{"write", 0, JS_DEF_CFUNC, 0, {
              func : {2, JS_CFUNC_generic, JSSPI::js_write}
          }},
          JSCFunctionListEntry{"read", 0, JS_DEF_CFUNC, 0, {
              func : {2, JS_CFUNC_generic, JSSPI::js_read}
          }},
      };
      JS_SetPropertyFunctionList(ctx, spiObj, spi_funcs,
                                 sizeof(spi_funcs) / sizeof(JSCFunctionListEntry));
    }
    // servo implementation was completely wrong... removing
    // // servo = { attach, detach, write, writeUs }
    // // All four return Promises; the actual MCPWM setup runs in loop().
    // {
    //   JSValue servoObj = JS_NewObject(ctx);
    //   JS_SetPropertyStr(ctx, global, "servo", servoObj);
    //   static const JSCFunctionListEntry servo_funcs[] = {
    //       JSCFunctionListEntry{"attach", 0, JS_DEF_CFUNC, 0, {
    //           func : {2, JS_CFUNC_generic, JSServo::js_attach}
    //       }},
    //       JSCFunctionListEntry{"detach", 0, JS_DEF_CFUNC, 0, {
    //           func : {1, JS_CFUNC_generic, JSServo::js_detach}
    //       }},
    //       JSCFunctionListEntry{"write", 0, JS_DEF_CFUNC, 0, {
    //           func : {2, JS_CFUNC_generic, JSServo::js_write}
    //       }},
    //       JSCFunctionListEntry{"writeUs", 0, JS_DEF_CFUNC, 0, {
    //           func : {2, JS_CFUNC_generic, JSServo::js_writeUs}
    //       }},
    //   };
    //   JS_SetPropertyFunctionList(ctx, servoObj, servo_funcs,
    //                              sizeof(servo_funcs) / sizeof(JSCFunctionListEntry));
    //}

    {
      // Register the RotaryEncoder class. Pattern follows the Worker
      // class in quickjs-libc.c:
      //   1. Allocate a class id (once per process).
      //   2. Register the class definition with the runtime.
      //   3. Build a prototype object with the methods.
      //   4. Build a constructor function and link it to the prototype.
      //   5. Bind the prototype to the class id so JS_NewObjectClass
      //      picks it up automatically.
      //   6. Expose the constructor as a global so JS can `new` it.
      JSRuntime *rt = JS_GetRuntime(ctx);
      JS_NewClassID(&JSRotaryEncoder::js_class_id);
      JS_NewClass(rt, JSRotaryEncoder::js_class_id,
                  &JSRotaryEncoder::js_class_def);

      JSValue proto = JS_NewObject(ctx);
      JS_SetPropertyStr(
          ctx, proto, "position",
          JS_NewCFunction(ctx, JSRotaryEncoder::js_position, "position", 1));

      JSValue ctor = JS_NewCFunction2(ctx, JSRotaryEncoder::js_ctor,
                                      "RotaryEncoder", 2,
                                      JS_CFUNC_constructor, 0);
      JS_SetConstructor(ctx, ctor, proto);
      JS_SetClassProto(ctx, JSRotaryEncoder::js_class_id, proto);

      JS_SetPropertyStr(ctx, global, "RotaryEncoder", ctor);
    }

    {
      // Register the MotorDriver class. Same pattern as RotaryEncoder.
      JSRuntime *rt = JS_GetRuntime(ctx);
      JS_NewClassID(&JSMotorDriver::js_class_id);
      JS_NewClass(rt, JSMotorDriver::js_class_id,
                  &JSMotorDriver::js_class_def);

      JSValue proto = JS_NewObject(ctx);
      JS_SetPropertyStr(
          ctx, proto, "speed",
          JS_NewCFunction(ctx, JSMotorDriver::js_speed, "speed", 1));
      JS_SetPropertyStr(
          ctx, proto, "position",
          JS_NewCFunction(ctx, JSMotorDriver::js_position, "position", 0));
      JS_SetPropertyStr(
          ctx, proto, "moveTo",
          JS_NewCFunction(ctx, JSMotorDriver::js_moveTo, "moveTo", 2));
      JS_SetPropertyStr(
          ctx, proto, "stop",
          JS_NewCFunction(ctx, JSMotorDriver::js_stop, "stop", 0));

      JSValue ctor = JS_NewCFunction2(ctx, JSMotorDriver::js_ctor,
                                      "MotorDriver", 4,
                                      JS_CFUNC_constructor, 0);
      JS_SetConstructor(ctx, ctor, proto);
      JS_SetClassProto(ctx, JSMotorDriver::js_class_id, proto);

      JS_SetPropertyStr(ctx, global, "MotorDriver", ctor);
    }

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

  // ---- Module system: require() and import() ----
  //
  // require(name): Synchronous, just like Node.js. Blocks the calling JS
  // code until the module is loaded and returns the module namespace
  // directly (NOT a Promise). While waiting for the background file
  // read to complete, it pumps ESP32QuickJS::loop() so that timers,
  // WiFi, I2C, pending jobs, etc. all keep running. The file I/O itself
  // happens on a FreeRTOS task — the JS thread never does blocking I/O.
  //   const fib = require("./fib.js")   // blocks until loaded, returns module
  //
  // importModule(name): ES dynamic import — returns a Promise per spec.
  //   const { fib } = await importModule("./fib.js")
  //
  static JSValue js_require(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
      return JS_ThrowTypeError(ctx, "require: expected module name string");
    }
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs) return JS_ThrowInternalError(ctx, "require: no context opaque");
    const char *module_name = JS_ToCString(ctx, argv[0]);
    if (!module_name) return JS_EXCEPTION;

    // Look up the filesystem path for this module name.
    auto it = qjs->module_cache.find(module_name);
    std::string path;
    if (it != qjs->module_cache.end()) {
      path = it->second;
    } else {
      // If not registered, treat the name itself as the path.
      path = module_name;
    }
    std::string name_str = module_name;
    JS_FreeCString(ctx, module_name);

    // Synchronous require — blocks JS, pumps event loop while waiting.
    return qjs->module_loader.requireSync(ctx, qjs, name_str.c_str(), path.c_str());
  }

  // importModule(name): ES dynamic import — returns a Promise (async).
  static JSValue js_import(JSContext *ctx, JSValueConst jsThis, int argc,
                           JSValueConst *argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
      return JS_ThrowTypeError(ctx, "importModule: expected module name string");
    }
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs) return JS_ThrowInternalError(ctx, "importModule: no context opaque");
    const char *module_name = JS_ToCString(ctx, argv[0]);
    if (!module_name) return JS_EXCEPTION;
    auto it = qjs->module_cache.find(module_name);
    std::string path = (it != qjs->module_cache.end()) ? it->second : module_name;
    std::string name_str = module_name;
    JS_FreeCString(ctx, module_name);
    return qjs->module_loader.queueAsyncLoad(ctx, name_str.c_str(), path.c_str());
  }

  // ---- Memory introspection ----
  // gc(): force a QuickJS garbage collection cycle. Returns the number
  // of bytes freed (rounded). Useful before OOM-prone operations or to
  // measure transient allocations.
  static JSValue run_gc(JSContext *ctx, JSValueConst jsThis, int argc,
                        JSValueConst *argv) {
    ESP32QuickJS* qjs = (ESP32QuickJS*)JS_GetContextOpaque(ctx);
    if (!qjs) return JS_ThrowInternalError(ctx, "gc: no context opaque");
    JSMemoryUsage before;
    JS_ComputeMemoryUsage(qjs->rt, &before);
    JS_RunGC(qjs->rt);
    JSMemoryUsage after;
    JS_ComputeMemoryUsage(qjs->rt, &after);
    int64_t freed = (int64_t)before.memory_used_count - (int64_t)after.memory_used_count;
    return JS_NewInt64(ctx, freed > 0 ? freed : 0);
  }

  // freeHeap(): total free heap on the ESP32 (Arduino's ESP.getFreeHeap()).
  // This is the physical heap; the JS engine has its own ceiling set
  // by JS_SetMemoryLimit (typically `freeHeap - 32KB`). The two are
  // related but not identical: JS heap allocations come out of the
  // physical heap, but the JS ceiling is the maximum the engine will
  // request before throwing InternalError.
  static JSValue free_heap(JSContext *ctx, JSValueConst jsThis, int argc,
                           JSValueConst *argv) {
    return JS_NewUint32(ctx, ESP.getFreeHeap());
  }

  // jsMemUsage(): snapshot of the QuickJS heap accounting. Returns an
  // object with { used, limit, malloc_count, malloc_size }. `used` is
  // the live JS heap; `limit` is the ceiling from JS_SetMemoryLimit.
  static JSValue js_mem_usage(JSContext *ctx, JSValueConst jsThis, int argc,
                              JSValueConst *argv) {
    ESP32QuickJS* qjs = (ESP32QuickJS*)JS_GetContextOpaque(ctx);
    if (!qjs) return JS_ThrowInternalError(ctx, "jsMemUsage: no context opaque");
    JSMemoryUsage m;
    JS_ComputeMemoryUsage(qjs->rt, &m);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "used", JS_NewInt64(ctx, (int64_t)m.memory_used_count));
    JS_SetPropertyStr(ctx, obj, "malloc_limit", JS_NewInt64(ctx, (int64_t)m.malloc_limit));
    JS_SetPropertyStr(ctx, obj, "malloc_count", JS_NewInt64(ctx, (int64_t)m.malloc_count));
    JS_SetPropertyStr(ctx, obj, "malloc_size", JS_NewInt64(ctx, (int64_t)m.malloc_size));
    JS_SetPropertyStr(ctx, obj, "atoms", JS_NewInt64(ctx, (int64_t)m.atom_count));
    JS_SetPropertyStr(ctx, obj, "strings", JS_NewInt64(ctx, (int64_t)m.str_count));
    JS_SetPropertyStr(ctx, obj, "objects", JS_NewInt64(ctx, (int64_t)m.obj_count));
    JS_SetPropertyStr(ctx, obj, "arrays", JS_NewInt64(ctx, (int64_t)m.array_count));
    JS_SetPropertyStr(ctx, obj, "fast_arrays", JS_NewInt64(ctx, (int64_t)m.fast_array_count));
    JS_SetPropertyStr(ctx, obj, "closures", JS_NewInt64(ctx, (int64_t)m.js_func_count));
    return obj;
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
    // Refuse to read a pin that is being driven by analogWrite or a
    // servo. The hardware read would either return the driven value
    // (misleading) or fight the driver. Call writeAnalogStop (or
    // servo.detach) first.
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (qjs && (qjs->analog.isPinBusy((uint8_t)pin)
               || qjs->servo.isPinBusy((uint8_t)pin)
        )) {
      return JS_ThrowReferenceError(ctx,
        "digitalRead: pin %u is in use by analog/servo output", (unsigned)pin);
    }
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
  // Returns a Promise<number> (the ADC reading). Rejects with a
  // ReferenceError if the pin is in use by analogWrite or a servo.
  static JSValue esp32_analog_read(JSContext *ctx, JSValueConst jsThis,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) {
      return JS_ThrowTypeError(ctx, "analogRead: need (pin)");
    }
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (qjs && (qjs->analog.isPinBusy((uint8_t)pin)
               || qjs->servo.isPinBusy((uint8_t)pin)
        )) {
      // Return a Promise that rejects immediately, mirroring the
      // normal "queued then rejected" path. The caller can .catch()
      // it instead of needing a try/catch.
      JSValue resolving_funcs[2];
      JSValue p = JS_NewPromiseCapability(ctx, resolving_funcs);
      char msg[80];
      snprintf(msg, sizeof(msg),
               "analogRead: pin %u is in use by analog/servo output", (unsigned)pin);
      JSValue err = JS_NewString(ctx, msg);
      JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1, &err);
      JS_FreeValue(ctx, err);
      JS_FreeValue(ctx, resolving_funcs[0]);
      JS_FreeValue(ctx, resolving_funcs[1]);
      return p;
    }
    // Forward to the queued read path (returns a Promise<number>).
    return qjs->analog.js_readAnalog(ctx, argc, argv);
  }

  static JSValue esp32_servo_write(JSContext *ctx, JSValueConst jsThis, int argc,
                                   JSValueConst *argv) {
    // servoWrite(pin, degrees)
    // Maps 0-180° to 544-2400µs pulse at 50Hz, 14-bit, using the LEDC
    // pool via analog.js_writeAnalog.
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "servoWrite: need (pin, degrees)");
    }
    uint32_t pin, degrees;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &degrees, argv[1]);
    if (degrees > 180) degrees = 180;

    // Standard servo pulse range (544-2400µs matches Arduino Servo.h).
    // Pulse width in µs: 544 + (degrees / 180) * (2400 - 544)
    // Duty for 14-bit @ 50Hz (20ms period): pulseUs * 16384 / 20000
    uint32_t pulseUs = 544 + (degrees * 1856) / 180;
    uint32_t duty = (pulseUs * 16384) / 20000;

    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);

    // Abort RTTTL if playing on this pin.
    RTTTLState *rt = qjs->rtttlFind((uint8_t)pin);
    if (rt) rt->gen++;

    JSValue wargv[4] = {
      JS_DupValue(ctx, argv[0]),
      JS_NewUint32(ctx, duty),
      JS_NewUint32(ctx, 50),
      JS_NewUint32(ctx, 14),
    };
    JSValue ret = qjs->analog.js_writeAnalog(ctx, 4, (JSValueConst*)wargv);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, wargv[i]);
    return ret;
  }

  // esp32.tone(pin, frequency) → Promise<void>
  // Square-wave tone via the LEDC pool (50% duty, 10-bit).
  static JSValue esp32_tone(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "tone: need (pin, frequency)");
    }
    uint32_t pin, frequency;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &frequency, argv[1]);

    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);

    // Abort RTTTL if playing on this pin.
    RTTTLState *rt = qjs->rtttlFind((uint8_t)pin);
    if (rt) rt->gen++;

    JSValue wargv[4] = {
      JS_NewUint32(ctx, pin),
      JS_NewUint32(ctx, 511),         // 50% duty (10-bit)
      JS_NewUint32(ctx, frequency),
      JS_NewUint32(ctx, 10),
    };
    JSValue ret = qjs->analog.js_writeAnalog(ctx, 4, (JSValueConst*)wargv);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, wargv[i]);
    return ret;
  }

  // analogWrite(pin, fraction, [freq=5000])
  // Arduino-style API. fraction is 0.0..1.0.
  //   - fraction == 0.0  -> digitalWrite(pin, LOW); if the pin was
  //     previously a PWM pin, detach the LEDC channel and free it
  //     (auto-attach/detach; the LEDC pool stays clean).
  //   - fraction == 1.0  -> digitalWrite(pin, HIGH); same detach.
  //   - 0 < fraction < 1  -> forward to the regular PWM path.
  // digitalWrite and ledcDetachPin are register writes — they run
  // synchronously here so the JS caller's expectation of "fraction
  // is now applied" is honored immediately. The returned Promise
  // resolves with a synthesized { channel: 0 } for symmetry.
  static JSValue esp32_analog_write(JSContext *ctx, JSValueConst jsThis,
                                    int argc, JSValueConst *argv) {
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "analogWrite: need (pin, fraction, [freq])");
    }
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    double frac = 0;
    JS_ToFloat64(ctx, &frac, argv[1]);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    if (!qjs) return JS_ThrowInternalError(ctx, "analogWrite: no context opaque");

    // Abort RTTTL if playing on this pin.
    RTTTLState *rt = qjs->rtttlFind((uint8_t)pin);
    if (rt) rt->gen++;

    if (frac <= 0.0 || frac >= 1.0) {
      // Edge case: 0% or 100% — use digitalWrite, detach PWM.
      // If the pin was previously a PWM pin, free the LEDC channel
      // (auto-attach/detach; the LEDC pool stays clean).
      qjs->analog.detachPwmIfAttached((uint8_t)pin);
      digitalWrite(pin, frac >= 1.0 ? HIGH : LOW);
      // Return a resolved Promise for API symmetry with the PWM path.
      JSValue resolving_funcs[2];
      JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
      JSValue v = JS_NewBool(ctx, true);
      JS_Call(ctx, resolving_funcs[0], JS_UNDEFINED, 1, &v);
      JS_FreeValue(ctx, v);
      JS_FreeValue(ctx, resolving_funcs[0]);
      JS_FreeValue(ctx, resolving_funcs[1]);
      return promise;
    }

    uint32_t duty = (uint32_t)(frac * 255.0 + 0.5);
    uint32_t freq = 5000;
    if (argc >= 3) JS_ToUint32(ctx, &freq, argv[2]);
    JSValue wargv[4] = {
      JS_DupValue(ctx, argv[0]),
      JS_NewUint32(ctx, duty),
      JS_NewUint32(ctx, freq),
      JS_NewUint32(ctx, 8),
    };
    JSValue ret = qjs->analog.js_writeAnalog(ctx, 4, (JSValueConst*)wargv);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, wargv[i]);
    return ret;
  }

  // esp32.playRTTTL(pin, rtttlString, [onComplete]) → void
  // Parses an RTTTL string into a note list and starts non-blocking
  // playback. Each note is driven via analog.js_writeAnalog (50% duty,
  // 10-bit). Playback advances from loop() based on millis() timing.
  // If any write function is called on the same pin, playback aborts.
  // onComplete is called (no args) when the song finishes naturally.
  static JSValue esp32_play_rtttl(JSContext *ctx, JSValueConst jsThis,
                                  int argc, JSValueConst *argv) {
    if (argc < 2) {
      return JS_ThrowTypeError(ctx, "playRTTTL: need (pin, rtttlString, [onComplete])");
    }
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    const char *str = JS_ToCString(ctx, argv[1]);
    if (!str) return JS_ThrowTypeError(ctx, "playRTTTL: rtttlString must be a string");

    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);

    // Abort any existing playback on THIS pin (not other pins).
    RTTTLState *existing = qjs->rtttlFind((uint8_t)pin);
    if (existing) {
      existing->gen++;
      existing->active = false;
      if (!JS_IsUndefined(existing->onComplete)) {
        JS_FreeValue(ctx, existing->onComplete);
        existing->onComplete = JS_UNDEFINED;
      }
      existing->notes.clear();
    }

    // --- Parse RTTTL ---
    // Format: name:d=duration,o=octave,b=tempo:note,note,...
    // Defaults: d=4 (quarter), o=5 (octave 5), b=63 (tempo)
    uint32_t defaultDuration = 4;  // quarter note
    uint32_t defaultOctave = 5;
    uint32_t tempo = 63;

    std::string s(str);
    JS_FreeCString(ctx, str);

    // Split into name:settings:notes
    // Find first ':' (end of name)
    size_t pos1 = s.find(':');
    if (pos1 == std::string::npos) {
      return JS_ThrowTypeError(ctx, "playRTTTL: invalid format (no name delimiter)");
    }
    // Find second ':' (end of settings)
    size_t pos2 = s.find(':', pos1 + 1);
    if (pos2 == std::string::npos) {
      return JS_ThrowTypeError(ctx, "playRTTTL: invalid format (no settings delimiter)");
    }

    // Parse settings (d=, o=, b=)
    std::string settings = s.substr(pos1 + 1, pos2 - pos1 - 1);
    {
      // Split by commas
      size_t start = 0;
      while (start < settings.size()) {
        size_t comma = settings.find(',', start);
        std::string token = (comma == std::string::npos)
          ? settings.substr(start)
          : settings.substr(start, comma - start);
        // token is like "d=4", "o=5", "b=63"
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
          std::string key = token.substr(0, eq);
          std::string val = token.substr(eq + 1);
          if (key == "d") defaultDuration = atoi(val.c_str());
          else if (key == "o") defaultOctave = atoi(val.c_str());
          else if (key == "b") tempo = atoi(val.c_str());
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }

    // Whole note duration in ms = (60 / tempo) * 4 * 1000
    // Each note duration = wholeNote / noteDurationValue
    uint32_t wholeNoteMs = (60000 / tempo) * 4;

    // Parse notes (comma-separated, after second ':')
    std::string notesStr = s.substr(pos2 + 1);
    std::vector<std::pair<uint32_t, uint32_t>> notes;

    // Note frequency table (octave 4 = base, MIDI-style).
    // Frequencies for octave 4: C=262, C#=277, D=294, D#=311, E=330,
    // F=349, F#=370, G=392, G#=415, A=440, A#=466, B=494
    static const uint32_t noteFreqs[12] = {
      262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
    };
    // Map note letter to index 0-11 (C=0, D=2, E=4, F=5, G=7, A=9, B=11)
    auto noteIndex = [](char c) -> int {
      switch (c) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
        default:  return -1;
      }
    };

    size_t start = 0;
    while (start < notesStr.size()) {
      size_t comma = notesStr.find(',', start);
      std::string token = (comma == std::string::npos)
        ? notesStr.substr(start)
        : notesStr.substr(start, comma - start);
      // Trim whitespace
      while (!token.empty() && isspace(token[0])) token.erase(0, 1);
      while (!token.empty() && isspace(token.back())) token.pop_back();
      if (token.empty()) {
        if (comma == std::string::npos) break;
        start = comma + 1;
        continue;
      }

      // Parse: [duration][note][#|.][octave]
      // duration: optional number (1, 2, 4, 8, 16, 32)
      // note: a-g or p (pause/rest)
      // #: sharp (optional)
      // .: dotted (optional, multiplies duration by 1.5)
      // octave: optional number (4-7)
      size_t i = 0;

      // Duration
      uint32_t duration = defaultDuration;
      if (i < token.size() && isdigit(token[i])) {
        uint32_t d = 0;
        while (i < token.size() && isdigit(token[i])) {
          d = d * 10 + (token[i] - '0');
          i++;
        }
        if (d > 0) duration = d;
      }

      // Note letter
      if (i >= token.size()) {
        if (comma == std::string::npos) break;
        start = comma + 1;
        continue;
      }
      char note = tolower(token[i]);
      i++;

      uint32_t freq = 0;  // 0 = rest
      if (note == 'p') {
        // Rest
        freq = 0;
      } else {
        int idx = noteIndex(note);
        if (idx < 0) {
          // Invalid note — skip
          if (comma == std::string::npos) break;
          start = comma + 1;
          continue;
        }
        // Sharp?
        if (i < token.size() && token[i] == '#') {
          idx++;
          i++;
        }
        // Dotted? (we'll handle after octave)
        bool dotted = false;
        if (i < token.size() && token[i] == '.') {
          dotted = true;
          i++;
        }
        // Octave
        uint32_t octave = defaultOctave;
        if (i < token.size() && isdigit(token[i])) {
          octave = token[i] - '0';
          i++;
        }
        // Calculate frequency: base freq * 2^(octave - 4)
        if (idx >= 0 && idx < 12) {
          freq = noteFreqs[idx];
          if (octave >= 4) {
            for (uint32_t o = 4; o < octave; o++) freq *= 2;
          } else {
            for (uint32_t o = octave; o < 4; o++) freq /= 2;
          }
        }
        // Duration with dotted note
        uint32_t durMs = wholeNoteMs / duration;
        if (dotted) durMs = durMs * 3 / 2;
        notes.push_back({freq, durMs});
        if (comma == std::string::npos) break;
        start = comma + 1;
        continue;
      }

      // Rest duration
      bool dotted = false;
      if (i < token.size() && token[i] == '.') {
        dotted = true;
        i++;
      }
      uint32_t durMs = wholeNoteMs / duration;
      if (dotted) durMs = durMs * 3 / 2;
      notes.push_back({0, durMs});

      if (comma == std::string::npos) break;
      start = comma + 1;
    }

    if (notes.empty()) {
      return JS_ThrowTypeError(ctx, "playRTTTL: no valid notes parsed");
    }

    // Find or create a player slot for this pin.
    RTTTLState *player = nullptr;
    for (auto &p : qjs->rtttlPlayers) {
      if (!p.active && p.pin == pin) { player = &p; break; }
    }
    if (!player) {
      for (auto &p : qjs->rtttlPlayers) {
        if (!p.active) { player = &p; break; }
      }
    }
    if (!player) {
      qjs->rtttlPlayers.emplace_back();
      player = &qjs->rtttlPlayers.back();
    }

    // Set up RTTTL state.
    player->pin = (uint8_t)pin;
    player->notes = std::move(notes);
    player->noteIdx = 0;
    player->noteStartMs = millis();
    player->gen++;
    player->startGen = player->gen;
    player->active = true;
    if (argc >= 3 && JS_IsFunction(ctx, argv[2])) {
      player->onComplete = JS_DupValue(ctx, argv[2]);
    } else {
      player->onComplete = JS_UNDEFINED;
    }

    // Start the first note immediately.
    uint32_t freq = player->notes[0].first;
    if (freq == 0) {
      // Rest — turn off PWM.
      qjs->analog.detachPwmIfAttached((uint8_t)pin);
    } else {
      JSValue wargv[4] = {
        JS_NewUint32(ctx, pin),
        JS_NewUint32(ctx, 511),
        JS_NewUint32(ctx, freq),
        JS_NewUint32(ctx, 10),
      };
      JSValue ret = qjs->analog.js_writeAnalog(ctx, 4, (JSValueConst*)wargv);
      for (int i = 0; i < 4; i++) JS_FreeValue(ctx, wargv[i]);
      JS_FreeValue(ctx, ret);
    }

    return JS_UNDEFINED;
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

  // WiFi.startDNS(domain, [port=53]) → boolean
  // Starts a DNS server that resolves ALL queries to the device's IP.
  // Used for captive portals when running as an AP.
  static JSValue wifi_start_dns(JSContext *ctx, JSValueConst jsThis,
                                int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "WiFi.startDNS: need (domain)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    const char *domain = JS_ToCString(ctx, argv[0]);
    if (!domain) return JS_EXCEPTION;
    uint32_t port = 53;
    if (argc >= 2) JS_ToUint32(ctx, &port, argv[1]);
    // Resolve all queries to our IP (captive portal pattern).
    bool ok = qjs->dnsServer.start(port, domain, WiFi.softAPIP());
    JS_FreeCString(ctx, domain);
    qjs->dnsRunning = ok;
    return JS_NewBool(ctx, ok);
  }

  // WiFi.stopDNS() → void
  static JSValue wifi_stop_dns(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    qjs->dnsServer.stop();
    qjs->dnsRunning = false;
    return JS_UNDEFINED;
  }

  // WiFi.startMDNS(hostname, [service], [port]) → boolean
  // Registers the device on the local network as <hostname>.local.
  // If service+port are provided, also advertises a service via mDNS.
  static JSValue wifi_start_mdns(JSContext *ctx, JSValueConst jsThis,
                                 int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "WiFi.startMDNS: need (hostname)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    const char *hostname = JS_ToCString(ctx, argv[0]);
    if (!hostname) return JS_EXCEPTION;
    bool ok = MDNS.begin(hostname);
    JS_FreeCString(ctx, hostname);
    qjs->mdnsRunning = ok;
    if (ok && argc >= 3) {
      const char *service = JS_ToCString(ctx, argv[1]);
      uint32_t port = 0;
      JS_ToUint32(ctx, &port, argv[2]);
      if (service) {
        MDNS.addService(service, "tcp", (uint16_t)port);
        JS_FreeCString(ctx, service);
      }
    }
    return JS_NewBool(ctx, ok);
  }

  // WiFi.addMDNSService(service, proto, port) → boolean
  // Adds an mDNS service advertisement (e.g. "http", "tcp", 80).
  static JSValue wifi_add_mdns_service(JSContext *ctx, JSValueConst jsThis,
                                       int argc, JSValueConst *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "WiFi.addMDNSService: need (service, proto, port)");
    if (!MDNS.begin("")) return JS_NewBool(ctx, false);  // ensure mDNS is up
    const char *service = JS_ToCString(ctx, argv[0]);
    const char *proto = JS_ToCString(ctx, argv[1]);
    uint32_t port = 0;
    JS_ToUint32(ctx, &port, argv[2]);
    bool ok = MDNS.addService(service, proto, (uint16_t)port);
    JS_FreeCString(ctx, service);
    JS_FreeCString(ctx, proto);
    return JS_NewBool(ctx, ok);
  }

  // WiFi.scan(callback) → void
  // Scans for WiFi networks NON-BLOCKING. WiFi.scanNetworks() runs
  // on a FreeRTOS task; the callback fires from loop() when done.
  static JSValue wifi_scan(JSContext *ctx, JSValueConst jsThis,
                           int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
      return JS_ThrowTypeError(ctx, "WiFi.scan: need (callback, [timeoutMs])");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);

    uint32_t timeoutMs = 300;
    if (argc >= 2) JS_ToUint32(ctx, &timeoutMs, argv[1]);

    // Allocate scan result struct.
    qjs->scanResult_ = new ScanResult();
    qjs->scanResult_->callback = JS_DupValue(ctx, argv[0]);
    qjs->scanResult_->done = false;
    qjs->scanResult_->count = 0;
    qjs->scanResult_->timeoutMs = timeoutMs;
    if (!qjs->scanMutex_) qjs->scanMutex_ = xSemaphoreCreateMutex();

    // Spawn a one-shot task to do the blocking scan.
    xTaskCreatePinnedToCore(scanTaskEntry, "wifi_scan", 4096, qjs,
                            1, &qjs->scanTask_, 0);
    return JS_UNDEFINED;
  }

  // WiFi.syncNTP([server="pool.ntp.org"], [tzOffset=0]) → boolean
  // Configures SNTP time sync. After calling, getTime() returns
  // wall-clock time. tzOffset is in hours.
  static JSValue wifi_sync_ntp(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    const char *server = "pool.ntp.org";
    if (argc >= 1 && JS_IsString(argv[0])) {
      const char *s = JS_ToCString(ctx, argv[0]);
      if (s) server = s;
    }
    int tzOffset = 0;
    if (argc >= 2) JS_ToInt32(ctx, &tzOffset, argv[1]);

    configTime(tzOffset * 3600, 0, server);
    if (argc >= 1 && JS_IsString(argv[0])) {
      const char *s = JS_ToCString(ctx, argv[0]);
      if (s) JS_FreeCString(ctx, s);
    }
    return JS_NewBool(ctx, true);
  }

  // ---- New esp32.* trampolines ----

  // Static ISR for setWatch — no-op, we poll from loop().
  static void IRAM_ATTR esp32_watch_isr() {}

  // esp32.dacWrite(pin, value) → void
  // True analog output on GPIO25/26. value is 0-255.
  static JSValue esp32_dac_write(JSContext *ctx, JSValueConst jsThis,
                                 int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "dacWrite: need (pin, value)");
    uint32_t pin, value;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &value, argv[1]);
    if (pin != 25 && pin != 26)
      return JS_ThrowRangeError(ctx, "dacWrite: only GPIO25 and GPIO26 have DAC");
    dacWrite(pin, value);
    return JS_UNDEFINED;
  }

  // esp32.setWatch(pin, mode, callback) → disposeFn
  // mode: 0=RISING, 1=FALLING, 2=CHANGE
  // Returns a function that removes the watch when called.
  static void IRAM_ATTR watch_isr(void *arg) {
    // Just set a flag — the actual JS callback fires from loop().
    ESP32QuickJS *qjs = (ESP32QuickJS *)arg;
    // We can't call JS from an ISR. We'll poll in loop().
    // The ISR is just to wake us up. We use a simple approach:
    // store the pin in a volatile queue.
    // Actually, attachInterrupt already debounces. We'll check
    // from loop() by comparing a counter.
  }

  static JSValue esp32_set_watch(JSContext *ctx, JSValueConst jsThis,
                                 int argc, JSValueConst *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "setWatch: need (pin, mode, callback)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t pin, mode;
    JS_ToUint32(ctx, &pin, argv[0]);
    JS_ToUint32(ctx, &mode, argv[1]);

    int intMode = RISING;
    if (mode == 1) intMode = FALLING;
    else if (mode == 2) intMode = CHANGE;

    // Find or create a watch entry.
    int idx = -1;
    for (size_t i = 0; i < qjs->watches.size(); i++) {
      if (!qjs->watches[i].active) { idx = i; break; }
    }
    if (idx < 0) {
      qjs->watches.push_back({0, JS_UNDEFINED, false});
      idx = qjs->watches.size() - 1;
    }
    qjs->watches[idx].pin = (uint8_t)pin;
    qjs->watches[idx].callback = JS_DupValue(ctx, argv[2]);
    qjs->watches[idx].active = true;

    // Use a no-op ISR — we poll pin state from loop() to fire the
    // JS callback. attachInterrupt needs a plain function pointer.
    attachInterrupt(digitalPinToInterrupt(pin), esp32_watch_isr, intMode);

    // Return a dispose function.
    // We create a JS function that calls clearWatch with the pin.
    char src[64];
    snprintf(src, sizeof(src), "function(){esp32.clearWatch(%u)}", (unsigned)pin);
    JSValue disposeFn = JS_Eval(ctx, src, strlen(src), "<dispose>",
                                JS_EVAL_TYPE_GLOBAL);
    return disposeFn;
  }

  // esp32.clearWatch(pin) → void
  static JSValue esp32_clear_watch(JSContext *ctx, JSValueConst jsThis,
                                   int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "clearWatch: need (pin)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    detachInterrupt(digitalPinToInterrupt(pin));
    for (auto &w : qjs->watches) {
      if (w.pin == pin && w.active) {
        JS_FreeValue(ctx, w.callback);
        w.callback = JS_UNDEFINED;
        w.active = false;
      }
    }
    return JS_UNDEFINED;
  }

  // esp32.getTime() → number
  // Returns seconds since boot (float). If NTP is synced, returns
  // Unix timestamp.
  static JSValue esp32_get_time(JSContext *ctx, JSValueConst jsThis,
                                int argc, JSValueConst *argv) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    // If time is > 946684800 (Jan 1 2000), NTP is synced.
    if (tv.tv_sec > 946684800) {
      return JS_NewFloat64(ctx, (double)tv.tv_sec + tv.tv_usec / 1000000.0);
    }
    // Otherwise return uptime in seconds.
    return JS_NewFloat64(ctx, millis() / 1000.0);
  }

  // esp32.setTime(seconds) → void
  static JSValue esp32_set_time(JSContext *ctx, JSValueConst jsThis,
                                int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "setTime: need (seconds)");
    double t;
    JS_ToFloat64(ctx, &t, argv[0]);
    struct timeval tv;
    tv.tv_sec = (time_t)t;
    tv.tv_usec = (long)((t - tv.tv_sec) * 1000000);
    settimeofday(&tv, nullptr);
    return JS_UNDEFINED;
  }

  // esp32.getSerial() → string
  // Returns the ESP32 MAC address as a unique identifier.
  static JSValue esp32_get_serial(JSContext *ctx, JSValueConst jsThis,
                                  int argc, JSValueConst *argv) {
    uint64_t mac = ESP.getEfuseMac();
    char buf[20];
    snprintf(buf, sizeof(buf), "%04X%08X",
             (uint16_t)(mac >> 32), (uint32_t)mac);
    return JS_NewString(ctx, buf);
  }

  // Serial1 / Serial2 — return a serial object with setup/write/onData.
  // Serial1 = UART1, Serial2 = UART2.
  static JSValue esp32_serial1(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__isSerial1", JS_NewBool(ctx, true));
    // Methods are added via a function list below.
    static const JSCFunctionListEntry serial_funcs[] = {
        JSCFunctionListEntry{"setup", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, serial_setup}
                             }},
        JSCFunctionListEntry{"write", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, serial_write}
                             }},
        JSCFunctionListEntry{"onData", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, serial_on_data}
                             }},
        JSCFunctionListEntry{"available", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, serial_available}
                             }},
        JSCFunctionListEntry{"read", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, serial_read}
                             }},
    };
    JS_SetPropertyFunctionList(ctx, obj, serial_funcs,
                               sizeof(serial_funcs) / sizeof(JSCFunctionListEntry));
    return obj;
  }

  static JSValue esp32_serial2(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__isSerial1", JS_NewBool(ctx, false));
    static const JSCFunctionListEntry serial_funcs[] = {
        JSCFunctionListEntry{"setup", 0, JS_DEF_CFUNC, 0, {
                               func : {3, JS_CFUNC_generic, serial_setup}
                             }},
        JSCFunctionListEntry{"write", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, serial_write}
                             }},
        JSCFunctionListEntry{"onData", 0, JS_DEF_CFUNC, 0, {
                               func : {1, JS_CFUNC_generic, serial_on_data}
                             }},
        JSCFunctionListEntry{"available", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, serial_available}
                             }},
        JSCFunctionListEntry{"read", 0, JS_DEF_CFUNC, 0, {
                               func : {0, JS_CFUNC_generic, serial_read}
                             }},
    };
    JS_SetPropertyFunctionList(ctx, obj, serial_funcs,
                               sizeof(serial_funcs) / sizeof(JSCFunctionListEntry));
    return obj;
  }

  // Serial.setup(baud, tx, rx) — called on the Serial1/Serial2 object.
  static JSValue serial_setup(JSContext *ctx, JSValueConst jsThis,
                              int argc, JSValueConst *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "Serial.setup: need (baud, tx, rx)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t baud, tx, rx;
    JS_ToUint32(ctx, &baud, argv[0]);
    JS_ToUint32(ctx, &tx, argv[1]);
    JS_ToUint32(ctx, &rx, argv[2]);

    // Check if this is Serial1 or Serial2.
    JSValue isS1 = JS_GetPropertyStr(ctx, jsThis, "__isSerial1");
    bool serial1 = JS_ToBool(ctx, isS1);
    JS_FreeValue(ctx, isS1);

    HardwareSerial **port = serial1 ? &qjs->serial1 : &qjs->serial2;
    if (!*port) {
      *port = new HardwareSerial(serial1 ? UART_NUM_1 : UART_NUM_2);
    }
    (*port)->begin(baud, SERIAL_8N1, tx, rx);
    return JS_UNDEFINED;
  }

  static JSValue serial_write(JSContext *ctx, JSValueConst jsThis,
                              int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Serial.write: need (data)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue isS1 = JS_GetPropertyStr(ctx, jsThis, "__isSerial1");
    bool s1 = JS_ToBool(ctx, isS1);
    JS_FreeValue(ctx, isS1);
    HardwareSerial *port = s1 ? qjs->serial1 : qjs->serial2;
    if (!port) return JS_ThrowInternalError(ctx, "Serial: call setup() first");

    const char *data = JS_ToCString(ctx, argv[0]);
    if (data) {
      port->write((const uint8_t *)data, strlen(data));
      JS_FreeCString(ctx, data);
    }
    return JS_UNDEFINED;
  }

  static JSValue serial_on_data(JSContext *ctx, JSValueConst jsThis,
                                int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Serial.onData: need (callback)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue isS1 = JS_GetPropertyStr(ctx, jsThis, "__isSerial1");
    bool s1 = JS_ToBool(ctx, isS1);
    JS_FreeValue(ctx, isS1);
    JSValue *cb = s1 ? &qjs->serial1Cb : &qjs->serial2Cb;
    JS_FreeValue(ctx, *cb);
    *cb = JS_DupValue(ctx, argv[0]);
    return JS_UNDEFINED;
  }

  static JSValue serial_available(JSContext *ctx, JSValueConst jsThis,
                                  int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue isS1 = JS_GetPropertyStr(ctx, jsThis, "__isSerial1");
    bool s1 = JS_ToBool(ctx, isS1);
    JS_FreeValue(ctx, isS1);
    HardwareSerial *port = s1 ? qjs->serial1 : qjs->serial2;
    if (!port) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, port->available());
  }

  static JSValue serial_read(JSContext *ctx, JSValueConst jsThis,
                             int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    JSValue isS1 = JS_GetPropertyStr(ctx, jsThis, "__isSerial1");
    bool s1 = JS_ToBool(ctx, isS1);
    JS_FreeValue(ctx, isS1);
    HardwareSerial *port = s1 ? qjs->serial1 : qjs->serial2;
    if (!port || !port->available()) return JS_NewString(ctx, "");
    String data;
    while (port->available()) data += (char)port->read();
    return JS_NewString(ctx, data.c_str());
  }

  // ---- OneWire trampolines ----
  // OneWire.setup(pin) → handle (0-3)
  static JSValue onewire_setup(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "OneWire.setup: need (pin)");
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    uint32_t pin;
    JS_ToUint32(ctx, &pin, argv[0]);
    OneWire *ow = new OneWire((uint8_t)pin);
    qjs->oneWireBuses.push_back(ow);
    return JS_NewInt32(ctx, (int)(qjs->oneWireBuses.size() - 1));
  }

  static OneWire *ow_get(ESP32QuickJS *qjs, JSValueConst argv) {
    int32_t h;
    JS_ToInt32(qjs->ctx, &h, argv);
    if (h < 0 || h >= (int)qjs->oneWireBuses.size()) return nullptr;
    return qjs->oneWireBuses[h];
  }

  static JSValue onewire_reset(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    return JS_NewBool(ctx, ow->reset());
  }

  static JSValue onewire_write(JSContext *ctx, JSValueConst jsThis,
                               int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    uint32_t byte;
    JS_ToUint32(ctx, &byte, argv[1]);
    uint32_t power = 0;
    if (argc >= 3) JS_ToUint32(ctx, &power, argv[2]);
    ow->write((uint8_t)byte, power != 0);
    return JS_UNDEFINED;
  }

  static JSValue onewire_read(JSContext *ctx, JSValueConst jsThis,
                              int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    return JS_NewInt32(ctx, ow->read());
  }

  static JSValue onewire_search(JSContext *ctx, JSValueConst jsThis,
                                int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    ow->reset_search();
    uint8_t addr[8];
    JSValue arr = JS_NewArray(ctx);
    int idx = 0;
    while (ow->search(addr)) {
      JSValue addrArr = JS_NewArray(ctx);
      for (int i = 0; i < 8; i++)
        JS_SetPropertyUint32(ctx, addrArr, i, JS_NewInt32(ctx, addr[i]));
      JS_SetPropertyUint32(ctx, arr, idx++, addrArr);
    }
    return arr;
  }

  static JSValue onewire_skip(JSContext *ctx, JSValueConst jsThis,
                              int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    ow->skip();
    return JS_UNDEFINED;
  }

  static JSValue onewire_select(JSContext *ctx, JSValueConst jsThis,
                                int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    if (argc < 2 || !JS_IsArray(ctx, argv[1]))
      return JS_ThrowTypeError(ctx, "OneWire.select: need (handle, addrArray)");
    uint8_t addr[8];
    for (int i = 0; i < 8; i++) {
      JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
      uint32_t b;
      JS_ToUint32(ctx, &b, v);
      addr[i] = (uint8_t)b;
      JS_FreeValue(ctx, v);
    }
    ow->select(addr);
    return JS_UNDEFINED;
  }

  static JSValue onewire_depower(JSContext *ctx, JSValueConst jsThis,
                                 int argc, JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    OneWire *ow = ow_get(qjs, argv[0]);
    if (!ow) return JS_ThrowInternalError(ctx, "OneWire: invalid handle");
    ow->depower();
    return JS_UNDEFINED;
  }

  static JSValue http_fetch(JSContext *ctx, JSValueConst jsThis, int argc,
                            JSValueConst *argv) {
    ESP32QuickJS *qjs = (ESP32QuickJS *)JS_GetContextOpaque(ctx);
    return qjs->httpFetcher.fetch(ctx, argv[0], argv[1]);
  }

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
};

// Static member definition — must be at file scope.
void (*ESP32QuickJS::JSBlockingGuard::pumpCallback)() = nullptr;

// RotaryEncoder is implemented in its own files (RotaryEncoder.h/.cpp
// for the native object, JSRotaryEncoder.h/.cpp for the JS wrapper).
// They are included below near the top of this header.

#pragma once

#if defined(WiFi_h) && !defined(ENABLE_WIFI)
#define ENABLE_WIFI
#endif

#include <Arduino.h>

#include <algorithm>
#include <vector>

#ifdef ENABLE_WIFI
#include <HTTPClient.h>
#include <Server.h>
#include <StreamString.h>
#endif

#include "../quickjs.h"

static void qjs_dump_exception(JSContext *ctx, JSValue v) {
  if (!JS_IsUndefined(v)) {
    const char *str = JS_ToCString(ctx, v);
    if (str) {
      Serial.println(str);
      JS_FreeCString(ctx, str);
    } else {
      Serial.println("[Exception]");
    }
  }
  JSValue e = JS_GetException(ctx);
  const char *str = JS_ToCString(ctx, e);
  if (str) {
    Serial.println(str);
    JS_FreeCString(ctx, str);
  }
  if (JS_IsError(ctx, e)) {
    JSValue s = JS_GetPropertyStr(ctx, e, "stack");
    if (!JS_IsUndefined(s)) {
      const char *str = JS_ToCString(ctx, s);
      if (str) {
        Serial.println(str);
        JS_FreeCString(ctx, str);
      }
    }
    JS_FreeValue(ctx, s);
  }
  JS_FreeValue(ctx, e);
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
  // 20 bytes / entry.
  struct TimerEntry {
    uint32_t id;
    int32_t timeout;
    int32_t interval;
    JSValue func;
  };
  std::vector<TimerEntry> timers;
  uint32_t id_counter = 0;

 public:
  uint32_t RegisterTimer(JSValue f, int32_t time, int32_t interval = -1) {
    uint32_t id = ++id_counter;
    timers.push_back(TimerEntry{id, time, interval, f});
    return id;
  }
  void RemoveTimer(uint32_t id) {
    timers.erase(std::remove_if(timers.begin(), timers.end(),
                                [id](TimerEntry &t) { return t.id == id; }),
                 timers.end());
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
      // NOTE: may update timers in this JS_Call().
      JSValue r = JS_Call(ctx, ent.func, ent.func, 0, nullptr);
      if (JS_IsException(r)) {
        qjs_dump_exception(ctx, r);
      }
      JS_FreeValue(ctx, r);

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

class ESP32QuickJS {
 public:
  JSRuntime *rt;
  JSContext *ctx;
  JSTimer timer;
  JSValue loop_func = JS_UNDEFINED;
#ifdef ENABLE_WIFI
  JSHttpFetcher httpFetcher;
  JSWebServer webServer;
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
  }

  void end() {
    timer.RemoveAll(ctx);
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

#ifdef ENABLE_WIFI
    httpFetcher.loop(ctx);
    webServer.loop();
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
                      JS_NewCFunction(ctx, clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "setInterval",
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
    for (int i = 0; i < argc; i++) {
      const char *str = JS_ToCString(ctx, argv[i]);
      if (str) {
        Serial.println(str);
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
    uint32_t id =
        qjs->timer.RegisterTimer(JS_DupValue(ctx, argv[0]), millis() + t);
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
    uint32_t id =
        qjs->timer.RegisterTimer(JS_DupValue(ctx, argv[0]), millis() + t, t);
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
};

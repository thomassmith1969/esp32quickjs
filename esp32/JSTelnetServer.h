// JSTelnetServer.h
//
// JavaScript wrapper for a raw AsyncServer-based Telnet REPL. Each
// connected client owns an AsyncClient* and a per-client REPL state
// (ReplState). The library owns everything; the example app just
// creates an ESP32QuickJS instance and calls qjs.telnet.loop() from
// its main loop.
//
// We do NOT use the AsyncTelnet library because it can only hold one
// client at a time (it overwrites its single `client` member on
// every new connection). The raw AsyncServer + per-client map
// approach lets 2-3+ clients REPL simultaneously.
//
// The class is opt-in: nothing happens until start() is called (either
// from C++ or from JS via the `Telnet.start()` global). stop()
// releases the AsyncServer.
//
// Pattern follows other JS wrapper classes (JSI2C, JSSPI, JSAnalog):
//   - The library provides both a C++ API (start/stop/loop) and a JS
//     API (Telnet.start / Telnet.stop) registered in ESP32QuickJS::setup().

#pragma once

#include <Arduino.h>
#include <AsyncTCP.h>
#include <map>
#include <stack>
#include "../quickjs.h"

// Forward declaration so we don't have to drag the whole QuickJS.h in.
class ESP32QuickJS;

// JSTelnetServer: per-ESP32QuickJS-instance Telnet REPL server.
//
// The C++ API:
//   JSTelnetServer telnet;             // declare in main.cpp / library
//   telnet.attach(&qjs);               // wire to a QuickJS instance
//   telnet.start();                    // listen on TELNET_PORT (23)
//   telnet.stop();                     // stop listening
//   telnet.loop();                     // pump from your main loop
//
// The JS API (registered in ESP32QuickJS::setup()):
//   Telnet.start()                     // begin listening
//   Telnet.stop()                      // stop listening
class JSTelnetServer {
 public:
  // Telnet REPL server configuration. We use a raw AsyncServer rather
  // than the AsyncTelnet library because AsyncTelnet can only hold one
  // client at a time. We keep up to TELNET_MAX_CONNECTIONS concurrent
  // clients in a map keyed by connectionIdCounter.
  static constexpr uint16_t TELNET_PORT = 23;
  static constexpr int TELNET_MAX_CONNECTIONS = 4;
  static constexpr int TELNET_BUF_SIZE = 2048;

  // Per-connection REPL state. Each telnet client owns one. The Serial
  // REPL in main.cpp uses the same struct for uniformity.
  struct ReplState {
    char buf[TELNET_BUF_SIZE];
    size_t len = 0;
    bool open = false;
    int parenDepth = 0, bracketDepth = 0, braceDepth = 0;
    char inString = 0;
    bool escaped = false;
  };

  // Print wrapper for a single telnet client. AsyncTCP doesn't ship a
  // Print subclass bound to a specific client, so we wrap the
  // AsyncClient* ourselves.
  class TelnetOut : public Print {
   public:
    explicit TelnetOut(AsyncClient* c) : client_(c) {}
    size_t write(uint8_t b) override {
      if (!client_ || !client_->connected()) return 0;
      return client_->write((const char*)&b, 1);
    }
    size_t write(const uint8_t* buf, size_t size) override {
      if (!client_ || !client_->connected()) return 0;
      return client_->write((const char*)buf, size);
    }
    bool ok() const { return client_ && client_->connected(); }
   private:
    AsyncClient* client_;
  };

  JSTelnetServer();
  ~JSTelnetServer();

  // Wire this server to a QuickJS instance. Required before start().
  // After attach(), JS-side Telnet.start/stop and per-client console.log
  // routing all use this qjs.
  void attach(ESP32QuickJS* qjs);

  // Begin listening on TELNET_PORT. Idempotent: calling start() while
  // already running is a no-op.
  void start();

  // Stop listening and close all live clients. Safe to call when not
  // started.
  void stop();

  // Returns true if the server is currently listening.
  bool running() const { return telnetServer != nullptr; }

  // Pump the per-client REPL state machines. Call once per main-loop
  // iteration (after popping incomingConnections). This evaluates any
  // complete statements each client has sent. The actual JS eval is
  // delegated to qjs->eval / qjs->evalAsync — this function just
  // handles the multi-line REPL parsing and routes console.log output
  // back to the originating client.
  //
  // MUST be called from the same task as qjs (QuickJS is not
  // thread-safe). MUST NOT be called from inside a JS callback.
  void loop();

  // Get the JS-side Telnet namespace object. Returns JS_UNDEFINED if
  // qjs.ctx is not yet set up. Used by ESP32QuickJS::setup() to expose
  // Telnet.start / Telnet.stop to JS.
  JSValue getJSNamespace(JSContext* ctx);

 private:
  // AsyncServer callbacks (called from AsyncTCP's task context).
  static void onTelnetConnectStatic(void* arg, AsyncClient* client);
  void onTelnetConnect(AsyncClient* client);
  void onTelnetData(void* arg, AsyncClient* client, void* data, size_t len);
  void onTelnetDisconnect(void* arg, AsyncClient* client);

  // Initialise a freshly-accepted client (assign id, install ReplState,
  // write welcome banner + prompt). Called from loop() on the JS task.
  void initClient(AsyncClient* client);

  // Drain any pending newline-terminated commands from one client's
  // ReplState buffer. Called from loop() on the JS task.
  void handleClientInput(int id);

  // Resolve a client pointer back to its assigned id, or -1 if not yet
  // initialised.
  int findClientId(AsyncClient* client) const;

  // State.
  ESP32QuickJS* qjs_ = nullptr;
  AsyncServer* telnetServer = nullptr;
  std::stack<AsyncClient*> incomingConnections;
  std::map<int, AsyncClient*> liveConnections;
  std::map<int, ReplState> telnetRepls;
  std::map<int, TelnetOut*> telnetOutputs;
  int connectionIdCounter = 0;
};

// Default singleton — exposed by ESP32QuickJS as `qjs.telnet`. Set up
// in ESP32QuickJS::setup() (created automatically, no extra wiring
// required from the example app).
//
// Defined in JSTelnetServer.cpp.
extern JSTelnetServer* g_jstelnet;

// JSTelnetServer.cpp
//
// JavaScript wrapper for a raw AsyncServer-based Telnet REPL. Each
// connected client owns an AsyncClient* and a per-client REPL state
// (JSTelnetServer::ReplState). The library owns everything; the example
// app just creates an ESP32QuickJS instance and calls qjs.telnet.loop()
// from its main loop.
//
// We do NOT use the AsyncTelnet library because it can only hold one
// client at a time (it overwrites its single `client` member on every
// new connection). The raw AsyncServer + per-client map approach lets
// 2-3+ clients REPL simultaneously.
//
// The class is opt-in: nothing happens until start() is called (either
// from C++ or from JS via the `Telnet.start()` global). stop()
// releases the AsyncServer.

#include "JSTelnetServer.h"
#include <Arduino.h>
#include <string.h>

#include "QuickJS.h"

// Default singleton — referenced by ESP32QuickJS via g_jstelnet.
JSTelnetServer* g_jstelnet = nullptr;

// Static instance — lives for the program's lifetime. The constructor
// sets g_jstelnet to this. We declare it here (rather than as a global
// variable in main.cpp) so the library is fully self-contained.
static JSTelnetServer g_jstelnet_instance;

// Forward declarations for symbols defined in QuickJS.h so this TU
// doesn't have to depend on the order of includes. (QuickJS.h already
// declares them as `extern`, so they're available with or without
// these redeclarations, but spelling them out here makes the
// dependency explicit.)
extern Stream* activeOutputStream;
extern int currentTelnetId;
// Forward declaration of the library's REPL helper. JSTelnetServer
// delegates command evaluation to JSRepl (see JSRepl.{h,cpp}).
class JSRepl;
extern JSRepl* g_jsrepl;

JSTelnetServer::JSTelnetServer() {
  g_jstelnet = this;
}

JSTelnetServer::~JSTelnetServer() {
  stop();
  if (g_jstelnet == this) g_jstelnet = nullptr;
}

void JSTelnetServer::attach(ESP32QuickJS* qjs) {
  qjs_ = qjs;
}

void JSTelnetServer::start() {
  if (telnetServer) return;  // already running — idempotent
  if (!qjs_) return;          // not attached yet — silently ignore
  telnetServer = new AsyncServer(TELNET_PORT);
  telnetServer->onClient(&onTelnetConnectStatic, this);
  telnetServer->begin();
}

void JSTelnetServer::stop() {
  if (!telnetServer) return;
  telnetServer->end();
  delete telnetServer;
  telnetServer = nullptr;
  // Free any per-client output wrappers and reset state.
  for (auto& kv : telnetOutputs) {
    delete kv.second;
  }
  telnetOutputs.clear();
  telnetRepls.clear();
  // AsyncClient* are owned by AsyncTCP; they'll be released when the
  // AsyncServer is destroyed.
  liveConnections.clear();
  while (!incomingConnections.empty()) incomingConnections.pop();
}

void JSTelnetServer::onTelnetConnectStatic(void* arg, AsyncClient* client) {
  JSTelnetServer* self = static_cast<JSTelnetServer*>(arg);
  if (self) self->onTelnetConnect(client);
}

void JSTelnetServer::onTelnetConnect(AsyncClient* client) {
  // The first byte of a telnet session is usually IAC negotiation. We
  // don't do any negotiation here — we just accept raw bytes. The
  // classic ESP32 Arduino telnet samples don't either; users can run
  // their own IAC handler in JS if they want.
  client->onData(
      [](void* arg, AsyncClient* c, void* data, size_t len) {
        JSTelnetServer* self = static_cast<JSTelnetServer*>(arg);
        if (self) self->onTelnetData(arg, c, data, len);
      },
      this);
  client->onDisconnect(
      [](void* arg, AsyncClient* c) {
        JSTelnetServer* self = static_cast<JSTelnetServer*>(arg);
        if (self) self->onTelnetDisconnect(arg, c);
      },
      this);
  incomingConnections.push(client);
}

int JSTelnetServer::findClientId(AsyncClient* client) const {
  if (!client) return -1;
  for (auto& kv : liveConnections) {
    if (kv.second == client) return kv.first;
  }
  return -1;
}

void JSTelnetServer::onTelnetData(void* arg, AsyncClient* client, void* data, size_t len) {
  if (!client || !client->connected()) return;
  // We need the client's id to look up its ReplState. The id is
  // assigned in initClient() — but data can arrive before init runs.
  // Use liveConnections for lookup; if not yet there, drop the data.
  int id = findClientId(client);
  if (id < 0) {
    // Pre-init data: drop it. The init call from main loop will wire
    // up the REPL on the next iteration, and we don't want pre-banner
    // bytes polluting the buffer.
    return;
  }
  // We do NOT echo bytes back to the client — the user's terminal is
  // responsible for local echo (most telnet clients in line mode do
  // it, and we don't want to double characters when the terminal
  // already echoes locally). Just append to the per-client buffer.
  const uint8_t* p = (const uint8_t*)data;
  auto& state = telnetRepls[id];
  for (size_t i = 0; i < len; i++) {
    char c = (char)p[i];
    // ---- Telnet IAC (RFC 854) negotiation stripping ----
    // Telnet clients send option-negotiation sequences at the start
    // of a session and sometimes mid-session. These are 3-byte
    // sequences starting with 0xFF (IAC) followed by a command byte
    // (DO/DONT/WILL/WONT = 0xFD/0xFE/0xFB/0xFC) and an option byte.
    // If we don't strip them, the bytes end up in the REPL input
    // buffer and corrupt the open-statement parser (notably the
    // 0xFB / 0xFD bytes can be followed by an option byte that
    // happens to be a quote character like '"' or '\'', which makes
    // the parser think we're inside a string).
    //
    // We strip all 3-byte IAC sequences. We do NOT respond to the
    // negotiation — most clients are happy to use defaults.
    if ((uint8_t)c == 0xFF) {
      // Need at least 2 more bytes for a complete IAC sequence. If
      // we don't have them, drop what we can and let the next data
      // delivery provide the rest. (Simplest: just drop the lone
      // 0xFF for now.)
      if (i + 2 < len) {
        // Skip 3 bytes total (IAC + cmd + option).
        i += 2;
        continue;
      }
      // Trailing partial IAC at end of buffer — drop it.
      continue;
    }
    // Normalize line endings. Telnet clients in line mode typically
    // send \r\n on Enter. We translate \r\n and lone \r to \n so the
    // main loop's newline-driven eval triggers reliably. A bare \0
    // after \r is the RFC 854 NVT line-ending terminator — drop it.
    if (c == '\r') {
      // If the next byte is \n, consume it too (CRLF → LF).
      if (i + 1 < len && p[i + 1] == '\n') i++;
      if (state.len < TELNET_BUF_SIZE - 1) {
        state.buf[state.len++] = '\n';
      }
      continue;
    }
    if (c == '\0') {
      // NVT line-ending terminator. Drop.
      continue;
    }
    if (state.len < TELNET_BUF_SIZE - 1) {
      state.buf[state.len++] = c;
    }
  }
}

void JSTelnetServer::onTelnetDisconnect(void* arg, AsyncClient* client) {
  int id = findClientId(client);
  if (id < 0) return;
  liveConnections.erase(id);
  telnetRepls.erase(id);
  auto it = telnetOutputs.find(id);
  if (it != telnetOutputs.end()) {
    delete it->second;
    telnetOutputs.erase(it);
  }
  // Drop timers owned by this client. Nohup timers are kept alive
  // but rerouted to Serial (console output).
  if (qjs_ && qjs_->ctx) {
    qjs_->timer.RemoveTimersForOwner(qjs_->ctx, id);
  }
  // The AsyncClient* itself is owned by AsyncTCP and freed by it on
  // disconnect — don't delete it here.
}

void JSTelnetServer::initClient(AsyncClient* client) {
  connectionIdCounter++;
  int id = connectionIdCounter;
  liveConnections[id] = client;
  telnetRepls[id] = ReplState{};
  telnetOutputs[id] = new TelnetOut(client);
  TelnetOut* out = telnetOutputs[id];
  out->println();
  out->println("=== QuickJS REPL (conn #" + String(id) + ") ===");
  out->println("Type JS expressions and press Enter.");
  out->print("js> ");
}

void JSTelnetServer::handleClientInput(int id) {
  auto itClient = liveConnections.find(id);
  if (itClient == liveConnections.end()) return;
  auto itState = telnetRepls.find(id);
  if (itState == telnetRepls.end()) return;
  auto itOut = telnetOutputs.find(id);
  if (itOut == telnetOutputs.end()) return;

  ReplState& state = itState->second;
  AsyncClient* client = itClient->second;
  TelnetOut* out = itOut->second;
  if (!client->connected()) return;

  // Walk the buffer, looking for newlines.
  size_t i = 0;
  while (i < state.len) {
    if (state.buf[i] != '\n') { i++; continue; }
    // Process everything up to and including this newline.
    state.buf[i] = '\0';   // null-terminate the command at the newline
    // Echo a CR+LF (telnet clients expect it; we already echoed each
    // char as it arrived so we just need to push to next line
    // visually).
    out->print("\r\n");

    // The "command" is at the start of the buffer. But if we're inside
    // an open statement, the whole partial line is the start; we just
    // don't evaluate yet. To handle this correctly, run the open-
    // statement check on the whole buffer including the current line,
    // and only evaluate when it closes.
    // For simplicity here, check the buffer up to and including the
    // current newline.
    int parenDepth = 0, bracketDepth = 0, braceDepth = 0;
    char inString = 0;
    bool escaped = false;
    for (size_t k = 0; k <= i; k++) {
      char ch = state.buf[k];
      if (escaped) { escaped = false; continue; }
      if (inString) {
        if (ch == '\\') { escaped = true; continue; }
        if (ch == inString) inString = 0;
        continue;
      }
      if (ch == '\'' || ch == '"' || ch == '`') { inString = ch; continue; }
      if (ch == '(') parenDepth++;
      else if (ch == ')') { if (parenDepth > 0) parenDepth--; }
      if (ch == '[') bracketDepth++;
      else if (ch == ']') { if (bracketDepth > 0) bracketDepth--; }
      if (ch == '{') braceDepth++;
      else if (ch == '}') { if (braceDepth > 0) braceDepth--; }
    }
    bool openStatement = (parenDepth > 0) || (bracketDepth > 0) ||
                         (braceDepth > 0) || (inString != 0);

    if (openStatement) {
      // Still in a multi-line construct; print the continuation
      // prompt and consume the newline from the buffer.
      out->print("js. ");
      // Shift remaining bytes down.
      size_t remaining = state.len - (i + 1);
      memmove(state.buf, state.buf + i + 1, remaining);
      state.len = remaining;
      i = 0;
      continue;
    }

    // Statement complete. Evaluate it.
    // Set the active output stream so console.log / errors route to
    // this telnet client. Also tag this REPL as the current telnet
    // owner so setTimeout/setInterval can register timers that will
    // be cleaned up (or rerouted, if nohup=true) when this client
    // disconnects.
    activeOutputStream = (Stream*)out;
    currentTelnetId = id;
    if (g_jsrepl) {
      g_jsrepl->processCommand(state.buf);
    }
    // NOTE: do NOT clear activeOutputStream here. async callbacks
    // (.then() attached to a pending Promise, or setTimeout/setInterval
    // timers) may fire later, after the command handler returns.
    // They need activeOutputStream to still point to the right client
    // so console.log / errors route back to the originating REPL. The
    // next command on any client overwrites activeOutputStream, so a
    // stale value is fine.
    currentTelnetId = 0;

    out->print("js> ");

    // Shift remaining bytes down (skip the newline + null we just
    // consumed).
    size_t remaining = state.len - (i + 1);
    memmove(state.buf, state.buf + i + 1, remaining);
    state.len = remaining;
    i = 0;
  }
}

void JSTelnetServer::loop() {
  while (incomingConnections.size() > 0) {
    initClient(incomingConnections.top());
    incomingConnections.pop();
  }
  for (auto& kv : liveConnections) {
    handleClientInput(kv.first);
  }
}

JSValue JSTelnetServer::getJSNamespace(JSContext* ctx) {
  JSValue telnetNs = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, telnetNs, "start",
                    JS_NewCFunction(ctx, [](JSContext* ctx_, JSValueConst,
                                            int argc, JSValueConst* argv) -> JSValue {
                      if (g_jstelnet) g_jstelnet->start();
                      return JS_UNDEFINED;
                    }, "start", 0));
  JS_SetPropertyStr(ctx, telnetNs, "stop",
                    JS_NewCFunction(ctx, [](JSContext* ctx_, JSValueConst,
                                            int argc, JSValueConst* argv) -> JSValue {
                      if (g_jstelnet) g_jstelnet->stop();
                      return JS_UNDEFINED;
                    }, "stop", 0));
  return telnetNs;
}

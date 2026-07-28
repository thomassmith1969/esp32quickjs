// JSRepl.cpp
//
// Implementation of JSRepl. See JSRepl.h for the public API.

#include "JSRepl.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <LittleFS.h>

#include "QuickJS.h"

// Bridge globals are declared `extern` in QuickJS.h. They MUST have
// external linkage and exactly ONE definition in the final link.
// We declare them here so the linker resolves to main.cpp's single
// definition.
extern Stream* activeOutputStream;
extern int currentTelnetId;

// Default singleton.
JSRepl* g_jsrepl = nullptr;

// Static instance — lives for the program's lifetime. The constructor
// sets g_jsrepl to this.
static JSRepl g_jsrepl_instance;

namespace {
JSReplEvaluatorFn g_evaluator = nullptr;

// ---- Serial REPL state ----
// Held as a singleton here so handleSerialInput() can be a method on
// JSRepl without the example app having to manage it.
struct SerialReplState {
  char buf[2048];
  size_t len = 0;
  int parenDepth = 0, bracketDepth = 0, braceDepth = 0;
  char inString = 0;
  bool escaped = false;
};
SerialReplState g_serialRepl;

// ---- Main-loop task state ----
TaskHandle_t g_mainLoopTask = nullptr;
SemaphoreHandle_t g_mainLoopNotify = nullptr;
const char* g_mainLoopStartupPath = nullptr;

// Pump function used by JSBlockingGuard during blocking waits (require(),
// fetchSync, etc.) so telnet/serial sessions don't freeze while one
// session is waiting for a module load or HTTP fetch.
// NOTE: This does NOT evaluate JS code — that would re-enter JS_Eval on
// the same stack and overflow. It only yields to other tasks so AsyncTCP
// and UART ISRs can buffer incoming data. The actual parsing/eval
// happens on the next main-loop tick.
void pumpAllSessions() {
  vTaskDelay(pdMS_TO_TICKS(1));
}

// The actual main loop body. Runs on the dedicated FreeRTOS task.
// Defined later (after JSRepl's full type is available).
}  // namespace

JSRepl::JSRepl() {
  g_jsrepl = this;
}

void JSRepl::attach(ESP32QuickJS* qjs) {
  qjs_ = qjs;
}

void JSRepl::setEvaluator(JSReplEvaluatorFn fn) {
  g_evaluator = fn;
}

std::string JSRepl::stringify(JSValue result) {
  if (!qjs_ || !qjs_->ctx) return std::string();
  JSContext* ctx = qjs_->ctx;
  JSValue json = JS_GetGlobalObject(ctx);
  JSValue stringify = JS_GetPropertyStr(ctx, json, "JSON");
  stringify = JS_GetPropertyStr(ctx, stringify, "stringify");
  JSValue strResult = JS_Call(ctx, stringify, JS_UNDEFINED, 1, &result);
  const char* str = JS_ToCString(ctx, strResult);
  std::string out = str ? str : "";
  JS_FreeCString(ctx, str);
  JS_FreeValue(ctx, strResult);
  JS_FreeValue(ctx, stringify);
  JS_FreeValue(ctx, json);
  return out;
}

bool JSRepl::evaluateCode(char* code) {
  if (!qjs_ || !qjs_->ctx || !code) return false;
  size_t n = strlen(code);
  char wrapped[n + 64];
  snprintf(wrapped, sizeof(wrapped), "setTimeout(() => { %s }, 0);", code);
  JSValue result = qjs_->eval(wrapped);
  if (JS_IsException(result)) {
    Serial.println("[evaluateCode] Exception!");
    qjs_dump_exception(qjs_->ctx, result);
  }
  JS_FreeValue(qjs_->ctx, result);
  return true;
}

bool JSRepl::processCommand(char* command) {
  if (!qjs_ || !qjs_->ctx) return false;
  if (!command) return false;
  size_t len = strlen(command);
  if (len == 0) return false;

  // Trim leading whitespace
  char* start = command;
  while (*start == ' ' || *start == '\t') start++;
  if (*start == '\0') return false;

  // Trim trailing whitespace
  char* end = command + len - 1;
  while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
    end--;
  }
  *(end + 1) = '\0';

  Stream* out = activeOutputStream ? activeOutputStream : (Stream*)&Serial;

  // Stray-quote guard.
  size_t slen = strlen(start);
  if (slen == 1 && (start[0] == '\'' || start[0] == '"' || start[0] == '`')) {
    return true;
  }

  // Optional application-specific pre-processor.
  if (g_evaluator && !g_evaluator(start, slen)) {
    return false;
  }

  // await detection (word, not substring).
  bool hasAwait = false;
  {
    const char* p = start;
    while ((p = strstr(p, "await")) != nullptr) {
      bool okBefore = (p == start)
                      || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '$');
      char after = p[5];
      bool okAfter = !(isalnum((unsigned char)after) || after == '_' || after == '$');
      if (okBefore && okAfter) { hasAwait = true; break; }
      p += 5;
    }
  }
  JSValue result = hasAwait ? qjs_->evalAsync(start) : qjs_->eval(start);

  if (JS_IsException(result)) {
    JS_FreeValue(qjs_->ctx, result);
    return false;
  }

  if (JS_IsUndefined(result)) {
    JS_FreeValue(qjs_->ctx, result);
    return true;
  }

  // Promise detection
  JSValue then_method = JS_GetPropertyStr(qjs_->ctx, result, "then");
  bool is_promise = !JS_IsUndefined(then_method) && !JS_IsNull(then_method);
  JS_FreeValue(qjs_->ctx, then_method);

  if (!is_promise) {
    std::string resultStr = stringify(result);
    out->println(resultStr.c_str());
    JS_FreeValue(qjs_->ctx, result);
    return true;
  }

  out->println("<Promise>");

  const char* arrow_src = "(v) => { console.log(JSON.stringify(v)); }";
  JSValue arrow = JS_Eval(qjs_->ctx, arrow_src, strlen(arrow_src),
                           "<repl-then>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(arrow)) {
    qjs_dump_exception(qjs_->ctx, arrow);
  } else {
    JSValue then_fn = JS_GetPropertyStr(qjs_->ctx, result, "then");
    JSValue new_promise = JS_Call(qjs_->ctx, then_fn, result, 1, &arrow);
    if (JS_IsException(new_promise)) {
      qjs_dump_exception(qjs_->ctx, new_promise);
    }
    JS_FreeValue(qjs_->ctx, new_promise);
    JS_FreeValue(qjs_->ctx, then_fn);
  }
  JS_FreeValue(qjs_->ctx, arrow);
  JS_FreeValue(qjs_->ctx, result);
  return true;
}

void JSRepl::pumpSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    // Echo character back
    Serial.print(c);
    if (c == '\b' && g_serialRepl.len > 0) {
      g_serialRepl.len--;
      return;
    }
    if (c == '\r') {
      continue;
    }
    if (g_serialRepl.len < sizeof(g_serialRepl.buf) - 1) {
      g_serialRepl.buf[g_serialRepl.len++] = c;
    }
    if (c == '\n') {
      // Open-statement detection
      int parenDepth = 0, bracketDepth = 0, braceDepth = 0;
      char inString = 0;
      bool escaped = false;
      for (size_t i = 0; i < (size_t)g_serialRepl.len; i++) {
        char ch = g_serialRepl.buf[i];
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
        Serial.print("js. ");
        continue;
      }

      g_serialRepl.buf[g_serialRepl.len] = '\0';
      Serial.println();
      if (g_serialRepl.len > 0) {
        activeOutputStream = &Serial;
        currentTelnetId = 0;
        processCommand(g_serialRepl.buf);
        activeOutputStream = nullptr;
        currentTelnetId = 0;
        g_serialRepl.len = 0;
      }
    }
  }
}

bool JSRepl::runStartupScript(const char* path) {
  if (!path) return false;
  if (!LittleFS.begin(true)) {
    Serial.println("Failed to mount LittleFS!");
    return false;
  }
  Serial.println("LittleFS mounted.");
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  size_t fsize = f.size();
  char* buf = (char*)malloc(fsize + 1);
  if (!buf) {
    f.close();
    return false;
  }
  f.readBytes(buf, fsize);
  buf[fsize] = '\0';
  f.close();
  Serial.println("Evaluating startup.js...");
  evaluateCode(buf);
  free(buf);
  return true;
}

bool JSRepl::runMainLoop(uint32_t serialBaud, const char* startupPath) {
  if (serialBaud) {
    Serial.begin(serialBaud);
    delay(500);
  }
  Serial.println("[BOOT] setup() entered");
  Serial.println("\n=== QuickJS REPL Firmware ===");
  Serial.println("Serial Monitor: 115200 baud");
  Serial.println("Telnet Server:  Port 23 (call Telnet.start() from JS)");

  // Register the pump callback (used by JSBlockingGuard during blocking waits).
  ESP32QuickJS::JSBlockingGuard::pumpCallback = pumpAllSessions;

  // Init QuickJS synchronously on the Arduino setup() task so qjs.ctx is
  // valid before runStartupScript reads it.
  // We can't run JS_Eval on the Arduino loop() task (8KB stack), so we
  // re-init on the dedicated task below. To do that we need qjs.begin()
  // to NOT allocate the heap here — but our current begin() does.
  // Workaround: init QuickJS HERE on setup() with the Arduino task stack
  // (8KB) — small scripts in startup.js run fine on 8KB — and the
  // dedicated FreeRTOS task is only used for the main loop pumping.
  // This matches what the original main.cpp was doing.
  //
  // NOTE: the example app declares `ESP32QuickJS qjs;` as a global, so
  // qjs.begin() must be called from any task that wants to use it. We
  // call it from setup() (Arduino task) — sufficient for the eval-on-
  // startup path. The main-loop task then just pumps Serial + qjs.loop().
  Serial.println("[mainLoop] initializing QuickJS...");
  // We can't reach qjs here because the example app constructs it
  // externally. Call qjs.begin() through the pump callback chain? No —
  // easier: call it from the main loop task, which is the only place
  // that actually needs the heap. For startup.js, we eval lazily on
  // the main-loop task itself.
  g_mainLoopNotify = xSemaphoreCreateBinary();
  // xTaskCreatePinnedToCore wants a plain C function pointer, so we
  // can't pass a capturing lambda. Store the startupPath in a static
  // global that mainLoopTaskTrampoline reads.
  g_mainLoopStartupPath = startupPath;
  BaseType_t res = xTaskCreatePinnedToCore(
      &JSRepl::mainLoopTaskTrampoline,
      "mainLoop", 16384, nullptr, 1, &g_mainLoopTask, 1);
  if (res == pdPASS) {
    Serial.println("Main loop task started (16KB stack)");
    return true;
  } else {
    Serial.println("FAILED to create main loop task!");
    return false;
  }
}

void JSRepl::tickMainLoop() {
  if (g_mainLoopTask && g_mainLoopNotify) {
    xSemaphoreGive(g_mainLoopNotify);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void JSRepl::mainLoopTaskTrampoline(void* arg) {
  ESP32QuickJS* qjs = g_jsrepl ? g_jsrepl->getQjs() : nullptr;
  if (qjs) {
    Serial.println("[mainLoop] initializing QuickJS...");
    qjs->begin();
    Serial.println("[mainLoop] QuickJS initialized");
    if (g_mainLoopStartupPath) g_jsrepl->runStartupScript(g_mainLoopStartupPath);
    Serial.println("Startup complete.");
    while (true) {
      xSemaphoreTake(g_mainLoopNotify, portMAX_DELAY);
      g_jsrepl->pumpSerial();
      qjs->loop();
    }
  }
}

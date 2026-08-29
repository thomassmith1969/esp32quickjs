#pragma once
#include "JSStash.h"
#include "../quickjs.h"
#include <vector>
#include <functional>

struct PendingExecution {
    std::function<void*(JSContext*)> resolveFunc;
};

extern std::vector<PendingExecution> g_pendingExecutions;
extern SemaphoreHandle_t g_pendingExecsMutex;

void pushPendingOp(PendingExecution op);
bool popAndExecutePendingOp(JSContext* ctx);
void runPendingOps(JSContext* ctx);
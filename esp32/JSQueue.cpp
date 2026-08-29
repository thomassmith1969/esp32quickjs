#include "JSQueue.h"
std::vector<PendingExecution> g_pendingExecutions;
SemaphoreHandle_t g_pendingExecsMutex = xSemaphoreCreateMutex();

void pushPendingOp(PendingExecution op) {
    xSemaphoreTake(g_pendingExecsMutex, portMAX_DELAY);
    g_pendingExecutions.push_back(std::move(op));
    xSemaphoreGive(g_pendingExecsMutex);
}

bool popAndExecutePendingOp(JSContext* ctx) {
    xSemaphoreTake(g_pendingExecsMutex, portMAX_DELAY);
    if (g_pendingExecutions.empty()) {
        xSemaphoreGive(g_pendingExecsMutex);
        return false;
    }
    PendingExecution op = std::move(g_pendingExecutions.back());
    g_pendingExecutions.pop_back();
    xSemaphoreGive(g_pendingExecsMutex);
    if (op.resolveFunc) {
        op.resolveFunc(ctx);
    }
    return true;
}

void runPendingOps(JSContext* ctx) {
    while (popAndExecutePendingOp(ctx)) {}
}

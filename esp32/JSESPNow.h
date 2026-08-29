#include "../quickjs.h"
#include "JSQueue.h"
#include "esp_now.h"
#include "JSStash.h"




void js_init_espnow(JSContext *ctx, JSValue global) {
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

    JS_SetPropertyStr(ctx, en, "init", JS_NewCFunction(ctx, espnow_init, "init", 0));
    JS_SetPropertyStr(ctx, en, "end", JS_NewCFunction(ctx, espnow_end, "end", 0));
    JS_SetPropertyStr(ctx, en, "addPeer", JS_NewCFunction(ctx, espnow_add_peer, "addPeer", 1));
    JS_SetPropertyStr(ctx, en, "delPeer", JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
        return espnow_del_peer(ctx, this_val, argc, argv);
    }, "delPeer", 1));
    JS_SetPropertyStr(ctx, en, "peerExists", JS_NewCFunction(ctx, espnow_peer_exists, "peerExists", 1));
    JS_SetPropertyStr(ctx, en, "send", JS_NewCFunction(ctx, espnow_send, "send", 2));
    JS_SetPropertyStr(ctx, en, "broadcast", JS_NewCFunction(ctx, espnow_broadcast, "broadcast", 1));
    JS_SetPropertyStr(ctx, en, "onReceive", JS_NewCFunction(ctx, espnow_on_receive, "onReceive", 1));
    JS_SetPropertyStr(ctx, en, "onSend", JS_NewCFunction(ctx, espnow_on_send, "onSend", 1));
}

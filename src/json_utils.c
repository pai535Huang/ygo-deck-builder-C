#include "json_utils.h"

JsonNode* json_node_deep_copy(JsonNode *node) {
    if (!node) return NULL;

    // json_node_copy() 对 object/array 只是增加引用计数（浅拷贝），
    // 无法用于“改副本而不影响共享缓存”的场景，故走序列化往返。
    gchar *text = json_to_string(node, FALSE);
    if (!text) return NULL;

    JsonNode *copy = json_from_string(text, NULL);
    g_free(text);
    return copy;
}

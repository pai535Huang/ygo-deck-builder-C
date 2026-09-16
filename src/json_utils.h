#ifndef JSON_UTILS_H
#define JSON_UTILS_H

#include <json-glib/json-glib.h>

/**
 * 真正深拷贝一个 JsonNode。
 *
 * 注意 json_node_copy() 只是“浅拷贝”：对 object/array 节点它只增加内部引用计数，
 * 返回的 JsonObject* 与原对象是同一个指针，因此修改副本同样会修改原对象。
 * 对来自 cards.json / 先行卡共享解析缓存的卡片对象来说，那等于永久污染缓存
 * （例如注入 is_prerelease / forbidden_change 后，后续所有读取者都会看到）。
 * 本函数通过序列化往返得到与原对象完全独立的副本。
 *
 * @param node 源节点
 * @return 独立的深拷贝，需要调用者用 json_node_free() 释放；失败返回 NULL
 */
JsonNode* json_node_deep_copy(JsonNode *node);

#endif // JSON_UTILS_H

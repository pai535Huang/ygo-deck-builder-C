#include "forbidden_list.h"
#include <json-glib/json-glib.h>

// 加载禁限卡表JSON文件
GHashTable* load_forbidden_list(const char *filename) {
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    
    if (!json_parser_load_from_file(parser, filename, &error)) {
        if (error) {
            g_warning("Failed to load forbidden list %s: %s", filename, error->message);
            g_error_free(error);
        }
        g_object_unref(parser);
        return table;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return table;
    }
    
    JsonObject *obj = json_node_get_object(root);
    GList *members = json_object_get_members(obj);
    
    for (GList *l = members; l != NULL; l = l->next) {
        const char *cid = (const char *)l->data;
        const char *status = json_object_get_string_member(obj, cid);
        if (cid && status) {
            g_hash_table_insert(table, g_strdup(cid), g_strdup(status));
        }
    }
    
    g_list_free(members);
    g_object_unref(parser);
    
    return table;
}

// 获取卡片在指定禁限卡表中的最大数量限制
int get_card_limit_from_table(GHashTable *forbidden_table, int card_id) {
    if (!forbidden_table) return 3; // 无限制
    
    char key[32];
    g_snprintf(key, sizeof(key), "%d", card_id);
    
    const char *status = (const char*)g_hash_table_lookup(forbidden_table, key);
    if (!status) return 3; // 不在表中，无限制
    
    if (g_strcmp0(status, "forbidden") == 0) return 0; // 禁止
    if (g_strcmp0(status, "limited") == 0) return 1;   // 限制1
    if (g_strcmp0(status, "semi_limited") == 0) return 2; // 限制2
    
    return 3; // 默认无限制
}

// 加载禁限卡表变更信息JSON文件
GHashTable* load_forbidden_changes(const char *filename) {
    GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    
    if (!json_parser_load_from_file(parser, filename, &error)) {
        if (error) {
            g_warning("Failed to load forbidden changes %s: %s", filename, error->message);
            g_error_free(error);
        }
        g_object_unref(parser);
        return table;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return table;
    }
    
    JsonObject *obj = json_node_get_object(root);
    GList *members = json_object_get_members(obj);
    
    for (GList *l = members; l != NULL; l = l->next) {
        const char *card_key = (const char *)l->data;
        JsonNode *change_node = json_object_get_member(obj, card_key);

        if (!change_node || !JSON_NODE_HOLDS_OBJECT(change_node)) {
            continue;
        }

        JsonObject *change_obj = json_node_get_object(change_node);
        JsonNode *old_node = json_object_get_member(change_obj, "old");
        JsonNode *new_node = json_object_get_member(change_obj, "new");

        if (!old_node || !new_node ||
            !JSON_NODE_HOLDS_VALUE(old_node) || !JSON_NODE_HOLDS_VALUE(new_node) ||
            json_node_get_value_type(old_node) != G_TYPE_STRING ||
            json_node_get_value_type(new_node) != G_TYPE_STRING) {
            continue;
        }

        const char *old_status = json_node_get_string(old_node);
        const char *new_status = json_node_get_string(new_node);

        if (old_status && new_status && *old_status && *new_status) {
            // 存储为"old→new"格式的字符串
            gchar *change_str = g_strdup_printf("%s→%s", old_status, new_status);
            g_hash_table_insert(table, g_strdup(card_key), change_str);
        }
    }
    
    g_list_free(members);
    g_object_unref(parser);
    
    return table;
}

// 获取指定卡片名称的禁限变更信息
gboolean get_forbidden_change(GHashTable *changes_table, const char *card_key,
                               const char **out_old, const char **out_new) {
    if (!changes_table || !card_key) return FALSE;
    
    const char *change_str = (const char*)g_hash_table_lookup(changes_table, card_key);
    if (!change_str) return FALSE;
    
    // 解析"old→new"格式
    const char *arrow = strstr(change_str, "→");
    if (!arrow) return FALSE;
    
    // 返回指针指向箭头前后的部分
    *out_old = change_str;
    *out_new = arrow + strlen("→");
    
    return TRUE;
}

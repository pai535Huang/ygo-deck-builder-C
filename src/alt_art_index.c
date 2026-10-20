#include "alt_art_index.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>

struct AltArtIndex {
    GHashTable *base_to_alts; // int base -> GArray* of int alt ids
    GHashTable *art_to_base;  // int alt  -> int base
};

static gint cmp_int_ptr(gconstpointer a, gconstpointer b) {
    int ia = GPOINTER_TO_INT(a);
    int ib = GPOINTER_TO_INT(b);
    return (ia > ib) - (ia < ib);
}

static gint cmp_int_element(gconstpointer a, gconstpointer b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

AltArtIndex *alt_art_index_new(void) {
    AltArtIndex *index = g_new0(AltArtIndex, 1);
    index->base_to_alts = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                NULL, (GDestroyNotify)g_array_unref);
    index->art_to_base = g_hash_table_new(g_direct_hash, g_direct_equal);
    return index;
}

void alt_art_index_free(AltArtIndex *index) {
    if (!index) return;
    if (index->base_to_alts) g_hash_table_unref(index->base_to_alts);
    if (index->art_to_base) g_hash_table_unref(index->art_to_base);
    g_free(index);
}

gboolean alt_art_index_add(AltArtIndex *index, int base_id, int art_id) {
    if (!index || base_id <= 0 || art_id <= 0 || base_id == art_id) return FALSE;
    // 一个异画 ID 只能属于一个本体，避免下载数据异常时交叉污染。
    if (g_hash_table_contains(index->art_to_base, GINT_TO_POINTER(art_id))) return FALSE;

    GArray *alts = g_hash_table_lookup(index->base_to_alts, GINT_TO_POINTER(base_id));
    if (alts) {
        for (guint i = 0; i < alts->len; i++) {
            if (g_array_index(alts, int, i) == art_id) return FALSE;
        }
    } else {
        alts = g_array_new(FALSE, FALSE, sizeof(int));
        g_hash_table_insert(index->base_to_alts, GINT_TO_POINTER(base_id), alts);
    }

    g_array_append_val(alts, art_id);
    g_hash_table_insert(index->art_to_base, GINT_TO_POINTER(art_id), GINT_TO_POINTER(base_id));
    return TRUE;
}

void alt_art_index_sort(AltArtIndex *index) {
    if (!index) return;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, index->base_to_alts);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        (void)key;
        GArray *alts = (GArray *)value;
        if (!alts || alts->len < 2) continue;
        g_array_sort(alts, cmp_int_element);
        // 去重（正常情况下 add 已挡住重复，这里防御异常数据）
        for (guint i = alts->len; i > 1; i--) {
            if (g_array_index(alts, int, i - 1) == g_array_index(alts, int, i - 2)) {
                g_array_remove_index(alts, i - 1);
            }
        }
    }
}

GArray *alt_art_index_get_variants(AltArtIndex *index, int base_id) {
    if (!index || base_id <= 0) return NULL;
    GArray *alts = g_hash_table_lookup(index->base_to_alts, GINT_TO_POINTER(base_id));
    if (!alts || alts->len == 0) return NULL;

    GArray *result = g_array_sized_new(FALSE, FALSE, sizeof(int), alts->len + 1);
    g_array_append_val(result, base_id);
    for (guint i = 0; i < alts->len; i++) {
        int art_id = g_array_index(alts, int, i);
        g_array_append_val(result, art_id);
    }
    return result;
}

int alt_art_index_get_base(AltArtIndex *index, int art_id) {
    if (!index || art_id <= 0) return art_id;
    gpointer base = g_hash_table_lookup(index->art_to_base, GINT_TO_POINTER(art_id));
    return base ? GPOINTER_TO_INT(base) : art_id;
}

gboolean alt_art_index_has_variants(AltArtIndex *index, int base_id) {
    if (!index || base_id <= 0) return FALSE;
    GArray *alts = g_hash_table_lookup(index->base_to_alts, GINT_TO_POINTER(base_id));
    return alts && alts->len > 0;
}

guint alt_art_index_base_count(AltArtIndex *index) {
    return index ? g_hash_table_size(index->base_to_alts) : 0;
}

static gboolean node_get_positive_int(JsonNode *node, gint64 *out) {
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || !out) return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_INT64 && type != G_TYPE_INT && type != G_TYPE_LONG) return FALSE;
    gint64 value = json_node_get_int(node);
    if (value <= 0) return FALSE;
    *out = value;
    return TRUE;
}

static gboolean parse_positive_int(const gchar *text, int *out) {
    if (!text || !*text || !out) return FALSE;
    gchar *end = NULL;
    gint64 value = g_ascii_strtoll(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > G_MAXINT) return FALSE;
    *out = (int)value;
    return TRUE;
}

gboolean alt_art_index_load_json(AltArtIndex *index, const char *json, GError **error) {
    if (!index) return FALSE;
    if (!json) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty alt art JSON");
        return FALSE;
    }

    JsonParser *parser = json_parser_new();
    GError *parse_error = NULL;
    if (!json_parser_load_from_data(parser, json, -1, &parse_error)) {
        g_propagate_error(error, parse_error);
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "alt art JSON root is not an object");
        g_object_unref(parser);
        return FALSE;
    }
    JsonObject *root_obj = json_node_get_object(root);
    if (!json_object_has_member(root_obj, "bases")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "alt art JSON has no bases");
        g_object_unref(parser);
        return FALSE;
    }

    // 先解析到临时索引，成功后整体替换，避免无效数据破坏现有映射。
    AltArtIndex *tmp = alt_art_index_new();
    JsonObject *bases = json_object_get_object_member(root_obj, "bases");
    if (bases) {
        JsonObjectIter iter;
        json_object_iter_init(&iter, bases);
        const gchar *member = NULL;
        JsonNode *node = NULL;
        while (json_object_iter_next(&iter, &member, &node)) {
            int base_id = 0;
            if (!parse_positive_int(member, &base_id)) continue;
            if (!node || !JSON_NODE_HOLDS_ARRAY(node)) continue;
            JsonArray *arr = json_node_get_array(node);
            guint len = json_array_get_length(arr);
            for (guint i = 0; i < len; i++) {
                JsonNode *item = json_array_get_element(arr, i);
                gint64 art = 0;
                if (node_get_positive_int(item, &art) && art <= G_MAXINT) {
                    alt_art_index_add(tmp, base_id, (int)art);
                }
            }
        }
    }
    alt_art_index_sort(tmp);

    g_hash_table_unref(index->base_to_alts);
    g_hash_table_unref(index->art_to_base);
    index->base_to_alts = tmp->base_to_alts;
    index->art_to_base = tmp->art_to_base;
    tmp->base_to_alts = NULL;
    tmp->art_to_base = NULL;
    alt_art_index_free(tmp);

    g_object_unref(parser);
    return TRUE;
}

gchar *alt_art_index_to_json(AltArtIndex *index) {
    if (!index) return NULL;
    alt_art_index_sort(index);

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "bases");
    json_builder_begin_object(builder);

    GList *keys = g_hash_table_get_keys(index->base_to_alts);
    keys = g_list_sort(keys, cmp_int_ptr);
    for (GList *l = keys; l; l = l->next) {
        int base_id = GPOINTER_TO_INT(l->data);
        GArray *alts = g_hash_table_lookup(index->base_to_alts, GINT_TO_POINTER(base_id));
        if (!alts) continue;
        gchar key[32];
        g_snprintf(key, sizeof key, "%d", base_id);
        json_builder_set_member_name(builder, key);
        json_builder_begin_array(builder);
        for (guint i = 0; i < alts->len; i++) {
            json_builder_add_int_value(builder, g_array_index(alts, int, i));
        }
        json_builder_end_array(builder);
    }
    g_list_free(keys);

    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);

    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(builder);
    return data;
}

GHashTable *alt_art_choices_parse_json(const char *json) {
    GHashTable *choices = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!json) return choices;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        if (error) g_error_free(error);
        g_object_unref(parser);
        return choices;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return choices;
    }
    JsonObject *root_obj = json_node_get_object(root);
    JsonObject *obj = json_object_has_member(root_obj, "choices")
                          ? json_object_get_object_member(root_obj, "choices")
                          : NULL;
    if (obj) {
        JsonObjectIter iter;
        json_object_iter_init(&iter, obj);
        const gchar *member = NULL;
        JsonNode *node = NULL;
        while (json_object_iter_next(&iter, &member, &node)) {
            int base_id = 0;
            if (!parse_positive_int(member, &base_id)) continue;
            gint64 art = 0;
            if (node_get_positive_int(node, &art) && art <= G_MAXINT) {
                g_hash_table_insert(choices, GINT_TO_POINTER(base_id), GINT_TO_POINTER((int)art));
            }
        }
    }

    g_object_unref(parser);
    return choices;
}

gchar *alt_art_choices_to_json(GHashTable *choices) {    if (!choices || g_hash_table_size(choices) == 0) {
        return g_strdup("{\"choices\":{}}");
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "choices");
    json_builder_begin_object(builder);

    GList *keys = g_hash_table_get_keys(choices);
    keys = g_list_sort(keys, cmp_int_ptr);
    for (GList *l = keys; l; l = l->next) {
        int base_id = GPOINTER_TO_INT(l->data);
        int art_id = GPOINTER_TO_INT(g_hash_table_lookup(choices, l->data));
        if (base_id <= 0 || art_id <= 0) continue;
        gchar key[32];
        g_snprintf(key, sizeof key, "%d", base_id);
        json_builder_set_member_name(builder, key);
        json_builder_add_int_value(builder, art_id);
    }
    g_list_free(keys);

    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);

    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(builder);
    return data;
}

GArray *alt_art_parse_image_metadata(const char *metadata, GHashTable *base_ids) {
    GArray *candidates = g_array_new(FALSE, FALSE, sizeof(int));
    if (!metadata) return candidates;

    const char *p = metadata;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);
        const char *q = p;
        gint64 id = 0;
        while (q < line_end && g_ascii_isdigit(*q)) {
            id = id * 10 + (*q - '0');
            if (id > G_MAXINT) { id = 0; break; }
            q++;
        }
        // 只接受 "数字.webp:..." 形式的行；.bin/.jpg 等一律跳过
        if (id > 0 && (line_end - q) >= 6 && strncmp(q, ".webp:", 6) == 0) {
            gboolean known = base_ids &&
                             g_hash_table_contains(base_ids, GINT_TO_POINTER((int)id));
            if (!known) {
                int value = (int)id;
                g_array_append_val(candidates, value);
            }
        }
        p = (*line_end == '\n') ? line_end + 1 : line_end;
    }
    return candidates;
}

guint alt_art_apply_cardset_json(AltArtIndex *index, const char *json, GHashTable *base_ids) {
    if (!index || !json || !base_ids) return 0;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, json, -1, &error)) {
        if (error) g_error_free(error);
        g_object_unref(parser);
        return 0;
    }

    guint added = 0;
    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);
        JsonObjectIter iter;
        json_object_iter_init(&iter, obj);
        const gchar *member = NULL;
        JsonNode *node = NULL;
        while (json_object_iter_next(&iter, &member, &node)) {
            int key_id = 0;
            if (!parse_positive_int(member, &key_id)) continue;
            if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
            JsonObject *entry = json_node_get_object(node);
            gint64 base = 0;
            gint64 altart = 0;
            if (!node_get_positive_int(json_object_get_member(entry, "id"), &base) ||
                !node_get_positive_int(json_object_get_member(entry, "altart"), &altart)) {
                continue;
            }
            if (base > G_MAXINT) continue;
            if (altart != key_id) continue;
            if (!g_hash_table_contains(base_ids, GINT_TO_POINTER((int)base))) continue;
            if (alt_art_index_add(index, (int)base, key_id)) added++;
        }
    }

    g_object_unref(parser);
    return added;
}

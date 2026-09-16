// json_utils 单元测试（meson test）
// 关键回归：json_node_deep_copy() 必须返回独立副本——修改副本不得影响原对象。
// （json_node_copy() 是浅拷贝，本测试正是为捕捉“深拷贝后再写标记”这一需求被
//   浅拷贝悄悄破坏的情况。）
#include "json_utils.h"
#include <stdio.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

static JsonNode* parse(const char *json) {
    JsonParser *p = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(p, json, -1, &err)) {
        if (err) g_error_free(err);
        g_object_unref(p);
        return NULL;
    }
    JsonNode *root = json_node_copy(json_parser_get_root(p));
    g_object_unref(p);
    return root;
}

static void test_deep_copy_is_independent(void) {
    JsonNode *orig = parse("{\"id\":89631139,\"text\":{\"name\":\"Blue-Eyes\"},\"tags\":[\"a\",\"b\"]}");
    CHECK(orig != NULL, "parse ok");
    if (!orig) return;

    JsonNode *copy = json_node_deep_copy(orig);
    CHECK(copy != NULL, "deep copy non-NULL");
    if (!copy) { json_node_free(orig); return; }

    JsonObject *orig_obj = json_node_get_object(orig);
    JsonObject *copy_obj = json_node_get_object(copy);

    // 这是 json_node_copy 会失败、json_node_deep_copy 必须通过的核心断言
    CHECK(copy_obj != orig_obj, "deep copy returns an independent object");

    // 顶层新增成员不得影响原对象
    json_object_set_boolean_member(copy_obj, "is_prerelease", TRUE);
    CHECK(!json_object_has_member(orig_obj, "is_prerelease"),
          "adding a member to the copy must not touch the original");

    json_object_set_string_member(copy_obj, "forbidden_change", "old->new");
    CHECK(!json_object_has_member(orig_obj, "forbidden_change"),
          "forbidden_change must not leak into the original");

    // 嵌套对象也必须独立
    JsonObject *orig_text = json_object_get_object_member(orig_obj, "text");
    JsonObject *copy_text = json_object_get_object_member(copy_obj, "text");
    CHECK(orig_text && copy_text && orig_text != copy_text, "nested object is independent");
    if (orig_text && copy_text) {
        json_object_set_string_member(copy_text, "name", "CHANGED");
        CHECK(g_strcmp0(json_object_get_string_member(orig_text, "name"), "Blue-Eyes") == 0,
              "mutating nested copy leaves original intact");
    }

    // 嵌套数组也必须独立
    JsonArray *orig_tags = json_object_get_array_member(orig_obj, "tags");
    JsonArray *copy_tags = json_object_get_array_member(copy_obj, "tags");
    CHECK(orig_tags && copy_tags && orig_tags != copy_tags, "nested array is independent");
    if (orig_tags && copy_tags) {
        json_array_add_string_element(copy_tags, "c");
        CHECK(json_array_get_length(orig_tags) == 2, "mutating nested array leaves original intact");
    }

    json_node_free(copy);
    json_node_free(orig);
}

static void test_values_are_preserved(void) {
    JsonNode *orig = parse("{\"id\":1234,\"level\":8,\"text\":{\"name\":\"X\"}}");
    CHECK(orig != NULL, "parse ok 2");
    if (!orig) return;

    JsonNode *copy = json_node_deep_copy(orig);
    CHECK(copy != NULL, "deep copy non-NULL 2");
    if (copy) {
        JsonObject *o = json_node_get_object(copy);
        CHECK(json_object_get_int_member(o, "id") == 1234, "int member preserved");
        CHECK(json_object_get_int_member(o, "level") == 8, "second int member preserved");
        JsonObject *text = json_object_get_object_member(o, "text");
        CHECK(text && g_strcmp0(json_object_get_string_member(text, "name"), "X") == 0,
              "nested string preserved");
        json_node_free(copy);
    }
    json_node_free(orig);
}

static void test_null_and_scalars(void) {
    CHECK(json_node_deep_copy(NULL) == NULL, "NULL in, NULL out");

    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_int(n, 42);
    JsonNode *c = json_node_deep_copy(n);
    CHECK(c != NULL && json_node_get_int(c) == 42, "scalar value copied");
    if (c) json_node_free(c);
    json_node_free(n);
}

int main(void) {
    test_deep_copy_is_independent();
    test_values_are_preserved();
    test_null_and_scalars();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_json_utils: all checks passed\n");
    return 0;
}

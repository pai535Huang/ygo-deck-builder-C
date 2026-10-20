// alt_art_index 单元测试（meson test）
// 覆盖：异画映射的增删查、版本列表顺序、JSON 往返、无效 JSON 容错，
// 以及“用户选择”JSON 的解析/序列化。
#include "alt_art_index.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

static guint array_len(GArray *a) {
    return a ? a->len : 0;
}

static int array_at(GArray *a, guint i) {
    return g_array_index(a, int, i);
}

static void test_empty_index(void) {
    AltArtIndex *index = alt_art_index_new();
    CHECK(index != NULL, "new index");
    if (!index) return;

    CHECK(alt_art_index_base_count(index) == 0, "empty index has no bases");
    CHECK(!alt_art_index_has_variants(index, 14558127), "empty index has no variants");
    CHECK(alt_art_index_get_variants(index, 14558127) == NULL, "empty lookup returns NULL");
    CHECK(alt_art_index_get_base(index, 14558129) == 14558129,
          "unknown art id resolves to itself");

    gchar *json = alt_art_index_to_json(index);
    CHECK(json != NULL, "empty index serializes");
    CHECK(json && strstr(json, "\"bases\"") != NULL, "serialized JSON has bases");
    g_free(json);
    alt_art_index_free(index);
}

static void test_add_and_lookup(void) {
    AltArtIndex *index = alt_art_index_new();
    CHECK(index != NULL, "new index 2");
    if (!index) return;

    CHECK(alt_art_index_add(index, 14558127, 14558129), "add first alt");
    CHECK(alt_art_index_add(index, 14558127, 14558128), "add second alt");
    CHECK(!alt_art_index_add(index, 14558127, 14558128), "duplicate alt ignored");
    CHECK(!alt_art_index_add(index, 14558127, 14558127), "base as alt ignored");
    CHECK(!alt_art_index_add(index, 0, 14558129), "invalid base ignored");

    alt_art_index_sort(index);
    GArray *variants = alt_art_index_get_variants(index, 14558127);
    CHECK(variants != NULL, "variants found");
    CHECK(array_len(variants) == 3, "base + two alts");
    if (variants && array_len(variants) == 3) {
        CHECK(array_at(variants, 0) == 14558127, "base is first");
        CHECK(array_at(variants, 1) == 14558128, "alts sorted ascending (1)");
        CHECK(array_at(variants, 2) == 14558129, "alts sorted ascending (2)");
    }
    if (variants) g_array_unref(variants);

    CHECK(alt_art_index_has_variants(index, 14558127), "has variants");
    CHECK(alt_art_index_base_count(index) == 1, "one base");
    CHECK(alt_art_index_get_base(index, 14558128) == 14558127, "alt resolves to base");
    CHECK(alt_art_index_get_base(index, 14558127) == 14558127, "base resolves to itself");
    CHECK(alt_art_index_get_base(index, 999999) == 999999, "unknown resolves to itself");

    alt_art_index_free(index);
}

static void test_json_roundtrip(void) {
    AltArtIndex *index = alt_art_index_new();
    CHECK(index != NULL, "new index 3");
    if (!index) return;

    alt_art_index_add(index, 89631139, 89631140);
    alt_art_index_add(index, 89631139, 89631142);
    alt_art_index_add(index, 46986414, 46986415);
    alt_art_index_sort(index);

    gchar *json = alt_art_index_to_json(index);
    CHECK(json != NULL, "serialize index");
    if (json) {
        CHECK(strstr(json, "\"89631139\"") != NULL, "serialized base id present");
        CHECK(strstr(json, "89631142") != NULL, "serialized alt id present");
    }

    AltArtIndex *loaded = alt_art_index_new();
    GError *error = NULL;
    CHECK(loaded && alt_art_index_load_json(loaded, json ? json : "", &error),
          "reload serialized index");
    if (error) g_error_free(error);
    if (loaded) {
        CHECK(alt_art_index_base_count(loaded) == 2, "roundtrip keeps both bases");
        GArray *v = alt_art_index_get_variants(loaded, 89631139);
        CHECK(v && array_len(v) == 3, "roundtrip keeps both alts");
        if (v && array_len(v) == 3) {
            CHECK(array_at(v, 1) == 89631140, "roundtrip alt 1");
            CHECK(array_at(v, 2) == 89631142, "roundtrip alt 2");
        }
        if (v) g_array_unref(v);
    }
    if (loaded) alt_art_index_free(loaded);
    g_free(json);
    alt_art_index_free(index);
}

static void test_invalid_json(void) {
    AltArtIndex *index = alt_art_index_new();
    CHECK(index != NULL, "new index 4");
    if (!index) return;

    GError *error = NULL;
    CHECK(!alt_art_index_load_json(index, "not json", &error), "invalid JSON rejected");
    CHECK(error != NULL, "invalid JSON sets error");
    if (error) g_error_free(error);

    error = NULL;
    CHECK(!alt_art_index_load_json(index, "{\"version\":1}", &error),
          "missing bases rejected");
    if (error) g_error_free(error);

    // 失败后索引应保持可用（空/原内容）
    CHECK(alt_art_index_base_count(index) == 0, "index still usable after error");
    alt_art_index_free(index);
}

static void test_choices_roundtrip(void) {
    GHashTable *choices = alt_art_choices_parse_json(
        "{\"choices\":{\"14558127\":14558129,\"89631139\":89631142}}");
    CHECK(choices != NULL, "choices parsed");
    if (!choices) return;

    CHECK(GPOINTER_TO_INT(g_hash_table_lookup(choices, GINT_TO_POINTER(14558127))) == 14558129,
          "choice lookup 1");
    CHECK(GPOINTER_TO_INT(g_hash_table_lookup(choices, GINT_TO_POINTER(89631139))) == 89631142,
          "choice lookup 2");

    gchar *json = alt_art_choices_to_json(choices);
    CHECK(json != NULL, "choices serialized");
    if (json) {
        GHashTable *again = alt_art_choices_parse_json(json);
        CHECK(again != NULL, "choices reparsed");
        if (again) {
            CHECK(GPOINTER_TO_INT(g_hash_table_lookup(again, GINT_TO_POINTER(14558127))) == 14558129,
                  "roundtrip choice 1");
            g_hash_table_unref(again);
        }
    }
    g_free(json);
    g_hash_table_unref(choices);
}

static void test_invalid_choices(void) {
    GHashTable *choices = alt_art_choices_parse_json("bad json");
    CHECK(choices != NULL, "invalid choices returns empty table");
    if (choices) {
        CHECK(g_hash_table_size(choices) == 0, "invalid choices empty");
        gchar *json = alt_art_choices_to_json(choices);
        CHECK(json && strcmp(json, "{\"choices\":{}}") == 0, "empty choices serialization");
        g_free(json);
        g_hash_table_unref(choices);
    }

    choices = alt_art_choices_parse_json(NULL);
    CHECK(choices != NULL && g_hash_table_size(choices) == 0, "NULL choices empty");
    if (choices) g_hash_table_unref(choices);

    // 非整数值必须被忽略，且不能触发 GLib 警告
    choices = alt_art_choices_parse_json("{\"choices\":{\"14558127\":\"oops\"}}");
    CHECK(choices != NULL && g_hash_table_size(choices) == 0, "non-int choice ignored");
    if (choices) g_hash_table_unref(choices);
}

static void test_load_ignores_non_int_elements(void) {
    AltArtIndex *index = alt_art_index_new();
    CHECK(index != NULL, "new index for mixed array");
    if (!index) return;

    GError *error = NULL;
    CHECK(alt_art_index_load_json(index,
          "{\"version\":1,\"bases\":{\"14558127\":[14558128,\"bad\"]}}", &error),
          "mixed array JSON loads");
    if (error) g_error_free(error);

    GArray *v = alt_art_index_get_variants(index, 14558127);
    CHECK(v && v->len == 2, "string element ignored");
    if (v && v->len == 2) {
        CHECK(g_array_index(v, int, 1) == 14558128, "valid element kept");
    }
    if (v) g_array_unref(v);
    alt_art_index_free(index);
}

static void test_parse_image_metadata(void) {
    GHashTable *bases = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(bases, GINT_TO_POINTER(89631139), GINT_TO_POINTER(1));
    g_hash_table_insert(bases, GINT_TO_POINTER(14558127), GINT_TO_POINTER(1));

    const char *metadata =
        "89631139.webp:96012,1,abc\n"
        "89631140.webp:116056,1,abc\n"
        "14558129.webp:101124,1,abc\n"
        "99999999.webp:1,1,abc\n"
        "not-a-line\n"
        "123.bin:1,1,abc\n";

    GArray *candidates = alt_art_parse_image_metadata(metadata, bases);
    CHECK(candidates != NULL, "metadata candidates returned");
    CHECK(candidates && candidates->len == 3, "only non-base .webp ids are candidates");
    if (candidates && candidates->len == 3) {
        CHECK(g_array_index(candidates, int, 0) == 89631140, "metadata candidate 1");
        CHECK(g_array_index(candidates, int, 1) == 14558129, "metadata candidate 2");
        CHECK(g_array_index(candidates, int, 2) == 99999999, "metadata candidate 3");
    }
    if (candidates) g_array_unref(candidates);
    g_hash_table_unref(bases);
}

static void test_apply_cardset_json(void) {
    GHashTable *bases = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(bases, GINT_TO_POINTER(14558127), GINT_TO_POINTER(1));

    const char *json =
        "{"
        "\"14558128\":{\"id\":14558127,\"altart\":14558128},"
        "\"14558129\":{\"id\":14558127,\"altart\":14558129},"
        "\"14558130\":{\"id\":14558127},"
        "\"55555555\":{\"id\":14558127,\"altart\":99999999},"
        "\"88888888\":{\"id\":88888888,\"altart\":88888888}"
        "}";

    AltArtIndex *index = alt_art_index_new();
    CHECK(index != NULL, "new index for cardset");
    if (!index) { g_hash_table_unref(bases); return; }

    guint added = alt_art_apply_cardset_json(index, json, bases);
    CHECK(added == 2, "only matching altart entries accepted");
    alt_art_index_sort(index);
    GArray *v = alt_art_index_get_variants(index, 14558127);
    CHECK(v && v->len == 3, "cardset merge yields base + 2 alts");
    if (v && v->len == 3) {
        CHECK(g_array_index(v, int, 1) == 14558128, "cardset alt 1");
        CHECK(g_array_index(v, int, 2) == 14558129, "cardset alt 2");
    }
    if (v) g_array_unref(v);

    alt_art_index_free(index);
    g_hash_table_unref(bases);
}

int main(void) {
    test_empty_index();
    test_add_and_lookup();
    test_json_roundtrip();
    test_invalid_json();
    test_choices_roundtrip();
    test_invalid_choices();
    test_parse_image_metadata();
    test_apply_cardset_json();
    test_load_ignores_non_int_elements();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_alt_art_index: all checks passed\n");
    return 0;
}

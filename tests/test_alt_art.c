// alt_art 运行时模块测试（meson test）
// 覆盖：从 cards/altarts.json 加载映射、正反向查询、用户选择持久化
// 以及 cleanup 后重新 init 仍能恢复默认异画。
// 测试使用临时 XDG 目录，不触碰真实用户配置。
#include "alt_art.h"
#include "offline_data.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

static gboolean ready_called = FALSE;

static void on_alt_art_ready(gpointer user_data) {
    (void)user_data;
    ready_called = TRUE;
}

static gboolean write_text(const char *path, const char *text) {
    GError *error = NULL;
    gboolean ok = g_file_set_contents(path, text, -1, &error);
    if (!ok && error) g_error_free(error);
    return ok;
}

int main(void) {
    gchar *root = g_dir_make_tmp("alt-art-test-XXXXXX", NULL);
    if (!root) {
        fprintf(stderr, "FAIL: cannot create temp dir\n");
        return 1;
    }

    gchar *data_home = g_build_filename(root, "data", NULL);
    gchar *config_home = g_build_filename(root, "config", NULL);
    g_setenv("XDG_DATA_HOME", data_home, TRUE);
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);

    gchar *cards_dir = g_build_filename(data_home, "ygo-deck-builder", "cards", NULL);
    gchar *config_dir = g_build_filename(config_home, "ygo-deck-builder", NULL);
    g_mkdir_with_parents(cards_dir, 0700);
    g_mkdir_with_parents(config_dir, 0700);

    gchar *cards_json_path = g_build_filename(cards_dir, "cards.json", NULL);
    CHECK(write_text(cards_json_path,
          "{\"12950\":{\"cid\":12950,\"id\":14558127,"
          "\"cn_name\":\"灰流丽\",\"text\":{\"name\":\"灰流丽\"}}}"),
          "write fake cards.json");

    gchar *cache_path = g_build_filename(cards_dir, "altarts.json", NULL);
    CHECK(write_text(cache_path,
          "{\"version\":1,\"bases\":{\"14558127\":[14558128,14558129]}}"),
          "write fake alt art cache");

    alt_art_set_ready_callback(on_alt_art_ready, NULL);
    alt_art_init();
    for (int i = 0; i < 10 && !ready_called; i++) {
        g_main_context_iteration(NULL, FALSE);
    }
    CHECK(ready_called, "ready callback fired after cache load");
    CHECK(alt_art_has_cache(), "cache loaded");
    CHECK(alt_art_get_base_id(14558129) == 14558127, "alt resolves to base");
    CHECK(alt_art_get_base_id(14558127) == 14558127, "base resolves to itself");

    GArray *variants = alt_art_get_variants(14558127);
    CHECK(variants && variants->len == 3, "three versions available");
    if (variants && variants->len == 3) {
        CHECK(g_array_index(variants, int, 0) == 14558127, "base first");
        CHECK(g_array_index(variants, int, 1) == 14558128, "alt 1");
        CHECK(g_array_index(variants, int, 2) == 14558129, "alt 2");
    }
    if (variants) g_array_unref(variants);

    CHECK(alt_art_get_selected(14558127) == 14558127, "default is base");

    // ydk 导入路径：异画密码 -> 本体密码 -> 本体 cid
    CHECK(offline_get_cid_by_img_id(14558127) == 12950, "base cid lookup");
    CHECK(offline_get_cid_by_img_id(14558129) == 0, "alt id is not indexed as base");
    CHECK(offline_get_cid_by_img_id(alt_art_get_base_id(14558129)) == 12950,
          "alt id maps back to base cid");

    alt_art_set_selected(14558127, 14558129);
    CHECK(alt_art_get_selected(14558127) == 14558129, "selection applied");

    // 模拟重启：cleanup 后重新 init，选择应从文件恢复
    alt_art_cleanup();
    alt_art_init();
    CHECK(alt_art_get_selected(14558127) == 14558129, "selection persists across re-init");

    // 选回原画应删除记录
    alt_art_set_selected(14558127, 14558127);
    CHECK(alt_art_get_selected(14558127) == 14558127, "selecting base clears choice");

    alt_art_set_ready_callback(NULL, NULL);
    alt_art_cleanup();

    gchar *choices_path = g_build_filename(config_dir, "altart_choices.json", NULL);
    g_remove(choices_path);
    g_remove(cache_path);
    g_remove(cards_json_path);
    g_rmdir(cards_dir);
    gchar *data_app_dir = g_build_filename(data_home, "ygo-deck-builder", NULL);
    g_rmdir(data_app_dir);
    g_rmdir(config_dir);
    g_rmdir(data_home);
    g_rmdir(config_home);
    g_rmdir(root);

    g_free(choices_path);
    g_free(cache_path);
    g_free(cards_json_path);
    g_free(config_dir);
    g_free(cards_dir);
    g_free(config_home);
    g_free(data_home);
    g_free(root);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_alt_art: all checks passed\n");
    return 0;
}

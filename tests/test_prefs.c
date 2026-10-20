// 单元测试：settings.conf 布尔配置的默认值与持久化。
// 通过设置 XDG_CONFIG_HOME 指向临时目录，避免读写真实用户配置。
#include <glib.h>
#include <glib/gstdio.h>
#include "prefs.h"

static gchar *tmp_root = NULL;
static gchar *config_dir = NULL;
static gchar *config_path = NULL;

static void
test_missing_key_uses_default(void)
{
    g_assert_true(prefs_get_boolean("Preferences", "PrereleaseEnabled", TRUE));
    g_assert_false(prefs_get_boolean("Preferences", "PrereleaseEnabled", FALSE));
}

static void
test_boolean_roundtrip(void)
{
    g_assert_true(prefs_set_boolean("Preferences", "PrereleaseEnabled", TRUE));
    g_assert_true(g_file_test(config_path, G_FILE_TEST_EXISTS));
    g_assert_true(prefs_get_boolean("Preferences", "PrereleaseEnabled", FALSE));

    g_assert_true(prefs_set_boolean("Preferences", "PrereleaseEnabled", FALSE));
    g_assert_false(prefs_get_boolean("Preferences", "PrereleaseEnabled", TRUE));
}

static void
write_existing_config(const gchar *contents)
{
    GError *error = NULL;

    g_assert_cmpint(g_mkdir_with_parents(config_dir, 0755), ==, 0);
    g_assert_true(g_file_set_contents(config_path, contents, -1, &error));
    g_assert_no_error(error);
}

static void
test_set_boolean_preserves_other_keys(void)
{
    const gchar *initial =
        "[Preferences]\n"
        "OfflineDataEnabled=true\n"
        "\n"
        "[Directories]\n"
        "LastExportDirectory=/tmp/decks\n";
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    gchar *value;

    write_existing_config(initial);

    g_assert_true(prefs_set_boolean("Preferences", "PrereleaseEnabled", TRUE));

    g_assert_true(g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error));
    g_assert_no_error(error);

    g_assert_true(g_key_file_get_boolean(keyfile, "Preferences",
                                         "OfflineDataEnabled", &error));
    g_assert_no_error(error);
    g_assert_true(g_key_file_get_boolean(keyfile, "Preferences",
                                         "PrereleaseEnabled", &error));
    g_assert_no_error(error);

    value = g_key_file_get_string(keyfile, "Directories",
                                  "LastExportDirectory", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(value, ==, "/tmp/decks");

    g_free(value);
    g_key_file_free(keyfile);
}

int
main(int argc, char *argv[])
{
    gchar *tmp_dir = g_dir_make_tmp("ygo-prefs-test-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);

    tmp_root = g_strdup(tmp_dir);
    g_free(tmp_dir);

    // g_get_user_config_dir() 只在首次调用时读取环境，必须在任何 prefs 调用前设置
    g_setenv("XDG_CONFIG_HOME", tmp_root, TRUE);

    config_dir = g_build_filename(tmp_root, "ygo-deck-builder", NULL);
    config_path = g_build_filename(config_dir, "settings.conf", NULL);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/prefs/missing-key-uses-default", test_missing_key_uses_default);
    g_test_add_func("/prefs/boolean-roundtrip", test_boolean_roundtrip);
    g_test_add_func("/prefs/preserves-other-keys", test_set_boolean_preserves_other_keys);

    int result = g_test_run();

    g_remove(config_path);
    g_rmdir(config_dir);
    g_rmdir(tmp_root);
    g_free(config_path);
    g_free(config_dir);
    g_free(tmp_root);

    return result;
}

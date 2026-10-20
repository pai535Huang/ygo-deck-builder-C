// 单元测试：取消“启用先行卡”后清空本地先行卡数据。
// 通过 XDG_DATA_HOME 指向临时目录，避免触碰真实用户数据。
#include <glib.h>
#include <glib/gstdio.h>
#include "prerelease.h"

static gchar *make_temp_data_home(void) {
    gchar *tmp = g_dir_make_tmp("ygo-prerelease-clear-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_setenv("XDG_DATA_HOME", tmp, TRUE);
    return tmp;
}

static gchar *get_data_dir(const gchar *data_home) {
    return g_build_filename(data_home, "ygo-deck-builder", "pre-release", NULL);
}

static void
test_clear_removes_all_data(void)
{
    gchar *data_home = make_temp_data_home();
    gchar *data_dir = get_data_dir(data_home);
    gchar *pics_dir = g_build_filename(data_dir, "pics", NULL);
    gchar *json_path = g_build_filename(data_dir, "pre-release.json", NULL);
    gchar *etag_path = g_build_filename(data_dir, "pre-release.etag", NULL);
    gchar *image_path = g_build_filename(pics_dir, "100200298.jpg", NULL);
    GError *error = NULL;

    g_assert_cmpint(g_mkdir_with_parents(pics_dir, 0755), ==, 0);
    g_assert_true(g_file_set_contents(json_path, "[]", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(etag_path, "\"test-etag\"", -1, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(image_path, "image", -1, &error));
    g_assert_no_error(error);

    g_assert_true(clear_prerelease_data());
    g_assert_false(g_file_test(data_dir, G_FILE_TEST_EXISTS));

    g_remove(image_path);
    g_remove(etag_path);
    g_remove(json_path);
    g_rmdir(pics_dir);
    g_rmdir(data_dir);
    g_free(image_path);
    g_free(etag_path);
    g_free(json_path);
    g_free(pics_dir);
    g_free(data_dir);
    g_rmdir(data_home);
    g_free(data_home);
}

static void
test_clear_missing_directory_is_ok(void)
{
    gchar *data_home = make_temp_data_home();
    gchar *data_dir = get_data_dir(data_home);

    g_assert_true(clear_prerelease_data());
    g_assert_false(g_file_test(data_dir, G_FILE_TEST_EXISTS));

    g_rmdir(data_dir);
    g_free(data_dir);
    g_rmdir(data_home);
    g_free(data_home);
}

int
main(int argc, char *argv[])
{
    // g_get_user_data_dir() 只在首次调用时读取环境，测试中先设置再调用
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/prerelease/clear-removes-all-data", test_clear_removes_all_data);
    g_test_add_func("/prerelease/clear-missing-directory", test_clear_missing_directory_is_ok);

    return g_test_run();
}

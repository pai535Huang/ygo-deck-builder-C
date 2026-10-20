#include "prefs.h"
#include "app_path.h"
#include <glib/gstdio.h>

#define CONFIG_FILE "settings.conf"

/**
 * settings.conf 的绝对路径。调用者用 g_free 释放。
 * 便携模式放在程序目录，系统安装模式放在 XDG_CONFIG_HOME/ygo-deck-builder。
 */
static gchar *get_config_path(void) {
    if (is_portable_mode()) {
        return g_build_filename(get_program_directory(), CONFIG_FILE, NULL);
    }

    return g_build_filename(g_get_user_config_dir(), "ygo-deck-builder", CONFIG_FILE, NULL);
}

gboolean prefs_get_boolean(const char *group, const char *key, gboolean default_value) {
    if (!group || !key) {
        return default_value;
    }

    gchar *config_path = get_config_path();
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    gboolean result = default_value;

    if (g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        GError *key_error = NULL;
        gboolean value = g_key_file_get_boolean(keyfile, group, key, &key_error);
        if (!key_error) {
            result = value;
        } else {
            g_error_free(key_error);
        }
    } else if (error) {
        g_error_free(error);
    }

    g_key_file_free(keyfile);
    g_free(config_path);

    return result;
}

gboolean prefs_set_boolean(const char *group, const char *key, gboolean value) {
    if (!group || !key) {
        return FALSE;
    }

    gchar *config_path = get_config_path();
    gchar *config_dir = g_path_get_dirname(config_path);
    gboolean success = FALSE;

    if (g_mkdir_with_parents(config_dir, 0755) != 0) {
        g_warning("无法创建配置目录: %s", config_dir);
    } else {
        GKeyFile *keyfile = g_key_file_new();
        GError *error = NULL;

        // 保留已有的其它组和键；文件不存在或损坏时按空配置处理
        g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, NULL);
        g_key_file_set_boolean(keyfile, group, key, value);

        if (g_key_file_save_to_file(keyfile, config_path, &error)) {
            success = TRUE;
        } else {
            g_warning("无法保存配置: %s", error->message);
            g_error_free(error);
        }

        g_key_file_free(keyfile);
    }

    g_free(config_dir);
    g_free(config_path);

    return success;
}

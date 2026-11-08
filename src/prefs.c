#include "prefs.h"
#include "app_path.h"
#include <glib/gstdio.h>

#define CONFIG_FILE "settings.conf"

static gchar *get_config_path(void) {
    if (is_portable_mode())
        return g_build_filename(get_program_directory(), CONFIG_FILE, NULL);

    return g_build_filename(g_get_user_config_dir(), "ygo-deck-builder", CONFIG_FILE, NULL);
}

static GKeyFile *load_config(const char *path) {
    GKeyFile *keyfile = g_key_file_new();
    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_unref(keyfile);
        return g_key_file_new();
    }
    return keyfile;
}

static gboolean save_config(GKeyFile *keyfile, const char *path) {
    g_autofree char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0755) != 0) {
        g_warning("Could not create configuration directory: %s", directory);
        return FALSE;
    }

    g_autoptr(GError) error = NULL;
    if (!g_key_file_save_to_file(keyfile, path, &error)) {
        g_warning("Could not save configuration: %s", error->message);
        return FALSE;
    }
    return TRUE;
}

gboolean prefs_get_boolean(const char *group, const char *key, gboolean default_value) {
    if (!group || !key)
        return default_value;

    g_autofree char *path = get_config_path();
    g_autoptr(GKeyFile) keyfile = load_config(path);
    g_autoptr(GError) error = NULL;
    gboolean value = g_key_file_get_boolean(keyfile, group, key, &error);
    return error ? default_value : value;
}

gboolean prefs_set_boolean(const char *group, const char *key, gboolean value) {
    if (!group || !key)
        return FALSE;

    g_autofree char *path = get_config_path();
    g_autoptr(GKeyFile) keyfile = load_config(path);
    g_key_file_set_boolean(keyfile, group, key, value);
    return save_config(keyfile, path);
}

char *prefs_get_string(const char *group, const char *key) {
    g_autofree char *path = get_config_path();
    g_autoptr(GKeyFile) keyfile = load_config(path);
    return g_key_file_get_string(keyfile, group, key, NULL);
}

gboolean prefs_set_string(const char *group, const char *key, const char *value) {
    g_autofree char *path = get_config_path();
    g_autoptr(GKeyFile) keyfile = load_config(path);
    g_key_file_set_string(keyfile, group, key, value);
    return save_config(keyfile, path);
}

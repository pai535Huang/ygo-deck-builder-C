#include "deck_io.h"
#include "deck_slot.h"
#include "deck_clear.h"
#include "image_loader.h"
#include "prerelease.h"
#include "offline_data.h"
#include "alt_art.h"
#include "app_path.h"
#include "prefs.h"
#include "card_language.h"
#include "card_info.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define CONFIG_FILE "settings.conf"

// main.c：为槽位异步加载卡图（含先行卡本地异步读、内存/磁盘缓存、网络回退）
extern void load_card_image(GtkWidget *slot, int img_id, SoupSession *session);

static void append_ydk_region(GString *deck, const char *section,
                              GPtrArray *slots, int count) {
    g_string_append(deck, section);
    for (int i = 0; slots && i < count && i < (int)slots->len; i++) {
        int id = slot_get_img_id(g_ptr_array_index(slots, i));
        if (id > 0) g_string_append_printf(deck, "%d\n", id);
    }
}

gboolean export_deck_to_ydk(
    GPtrArray *main_pics, int main_count,
    GPtrArray *extra_pics, int extra_count,
    GPtrArray *side_pics, int side_count,
    const char *filepath, GError **error
) {
    if (!filepath || !*filepath ||
        (g_file_test(filepath, G_FILE_TEST_EXISTS) &&
         !g_file_test(filepath, G_FILE_TEST_IS_REGULAR))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The export destination must be a regular file");
        return FALSE;
    }

    g_autoptr(GString) deck = g_string_new("#created by ygo-deck-builder\n");
    append_ydk_region(deck, "#main\n", main_pics, main_count);
    append_ydk_region(deck, "#extra\n", extra_pics, extra_count);
    append_ydk_region(deck, "!side\n", side_pics, side_count);
    return g_file_set_contents_full(filepath, deck->str, deck->len,
                                    G_FILE_SET_CONTENTS_CONSISTENT |
                                    G_FILE_SET_CONTENTS_DURABLE, 0666, error);
}

gboolean deck_apply_card_metadata(GtkWidget *slot, JsonObject *card) {
    if (!card) return FALSE;
    JsonNode *type_node = json_object_get_member(card, "type");
    if (!type_node) {
        JsonNode *data = json_object_get_member(card, "data");
        if (data && JSON_NODE_HOLDS_OBJECT(data))
            type_node = json_object_get_member(json_node_get_object(data), "type");
    }
    if (!type_node || json_node_get_value_type(type_node) != G_TYPE_INT64) return FALSE;
    guint32 type = json_node_get_int(type_node);
    if (!type) return FALSE;

    JsonNode *cid_node = json_object_get_member(card, "cid");
    if (cid_node && json_node_get_value_type(cid_node) == G_TYPE_INT64) {
        gint64 cid = json_node_get_int(cid_node);
        if (cid > 0 && cid <= G_MAXINT)
            g_object_set_data(G_OBJECT(slot), "card_id", GINT_TO_POINTER((int)cid));
    }
    slot_set_en_name(slot, card_english_name(card));
    slot_set_is_extra(slot, card_type_is_extra(type));
    slot_set_card_details(slot, type, card_deck_sheet_name(card));
    g_object_set_data(G_OBJECT(slot), "needs-card-metadata", NULL);
    return TRUE;
}

gboolean deck_load_local_card_metadata(GtkWidget *slot) {
    int img_id = slot_get_img_id(slot);
    JsonObject *card = find_prerelease_card_by_id(img_id);
    if (!card) {
        int cid = offline_get_cid_by_img_id(img_id);
        if (!cid) cid = offline_get_cid_by_img_id(alt_art_get_base_id(img_id));
        card = get_card_by_id_offline(cid);
    }
    gboolean resolved = deck_apply_card_metadata(slot, card);
    if (card) json_object_unref(card);
    return resolved;
}

void deck_fill_region(GPtrArray *slots, int *count, GtkLabel *label,
                      const int *ids, int length, gboolean extra, SoupSession *session) {
    for (int i = 0; i < length; i++) {
        GtkWidget *slot = g_ptr_array_index(slots, i);
        int img_id = ids[i];
        g_object_set_data(G_OBJECT(slot), "img_id", GINT_TO_POINTER(img_id));
        g_object_set_data(G_OBJECT(slot), "card_id", GINT_TO_POINTER(img_id));
        slot_set_is_extra(slot, extra);

        if (!deck_load_local_card_metadata(slot)) {
            const char *region = g_object_get_data(G_OBJECT(slot), "slot_region");
            int metadata = g_strcmp0(region, "side") == 0
                ? SLOT_METADATA_TYPE_UNKNOWN : SLOT_METADATA_PENDING;
            g_object_set_data(G_OBJECT(slot), "needs-card-metadata", GINT_TO_POINTER(metadata));
        }
        if (session) load_card_image(slot, img_id, session);
    }
    *count = length;
    update_count_label(label, length);
}

gboolean import_deck_from_ydk(
    GPtrArray *main_pics, int *main_idx, GtkLabel *main_count,
    GPtrArray *extra_pics, int *extra_idx, GtkLabel *extra_count,
    GPtrArray *side_pics, int *side_idx, GtkLabel *side_count,
    SoupSession *session,
    const char *filepath
) {
    if (!filepath || !g_file_test(filepath, G_FILE_TEST_IS_REGULAR)) return FALSE;

    FILE *file = g_fopen(filepath, "r");
    if (!file) return FALSE;

    g_autoptr(GArray) main_ids = g_array_new(FALSE, FALSE, sizeof(int));
    g_autoptr(GArray) extra_ids = g_array_new(FALSE, FALSE, sizeof(int));
    g_autoptr(GArray) side_ids = g_array_new(FALSE, FALSE, sizeof(int));
    GArray *section = NULL;
    guint capacity = 0;
    gboolean valid = TRUE;
    gboolean first = TRUE;
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            valid = FALSE;
            break;
        }

        char *entry = line;
        if (first && g_str_has_prefix(entry, "\xef\xbb\xbf")) entry += 3;
        first = FALSE;
        g_strstrip(entry);
        if (g_str_equal(entry, "#main")) {
            section = main_ids;
            capacity = main_pics->len;
        } else if (g_str_equal(entry, "#extra")) {
            section = extra_ids;
            capacity = extra_pics->len;
        } else if (g_str_equal(entry, "!side")) {
            section = side_ids;
            capacity = side_pics->len;
        } else if (*entry && *entry != '#') {
            gint64 id;
            if (!section || section->len >= capacity ||
                !g_ascii_string_to_signed(entry, 10, 1, G_MAXINT, &id, NULL)) {
                valid = FALSE;
                break;
            }
            int card_id = id;
            g_array_append_val(section, card_id);
        }
    }
    valid = valid && section && !ferror(file);
    if (fclose(file) != 0) valid = FALSE;
    if (!valid) return FALSE;

    clear_all_deck_regions(main_pics, main_idx, main_count, extra_pics, extra_idx,
                           extra_count, side_pics, side_idx, side_count);
    deck_fill_region(main_pics, main_idx, main_count, (int *)main_ids->data,
                     main_ids->len, FALSE, session);
    deck_fill_region(extra_pics, extra_idx, extra_count, (int *)extra_ids->data,
                     extra_ids->len, TRUE, session);
    deck_fill_region(side_pics, side_idx, side_count, (int *)side_ids->data,
                     side_ids->len, FALSE, session);
    return TRUE;
}

static void load_directory_config(GKeyFile *keyfile, const char *key, char **out_dir) {
    char *dir = g_key_file_get_string(keyfile, "Directories", key, NULL);
    if (dir && g_file_test(dir, G_FILE_TEST_IS_DIR)) {
        if (out_dir) {
            g_free(*out_dir);
            *out_dir = dir;
        } else {
            g_free(dir);
        }
    } else {
        g_free(dir);
    }
}

// 加载导入导出目录配置
void load_io_config(char **last_export_dir, char **last_import_dir, char **last_deck_sheet_dir) {
    char *config_path;
    
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        config_path = g_build_filename(prog_dir, CONFIG_FILE, NULL);
    } else {
        // 系统安装模式：使用 XDG_CONFIG_HOME
        const char *config_home = g_get_user_config_dir();
        config_path = g_build_filename(config_home, "ygo-deck-builder", CONFIG_FILE, NULL);
        // 确保配置目录存在
        char *config_dir = g_path_get_dirname(config_path);
        g_mkdir_with_parents(config_dir, 0755);
        g_free(config_dir);
    }
    
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    
    if (g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        // 加载导出目录
        load_directory_config(keyfile, "LastExportDirectory", last_export_dir);
        
        // 加载导入目录
        load_directory_config(keyfile, "LastImportDirectory", last_import_dir);

        // 加载生成卡表目录
        load_directory_config(keyfile, "LastDeckSheetDirectory", last_deck_sheet_dir);
    } else if (error) {
        g_error_free(error);
    }
    
    g_key_file_free(keyfile);
    g_free(config_path);
}

// 保存导入导出目录配置
void save_io_config(const char *last_export_dir, const char *last_import_dir, const char *last_deck_sheet_dir) {
    char *config_path;
    
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        config_path = g_build_filename(prog_dir, CONFIG_FILE, NULL);
    } else {
        // 系统安装模式：使用 XDG_CONFIG_HOME
        const char *config_home = g_get_user_config_dir();
        config_path = g_build_filename(config_home, "ygo-deck-builder", CONFIG_FILE, NULL);
        // 确保配置目录存在
        char *config_dir = g_path_get_dirname(config_path);
        g_mkdir_with_parents(config_dir, 0755);
        g_free(config_dir);
    }
    
    GKeyFile *keyfile = g_key_file_new();
    
    // 尝试加载现有配置
    g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, NULL);
    
    // 设置新值
    if (last_export_dir) {
        g_key_file_set_string(keyfile, "Directories", "LastExportDirectory", last_export_dir);
    }
    if (last_import_dir) {
        g_key_file_set_string(keyfile, "Directories", "LastImportDirectory", last_import_dir);
    }
    if (last_deck_sheet_dir) {
        g_key_file_set_string(keyfile, "Directories", "LastDeckSheetDirectory", last_deck_sheet_dir);
    }
    
    // 保存到文件
    GError *error = NULL;
    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        g_warning("无法保存配置: %s", error->message);
        g_error_free(error);
    }
    
    g_key_file_free(keyfile);
    g_free(config_path);
}

// 加载离线数据开关状态
gboolean load_offline_data_switch_state(void) {
    return prefs_get_boolean("Preferences", "OfflineDataEnabled", FALSE);
}

// 保存离线数据开关状态
void save_offline_data_switch_state(gboolean enabled) {
    prefs_set_boolean("Preferences", "OfflineDataEnabled", enabled);
}

// 加载“启用先行卡”状态（开启后每次启动自动检查更新）
gboolean load_prerelease_enabled_state(void) {
    return prefs_get_boolean("Preferences", "PrereleaseEnabled", FALSE);
}

// 保存“启用先行卡”状态
void save_prerelease_enabled_state(gboolean enabled) {
    prefs_set_boolean("Preferences", "PrereleaseEnabled", enabled);
}

// 加载个人信息中的 Konami ID
char* load_personal_konami_id(void) {
    char *config_path;

    if (is_portable_mode()) {
        const char *prog_dir = get_program_directory();
        config_path = g_build_filename(prog_dir, CONFIG_FILE, NULL);
    } else {
        const char *config_home = g_get_user_config_dir();
        config_path = g_build_filename(config_home, "ygo-deck-builder", CONFIG_FILE, NULL);
    }

    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    char *konami_id = NULL;

    if (g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        konami_id = g_key_file_get_string(keyfile, "PersonalInfo", "KonamiId", NULL);
        if (konami_id && !*konami_id) {
            g_clear_pointer(&konami_id, g_free);
        }
    } else if (error) {
        g_error_free(error);
    }

    g_key_file_free(keyfile);
    g_free(config_path);

    return konami_id;
}

// 保存个人信息中的 Konami ID
void save_personal_konami_id(const char *konami_id) {
    char *config_path;

    if (is_portable_mode()) {
        const char *prog_dir = get_program_directory();
        config_path = g_build_filename(prog_dir, CONFIG_FILE, NULL);
    } else {
        const char *config_home = g_get_user_config_dir();
        config_path = g_build_filename(config_home, "ygo-deck-builder", CONFIG_FILE, NULL);
        char *config_dir = g_path_get_dirname(config_path);
        g_mkdir_with_parents(config_dir, 0755);
        g_free(config_dir);
    }

    GKeyFile *keyfile = g_key_file_new();
    g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, NULL);

    if (konami_id && *konami_id) {
        g_key_file_set_string(keyfile, "PersonalInfo", "KonamiId", konami_id);
    } else {
        g_key_file_remove_key(keyfile, "PersonalInfo", "KonamiId", NULL);
    }

    GError *error = NULL;
    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        g_warning("无法保存个人信息配置: %s", error->message);
        g_error_free(error);
    }

    g_key_file_free(keyfile);
    g_free(config_path);
}

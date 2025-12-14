#include "deck_io.h"
#include "deck_slot.h"
#include "deck_clear.h"
#include "image_loader.h"
#include "prerelease.h"
#include "offline_data.h"
#include "app_path.h"
#include <stdio.h>
#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define CONFIG_FILE "settings.conf"

// 导出卡组为YDK文件
void export_deck_to_ydk(
    GPtrArray *main_pics, int main_count,
    GPtrArray *extra_pics, int extra_count,
    GPtrArray *side_pics, int side_count,
    const char *filepath
) {
    if (!filepath) return;
    
    FILE *f = fopen(filepath, "w");
    if (!f) {
        g_warning("无法创建文件: %s", filepath);
        return;
    }
    
    // 写入文件头
    fprintf(f, "#created by ygo-deck-builder\n");
    
    // 写入Main卡组
    fprintf(f, "#main\n");
    if (main_pics) {
        for (int i = 0; i < main_count && i < (int)main_pics->len; i++) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(main_pics, i));
            int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
            if (img_id > 0) {
                fprintf(f, "%d\n", img_id);
            }
        }
    }
    
    // 写入Extra卡组
    fprintf(f, "#extra\n");
    if (extra_pics) {
        for (int i = 0; i < extra_count && i < (int)extra_pics->len; i++) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(extra_pics, i));
            int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
            if (img_id > 0) {
                fprintf(f, "%d\n", img_id);
            }
        }
    }
    
    // 写入Side卡组
    fprintf(f, "!side\n");
    if (side_pics) {
        for (int i = 0; i < side_count && i < (int)side_pics->len; i++) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(side_pics, i));
            int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
            if (img_id > 0) {
                fprintf(f, "%d\n", img_id);
            }
        }
    }
    
    fclose(f);
    g_print("卡组已导出到: %s\n", filepath);
}

// 从YDK文件导入卡组
gboolean import_deck_from_ydk(
    GPtrArray *main_pics, int *main_idx, GtkLabel *main_count,
    GPtrArray *extra_pics, int *extra_idx, GtkLabel *extra_count,
    GPtrArray *side_pics, int *side_idx, GtkLabel *side_count,
    SoupSession *session,
    const char *filepath
) {
    if (!filepath) return FALSE;
    
    FILE *f = fopen(filepath, "r");
    if (!f) {
        g_warning("无法打开文件: %s", filepath);
        return FALSE;
    }
    
    // 清空当前卡组
    clear_all_deck_regions(
        main_pics, main_idx, main_count,
        extra_pics, extra_idx, extra_count,
        side_pics, side_idx, side_count
    );
    
    char line[256];
    enum { SECTION_NONE, SECTION_MAIN, SECTION_EXTRA, SECTION_SIDE } current_section = SECTION_NONE;
    
    while (fgets(line, sizeof(line), f)) {
        // 去除换行符
        line[strcspn(line, "\r\n")] = 0;
        
        // 跳过注释和空行
        if (line[0] == '#' && strstr(line, "#main") == line) {
            current_section = SECTION_MAIN;
            continue;
        } else if (line[0] == '#' && strstr(line, "#extra") == line) {
            current_section = SECTION_EXTRA;
            continue;
        } else if (line[0] == '!' && strstr(line, "!side") == line) {
            current_section = SECTION_SIDE;
            continue;
        } else if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        
        // 解析卡片ID
        int img_id = atoi(line);
        if (img_id <= 0) continue;
        
        // 根据当前section添加到相应区域
        GPtrArray *target_pics = NULL;
        int *target_idx = NULL;
        GtkLabel *target_count_label = NULL;
        gboolean is_extra = FALSE;
        
        switch (current_section) {
            case SECTION_MAIN:
                if (main_idx && *main_idx < (int)main_pics->len) {
                    target_pics = main_pics;
                    target_idx = main_idx;
                    target_count_label = main_count;
                } else if (side_idx && *side_idx < (int)side_pics->len) {
                    // Main满了，放到Side
                    target_pics = side_pics;
                    target_idx = side_idx;
                    target_count_label = side_count;
                }
                break;
            case SECTION_EXTRA:
                if (extra_idx && *extra_idx < (int)extra_pics->len) {
                    target_pics = extra_pics;
                    target_idx = extra_idx;
                    target_count_label = extra_count;
                    is_extra = TRUE;
                } else if (side_idx && *side_idx < (int)side_pics->len) {
                    // Extra满了，放到Side
                    target_pics = side_pics;
                    target_idx = side_idx;
                    target_count_label = side_count;
                    is_extra = TRUE;
                }
                break;
            case SECTION_SIDE:
                if (side_idx && *side_idx < (int)side_pics->len) {
                    target_pics = side_pics;
                    target_idx = side_idx;
                    target_count_label = side_count;
                }
                break;
            default:
                continue;
        }
        
        if (!target_pics || !target_idx) continue;
        
        GtkWidget *target_pic = GTK_WIDGET(g_ptr_array_index(target_pics, *target_idx));
        if (!target_pic || !GTK_IS_WIDGET(target_pic)) continue;
        
        slot_set_is_extra(target_pic, is_extra);
        g_object_set_data(G_OBJECT(target_pic), "img_id", GINT_TO_POINTER(img_id));
        g_object_set_data(G_OBJECT(target_pic), "card_id", GINT_TO_POINTER(img_id));
        
        // 加载卡片图片
        if (img_id > 0 && session) {
            // 检查是否是先行卡
            JsonObject *prerelease_card = find_prerelease_card_by_id(img_id);
            gboolean is_prerelease = (prerelease_card != NULL);
            if (prerelease_card) {
                json_object_unref(prerelease_card);
            }
            
            if (is_prerelease) {
                // 先行卡：从本地加载
                gchar *local_path = get_prerelease_card_image_path(img_id);
                if (local_path && g_file_test(local_path, G_FILE_TEST_EXISTS)) {
                    GError *error = NULL;
                    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(local_path, &error);
                    if (pixbuf) {
                        slot_set_pixbuf(target_pic, pixbuf);
                        g_object_unref(pixbuf);
                    } else {
                        if (error) {
                            g_warning("加载先行卡图片失败: %s", error->message);
                            g_error_free(error);
                        }
                    }
                }
                g_free(local_path);
            } else {
                // 普通卡：从缓存或在线加载（离线模式不影响此逻辑）
                // 先尝试从缓存加载
                GdkPixbuf *cached = get_thumb_from_cache(img_id);
                if (!cached) {
                    cached = load_from_disk_cache(img_id);
                }
                
                if (cached) {
                    // 缓存命中，直接设置
                    slot_set_pixbuf(target_pic, cached);
                    if (get_thumb_from_cache(img_id) == NULL) {
                        // 如果不在内存缓存中，需要unref
                        g_object_unref(cached);
                    }
                } else {
                    // 缓存未命中，异步加载
                    char url[128];
                    g_snprintf(url, sizeof url, "https://cdn.233.momobako.com/ygoimg/jp/%d.webp", img_id);
                    ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
                    ctx->stack = NULL;
                    ctx->target = GTK_WIDGET(target_pic);
                    g_object_add_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
                    ctx->scale_to_thumb = TRUE;
                    ctx->cache_id = img_id;
                    ctx->add_to_thumb_cache = TRUE;
                    ctx->url = g_strdup(url);
                    load_image_async(session, url, ctx);
                }
            }
        }
        
        (*target_idx)++;
        update_count_label(target_count_label, *target_idx);
    }
    
    fclose(f);
    g_print("卡组已导入: %s\n", filepath);
    return TRUE;
}

// 加载导入导出目录配置
void load_io_config(char **last_export_dir, char **last_import_dir) {
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
        char *export_dir = g_key_file_get_string(keyfile, "Directories", "LastExportDirectory", NULL);
        if (export_dir && g_file_test(export_dir, G_FILE_TEST_IS_DIR)) {
            if (last_export_dir) {
                g_free(*last_export_dir);
                *last_export_dir = export_dir;
            } else {
                g_free(export_dir);
            }
        } else {
            g_free(export_dir);
        }
        
        // 加载导入目录
        char *import_dir = g_key_file_get_string(keyfile, "Directories", "LastImportDirectory", NULL);
        if (import_dir && g_file_test(import_dir, G_FILE_TEST_IS_DIR)) {
            if (last_import_dir) {
                g_free(*last_import_dir);
                *last_import_dir = import_dir;
            } else {
                g_free(import_dir);
            }
        } else {
            g_free(import_dir);
        }
    } else if (error) {
        g_error_free(error);
    }
    
    g_key_file_free(keyfile);
    g_free(config_path);
}

// 保存导入导出目录配置
void save_io_config(const char *last_export_dir, const char *last_import_dir) {
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
    char *config_path;
    
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        config_path = g_build_filename(prog_dir, CONFIG_FILE, NULL);
    } else {
        // 系统安装模式
        const char *config_home = g_get_user_config_dir();
        config_path = g_build_filename(config_home, "ygo-deck-builder", CONFIG_FILE, NULL);
    }
    
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    gboolean enabled = FALSE;
    
    if (g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        enabled = g_key_file_get_boolean(keyfile, "Preferences", "OfflineDataEnabled", NULL);
    } else if (error) {
        g_error_free(error);
    }
    
    g_key_file_free(keyfile);
    g_free(config_path);
    
    return enabled;
}

// 保存离线数据开关状态
void save_offline_data_switch_state(gboolean enabled) {
    char *config_path;
    
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        config_path = g_build_filename(prog_dir, CONFIG_FILE, NULL);
    } else {
        // 系统安装模式
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
    g_key_file_set_boolean(keyfile, "Preferences", "OfflineDataEnabled", enabled);
    
    // 保存到文件
    GError *error = NULL;
    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        g_warning("无法保存配置: %s", error->message);
        g_error_free(error);
    }
    
    g_key_file_free(keyfile);
    g_free(config_path);
}

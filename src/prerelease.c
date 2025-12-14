#include "prerelease.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PRERELEASE_URL "https://cdntx.moecube.com/ygopro-super-pre/archive/ygopro-super-pre.ypk"
#define PRERELEASE_JSON_FILENAME "pre-release.json"

/**
 * 获取先行卡数据目录的绝对路径
 */
static gchar *get_prerelease_data_dir(void) {
    const gchar *config_dir = g_get_user_config_dir();
    if (!config_dir) {
        g_warning("Unable to get user config directory");
        return NULL;
    }
    
    return g_build_filename(config_dir, "ygo-deck-builder", "data", "pre-release", NULL);
}

/**
 * 获取先行卡JSON文件路径
 */
static gchar *get_prerelease_json_path(void) {
    gchar *data_dir = get_prerelease_data_dir();
    if (!data_dir) {
        return NULL;
    }
    
    gchar *filepath = g_build_filename(data_dir, PRERELEASE_JSON_FILENAME, NULL);
    g_free(data_dir);
    
    return filepath;
}

/**
 * 确保目录存在
 */
static gboolean ensure_directory_exists(const char *path) {
    if (g_mkdir_with_parents(path, 0755) == -1) {
        g_warning("Failed to create directory: %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * 递归删除目录及其内容
 */
static gboolean remove_directory_recursive(const char *path) {
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        return FALSE;
    }
    
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *full_path = g_build_filename(path, name, NULL);
        
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            // 递归删除子目录
            remove_directory_recursive(full_path);
        } else {
            // 删除文件
            remove(full_path);
        }
        
        g_free(full_path);
    }
    
    g_dir_close(dir);
    
    // 删除空目录
    if (rmdir(path) != 0) {
        g_warning("Failed to remove directory: %s", path);
        return FALSE;
    }
    
    return TRUE;
}

/**
 * 清空先行卡目录
 */
static void clear_prerelease_directory(void) {
    gchar *data_dir = get_prerelease_data_dir();
    if (!data_dir) {
        return;
    }
    
    // 如果目录存在，递归删除
    if (g_file_test(data_dir, G_FILE_TEST_IS_DIR)) {
        g_message("Clearing pre-release directory: %s", data_dir);
        remove_directory_recursive(data_dir);
    }
    
    g_free(data_dir);
}

/**
 * 解压YPK文件（ZIP格式），只保留pic目录和test-release.cdb
 */
static gboolean extract_ypk_file(const char *ypk_path, const char *dest_dir) {
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int r;
    
    a = archive_read_new();
    archive_read_support_format_zip(a);
    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    
    if ((r = archive_read_open_filename(a, ypk_path, 10240))) {
        g_warning("Failed to open archive: %s", archive_error_string(a));
        archive_read_free(a);
        archive_write_free(ext);
        return FALSE;
    }
    
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *current_file = archive_entry_pathname(entry);
        
        // 只处理pics目录下的文件和test-release.cdb
        gboolean should_extract = FALSE;
        gchar *output_path = NULL;
        
        if (g_str_has_prefix(current_file, "pics/")) {
            // 提取pics目录下的文件
            output_path = g_build_filename(dest_dir, current_file, NULL);
            should_extract = TRUE;
        } else if (g_strcmp0(current_file, "test-release.cdb") == 0 ||
                   g_strcmp0(current_file, "./test-release.cdb") == 0) {
            // 提取test-release.cdb
            output_path = g_build_filename(dest_dir, "test-release.cdb", NULL);
            should_extract = TRUE;
        }
        
        if (should_extract && output_path) {
            // 设置输出路径
            archive_entry_set_pathname(entry, output_path);
            
            // 确保父目录存在
            gchar *parent_dir = g_path_get_dirname(output_path);
            ensure_directory_exists(parent_dir);
            g_free(parent_dir);
            
            // 写入文件
            r = archive_write_header(ext, entry);
            if (r == ARCHIVE_OK) {
                const void *buff;
                size_t size;
                la_int64_t offset;
                
                while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                    if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                        g_warning("Error writing data: %s", archive_error_string(ext));
                        break;
                    }
                }
                if (r != ARCHIVE_EOF) {
                    g_warning("Error reading data: %s", archive_error_string(a));
                }
                
                archive_write_finish_entry(ext);
            }
            
            g_free(output_path);
        } else {
            // 跳过不需要的文件
            archive_read_data_skip(a);
        }
    }
    
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    
    return TRUE;
}

/**
 * 从SQLite CDB文件中提取卡片数据并转换为JSON
 */
static JsonNode *parse_cdb_to_json(const char *cdb_path) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;
    
    rc = sqlite3_open(cdb_path, &db);
    if (rc != SQLITE_OK) {
        g_warning("Cannot open database: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_array(builder);
    
    // 查询卡片数据
    const char *sql = "SELECT d.id, d.ot, d.alias, d.setcode, d.type, d.atk, d.def, d.level, "
                      "d.race, d.attribute, t.name, t.desc "
                      "FROM datas d LEFT JOIN texts t ON d.id = t.id";
    
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_warning("Failed to prepare statement: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        g_object_unref(builder);
        return NULL;
    }
    
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json_builder_begin_object(builder);
        
        // id
        int id = sqlite3_column_int(stmt, 0);
        json_builder_set_member_name(builder, "id");
        json_builder_add_int_value(builder, id);
        
        // cid (使用id作为cid)
        json_builder_set_member_name(builder, "cid");
        json_builder_add_int_value(builder, id);
        
        // ot
        json_builder_set_member_name(builder, "ot");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 1));
        
        // alias
        json_builder_set_member_name(builder, "alias");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 2));
        
        // setcode
        json_builder_set_member_name(builder, "setcode");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 3));
        
        // type
        int type = sqlite3_column_int(stmt, 4);
        json_builder_set_member_name(builder, "type");
        json_builder_add_int_value(builder, type);
        
        // atk
        json_builder_set_member_name(builder, "atk");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 5));
        
        // def
        json_builder_set_member_name(builder, "def");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 6));
        
        // level (包含等级/阶级/连接值等)
        json_builder_set_member_name(builder, "level");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 7));
        
        // race
        json_builder_set_member_name(builder, "race");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 8));
        
        // attribute
        json_builder_set_member_name(builder, "attribute");
        json_builder_add_int_value(builder, sqlite3_column_int(stmt, 9));
        
        // text object with name and desc
        json_builder_set_member_name(builder, "text");
        json_builder_begin_object(builder);
        
        const char *name = (const char *)sqlite3_column_text(stmt, 10);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name ? name : "");
        
        const char *desc = (const char *)sqlite3_column_text(stmt, 11);
        json_builder_set_member_name(builder, "desc");
        json_builder_add_string_value(builder, desc ? desc : "");
        
        // 根据类型生成types字段
        GString *types_str = g_string_new("");
        if (type & 0x1) g_string_append(types_str, "怪兽 ");
        if (type & 0x2) g_string_append(types_str, "魔法 ");
        if (type & 0x4) g_string_append(types_str, "陷阱 ");
        if (type & 0x40) g_string_append(types_str, "效果 ");
        if (type & 0x800) g_string_append(types_str, "特殊召唤 ");
        
        json_builder_set_member_name(builder, "types");
        json_builder_add_string_value(builder, types_str->str);
        g_string_free(types_str, TRUE);
        
        json_builder_end_object(builder); // end text
        
        json_builder_end_object(builder); // end card
        count++;
    }
    
    json_builder_end_array(builder);
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    
    g_message("Parsed %d pre-release cards from CDB", count);
    
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    return root;
}

/**
 * 后台线程执行的下载和处理函数
 */
typedef struct {
    GSourceFunc callback;
    gpointer user_data;
    gboolean success;
} DownloadContext;

static gpointer download_prerelease_thread(gpointer data) {
    DownloadContext *ctx = (DownloadContext *)data;
    
    // 首先清空先行卡目录，强制重新下载
    clear_prerelease_directory();
    
    gchar *data_dir = get_prerelease_data_dir();
    if (!data_dir) {
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    ensure_directory_exists(data_dir);
    
    // 下载YPK文件
    gchar *ypk_path = g_build_filename(data_dir, "ygopro-super-pre.ypk", NULL);
    
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", PRERELEASE_URL);
    
    g_message("Downloading pre-release cards from %s", PRERELEASE_URL);
    
    GError *error = NULL;
    GInputStream *input = soup_session_send(session, msg, NULL, &error);
    
    if (error) {
        g_warning("Failed to download pre-release cards: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        g_free(ypk_path);
        g_free(data_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 保存到文件
    GFile *file = g_file_new_for_path(ypk_path);
    GOutputStream *output = (GOutputStream *)g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error);
    
    if (error) {
        g_warning("Failed to create output file: %s", error->message);
        g_error_free(error);
        g_object_unref(input);
        g_object_unref(file);
        g_object_unref(session);
        g_free(ypk_path);
        g_free(data_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 复制数据
    g_output_stream_splice(output, input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, &error);
    
    if (error) {
        g_warning("Failed to save file: %s", error->message);
        g_error_free(error);
        g_object_unref(input);
        g_object_unref(output);
        g_object_unref(file);
        g_object_unref(session);
        g_free(ypk_path);
        g_free(data_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    g_object_unref(input);
    g_object_unref(output);
    g_object_unref(file);
    g_object_unref(session);
    
    g_message("Pre-release cards downloaded successfully");
    
    // 解压文件
    g_message("Extracting YPK archive...");
    if (!extract_ypk_file(ypk_path, data_dir)) {
        g_warning("Failed to extract YPK file");
        g_free(ypk_path);
        g_free(data_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    g_message("YPK archive extracted successfully");
    
    // 删除YPK文件
    remove(ypk_path);
    g_free(ypk_path);
    
    // 解析CDB文件
    gchar *cdb_path = g_build_filename(data_dir, "test-release.cdb", NULL);
    JsonNode *json_root = parse_cdb_to_json(cdb_path);
    g_free(cdb_path);
    
    if (!json_root) {
        g_warning("Failed to parse CDB file");
        g_free(data_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 保存为JSON文件
    gchar *json_path = get_prerelease_json_path();
    if (json_path) {
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, json_root);
        json_generator_set_pretty(gen, TRUE);
        
        GError *json_error = NULL;
        if (!json_generator_to_file(gen, json_path, &json_error)) {
            g_warning("Failed to save JSON file: %s", json_error->message);
            g_error_free(json_error);
            ctx->success = FALSE;
        } else {
            g_message("Pre-release JSON saved to %s", json_path);
            ctx->success = TRUE;
        }
        
        g_object_unref(gen);
        g_free(json_path);
    }
    
    json_node_free(json_root);
    g_free(data_dir);
    
    // 回调通知完成
    if (ctx->callback) {
        g_idle_add(ctx->callback, ctx->user_data);
    }
    
    g_free(ctx);
    return NULL;
}

void download_prerelease_cards(GSourceFunc callback, gpointer user_data) {
    DownloadContext *ctx = g_new0(DownloadContext, 1);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->success = FALSE;
    
    GThread *thread = g_thread_new("prerelease-download", download_prerelease_thread, ctx);
    g_thread_unref(thread);
}

JsonArray* search_prerelease_cards(const char *search_query) {
    if (!search_query || *search_query == '\0') {
        return NULL;
    }
    
    gchar *json_path = get_prerelease_json_path();
    if (!json_path || !g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_free(json_path);
        return NULL;
    }
    
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        g_warning("Failed to load pre-release JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return NULL;
    }
    
    g_free(json_path);
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return NULL;
    }
    
    JsonArray *all_cards = json_node_get_array(root);
    JsonArray *results = json_array_new();
    
    // 转换搜索词为小写以进行不区分大小写的搜索
    gchar *query_lower = g_utf8_strdown(search_query, -1);
    
    guint len = json_array_get_length(all_cards);
    for (guint i = 0; i < len; i++) {
        JsonObject *card = json_array_get_object_element(all_cards, i);
        if (!card) continue;
        
        gboolean match = FALSE;
        
        // 检查ID匹配
        if (g_str_has_prefix(query_lower, "id:") || g_ascii_isdigit(search_query[0])) {
            const char *id_str = strchr(search_query, ':');
            if (id_str) id_str++; // 跳过冒号
            else id_str = search_query;
            
            int query_id = atoi(id_str);
            int card_id = json_object_get_int_member(card, "id");
            
            if (card_id == query_id) {
                match = TRUE;
            }
        }
        
        // 检查名称和描述匹配
        if (!match && json_object_has_member(card, "text")) {
            JsonObject *text = json_object_get_object_member(card, "text");
            
            if (json_object_has_member(text, "name")) {
                const char *name = json_object_get_string_member(text, "name");
                gchar *name_lower = g_utf8_strdown(name, -1);
                if (strstr(name_lower, query_lower)) {
                    match = TRUE;
                }
                g_free(name_lower);
            }
            
            if (!match && json_object_has_member(text, "desc")) {
                const char *desc = json_object_get_string_member(text, "desc");
                gchar *desc_lower = g_utf8_strdown(desc, -1);
                if (strstr(desc_lower, query_lower)) {
                    match = TRUE;
                }
                g_free(desc_lower);
            }
        }
        
        if (match) {
            json_array_add_object_element(results, json_object_ref(card));
        }
    }
    
    g_free(query_lower);
    g_object_unref(parser);
    
    return results;
}

JsonArray* get_all_prerelease_cards(void) {
    gchar *json_path = get_prerelease_json_path();
    if (!json_path || !g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_free(json_path);
        return NULL;
    }
    
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        g_warning("Failed to load pre-release JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return NULL;
    }
    
    g_free(json_path);
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return NULL;
    }
    
    JsonArray *all_cards = json_node_get_array(root);
    JsonArray *results = json_array_new();
    
    // 复制所有卡片到结果数组
    guint len = json_array_get_length(all_cards);
    for (guint i = 0; i < len; i++) {
        JsonObject *card = json_array_get_object_element(all_cards, i);
        if (card) {
            json_array_add_object_element(results, json_object_ref(card));
        }
    }
    
    g_object_unref(parser);
    
    return results;
}

JsonObject* find_prerelease_card_by_id(int card_id) {
    gchar *json_path = get_prerelease_json_path();
    if (!json_path || !g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_free(json_path);
        return NULL;
    }
    
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        g_warning("Failed to load pre-release JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return NULL;
    }
    
    g_free(json_path);
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return NULL;
    }
    
    JsonArray *all_cards = json_node_get_array(root);
    JsonObject *found_card = NULL;
    
    guint len = json_array_get_length(all_cards);
    for (guint i = 0; i < len; i++) {
        JsonObject *card = json_array_get_object_element(all_cards, i);
        if (!card) continue;
        
        if (json_object_has_member(card, "id")) {
            int id = json_object_get_int_member(card, "id");
            if (id == card_id) {
                found_card = json_object_ref(card);
                break;
            }
        }
    }
    
    g_object_unref(parser);
    
    return found_card;
}

gchar* get_prerelease_card_image_path(int card_id) {
    gchar *data_dir = get_prerelease_data_dir();
    if (!data_dir) {
        return NULL;
    }
    
    gchar *image_path = g_build_filename(data_dir, "pics", g_strdup_printf("%d.jpg", card_id), NULL);
    g_free(data_dir);
    
    // 检查文件是否存在
    if (!g_file_test(image_path, G_FILE_TEST_EXISTS)) {
        g_free(image_path);
        return NULL;
    }
    
    return image_path;
}

gboolean prerelease_data_exists(void) {
    gchar *json_path = get_prerelease_json_path();
    if (!json_path) {
        return FALSE;
    }
    
    gboolean exists = g_file_test(json_path, G_FILE_TEST_EXISTS);
    g_free(json_path);
    
    return exists;
}

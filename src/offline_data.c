#include "offline_data.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#define OFFLINE_DATA_URL "https://ygocdb.com/api/v0/cards.zip"
#define OFFLINE_DATA_MD5_URL "https://ygocdb.com/api/v0/cards.zip.md5"
#define CARDS_DIR_NAME "cards"

/**
 * 获取离线数据目录的绝对路径 (~/.config/ygo-deck-builder/data)
 */
static gchar *get_offline_data_dir(void) {
    const gchar *config_dir = g_get_user_config_dir();
    if (!config_dir) {
        g_warning("Unable to get user config directory");
        return NULL;
    }
    
    return g_build_filename(config_dir, "ygo-deck-builder", "data", NULL);
}

/**
 * 获取 cards 目录的绝对路径 (~/.config/ygo-deck-builder/data/cards)
 */
static gchar *get_cards_dir(void) {
    const gchar *config_dir = g_get_user_config_dir();
    if (!config_dir) {
        g_warning("Unable to get user config directory");
        return NULL;
    }
    
    return g_build_filename(config_dir, "ygo-deck-builder", "data", CARDS_DIR_NAME, NULL);
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
 * 递归删除目录内容（保留目录本身）
 */
static gboolean clear_directory_contents(const char *path) {
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        return FALSE;
    }
    
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *full_path = g_build_filename(path, name, NULL);
        
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            // 递归删除子目录
            clear_directory_contents(full_path);
            rmdir(full_path);
        } else {
            // 删除文件
            remove(full_path);
        }
        
        g_free(full_path);
    }
    
    g_dir_close(dir);
    return TRUE;
}

/**
 * 解压ZIP文件到指定目录
 */
static gboolean extract_zip_file(const char *zip_path, const char *dest_dir) {
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int r;
    
    a = archive_read_new();
    archive_read_support_format_zip(a);
    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
    
    if ((r = archive_read_open_filename(a, zip_path, 10240))) {
        g_warning("Failed to open archive: %s", archive_error_string(a));
        archive_read_free(a);
        archive_write_free(ext);
        return FALSE;
    }
    
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *current_file = archive_entry_pathname(entry);
        
        // 构建输出路径
        gchar *output_path = g_build_filename(dest_dir, current_file, NULL);
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
    }
    
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    
    return TRUE;
}

/**
 * 下载上下文结构
 */
typedef struct {
    GSourceFunc callback;
    gpointer user_data;
    gboolean success;
} DownloadContext;

/**
 * 后台线程执行下载和处理
 */
static gpointer download_offline_data_thread(gpointer data) {
    DownloadContext *ctx = (DownloadContext*)data;
    
    // 获取数据目录和 cards 目录
    gchar *data_dir = get_offline_data_dir();
    gchar *cards_dir = get_cards_dir();
    
    if (!data_dir || !cards_dir) {
        g_warning("Failed to get directories");
        g_free(data_dir);
        g_free(cards_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 确保数据目录存在
    if (!ensure_directory_exists(data_dir)) {
        g_warning("Failed to create data directory");
        g_free(data_dir);
        g_free(cards_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 构建 ZIP 文件路径
    gchar *zip_path = g_build_filename(data_dir, "cards.zip", NULL);
    
    // 下载文件
    g_message("Downloading offline data from %s...", OFFLINE_DATA_URL);
    
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", OFFLINE_DATA_URL);
    GError *error = NULL;
    
    GInputStream *input = soup_session_send(session, msg, NULL, &error);
    
    if (error) {
        g_warning("Failed to download offline data: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        g_free(zip_path);
        g_free(data_dir);
        g_free(cards_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 保存到文件
    GFile *file = g_file_new_for_path(zip_path);
    GOutputStream *output = (GOutputStream *)g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error);
    
    if (error) {
        g_warning("Failed to create output file: %s", error->message);
        g_error_free(error);
        g_object_unref(input);
        g_object_unref(file);
        g_object_unref(session);
        g_free(zip_path);
        g_free(data_dir);
        g_free(cards_dir);
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
        g_free(zip_path);
        g_free(data_dir);
        g_free(cards_dir);
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
    
    g_message("Offline data downloaded successfully");
    
    // 确保 cards 目录存在，如果存在则清空
    if (g_file_test(cards_dir, G_FILE_TEST_IS_DIR)) {
        g_message("Clearing existing cards directory: %s", cards_dir);
        clear_directory_contents(cards_dir);
    } else {
        g_message("Creating cards directory: %s", cards_dir);
        if (!ensure_directory_exists(cards_dir)) {
            g_warning("Failed to create cards directory");
            remove(zip_path);
            g_free(zip_path);
            g_free(data_dir);
            g_free(cards_dir);
            ctx->success = FALSE;
            if (ctx->callback) {
                g_idle_add(ctx->callback, ctx->user_data);
            }
            g_free(ctx);
            return NULL;
        }
    }
    
    // 解压文件到 cards 目录
    g_message("Extracting ZIP archive to %s...", cards_dir);
    if (!extract_zip_file(zip_path, cards_dir)) {
        g_warning("Failed to extract ZIP file");
        remove(zip_path);
        g_free(zip_path);
        g_free(data_dir);
        g_free(cards_dir);
        ctx->success = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    g_message("ZIP archive extracted successfully");
    
    // 下载 MD5 文件
    g_message("Downloading MD5 checksum from %s...", OFFLINE_DATA_MD5_URL);
    
    SoupSession *md5_session = soup_session_new();
    SoupMessage *md5_msg = soup_message_new("GET", OFFLINE_DATA_MD5_URL);
    GError *md5_error = NULL;
    
    GInputStream *md5_input = soup_session_send(md5_session, md5_msg, NULL, &md5_error);
    
    if (md5_error) {
        g_warning("Failed to download MD5 file: %s", md5_error->message);
        g_error_free(md5_error);
        g_object_unref(md5_session);
        // MD5 下载失败不影响主流程，继续执行
    } else {
        // 保存 MD5 文件到 cards 目录
        gchar *md5_path = g_build_filename(cards_dir, "cards.zip.md5", NULL);
        GFile *md5_file = g_file_new_for_path(md5_path);
        GOutputStream *md5_output = (GOutputStream *)g_file_replace(md5_file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &md5_error);
        
        if (md5_error) {
            g_warning("Failed to create MD5 output file: %s", md5_error->message);
            g_error_free(md5_error);
            g_object_unref(md5_input);
            g_object_unref(md5_file);
            g_object_unref(md5_session);
            g_free(md5_path);
            // MD5 保存失败不影响主流程
        } else {
            // 复制数据
            g_output_stream_splice(md5_output, md5_input, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, &md5_error);
            
            if (md5_error) {
                g_warning("Failed to save MD5 file: %s", md5_error->message);
                g_error_free(md5_error);
                // MD5 保存失败不影响主流程
            } else {
                g_message("MD5 checksum saved to %s", md5_path);
            }
            
            g_object_unref(md5_input);
            g_object_unref(md5_output);
            g_object_unref(md5_file);
        }
        
        g_object_unref(md5_session);
        g_free(md5_path);
    }
    
    // 删除 ZIP 文件
    g_message("Removing ZIP file: %s", zip_path);
    remove(zip_path);
    
    g_free(zip_path);
    g_free(data_dir);
    g_free(cards_dir);
    
    ctx->success = TRUE;
    if (ctx->callback) {
        g_idle_add(ctx->callback, ctx->user_data);
    }
    g_free(ctx);
    
    return NULL;
}

/**
 * 下载并处理离线卡片数据（公共接口）
 */
void download_offline_data(GSourceFunc callback, gpointer user_data) {
    DownloadContext *ctx = g_new(DownloadContext, 1);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->success = FALSE;
    
    // 在新线程中执行下载
    GThread *thread = g_thread_new("download-offline-data", download_offline_data_thread, ctx);
    g_thread_unref(thread);
}

/**
 * 检查离线数据是否存在
 */
gboolean offline_data_exists(void) {
    gchar *cards_dir = get_cards_dir();
    if (!cards_dir) {
        return FALSE;
    }
    
    gboolean exists = g_file_test(cards_dir, G_FILE_TEST_IS_DIR);
    g_free(cards_dir);
    
    return exists;
}

/**
 * 清理离线数据（删除 cards 目录下的所有文件）
 */
gboolean clear_offline_data(void) {
    gchar *cards_dir = get_cards_dir();
    if (!cards_dir) {
        g_warning("Failed to get cards directory");
        return FALSE;
    }
    
    // 检查目录是否存在
    if (!g_file_test(cards_dir, G_FILE_TEST_IS_DIR)) {
        g_free(cards_dir);
        return TRUE; // 目录不存在，视为已清理
    }
    
    g_message("Clearing offline data from: %s", cards_dir);
    
    // 清空目录内容
    gboolean success = clear_directory_contents(cards_dir);
    
    if (success) {
        // 删除空目录
        if (rmdir(cards_dir) == 0) {
            g_message("Offline data cleared successfully");
        } else {
            g_warning("Failed to remove cards directory");
            success = FALSE;
        }
    } else {
        g_warning("Failed to clear offline data");
    }
    
    g_free(cards_dir);
    return success;
}

/**
 * 读取本地 MD5 文件内容
 */
static gchar *read_local_md5(void) {
    gchar *cards_dir = get_cards_dir();
    if (!cards_dir) {
        return NULL;
    }
    
    gchar *md5_path = g_build_filename(cards_dir, "cards.zip.md5", NULL);
    g_free(cards_dir);
    
    if (!g_file_test(md5_path, G_FILE_TEST_EXISTS)) {
        g_free(md5_path);
        return NULL;
    }
    
    gchar *content = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(md5_path, &content, NULL, &error)) {
        if (error) {
            g_warning("Failed to read local MD5: %s", error->message);
            g_error_free(error);
        }
        g_free(md5_path);
        return NULL;
    }
    
    g_free(md5_path);
    
    // 去除前后空白字符
    if (content) {
        g_strstrip(content);
    }
    
    return content;
}

/**
 * 下载远程 MD5 内容
 */
static gchar *download_remote_md5(void) {
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", OFFLINE_DATA_MD5_URL);
    GError *error = NULL;
    
    GInputStream *input = soup_session_send(session, msg, NULL, &error);
    
    if (error) {
        g_warning("Failed to download remote MD5: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return NULL;
    }
    
    // 读取内容
    GString *content = g_string_new(NULL);
    gchar buffer[1024];
    gssize bytes_read;
    
    while ((bytes_read = g_input_stream_read(input, buffer, sizeof(buffer), NULL, &error)) > 0) {
        g_string_append_len(content, buffer, bytes_read);
    }
    
    if (error) {
        g_warning("Failed to read remote MD5: %s", error->message);
        g_error_free(error);
        g_object_unref(input);
        g_object_unref(session);
        g_string_free(content, TRUE);
        return NULL;
    }
    
    g_object_unref(input);
    g_object_unref(session);
    
    gchar *result = g_string_free(content, FALSE);
    
    // 去除前后空白字符
    if (result) {
        g_strstrip(result);
    }
    
    return result;
}

/**
 * 检查更新的上下文结构
 */
typedef struct {
    GSourceFunc callback;
    gpointer user_data;
    gboolean needs_update;
} CheckUpdateContext;

/**
 * 后台线程执行检查和更新
 */
static gpointer check_offline_data_update_thread(gpointer data) {
    CheckUpdateContext *ctx = (CheckUpdateContext*)data;
    
    g_message("Checking offline data for updates...");
    
    // 检查本地数据是否存在
    if (!offline_data_exists()) {
        g_message("Offline data does not exist, no update needed");
        ctx->needs_update = FALSE;
        if (ctx->callback) {
            g_idle_add(ctx->callback, ctx->user_data);
        }
        g_free(ctx);
        return NULL;
    }
    
    // 读取本地 MD5
    gchar *local_md5 = read_local_md5();
    if (!local_md5) {
        g_message("Local MD5 not found, will update");
        ctx->needs_update = TRUE;
    } else {
        // 下载远程 MD5
        gchar *remote_md5 = download_remote_md5();
        if (!remote_md5) {
            g_warning("Failed to download remote MD5, skipping update");
            g_free(local_md5);
            ctx->needs_update = FALSE;
            if (ctx->callback) {
                g_idle_add(ctx->callback, ctx->user_data);
            }
            g_free(ctx);
            return NULL;
        }
        
        // 比较 MD5
        if (g_strcmp0(local_md5, remote_md5) == 0) {
            g_message("Offline data is up to date");
            ctx->needs_update = FALSE;
            g_free(local_md5);
            g_free(remote_md5);
            if (ctx->callback) {
                g_idle_add(ctx->callback, ctx->user_data);
            }
            g_free(ctx);
            return NULL;
        }
        
        g_message("Offline data needs update (local: %s, remote: %s)", local_md5, remote_md5);
        g_free(local_md5);
        g_free(remote_md5);
        ctx->needs_update = TRUE;
    }
    
    // 需要更新，先清理旧数据
    if (ctx->needs_update) {
        g_message("Updating offline data...");
        clear_offline_data();
        
        // 创建下载上下文
        DownloadContext *download_ctx = g_new(DownloadContext, 1);
        download_ctx->callback = ctx->callback;
        download_ctx->user_data = ctx->user_data;
        download_ctx->success = FALSE;
        
        // 直接在当前线程执行下载
        g_free(ctx);
        return download_offline_data_thread(download_ctx);
    }
    
    g_free(ctx);
    return NULL;
}

/**
 * 检查离线数据更新（公共接口）
 */
void check_offline_data_update(GSourceFunc callback, gpointer user_data) {
    CheckUpdateContext *ctx = g_new(CheckUpdateContext, 1);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->needs_update = FALSE;
    
    // 在新线程中执行检查
    GThread *thread = g_thread_new("check-offline-data-update", check_offline_data_update_thread, ctx);
    g_thread_unref(thread);
}

/**
 * 从离线数据中搜索卡片
 */
JsonArray* search_offline_cards(const char *query) {
    if (!query || strlen(query) == 0) {
        return NULL;
    }
    
    // 获取 cards.json 文件路径
    gchar *cards_dir = get_cards_dir();
    if (!cards_dir) {
        return NULL;
    }
    
    gchar *json_path = g_build_filename(cards_dir, "cards.json", NULL);
    g_free(cards_dir);
    
    if (!g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_warning("cards.json not found");
        g_free(json_path);
        return NULL;
    }
    
    // 加载 JSON 文件
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        g_warning("Failed to parse cards.json: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return NULL;
    }
    
    g_free(json_path);
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("Invalid JSON structure in cards.json");
        g_object_unref(parser);
        return NULL;
    }
    
    JsonObject *root_obj = json_node_get_object(root);
    
    // 创建结果数组
    JsonArray *results = json_array_new();
    
    // 转换查询字符串为小写以进行不区分大小写的搜索
    gchar *query_lower = g_utf8_strdown(query, -1);
    
    // 遍历所有卡片
    GList *members = json_object_get_members(root_obj);
    for (GList *l = members; l != NULL; l = l->next) {
        const gchar *card_id = (const gchar*)l->data;
        JsonNode *card_node = json_object_get_member(root_obj, card_id);
        
        if (!card_node || !JSON_NODE_HOLDS_OBJECT(card_node)) {
            continue;
        }
        
        JsonObject *card = json_node_get_object(card_node);
        gboolean match = FALSE;
        
        // 在卡名中搜索（支持多种语言的卡名）
        const gchar *name_fields[] = {"cn_name", "sc_name", "md_name", "jp_name", "en_name", "nwbbs_n", "cnocg_n", NULL};
        for (int i = 0; name_fields[i] != NULL && !match; i++) {
            if (json_object_has_member(card, name_fields[i])) {
                const gchar *name = json_object_get_string_member(card, name_fields[i]);
                if (name) {
                    gchar *name_lower = g_utf8_strdown(name, -1);
                    if (strstr(name_lower, query_lower) != NULL) {
                        match = TRUE;
                    }
                    g_free(name_lower);
                }
            }
        }
        
        // 如果卡名不匹配，在效果描述中搜索
        if (!match && json_object_has_member(card, "text")) {
            JsonObject *text = json_object_get_object_member(card, "text");
            if (text && json_object_has_member(text, "desc")) {
                const gchar *desc = json_object_get_string_member(text, "desc");
                if (desc) {
                    gchar *desc_lower = g_utf8_strdown(desc, -1);
                    if (strstr(desc_lower, query_lower) != NULL) {
                        match = TRUE;
                    }
                    g_free(desc_lower);
                }
            }
        }
        
        // 如果匹配，添加到结果中
        if (match) {
            // 复制整个卡片对象到结果数组
            JsonNode *card_copy = json_node_copy(card_node);
            json_array_add_element(results, card_copy);
        }
    }
    
    g_list_free(members);
    g_free(query_lower);
    g_object_unref(parser);
    
    // 如果没有结果，返回 NULL
    if (json_array_get_length(results) == 0) {
        json_array_unref(results);
        return NULL;
    }
    
    return results;
}

/**
 * 从离线数据中根据卡片ID获取单张卡片信息
 */
JsonObject* get_card_by_id_offline(int card_id) {
    if (card_id <= 0) {
        return NULL;
    }
    
    // 获取 cards.json 文件路径
    gchar *cards_dir = get_cards_dir();
    if (!cards_dir) {
        return NULL;
    }
    
    gchar *json_path = g_build_filename(cards_dir, "cards.json", NULL);
    g_free(cards_dir);
    
    if (!g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_free(json_path);
        return NULL;
    }
    
    // 加载 JSON 文件
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        if (error) g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return NULL;
    }
    
    g_free(json_path);
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }
    
    JsonObject *root_obj = json_node_get_object(root);
    
    // 根据ID查找卡片
    char card_id_str[32];
    g_snprintf(card_id_str, sizeof(card_id_str), "%d", card_id);
    
    JsonObject *result = NULL;
    if (json_object_has_member(root_obj, card_id_str)) {
        JsonNode *card_node = json_object_get_member(root_obj, card_id_str);
        if (card_node && JSON_NODE_HOLDS_OBJECT(card_node)) {
            // 复制卡片对象（调用者需要 unref）
            result = json_object_ref(json_node_get_object(card_node));
        }
    }
    
    g_object_unref(parser);
    return result;
}

/**
 * 获取所有离线卡片数据
 */
JsonArray* get_all_offline_cards(void) {
    // 获取 cards.json 文件路径
    gchar *cards_dir = get_cards_dir();
    if (!cards_dir) {
        return NULL;
    }
    
    gchar *json_path = g_build_filename(cards_dir, "cards.json", NULL);
    g_free(cards_dir);
    
    if (!g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_warning("cards.json not found");
        g_free(json_path);
        return NULL;
    }
    
    // 加载 JSON 文件
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        g_warning("Failed to parse cards.json: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return NULL;
    }
    
    g_free(json_path);
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("Invalid JSON structure in cards.json");
        g_object_unref(parser);
        return NULL;
    }
    
    JsonObject *root_obj = json_node_get_object(root);
    
    // 创建结果数组
    JsonArray *results = json_array_new();
    
    // 遍历所有卡片并添加到结果数组
    GList *members = json_object_get_members(root_obj);
    for (GList *l = members; l != NULL; l = l->next) {
        const gchar *card_id = (const gchar*)l->data;
        JsonNode *card_node = json_object_get_member(root_obj, card_id);
        
        if (card_node && JSON_NODE_HOLDS_OBJECT(card_node)) {
            // 复制整个卡片对象到结果数组
            JsonNode *card_copy = json_node_copy(card_node);
            json_array_add_element(results, card_copy);
        }
    }
    
    g_list_free(members);
    g_object_unref(parser);
    
    // 如果没有结果，返回 NULL
    if (json_array_get_length(results) == 0) {
        json_array_unref(results);
        return NULL;
    }
    
    return results;
}

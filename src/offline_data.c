#define _POSIX_C_SOURCE 200809L

#include "offline_data.h"
#include "alt_art.h"
#include "app_path.h"
#include "data_directory.h"
#include <errno.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>

typedef struct {
    JsonParser *parser;
    JsonObject *root;
    GStatBuf file;
    GHashTable *img_to_cid;
    GHashTable *name_to_cid;
    GHashTable *effective_cid;
} OfflineCardCache;

static GMutex offline_cache_mutex;
static GMutex offline_load_mutex;
static OfflineCardCache *offline_cache;
static guint offline_cache_revision;

static gchar *get_cards_dir(void);

static gchar *get_cards_json_path(void) {
    g_autofree char *cards_dir = get_cards_dir();
    return g_build_filename(cards_dir, "cards.json", NULL);
}

static void offline_card_cache_free(OfflineCardCache *cache) {
    if (!cache) return;
    g_object_unref(cache->parser);
    g_hash_table_unref(cache->img_to_cid);
    g_hash_table_unref(cache->name_to_cid);
    g_hash_table_unref(cache->effective_cid);
    g_free(cache);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(OfflineCardCache, offline_card_cache_free)

static OfflineCardCache *offline_cache_replace_locked(OfflineCardCache *replacement) {
    OfflineCardCache *old = offline_cache;
    offline_cache = replacement;
    offline_cache_revision++;
    return old;
}

void offline_data_clear_cache(void) {
    g_mutex_lock(&offline_cache_mutex);
    OfflineCardCache *old = offline_cache_replace_locked(NULL);
    g_mutex_unlock(&offline_cache_mutex);
    offline_card_cache_free(old);
}

static gboolean same_card_file(const GStatBuf *first, const GStatBuf *second) {
    return first->st_dev == second->st_dev && first->st_ino == second->st_ino &&
           first->st_size == second->st_size && first->st_mtim.tv_sec == second->st_mtim.tv_sec &&
           first->st_mtim.tv_nsec == second->st_mtim.tv_nsec;
}

static OfflineCardCache *offline_load_cache(const char *path, GError **error) {
    GStatBuf file;
    if (g_stat(path, &file) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot read %s: %s", path, g_strerror(errno));
        return NULL;
    }
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) return NULL;

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Offline card database must contain a JSON object");
        return NULL;
    }

    OfflineCardCache *cache = g_new0(OfflineCardCache, 1);
    cache->root = json_node_get_object(root);
    cache->parser = g_steal_pointer(&parser);
    cache->file = file;
    cache->img_to_cid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    cache->name_to_cid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    cache->effective_cid = g_hash_table_new(g_direct_hash, g_direct_equal);

    const char *name_fields[] = {
        "cn_name", "sc_name", "md_name", "jp_name", "en_name", "nwbbs_n", "cnocg_n"
    };
    JsonObjectIter iter;
    json_object_iter_init(&iter, cache->root);
    const char *key;
    JsonNode *node;
    while (json_object_iter_next(&iter, &key, &node)) {
        gint64 cid;
        if (!node || !JSON_NODE_HOLDS_OBJECT(node) ||
            !g_ascii_string_to_signed(key, 10, 1, G_MAXINT, &cid, NULL))
            continue;

        gpointer cid_value = GINT_TO_POINTER((int)cid);
        JsonObject *card = json_node_get_object(node);
        JsonNode *id_node = json_object_get_member(card, "id");
        if (id_node && json_node_get_value_type(id_node) == G_TYPE_INT64) {
            gint64 id = json_node_get_int(id_node);
            if (id > 0)
                g_hash_table_insert(cache->img_to_cid, g_strdup_printf("%" G_GINT64_FORMAT, id), cid_value);
        }

        for (guint i = 0; i < G_N_ELEMENTS(name_fields); i++) {
            JsonNode *name_node = json_object_get_member(card, name_fields[i]);
            if (!name_node || json_node_get_value_type(name_node) != G_TYPE_STRING) continue;
            g_autofree char *name = g_strdup(json_node_get_string(name_node));
            if (name && *g_strstrip(name) && !g_hash_table_contains(cache->name_to_cid, name))
                g_hash_table_insert(cache->name_to_cid, g_steal_pointer(&name), cid_value);
        }
    }
    if (g_hash_table_size(cache->img_to_cid) == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Offline card database contains no cards");
        offline_card_cache_free(cache);
        return NULL;
    }
    return cache;
}

gboolean offline_data_is_ready(void) {
    g_mutex_lock(&offline_cache_mutex);
    gboolean ready = offline_cache != NULL;
    g_mutex_unlock(&offline_cache_mutex);
    return ready;
}

static gboolean offline_cache_ensure_loaded_locked(void) {
    // UI consumers use the preloaded snapshot. Workers parse replacements without
    // holding the lookup mutex, then publish the complete snapshot together.
    if (g_main_context_is_owner(g_main_context_default()))
        return offline_cache != NULL;

    g_autofree char *path = get_cards_json_path();
    GStatBuf file;
    if (g_stat(path, &file) != 0) return FALSE;
    if (offline_cache && same_card_file(&offline_cache->file, &file)) return TRUE;

    g_mutex_unlock(&offline_cache_mutex);
    g_mutex_lock(&offline_load_mutex);
    g_mutex_lock(&offline_cache_mutex);
    if (g_stat(path, &file) == 0 &&
        (!offline_cache || !same_card_file(&offline_cache->file, &file))) {
        guint revision = offline_cache_revision;
        g_mutex_unlock(&offline_cache_mutex);

        g_autoptr(GError) error = NULL;
        g_autoptr(OfflineCardCache) loaded = offline_load_cache(path, &error);
        g_autoptr(OfflineCardCache) old = NULL;
        if (error) g_warning("Failed to parse cards.json: %s", error->message);
        g_mutex_lock(&offline_cache_mutex);
        if (loaded && revision == offline_cache_revision &&
            g_stat(path, &file) == 0 && same_card_file(&loaded->file, &file))
            old = offline_cache_replace_locked(g_steal_pointer(&loaded));
        g_mutex_unlock(&offline_cache_mutex);
        g_clear_pointer(&old, offline_card_cache_free);
        g_clear_pointer(&loaded, offline_card_cache_free);
        g_mutex_lock(&offline_cache_mutex);
    }
    g_mutex_unlock(&offline_load_mutex);
    return offline_cache != NULL;
}

static gboolean card_matches_query(JsonObject *card, const gchar *query_lower) {
    if (!card || !query_lower || query_lower[0] == '\0') return TRUE;

    gboolean match = FALSE;
    const gchar *name_fields[] = {"cn_name", "sc_name", "md_name", "jp_name", "en_name", "nwbbs_n", "cnocg_n", NULL};
    for (int i = 0; name_fields[i] != NULL && !match; i++) {
        if (json_object_has_member(card, name_fields[i])) {
            const gchar *name = json_object_get_string_member(card, name_fields[i]);
            if (name) {
                gchar *name_lower = g_utf8_strdown(name, -1);
                if (name_lower && strstr(name_lower, query_lower) != NULL) {
                    match = TRUE;
                }
                g_free(name_lower);
            }
        }
    }

    if (!match && json_object_has_member(card, "text")) {
        JsonObject *text = json_object_get_object_member(card, "text");
        if (text && json_object_has_member(text, "desc")) {
            const gchar *desc = json_object_get_string_member(text, "desc");
            if (desc) {
                gchar *desc_lower = g_utf8_strdown(desc, -1);
                if (desc_lower && strstr(desc_lower, query_lower) != NULL) {
                    match = TRUE;
                }
                g_free(desc_lower);
            }
        }
    }

    return match;
}

guint offline_foreach_card(const char *query,
                           gboolean search_all,
                           OfflineCardMatchFunc match_cb,
                           gpointer user_data,
                           guint max_results) {
    // max_results==0 表示不限制，但这里为了 UI 不卡死，通常会传入 500。
    if (!offline_data_exists()) return 0;

    gchar *query_lower = NULL;
    if (!search_all && query && query[0] != '\0') {
        query_lower = g_utf8_strdown(query, -1);
    }

    guint accepted = 0;

    JsonParser *parser_ref = NULL;
    JsonObject *root_obj = NULL;
    g_mutex_lock(&offline_cache_mutex);
    gboolean ok = offline_cache_ensure_loaded_locked();
    if (ok && offline_cache->parser && offline_cache->root) {
        parser_ref = g_object_ref(offline_cache->parser);
        root_obj = offline_cache->root;
    }
    g_mutex_unlock(&offline_cache_mutex);

    if (!ok || !parser_ref || !root_obj) {
        if (parser_ref) g_object_unref(parser_ref);
        g_free(query_lower);
        return 0;
    }

    JsonObjectIter iter;
    json_object_iter_init(&iter, root_obj);
    const gchar *member_name = NULL;
    JsonNode *card_node = NULL;

    while (json_object_iter_next(&iter, &member_name, &card_node)) {
        (void)member_name;
        if (!card_node || !JSON_NODE_HOLDS_OBJECT(card_node)) continue;
        JsonObject *card = json_node_get_object(card_node);
        if (!card) continue;

        if (!search_all && query_lower && !card_matches_query(card, query_lower)) {
            continue;
        }

        gboolean accept = TRUE;
        if (match_cb) {
            accept = match_cb(card, user_data);
        }
        if (accept) {
            accepted++;
            if (max_results > 0 && accepted >= max_results) break;
        }
    }

    g_free(query_lower);
    g_object_unref(parser_ref);
    return accepted;
}

static void offline_warm_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
    (void)source;
    (void)data;
    (void)cancellable;
    g_mutex_lock(&offline_cache_mutex);
    gboolean loaded = offline_cache_ensure_loaded_locked();
    g_mutex_unlock(&offline_cache_mutex);
    g_task_return_boolean(task, loaded);
}

void offline_data_warm_cache_async(GObject *owner, GAsyncReadyCallback callback) {
    g_autoptr(GTask) task = g_task_new(owner, NULL, callback, NULL);
    g_task_run_in_thread(task, offline_warm_thread);
}

#define OFFLINE_DATA_URL "https://ygocdb.com/api/v0/cards.zip"
#define OFFLINE_DATA_MD5_URL "https://ygocdb.com/api/v0/cards.zip.md5"
#define STRINGS_CONF_URL "https://raw.githubusercontent.com/Fluorohydride/ygopro/master/strings.conf"
#define CARDS_DIR_NAME "cards"

/**
 * 获取离线数据目录的绝对路径
 */
static gchar *get_offline_data_dir(void) {
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        return g_build_filename(prog_dir, "data", NULL);
    } else {
        // 系统安装模式：使用 XDG_DATA_HOME
        const char *data_home = g_get_user_data_dir();
        return g_build_filename(data_home, "ygo-deck-builder", NULL);
    }
}

/**
 * 获取 cards 目录的绝对路径
 */
static gchar *get_cards_dir(void) {
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        return g_build_filename(prog_dir, "data", CARDS_DIR_NAME, NULL);
    } else {
        // 系统安装模式：使用 XDG_DATA_HOME
        const char *data_home = g_get_user_data_dir();
        return g_build_filename(data_home, "ygo-deck-builder", CARDS_DIR_NAME, NULL);
    }
}

gchar *offline_get_cards_dir_copy(void) {
    return get_cards_dir();
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

static GMutex offline_install_mutex;
static GMutex offline_download_mutex;
static guint offline_install_generation;

typedef struct {
    OfflineDownloadCallback callback;
    gpointer user_data;
    guint generation;
    gboolean installed;
} DownloadContext;

static gboolean notify_offline_download(gpointer data) {
    DownloadContext *ctx = data;
    ctx->callback(ctx->installed, ctx->user_data);
    return G_SOURCE_REMOVE;
}

static guint get_install_generation(void) {
    g_mutex_lock(&offline_install_mutex);
    guint generation = offline_install_generation;
    g_mutex_unlock(&offline_install_mutex);
    return generation;
}

static gboolean download_file(SoupSession *session, const char *url,
                              const char *path, GError **error) {
    g_autoptr(SoupMessage) message = soup_message_new("GET", url);
    g_autoptr(GInputStream) input = soup_session_send(session, message, NULL, error);
    if (!input) return FALSE;
    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
                    soup_message_get_status(message), soup_message_get_reason_phrase(message));
        return FALSE;
    }

    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileOutputStream) output =
        g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
    return output &&
        g_output_stream_splice(G_OUTPUT_STREAM(output), input,
            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
            NULL, error) >= 0;
}

static void download_optional_file(SoupSession *session, const char *url,
                                   const char *stage, const char *filename) {
    g_autofree char *path = g_build_filename(stage, filename, NULL);
    g_autoptr(GError) error = NULL;
    if (!download_file(session, url, path, &error)) {
        g_warning("Failed to download %s: %s", filename, error ? error->message : "empty response");
        g_remove(path);
    }
}

static gpointer download_offline_data_thread(gpointer data) {
    g_autofree DownloadContext *ctx = data;
    g_autofree char *data_dir = get_offline_data_dir();
    g_autofree char *cards_dir = get_cards_dir();
    g_autofree char *stage = NULL;
    g_autofree char *backup = NULL;
    g_autoptr(GError) error = NULL;
    gboolean installed = FALSE;

    g_autofree char *zip_path = NULL;
    g_autofree char *replacement = NULL;
    g_autofree char *json_path = NULL;
    g_autofree char *strings_path = NULL;
    g_autoptr(SoupSession) session = NULL;
    g_autoptr(OfflineCardCache) prepared = NULL;
    g_autoptr(OfflineCardCache) old = NULL;

    g_mutex_lock(&offline_download_mutex);
    if (ctx->generation != get_install_generation()) goto done;
    if (!data_dir || !cards_dir || !ensure_directory_exists(data_dir)) goto done;

    stage = g_build_filename(data_dir, ".cards-update-XXXXXX", NULL);
    if (!g_mkdtemp(stage)) {
        g_warning("Failed to create offline update directory: %s", g_strerror(errno));
        g_clear_pointer(&stage, g_free);
        goto done;
    }

    zip_path = g_build_filename(stage, "cards.zip", NULL);
    replacement = g_build_filename(stage, "cards", NULL);
    backup = g_build_filename(stage, "previous", NULL);
    json_path = g_build_filename(replacement, "cards.json", NULL);
    if (!ensure_directory_exists(replacement)) goto done;

    session = soup_session_new();
    soup_session_set_timeout(session, 60);
    if (!download_file(session, OFFLINE_DATA_URL, zip_path, &error) ||
        !data_directory_extract_zip(zip_path, replacement, NULL, &error))
        goto done;

    prepared = offline_load_cache(json_path, &error);
    if (!prepared) goto done;
    download_optional_file(session, OFFLINE_DATA_MD5_URL, replacement, "cards.zip.md5");
    download_optional_file(session, STRINGS_CONF_URL, replacement, "strings.conf");
    strings_path = g_build_filename(replacement, "strings.conf", NULL);
    if (!g_file_test(strings_path, G_FILE_TEST_IS_REGULAR)) {
        g_autofree char *old_strings = g_build_filename(cards_dir, "strings.conf", NULL);
        g_autoptr(GFile) source = g_file_new_for_path(old_strings);
        g_autoptr(GFile) target = g_file_new_for_path(strings_path);
        g_file_copy(source, target, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
    }

    g_mutex_lock(&offline_install_mutex);
    if (ctx->generation == offline_install_generation) {
        g_mutex_lock(&offline_cache_mutex);
        installed = data_directory_replace(cards_dir, replacement, backup, &error);
        if (installed)
            old = offline_cache_replace_locked(g_steal_pointer(&prepared));
        g_mutex_unlock(&offline_cache_mutex);
    }
    g_mutex_unlock(&offline_install_mutex);
    if (installed) alt_art_rebuild_cache_async();

done:
    if (error) g_warning("Failed to update offline data: %s", error->message);
    if (stage && (installed || !backup ||
                  !g_file_test(backup, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK)))
        data_directory_remove(stage);
    g_mutex_unlock(&offline_download_mutex);
    if (ctx->callback) {
        ctx->installed = installed;
        g_idle_add_full(G_PRIORITY_DEFAULT, notify_offline_download,
                        g_steal_pointer(&ctx), g_free);
    }
    return NULL;
}

/**
 * 下载并处理离线卡片数据（公共接口）
 */
void download_offline_data(OfflineDownloadCallback callback, gpointer user_data) {
    DownloadContext *ctx = g_new0(DownloadContext, 1);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->generation = get_install_generation();
    
    // 在新线程中执行下载
    GThread *thread = g_thread_new("download-offline-data", download_offline_data_thread, ctx);
    g_thread_unref(thread);
}

/**
 * 检查离线数据是否存在
 */
gboolean offline_data_exists(void) {
    gchar *json_path = get_cards_json_path();
    if (!json_path) {
        return FALSE;
    }

    gboolean exists = g_file_test(json_path, G_FILE_TEST_IS_REGULAR);
    g_free(json_path);
    
    return exists;
}

/**
 * 清理离线数据（删除 cards 目录下的所有文件）
 */
gboolean clear_offline_data(void) {
    g_autofree char *cards_dir = get_cards_dir();
    g_mutex_lock(&offline_install_mutex);
    offline_install_generation++;
    gboolean removed = data_directory_remove(cards_dir);
    offline_data_clear_cache();
    g_mutex_unlock(&offline_install_mutex);
    return removed;
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
    // 错误页不可当作远程 MD5 内容使用
    if (!error && !SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(msg))) {
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
                            soup_message_get_status(msg), soup_message_get_reason_phrase(msg));
    }
    g_object_unref(msg);  // 释放调用方引用
    
    if (error) {
        g_warning("Failed to download remote MD5: %s", error->message);
        g_error_free(error);
        if (input) g_object_unref(input);  // HTTP 失败时 send 仍会返回非空流
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
static gpointer check_offline_data_update_thread(gpointer data) {
    guint generation = GPOINTER_TO_UINT(data);
    if (!offline_data_exists() || generation != get_install_generation()) return NULL;

    g_autofree char *local_md5 = offline_data_is_ready() ? read_local_md5() : NULL;
    if (local_md5) {
        g_autofree char *remote_md5 = download_remote_md5();
        if (!remote_md5 || g_str_equal(local_md5, remote_md5)) return NULL;
    }

    DownloadContext *download = g_new0(DownloadContext, 1);
    download->generation = generation;
    return download_offline_data_thread(download);
}

void check_offline_data_update(void) {
    GThread *thread = g_thread_new("check-offline-data-update", check_offline_data_update_thread,
                                   GUINT_TO_POINTER(get_install_generation()));
    g_thread_unref(thread);
}

/**
 * 从离线数据中根据卡片ID获取单张卡片信息（复用共享缓存，不重复解析）
 */
JsonObject* get_card_by_id_offline(int card_id) {
    if (card_id <= 0) return NULL;

    g_mutex_lock(&offline_cache_mutex);
    if (!offline_cache_ensure_loaded_locked()) {
        g_mutex_unlock(&offline_cache_mutex);
        return NULL;
    }

    char card_id_str[32];
    g_snprintf(card_id_str, sizeof(card_id_str), "%d", card_id);

    JsonObject *result = NULL;
    if (json_object_has_member(offline_cache->root, card_id_str)) {
        JsonNode *card_node = json_object_get_member(offline_cache->root, card_id_str);
        if (card_node && JSON_NODE_HOLDS_OBJECT(card_node)) {
            result = json_object_ref(json_node_get_object(card_node));
        }
    }
    g_mutex_unlock(&offline_cache_mutex);
    return result;
}

/**
 * 通过卡片ID获取英文卡名（使用共享缓存）
 * @return 新分配的字符串，由调用者用 g_free 释放；找不到返回 NULL
 */
gchar* offline_get_en_name_by_id(int card_id) {
    if (card_id <= 0) return NULL;

    g_mutex_lock(&offline_cache_mutex);
    if (!offline_cache_ensure_loaded_locked()) {
        g_mutex_unlock(&offline_cache_mutex);
        return NULL;
    }

    char id_str[32];
    g_snprintf(id_str, sizeof(id_str), "%d", card_id);

    gchar *result = NULL;
    if (json_object_has_member(offline_cache->root, id_str)) {
        JsonNode *card_node = json_object_get_member(offline_cache->root, id_str);
        if (card_node && JSON_NODE_HOLDS_OBJECT(card_node)) {
            JsonObject *card_obj = json_node_get_object(card_node);
            if (json_object_has_member(card_obj, "en_name")) {
                const char *en = json_object_get_string_member(card_obj, "en_name");
                if (en && *en) result = g_strdup(en);
            }
        }
    }
    g_mutex_unlock(&offline_cache_mutex);
    return result;
}

/**
 * 通过 Konami img_id（八位码）查找 ygocdb 短 cid
 * @return cid（短 ID），找不到返回 0
 */
int offline_get_cid_by_img_id(int img_id) {
    if (img_id <= 0) return 0;

    g_mutex_lock(&offline_cache_mutex);
    if (!offline_cache_ensure_loaded_locked()) {
        g_mutex_unlock(&offline_cache_mutex);
        return 0;
    }

    char img_str[32];
    g_snprintf(img_str, sizeof(img_str), "%d", img_id);
    int cid = 0;
    if (offline_cache->img_to_cid) {
        gpointer val = g_hash_table_lookup(offline_cache->img_to_cid, img_str);
        if (val) cid = GPOINTER_TO_INT(val);
    }
    g_mutex_unlock(&offline_cache_mutex);
    return cid;
}

static JsonObject *offline_get_card_object_by_cid_locked(int cid) {
    if (!offline_cache->root || cid <= 0) return NULL;

    char cid_str[32];
    g_snprintf(cid_str, sizeof(cid_str), "%d", cid);
    JsonNode *card_node = json_object_get_member(offline_cache->root, cid_str);
    if (!card_node || !JSON_NODE_HOLDS_OBJECT(card_node)) return NULL;
    return json_node_get_object(card_node);
}

static int offline_lookup_cid_by_img_id_locked(gint64 img_id) {
    if (!offline_cache->img_to_cid || img_id <= 0) return 0;

    char img_str[32];
    g_snprintf(img_str, sizeof(img_str), "%" G_GINT64_FORMAT, img_id);
    gpointer val = g_hash_table_lookup(offline_cache->img_to_cid, img_str);
    return val ? GPOINTER_TO_INT(val) : 0;
}

static int offline_lookup_cid_by_name_locked(const char *name) {
    if (!offline_cache->name_to_cid || !name || !*name) return 0;

    gchar *stripped = g_strdup(name);
    if (!stripped) return 0;
    g_strstrip(stripped);
    int cid = 0;
    if (*stripped) {
        gpointer val = g_hash_table_lookup(offline_cache->name_to_cid, stripped);
        if (val) cid = GPOINTER_TO_INT(val);
    }
    g_free(stripped);
    return cid;
}

static int offline_alias_to_cid_locked(gint64 alias_img_id) {
    if (alias_img_id <= 0) return 0;

    int alias_cid = offline_lookup_cid_by_img_id_locked(alias_img_id);
    if (alias_cid > 0) return alias_cid;

    if (alias_img_id <= G_MAXINT) {
        JsonObject *by_cid = offline_get_card_object_by_cid_locked((int)alias_img_id);
        if (by_cid) return (int)alias_img_id;
    }

    return 0;
}

static const char *offline_segment_start_for_marker(const char *desc, const char *marker) {
    const char *start = desc;
    const char *p = desc;
    while (p && *p && p < marker) {
        if (*p == '\r' || *p == '\n') {
            start = p + 1;
            p++;
            continue;
        }
        if (g_str_has_prefix(p, "。")) {
            start = p + strlen("。");
            p += strlen("。");
            continue;
        }
        p = g_utf8_next_char(p);
    }
    return start;
}

static const char *offline_segment_end_for_marker(const char *marker) {
    const char *end = NULL;
    const char *line_end = strpbrk(marker, "\r\n");
    const char *period = strstr(marker, "。");
    if (line_end && period) {
        end = (line_end < period) ? line_end : period;
    } else {
        end = line_end ? line_end : period;
    }
    return end;
}

static gchar *offline_extract_alias_name_from_segment(const char *segment) {
    if (!segment || !*segment) return NULL;

    gchar *candidate = g_strdup(segment);
    if (!candidate) return NULL;
    g_strstrip(candidate);
    if (!*candidate) {
        g_free(candidate);
        return NULL;
    }

    gboolean name_rule = (strstr(candidate, "这个卡名") != NULL ||
                          strstr(candidate, "这张卡名") != NULL ||
                          strstr(candidate, "这张卡的卡名") != NULL);
    gboolean treated_as = strstr(candidate, "当作") != NULL;
    gboolean scoped_rule = (strstr(candidate, "只要") != NULL ||
                            strstr(candidate, "直到") != NULL ||
                            strstr(candidate, "结束阶段") != NULL);

    if (!name_rule || !treated_as || scoped_rule) {
        g_free(candidate);
        return NULL;
    }

    char *open = strstr(candidate, "「");
    char *close = open ? strstr(open + strlen("「"), "」") : NULL;
    if (!open || !close || close <= open) {
        g_free(candidate);
        return NULL;
    }

    const char *after_close = close + strlen("」");
    if (g_str_has_prefix(after_close, "卡使用") || strstr(after_close, "」")) {
        g_free(candidate);
        return NULL;
    }
    if (!strstr(after_close, "使用")) {
        g_free(candidate);
        return NULL;
    }

    gchar *alias_name = g_strndup(open + strlen("「"), (gsize)(close - (open + strlen("「"))));
    g_free(candidate);
    if (!alias_name) return NULL;

    g_strstrip(alias_name);
    if (!*alias_name) {
        g_free(alias_name);
        return NULL;
    }
    return alias_name;
}

static gchar *offline_extract_rules_alias_name_from_desc(const char *desc) {
    if (!desc || !*desc) return NULL;

    const char *scan = desc;
    for (int i = 0; i < 8; i++) {
        const char *marker = strstr(scan, "当作");
        if (!marker) break;

        const char *start = offline_segment_start_for_marker(desc, marker);
        const char *end = offline_segment_end_for_marker(marker);
        gsize len = end ? (gsize)(end - start) : strlen(start);
        if (len > 0 && len <= 512) {
            gchar *segment = g_strndup(start, len);
            gchar *alias_name = offline_extract_alias_name_from_segment(segment);
            g_free(segment);
            if (alias_name) return alias_name;
        }

        scan = marker + strlen("当作");
    }

    return NULL;
}

static int offline_get_text_alias_cid_locked(JsonObject *card) {
    if (!card || !json_object_has_member(card, "text")) return 0;

    JsonNode *text_node = json_object_get_member(card, "text");
    if (!text_node || !JSON_NODE_HOLDS_OBJECT(text_node)) return 0;

    JsonObject *text = json_node_get_object(text_node);
    if (!text || !json_object_has_member(text, "desc")) return 0;

    const char *desc = json_object_get_string_member(text, "desc");
    gchar *alias_name = offline_extract_rules_alias_name_from_desc(desc);
    if (!alias_name) return 0;

    int alias_cid = offline_lookup_cid_by_name_locked(alias_name);
    g_free(alias_name);
    return alias_cid;
}

static int offline_resolve_effective_card_id_locked(int card_id, int depth) {
    if (card_id <= 0) return 0;
    if (depth > 8) return card_id;

    if (offline_cache->effective_cid) {
        gpointer cached = g_hash_table_lookup(offline_cache->effective_cid, GINT_TO_POINTER(card_id));
        if (cached) return GPOINTER_TO_INT(cached);
    }

    int source_cid = card_id;
    JsonObject *card = offline_get_card_object_by_cid_locked(source_cid);
    if (!card) {
        int mapped_cid = offline_lookup_cid_by_img_id_locked(card_id);
        if (mapped_cid > 0) {
            source_cid = mapped_cid;
            card = offline_get_card_object_by_cid_locked(source_cid);
        }
    }

    int effective_cid = source_cid;
    if (card) {
        gint64 alias_img_id = 0;
        if (json_object_has_member(card, "alias")) {
            alias_img_id = json_object_get_int_member(card, "alias");
        }

        if (alias_img_id > 0) {
            int alias_cid = offline_alias_to_cid_locked(alias_img_id);
            if (alias_cid > 0 && alias_cid != source_cid) {
                effective_cid = offline_resolve_effective_card_id_locked(alias_cid, depth + 1);
            }
        } else if (!json_object_has_member(card, "alias")) {
            int alias_cid = offline_get_text_alias_cid_locked(card);
            if (alias_cid > 0 && alias_cid != source_cid) {
                effective_cid = offline_resolve_effective_card_id_locked(alias_cid, depth + 1);
            }
        }
    }

    if (offline_cache->effective_cid && effective_cid > 0) {
        g_hash_table_insert(offline_cache->effective_cid,
                            GINT_TO_POINTER(card_id),
                            GINT_TO_POINTER(effective_cid));
        if (source_cid != card_id) {
            g_hash_table_insert(offline_cache->effective_cid,
                                GINT_TO_POINTER(source_cid),
                                GINT_TO_POINTER(effective_cid));
        }
    }

    return effective_cid > 0 ? effective_cid : card_id;
}

int offline_get_effective_card_id(int card_id) {
    if (card_id <= 0) return card_id;

    g_mutex_lock(&offline_cache_mutex);
    if (!offline_cache_ensure_loaded_locked()) {
        g_mutex_unlock(&offline_cache_mutex);
        return card_id;
    }

    int effective_id = offline_resolve_effective_card_id_locked(card_id, 0);
    g_mutex_unlock(&offline_cache_mutex);
    return effective_id > 0 ? effective_id : card_id;
}

/**
 * 获取所有离线卡片数据
 * 遍历带 mtime 缓存的共享解析根对象，返回卡片对象的引用数组（浅引用）。
 * 此前每次调用都重新解析 cards.json 并逐卡 json_node_copy 深拷贝，
 * 禁限变更视图每次打开都会产生一次全量解析 + 上万次深拷贝。
 */
JsonArray* get_all_offline_cards(void) {
    JsonParser *parser_ref = NULL;
    JsonObject *root_obj = NULL;
    g_mutex_lock(&offline_cache_mutex);
    gboolean ok = offline_cache_ensure_loaded_locked();
    if (ok && offline_cache->parser && offline_cache->root) {
        // 遍历期间必须持有解析器引用：并发的 reload/clear 会释放共享根对象，
        // 否则下面无锁遍历 root_obj 会读到已释放内存
        parser_ref = g_object_ref(offline_cache->parser);
        root_obj = offline_cache->root;
    }
    g_mutex_unlock(&offline_cache_mutex);

    if (!parser_ref || !root_obj) {
        if (parser_ref) g_object_unref(parser_ref);
        return NULL;
    }

    JsonArray *results = json_array_new();

    GList *members = json_object_get_members(root_obj);
    for (GList *l = members; l != NULL; l = l->next) {
        JsonNode *card_node = json_object_get_member(root_obj, (const gchar*)l->data);
        if (card_node && JSON_NODE_HOLDS_OBJECT(card_node)) {
            JsonObject *card = json_node_get_object(card_node);
            if (card) {
                // add_object_element 接管调用者传入的引用（transfer full）
                json_array_add_object_element(results, json_object_ref(card));
            }
        }
    }
    g_list_free(members);
    g_object_unref(parser_ref);

    // 如果没有结果，返回 NULL
    if (json_array_get_length(results) == 0) {
        json_array_unref(results);
        return NULL;
    }

    return results;
}

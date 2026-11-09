#include "prerelease.h"
#include "app_path.h"
#include "data_directory.h"
#include <errno.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PRERELEASE_URL "https://cdntx.moecube.com/ygopro-super-pre/archive/ygopro-super-pre.ypk"
#define PRERELEASE_JSON_FILENAME "pre-release.json"
#define PRERELEASE_ETAG_FILENAME "pre-release.etag"
#define PRERELEASE_CDB_FILENAME "test-release.cdb"

// 先行卡 JSON 解析共享缓存：
// find_prerelease_card_by_id 会被逐卡调用（如排序 60 张卡会调用 60 次），
// 每次都重新解析整个 JSON 文件的开销极大；参照 offline_data.c 的模式缓存解析结果，
// 以文件 mtime 判断是否需要重载（先行卡下载更新后自动失效）。
static GMutex prerelease_cache_mutex;
static JsonParser *prerelease_parser = NULL;
static JsonArray *prerelease_root_array = NULL; // owned by parser->root
static gchar *prerelease_json_path = NULL;
static gint64 prerelease_json_mtime = 0;

static gchar *get_prerelease_json_path(void);

static gint64 prerelease_stat_mtime(const char *path) {
    if (!path) return 0;
    GStatBuf st;
    if (g_stat(path, &st) != 0) return 0;
    return (gint64)st.st_mtime * 1000000;
}

// 旧JSON缓存只保存了setcode的低32位。用同一份CDB恢复其余系列编号。
static void restore_prerelease_setcodes(JsonArray *cards, const char *json_path) {
    g_autofree gchar *directory = g_path_get_dirname(json_path);
    g_autofree gchar *cdb_path = g_build_filename(directory, PRERELEASE_CDB_FILENAME, NULL);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(cdb_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, setcode FROM datas WHERE setcode < 0 OR setcode > 2147483647";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 id = sqlite3_column_int64(stmt, 0);
            for (guint i = 0; i < json_array_get_length(cards); i++) {
                JsonObject *card = json_array_get_object_element(cards, i);
                if (card && json_object_get_int_member(card, "id") == id) {
                    json_object_set_int_member(card, "setcode", sqlite3_column_int64(stmt, 1));
                    break;
                }
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// 必须持有 prerelease_cache_mutex 调用
static gboolean prerelease_cache_ensure_loaded_locked(void) {
    gchar *json_path = get_prerelease_json_path();
    if (!json_path || !g_file_test(json_path, G_FILE_TEST_EXISTS)) {
        g_free(json_path);
        return FALSE;
    }

    gint64 mtime = prerelease_stat_mtime(json_path);
    gboolean need_reload = (prerelease_parser == NULL) ||
                           (prerelease_root_array == NULL) ||
                           (prerelease_json_path == NULL) ||
                           (g_strcmp0(prerelease_json_path, json_path) != 0) ||
                           (prerelease_json_mtime != mtime);
    if (!need_reload) {
        g_free(json_path);
        return TRUE;
    }

    if (prerelease_parser) {
        g_object_unref(prerelease_parser);
        prerelease_parser = NULL;
    }
    prerelease_root_array = NULL;
    g_clear_pointer(&prerelease_json_path, g_free);
    prerelease_json_mtime = 0;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, json_path, &error)) {
        g_warning("Failed to load pre-release JSON: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        g_object_unref(parser);
        g_free(json_path);
        return FALSE;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_warning("Invalid JSON structure in pre-release JSON");
        g_object_unref(parser);
        g_free(json_path);
        return FALSE;
    }

    prerelease_parser = parser;
    prerelease_root_array = json_node_get_array(root);
    restore_prerelease_setcodes(prerelease_root_array, json_path);
    prerelease_json_path = json_path; // take ownership
    prerelease_json_mtime = mtime;
    return TRUE;
}

/**
 * 获取先行卡数据目录的绝对路径
 */
static gchar *get_prerelease_data_dir(void) {
    if (is_portable_mode()) {
        // 便携模式
        const char *prog_dir = get_program_directory();
        return g_build_filename(prog_dir, "data", "pre-release", NULL);
    } else {
        // 系统安装模式：使用 XDG_DATA_HOME
        const char *data_home = g_get_user_data_dir();
        return g_build_filename(data_home, "ygo-deck-builder", "pre-release", NULL);
    }
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

static GMutex prerelease_install_mutex;
static GMutex prerelease_update_mutex;
static guint prerelease_install_generation;

gboolean clear_prerelease_data(void) {
    g_autofree char *dir = get_prerelease_data_dir();
    g_mutex_lock(&prerelease_install_mutex);
    prerelease_install_generation++;
    gboolean removed = data_directory_remove(dir);
    g_mutex_unlock(&prerelease_install_mutex);
    return removed;
}

static gboolean prerelease_archive_entry(const char *name) {
    if (g_str_has_prefix(name, "./")) name += 2;
    return g_str_has_prefix(name, "pics/") ||
           g_str_equal(name, PRERELEASE_CDB_FILENAME);
}

/**
 * 从SQLite CDB文件中提取卡片数据并转换为JSON
 */
static JsonNode *parse_cdb_to_json(const char *cdb_path) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;
    
    rc = sqlite3_open_v2(cdb_path, &db, SQLITE_OPEN_READONLY, NULL);
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
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
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
        json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 3));
        
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
    
    if (rc != SQLITE_DONE || count == 0) {
        g_object_unref(builder);
        return NULL;
    }

    g_message("Parsed %d pre-release cards from CDB", count);
    
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    return root;
}

/**
 * 后台线程执行的更新和处理函数
 */
typedef struct {
    PrereleaseUpdateCallback callback;
    gpointer user_data;
    guint generation;
} UpdateContext;

typedef struct {
    PrereleaseUpdateCallback callback;
    gpointer user_data;
    PrereleaseUpdateResult result;
} UpdateDone;

static gboolean update_done_idle(gpointer data) {
    UpdateDone *done = (UpdateDone *)data;

    if (done->callback) {
        done->callback(done->result, done->user_data);
    }

    g_free(done);
    return G_SOURCE_REMOVE;
}

static void update_finish(UpdateContext *ctx, PrereleaseUpdateResult result) {
    if (ctx->callback) {
        UpdateDone *done = g_new0(UpdateDone, 1);
        done->callback = ctx->callback;
        done->user_data = ctx->user_data;
        done->result = result;
        g_idle_add(update_done_idle, done);
    }

    g_free(ctx);
}

/**
 * 记录上次成功更新时上游 YPK 的 ETag。
 * 下次启动时带上它发条件请求：上游未变化则返回 304，无需下载。
 */
static gchar *get_prerelease_etag_path(void) {
    gchar *data_dir = get_prerelease_data_dir();
    if (!data_dir) {
        return NULL;
    }

    gchar *path = g_build_filename(data_dir, PRERELEASE_ETAG_FILENAME, NULL);
    g_free(data_dir);
    return path;
}

static gchar *read_prerelease_etag(void) {
    gchar *path = get_prerelease_etag_path();
    if (!path) {
        return NULL;
    }

    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        g_free(path);
        return NULL;
    }

    g_free(path);
    g_strstrip(contents);
    if (!*contents) {
        g_free(contents);
        return NULL;
    }

    return contents;
}

static gpointer update_prerelease_thread(gpointer data) {
    UpdateContext *ctx = (UpdateContext *)data;
    PrereleaseUpdateResult result = PRERELEASE_UPDATE_FAILED;
    gchar *data_dir = get_prerelease_data_dir();
    gchar *local_etag = NULL;
    gchar *etag_to_store = NULL;
    gchar *ypk_path = NULL;
    SoupSession *session = NULL;
    SoupMessage *msg = NULL;
    GInputStream *input = NULL;
    GOutputStream *output = NULL;
    GFile *file = NULL;
    GError *error = NULL;
    g_autofree char *stage = NULL;
    g_autofree char *replacement = NULL;
    g_autofree char *backup = NULL;
    g_autofree char *parent = NULL;
    g_autofree char *cdb_path = NULL;
    g_autofree char *json_path = NULL;
    g_autofree char *etag_path = NULL;
    g_autoptr(JsonNode) json_root = NULL;
    g_autoptr(JsonGenerator) generator = NULL;

    g_mutex_lock(&prerelease_update_mutex);
    g_mutex_lock(&prerelease_install_mutex);
    gboolean current = ctx->generation == prerelease_install_generation;
    g_mutex_unlock(&prerelease_install_mutex);
    if (!current) goto out;

    if (!data_dir) {
        g_warning("Failed to resolve pre-release data directory");
        goto out;
    }

    // 本地没有数据时不发条件请求，避免 304 后仍然没有可用数据
    if (prerelease_data_exists()) {
        local_etag = read_prerelease_etag();
    }

    session = soup_session_new();
    // YPK 文件较大，沿用较长的超时时间
    g_object_set(session, "timeout", 300u, NULL);

    msg = soup_message_new("GET", PRERELEASE_URL);
    if (!msg) {
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "无效的先行卡下载地址");
        goto out;
    }

    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    if (local_etag) {
        // 本地已有数据：带上上次的 ETag，上游未变化时返回 304 且无响应体
        soup_message_headers_append(headers, "If-None-Match", local_etag);
    }

    g_message("Checking pre-release cards from %s (local ETag: %s)",
              PRERELEASE_URL, local_etag ? local_etag : "none");

    input = soup_session_send(session, msg, NULL, &error);
    guint status = soup_message_get_status(msg);

    if (!error && status == SOUP_STATUS_NOT_MODIFIED) {
        g_message("Pre-release cards are up to date");
        result = PRERELEASE_UPDATE_UP_TO_DATE;
        goto out;
    }

    // 非 2xx 的错误页不能当作 YPK 压缩包保存（否则会写出损坏的压缩包）
    if (!error && !SOUP_STATUS_IS_SUCCESSFUL(status)) {
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
                            status, soup_message_get_reason_phrase(msg));
    }

    if (error) {
        goto out;
    }

    if (!input) {
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "下载响应为空");
        goto out;
    }

    const char *remote_etag = soup_message_headers_get_one(
        soup_message_get_response_headers(msg), "ETag");
    if (remote_etag) {
        etag_to_store = g_strdup(remote_etag);
    }

    // 下载到数据目录之外的临时文件，下载完成前不碰本地旧数据
    gint fd = g_file_open_tmp("ygo-prerelease-XXXXXX.ypk", &ypk_path, &error);
    if (fd < 0) {
        goto out;
    }
    g_close(fd, NULL);

    file = g_file_new_for_path(ypk_path);
    output = (GOutputStream *)g_file_replace(file, NULL, FALSE,
                                             G_FILE_CREATE_NONE, NULL, &error);
    if (error) {
        goto out;
    }

    gssize spliced = g_output_stream_splice(output, input,
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                            NULL, &error);
    if (spliced < 0 || error) {
        goto out;
    }

    // splice 已关闭流；清空指针避免重复释放
    g_clear_object(&output);
    g_clear_object(&input);
    g_clear_object(&file);

    parent = g_path_get_dirname(data_dir);
    stage = g_build_filename(parent, ".prerelease-update-XXXXXX", NULL);
    if (g_mkdir_with_parents(parent, 0755) != 0 || !g_mkdtemp(stage)) {
        error = g_error_new(G_FILE_ERROR, g_file_error_from_errno(errno),
                            "Cannot create staging directory: %s", g_strerror(errno));
        g_clear_pointer(&stage, g_free);
        goto out;
    }

    replacement = g_build_filename(stage, "pre-release", NULL);
    backup = g_build_filename(stage, "previous", NULL);
    if (g_mkdir(replacement, 0755) != 0) {
        error = g_error_new(G_FILE_ERROR, g_file_error_from_errno(errno),
                            "Cannot create replacement directory: %s", g_strerror(errno));
        goto out;
    }
    if (!data_directory_extract_zip(ypk_path, replacement, prerelease_archive_entry, &error))
        goto out;

    cdb_path = g_build_filename(replacement, PRERELEASE_CDB_FILENAME, NULL);
    json_root = parse_cdb_to_json(cdb_path);
    if (!json_root) {
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Cannot parse pre-release CDB");
        goto out;
    }

    json_path = g_build_filename(replacement, PRERELEASE_JSON_FILENAME, NULL);
    generator = json_generator_new();
    json_generator_set_root(generator, json_root);
    json_generator_set_pretty(generator, TRUE);
    if (!json_generator_to_file(generator, json_path, &error)) goto out;

    if (etag_to_store) {
        etag_path = g_build_filename(replacement, PRERELEASE_ETAG_FILENAME, NULL);
        if (!g_file_set_contents(etag_path, etag_to_store, -1, &error)) goto out;
    }

    g_mutex_lock(&prerelease_install_mutex);
    if (ctx->generation == prerelease_install_generation) {
        g_mutex_lock(&prerelease_cache_mutex);
        if (data_directory_replace(data_dir, replacement, backup, &error)) {
            prerelease_json_mtime = 0;
            result = PRERELEASE_UPDATE_UPDATED;
        }
        g_mutex_unlock(&prerelease_cache_mutex);
    }
    g_mutex_unlock(&prerelease_install_mutex);

out:
    if (error) {
        g_warning("Failed to update pre-release cards: %s", error->message);
        g_error_free(error);
    }

    g_clear_object(&output);
    g_clear_object(&input);
    g_clear_object(&file);
    g_clear_object(&msg);
    g_clear_object(&session);

    if (ypk_path) {
        g_remove(ypk_path);
        g_free(ypk_path);
    }

    g_free(etag_to_store);
    g_free(local_etag);
    g_free(data_dir);

    if (stage && (result == PRERELEASE_UPDATE_UPDATED || !backup ||
                  !g_file_test(backup, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK)))
        data_directory_remove(stage);
    g_mutex_unlock(&prerelease_update_mutex);
    update_finish(ctx, result);
    return NULL;
}

void update_prerelease_cards(PrereleaseUpdateCallback callback, gpointer user_data) {
    UpdateContext *ctx = g_new0(UpdateContext, 1);
    ctx->callback = callback;
    ctx->user_data = user_data;
    g_mutex_lock(&prerelease_install_mutex);
    ctx->generation = prerelease_install_generation;
    g_mutex_unlock(&prerelease_install_mutex);

    GThread *thread = g_thread_new("prerelease-update", update_prerelease_thread, ctx);
    g_thread_unref(thread);
}

JsonArray* search_prerelease_cards(const char *search_query) {
    if (!search_query || *search_query == '\0') {
        return NULL;
    }

    JsonArray *results = json_array_new();

    g_mutex_lock(&prerelease_cache_mutex);
    if (!prerelease_cache_ensure_loaded_locked() || !prerelease_root_array) {
        g_mutex_unlock(&prerelease_cache_mutex);
        json_array_unref(results);
        return NULL;
    }

    // 转换搜索词为小写以进行不区分大小写的搜索
    gchar *query_lower = g_utf8_strdown(search_query, -1);

    guint len = json_array_get_length(prerelease_root_array);
    for (guint i = 0; i < len; i++) {
        JsonObject *card = json_array_get_object_element(prerelease_root_array, i);
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
    g_mutex_unlock(&prerelease_cache_mutex);

    return results;
}

JsonArray* get_all_prerelease_cards(void) {
    JsonArray *results = json_array_new();

    g_mutex_lock(&prerelease_cache_mutex);
    if (!prerelease_cache_ensure_loaded_locked() || !prerelease_root_array) {
        g_mutex_unlock(&prerelease_cache_mutex);
        json_array_unref(results);
        return NULL;
    }

    guint len = json_array_get_length(prerelease_root_array);
    for (guint i = 0; i < len; i++) {
        JsonObject *card = json_array_get_object_element(prerelease_root_array, i);
        if (card) {
            json_array_add_object_element(results, json_object_ref(card));
        }
    }

    g_mutex_unlock(&prerelease_cache_mutex);

    return results;
}

JsonObject* find_prerelease_card_by_id(int card_id) {
    JsonObject *found_card = NULL;

    g_mutex_lock(&prerelease_cache_mutex);
    if (prerelease_cache_ensure_loaded_locked() && prerelease_root_array) {
        guint len = json_array_get_length(prerelease_root_array);
        for (guint i = 0; i < len; i++) {
            JsonObject *card = json_array_get_object_element(prerelease_root_array, i);
            if (!card) continue;

            if (json_object_has_member(card, "id")) {
                int id = json_object_get_int_member(card, "id");
                if (id == card_id) {
                    found_card = json_object_ref(card);
                    break;
                }
            }
        }
    }
    g_mutex_unlock(&prerelease_cache_mutex);

    return found_card;
}

gchar* get_prerelease_card_image_path(int card_id) {
    gchar *data_dir = get_prerelease_data_dir();
    if (!data_dir) {
        return NULL;
    }

    gchar *filename = g_strdup_printf("%d.jpg", card_id);
    gchar *image_path = g_build_filename(data_dir, "pics", filename, NULL);
    g_free(filename);
    g_free(data_dir);
    
    // 检查文件是否存在
    if (!g_file_test(image_path, G_FILE_TEST_EXISTS)) {
        g_free(image_path);
        return NULL;
    }
    
    return image_path;
}

/**
 * 检查卡片ID是否为先行卡ID（9位数字，100000000-999999999）
 */
gboolean is_prerelease_id(int card_id) {
    return (card_id >= 100000000 && card_id <= 999999999);
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

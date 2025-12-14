#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "app_path.h"

#define OCG_FORBIDDEN_URL "https://www.db.yugioh-card.com/yugiohdb/forbidden_limited.action?request_locale=ja"
#define TCG_FORBIDDEN_URL "https://www.db.yugioh-card.com/yugiohdb/forbidden_limited.action?request_locale=en"
#define SC_FORBIDDEN_URL "https://yxwdbapi.windoent.com/forbiddenCard/forbiddencard/cachelist?groupId=1"

// 文件名常量
#define OCG_FORBIDDEN_FILENAME "ocg_forbidden.json"
#define TCG_FORBIDDEN_FILENAME "tcg_forbidden.json"
#define SC_FORBIDDEN_FILENAME "sc_forbidden.json"

/**
 * 获取配置数据目录的绝对路径
 * 返回值需要调用者使用 g_free() 释放
 */
static gchar *get_config_data_dir(void) {
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
 * 构建完整的输出文件路径
 * 返回值需要调用者使用 g_free() 释放
 */
static gchar *get_output_file_path(const char *filename) {
    gchar *data_dir = get_config_data_dir();
    if (!data_dir) {
        return NULL;
    }
    
    gchar *filepath = g_build_filename(data_dir, filename, NULL);
    g_free(data_dir);
    
    return filepath;
}

/**
 * 确保目录存在
 */
static gboolean ensure_directory_exists(const char *path) {
    // 使用 g_mkdir_with_parents 递归创建目录（类似 mkdir -p）
    if (g_mkdir_with_parents(path, 0755) == -1) {
        g_warning("Failed to create directory: %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * 解析HTML并提取禁限卡信息转换为JSON (OCG版本)
 * 根据JS代码逻辑，通过检测div id和提取cid来构建cid -> 限制等级的映射
 */
static JsonNode *parse_html_to_json_ocg(const char *html_content) {
    // 状态机：-1=none, 0=forbidden, 1=limited, 2=semi-limited
    int current_status = -1;
    const char *status_labels[] = {"禁止", "限制", "准限制"};
    
    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    // 按行分割HTML
    gchar **lines = g_strsplit(html_content, "\n", -1);
    
    for (int i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];
        
        // 检测区域结束
        if (strstr(line, "</div><!-- #list_semi_limited .list_set -->")) {
            current_status = -1;
        }
        
        // 检测区域开始
        if (strstr(line, "<div id=\"list_semi_limited\" class=\"list_set\">")) {
            current_status = 2; // 准限制
        }
        if (strstr(line, "<div id=\"list_forbidden\" class=\"list_set\">")) {
            current_status = 0; // 禁止
        }
        if (strstr(line, "<div id=\"list_limited\" class=\"list_set\">")) {
            current_status = 1; // 限制
        }
        
        // 提取cid
        if (current_status >= 0) {
            // 匹配: <input class="link_value" value="...cid=123..."
            const char *input_pos = strstr(line, "<input");
            if (input_pos) {
                const char *class_pos = strstr(input_pos, "class=\"link_value\"");
                const char *value_pos = strstr(input_pos, "value=\"");
                
                if (class_pos && value_pos) {
                    const char *cid_pos = strstr(value_pos, "cid=");
                    if (cid_pos) {
                        cid_pos += 4; // 跳过 "cid="
                        char cid_buf[32] = {0};
                        int j = 0;
                        while (cid_pos[j] >= '0' && cid_pos[j] <= '9' && j < 31) {
                            cid_buf[j] = cid_pos[j];
                            j++;
                        }
                        if (j > 0) {
                            g_hash_table_insert(
                                mapping,
                                g_strdup(cid_buf),
                                g_strdup(status_labels[current_status])
                            );
                        }
                    }
                }
            }
        }
    }
    
    g_strfreev(lines);
    
    // 构建JSON
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mapping);
    
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        json_builder_set_member_name(builder, (const char *)key);
        json_builder_add_string_value(builder, (const char *)value);
    }
    
    json_builder_end_object(builder);
    
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    guint entry_count = g_hash_table_size(mapping);
    g_hash_table_unref(mapping);
    
    g_message("OCG: Parsed %d card entries", entry_count);
    
    return root;
}

/**
 * 解析HTML并提取禁限卡信息转换为JSON (TCG版本)
 * 根据JS代码逻辑，通过检测div id和提取cid来构建cid -> 限制等级的映射
 */
static JsonNode *parse_html_to_json_tcg(const char *html_content) {
    // 状态机：-1=none, 0=forbidden, 1=limited, 2=semi-limited
    int current_status = -1;
    const char *status_labels[] = {"禁止", "限制", "准限制"};
    
    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    // 按行分割HTML
    gchar **lines = g_strsplit(html_content, "\n", -1);
    
    for (int i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];
        
        // 检测区域结束
        if (strstr(line, "</div><!-- #list_semi_limited .list_set -->")) {
            current_status = -1;
        }
        
        // 检测区域开始
        if (strstr(line, "<div id=\"list_semi_limited\" class=\"list_set\">")) {
            current_status = 2; // 准限制
        }
        if (strstr(line, "<div id=\"list_forbidden\" class=\"list_set\">")) {
            current_status = 0; // 禁止
        }
        if (strstr(line, "<div id=\"list_limited\" class=\"list_set\">")) {
            current_status = 1; // 限制
        }
        
        // 提取cid
        if (current_status >= 0) {
            const char *input_pos = strstr(line, "<input");
            if (input_pos) {
                const char *class_pos = strstr(input_pos, "class=\"link_value\"");
                const char *value_pos = strstr(input_pos, "value=\"");
                
                if (class_pos && value_pos) {
                    const char *cid_pos = strstr(value_pos, "cid=");
                    if (cid_pos) {
                        cid_pos += 4;
                        char cid_buf[32] = {0};
                        int j = 0;
                        while (cid_pos[j] >= '0' && cid_pos[j] <= '9' && j < 31) {
                            cid_buf[j] = cid_pos[j];
                            j++;
                        }
                        if (j > 0) {
                            g_hash_table_insert(
                                mapping,
                                g_strdup(cid_buf),
                                g_strdup(status_labels[current_status])
                            );
                        }
                    }
                }
            }
        }
    }
    
    g_strfreev(lines);
    
    // 构建JSON
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mapping);
    
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        json_builder_set_member_name(builder, (const char *)key);
        json_builder_add_string_value(builder, (const char *)value);
    }
    
    json_builder_end_object(builder);
    
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    
    guint entry_count = g_hash_table_size(mapping);
    g_hash_table_unref(mapping);
    
    g_message("TCG: Parsed %d card entries", entry_count);
    
    return root;
}

/**
 * 处理SC禁限卡表JSON数据（已经是JSON格式）
 * 解析JSON数组，提取type为"禁止卡"、"限制卡"、"准限制卡"的条目
 * 将cardNo映射到对应的禁限状态，输出格式与OCG/TCG一致
 */
static JsonNode *process_sc_json(const char *json_content) {
    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    
    if (!json_parser_load_from_data(parser, json_content, -1, &error)) {
        g_warning("Failed to parse SC JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return NULL;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("SC JSON root is not an object");
        g_object_unref(parser);
        return NULL;
    }
    
    JsonObject *root_obj = json_node_get_object(root);
    
    // 获取list字段
    if (!json_object_has_member(root_obj, "list")) {
        g_warning("SC JSON does not have 'list' field");
        g_object_unref(parser);
        return NULL;
    }
    
    JsonArray *list_array = json_object_get_array_member(root_obj, "list");
    guint list_length = json_array_get_length(list_array);
    
    // 创建映射表：cardNo -> 状态
    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    
    // 遍历list数组中的每个分组
    for (guint i = 0; i < list_length; i++) {
        JsonNode *group_node = json_array_get_element(list_array, i);
        
        if (!JSON_NODE_HOLDS_OBJECT(group_node)) {
            continue;
        }
        
        JsonObject *group_obj = json_node_get_object(group_node);
        
        // 获取该分组的type
        if (!json_object_has_member(group_obj, "type")) {
            continue;
        }
        const char *group_type = json_object_get_string_member(group_obj, "type");
        
        // 确定该分组的状态映射
        const char *status = NULL;
        if (g_strcmp0(group_type, "禁止卡") == 0) {
            status = "禁止";
        } else if (g_strcmp0(group_type, "限制卡") == 0) {
            status = "限制";
        } else if (g_strcmp0(group_type, "准限制卡") == 0) {
            status = "准限制";
        } else {
            continue; // 跳过其他分组（如"更新卡片"、"解除限制卡片"）
        }
        
        // 获取该分组的list数组
        if (!json_object_has_member(group_obj, "list")) {
            continue;
        }
        
        JsonArray *cards_array = json_object_get_array_member(group_obj, "list");
        guint cards_length = json_array_get_length(cards_array);
        
        // 遍历该分组中的每张卡
        for (guint j = 0; j < cards_length; j++) {
            JsonNode *card_node = json_array_get_element(cards_array, j);
            
            if (!JSON_NODE_HOLDS_OBJECT(card_node)) {
                continue;
            }
            
            JsonObject *card_obj = json_node_get_object(card_node);
            
            // 获取cardNo字段
            if (!json_object_has_member(card_obj, "cardNo")) {
                continue;
            }
            
            // cardNo可能是字符串或整数
            const char *card_no = NULL;
            gchar *card_no_str = NULL;
            
            JsonNode *card_no_node = json_object_get_member(card_obj, "cardNo");
            if (JSON_NODE_HOLDS_VALUE(card_no_node)) {
                GType value_type = json_node_get_value_type(card_no_node);
                
                if (value_type == G_TYPE_STRING) {
                    card_no = json_object_get_string_member(card_obj, "cardNo");
                } else if (value_type == G_TYPE_INT64) {
                    gint64 card_no_int = json_object_get_int_member(card_obj, "cardNo");
                    card_no_str = g_strdup_printf("%ld", card_no_int);
                    card_no = card_no_str;
                }
            }
            
            if (card_no && strlen(card_no) > 0) {
                g_hash_table_insert(
                    mapping,
                    g_strdup(card_no),
                    g_strdup(status)
                );
            }
            
            if (card_no_str) {
                g_free(card_no_str);
            }
        }
    }
    
    // 构建输出JSON（格式与OCG/TCG一致）
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mapping);
    
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        json_builder_set_member_name(builder, (const char *)key);
        json_builder_add_string_value(builder, (const char *)value);
    }
    
    json_builder_end_object(builder);
    
    JsonNode *result = json_builder_get_root(builder);
    g_object_unref(builder);
    
    guint entry_count = g_hash_table_size(mapping);
    g_hash_table_unref(mapping);
    g_object_unref(parser);
    
    g_message("SC: Parsed %d card entries", entry_count);
    
    return result;
}

/**
 * 保存JSON数据到文件
 */
static gboolean save_json_to_file(JsonNode *root, const char *filepath) {
    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    
    GError *error = NULL;
    gboolean success = json_generator_to_file(generator, filepath, &error);
    
    if (!success) {
        g_warning("Failed to save JSON to %s: %s", filepath, error->message);
        g_error_free(error);
    } else {
        g_message("Successfully saved forbidden list to %s", filepath);
    }
    
    g_object_unref(generator);
    return success;
}

/**
 * 下载完成后的回调函数
 */
static void on_download_complete(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download OCG forbidden list: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }
    
    gsize size;
    const char *html_content = g_bytes_get_data(response_body, &size);
    
    if (html_content && size > 0) {
        // 确保配置数据目录存在
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);
        
        // 解析HTML并转换为JSON
        JsonNode *json_root = parse_html_to_json_ocg(html_content);
        
        // 获取输出文件路径并保存
        gchar *output_file = get_output_file_path(OCG_FORBIDDEN_FILENAME);
        if (output_file) {
            save_json_to_file(json_root, output_file);
            g_free(output_file);
        }
        
        json_node_free(json_root);
    } else {
        g_warning("Received empty response from OCG forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 后台线程执行的下载任务
 */
static gpointer download_thread_func(G_GNUC_UNUSED gpointer data) {
    // 创建独立的SoupSession用于后台下载
    SoupSession *session = soup_session_new();
    
    // 创建请求消息
    SoupMessage *msg = soup_message_new("GET", OCG_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for OCG forbidden list");
        g_object_unref(session);
        return NULL;
    }
    
    // 设置User-Agent避免被服务器拒绝
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of OCG forbidden list...");
    
    // 异步发送请求
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete,
        NULL
    );
    
    g_object_unref(msg);
    
    return NULL;
}

/**
 * 下载完成后的回调函数 (TCG)
 */
static void on_download_complete_tcg(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download TCG forbidden list: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }
    
    gsize size;
    const char *html_content = g_bytes_get_data(response_body, &size);
    
    if (html_content && size > 0) {
        // 确保配置数据目录存在
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);
        
        // 解析HTML并转换为JSON
        JsonNode *json_root = parse_html_to_json_tcg(html_content);
        
        // 获取输出文件路径并保存
        gchar *output_file = get_output_file_path(TCG_FORBIDDEN_FILENAME);
        if (output_file) {
            save_json_to_file(json_root, output_file);
            g_free(output_file);
        }
        
        json_node_free(json_root);
    } else {
        g_warning("Received empty response from TCG forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 下载完成后的回调函数 (SC)
 */
static void on_download_complete_sc(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download SC forbidden list: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }
    
    gsize size;
    const char *json_content = g_bytes_get_data(response_body, &size);
    
    if (json_content && size > 0) {
        // 确保配置数据目录存在
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);
        
        // 处理JSON数据
        JsonNode *json_root = process_sc_json(json_content);
        
        if (json_root) {
            // 获取输出文件路径并保存
            gchar *output_file = get_output_file_path(SC_FORBIDDEN_FILENAME);
            if (output_file) {
                save_json_to_file(json_root, output_file);
                g_free(output_file);
            }
            json_node_free(json_root);
        }
    } else {
        g_warning("Received empty response from SC forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 后台线程执行的下载任务 (TCG)
 */
static gpointer download_thread_func_tcg(G_GNUC_UNUSED gpointer data) {
    // 创建独立的SoupSession用于后台下载
    SoupSession *session = soup_session_new();
    
    // 创建请求消息
    SoupMessage *msg = soup_message_new("GET", TCG_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for TCG forbidden list");
        g_object_unref(session);
        return NULL;
    }
    
    // 设置User-Agent避免被服务器拒绝
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of TCG forbidden list...");
    
    // 异步发送请求
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_tcg,
        NULL
    );
    
    g_object_unref(msg);
    
    return NULL;
}

/**
 * 后台线程执行的下载任务 (SC)
 */
static gpointer download_thread_func_sc(G_GNUC_UNUSED gpointer data) {
    // 创建独立的SoupSession用于后台下载
    SoupSession *session = soup_session_new();
    
    // 创建请求消息
    SoupMessage *msg = soup_message_new("GET", SC_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for SC forbidden list");
        g_object_unref(session);
        return NULL;
    }
    
    // 设置User-Agent避免被服务器拒绝
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of SC forbidden list...");
    
    // 异步发送请求
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_sc,
        NULL
    );
    
    g_object_unref(msg);
    
    return NULL;
}

/**
 * 启动后台更新OCG禁限卡表
 * 此函数立即返回，实际下载在后台线程中进行
 */
void startup_update_ocg_forbidden(void) {
    GThread *thread = g_thread_new("ocg-forbidden-update", download_thread_func, NULL);
    
    if (thread) {
        // 分离线程，让它在后台自行运行
        g_thread_unref(thread);
        g_message("OCG forbidden list update started in background");
    } else {
        g_warning("Failed to start OCG forbidden list update thread");
    }
}

/**
 * 启动后台更新TCG禁限卡表
 * 此函数立即返回，实际下载在后台线程中进行
 */
void startup_update_tcg_forbidden(void) {
    GThread *thread = g_thread_new("tcg-forbidden-update", download_thread_func_tcg, NULL);
    
    if (thread) {
        // 分离线程，让它在后台自行运行
        g_thread_unref(thread);
        g_message("TCG forbidden list update started in background");
    } else {
        g_warning("Failed to start TCG forbidden list update thread");
    }
}

/**
 * 启动后台更新SC禁限卡表
 * 此函数立即返回，实际下载在后台线程中进行
 */
void startup_update_sc_forbidden(void) {
    GThread *thread = g_thread_new("sc-forbidden-update", download_thread_func_sc, NULL);
    
    if (thread) {
        // 分离线程，让它在后台自行运行
        g_thread_unref(thread);
        g_message("SC forbidden list update started in background");
    } else {
        g_warning("Failed to start SC forbidden list update thread");
    }
}

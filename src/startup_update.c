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
#define AE_FORBIDDEN_URL "https://www.db.yugioh-card.com/yugiohdb/forbidden_limited.action?request_locale=ae"
#define SC_FORBIDDEN_URL "https://yxwdbapi.windoent.com/forbiddenCard/forbiddencard/cachelist?groupId=1"
#define GENESYS_FORBIDDEN_URL "https://www.yugioh-card.com/en/genesys/"

// 文件名常量
#define OCG_FORBIDDEN_FILENAME "ocg_forbidden.json"
#define TCG_FORBIDDEN_FILENAME "tcg_forbidden.json"
#define AE_FORBIDDEN_FILENAME "ae_forbidden.json"
#define SC_FORBIDDEN_FILENAME "sc_forbidden.json"
#define GENESYS_FORBIDDEN_FILENAME "genesys_forbidden.json"
#define OCG_FORBIDDEN_CHANGES_FILENAME "ocg_forbidden_changes.json"
#define TCG_FORBIDDEN_CHANGES_FILENAME "tcg_forbidden_changes.json"
#define AE_FORBIDDEN_CHANGES_FILENAME "ae_forbidden_changes.json"
#define SC_FORBIDDEN_CHANGES_FILENAME "sc_forbidden_changes.json"

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
 * 解析HTML并提取禁限卡信息转换为JSON (AE版本)
 * 根据JS代码逻辑，通过检测div id和提取cid来构建cid -> 限制等级的映射
 */
static JsonNode *parse_html_to_json_ae(const char *html_content) {
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
    
    g_message("AE: Parsed %d card entries", entry_count);
    
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
 * 下载完成后的回调函数 (AE)
 */
static void on_download_complete_ae(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download AE forbidden list: %s", error->message);
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
        JsonNode *json_root = parse_html_to_json_ae(html_content);
        
        // 获取输出文件路径并保存
        gchar *output_file = get_output_file_path(AE_FORBIDDEN_FILENAME);
        if (output_file) {
            save_json_to_file(json_root, output_file);
            g_free(output_file);
        }
        
        json_node_free(json_root);
    } else {
        g_warning("Received empty response from AE forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
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
 * 后台线程执行的下载任务 (AE)
 */
static gpointer download_thread_func_ae(G_GNUC_UNUSED gpointer data) {
    // 创建独立的SoupSession用于后台下载
    SoupSession *session = soup_session_new();
    
    // 创建请求消息
    SoupMessage *msg = soup_message_new("GET", AE_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for AE forbidden list");
        g_object_unref(session);
        return NULL;
    }
    
    // 设置User-Agent避免被服务器拒绝
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of AE forbidden list...");
    
    // 异步发送请求
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_ae,
        NULL
    );
    
    g_object_unref(msg);
    
    return NULL;
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
 * 启动后台更新AE禁限卡表
 * 此函数立即返回，实际下载在后台线程中进行
 */
void startup_update_ae_forbidden(void) {
    GThread *thread = g_thread_new("ae-forbidden-update", download_thread_func_ae, NULL);
    
    if (thread) {
        // 分离线程，让它在后台自行运行
        g_thread_unref(thread);
        g_message("AE forbidden list update started in background");
    } else {
        g_warning("Failed to start AE forbidden list update thread");
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

// ============================================================
// GENESYS 分值表解析与下载
// ============================================================

/**
 * 去除字符串中的HTML标签，返回纯文字
 * 返回值需要调用者 g_free() 释放
 */
static gchar *strip_html_tags(const char *input) {
    GString *result = g_string_new(NULL);
    gboolean in_tag = FALSE;
    for (const char *p = input; *p; p++) {
        if (*p == '<') {
            in_tag = TRUE;
        } else if (*p == '>') {
            in_tag = FALSE;
        } else if (!in_tag) {
            g_string_append_c(result, *p);
        }
    }
    return g_string_free(result, FALSE);
}

/**
 * 解析GENESYS页面HTML，提取 英文卡名 -> 分值 映射
 * GENESYS规则：灵摆怪兽、连接怪兽全部禁止；其余卡片使用分值制
 * 分值表格式：<table>...<tr><td>Card Name</td><td>Points</td></tr>...</table>
 */
static JsonNode *parse_html_to_json_genesys(const char *html_content) {
    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    const char *pos = html_content;

    while ((pos = strstr(pos, "<tr")) != NULL) {
        // 找到 <tr ...> 的结束 '>'
        const char *row_tag_end = strchr(pos, '>');
        if (!row_tag_end) break;
        const char *row_start = row_tag_end + 1;

        // 找到 </tr>
        const char *row_end = strstr(row_start, "</tr>");
        if (!row_end) break;

        // 提取第一个 <td>...</td>（卡名列）
        const char *td1 = strstr(row_start, "<td");
        if (!td1 || td1 >= row_end) { pos = row_end + 5; continue; }
        const char *td1_content_start = strchr(td1, '>');
        if (!td1_content_start || td1_content_start >= row_end) { pos = row_end + 5; continue; }
        td1_content_start++;
        const char *td1_end = strstr(td1_content_start, "</td>");
        if (!td1_end || td1_end >= row_end) { pos = row_end + 5; continue; }

        // 提取第二个 <td>...</td>（分值列）
        const char *td2 = strstr(td1_end + 5, "<td");
        if (!td2 || td2 >= row_end) { pos = row_end + 5; continue; }
        const char *td2_content_start = strchr(td2, '>');
        if (!td2_content_start || td2_content_start >= row_end) { pos = row_end + 5; continue; }
        td2_content_start++;
        const char *td2_end = strstr(td2_content_start, "</td>");
        if (!td2_end || td2_end >= row_end) { pos = row_end + 5; continue; }

        // 取出原始文字并去除HTML标签
        gchar *raw_name  = g_strndup(td1_content_start, td1_end - td1_content_start);
        gchar *raw_pts   = g_strndup(td2_content_start, td2_end - td2_content_start);

        gchar *stripped_name = strip_html_tags(raw_name);
        gchar *stripped_pts  = strip_html_tags(raw_pts);
        g_free(raw_name);
        g_free(raw_pts);

        gchar *card_name  = g_strstrip(stripped_name);
        gchar *points_str = g_strstrip(stripped_pts);

        // 跳过表头行（不全是数字的分值列）和空行
        if (card_name && *card_name != '\0' && points_str && *points_str != '\0') {
            // 检验 points_str 全部为数字
            gboolean is_number = TRUE;
            for (int k = 0; points_str[k] != '\0'; k++) {
                if (!g_ascii_isdigit(points_str[k])) { is_number = FALSE; break; }
            }
            if (is_number) {
                // 确保卡名是合法的 UTF-8 字符串。
                // 官网个别卡名含有 Latin-1 单字节字符（如 Ø = 0xD8），
                // 直接写入 JSON 会导致整个文件解析失败。
                // 若验证失败，则尝试从 ISO-8859-1 转换；仍失败则忽略该条目。
                gchar *safe_name = NULL;
                if (g_utf8_validate(card_name, -1, NULL)) {
                    safe_name = g_strdup(card_name);
                } else {
                    GError *conv_err = NULL;
                    gsize bytes_written = 0;
                    safe_name = g_convert(card_name, -1,
                                          "UTF-8", "ISO-8859-1",
                                          NULL, &bytes_written, &conv_err);
                    if (conv_err) {
                        g_warning("GENESYS: failed to convert card name to UTF-8, skipping: %s", conv_err->message);
                        g_error_free(conv_err);
                    }
                }

                if (safe_name) {
                    g_hash_table_insert(mapping, safe_name, g_strdup(points_str));
                }
            }
        }

        g_free(stripped_name);
        g_free(stripped_pts);

        pos = row_end + 5;
    }

    // 构建JSON对象：英文卡名 -> 分值字符串
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

    g_message("GENESYS: Parsed %d card point entries", entry_count);
    return root;
}

/**
 * GENESYS下载完成后的回调
 */
static void on_download_complete_genesys(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);

    if (error) {
        g_warning("Failed to download GENESYS points list: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }

    gsize size;
    const char *html_content = g_bytes_get_data(response_body, &size);

    if (html_content && size > 0) {
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);

        JsonNode *json_root = parse_html_to_json_genesys(html_content);

        gchar *output_file = get_output_file_path(GENESYS_FORBIDDEN_FILENAME);
        if (output_file) {
            save_json_to_file(json_root, output_file);
            g_free(output_file);
        }
        json_node_free(json_root);
    } else {
        g_warning("Received empty response from GENESYS points URL");
    }

    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 后台线程：下载GENESYS分值表
 */
static gpointer download_thread_func_genesys(G_GNUC_UNUSED gpointer data) {
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", GENESYS_FORBIDDEN_URL);

    if (!msg) {
        g_warning("Failed to create HTTP request for GENESYS points list");
        g_object_unref(session);
        return NULL;
    }

    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    // Accept cookies/HTML
    soup_message_headers_append(headers, "Accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

    g_message("Starting background download of GENESYS points list...");

    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_genesys,
        NULL
    );

    g_object_unref(msg);
    return NULL;
}

/**
 * 启动后台更新GENESYS分值表
 * 此函数立即返回，实际下载在后台线程中进行
 */
void startup_update_genesys_forbidden(void) {
    GThread *thread = g_thread_new("genesys-forbidden-update", download_thread_func_genesys, NULL);

    if (thread) {
        g_thread_unref(thread);
        g_message("GENESYS points list update started in background");
    } else {
        g_warning("Failed to start GENESYS points list update thread");
    }
}

// ============================================================
// 禁限卡表变更信息解析与下载
// ============================================================

/**
 * 规范化禁限状态名称
 * 将"制限"改为"限制"，"保持"等无意义的状态改为"未变化"
 */
static const char *normalize_status(const char *status) {
    if (!status) return NULL;

    g_autofree gchar *trimmed = g_strdup(status);
    g_strstrip(trimmed);

    if (!*trimmed) return NULL;

    // 日文/中文
    if (g_strcmp0(trimmed, "禁止") == 0) return "禁止";
    if (g_strcmp0(trimmed, "制限") == 0) return "限制";
    if (g_strcmp0(trimmed, "限制") == 0) return "限制";
    if (g_strcmp0(trimmed, "準制限") == 0) return "准限制";
    if (g_strcmp0(trimmed, "准限制") == 0) return "准限制";
    if (g_strcmp0(trimmed, "無制限") == 0) return "无限制";
    if (g_strcmp0(trimmed, "無限制") == 0) return "无限制";
    if (g_strcmp0(trimmed, "无限制") == 0) return "无限制";
    if (g_strcmp0(trimmed, "解除") == 0) return "无限制";

    // 英文（TCG/AE）
    if (g_ascii_strcasecmp(trimmed, "Forbidden") == 0) return "禁止";
    if (g_ascii_strcasecmp(trimmed, "Limited") == 0) return "限制";
    if (g_ascii_strcasecmp(trimmed, "Semi-Limited") == 0) return "准限制";
    if (g_ascii_strcasecmp(trimmed, "Semi Limited") == 0) return "准限制";
    if (g_ascii_strcasecmp(trimmed, "No Longer on List") == 0) return "无限制";
    if (g_ascii_strcasecmp(trimmed, "Unlimited") == 0) return "无限制";

    return NULL;
}

// 解析单条变更文本，输出 old/new 状态
static gboolean parse_forbidden_change_text(const char *raw_text,
                                            const char **out_old,
                                            const char **out_new) {
    if (!raw_text || !out_old || !out_new) return FALSE;

    g_autofree gchar *text = g_strdup(raw_text);
    g_strstrip(text);
    if (!*text) return FALSE;

    // 仅按需要处理 HTML 实体和箭头表示，不做整串替换
    const char *decoded3 = text;

    const char *norm_old = NULL;
    const char *norm_new = NULL;

    // 日文：新規・制限 / 新規 ・制限
    if (g_str_has_prefix(decoded3, "新規")) {
        const char *status_part = decoded3 + strlen("新規");
        while (*status_part == ' ' || *status_part == '\t') {
            status_part++;
        }
        if (g_str_has_prefix(status_part, "・")) {
            status_part += strlen("・");
        }
        while (*status_part == ' ' || *status_part == '\t') {
            status_part++;
        }
        norm_old = "无限制";
        norm_new = normalize_status(status_part);
    }

    // 英文：Newly Limited / Newly Forbidden / Newly Semi-Limited
    if (!norm_new && g_ascii_strncasecmp(decoded3, "Newly", 5) == 0) {
        const char *status_part = decoded3 + 5;
        while (*status_part == ' ' || *status_part == '\t' || *status_part == ':' || *status_part == '-') {
            status_part++;
        }
        norm_old = "无限制";
        norm_new = normalize_status(status_part);
    }

    // 通用：A → B
    if (!norm_old || !norm_new) {
        const char *arrow = strstr(decoded3, "→");
        int arrow_len = (int)strlen("→");
        if (!arrow) {
            arrow = strstr(decoded3, "-&gt;");
            arrow_len = (int)strlen("-&gt;");
        }
        if (!arrow) {
            arrow = strstr(decoded3, "&rArr;");
            arrow_len = (int)strlen("&rArr;");
        }
        if (!arrow) {
            arrow = strstr(decoded3, "->");
            arrow_len = 2;
        }
        if (arrow) {
            g_autofree gchar *old_part = g_strndup(decoded3, arrow - decoded3);
            const char *new_part = arrow + arrow_len;
            g_strstrip(old_part);

            g_autofree gchar *new_part_dup = g_strdup(new_part);
            g_strstrip(new_part_dup);

            norm_old = normalize_status(old_part);
            norm_new = normalize_status(new_part_dup);
        }
    }

    if (!norm_old || !norm_new) return FALSE;

    *out_old = norm_old;
    *out_new = norm_new;
    return TRUE;
}

static JsonNode *build_forbidden_changes_json_from_mapping(GHashTable *mapping) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mapping);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        json_builder_set_member_name(builder, (const char *)key);
        JsonObject *change_obj = (JsonObject *)value;
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "old");
        json_builder_add_string_value(builder, json_object_get_string_member(change_obj, "old"));

        json_builder_set_member_name(builder, "new");
        json_builder_add_string_value(builder, json_object_get_string_member(change_obj, "new"));

        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    g_object_unref(builder);
    return root;
}

static JsonNode *parse_html_forbidden_changes_common(const char *html_content, const char *mode_name) {
    if (!html_content) return NULL;

    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)json_object_unref);

    gchar **lines = g_strsplit(html_content, "\n", -1);
    gboolean in_changes = FALSE;
    gchar *current_card_id = NULL;

    for (int i = 0; lines[i] != NULL; i++) {
        const char *line = lines[i];

        if (!in_changes && (strstr(line, "id=\"list_update\"") || strstr(line, "更新のあったカード"))) {
            in_changes = TRUE;
            continue;
        }

        if (!in_changes) continue;

        // 进入下一分区即结束更新区块
        if (strstr(line, "<div id=\"list_forbidden\" class=\"list_set\">")) {
            break;
        }

        // 提取卡片cid
        const char *link_start = strstr(line, "class=\"link_value\" value=\"");
        if (link_start) {
            const char *cid_start = strstr(link_start, "cid=");
            if (cid_start) {
                cid_start += strlen("cid=");
                const char *cid_end = cid_start;
                while (*cid_end >= '0' && *cid_end <= '9') {
                    cid_end++;
                }
                if (cid_end > cid_start) {
                    g_free(current_card_id);
                    current_card_id = g_strndup(cid_start, cid_end - cid_start);
                }
            }
            continue;
        }

        // 提取变更文本
        const char *change_start = strstr(line, "<p>");
        if (change_start && current_card_id && *current_card_id) {
            change_start += strlen("<p>");
            const char *change_end = strstr(change_start, "</p>");
            if (!change_end) {
                continue;
            }

            g_autofree gchar *change_info = g_strndup(change_start, change_end - change_start);
            g_strstrip(change_info);

            const char *norm_old = NULL;
            const char *norm_new = NULL;
            if (parse_forbidden_change_text(change_info, &norm_old, &norm_new)) {
                JsonObject *change_obj = json_object_new();
                json_object_set_string_member(change_obj, "old", norm_old);
                json_object_set_string_member(change_obj, "new", norm_new);

                g_hash_table_insert(mapping, g_strdup(current_card_id), change_obj);
                g_free(current_card_id);
                current_card_id = NULL;
            }
        }
    }

    g_free(current_card_id);
    g_strfreev(lines);

    if (!in_changes) {
        g_warning("Could not find list_update section in %s forbidden list", mode_name);
    }

    JsonNode *root = build_forbidden_changes_json_from_mapping(mapping);
    guint entry_count = g_hash_table_size(mapping);
    g_hash_table_unref(mapping);

    g_message("%s forbidden changes: Parsed %u card changes", mode_name, entry_count);
    return root;
}

/**
 * 从"更新のあったカード"（更新的卡片）部分解析禁限变更
 * HTML格式示例：
 *   <span class="name">VS ホーリー・スー</span>
 *   ...
 *   <input type="hidden" class="link_value" value="/yugiohdb/card_search.action?ope=2&cid=21433">
 *   ...
 *   <p>新規 ・制限</p>
 */
static JsonNode *parse_html_forbidden_changes_ocg(const char *html_content) {
    return parse_html_forbidden_changes_common(html_content, "OCG");
}

/**
 * TCG禁限变更解析函数
 * 注意：TCG网页没有"更新のあったカード"部分，所以返回空的JSON对象
 */
static JsonNode *parse_html_forbidden_changes_tcg(const char *html_content) {
    return parse_html_forbidden_changes_common(html_content, "TCG");
}

/**
 * AE禁限变更解析函数
 * 注意：AE网页没有"更新のあったカード"部分，所以返回空的JSON对象
 */
static JsonNode *parse_html_forbidden_changes_ae(const char *html_content) {
    return parse_html_forbidden_changes_common(html_content, "AE");
}

// 解析SC接口中的单条变更状态文本
// 例如："准限制-限制"、"限制-准限制"、"准限制-解除"、"禁止"
static gboolean parse_sc_forbidden_change_type(const char *raw_type,
                                               const char **out_old,
                                               const char **out_new) {
    if (!raw_type || !out_old || !out_new) {
        return FALSE;
    }

    g_autofree gchar *type = g_strdup(raw_type);
    g_strstrip(type);
    if (!*type) {
        return FALSE;
    }

    // 统一分隔符，兼容 "A- B" / "A→B" / "A->B"
    const char *arrow = strstr(type, "→");
    int arrow_len = 0;
    if (arrow) {
        arrow_len = (int)strlen("→");
    } else {
        arrow = strstr(type, "->");
        if (arrow) {
            arrow_len = 2;
        } else {
            arrow = strchr(type, '-');
            if (arrow) {
                arrow_len = 1;
            }
        }
    }

    const char *norm_old = NULL;
    const char *norm_new = NULL;

    if (arrow) {
        g_autofree gchar *old_part = g_strndup(type, arrow - type);
        g_autofree gchar *new_part = g_strdup(arrow + arrow_len);
        g_strstrip(old_part);
        g_strstrip(new_part);

        norm_old = normalize_status(old_part);
        norm_new = normalize_status(new_part);
    } else {
        // 单状态按“从无限制变为该状态”处理
        norm_old = "无限制";
        norm_new = normalize_status(type);
    }

    if (!norm_old || !norm_new) {
        return FALSE;
    }

    *out_old = norm_old;
    *out_new = norm_new;
    return TRUE;
}

// 解析SC禁限变更（JSON接口）
static JsonNode *parse_sc_forbidden_changes_json(const char *json_content) {
    if (!json_content) {
        return NULL;
    }

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_content, -1, &error)) {
        g_warning("Failed to parse SC forbidden changes JSON: %s", error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *root_obj = json_node_get_object(root);
    JsonNode *list_node = json_object_get_member(root_obj, "list");
    if (!list_node || !JSON_NODE_HOLDS_ARRAY(list_node)) {
        g_warning("SC forbidden changes JSON does not contain list array");
        g_object_unref(parser);
        return NULL;
    }

    JsonArray *groups = json_node_get_array(list_node);
    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                (GDestroyNotify)json_object_unref);

    guint group_count = json_array_get_length(groups);
    for (guint i = 0; i < group_count; i++) {
        JsonNode *group_node = json_array_get_element(groups, i);
        if (!group_node || !JSON_NODE_HOLDS_OBJECT(group_node)) {
            continue;
        }

        JsonObject *group_obj = json_node_get_object(group_node);
        JsonNode *type_node = json_object_get_member(group_obj, "type");
        JsonNode *cards_node = json_object_get_member(group_obj, "list");

        if (!type_node || !cards_node ||
            !JSON_NODE_HOLDS_VALUE(type_node) ||
            json_node_get_value_type(type_node) != G_TYPE_STRING ||
            !JSON_NODE_HOLDS_ARRAY(cards_node)) {
            continue;
        }

        const char *group_type = json_node_get_string(type_node);
        if (!group_type || !strstr(group_type, "更新卡片")) {
            continue;
        }

        JsonArray *cards = json_node_get_array(cards_node);
        guint card_count = json_array_get_length(cards);
        for (guint j = 0; j < card_count; j++) {
            JsonNode *card_node = json_array_get_element(cards, j);
            if (!card_node || !JSON_NODE_HOLDS_OBJECT(card_node)) {
                continue;
            }

            JsonObject *card_obj = json_node_get_object(card_node);
            JsonNode *card_no_node = json_object_get_member(card_obj, "cardNo");
            JsonNode *change_type_node = json_object_get_member(card_obj, "forbiddenCardType");

            if (!card_no_node || !change_type_node ||
                !JSON_NODE_HOLDS_VALUE(change_type_node) ||
                json_node_get_value_type(change_type_node) != G_TYPE_STRING) {
                continue;
            }

            const char *change_type = json_node_get_string(change_type_node);
            if (!change_type || !*change_type) {
                continue;
            }

            gchar *card_no_str = NULL;
            if (JSON_NODE_HOLDS_VALUE(card_no_node)) {
                if (json_node_get_value_type(card_no_node) == G_TYPE_STRING) {
                    const char *s = json_node_get_string(card_no_node);
                    if (s && *s) {
                        card_no_str = g_strdup(s);
                    }
                } else if (json_node_get_value_type(card_no_node) == G_TYPE_INT64) {
                    gint64 v = json_node_get_int(card_no_node);
                    card_no_str = g_strdup_printf("%" G_GINT64_FORMAT, v);
                }
            }

            if (!card_no_str || !*card_no_str) {
                g_free(card_no_str);
                continue;
            }

            const char *norm_old = NULL;
            const char *norm_new = NULL;
            if (parse_sc_forbidden_change_type(change_type, &norm_old, &norm_new)) {
                JsonObject *change_obj = json_object_new();
                json_object_set_string_member(change_obj, "old", norm_old);
                json_object_set_string_member(change_obj, "new", norm_new);
                g_hash_table_insert(mapping, card_no_str, change_obj);
            } else {
                g_free(card_no_str);
            }
        }

        // SC只需要“更新卡片”分组，命中后可结束
        break;
    }

    JsonNode *out = build_forbidden_changes_json_from_mapping(mapping);
    guint entry_count = g_hash_table_size(mapping);
    g_hash_table_unref(mapping);
    g_object_unref(parser);

    g_message("SC forbidden changes: Parsed %u card changes", entry_count);
    return out;
}

/**
 * 下载完成后的回调函数（OCG变更）
 */
static void on_download_complete_ocg_changes(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download OCG forbidden list changes: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }
    
    gsize size;
    const char *html_content = g_bytes_get_data(response_body, &size);
    
    if (html_content && size > 0) {
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);
        
        JsonNode *json_root = parse_html_forbidden_changes_ocg(html_content);
        
        if (json_root) {
            gchar *output_file = get_output_file_path(OCG_FORBIDDEN_CHANGES_FILENAME);
            if (output_file) {
                save_json_to_file(json_root, output_file);
                g_free(output_file);
            }
            json_node_free(json_root);
        }
    } else {
        g_warning("Received empty response from OCG forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 下载完成后的回调函数（TCG变更）
 */
static void on_download_complete_tcg_changes(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download TCG forbidden list changes: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }
    
    gsize size;
    const char *html_content = g_bytes_get_data(response_body, &size);
    
    if (html_content && size > 0) {
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);
        
        JsonNode *json_root = parse_html_forbidden_changes_tcg(html_content);
        
        if (json_root) {
            gchar *output_file = get_output_file_path(TCG_FORBIDDEN_CHANGES_FILENAME);
            if (output_file) {
                save_json_to_file(json_root, output_file);
                g_free(output_file);
            }
            json_node_free(json_root);
        }
    } else {
        g_warning("Received empty response from TCG forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 下载完成后的回调函数（AE变更）
 */
static void on_download_complete_ae_changes(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);
    
    if (error) {
        g_warning("Failed to download AE forbidden list changes: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }
    
    gsize size;
    const char *html_content = g_bytes_get_data(response_body, &size);
    
    if (html_content && size > 0) {
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);
        
        JsonNode *json_root = parse_html_forbidden_changes_ae(html_content);
        
        if (json_root) {
            gchar *output_file = get_output_file_path(AE_FORBIDDEN_CHANGES_FILENAME);
            if (output_file) {
                save_json_to_file(json_root, output_file);
                g_free(output_file);
            }
            json_node_free(json_root);
        }
    } else {
        g_warning("Received empty response from AE forbidden list URL");
    }
    
    g_bytes_unref(response_body);
    g_object_unref(session);
}

/**
 * 下载完成后的回调函数（SC变更）
 */
static void on_download_complete_sc_changes(SoupSession *session, GAsyncResult *result, G_GNUC_UNUSED gpointer user_data) {
    GError *error = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, result, &error);

    if (error) {
        g_warning("Failed to download SC forbidden list changes: %s", error->message);
        g_error_free(error);
        g_object_unref(session);
        return;
    }

    gsize size;
    const char *json_content = g_bytes_get_data(response_body, &size);

    if (json_content && size > 0) {
        gchar *data_dir = get_config_data_dir();
        if (!data_dir || !ensure_directory_exists(data_dir)) {
            g_free(data_dir);
            g_bytes_unref(response_body);
            g_object_unref(session);
            return;
        }
        g_free(data_dir);

        JsonNode *json_root = parse_sc_forbidden_changes_json(json_content);
        if (json_root) {
            gchar *output_file = get_output_file_path(SC_FORBIDDEN_CHANGES_FILENAME);
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
 * 后台线程函数（OCG变更下载）
 */
static gpointer download_thread_func_ocg_changes(G_GNUC_UNUSED gpointer data) {
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", OCG_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for OCG forbidden list changes");
        g_object_unref(session);
        return NULL;
    }
    
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of OCG forbidden list changes...");
    
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_ocg_changes,
        NULL
    );
    
    g_object_unref(msg);
    return NULL;
}

/**
 * 后台线程函数（TCG变更下载）
 */
static gpointer download_thread_func_tcg_changes(G_GNUC_UNUSED gpointer data) {
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", TCG_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for TCG forbidden list changes");
        g_object_unref(session);
        return NULL;
    }
    
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of TCG forbidden list changes...");
    
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_tcg_changes,
        NULL
    );
    
    g_object_unref(msg);
    return NULL;
}

/**
 * 后台线程函数（AE变更下载）
 */
static gpointer download_thread_func_ae_changes(G_GNUC_UNUSED gpointer data) {
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", AE_FORBIDDEN_URL);
    
    if (!msg) {
        g_warning("Failed to create HTTP request for AE forbidden list changes");
        g_object_unref(session);
        return NULL;
    }
    
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent", 
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
    
    g_message("Starting background download of AE forbidden list changes...");
    
    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_ae_changes,
        NULL
    );
    
    g_object_unref(msg);
    return NULL;
}

/**
 * 后台线程函数（SC变更下载）
 */
static gpointer download_thread_func_sc_changes(G_GNUC_UNUSED gpointer data) {
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", SC_FORBIDDEN_URL);

    if (!msg) {
        g_warning("Failed to create HTTP request for SC forbidden list changes");
        g_object_unref(session);
        return NULL;
    }

    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    soup_message_headers_append(headers, "User-Agent",
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");

    g_message("Starting background download of SC forbidden list changes...");

    soup_session_send_and_read_async(
        session,
        msg,
        G_PRIORITY_LOW,
        NULL,
        (GAsyncReadyCallback)on_download_complete_sc_changes,
        NULL
    );

    g_object_unref(msg);
    return NULL;
}

/**
 * 启动后台更新OCG禁限变更
 */
void startup_update_ocg_forbidden_changes(void) {
    GThread *thread = g_thread_new("ocg-forbidden-changes-update", download_thread_func_ocg_changes, NULL);
    
    if (thread) {
        g_thread_unref(thread);
        g_message("OCG forbidden changes update started in background");
    } else {
        g_warning("Failed to start OCG forbidden changes update thread");
    }
}

/**
 * 启动后台更新TCG禁限变更
 */
void startup_update_tcg_forbidden_changes(void) {
    GThread *thread = g_thread_new("tcg-forbidden-changes-update", download_thread_func_tcg_changes, NULL);
    
    if (thread) {
        g_thread_unref(thread);
        g_message("TCG forbidden changes update started in background");
    } else {
        g_warning("Failed to start TCG forbidden changes update thread");
    }
}

/**
 * 启动后台更新AE禁限变更
 */
void startup_update_ae_forbidden_changes(void) {
    GThread *thread = g_thread_new("ae-forbidden-changes-update", download_thread_func_ae_changes, NULL);
    
    if (thread) {
        g_thread_unref(thread);
        g_message("AE forbidden changes update started in background");
    } else {
        g_warning("Failed to start AE forbidden changes update thread");
    }
}

/**
 * 启动后台更新SC禁限变更
 */
void startup_update_sc_forbidden_changes(void) {
    GThread *thread = g_thread_new("sc-forbidden-changes-update", download_thread_func_sc_changes, NULL);

    if (thread) {
        g_thread_unref(thread);
        g_message("SC forbidden changes update started in background");
    } else {
        g_warning("Failed to start SC forbidden changes update thread");
    }
}

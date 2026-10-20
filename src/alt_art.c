#include "alt_art.h"
#include "alt_art_index.h"
#include "offline_data.h"
#include "app_path.h"
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#define ALT_ART_CACHE_FILE "altarts.json"
#define ALT_ART_CHOICES_FILE "altart_choices.json"
#define ALT_ART_METADATA_URL "https://cdn.233.momobako.com/ygoimg/jp/metadata"
#define ALT_ART_CARDSET_URL "https://ygocdb.com/api/v0/cardset"
#define ALT_ART_BATCH_SIZE 100

static GMutex state_mutex;
static AltArtIndex *alt_index = NULL;
static GHashTable *alt_choices = NULL; // int base -> int art
static gboolean initialized = FALSE;
static gboolean building = FALSE;
static gboolean rebuild_pending = FALSE;
static AltArtReadyFunc ready_callback = NULL;
static gpointer ready_user_data = NULL;

typedef struct {
    AltArtReadyFunc callback;
    gpointer user_data;
} ReadyNotify;

static gboolean notify_ready_idle(gpointer data) {
    ReadyNotify *notify = (ReadyNotify *)data;
    if (notify) {
        if (notify->callback) notify->callback(notify->user_data);
        g_free(notify);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_ready_notify(void) {
    AltArtReadyFunc callback = NULL;
    gpointer user_data = NULL;
    g_mutex_lock(&state_mutex);
    callback = ready_callback;
    user_data = ready_user_data;
    g_mutex_unlock(&state_mutex);
    if (!callback) return;

    ReadyNotify *notify = g_new0(ReadyNotify, 1);
    notify->callback = callback;
    notify->user_data = user_data;
    g_idle_add(notify_ready_idle, notify);
}

static gchar *get_alt_art_cache_path(void) {
    gchar *cards_dir = offline_get_cards_dir_copy();
    if (!cards_dir) return NULL;
    gchar *path = g_build_filename(cards_dir, ALT_ART_CACHE_FILE, NULL);
    g_free(cards_dir);
    return path;
}

static gchar *get_alt_art_choices_path(void) {
    if (is_portable_mode()) {
        const char *prog_dir = get_program_directory();
        return g_build_filename(prog_dir, ALT_ART_CHOICES_FILE, NULL);
    }
    return g_build_filename(g_get_user_config_dir(), "ygo-deck-builder",
                            ALT_ART_CHOICES_FILE, NULL);
}

static AltArtIndex *load_index_from_file(void) {
    gchar *path = get_alt_art_cache_path();
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return NULL;
    }

    gchar *json = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &json, &len, NULL)) {
        g_free(path);
        return NULL;
    }
    g_free(path);

    AltArtIndex *index = alt_art_index_new();
    GError *error = NULL;
    if (!alt_art_index_load_json(index, json, &error)) {
        g_warning("无法解析异画缓存: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        alt_art_index_free(index);
        index = NULL;
    }
    g_free(json);
    return index;
}

static GHashTable *load_choices_from_file(void) {
    gchar *path = get_alt_art_choices_path();
    GHashTable *choices = NULL;
    if (path && g_file_test(path, G_FILE_TEST_EXISTS)) {
        gchar *json = NULL;
        gsize len = 0;
        if (g_file_get_contents(path, &json, &len, NULL)) {
            choices = alt_art_choices_parse_json(json);
        }
        g_free(json);
    }
    g_free(path);
    if (!choices) choices = g_hash_table_new(g_direct_hash, g_direct_equal);
    return choices;
}


void alt_art_init(void) {
    g_mutex_lock(&state_mutex);
    if (initialized) {
        g_mutex_unlock(&state_mutex);
        return;
    }
    initialized = TRUE;
    if (!alt_choices) alt_choices = load_choices_from_file();
    if (!alt_index) alt_index = load_index_from_file();
    gboolean need_build = !alt_index || alt_art_index_base_count(alt_index) == 0;
    g_mutex_unlock(&state_mutex);

    if (need_build) {
        if (offline_data_exists()) alt_art_rebuild_cache_async();
    } else {
        // 缓存已存在：注册回调的调用方需要立即知道
        schedule_ready_notify();
    }
}

void alt_art_cleanup(void) {
    g_mutex_lock(&state_mutex);
    if (alt_index) {
        alt_art_index_free(alt_index);
        alt_index = NULL;
    }
    if (alt_choices) {
        g_hash_table_unref(alt_choices);
        alt_choices = NULL;
    }
    ready_callback = NULL;
    ready_user_data = NULL;
    initialized = FALSE;
    g_mutex_unlock(&state_mutex);
}

void alt_art_set_ready_callback(AltArtReadyFunc callback, gpointer user_data) {
    gboolean already_ready = FALSE;
    g_mutex_lock(&state_mutex);
    ready_callback = callback;
    ready_user_data = user_data;
    already_ready = callback && alt_index && alt_art_index_base_count(alt_index) > 0;
    g_mutex_unlock(&state_mutex);
    if (already_ready) schedule_ready_notify();
}

gboolean alt_art_has_cache(void) {
    g_mutex_lock(&state_mutex);
    gboolean has = alt_index && alt_art_index_base_count(alt_index) > 0;
    g_mutex_unlock(&state_mutex);
    return has;
}

GArray *alt_art_get_variants(int base_id) {
    if (base_id <= 0) return NULL;
    g_mutex_lock(&state_mutex);
    GArray *variants = alt_index ? alt_art_index_get_variants(alt_index, base_id) : NULL;
    g_mutex_unlock(&state_mutex);
    return variants;
}

int alt_art_get_base_id(int art_id) {
    if (art_id <= 0) return art_id;
    g_mutex_lock(&state_mutex);
    int base = alt_index ? alt_art_index_get_base(alt_index, art_id) : art_id;
    g_mutex_unlock(&state_mutex);
    return base;
}

int alt_art_get_selected(int base_id) {
    if (base_id <= 0) return base_id;
    g_mutex_lock(&state_mutex);
    int selected = base_id;
    if (alt_choices) {
        gpointer value = g_hash_table_lookup(alt_choices, GINT_TO_POINTER(base_id));
        if (value) {
            int candidate = GPOINTER_TO_INT(value);
            if (candidate == base_id) {
                selected = base_id;
            } else if (alt_index &&
                       alt_art_index_get_base(alt_index, candidate) == base_id) {
                selected = candidate;
            }
        }
    }
    g_mutex_unlock(&state_mutex);
    return selected;
}

void alt_art_set_selected(int base_id, int art_id) {
    if (base_id <= 0 || art_id <= 0) return;

    g_mutex_lock(&state_mutex);
    if (!alt_choices) alt_choices = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (art_id == base_id) {
        g_hash_table_remove(alt_choices, GINT_TO_POINTER(base_id));
    } else {
        g_hash_table_insert(alt_choices, GINT_TO_POINTER(base_id), GINT_TO_POINTER(art_id));
    }
    gchar *json = alt_art_choices_to_json(alt_choices);
    g_mutex_unlock(&state_mutex);

    // 写盘放在锁外，避免阻塞其他查询。
    if (json) {
        gchar *path = get_alt_art_choices_path();
        if (path) {
            gchar *dir = g_path_get_dirname(path);
            g_mkdir_with_parents(dir, 0755);
            g_free(dir);

            GError *error = NULL;
            if (!g_file_set_contents(path, json, -1, &error)) {
                g_warning("无法保存异画选择: %s", error ? error->message : "unknown error");
                if (error) g_error_free(error);
            }
            g_free(path);
        }
        g_free(json);
    }
}

static GBytes *http_request(const char *url, const char *post_body) {
    SoupSession *session = soup_session_new();
    soup_session_set_timeout(session, 60);
    SoupMessage *msg = soup_message_new(post_body ? "POST" : "GET", url);
    if (!msg) {
        g_object_unref(session);
        return NULL;
    }
    if (post_body) {
        GBytes *body = g_bytes_new(post_body, strlen(post_body));
        soup_message_set_request_body_from_bytes(msg, "application/json", body);
        g_bytes_unref(body);
    }

    GError *error = NULL;
    GBytes *response = soup_session_send_and_read(session, msg, NULL, &error);
    if (!error && !SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(msg))) {
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP %u %s",
                            soup_message_get_status(msg),
                            soup_message_get_reason_phrase(msg));
    }
    if (error) {
        g_warning("异画数据请求失败 (%s): %s", url, error->message);
        g_error_free(error);
        if (response) {
            g_bytes_unref(response);
            response = NULL;
        }
    }

    g_object_unref(msg);
    g_object_unref(session);
    return response;
}

static gchar *http_get_text(const char *url) {
    GBytes *response = http_request(url, NULL);
    if (!response) return NULL;
    gsize size = 0;
    const gchar *data = g_bytes_get_data(response, &size);
    gchar *text = (data && size > 0) ? g_strndup(data, size) : NULL;
    g_bytes_unref(response);
    return text;
}

static gchar *http_post_text(const char *url, const gchar *body) {
    GBytes *response = http_request(url, body);
    if (!response) return NULL;
    gsize size = 0;
    const gchar *data = g_bytes_get_data(response, &size);
    gchar *text = (data && size > 0) ? g_strndup(data, size) : NULL;
    g_bytes_unref(response);
    return text;
}

static gboolean collect_base_id_cb(JsonObject *card, gpointer user_data) {
    GHashTable *base_ids = (GHashTable *)user_data;
    if (!card || !base_ids) return FALSE;
    if (json_object_has_member(card, "id")) {
        gint64 id = json_object_get_int_member(card, "id");
        if (id > 0 && id <= G_MAXINT) {
            g_hash_table_add(base_ids, GINT_TO_POINTER((int)id));
        }
    }
    return TRUE;
}

static gchar *build_cardset_body(GArray *ids, guint start, guint count) {
    if (!ids || start >= ids->len) return NULL;

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "ids");
    json_builder_begin_array(builder);
    for (guint i = 0; i < count && start + i < ids->len; i++) {
        json_builder_add_int_value(builder, g_array_index(ids, int, start + i));
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);

    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(builder);
    return data;
}

static gboolean save_index_to_file(AltArtIndex *index) {
    if (!index) return FALSE;
    gchar *path = get_alt_art_cache_path();
    if (!path) return FALSE;
    gchar *json = alt_art_index_to_json(index);
    if (!json) {
        g_free(path);
        return FALSE;
    }

    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GError *error = NULL;
    gboolean ok = g_file_set_contents(path, json, -1, &error);
    if (!ok) {
        g_warning("无法保存异画缓存: %s", error ? error->message : "unknown error");
        if (error) g_error_free(error);
    }
    g_free(json);
    g_free(path);
    return ok;
}

static void finish_build(void) {
    gboolean rerun = FALSE;
    g_mutex_lock(&state_mutex);
    building = FALSE;
    rerun = rebuild_pending;
    rebuild_pending = FALSE;
    g_mutex_unlock(&state_mutex);
    if (rerun) alt_art_rebuild_cache_async();
}

static gpointer alt_art_build_thread(gpointer data) {
    (void)data;

    GHashTable *base_ids = g_hash_table_new(g_direct_hash, g_direct_equal);
    offline_foreach_card(NULL, TRUE, collect_base_id_cb, base_ids, 0);
    guint base_count = g_hash_table_size(base_ids);
    if (base_count == 0) {
        g_message("异画缓存生成跳过：没有可用的离线卡片数据");
        g_hash_table_unref(base_ids);
        finish_build();
        return NULL;
    }

    gchar *metadata = http_get_text(ALT_ART_METADATA_URL);
    if (!metadata) {
        g_hash_table_unref(base_ids);
        finish_build();
        return NULL;
    }

    GArray *candidates = alt_art_parse_image_metadata(metadata, base_ids);
    g_free(metadata);
    g_message("异画缓存生成：%u 张本体，%u 个候选卡图 ID", base_count,
              candidates ? candidates->len : 0);

    AltArtIndex *built = alt_art_index_new();
    guint added = 0;
    if (candidates) {
        for (guint start = 0; start < candidates->len; start += ALT_ART_BATCH_SIZE) {
            guint count = MIN((guint)ALT_ART_BATCH_SIZE, candidates->len - start);
            gchar *body = build_cardset_body(candidates, start, count);
            if (!body) continue;
            gchar *response = http_post_text(ALT_ART_CARDSET_URL, body);
            g_free(body);
            if (response) {
                added += alt_art_apply_cardset_json(built, response, base_ids);
                g_free(response);
            }
            if (start + ALT_ART_BATCH_SIZE < candidates->len) {
                g_usleep(100 * 1000);
            }
        }
        g_array_unref(candidates);
    }
    g_hash_table_unref(base_ids);

    if (added == 0) {
        g_warning("异画缓存生成失败：没有解析到异画数据");
        alt_art_index_free(built);
        finish_build();
        return NULL;
    }

    alt_art_index_sort(built);
    save_index_to_file(built);

    g_mutex_lock(&state_mutex);
    AltArtIndex *old = alt_index;
    alt_index = built;
    g_mutex_unlock(&state_mutex);
    alt_art_index_free(old);

    g_message("异画缓存生成完成：%u 个本体，%u 个异画", alt_art_index_base_count(built),
              added);
    schedule_ready_notify();
    finish_build();
    return NULL;
}

void alt_art_rebuild_cache_async(void) {
    g_mutex_lock(&state_mutex);
    if (building) {
        // 已经有任务在跑：记一个待办，结束后再重建一次，避免用旧 cards.json
        rebuild_pending = TRUE;
        g_mutex_unlock(&state_mutex);
        return;
    }
    building = TRUE;
    g_mutex_unlock(&state_mutex);

    GThread *thread = g_thread_new("alt-art-build", alt_art_build_thread, NULL);
    g_thread_unref(thread);
}

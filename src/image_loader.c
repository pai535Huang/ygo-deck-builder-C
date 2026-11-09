#include "image_loader.h"
#include "deck_slot.h"
#include "app_path.h"
#include "pixbuf_utils.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>

// 全局缓存变量
static GHashTable *thumb_cache = NULL;       // 缩略图缓存
static GHashTable *fullsize_cache = NULL;    // 全尺寸图片缓存
static char *cache_dir = NULL;               // 缓存目录路径

// 缓存容量控制：避免内存无限增长
#define THUMB_CACHE_MAX_ENTRIES 700
#define FULLSIZE_CACHE_MAX_ENTRIES 80
static GQueue *thumb_cache_order = NULL;     // key(dup) FIFO，用于淘汰
static GQueue *fullsize_cache_order = NULL;  // key(dup) FIFO，用于淘汰

// 磁盘缓存文件数上限：全尺寸 PNG 每卡一张（数百 KB），
// 万级卡片长期使用可累积至数 GB，启动时按 mtime 从旧到新清理
#define DISK_CACHE_MAX_FILES 6000

// 是否启用内存缓存：默认关闭（只用磁盘缓存），避免内存持续上涨。
// 如需启用，设置环境变量：YGO_ENABLE_MEM_CACHE=1
static gboolean mem_cache_enabled = FALSE;
static gsize mem_cache_enabled_inited = 0;

static gboolean is_mem_cache_enabled(void) {
    if (g_once_init_enter(&mem_cache_enabled_inited)) {
        const char *v = g_getenv("YGO_ENABLE_MEM_CACHE");
        mem_cache_enabled = (v && v[0] == '1');
        g_once_init_leave(&mem_cache_enabled_inited, 1);
    }
    return mem_cache_enabled;
}

#define THUMB_CACHE_SCALE 2

static GdkPixbuf* create_thumb_pixbuf(GdkPixbuf *src, int scale_factor) {
    if (!src) return NULL;
    if (scale_factor < 1) scale_factor = 1;
    const int tw = SLOT_THUMB_W * scale_factor;
    const int th = SLOT_THUMB_H * scale_factor;

    const int w = gdk_pixbuf_get_width(src);
    const int h = gdk_pixbuf_get_height(src);
    if (w == tw && h == th) {
        return g_object_ref(src);
    }
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(src, tw, th, GDK_INTERP_HYPER);
    if (scaled) return scaled;
    return g_object_ref(src);
}

static void evict_cache_if_needed(GHashTable *cache, GQueue *order, guint max_entries) {
    if (!cache || !order) return;
    while (g_hash_table_size(cache) > max_entries) {
        char *old_key = (char*)g_queue_pop_head(order);
        if (!old_key) break;
        // 可能存在重复 key（replace 时再次入队），remove 失败则忽略
        g_hash_table_remove(cache, old_key);
        g_free(old_key);
    }
}

// 淘汰队列入队（带去重）：g_hash_table_replace 覆盖同 key 旧值后，
// 队列里会残留旧 key，同一 key 占两席会导致提前误淘汰
static void cache_order_push(GQueue *order, const char *key) {
    if (!order || !key) return;
    GList *node = g_queue_find_custom(order, key, (GCompareFunc)strcmp);
    if (node) {
        g_free(node->data);
        g_queue_delete_link(order, node);
    }
    g_queue_push_tail(order, g_strdup(key));
}

// 启动时的磁盘缓存清理（后台线程执行）：统计 <card_id>.png 文件数，
// 超过上限时按 mtime 从旧到新删除。
typedef struct {
    char *name;
    gint64 mtime;
} DiskCacheEntry;

static int disk_cache_entry_cmp(gconstpointer a, gconstpointer b) {
    const DiskCacheEntry *ea = (const DiskCacheEntry*)a;
    const DiskCacheEntry *eb = (const DiskCacheEntry*)b;
    return (ea->mtime < eb->mtime) ? -1 : (ea->mtime > eb->mtime) ? 1 : 0;
}

static void disk_cache_cleanup_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    const char *dir = (const char*)task_data;
    if (!dir) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    GDir *gdir = g_dir_open(dir, 0, NULL);
    if (!gdir) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    GArray *entries = g_array_new(FALSE, FALSE, sizeof(DiskCacheEntry));
    const char *name;
    while ((name = g_dir_read_name(gdir)) != NULL) {
        // 只统计本缓存生成的 <card_id>.png
        size_t len = strlen(name);
        if (len <= 4 || !g_str_has_suffix(name, ".png")) continue;
        gboolean digits_only = TRUE;
        for (size_t i = 0; i + 4 < len; i++) {
            if (!g_ascii_isdigit(name[i])) {
                digits_only = FALSE;
                break;
            }
        }
        if (!digits_only) continue;

        gchar *full = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            DiskCacheEntry e = { g_strdup(name), (gint64)st.st_mtime };
            g_array_append_val(entries, e);
        }
        g_free(full);
    }
    g_dir_close(gdir);

    if (entries->len > DISK_CACHE_MAX_FILES) {
        g_array_sort(entries, disk_cache_entry_cmp);
        guint to_delete = entries->len - DISK_CACHE_MAX_FILES;
        for (guint i = 0; i < to_delete; i++) {
            DiskCacheEntry *e = &g_array_index(entries, DiskCacheEntry, i);
            gchar *full = g_build_filename(dir, e->name, NULL);
            g_unlink(full);
            g_free(full);
        }
        g_message("Disk cache cleanup: removed %u oldest image files (limit %d)",
                  to_delete, DISK_CACHE_MAX_FILES);
    }

    for (guint i = 0; i < entries->len; i++) {
        g_free(g_array_index(entries, DiskCacheEntry, i).name);
    }
    g_array_unref(entries);

    g_task_return_boolean(task, TRUE);
}

static void disk_cache_cleanup_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    (void)user_data;
    GError *err = NULL;
    g_task_propagate_boolean(G_TASK(res), &err);
    if (err) g_error_free(err);
}

typedef struct {
    GtkWidget *widget;
    GtkStack *stack;
    SoupSession *session;
    int img_id;
    int scale;
    gboolean thumbnail;
    gboolean local_only;
    char *local_path;
} ImageTarget;

typedef struct {
    SoupSession *session;
    GPtrArray *targets;
    int img_id;
} ImageDownload;

// Target ownership, the request map and the queue belong to the UI thread.
#define MAX_CONCURRENT_DOWNLOADS 6
static GHashTable *pending_downloads;
static GQueue *download_queue;
static guint active_downloads;
static char *thumb_cache_dir;

static int image_expected_id(GtkWidget *widget) {
    int art_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "art_expected_id"));
    return art_id > 0 ? art_id : slot_get_img_id(widget);
}

static gboolean image_target_is_current(const ImageTarget *target) {
    return target->widget && image_expected_id(target->widget) == target->img_id &&
           g_object_get_data(G_OBJECT(target->widget), "loading-image") == target;
}

static void set_image_spinner(GtkStack *stack, gboolean loading) {
    GtkSpinner *spinner = stack ? g_object_get_data(G_OBJECT(stack), "image-spinner") : NULL;
    if (!spinner) return;
    gtk_spinner_set_spinning(spinner, loading);
    gtk_widget_set_visible(GTK_WIDGET(spinner), loading);
}

static void image_target_free(gpointer data) {
    ImageTarget *target = data;
    if (target->widget) {
        if (g_object_get_data(G_OBJECT(target->widget), "loading-image") == target) {
            set_image_spinner(target->stack, FALSE);
            g_object_set_data(G_OBJECT(target->widget), "loading-image", NULL);
        }
        g_object_remove_weak_pointer(G_OBJECT(target->widget), (gpointer *)&target->widget);
    }
    if (target->stack)
        g_object_remove_weak_pointer(G_OBJECT(target->stack), (gpointer *)&target->stack);
    g_object_unref(target->session);
    g_free(target->local_path);
    g_free(target);
}

static void image_download_free(gpointer data) {
    ImageDownload *download = data;
    g_object_unref(download->session);
    g_ptr_array_unref(download->targets);
    g_free(download);
}

static void schedule_disk_cleanup(const char *directory) {
    g_autoptr(GTask) task = g_task_new(NULL, NULL, disk_cache_cleanup_finished, NULL);
    g_task_set_task_data(task, g_strdup(directory), g_free);
    g_task_run_in_thread(task, disk_cache_cleanup_thread);
}

void init_image_cache(void) {
    if (download_queue) return;

    if (is_mem_cache_enabled()) {
        thumb_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
        fullsize_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
        thumb_cache_order = g_queue_new();
        fullsize_cache_order = g_queue_new();
    }

    pending_downloads = g_hash_table_new(g_direct_hash, g_direct_equal);
    download_queue = g_queue_new();
    cache_dir = is_portable_mode()
        ? g_build_filename(get_program_directory(), "img", NULL)
        : g_build_filename(g_get_user_cache_dir(), "ygo-deck-builder", "images", NULL);
    thumb_cache_dir = g_build_filename(cache_dir, "thumbs", NULL);
    g_mkdir_with_parents(thumb_cache_dir, 0755);
    schedule_disk_cleanup(cache_dir);
    schedule_disk_cleanup(thumb_cache_dir);
}

void cleanup_image_cache(void) {
    g_clear_pointer(&thumb_cache, g_hash_table_unref);
    g_clear_pointer(&fullsize_cache, g_hash_table_unref);
    if (thumb_cache_order) g_queue_free_full(thumb_cache_order, g_free);
    if (fullsize_cache_order) g_queue_free_full(fullsize_cache_order, g_free);
    thumb_cache_order = fullsize_cache_order = NULL;
    g_clear_pointer(&pending_downloads, g_hash_table_unref);
    if (download_queue) g_queue_free_full(download_queue, image_download_free);
    download_queue = NULL;

    // Running decode jobs still read the cache paths until process exit.
}

static GdkPixbuf *load_cached_image(const char *directory, int img_id) {
    if (!directory) return NULL;
    g_autofree char *path = g_strdup_printf("%s/%d.png", directory, img_id);
    return gdk_pixbuf_new_from_file(path, NULL);
}

GdkPixbuf *load_from_disk_cache(int img_id) {
    return load_cached_image(cache_dir, img_id);
}

static void save_cached_image(const char *directory, int img_id, GdkPixbuf *pixbuf) {
    if (!directory || !pixbuf) return;
    g_autofree char *path = g_strdup_printf("%s/%d.png", directory, img_id);
    g_autofree char *bytes = NULL;
    gsize size;
    if (gdk_pixbuf_save_to_buffer(pixbuf, &bytes, &size, "png", NULL, NULL))
        g_file_set_contents_full(path, bytes, size, G_FILE_SET_CONTENTS_CONSISTENT, 0644, NULL);
}

static void cache_memory_image(GHashTable *cache, GQueue *order, guint max_entries,
                               int img_id, GdkPixbuf *pixbuf) {
    if (!cache || !pixbuf) return;
    char *key = g_strdup_printf("%d", img_id);
    g_hash_table_replace(cache, key, g_object_ref(pixbuf));
    cache_order_push(order, key);
    evict_cache_if_needed(cache, order, max_entries);
}

static void apply_image(GtkWidget *widget, GtkStack *stack, int img_id, GdkPixbuf *pixbuf) {
    if (GTK_IS_DRAWING_AREA(widget)) {
        g_object_set_data(G_OBJECT(widget), "img_id", GINT_TO_POINTER(img_id));
        slot_set_pixbuf(widget, pixbuf);
    } else if (GTK_IS_PICTURE(widget)) {
        g_autoptr(GdkTexture) texture = pixbuf_utils_texture_from_pixbuf(pixbuf);
        if (!texture) return;
        gtk_picture_set_paintable(GTK_PICTURE(widget), GDK_PAINTABLE(texture));
        g_object_set_data(G_OBJECT(widget), "displayed-image-id", GINT_TO_POINTER(img_id));
    }
    if (stack) gtk_stack_set_visible_child_name(stack, "picture");
}

static void process_download_queue(void);

typedef struct {
    GBytes *bytes;
    int img_id;
} DecodeData;

typedef struct {
    GdkPixbuf *full;
    GdkPixbuf *thumbnail;
} DecodedImage;

static void decoded_image_free(gpointer data) {
    DecodedImage *image = data;
    if (!image) return;
    g_clear_object(&image->full);
    g_clear_object(&image->thumbnail);
    g_free(image);
}

static void decode_image_thread(GTask *task, gpointer source, gpointer task_data,
                                GCancellable *cancellable) {
    (void)source;
    (void)cancellable;
    DecodeData *data = task_data;
    g_autoptr(GInputStream) input = g_memory_input_stream_new_from_bytes(data->bytes);
    GdkPixbuf *full = gdk_pixbuf_new_from_stream(input, NULL, NULL);
    if (!full) {
        g_task_return_pointer(task, NULL, NULL);
        return;
    }

    DecodedImage *image = g_new0(DecodedImage, 1);
    image->full = full;
    image->thumbnail = create_thumb_pixbuf(full, THUMB_CACHE_SCALE);
    save_cached_image(cache_dir, data->img_id, image->full);
    save_cached_image(thumb_cache_dir, data->img_id, image->thumbnail);
    g_task_return_pointer(task, image, decoded_image_free);
}

static void finish_download(ImageDownload *download, DecodedImage *image) {
    if (pending_downloads)
        g_hash_table_remove(pending_downloads, GINT_TO_POINTER(download->img_id));
    if (image) {
        cache_memory_image(fullsize_cache, fullsize_cache_order, FULLSIZE_CACHE_MAX_ENTRIES,
                           download->img_id, image->full);
        cache_memory_image(thumb_cache, thumb_cache_order, THUMB_CACHE_MAX_ENTRIES,
                           download->img_id, image->thumbnail);
        for (guint i = 0; i < download->targets->len; i++) {
            ImageTarget *target = g_ptr_array_index(download->targets, i);
            if (!image_target_is_current(target)) continue;

            GdkPixbuf *pixbuf = target->thumbnail && target->scale <= THUMB_CACHE_SCALE
                ? image->thumbnail : image->full;
            apply_image(target->widget, target->stack, target->img_id, pixbuf);
        }
    }
    decoded_image_free(image);
    image_download_free(download);
    active_downloads--;
    process_download_queue();
}

static void decode_image_finished(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    GTask *task = G_TASK(result);
    DecodeData *data = g_task_get_task_data(task);
    g_bytes_unref(data->bytes);
    finish_download(user_data, g_task_propagate_pointer(task, NULL));
}

static void image_response_cb(GObject *source, GAsyncResult *result, gpointer user_data) {
    ImageDownload *download = user_data;
    SoupSession *session = SOUP_SESSION(source);
    g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(session, result, NULL);
    SoupMessage *message = soup_session_get_async_result_message(session, result);
    if (!bytes || !message || !SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message))) {
        finish_download(download, NULL);
        return;
    }

    DecodeData *data = g_new(DecodeData, 1);
    data->bytes = g_steal_pointer(&bytes);
    data->img_id = download->img_id;
    g_autoptr(GTask) task = g_task_new(NULL, NULL, decode_image_finished, download);
    g_task_set_task_data(task, data, g_free);
    g_task_run_in_thread(task, decode_image_thread);
}

static void process_download_queue(void) {
    while (download_queue && active_downloads < MAX_CONCURRENT_DOWNLOADS) {
        ImageDownload *download = g_queue_pop_head(download_queue);
        if (!download) break;

        for (guint i = download->targets->len; i > 0; i--) {
            if (!image_target_is_current(g_ptr_array_index(download->targets, i - 1)))
                g_ptr_array_remove_index_fast(download->targets, i - 1);
        }
        if (!download->targets->len) {
            g_hash_table_remove(pending_downloads, GINT_TO_POINTER(download->img_id));
            image_download_free(download);
            continue;
        }

        g_autofree char *url = g_strdup_printf("https://cdn.233.momobako.com/ygoimg/jp/%d.webp",
                                               download->img_id);
        g_autoptr(SoupMessage) message = soup_message_new("GET", url);
        active_downloads++;
        soup_session_send_and_read_async(download->session, message, G_PRIORITY_DEFAULT,
                                         NULL, image_response_cb, download);
    }
}

static void enqueue_download(ImageTarget *target) {
    gpointer key = GINT_TO_POINTER(target->img_id);
    ImageDownload *download = g_hash_table_lookup(pending_downloads, key);
    if (!download) {
        download = g_new0(ImageDownload, 1);
        download->img_id = target->img_id;
        download->session = g_object_ref(target->session);
        download->targets = g_ptr_array_new_with_free_func(image_target_free);
        g_hash_table_insert(pending_downloads, key, download);
        g_queue_push_tail(download_queue, download);
    }
    g_ptr_array_add(download->targets, target);
    process_download_queue();
}

static void load_image_thread(GTask *task, gpointer source, gpointer task_data,
                              GCancellable *cancellable) {
    (void)source;
    (void)cancellable;
    const ImageTarget *target = task_data;
    GdkPixbuf *pixbuf = NULL;
    if (target->local_path) {
        if (target->thumbnail)
            pixbuf = gdk_pixbuf_new_from_file_at_scale(target->local_path,
                SLOT_THUMB_W * target->scale, SLOT_THUMB_H * target->scale, TRUE, NULL);
        else
            pixbuf = gdk_pixbuf_new_from_file(target->local_path, NULL);
    } else if (!target->local_only) {
        if (target->thumbnail && target->scale <= THUMB_CACHE_SCALE)
            pixbuf = load_cached_image(thumb_cache_dir, target->img_id);
        if (!pixbuf) {
            g_autoptr(GdkPixbuf) full = load_from_disk_cache(target->img_id);
            if (target->thumbnail && full) {
                if (target->scale <= THUMB_CACHE_SCALE) {
                    pixbuf = create_thumb_pixbuf(full, THUMB_CACHE_SCALE);
                    save_cached_image(thumb_cache_dir, target->img_id, pixbuf);
                } else {
                    pixbuf = create_thumb_pixbuf(full, target->scale);
                }
            } else {
                pixbuf = g_steal_pointer(&full);
            }
        }
    }
    g_task_return_pointer(task, pixbuf, g_object_unref);
}

static void load_image_finished(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    ImageTarget *target = user_data;
    g_autoptr(GdkPixbuf) pixbuf = g_task_propagate_pointer(G_TASK(result), NULL);
    if (image_target_is_current(target)) {
        if (pixbuf) {
            if (!target->local_only) {
                if (!target->thumbnail)
                    cache_memory_image(fullsize_cache, fullsize_cache_order, FULLSIZE_CACHE_MAX_ENTRIES,
                                       target->img_id, pixbuf);
                else if (target->scale <= THUMB_CACHE_SCALE)
                    cache_memory_image(thumb_cache, thumb_cache_order, THUMB_CACHE_MAX_ENTRIES,
                                       target->img_id, pixbuf);
            }
            apply_image(target->widget, target->stack, target->img_id, pixbuf);
        } else if (!target->local_only && pending_downloads) {
            enqueue_download(target);
            return;
        }
    }
    image_target_free(target);
}

void load_card_picture_async(SoupSession *session, GtkWidget *widget, GtkStack *stack,
                             int img_id, gboolean thumbnail, gboolean local_only,
                             const char *local_path) {
    if (!session || !widget || img_id <= 0 || image_expected_id(widget) != img_id ||
        (local_only && !local_path)) return;
    ImageTarget *current = g_object_get_data(G_OBJECT(widget), "loading-image");
    if (current && current->img_id == img_id) return;

    int scale = MAX(1, gtk_widget_get_scale_factor(widget));
    GHashTable *cache = thumbnail && scale <= THUMB_CACHE_SCALE ? thumb_cache : fullsize_cache;
    if (!local_only && cache) {
        char key[32];
        g_snprintf(key, sizeof(key), "%d", img_id);
        GdkPixbuf *pixbuf = g_hash_table_lookup(cache, key);
        if (pixbuf) {
            g_object_set_data(G_OBJECT(widget), "loading-image", NULL);
            set_image_spinner(stack, FALSE);
            apply_image(widget, stack, img_id, pixbuf);
            return;
        }
    }

    ImageTarget *target = g_new0(ImageTarget, 1);
    target->widget = widget;
    target->stack = stack;
    target->img_id = img_id;
    target->thumbnail = thumbnail;
    target->scale = scale;
    target->local_only = local_only;
    target->local_path = g_strdup(local_path);
    target->session = g_object_ref(session);
    g_object_set_data(G_OBJECT(widget), "loading-image", target);
    set_image_spinner(stack, TRUE);
    g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)&target->widget);
    if (stack) g_object_add_weak_pointer(G_OBJECT(stack), (gpointer *)&target->stack);

    g_autoptr(GTask) task = g_task_new(NULL, NULL, load_image_finished, target);
    g_task_set_task_data(task, target, NULL);
    g_task_run_in_thread(task, load_image_thread);
}

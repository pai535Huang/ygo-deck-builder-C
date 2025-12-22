#include "image_loader.h"
#include "app_path.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

// 全局缓存变量
static GHashTable *thumb_cache = NULL;       // 缩略图缓存
static GHashTable *fullsize_cache = NULL;    // 全尺寸图片缓存
static char *cache_dir = NULL;               // 缓存目录路径
static GHashTable *pending_downloads = NULL; // 待处理下载
static GMutex cache_mutex;                   // 缓存互斥锁

// 全局取消标志（使用计数器而不是GCancellable列表）
// 每次开始新搜索时递增，回调检查这个值来判断是否应该继续
static guint64 global_cancel_generation = 0;
static GMutex cancel_generation_mutex;

// 下载队列管理
#define MAX_CONCURRENT_DOWNLOADS 6  // 最大并发下载数
static GQueue *download_queue = NULL;        // 待下载队列
static int active_downloads = 0;             // 当前活跃下载数
static GMutex download_queue_mutex;          // 队列互斥锁

// 内部结构：解码任务数据
typedef struct {
    GBytes *image_data;
    ImageLoadCtx *ctx;
    guint64 cancel_generation;  // 创建时的取消代次
} DecodeTaskData;

// 前向声明
static void decode_task_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable);
static void decode_task_finished(GObject *source, GAsyncResult *res, gpointer user_data);
static void image_response_cb(GObject *source, GAsyncResult *res, gpointer user_data);
static void process_download_queue(SoupSession *session);
static void start_download(SoupSession *session, ImageLoadCtx *ctx);

void init_image_cache(void) {
    g_mutex_init(&cache_mutex);
    g_mutex_init(&cancel_generation_mutex);
    g_mutex_init(&download_queue_mutex);
    
    thumb_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);
    fullsize_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_object_unref);
    pending_downloads = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    download_queue = g_queue_new();
    
    // 创建缓存目录
    if (is_portable_mode()) {
        // 便携模式：程序所在目录下的 img 子目录
        const char *prog_dir = get_program_directory();
        cache_dir = g_build_filename(prog_dir, "img", NULL);
    } else {
        // 系统安装模式：使用 XDG_CACHE_HOME
        const char *cache_home = g_get_user_cache_dir();
        cache_dir = g_build_filename(cache_home, "ygo-deck-builder", "images", NULL);
    }
    g_mkdir_with_parents(cache_dir, 0755);
}

void cleanup_image_cache(void) {
    g_mutex_lock(&cache_mutex);
    if (thumb_cache) {
        g_hash_table_destroy(thumb_cache);
        thumb_cache = NULL;
    }
    if (fullsize_cache) {
        g_hash_table_destroy(fullsize_cache);
        fullsize_cache = NULL;
    }
    if (pending_downloads) {
        g_hash_table_destroy(pending_downloads);
        pending_downloads = NULL;
    }
    g_free(cache_dir);
    cache_dir = NULL;
    g_mutex_unlock(&cache_mutex);
    
    g_mutex_lock(&download_queue_mutex);
    if (download_queue) {
        // 清理队列中的所有上下文
        gpointer item;
        while ((item = g_queue_pop_head(download_queue)) != NULL) {
            ImageLoadCtx *ctx = (ImageLoadCtx*)item;
            if (ctx->target) {
                g_object_remove_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
            }
            if (ctx->stack) {
                g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
            }
            g_free(ctx->url);
            g_free(ctx);
        }
        g_queue_free(download_queue);
        download_queue = NULL;
    }
    g_mutex_unlock(&download_queue_mutex);
    
    g_mutex_clear(&cache_mutex);
    g_mutex_clear(&cancel_generation_mutex);
    g_mutex_clear(&download_queue_mutex);
}

GdkPixbuf* load_from_disk_cache(int card_id) {
    if (!cache_dir) return NULL;
    char *filename = g_strdup_printf("%s/%d.png", cache_dir, card_id);
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(filename, &err);
    if (err) {
        g_error_free(err);
        pb = NULL;
    }
    g_free(filename);
    return pb;
}

void save_to_disk_cache(int card_id, GdkPixbuf *pixbuf) {
    if (!cache_dir || !pixbuf) return;
    char *filename = g_strdup_printf("%s/%d.png", cache_dir, card_id);
    GError *err = NULL;
    gdk_pixbuf_save(pixbuf, filename, "png", &err, NULL);
    if (err) {
        g_error_free(err);
    }
    g_free(filename);
}

GdkPixbuf* get_thumb_from_cache(int card_id) {
    GdkPixbuf *result = NULL;
    g_mutex_lock(&cache_mutex);
    if (thumb_cache) {
        char key[32];
        g_snprintf(key, sizeof(key), "%d", card_id);
        result = (GdkPixbuf*)g_hash_table_lookup(thumb_cache, key);
    }
    g_mutex_unlock(&cache_mutex);
    return result;
}

GdkPixbuf* get_fullsize_from_cache(int card_id) {
    GdkPixbuf *result = NULL;
    g_mutex_lock(&cache_mutex);
    if (fullsize_cache) {
        char key[32];
        g_snprintf(key, sizeof(key), "%d", card_id);
        result = (GdkPixbuf*)g_hash_table_lookup(fullsize_cache, key);
    }
    g_mutex_unlock(&cache_mutex);
    return result;
}

// 获取当前取消代次（用于在开始异步操作前记录）
guint64 get_cancel_generation(void) {
    guint64 gen;
    g_mutex_lock(&cancel_generation_mutex);
    gen = global_cancel_generation;
    g_mutex_unlock(&cancel_generation_mutex);
    return gen;
}

// 检查是否已被取消（比较当前代次与记录的代次）
gboolean is_cancelled(guint64 recorded_generation) {
    guint64 current;
    g_mutex_lock(&cancel_generation_mutex);
    current = global_cancel_generation;
    g_mutex_unlock(&cancel_generation_mutex);
    return recorded_generation != current;
}

void cancel_all_pending(void) {
    // 仅递增取消代次，不实际取消任何GCancellable
    // 这样libsoup的请求会自然完成，但回调会检测到代次变化并跳过处理
    g_mutex_lock(&cancel_generation_mutex);
    global_cancel_generation++;
    g_mutex_unlock(&cancel_generation_mutex);
    
    // 清空下载队列（这些还没开始下载）
    g_mutex_lock(&download_queue_mutex);
    if (download_queue) {
        gpointer item;
        while ((item = g_queue_pop_head(download_queue)) != NULL) {
            ImageLoadCtx *ctx = (ImageLoadCtx*)item;
            if (ctx->target) {
                g_object_remove_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
            }
            if (ctx->stack) {
                g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
            }
            g_free(ctx->url);
            g_free(ctx);
        }
    }
    g_mutex_unlock(&download_queue_mutex);
    
    // 不要清空 pending_downloads！
    // 活跃的下载回调仍然会访问这个哈希表
    // 让回调函数自己清理（它们会检测到代次变化并跳过处理UI，但仍会从哈希表中移除条目）
}

const char* get_cache_dir(void) {
    return cache_dir;
}

// 后台线程：解码图片
static void decode_task_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    
    if (!task_data) {
        g_task_return_pointer(task, NULL, NULL);
        return;
    }
    
    DecodeTaskData *data = (DecodeTaskData*)task_data;
    
    // 检查是否已被取消
    if (is_cancelled(data->cancel_generation)) {
        g_task_return_pointer(task, NULL, NULL);
        return;
    }
    
    GdkPixbuf *pixbuf = NULL;
    if (data->image_data) {
        gsize size;
        const guint8 *bytes_data = g_bytes_get_data(data->image_data, &size);
        
        if (size > 0 && !is_cancelled(data->cancel_generation)) {
            GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
            GError *err = NULL;
            if (gdk_pixbuf_loader_write(loader, bytes_data, size, &err)) {
                gdk_pixbuf_loader_close(loader, NULL);
                pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
                if (pixbuf) {
                    pixbuf = g_object_ref(pixbuf);
                }
            } else {
                if (err) g_error_free(err);
            }
            g_object_unref(loader);
        }
    }
    
    if (pixbuf) {
        g_task_return_pointer(task, pixbuf, (GDestroyNotify)g_object_unref);
    } else {
        g_task_return_pointer(task, NULL, NULL);
    }
}

// 解码完成回调
static void decode_task_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    DecodeTaskData *data = (DecodeTaskData*)user_data;
    
    if (!data || !data->ctx) {
        g_warning("decode_task_finished: data or ctx is NULL!");
        if (data) {
            if (data->image_data) g_bytes_unref(data->image_data);
            g_free(data);
        }
        return;
    }
    
    ImageLoadCtx *ctx = data->ctx;
    GTask *task = G_TASK(res);
    GError *err = NULL;
    GdkPixbuf *pixbuf = (GdkPixbuf*)g_task_propagate_pointer(task, &err);
    
    if (err) {
        g_error_free(err);
    }
    
    // 使用代次检查代替 g_cancellable_is_cancelled
    if (pixbuf && !is_cancelled(data->cancel_generation) && ctx->target) {
        // 额外的类型检查以确保对象仍然有效
        if (GTK_IS_WIDGET(ctx->target) && GTK_IS_DRAWING_AREA(ctx->target)) {
            // 目标是DrawingArea（卡组槽位）
            g_object_set_data_full(G_OBJECT(ctx->target), "pixbuf", g_object_ref(pixbuf), 
                                   (GDestroyNotify)g_object_unref);
            g_object_set_data(G_OBJECT(ctx->target), "cached_surface", NULL);
            gtk_widget_queue_draw(ctx->target);
            
            if (ctx->stack && GTK_IS_STACK(ctx->stack)) {
                gtk_stack_set_visible_child_name(ctx->stack, "picture");
            }
            
            // 添加到缩略图缓存
            if (ctx->add_to_thumb_cache && ctx->cache_id > 0) {
                g_mutex_lock(&cache_mutex);
                if (!thumb_cache) {
                    thumb_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 
                                                       (GDestroyNotify)g_object_unref);
                }
                char *key = g_strdup_printf("%d", ctx->cache_id);
                g_hash_table_replace(thumb_cache, key, g_object_ref(pixbuf));
                g_mutex_unlock(&cache_mutex);
            }
            
            // 保存到磁盘缓存和全尺寸缓存
            if (ctx->cache_id > 0) {
                g_mutex_lock(&cache_mutex);
                char *key = g_strdup_printf("%d", ctx->cache_id);
                if (!fullsize_cache) {
                    fullsize_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, 
                                                          (GDestroyNotify)g_object_unref);
                }
                g_hash_table_replace(fullsize_cache, g_strdup(key), g_object_ref(pixbuf));
                g_mutex_unlock(&cache_mutex);
                save_to_disk_cache(ctx->cache_id, pixbuf);
                g_free(key);
            }
        } else if (GTK_IS_PICTURE(ctx->target)) {
            // 目标是GtkPicture（左侧预览）
            GdkTexture *tex = gdk_texture_new_for_pixbuf(pixbuf);
            if (tex) {
                gtk_picture_set_paintable(GTK_PICTURE(ctx->target), GDK_PAINTABLE(tex));
                if (ctx->stack && GTK_IS_STACK(ctx->stack)) {
                    gtk_stack_set_visible_child_name(ctx->stack, "picture");
                }
                g_object_unref(tex);
            }
        }
    }
    
    // 处理同URL的其他等待请求
    // 注意：即使被取消也需要清理等待上下文，只是不更新UI
    gboolean cancelled = is_cancelled(data->cancel_generation);
    if (ctx->url) {
        g_mutex_lock(&cache_mutex);
        GPtrArray *waiting = (GPtrArray*)g_hash_table_lookup(pending_downloads, ctx->url);
        if (waiting) {
            for (guint i = 0; i < waiting->len; i++) {
                ImageLoadCtx *waiting_ctx = (ImageLoadCtx*)g_ptr_array_index(waiting, i);
                if (!waiting_ctx) continue;
                
                // 只有在未取消且有有效pixbuf时才更新UI
                if (!cancelled && pixbuf && waiting_ctx->target) {
                    // 额外的类型检查以确保对象仍然有效
                    if (GTK_IS_WIDGET(waiting_ctx->target) && GTK_IS_DRAWING_AREA(waiting_ctx->target)) {
                        g_object_set_data_full(G_OBJECT(waiting_ctx->target), "pixbuf", 
                                              g_object_ref(pixbuf), (GDestroyNotify)g_object_unref);
                        g_object_set_data(G_OBJECT(waiting_ctx->target), "cached_surface", NULL);
                        gtk_widget_queue_draw(waiting_ctx->target);
                        if (waiting_ctx->stack && GTK_IS_WIDGET(waiting_ctx->stack) && GTK_IS_STACK(waiting_ctx->stack)) {
                            gtk_stack_set_visible_child_name(waiting_ctx->stack, "picture");
                        }
                    } else if (GTK_IS_WIDGET(waiting_ctx->target) && GTK_IS_PICTURE(waiting_ctx->target)) {
                        GdkTexture *tex = gdk_texture_new_for_pixbuf(pixbuf);
                        if (tex) {
                            gtk_picture_set_paintable(GTK_PICTURE(waiting_ctx->target), 
                                                     GDK_PAINTABLE(tex));
                            g_object_unref(tex);
                        }
                        if (waiting_ctx->stack && GTK_IS_WIDGET(waiting_ctx->stack) && GTK_IS_STACK(waiting_ctx->stack)) {
                            gtk_stack_set_visible_child_name(waiting_ctx->stack, "picture");
                        }
                    }
                }
                
                // 无论是否取消都要清理等待上下文
                if (waiting_ctx->target) {
                    g_object_remove_weak_pointer(G_OBJECT(waiting_ctx->target), 
                                                (gpointer*)&waiting_ctx->target);
                }
                if (waiting_ctx->stack) {
                    g_object_remove_weak_pointer(G_OBJECT(waiting_ctx->stack), 
                                                (gpointer*)&waiting_ctx->stack);
                }
                g_free(waiting_ctx->url);
                g_free(waiting_ctx);
            }
            g_hash_table_remove(pending_downloads, ctx->url);
        }
        g_mutex_unlock(&cache_mutex);
    }
    
    if (pixbuf) g_object_unref(pixbuf);
    if (data->image_data) g_bytes_unref(data->image_data);
    
    // 保存session用于处理队列
    SoupSession *session = NULL;
    if (ctx->target && GTK_IS_WIDGET(ctx->target)) {
        // 尝试从target获取session（如果之前保存过）
        // 额外检查 GTK_IS_WIDGET 确保对象仍然有效
        session = (SoupSession*)g_object_get_data(G_OBJECT(ctx->target), "_loader_session");
    }
    
    // 移除弱引用（只在指针非NULL且对象仍然有效时调用）
    // 注意：需要在使用session之前清理，因为session可能来自target
    if (ctx->target && G_IS_OBJECT(ctx->target)) {
        g_object_remove_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
    }
    if (ctx->stack && G_IS_OBJECT(ctx->stack)) {
        g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
    }
    
    g_free(ctx->url);
    g_free(ctx);
    g_free(data);
    
    // 下载完成，处理队列中的下一个请求
    g_mutex_lock(&download_queue_mutex);
    active_downloads--;
    g_mutex_unlock(&download_queue_mutex);
    
    if (session) {
        process_download_queue(session);
    }
}

// HTTP响应回调
static void image_response_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    SoupSession *session = SOUP_SESSION(source);
    ImageLoadCtx *ctx = (ImageLoadCtx*)user_data;
    
    if (!ctx) {
        g_warning("image_response_cb: ctx is NULL!");
        return;
    }
    
    // 检查是否已取消（代次变化）
    gboolean cancelled = is_cancelled(ctx->cancel_generation);
    
    GError *err = NULL;
    GInputStream *in = soup_session_send_finish(session, res, &err);
    
    if (!in || cancelled) {
        if (err) {
            // 不记录取消的错误
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_warning("图片下载失败 %s: %s", ctx->url ? ctx->url : "unknown", err->message);
            }
            g_error_free(err);
        }
        if (in) {
            g_input_stream_close(in, NULL, NULL);
            g_object_unref(in);
        }
        
        // 从待处理下载中移除，并清理所有等待的上下文
        if (ctx->url) {
            g_mutex_lock(&cache_mutex);
            if (pending_downloads) {
                GPtrArray *waiting = (GPtrArray*)g_hash_table_lookup(pending_downloads, ctx->url);
                if (waiting) {
                    // 清理所有等待的上下文
                    for (guint i = 0; i < waiting->len; i++) {
                        ImageLoadCtx *waiting_ctx = (ImageLoadCtx*)g_ptr_array_index(waiting, i);
                        if (waiting_ctx) {
                            if (waiting_ctx->target) {
                                g_object_remove_weak_pointer(G_OBJECT(waiting_ctx->target), 
                                                            (gpointer*)&waiting_ctx->target);
                            }
                            if (waiting_ctx->stack) {
                                g_object_remove_weak_pointer(G_OBJECT(waiting_ctx->stack), 
                                                            (gpointer*)&waiting_ctx->stack);
                            }
                            g_free(waiting_ctx->url);
                            g_free(waiting_ctx);
                        }
                    }
                }
                g_hash_table_remove(pending_downloads, ctx->url);
            }
            g_mutex_unlock(&cache_mutex);
        }
        
        // 清理弱引用（检查对象是否仍然有效）
        if (ctx->target && G_IS_OBJECT(ctx->target)) {
            g_object_remove_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
        }
        if (ctx->stack && G_IS_OBJECT(ctx->stack)) {
            g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
        }
        g_free(ctx->url);
        g_free(ctx);
        
        // 下载失败，减少计数并处理队列
        g_mutex_lock(&download_queue_mutex);
        active_downloads--;
        g_mutex_unlock(&download_queue_mutex);
        process_download_queue(session);
        return;
    }
    
    // 读取整个流到内存
    GByteArray *ba = g_byte_array_new();
    guint8 buf[8192];
    gssize n;
    while ((n = g_input_stream_read(in, buf, sizeof(buf), NULL, &err)) > 0) {
        g_byte_array_append(ba, buf, (guint)n);
    }
    g_input_stream_close(in, NULL, NULL);
    g_object_unref(in);
    
    if (err) {
        g_error_free(err);
        g_byte_array_free(ba, TRUE);
        
        // 从待处理下载中移除，并清理所有等待的上下文
        if (ctx->url) {
            g_mutex_lock(&cache_mutex);
            if (pending_downloads) {
                GPtrArray *waiting = (GPtrArray*)g_hash_table_lookup(pending_downloads, ctx->url);
                if (waiting) {
                    // 清理所有等待的上下文
                    for (guint i = 0; i < waiting->len; i++) {
                        ImageLoadCtx *waiting_ctx = (ImageLoadCtx*)g_ptr_array_index(waiting, i);
                        if (waiting_ctx) {
                            if (waiting_ctx->target) {
                                g_object_remove_weak_pointer(G_OBJECT(waiting_ctx->target), 
                                                            (gpointer*)&waiting_ctx->target);
                            }
                            if (waiting_ctx->stack) {
                                g_object_remove_weak_pointer(G_OBJECT(waiting_ctx->stack), 
                                                            (gpointer*)&waiting_ctx->stack);
                            }
                            g_free(waiting_ctx->url);
                            g_free(waiting_ctx);
                        }
                    }
                }
                g_hash_table_remove(pending_downloads, ctx->url);
            }
            g_mutex_unlock(&cache_mutex);
        }
        if (ctx->target && G_IS_OBJECT(ctx->target)) {
            g_object_remove_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
        }
        if (ctx->stack && G_IS_OBJECT(ctx->stack)) {
            g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
        }
        g_free(ctx->url);
        g_free(ctx);
        
        // 下载失败，减少计数并处理队列
        g_mutex_lock(&download_queue_mutex);
        active_downloads--;
        g_mutex_unlock(&download_queue_mutex);
        process_download_queue(session);
        return;
    }
    
    // 创建GBytes
    GBytes *image_data = g_bytes_new_take(ba->data, ba->len);
    g_byte_array_free(ba, FALSE);  // 不释放data，已交给GBytes
    
    // 启动解码任务
    DecodeTaskData *data = g_new0(DecodeTaskData, 1);
    data->image_data = image_data;
    data->ctx = ctx;
    data->cancel_generation = ctx->cancel_generation;  // 继承上下文的取消代次
    
    GTask *task = g_task_new(NULL, NULL, decode_task_finished, data);
    g_task_set_task_data(task, data, NULL);
    g_task_run_in_thread(task, decode_task_thread);
    g_object_unref(task);
}

// 开始一个下载
static void start_download(SoupSession *session, ImageLoadCtx *ctx) {
    if (!session || !ctx || !ctx->url) return;
    
    // 保存session到target以便后续使用
    if (ctx->target && GTK_IS_WIDGET(ctx->target)) {
        g_object_set_data(G_OBJECT(ctx->target), "_loader_session", session);
    }
    
    // 创建HTTP消息（检查返回值，URL可能无效）
    SoupMessage *msg = soup_message_new("GET", ctx->url);
    if (!msg) {
        g_warning("无效的URL，无法创建soup消息: %s", ctx->url);
        // 清理上下文
        if (ctx->target && G_IS_OBJECT(ctx->target)) {
            g_object_remove_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
        }
        if (ctx->stack && G_IS_OBJECT(ctx->stack)) {
            g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
        }
        g_free(ctx->url);
        g_free(ctx);
        
        // 减少活跃下载计数
        g_mutex_lock(&download_queue_mutex);
        active_downloads--;
        g_mutex_unlock(&download_queue_mutex);
        
        // 继续处理队列
        process_download_queue(session);
        return;
    }
    
    // 发起HTTP请求（不使用GCancellable，让请求自然完成，回调中检查代次来决定是否处理结果）
    soup_session_send_async(session, msg, G_PRIORITY_DEFAULT, NULL, image_response_cb, ctx);
    g_object_unref(msg);  // soup_session_send_async 会内部增加引用
}

// 处理下载队列
static void process_download_queue(SoupSession *session) {
    if (!session) return;
    
    g_mutex_lock(&download_queue_mutex);
    
    // 启动尽可能多的下载（不超过最大并发数）
    while (active_downloads < MAX_CONCURRENT_DOWNLOADS) {
        ImageLoadCtx *ctx = (ImageLoadCtx*)g_queue_pop_head(download_queue);
        if (!ctx) break;  // 队列为空
        
        // 检查目标是否仍然有效
        if (!ctx->target) {
            // 目标已销毁（弱指针自动设为NULL），清理上下文
            // 注意：不要调用 g_object_remove_weak_pointer，因为对象已不存在
            if (ctx->stack) {
                g_object_remove_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
            }
            g_free(ctx->url);
            g_free(ctx);
            continue;
        }
        
        active_downloads++;
        g_mutex_unlock(&download_queue_mutex);
        
        // 启动下载
        start_download(session, ctx);
        
        g_mutex_lock(&download_queue_mutex);
    }
    
    g_mutex_unlock(&download_queue_mutex);
}

void load_image_async(SoupSession *session, const char *url, ImageLoadCtx *ctx) {
    if (!session || !url || !ctx) return;
    
    // 记录当前取消代次
    ctx->cancel_generation = get_cancel_generation();
    
    // 添加弱引用（必须在任何使用ctx之前设置）
    if (ctx->target) {
        g_object_add_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
    }
    if (ctx->stack) {
        g_object_add_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
    }
    
    // 检查是否已有相同URL的下载
    g_mutex_lock(&cache_mutex);
    GPtrArray *waiting = (GPtrArray*)g_hash_table_lookup(pending_downloads, url);
    if (waiting) {
        // 已有下载进行中，加入等待队列
        g_ptr_array_add(waiting, ctx);
        g_mutex_unlock(&cache_mutex);
        return;
    }
    
    // 创建新的等待队列
    waiting = g_ptr_array_new();
    g_hash_table_insert(pending_downloads, g_strdup(url), waiting);
    g_mutex_unlock(&cache_mutex);
    
    // 加入下载队列或立即开始下载
    g_mutex_lock(&download_queue_mutex);
    if (active_downloads < MAX_CONCURRENT_DOWNLOADS) {
        active_downloads++;
        g_mutex_unlock(&download_queue_mutex);
        start_download(session, ctx);
    } else {
        // 队列已满，加入等待队列
        g_queue_push_tail(download_queue, ctx);
        g_mutex_unlock(&download_queue_mutex);
    }
}

#ifndef IMAGE_CACHE_H
#define IMAGE_CACHE_H

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libsoup/soup.h>

/**
 * 图片加载上下文
 */
typedef struct {
    GtkStack *stack;
    GtkWidget *target; // GtkPicture (left) or GtkDrawingArea (middle/right)
    gboolean scale_to_thumb; // TRUE for small thumbnails
    int cache_id; // >0 when we want to cache row thumbnails by card id
    gboolean add_to_thumb_cache; // TRUE to store into thumb cache
    char *url; // URL being loaded (for pending download tracking)
} ImageLoadCtx;

/**
 * 初始化图片缓存系统
 */
void init_image_cache(void);

/**
 * 从内存缓存获取缩略图
 * @param card_id 卡片ID
 * @return 缓存的GdkPixbuf，如果不存在返回NULL（不需要释放）
 */
GdkPixbuf* get_thumb_cache(int card_id);

/**
 * 存储缩略图到内存缓存
 * @param card_id 卡片ID
 * @param pixbuf 要缓存的图片
 */
void set_thumb_cache(int card_id, GdkPixbuf *pixbuf);

/**
 * 从内存缓存获取全尺寸图片
 * @param card_id 卡片ID
 * @return 缓存的GdkPixbuf，如果不存在返回NULL（不需要释放）
 */
GdkPixbuf* get_fullsize_cache(int card_id);

/**
 * 存储全尺寸图片到内存缓存
 * @param card_id 卡片ID
 * @param pixbuf 要缓存的图片
 */
void set_fullsize_cache(int card_id, GdkPixbuf *pixbuf);

/**
 * 从磁盘缓存加载图片
 * @param card_id 卡片ID
 * @return 加载的GdkPixbuf，需要调用者释放，失败返回NULL
 */
GdkPixbuf* load_from_disk_cache(int card_id);

/**
 * 保存图片到磁盘缓存
 * @param card_id 卡片ID
 * @param pixbuf 要保存的图片
 */
void save_to_disk_cache(int card_id, GdkPixbuf *pixbuf);

/**
 * 添加可取消对象到待取消列表
 * @param cancellable 可取消对象
 */
void add_pending_cancel(GCancellable *cancellable);

/**
 * 取消所有待处理的操作
 */
void cancel_all_pending(void);

/**
 * 异步加载图片
 * @param session SoupSession
 * @param url 图片URL
 * @param ctx 加载上下文（会被函数接管和释放）
 */
void load_image_async(SoupSession *session, const char *url, ImageLoadCtx *ctx);

/**
 * 检查URL是否正在下载中
 * @param url 图片URL
 * @return TRUE表示正在下载
 */
gboolean is_url_downloading(const char *url);

/**
 * 添加等待队列到正在下载的URL
 * @param url 图片URL
 * @param ctx 等待的上下文
 */
void add_waiting_context(const char *url, ImageLoadCtx *ctx);

#endif // IMAGE_CACHE_H

#ifndef IMAGE_LOADER_H
#define IMAGE_LOADER_H

#include <gtk/gtk.h>
#include <libsoup/soup.h>

/**
 * 图片加载上下文结构
 * 包含加载图片所需的所有信息
 */
typedef struct {
    GtkStack *stack;           // 可选：加载完成后切换到picture页面
    GtkWidget *target;         // 目标控件：GtkPicture或GtkDrawingArea
    gboolean scale_to_thumb;   // TRUE表示缩略图，FALSE表示全尺寸
    int cache_id;              // >0时启用缓存（使用卡片ID）
    gboolean add_to_thumb_cache; // TRUE时添加到缩略图缓存
    char *url;                 // 正在加载的URL（网络）或file_path（本地）
    gboolean is_local_file;    // TRUE表示从本地文件加载，FALSE表示从网络加载
    guint64 cancel_generation; // 创建时的取消代次，用于检测是否应该取消
} ImageLoadCtx;

/**
 * 初始化图片缓存系统
 * 必须在使用其他函数前调用
 */
void init_image_cache(void);

/**
 * 清理图片缓存系统
 * 释放所有缓存资源
 */
void cleanup_image_cache(void);

/**
 * 从磁盘缓存加载图片
 * @param card_id 卡片ID
 * @return GdkPixbuf指针，调用者需要unref，失败返回NULL
 */
GdkPixbuf* load_from_disk_cache(int card_id);

/**
 * 保存图片到磁盘缓存
 * @param card_id 卡片ID
 * @param pixbuf 要保存的图片
 */
void save_to_disk_cache(int card_id, GdkPixbuf *pixbuf);

/**
 * 从内存缓存获取缩略图
 * @param card_id 卡片ID
 * @return GdkPixbuf指针，不需要unref，失败返回NULL
 */
GdkPixbuf* get_thumb_from_cache(int card_id);

/**
 * 添加缩略图到内存缓存
 * @param card_id 卡片ID
 * @param pixbuf 缩略图（会增加引用计数）
 */
void add_thumb_to_cache(int card_id, GdkPixbuf *pixbuf);

/**
 * 从内存缓存获取全尺寸图片
 * @param card_id 卡片ID
 * @return GdkPixbuf指针，不需要unref，失败返回NULL
 */
GdkPixbuf* get_fullsize_from_cache(int card_id);

/**
 * 获取当前取消代次
 * @return 当前的取消代次号
 */
guint64 get_cancel_generation(void);

/**
 * 检查是否已被取消
 * @param recorded_generation 记录的取消代次
 * @return TRUE表示已被取消
 */
gboolean is_cancelled(guint64 recorded_generation);

/**
 * 取消所有待处理的图片加载
 * 用于搜索刷新时清理旧请求
 */
void cancel_all_pending(void);

/**
 * 异步加载图片
 * @param session libsoup会话
 * @param url 图片URL
 * @param ctx 加载上下文（函数会接管所有权）
 */
void load_image_async(SoupSession *session, const char *url, ImageLoadCtx *ctx);

/**
 * 获取缓存目录路径
 * @return 缓存目录的完整路径，不要释放
 */
const char* get_cache_dir(void);

#endif // IMAGE_LOADER_H

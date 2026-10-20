#ifndef ALT_ART_H
#define ALT_ART_H

#include <glib.h>

/**
 * 初始化异画数据：加载本地缓存与用户选择。
 * 若缓存不存在且离线卡片数据可用，会在后台线程生成缓存。
 * 可重复调用；只在首次调用时执行加载。
 */
void alt_art_init(void);

/**
 * 释放异画数据（主要供退出/测试使用）。
 */
void alt_art_cleanup(void);

/** 异画缓存就绪回调（在主线程中调用）。 */
typedef void (*AltArtReadyFunc)(gpointer user_data);

/**
 * 注册“异画缓存已就绪”回调；如果注册时缓存已存在，会立即异步触发一次。
 * 传 NULL 取消注册。
 */
void alt_art_set_ready_callback(AltArtReadyFunc callback, gpointer user_data);

/** 是否已有可用的异画映射。 */
gboolean alt_art_has_cache(void);

/**
 * 获取某张卡的完整版本列表（本体在前，异画按 ID 升序）。
 * @return 新的 GArray（元素 int），调用者需 g_array_unref；没有异画返回 NULL
 */
GArray *alt_art_get_variants(int base_id);

/** 异画 ID -> 本体 ID；不是已知异画时原样返回。 */
int alt_art_get_base_id(int art_id);

/**
 * 获取该卡当前默认版本：用户选择过且仍然有效则返回异画 ID，否则返回本体 ID。
 */
int alt_art_get_selected(int base_id);

/**
 * 记住该卡的默认版本。art_id == base_id 表示恢复原画（删除记录）。
 */
void alt_art_set_selected(int base_id, int art_id);

/**
 * 后台重新生成异画缓存（离线数据更新后调用）。
 * 同一时间只允许一个生成任务。
 */
void alt_art_rebuild_cache_async(void);

#endif // ALT_ART_H

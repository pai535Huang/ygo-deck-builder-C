#ifndef OFFLINE_DATA_H
#define OFFLINE_DATA_H

#include <glib.h>
#include <json-glib/json-glib.h>

/**
 * 下载并处理离线卡片数据
 * 此函数会在后台线程中执行，不会阻塞主线程
 * 从 https://ygocdb.com/api/v0/cards.zip 下载数据
 * 保存到程序根目录下的 data 目录
 * 解压到根目录的 cards 目录
 * @param callback 完成后的回调函数，在主线程中调用
 * @param user_data 传递给回调函数的用户数据
 */
void download_offline_data(GSourceFunc callback, gpointer user_data);

/**
 * 检查离线数据是否存在
 * @return TRUE如果数据已下载，否则FALSE
 */
gboolean offline_data_exists(void);

/**
 * 清理离线数据（删除 cards 目录下的所有文件）
 * @return TRUE如果清理成功，否则FALSE
 */
gboolean clear_offline_data(void);

/**
 * 检查离线数据更新
 * 如果本地数据存在，会检查远程 MD5 是否与本地 MD5 不同
 * 如果不同则自动下载更新
 * 此函数会在后台线程中执行，不会阻塞主线程
 * @param callback 完成后的回调函数，在主线程中调用（可选）
 * @param user_data 传递给回调函数的用户数据
 */
void check_offline_data_update(GSourceFunc callback, gpointer user_data);

/**
 * 从离线数据中搜索卡片
 * @param query 搜索关键词（在卡名和效果描述中搜索）
 * @return JSON数组，包含匹配的卡片数据，需要调用者使用json_array_unref释放；如果失败返回NULL
 */
JsonArray* search_offline_cards(const char *query);

/**
 * 获取所有离线卡片数据
 * @return JSON数组，包含所有卡片数据，需要调用者使用json_array_unref释放；如果失败返回NULL
 */
JsonArray* get_all_offline_cards(void);

// 以流式方式遍历离线卡片：避免一次性构建 1w+ 结果数组导致首搜卡顿。
// match_cb 返回 TRUE 表示“接受并计数”；当接受数量达到 max_results 时停止遍历。
typedef gboolean (*OfflineCardMatchFunc)(JsonObject *card, gpointer user_data);

guint offline_foreach_card(const char *query,
						   gboolean search_all,
						   OfflineCardMatchFunc match_cb,
						   gpointer user_data,
						   guint max_results);

// 预热离线 JSON 解析缓存（后台线程）：减少第一次搜索的卡顿。
void offline_data_warm_cache_async(void);

// 清空离线 JSON 解析缓存（例如清理/更新离线数据后）。
void offline_data_clear_cache(void);

/**
 * 从离线数据中根据卡片ID获取单张卡片信息
 * @param card_id 卡片ID
 * @return JSON对象，包含卡片数据，需要调用者使用json_object_unref释放；如果失败返回NULL
 */
JsonObject* get_card_by_id_offline(int card_id);

#endif // OFFLINE_DATA_H

#ifndef PRERELEASE_H
#define PRERELEASE_H

#include <glib.h>
#include <json-glib/json-glib.h>

/**
 * 下载并处理先行卡数据
 * 此函数会在后台线程中执行，不会阻塞主线程
 * @param callback 完成后的回调函数，在主线程中调用
 * @param user_data 传递给回调函数的用户数据
 */
void download_prerelease_cards(GSourceFunc callback, gpointer user_data);

/**
 * 从先行卡JSON文件中搜索卡片
 * @param search_query 搜索关键词
 * @return JSON数组，包含匹配的卡片数据，需要调用者使用json_array_unref释放
 */
JsonArray* search_prerelease_cards(const char *search_query);

/**
 * 获取所有先行卡
 * @return JSON数组，包含所有先行卡数据，需要调用者使用json_array_unref释放；如果失败返回NULL
 */
JsonArray* get_all_prerelease_cards(void);

/**
 * 根据ID查找先行卡
 * @param card_id 卡片ID
 * @return JSON对象，包含卡片数据，需要调用者使用json_object_unref释放；如果未找到返回NULL
 */
JsonObject* find_prerelease_card_by_id(int card_id);

/**
 * 获取先行卡图片路径
 * @param card_id 卡片ID
 * @return 图片文件路径，需要调用者使用g_free释放；如果文件不存在返回NULL
 */
gchar* get_prerelease_card_image_path(int card_id);

/**
 * 检查先行卡数据是否存在
 * @return TRUE如果数据已下载，否则FALSE
 */
gboolean prerelease_data_exists(void);

#endif // PRERELEASE_H

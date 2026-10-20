#ifndef PRERELEASE_H
#define PRERELEASE_H

#include <glib.h>
#include <json-glib/json-glib.h>

/**
 * 先行卡更新结果
 */
typedef enum {
    PRERELEASE_UPDATE_UPDATED,    // 已下载并替换为新数据
    PRERELEASE_UPDATE_UP_TO_DATE, // 上游 ETag 未变化，保留本地数据
    PRERELEASE_UPDATE_FAILED      // 网络或处理失败，本地数据保持不变
} PrereleaseUpdateResult;

/**
 * 先行卡更新完成回调（在主线程中调用）
 * @param result 更新结果
 * @param user_data 传递给回调函数的用户数据
 */
typedef void (*PrereleaseUpdateCallback)(PrereleaseUpdateResult result, gpointer user_data);

/**
 * 检查并更新先行卡数据
 * 先比较本地保存的 ETag 与上游；未变化时不做任何下载，失败时保留旧数据。
 * 此函数会在后台线程中执行，不会阻塞主线程。
 * @param callback 完成后的回调函数，在主线程中调用（可选）
 * @param user_data 传递给回调函数的用户数据
 */
void update_prerelease_cards(PrereleaseUpdateCallback callback, gpointer user_data);

/**
 * 清空本地先行卡数据（图片、JSON、ETag）。
 * 取消“启用先行卡”后调用。
 * @return TRUE 如果删除成功或目录本就不存在，FALSE 删除失败
 */
gboolean clear_prerelease_data(void);

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

/**
 * 通过ID判断是否是先行卡（先行卡ID为9位数）
 * @param card_id 卡片ID
 * @return TRUE如果是先行卡ID，否则FALSE
 */
gboolean is_prerelease_id(int card_id);

#endif // PRERELEASE_H

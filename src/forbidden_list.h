#ifndef FORBIDDEN_LIST_H
#define FORBIDDEN_LIST_H

#include <glib.h>
#include <gtk/gtk.h>

/**
 * 加载禁限卡表JSON文件
 * @param filename 文件路径
 * @return 卡片ID到限制状态的哈希表，需要调用者使用g_hash_table_unref释放
 */
GHashTable* load_forbidden_list(const char *filename);

/**
 * 加载禁限卡表变更信息JSON文件
 * 格式：{卡片cid字符串: {old: "旧状态", new: "新状态"}}
 * @param filename 文件路径
 * @return 卡片cid字符串到变更信息哈希表，需要调用者使用g_hash_table_unref释放
 */
GHashTable* load_forbidden_changes(const char *filename);

/**
 * 获取指定键（通常为卡片cid字符串）的禁限变更信息
 * @param changes_table 变更信息表（可以为NULL）
 * @param card_key 卡片键
 * @param out_old 指针，用于返回旧状态
 * @param out_new 指针，用于返回新状态
 * @return TRUE如果找到变更信息，FALSE否则
 */
gboolean get_forbidden_change(GHashTable *changes_table, const char *card_key, 
                               const char **out_old, const char **out_new);

#endif // FORBIDDEN_LIST_H

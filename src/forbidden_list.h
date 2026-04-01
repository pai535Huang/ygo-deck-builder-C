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
 * 获取卡片在指定禁限卡表中的最大数量限制
 * @param forbidden_table 禁限卡表（可以为NULL）
 * @param card_id 卡片ID
 * @return 0=禁止, 1=限制1, 2=限制2, 3=无限制
 */
int get_card_limit_from_table(GHashTable *forbidden_table, int card_id);

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

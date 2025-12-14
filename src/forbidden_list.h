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

#endif // FORBIDDEN_LIST_H

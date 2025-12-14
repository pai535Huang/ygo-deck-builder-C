#ifndef DECK_CLEAR_H
#define DECK_CLEAR_H

#include <gtk/gtk.h>

/**
 * 清空指定区域的所有卡片
 * @param pics 槽位数组
 * @param count 当前数量指针
 * @param count_label 显示计数的标签
 */
void clear_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label);

/**
 * 清空所有卡组区域（main、extra、side）
 * @param main_pics 主卡组槽位数组
 * @param main_count 主卡组数量指针
 * @param main_label 主卡组计数标签
 * @param extra_pics 额外卡组槽位数组
 * @param extra_count 额外卡组数量指针
 * @param extra_label 额外卡组计数标签
 * @param side_pics 副卡组槽位数组
 * @param side_count 副卡组数量指针
 * @param side_label 副卡组计数标签
 */
void clear_all_deck_regions(
    GPtrArray *main_pics, int *main_count, GtkLabel *main_label,
    GPtrArray *extra_pics, int *extra_count, GtkLabel *extra_label,
    GPtrArray *side_pics, int *side_count, GtkLabel *side_label
);

#endif // DECK_CLEAR_H

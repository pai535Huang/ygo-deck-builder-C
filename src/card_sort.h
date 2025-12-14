#ifndef CARD_SORT_H
#define CARD_SORT_H

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/**
 * 对指定区域的卡片按类型和等级排序
 * 排序规则：怪兽-魔法-陷阱，怪兽按等级降序，魔法陷阱按子类型排序
 * 
 * @param pics 槽位数组
 * @param count 当前卡片数量指针
 * @param count_label 显示计数的标签
 */
void sort_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label);

/**
 * 对Extra区域的卡片排序
 * 排序规则：融合-同调-超量-连接，同类按等级降序
 * 
 * @param pics 槽位数组
 * @param count 当前卡片数量指针
 * @param count_label 显示计数的标签
 */
void sort_extra_region(GPtrArray *pics, int *count, GtkLabel *count_label);

#endif // CARD_SORT_H

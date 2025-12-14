#ifndef CARD_SHUFFLE_H
#define CARD_SHUFFLE_H

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/**
 * 使用Fisher-Yates洗牌算法打乱指定区域的卡片
 * 
 * @param pics 槽位数组
 * @param count 当前卡片数量指针
 * @param count_label 显示计数的标签
 */
void shuffle_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label);

#endif // CARD_SHUFFLE_H

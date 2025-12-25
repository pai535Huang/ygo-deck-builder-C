#ifndef DECK_SLOT_H
#define DECK_SLOT_H

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

// 槽位缩略图的逻辑尺寸（与 UI 中 thumb-fixed / image_loader 缩略图保持一致）
#define SLOT_THUMB_W 68
#define SLOT_THUMB_H 99

/**
 * 从槽位获取图片
 * @param pic 槽位widget
 * @return 槽位中的GdkPixbuf，如果为空返回NULL
 */
GdkPixbuf* slot_get_pixbuf(GtkWidget *pic);

/**
 * 设置槽位图片
 * @param pic 槽位widget
 * @param pb 要设置的GdkPixbuf，NULL表示清空
 */
void slot_set_pixbuf(GtkWidget *pic, GdkPixbuf *pb);

/**
 * 获取槽位是否为额外卡类型
 * @param pic 槽位widget
 * @return TRUE表示额外卡（融合/同调/超量/连接），FALSE表示主卡组卡
 */
gboolean slot_get_is_extra(GtkWidget *pic);

/**
 * 设置槽位的额外卡标记
 * @param pic 槽位widget
 * @param is_extra TRUE表示额外卡，FALSE表示主卡组卡
 */
void slot_set_is_extra(GtkWidget *pic, gboolean is_extra);

/**
 * 查找第一个空槽位
 * @param pics 槽位数组
 * @return 第一个空槽位的索引，如果没有空槽位返回-1
 */
int array_find_first_empty(GPtrArray *pics);

/**
 * 更新计数标签
 * @param label 要更新的标签
 * @param count 当前数量
 */
void update_count_label(GtkLabel *label, int count);

/**
 * 删除指定槽位并将后续槽位前移
 * @param pics 槽位数组
 * @param count_ptr 当前数量指针
 * @param del_index 要删除的槽位索引
 */
void shift_delete_slots(GPtrArray *pics, int *count_ptr, int del_index);

/**
 * 将槽位数组中的卡片向右移动
 * @param pics 槽位数组
 * @param start 起始索引
 * @param end 结束索引
 */
void array_shift_right(GPtrArray *pics, int start, int end);

/**
 * 将槽位数组中的卡片向左移动
 * @param pics 槽位数组
 * @param start 起始索引
 * @param end 结束索引
 */
void array_shift_left(GPtrArray *pics, int start, int end);

#endif // DECK_SLOT_H

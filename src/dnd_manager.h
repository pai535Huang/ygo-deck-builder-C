#ifndef DND_MANAGER_H
#define DND_MANAGER_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include "app_types.h"

// DnD 回调函数
GdkContentProvider* on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data);
void on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data);
gboolean on_drop_accept(GtkDropTarget *target, GdkDrop *drop, gpointer user_data);
void on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data);

// 辅助函数：获取卡片在禁限卡表中的限制数量
int get_card_limit(SearchUI *ui, int card_id);

// 辅助函数：统计卡组中某张卡的数量
int count_card_in_deck(SearchUI *ui, int card_id, gboolean is_extra);

#endif // DND_MANAGER_H

#include "deck_slot.h"

// 从槽位获取图片
GdkPixbuf* slot_get_pixbuf(GtkWidget *pic) {
    return (GdkPixbuf*)g_object_get_data(G_OBJECT(pic), "pixbuf");
}

// 设置槽位图片
void slot_set_pixbuf(GtkWidget *pic, GdkPixbuf *pb) {
    // 清除缓存的surface：由 destroy notify 统一回收
    g_object_set_data_full(G_OBJECT(pic), "cached_surface", NULL, NULL);
    // 清除渲染缓存（缩放后的 pixbuf）
    g_object_set_data_full(G_OBJECT(pic), "cached_render", NULL, NULL);
    
    if (pb) {
        // 槽位展示为固定缩略图：按 HiDPI scale_factor 生成 device-pixel 尺寸，避免模糊
        int sf = gtk_widget_get_scale_factor(pic);
        if (sf < 1) sf = 1;
        int tw = SLOT_THUMB_W * sf;
        int th = SLOT_THUMB_H * sf;
        GdkPixbuf *thumb = NULL;
        if (gdk_pixbuf_get_width(pb) != tw || gdk_pixbuf_get_height(pb) != th) {
            thumb = gdk_pixbuf_scale_simple(pb, tw, th, GDK_INTERP_HYPER);
        }
        if (!thumb) thumb = g_object_ref(pb);
        g_object_set_data_full(G_OBJECT(pic), "pixbuf", thumb, (GDestroyNotify)g_object_unref);
    } else {
        g_object_set_data_full(G_OBJECT(pic), "pixbuf", NULL, NULL);
    }
    gtk_widget_queue_draw(pic);
}

// 获取槽位是否为额外卡类型
gboolean slot_get_is_extra(GtkWidget *pic) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "slot_is_extra_type")) ? TRUE : FALSE;
}

// 设置槽位的额外卡标记
void slot_set_is_extra(GtkWidget *pic, gboolean is_extra) {
    g_object_set_data(G_OBJECT(pic), "slot_is_extra_type", GINT_TO_POINTER(is_extra ? 1 : 0));
}

// 查找第一个空槽位
int array_find_first_empty(GPtrArray *pics) {
    if (!pics) return -1;
    for (guint i = 0; i < pics->len; ++i) {
        GtkWidget *w = GTK_WIDGET(g_ptr_array_index(pics, i));
        if (w && slot_get_pixbuf(w) == NULL) return (int)i;
    }
    return -1;
}

// 更新计数标签
void update_count_label(GtkLabel *label, int count) {
    if (!label) return;
    char buf[16];
    g_snprintf(buf, sizeof buf, "(%d)", count);
    gtk_label_set_text(label, buf);
}

// 删除指定槽位并将后续槽位前移
void shift_delete_slots(GPtrArray *pics, int *count_ptr, int del_index) {
    if (!pics || !count_ptr) return;
    int count = *count_ptr;
    if (del_index < 0 || del_index >= count) return;
    for (int i = del_index; i < count - 1; ++i) {
        GtkWidget *dst = GTK_WIDGET(g_ptr_array_index(pics, i));
        GtkWidget *src = GTK_WIDGET(g_ptr_array_index(pics, i + 1));
        GdkPixbuf *pb = slot_get_pixbuf(src);
        slot_set_pixbuf(dst, pb);
        slot_set_is_extra(dst, slot_get_is_extra(src));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "card_id"));
        g_object_set_data(G_OBJECT(dst), "card_id", GINT_TO_POINTER(card_id));
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "img_id"));
        g_object_set_data(G_OBJECT(dst), "img_id", GINT_TO_POINTER(img_id));
    }
    // 清空最后一个原已填槽
    if (count > 0) {
        GtkWidget *last = GTK_WIDGET(g_ptr_array_index(pics, count - 1));
        slot_set_pixbuf(last, NULL);
        slot_set_is_extra(last, FALSE);
        g_object_set_data(G_OBJECT(last), "card_id", GINT_TO_POINTER(0));
        g_object_set_data(G_OBJECT(last), "img_id", GINT_TO_POINTER(0));
    }
    *count_ptr = count - 1;
}

// 将槽位数组中的卡片向右移动
void array_shift_right(GPtrArray *pics, int start, int end) {
    if (!pics) return;
    if (start < 0) start = 0;
    if (end < 0) return;
    if (start >= (int)pics->len) return;
    if (end >= (int)pics->len) end = (int)pics->len - 1;
    for (int i = end; i > start; --i) {
        GtkWidget *dst = GTK_WIDGET(g_ptr_array_index(pics, i));
        GtkWidget *src = GTK_WIDGET(g_ptr_array_index(pics, i - 1));
        GdkPixbuf *pb = slot_get_pixbuf(src);
        slot_set_pixbuf(dst, pb);
        slot_set_is_extra(dst, slot_get_is_extra(src));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "card_id"));
        g_object_set_data(G_OBJECT(dst), "card_id", GINT_TO_POINTER(card_id));
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "img_id"));
        g_object_set_data(G_OBJECT(dst), "img_id", GINT_TO_POINTER(img_id));
    }
}

// 将槽位数组中的卡片向左移动
void array_shift_left(GPtrArray *pics, int start, int end) {
    if (!pics) return;
    if (start < 0) start = 0;
    if (end < 0) return;
    if (start >= (int)pics->len) return;
    if (end >= (int)pics->len) end = (int)pics->len - 1;
    for (int i = start; i < end; ++i) {
        GtkWidget *dst = GTK_WIDGET(g_ptr_array_index(pics, i));
        GtkWidget *src = GTK_WIDGET(g_ptr_array_index(pics, i + 1));
        GdkPixbuf *pb = slot_get_pixbuf(src);
        slot_set_pixbuf(dst, pb);
        slot_set_is_extra(dst, slot_get_is_extra(src));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "card_id"));
        g_object_set_data(G_OBJECT(dst), "card_id", GINT_TO_POINTER(card_id));
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "img_id"));
        g_object_set_data(G_OBJECT(dst), "img_id", GINT_TO_POINTER(img_id));
    }
}

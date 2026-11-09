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
    // 图片变更后拖拽图标也需要重新生成
    g_object_set_data_full(G_OBJECT(pic), "drag_icon_texture", NULL, NULL);
    
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

// 获取槽位存储的英文卡名
const char* slot_get_en_name(GtkWidget *pic) {
    return (const char*)g_object_get_data(G_OBJECT(pic), "en_name");
}

// 设置槽位的英文卡名
void slot_set_en_name(GtkWidget *pic, const char *en_name) {
    g_object_set_data_full(G_OBJECT(pic), "en_name",
                           en_name ? g_strdup(en_name) : NULL,
                           en_name ? (GDestroyNotify)g_free : NULL);
}

int slot_get_img_id(GtkWidget *slot) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "img_id"));
}

uint32_t slot_get_card_type(GtkWidget *slot) {
    return GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(slot), "card-type"));
}

const char *slot_get_sheet_name(GtkWidget *slot) {
    return g_object_get_data(G_OBJECT(slot), "sheet-name");
}

void slot_set_card_details(GtkWidget *slot, uint32_t type, const char *sheet_name) {
    g_object_set_data(G_OBJECT(slot), "card-type", GUINT_TO_POINTER(type));
    g_object_set_data_full(G_OBJECT(slot), "sheet-name",
                           sheet_name && *sheet_name ? g_strdup(sheet_name) : NULL, g_free);
}

static void slot_set_card(GtkWidget *slot, int img_id, gpointer card_id,
                          gboolean is_extra, const char *name, GdkPixbuf *pixbuf,
                          gpointer needs_metadata) {
    g_object_set_data(G_OBJECT(slot), "img_id", GINT_TO_POINTER(img_id));
    g_object_set_data(G_OBJECT(slot), "card_id", card_id);
    slot_set_is_extra(slot, is_extra);
    slot_set_en_name(slot, name);
    slot_set_pixbuf(slot, pixbuf);
    g_object_set_data(G_OBJECT(slot), "needs-card-metadata", needs_metadata);
}

void slot_clear_card(GtkWidget *slot) {
    slot_set_card(slot, 0, NULL, FALSE, NULL, NULL, NULL);
    slot_set_card_details(slot, 0, NULL);
}

static void slot_copy_card(GtkWidget *target, GtkWidget *source) {
    slot_set_card(target, slot_get_img_id(source),
                  g_object_get_data(G_OBJECT(source), "card_id"),
                  slot_get_is_extra(source), slot_get_en_name(source),
                  slot_get_pixbuf(source),
                  g_object_get_data(G_OBJECT(source), "needs-card-metadata"));
    slot_set_card_details(target, slot_get_card_type(source), slot_get_sheet_name(source));
}

void slot_swap_cards(GtkWidget *first, GtkWidget *second) {
    if (first == second) return;
    int img_id = slot_get_img_id(first);
    gpointer card_id = g_object_get_data(G_OBJECT(first), "card_id");
    gboolean is_extra = slot_get_is_extra(first);
    g_autofree char *name = g_strdup(slot_get_en_name(first));
    g_autoptr(GdkPixbuf) pixbuf = slot_get_pixbuf(first);
    if (pixbuf) g_object_ref(pixbuf);

    gpointer needs_metadata = g_object_get_data(G_OBJECT(first), "needs-card-metadata");
    uint32_t type = slot_get_card_type(first);
    g_autofree char *sheet_name = g_strdup(slot_get_sheet_name(first));
    slot_copy_card(first, second);
    slot_set_card(second, img_id, card_id, is_extra, name, pixbuf, needs_metadata);
    slot_set_card_details(second, type, sheet_name);
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
    array_shift_left(pics, del_index, count - 1);
    slot_clear_card(g_ptr_array_index(pics, count - 1));
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
        slot_copy_card(dst, src);
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
        slot_copy_card(dst, src);
    }
}
